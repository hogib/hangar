// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar-mavlink's entry point and the UDP link the three modes share. One
 * program rather than two: the vehicle's and the topside's module share the
 * framing, the signing, the configuration grammar and this link; which
 * module a process is comes from its wiring (the manifest names the role on
 * the Exec line), as every module's identity does. The vehicle's module may
 * also reach the autopilot over a serial line (Serial=, a Pixhawk on USB):
 * then the link is that line, parsed as a byte stream, written through a
 * small queue that never makes the control path wait. */
#define _GNU_SOURCE
#include "mavlink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "mavlink_gen.h"

/* ---- the serial line -------------------------------------------------------- */

#define BACKOFF_MIN_NS 500000000ull
#define BACKOFF_MAX_NS 5000000000ull

static uint64_t now_mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static speed_t speed_of(int baud)
{
	switch (baud) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 500000: return B500000;
	case 921600: return B921600;
	case 1000000: return B1000000;
	case 1500000: return B1500000;
	case 2000000: return B2000000;
	default: return B115200;
	}
}

/* Opens the line raw: no echo, no line discipline, 8N1, receiver on, modem
 * control lines ignored (CLOCAL), RTS/CTS only when configured. Non-blocking:
 * the module reads it from its epoll and never waits on a write. TIOCEXCL
 * keeps a second program (a MAVROS left running, a stray screen) from opening
 * it while the module has it; root still can. */
static int serial_open(struct mav_link *l)
{
	int fd = open(l->dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		snprintf(l->line_state, sizeof l->line_state, "down (%s)", strerror(errno));
		return -1;
	}
	if (l->exclusive && ioctl(fd, TIOCEXCL) < 0) {
		snprintf(l->line_state, sizeof l->line_state, "down (TIOCEXCL: %s)", strerror(errno));
		close(fd);
		return -1;
	}
	struct termios t;
	if (tcgetattr(fd, &t) < 0) {
		snprintf(l->line_state, sizeof l->line_state, "down (not a terminal: %s)", strerror(errno));
		close(fd);
		return -1;
	}
	cfmakeraw(&t);
	t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB | CRTSCTS);
	t.c_cflag |= CS8 | CREAD | CLOCAL;
	if (l->rtscts)
		t.c_cflag |= CRTSCTS;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, speed_of(l->baud));
	cfsetospeed(&t, speed_of(l->baud));
	if (tcsetattr(fd, TCSANOW, &t) < 0) {
		snprintf(l->line_state, sizeof l->line_state, "down (tcsetattr: %s)", strerror(errno));
		close(fd);
		return -1;
	}
	tcflush(fd, TCIOFLUSH); /* nothing from before the open: half a frame would only be garbage */
	l->fd = fd;
	l->have_peer = 1;
	l->rx_len = 0;
	l->opens++;
	l->backoff_ns = 0;
	snprintf(l->line_state, sizeof l->line_state, "open");
	return 0;
}

/* The line went away (a USB unplug, the autopilot rebooting, the far end of a
 * pseudo-terminal closed): closed at once, so that a hung-up descriptor never
 * wakes the epoll in a loop; reopened after a back-off that doubles from 0.5 s
 * to 5 s. Frames queued for the old line are dropped: they were for a moment
 * that has passed. */
/* The exclusive flag is the terminal's, not the descriptor's: cleared before
 * the close, or the Neutral command (and the next instance) would find the
 * line busy where the terminal outlives the close (a pseudo-terminal whose
 * master stays open). */
static void serial_close_fd(struct mav_link *l)
{
	if (l->fd < 0)
		return;
	if (l->exclusive)
		(void)ioctl(l->fd, TIOCNXCL);
	tcflush(l->fd, TCIOFLUSH);
	close(l->fd);
	l->fd = -1;
}

static void serial_down(struct mav_link *l, const char *why)
{
	serial_close_fd(l);
	l->fd = -1;
	l->have_peer = 0;
	l->rx_len = 0;
	l->tx_dropped += (uint64_t)l->tx_count;
	l->tx_count = 0;
	l->tx_head = 0;
	l->drops++;
	l->backoff_ns = l->backoff_ns ? l->backoff_ns * 2 : BACKOFF_MIN_NS;
	if (l->backoff_ns > BACKOFF_MAX_NS)
		l->backoff_ns = BACKOFF_MAX_NS;
	l->next_open_ns = now_mono_ns() + l->backoff_ns;
	snprintf(l->line_state, sizeof l->line_state, "down (%s)", why);
}

static int line_gone(int err)
{
	return err == EIO || err == ENXIO || err == ENODEV || err == EBADF || err == EPIPE || err == ENOTTY;
}

static void tlog_write(struct mav_link *l, const uint8_t *frame, size_t n);

/* Writes the queue's head frames as far as the line takes them. A message
 * is framed here, when it is first at the head and nothing of it has gone:
 * a MANUAL_CONTROL that replaced a queued one used to be signed when it was
 * queued, ahead of frames signed earlier, and a receiver that wants rising
 * timestamps (ArduSub's does) then refused everything queued behind it, a
 * disarm included. */
static void serial_flush(struct mav_link *l)
{
	while (l->fd >= 0 && l->tx_count > 0) {
		struct mav_txframe *f = &l->txq[l->tx_head];
		if (!f->framed) {
			uint8_t payload[MAV_MAX_PAYLOAD];
			size_t full_len = f->len;
			memcpy(payload, f->data, full_len);
			size_t n = mav_encode(f->data, sizeof f->data, l->seq++, l->sysid, l->compid, f->msgid, payload,
					      full_len, f->crc_extra, &l->signing);
			if (!n) { /* refused at mav_link_send already: not reached */
				l->tx_head = (l->tx_head + 1) % MAV_TXQ;
				l->tx_count--;
				l->tx_dropped++;
				continue;
			}
			f->len = (uint16_t)n;
			f->off = 0;
			f->framed = 1;
		}
		ssize_t w = write(l->fd, f->data + f->off, (size_t)(f->len - f->off));
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			if (line_gone(errno)) {
				char why[96];
				snprintf(why, sizeof why, "write: %s", strerror(errno));
				serial_down(l, why);
			}
			return;
		}
		f->off = (uint16_t)(f->off + (uint16_t)w);
		if (f->off < f->len)
			return;
		tlog_write(l, f->data, f->len); /* what went on the line, when it had gone */
		l->tx_head = (l->tx_head + 1) % MAV_TXQ;
		l->tx_count--;
	}
}

/* What a message is to the queue. A COMMAND_LONG is a control frame only
 * when it arms or disarms: the rate requests are COMMAND_LONGs too
 * (MAV_CMD_SET_MESSAGE_INTERVAL, six of them every 5 s), and while they
 * counted as control nothing could evict them: a line blocked for half a
 * minute filled the queue with them and the next disarm found no room. */
static int tx_kind(uint32_t msgid, const uint8_t *payload, size_t full_len)
{
	if (msgid == MAV_MSG_ID_MANUAL_CONTROL)
		return MAV_TX_MANUAL_CONTROL;
	if (msgid == MAV_MSG_ID_COMMAND_LONG && full_len <= MAV_MAX_PAYLOAD) {
		struct mav_command_long c;
		mav_unpack_command_long(payload, (uint8_t)full_len, &c);
		if (c.command == MAV_CMD_COMPONENT_ARM_DISARM)
			return c.param1 >= 0.5f ? MAV_TX_ARM : MAV_TX_DISARM;
	}
	return MAV_TX_OTHER;
}

/* The message into one slot of the queue, to be framed when its turn comes. */
static void txframe_set(struct mav_txframe *q, uint32_t msgid, const uint8_t *payload, size_t full_len,
			uint8_t crc_extra)
{
	memcpy(q->data, payload, full_len);
	q->len = (uint16_t)full_len;
	q->off = 0;
	q->msgid = msgid;
	q->crc_extra = crc_extra;
	q->framed = 0;
	q->kind = (uint8_t)tx_kind(msgid, payload, full_len);
}

/* Takes the i-th message (from the head) out of the queue. */
static void tx_remove(struct mav_link *l, int i)
{
	for (; i < l->tx_count - 1; i++)
		l->txq[(l->tx_head + i) % MAV_TXQ] = l->txq[(l->tx_head + i + 1) % MAV_TXQ];
	l->tx_count--;
}

int mav_link_cancel_arm(struct mav_link *l)
{
	int n = 0;
	if (!l->serial)
		return 0;
	for (int i = l->tx_count - 1; i >= 0; i--) {
		struct mav_txframe *q = &l->txq[(l->tx_head + i) % MAV_TXQ];
		if (q->kind == MAV_TX_ARM && q->off == 0) {
			tx_remove(l, i);
			n++;
		}
	}
	l->tx_dropped += (uint64_t)n;
	return n;
}

/* The queue's policy (mavlink.h, mav_link_send). Index i counts from the head;
 * a frame of which a byte has gone is never touched. */
static int serial_enqueue(struct mav_link *l, uint32_t msgid, const uint8_t *payload, size_t full_len,
			  uint8_t crc_extra)
{
	int kind = tx_kind(msgid, payload, full_len);
	/* A disarm overtakes nothing, but an arm still waiting ahead of it would
	 * go out first and be undone a frame later: it does not go out at all. */
	if (kind == MAV_TX_DISARM)
		mav_link_cancel_arm(l);
	if (kind != MAV_TX_OTHER) {
		for (int i = l->tx_count - 1; i >= 0; i--) {
			struct mav_txframe *q = &l->txq[(l->tx_head + i) % MAV_TXQ];
			if (q->kind == kind && q->off == 0) {
				/* In its place: it keeps its turn, and takes the sequence
				 * number and the timestamp of that turn. A disarm sent
				 * again while the first still waits is that disarm: the
				 * queue holds one, however long the line stays blocked. */
				txframe_set(q, msgid, payload, full_len, crc_extra);
				if (kind == MAV_TX_MANUAL_CONTROL)
					l->tx_superseded++;
				return 0;
			}
		}
	}
	if (l->tx_count == MAV_TXQ) {
		int victim = -1;
		if (kind != MAV_TX_OTHER)
			for (int i = l->tx_count - 1; i >= 1; i--) {
				struct mav_txframe *q = &l->txq[(l->tx_head + i) % MAV_TXQ];
				if (q->kind == MAV_TX_OTHER && q->off == 0) {
					victim = i;
					break;
				}
			}
		if (victim < 0) {
			l->tx_dropped++;
			return -1;
		}
		tx_remove(l, victim);
		l->tx_dropped++;
	}
	txframe_set(&l->txq[(l->tx_head + l->tx_count) % MAV_TXQ], msgid, payload, full_len, crc_extra);
	l->tx_count++;
	return 0;
}

/* ---- the link ----------------------------------------------------------------- */

int mav_link_open(struct mav_link *l, const struct mavlink_conf *c, char *err, size_t errlen)
{
	return mav_link_open_ex(l, c, 1, err, errlen);
}

int mav_link_open_ex(struct mav_link *l, const struct mavlink_conf *c, int exclusive, char *err, size_t errlen)
{
	memset(l, 0, sizeof l[0]);
	l->fd = -1;
	l->tlog_fd = -1;
	l->sysid = c->sysid;
	l->compid = c->compid;
	if (c->signing) {
		l->signing.enabled = 1;
		l->signing.link_id = 1;
		if (mav_signing_load_key(&l->signing, c->key_file, err, errlen) < 0)
			return -1;
	}
	if (c->tlog[0]) {
		l->tlog_fd = open(c->tlog, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
		if (l->tlog_fd < 0) {
			snprintf(err, errlen, "tlog %s: %s", c->tlog, strerror(errno));
			return -1;
		}
	}
	if (c->serial[0]) {
		l->serial = 1;
		snprintf(l->dev, sizeof l->dev, "%s", c->serial);
		l->baud = c->baud ? c->baud : 115200;
		l->rtscts = c->rtscts;
		l->exclusive = exclusive;
		if (serial_open(l) < 0) {
			/* Not there yet (the autopilot booting, a cable): retried. */
			l->backoff_ns = BACKOFF_MIN_NS;
			l->next_open_ns = now_mono_ns() + l->backoff_ns;
		}
		return 0;
	}
	l->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (l->fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		mav_link_close(l);
		return -1;
	}
	int one = 1;
	setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(l->fd, (const struct sockaddr *)&c->bind, sizeof c->bind) < 0) {
		snprintf(err, errlen, "bind %s: %s", "the configured address", strerror(errno));
		mav_link_close(l);
		return -1;
	}
	if (c->have_peer) {
		l->peer = c->peer;
		l->have_peer = 1;
		l->peer_fixed = 1;
	}
	l->peer_quiet_ns = MAV_PEER_QUIET_NS;
	return 0;
}

int mav_link_service(struct mav_link *l, uint64_t now_ns)
{
	if (!l->serial)
		return 0;
	int before = l->fd;
	if (l->fd < 0 && now_ns >= l->next_open_ns) {
		if (serial_open(l) < 0) {
			l->backoff_ns = l->backoff_ns ? l->backoff_ns * 2 : BACKOFF_MIN_NS;
			if (l->backoff_ns > BACKOFF_MAX_NS)
				l->backoff_ns = BACKOFF_MAX_NS;
			l->next_open_ns = now_ns + l->backoff_ns;
		}
	}
	serial_flush(l);
	return l->fd != before;
}

void mav_link_drain(struct mav_link *l, int timeout_ms)
{
	if (!l->serial || l->fd < 0)
		return;
	uint64_t deadline = now_mono_ns() + (uint64_t)timeout_ms * 1000000ull;
	for (;;) {
		serial_flush(l);
		if (l->fd < 0)
			return;
		int queued = 0;
		if (ioctl(l->fd, TIOCOUTQ, &queued) < 0)
			queued = 0;
		if (l->tx_count == 0 && queued == 0)
			return;
		if (now_mono_ns() >= deadline)
			break;
		struct pollfd p = { .fd = l->fd, .events = POLLOUT };
		poll(&p, 1, 10);
	}
	l->tx_dropped += (uint64_t)l->tx_count;
	l->tx_count = 0;
	tcflush(l->fd, TCOFLUSH);
}

void mav_link_describe(const struct mav_link *l, char *out, size_t len)
{
	if (!l->serial) {
		snprintf(out, len, "udp");
		return;
	}
	size_t u = (size_t)snprintf(out, len, "serial %s %d %s", l->dev, l->baud, l->line_state);
	if (u < len && l->drops)
		u += (size_t)snprintf(out + u, len - u, " drops %llu", (unsigned long long)l->drops);
	if (u < len && l->tx_dropped)
		u += (size_t)snprintf(out + u, len - u, " tx-dropped %llu", (unsigned long long)l->tx_dropped);
	if (u < len && l->garbage)
		(void)snprintf(out + u, len - u, " resync %llu B", (unsigned long long)l->garbage);
}

void mav_link_close(struct mav_link *l)
{
	if (l->serial)
		serial_close_fd(l);
	else if (l->fd >= 0)
		close(l->fd);
	if (l->tlog_fd >= 0)
		close(l->tlog_fd);
	l->fd = l->tlog_fd = -1;
	l->have_peer = 0;
}

/* pymavlink's .tlog: each frame preceded by its time in microseconds since
 * the epoch, 8 bytes big-endian. */
static void tlog_write(struct mav_link *l, const uint8_t *frame, size_t n)
{
	if (l->tlog_fd < 0)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
	uint8_t buf[8 + MAV_MAX_FRAME];
	for (int i = 0; i < 8; i++)
		buf[i] = (uint8_t)(us >> (56 - 8 * i));
	memcpy(buf + 8, frame, n);
	(void)!write(l->tlog_fd, buf, 8 + n);
}

int mav_link_send(struct mav_link *l, uint32_t msgid, const uint8_t *payload, size_t full_len, uint8_t crc_extra)
{
	if (!l->have_peer || l->fd < 0)
		return -1;
	if (l->serial) {
		/* Framed in serial_flush, when its turn on the line comes. */
		if (full_len == 0 || full_len > MAV_MAX_PAYLOAD || msgid > 0xffffffu ||
		    serial_enqueue(l, msgid, payload, full_len, crc_extra) < 0)
			return -1;
		l->sent++;
		serial_flush(l);
		return 0;
	}
	uint8_t frame[MAV_MAX_FRAME];
	size_t n = mav_encode(frame, sizeof frame, l->seq++, l->sysid, l->compid, msgid, payload, full_len, crc_extra,
			      &l->signing);
	if (!n)
		return -1;
	ssize_t w = sendto(l->fd, frame, n, MSG_DONTWAIT, (const struct sockaddr *)&l->peer, sizeof l->peer);
	if (w < 0)
		return -1;
	l->sent++;
	tlog_write(l, frame, n);
	return 0;
}

/* One decoded frame from the byte stream, counted as the UDP path counts it.
 * Returns the bytes to step over: the whole frame when it decoded or is of an
 * unknown id (its CRC cannot be checked, but its length is the frame's); one
 * byte when it failed its checks, so that a 0xFD inside line noise or a cut
 * frame does not swallow the real frame behind it. */
static size_t take_frame(struct mav_link *l, const uint8_t *p, size_t used, const struct mav_frame *f,
			 const char *why, void (*fn)(void *, const struct mav_frame *), void *ctx, int *valid)
{
	if (!f->valid) {
		if (used > 1 && strncmp(why, "unknown", 7) == 0) {
			l->unknown++; /* a message outside tools/mavgen/messages.txt: not an error */
			return used;
		}
		if (used > 1) {
			l->rejected++;
			snprintf(l->last_reject, sizeof l->last_reject, "%s", why);
		}
		return 1;
	}
	l->received++;
	(*valid)++;
	tlog_write(l, p, used);
	fn(ctx, f);
	return used;
}

/* A byte stream: what the line gave is appended to the receive buffer and
 * parsed from there; a frame cut between two reads waits for its rest. */
static int serial_receive(struct mav_link *l, void (*fn)(void *ctx, const struct mav_frame *f), void *ctx)
{
	int valid = 0;
	while (l->fd >= 0) {
		ssize_t r = read(l->fd, l->rx + l->rx_len, sizeof l->rx - l->rx_len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				char why[96];
				snprintf(why, sizeof why, "read: %s", strerror(errno));
				serial_down(l, why);
			}
			break;
		}
		if (r == 0) {
			/* A terminal says nothing with 0 unless it was hung up. */
			struct pollfd p = { .fd = l->fd, .events = POLLIN };
			if (poll(&p, 1, 0) > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)))
				serial_down(l, "hung up");
			break;
		}
		l->rx_len += (size_t)r;
		size_t off = 0;
		while (off < l->rx_len) {
			if (l->rx[off] != MAV_STX) {
				off++;
				l->garbage++;
				continue;
			}
			struct mav_frame f;
			const char *why = "";
			size_t used = mav_decode(l->rx + off, l->rx_len - off, &f, &l->signing, &why);
			if (used == 0)
				break; /* the rest of the frame is still on its way */
			/* A frame of an unknown id cannot be CRC-checked, and noise
			 * with a 0xFD in it reads as one: trusted as a frame only when
			 * the next frame starts right behind it (or nothing is there
			 * yet), else stepped over one byte at a time like other noise. */
			if (!f.valid && used > 1 && strncmp(why, "unknown", 7) == 0 && off + used < l->rx_len &&
			    l->rx[off + used] != MAV_STX) {
				off++;
				l->garbage++;
				continue;
			}
			off += take_frame(l, l->rx + off, used, &f, why, fn, ctx, &valid);
		}
		/* A frame's handler may send, and a send that finds the line gone
		 * closes it and empties this buffer (serial_down) under the loop
		 * above: what was parsed is then more than what is left. */
		if (off > l->rx_len)
			off = l->rx_len;
		if (off > 0) {
			memmove(l->rx, l->rx + off, l->rx_len - off);
			l->rx_len -= off;
		}
	}
	return valid;
}

int mav_link_receive(struct mav_link *l, void (*fn)(void *ctx, const struct mav_frame *f), void *ctx)
{
	if (l->serial)
		return serial_receive(l, fn, ctx);
	int valid = 0;
	for (;;) {
		uint8_t buf[2048];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(l->fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0)
			break;
		/* Whose datagram (mavlink.h). The peer used to be learned once and
		 * for good, and frames were read from anybody: a relay that came
		 * back on another source port kept the telemetry flowing in while
		 * every MANUAL_CONTROL and disarm went to the dead port, and on a
		 * link without signing anybody could report a disarmed ArduSub. */
		uint64_t now = now_mono_ns();
		int from_peer = l->have_peer && from.sin_addr.s_addr == l->peer.sin_addr.s_addr &&
				from.sin_port == l->peer.sin_port;
		int peer_there = l->peer_fixed || (l->peer_heard_ns && now - l->peer_heard_ns < l->peer_quiet_ns);
		if (l->have_peer && !from_peer && peer_there && !l->signing.enabled) {
			l->strangers++;
			continue;
		}
		size_t off = 0;
		while (off < (size_t)n) {
			struct mav_frame f;
			const char *why;
			size_t used = mav_decode(buf + off, (size_t)n - off, &f, &l->signing, &why);
			if (used == 0)
				break; /* a truncated frame: the rest of the datagram is dropped */
			off += used;
			if (!f.valid) {
				if (used > 1 && strncmp(why, "unknown", 7) == 0) {
					l->unknown++; /* a message outside tools/mavgen/messages.txt: not an error */
				} else if (used > 1) {
					l->rejected++;
					snprintf(l->last_reject, sizeof l->last_reject, "%s", why);
				}
				continue;
			}
			l->received++;
			valid++;
			if (!from_peer && !(l->have_peer && peer_there)) {
				if (l->have_peer)
					l->peer_changes++;
				l->peer = from;
				l->have_peer = 1;
				from_peer = 1;
			}
			if (from_peer)
				l->peer_heard_ns = now;
			tlog_write(l, buf + off - used, used);
			fn(ctx, &f);
		}
	}
	return valid;
}

static void usage(void)
{
	fputs("usage: hangar-mavlink <mavlink.conf>             the vehicle's module\n"
	      "       hangar-mavlink --topside <topside.conf>   the topside's module\n"
	      "       hangar-mavlink --neutral <mavlink.conf>   neutral manual control and a disarm, then exit\n",
	      stderr);
}

int main(int argc, char **argv)
{
	if (argc == 2 && argv[1][0] != '-')
		return mavlink_vehicle_main(argv[1]);
	if (argc == 3 && strcmp(argv[1], "--topside") == 0)
		return mavlink_topside_main(argv[2]);
	if (argc == 3 && strcmp(argv[1], "--neutral") == 0)
		return mavlink_neutral_main(argv[2]);
	usage();
	return 2;
}
