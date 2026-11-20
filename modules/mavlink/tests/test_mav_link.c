// SPDX-License-Identifier: GPL-3.0-or-later
// The link of modules/mavlink/mavlink_main.c by itself, with no manager and
// no module around it. The UDP link's peer: whose datagrams are read, and when
// a learned peer gives way to another address. The serial line's transmit queue against a
// pseudo-terminal whose far end does not read (the kernel's buffer fills,
// then the module's queue holds what follows), with signing on, and what
// comes out when the far end reads again judged by a receiver that checks
// signatures and wants rising timestamps, as ArduSub's does. Nothing here
// depends on time: the line is blocked for as long as the test says.
// mavlink_main.c is compiled into this file, its main renamed, so that the
// queue's own state can be looked at.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define main mavlink_prog_main
#include "../mavlink_main.c"
#undef main
int mavlink_vehicle_main(const char *p) { (void)p; return 0; }
int mavlink_topside_main(const char *p) { (void)p; return 0; }
int mavlink_neutral_main(const char *p) { (void)p; return 0; }

#include "test_harness.h"

static struct mav_link L;
static int master = -1;
static char keyfile[256], tlogfile[300];

static void open_line(int tlog)
{
	master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
		printf("    no pseudo-terminal: %s\n", strerror(errno));
		exit(2);
	}
	struct mavlink_conf c;
	memset(&c, 0, sizeof c);
	snprintf(c.serial, sizeof c.serial, "%s", ptsname(master));
	c.baud = 115200;
	c.signing = 1;
	snprintf(c.key_file, sizeof c.key_file, "%s", keyfile);
	if (tlog)
		snprintf(c.tlog, sizeof c.tlog, "%s", tlogfile);
	c.sysid = 255;
	c.compid = 190;
	char err[128] = "";
	if (mav_link_open_ex(&L, &c, 0, err, sizeof err) < 0 || L.fd < 0) {
		printf("    cannot open the line: %s %s\n", err, L.line_state);
		exit(2);
	}
}

static void close_line(void)
{
	mav_link_close(&L);
	if (master >= 0)
		close(master);
	master = -1;
}

static int send_mc(int x)
{
	struct mav_manual_control m;
	memset(&m, 0, sizeof m);
	m.target = 1;
	m.x = (int16_t)x;
	m.z = 500;
	uint8_t pl[MAV_LEN_MANUAL_CONTROL];
	mav_pack_manual_control(&m, pl);
	return mav_link_send(&L, MAV_MSG_ID_MANUAL_CONTROL, pl, sizeof pl, MAV_CRC_MANUAL_CONTROL);
}

static int send_cmd(uint16_t command, float p1, float p2)
{
	struct mav_command_long c;
	memset(&c, 0, sizeof c);
	c.target_system = 1;
	c.target_component = 1;
	c.command = command;
	c.param1 = p1;
	c.param2 = p2;
	uint8_t pl[MAV_LEN_COMMAND_LONG];
	mav_pack_command_long(&c, pl);
	return mav_link_send(&L, MAV_MSG_ID_COMMAND_LONG, pl, sizeof pl, MAV_CRC_COMMAND_LONG);
}

#define send_disarm() send_cmd(MAV_CMD_COMPONENT_ARM_DISARM, 0, 0)
#define send_arm() send_cmd(MAV_CMD_COMPONENT_ARM_DISARM, 1, 0)

static int send_hb(void)
{
	struct mav_heartbeat h;
	memset(&h, 0, sizeof h);
	h.type = 6;
	uint8_t pl[MAV_LEN_HEARTBEAT];
	mav_pack_heartbeat(&h, pl);
	return mav_link_send(&L, MAV_MSG_ID_HEARTBEAT, pl, sizeof pl, MAV_CRC_HEARTBEAT);
}

/* Fills the kernel's buffer of the unread line until the module's own queue
 * starts to hold frames (HEARTBEATs: not control frames, never superseded),
 * then takes those out of the queue again by reading just enough: the queue
 * is empty, and the next frame sent stays in it. */
static void block_line(void)
{
	int n = 0;
	while (L.tx_count == 0 && n < 1000000) {
		send_hb();
		n++;
	}
	if (L.tx_count == 0) {
		printf("    the line never blocked\n");
		exit(2);
	}
}

/* What the far end received, decoded as ArduSub would (the same key,
 * signatures required, timestamps rising per stream). */
static struct {
	int disarms, arms, mc, rates, heartbeats, other;
	int refused;          /* frames the receiver refused, of any kind */
	int refused_disarms;
	char refused_why[64];
	int last_mc_x;
	int seq_backwards;    /* a sequence number below the one before it (mod 256, by less than half) */
	size_t bytes;
} R;

static uint8_t rxbuf[4 << 20];

/* The far end reads: everything the line and the queue hold. */
static void drain_and_decode(void)
{
	size_t len = 0;
	for (int idle = 0; idle < 50;) {
		mav_link_service(&L, 0);
		ssize_t r = read(master, rxbuf + len, sizeof rxbuf - len);
		if (r > 0) {
			len += (size_t)r;
			idle = 0;
		} else if (L.tx_count == 0) {
			idle++;
			usleep(1000);
		} else {
			usleep(1000);
		}
	}
	struct mav_signing rx;
	memset(&rx, 0, sizeof rx);
	rx.enabled = 1;
	char err[64];
	mav_signing_load_key(&rx, keyfile, err, sizeof err);
	memset(&R, 0, sizeof R);
	R.bytes = len;
	int last_seq = -1;
	for (size_t off = 0; off < len;) {
		if (rxbuf[off] != MAV_STX) {
			off++;
			R.other++;
			continue;
		}
		struct mav_frame f;
		const char *why = "";
		size_t used = mav_decode(rxbuf + off, len - off, &f, &rx, &why);
		if (!used)
			break;
		if (used == 1) {
			off++;
			continue;
		}
		off += used;
		if (last_seq >= 0 && (uint8_t)(f.seq - last_seq) > 128)
			R.seq_backwards++;
		last_seq = f.seq;
		struct mav_command_long c;
		memset(&c, 0, sizeof c);
		if (f.msgid == MAV_MSG_ID_COMMAND_LONG)
			mav_unpack_command_long(f.payload, f.len, &c);
		int disarm = f.msgid == MAV_MSG_ID_COMMAND_LONG && c.command == MAV_CMD_COMPONENT_ARM_DISARM && c.param1 == 0;
		int arm = f.msgid == MAV_MSG_ID_COMMAND_LONG && c.command == MAV_CMD_COMPONENT_ARM_DISARM && c.param1 == 1;
		if (!f.valid) {
			R.refused++;
			R.refused_disarms += disarm;
			snprintf(R.refused_why, sizeof R.refused_why, "%s", why);
			continue;
		}
		if (f.msgid == MAV_MSG_ID_MANUAL_CONTROL) {
			struct mav_manual_control m;
			mav_unpack_manual_control(f.payload, f.len, &m);
			R.mc++;
			R.last_mc_x = m.x;
		} else if (disarm)
			R.disarms++;
		else if (arm)
			R.arms++;
		else if (f.msgid == MAV_MSG_ID_COMMAND_LONG && c.command == MAV_CMD_SET_MESSAGE_INTERVAL)
			R.rates++;
		else if (f.msgid == MAV_MSG_ID_HEARTBEAT)
			R.heartbeats++;
	}
}

/* A MANUAL_CONTROL that replaces a queued one takes its place in the queue.
 * It used to be framed and signed at that moment, later than the frames
 * queued behind it: the receiver, which wants each stream's timestamps to
 * rise, then refused every one of those, a disarm included, and all three
 * sends of a disarm 100 ms apart land behind the same MANUAL_CONTROL. A
 * message is framed when its turn on the line comes now, so sequence numbers
 * and timestamps leave in order whatever was replaced meanwhile. */
static void test_signed_in_the_order_written(void)
{
	open_line(0);
	block_line();
	int q0 = L.tx_count;
	CHECK_EQ_INT(send_mc(100), 0);      /* queued */
	CHECK_EQ_INT(send_disarm(), 0);     /* behind it */
	usleep(2000);                       /* the clock moves on: a later timestamp for what is framed now */
	CHECK_EQ_INT(send_mc(200), 0);      /* replaces the queued MANUAL_CONTROL, in its place */
	CHECK_EQ_INT(L.tx_count, q0 + 2);
	CHECK_EQ_INT(L.tx_superseded, 1);
	CHECK_EQ_INT(send_cmd(MAV_CMD_SET_MESSAGE_INTERVAL, 30, 20000), 0);
	usleep(2000);
	CHECK_EQ_INT(send_mc(300), 0);
	CHECK_EQ_INT(L.tx_superseded, 2);
	drain_and_decode();
	CHECK_EQ_INT(R.refused, 0);
	if (R.refused)
		printf("    %d frames refused by the receiver, %d of them disarms: %s\n", R.refused, R.refused_disarms,
		       R.refused_why);
	CHECK_EQ_INT(R.disarms, 1);
	CHECK_EQ_INT(R.rates, 1);
	CHECK_EQ_INT(R.mc, 1);
	CHECK_EQ_INT(R.last_mc_x, 300); /* the newest command, once */
	CHECK_EQ_INT(R.seq_backwards, 0);
	CHECK_EQ_INT(R.other, 0);
	CHECK_EQ_INT(L.tx_count, 0);
	close_line();
}

static int queued_of(int kind)
{
	int n = 0;
	for (int i = 0; i < L.tx_count; i++)
		n += L.txq[(L.tx_head + i) % MAV_TXQ].kind == kind;
	return n;
}

/* A minute of what the vehicle's module sends, into a line that stays
 * blocked: per 5 s, 250 MANUAL_CONTROL, 6 rate requests
 * (MAV_CMD_SET_MESSAGE_INTERVAL) and some parameter traffic, with a disarm's
 * three sends in between. Every COMMAND_LONG used to be a control frame
 * that nothing could evict: after 25 s the 32 places held rate requests and
 * mav_link_send(disarm) returned -1. A disarm is never the frame dropped:
 * rate requests are evictable, and a disarm sent again takes the place of
 * the one still waiting, so that disarms cannot fill the queue either. */
static void test_a_disarm_is_never_dropped(void)
{
	open_line(0);
	block_line();
	for (int round = 0; round < 12; round++) {
		for (int i = 0; i < 6; i++)
			send_cmd(MAV_CMD_SET_MESSAGE_INTERVAL, (float)(30 + i), 20000);
		for (int i = 0; i < 250; i++)
			CHECK_EQ_INT(send_mc(i), 0);
		for (int i = 0; i < 5; i++)
			send_hb();
		if (round % 2)
			for (int i = 0; i < 3; i++)
				CHECK_EQ_INT(send_disarm(), 0);
	}
	CHECK_EQ_INT(L.tx_count, MAV_TXQ); /* full, of frames that may be evicted */
	CHECK_EQ_INT(queued_of(MAV_TX_MANUAL_CONTROL), 1);
	CHECK_EQ_INT(queued_of(MAV_TX_DISARM), 1);
	CHECK_EQ_INT(queued_of(MAV_TX_OTHER), MAV_TXQ - 2);
	CHECK(L.tx_dropped > 0);
	/* The disarm that matters, into the full queue; and its two repeats. */
	CHECK_EQ_INT(send_disarm(), 0);
	CHECK_EQ_INT(send_disarm(), 0);
	CHECK_EQ_INT(send_disarm(), 0);
	CHECK_EQ_INT(send_mc(777), 0);
	CHECK_EQ_INT(L.tx_count, MAV_TXQ);
	/* With no disarm waiting (a queue of nothing but telemetry-side frames),
	 * a disarm takes the place of the newest of them. */
	close_line();
	open_line(0);
	block_line();
	for (int i = 0; i < 100; i++)
		send_cmd(MAV_CMD_SET_MESSAGE_INTERVAL, (float)(30 + i % 6), 20000);
	CHECK_EQ_INT(L.tx_count, MAV_TXQ);
	CHECK_EQ_INT(queued_of(MAV_TX_OTHER), MAV_TXQ);
	uint64_t dropped = L.tx_dropped;
	CHECK_EQ_INT(send_disarm(), 0);
	CHECK_EQ_INT(L.tx_dropped, dropped + 1); /* the rate request it replaced */
	CHECK_EQ_INT(queued_of(MAV_TX_DISARM), 1);
	CHECK_EQ_INT(send_mc(5), 0);
	CHECK_EQ_INT(send_cmd(MAV_CMD_SET_MESSAGE_INTERVAL, 30, 20000), -1); /* a rate request finds no room, and is none */
	drain_and_decode();
	CHECK_EQ_INT(R.refused, 0);
	CHECK_EQ_INT(R.disarms, 1);
	CHECK_EQ_INT(R.mc, 1);
	CHECK_EQ_INT(R.last_mc_x, 5);
	CHECK_EQ_INT(R.seq_backwards, 0);
	close_line();
}

/* An arm is for the moment it was asked in. On a blocked line it waited in
 * the queue for as long as the block lasted and was delivered when the line
 * drained, whatever had happened meanwhile: the safety module's arm_timeout
 * (2 s) long over, or a disarm queued right behind it. A disarm now takes
 * every arm still waiting out of the queue, and so does mav_link_cancel_arm,
 * which the vehicle's module calls while no arming window is open. */
static void test_a_queued_arm_does_not_outlive_its_window(void)
{
	/* A disarm behind it. */
	open_line(0);
	block_line();
	CHECK_EQ_INT(send_arm(), 0);
	CHECK_EQ_INT(send_arm(), 0); /* asked twice: one waits */
	CHECK_EQ_INT(queued_of(MAV_TX_ARM), 1);
	CHECK_EQ_INT(send_mc(1), 0);
	CHECK_EQ_INT(send_disarm(), 0);
	CHECK_EQ_INT(queued_of(MAV_TX_ARM), 0);
	CHECK_EQ_INT(queued_of(MAV_TX_DISARM), 1);
	drain_and_decode();
	CHECK_EQ_INT(R.arms, 0);
	CHECK_EQ_INT(R.disarms, 1);
	CHECK_EQ_INT(R.mc, 1);
	CHECK_EQ_INT(R.refused, 0);
	close_line();
	/* The window closed with no disarm to send (ArduSub never armed). */
	open_line(0);
	block_line();
	CHECK_EQ_INT(send_arm(), 0);
	CHECK_EQ_INT(send_mc(2), 0);
	CHECK_EQ_INT(mav_link_cancel_arm(&L), 1);
	CHECK_EQ_INT(mav_link_cancel_arm(&L), 0);
	CHECK_EQ_INT(queued_of(MAV_TX_ARM), 0);
	drain_and_decode();
	CHECK_EQ_INT(R.arms, 0);
	CHECK_EQ_INT(R.mc, 1);
	CHECK_EQ_INT(R.refused, 0);
	close_line();
	/* A line that takes it: the arm goes at once, and there is nothing to cancel. */
	open_line(0);
	CHECK_EQ_INT(send_arm(), 0);
	CHECK_EQ_INT(L.tx_count, 0);
	CHECK_EQ_INT(mav_link_cancel_arm(&L), 0);
	drain_and_decode();
	CHECK_EQ_INT(R.arms, 1);
	close_line();
}

/* A frame handler that sends, on a line that went away under it. The send
 * finds the line gone (EIO), serial_down closes it and empties the receive
 * buffer, and the receive loop, back from the handler, moved "what is left"
 * of a buffer that now held less than it had parsed: a length of minus one
 * frame, as a size_t, and kept that as the buffer's length. The vehicle's
 * handler sends nothing today; the next one might. Run in a child, since a
 * memmove of that length may do anything: here it left a closed line with a
 * receive buffer "holding" nearly 2^64 bytes. */
static int frames_handled;

static void handler_that_sends(void *ctx, const struct mav_frame *f)
{
	(void)ctx;
	(void)f;
	frames_handled++;
	if (master >= 0) {
		close(master); /* the far end is gone: an unplugged cable */
		master = -1;
	}
	send_hb();
}

static void test_line_gone_inside_a_handler(void)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		open_line(0);
		/* Two frames from the far end, signed with the link's key, in one write. */
		struct mav_signing far;
		memset(&far, 0, sizeof far);
		far.enabled = 1;
		far.link_id = 2;
		char err[64];
		mav_signing_load_key(&far, keyfile, err, sizeof err);
		uint8_t buf[2 * MAV_MAX_FRAME], pl[MAV_LEN_HEARTBEAT];
		struct mav_heartbeat h;
		memset(&h, 0, sizeof h);
		h.type = 12;
		mav_pack_heartbeat(&h, pl);
		size_t n = 0;
		for (int i = 0; i < 2; i++)
			n += mav_encode(buf + n, sizeof buf - n, (uint8_t)i, 1, 1, MAV_MSG_ID_HEARTBEAT, pl, sizeof pl,
					MAV_CRC_HEARTBEAT, &far);
		if (write(master, buf, n) != (ssize_t)n)
			_exit(10);
		int valid = 0;
		for (int i = 0; i < 200 && !frames_handled; i++) {
			valid += mav_link_receive(&L, handler_that_sends, NULL);
			usleep(1000);
		}
		if (frames_handled != 1)
			_exit(11); /* the second frame went with the buffer */
		if (L.fd >= 0 || L.rx_len != 0 || L.drops != 1)
			_exit(12);
		if (strncmp(L.line_state, "down (write:", 12) != 0)
			_exit(13);
		_exit(valid == 1 ? 0 : 14);
	}
	int st = 0;
	CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
	CHECK(WIFEXITED(st));
	if (WIFSIGNALED(st))
		printf("    the child died of signal %d\n", WTERMSIG(st));
	CHECK_EQ_INT(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
}

/* ---- UDP: whose datagrams are read, and where the commands go ---------------- */

static int udp_frames;

static void count_frame(void *ctx, const struct mav_frame *f)
{
	(void)ctx;
	(void)f;
	udp_frames++;
}

static int udp_socket(struct sockaddr_in *addr)
{
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t sl = sizeof sa;
	if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 || getsockname(s, (struct sockaddr *)&sa, &sl) < 0) {
		printf("    no UDP socket: %s\n", strerror(errno));
		exit(2);
	}
	*addr = sa;
	return s;
}

/* The link on a free loopback port; `fixed` names its peer in the configuration. */
static struct sockaddr_in open_udp(int signing, const struct sockaddr_in *fixed)
{
	struct sockaddr_in at;
	int probe = udp_socket(&at);
	close(probe);
	struct mavlink_conf c;
	memset(&c, 0, sizeof c);
	c.bind = at;
	c.signing = signing;
	snprintf(c.key_file, sizeof c.key_file, "%s", keyfile);
	c.sysid = 255;
	c.compid = 190;
	if (fixed) {
		c.peer = *fixed;
		c.have_peer = 1;
	}
	char err[128] = "";
	if (mav_link_open_ex(&L, &c, 0, err, sizeof err) < 0) {
		printf("    cannot open the UDP link: %s\n", err);
		exit(2);
	}
	return at;
}

/* One HEARTBEAT from socket s to the link, signed with `sign` or not; then the
 * link reads what came (the datagram is waited for, not assumed there). The
 * number of frames the handler got. */
static int heartbeat_from(int s, const struct sockaddr_in *to, struct mav_signing *sign, uint8_t seq)
{
	struct mav_heartbeat h;
	memset(&h, 0, sizeof h);
	h.type = 12;
	uint8_t pl[MAV_LEN_HEARTBEAT], frame[MAV_MAX_FRAME];
	mav_pack_heartbeat(&h, pl);
	size_t n = mav_encode(frame, sizeof frame, seq, 1, 1, MAV_MSG_ID_HEARTBEAT, pl, sizeof pl, MAV_CRC_HEARTBEAT, sign);
	if (sendto(s, frame, n, 0, (const struct sockaddr *)to, sizeof *to) != (ssize_t)n)
		return -1;
	struct pollfd p = { .fd = L.fd, .events = POLLIN };
	if (poll(&p, 1, 5000) <= 0)
		return -2;
	udp_frames = 0;
	mav_link_receive(&L, count_frame, NULL);
	return udp_frames;
}

/* 1 if a datagram the link sent arrives at socket s within 5 s. */
static int arrives_at(int s)
{
	struct pollfd p = { .fd = s, .events = POLLIN };
	uint8_t buf[512];
	return poll(&p, 1, 5000) > 0 && recv(s, buf, sizeof buf, MSG_DONTWAIT) > 0;
}

static int nothing_at(int s)
{
	uint8_t buf[512];
	return recv(s, buf, sizeof buf, MSG_DONTWAIT) < 0;
}

/* The peer was learned from the first valid frame and never again, and frames
 * were read from any address. On a link without signing (the lab's): anybody
 * who reached the port could report a disarmed ArduSub in MANUAL, and a relay
 * that came back from another source port kept its telemetry flowing in
 * (ArduSub "heard") while every MANUAL_CONTROL and disarm went to the port
 * nobody held any more, for good. */
static void test_udp_peer(void)
{
	struct sockaddr_in a_addr, b_addr;
	int a = udp_socket(&a_addr), b = udp_socket(&b_addr);
	/* Without signing, the peer learned. */
	struct sockaddr_in at = open_udp(0, NULL);
	L.peer_quiet_ns = 3600ull * 1000000000ull; /* the peer is "there" for this part, however slow the machine */
	CHECK_EQ_INT(send_hb(), -1);                     /* nobody to send to yet */
	CHECK_EQ_INT(heartbeat_from(a, &at, NULL, 0), 1); /* A speaks: A is the peer */
	CHECK(L.have_peer && L.peer.sin_port == a_addr.sin_port);
	CHECK_EQ_INT(heartbeat_from(b, &at, NULL, 0), 0); /* B is not read */
	CHECK_EQ_INT(heartbeat_from(b, &at, NULL, 1), 0);
	CHECK_EQ_INT(L.strangers, 2);
	CHECK_EQ_INT(L.received, 1);
	CHECK(L.peer.sin_port == a_addr.sin_port);
	CHECK_EQ_INT(send_hb(), 0);
	CHECK(arrives_at(a) && nothing_at(b));
	/* A falls silent for longer than the bound (the relay restarted and
	 * speaks from B now): B's next frame is read and B is the peer. */
	L.peer_quiet_ns = 20000000ull;
	usleep(60000);
	CHECK_EQ_INT(heartbeat_from(b, &at, NULL, 2), 1);
	CHECK(L.peer.sin_port == b_addr.sin_port);
	CHECK_EQ_INT(L.peer_changes, 1);
	CHECK_EQ_INT(send_hb(), 0);
	CHECK(arrives_at(b) && nothing_at(a));
	/* And A, back while B speaks, is the stranger. */
	L.peer_quiet_ns = 3600ull * 1000000000ull;
	CHECK_EQ_INT(heartbeat_from(a, &at, NULL, 3), 0);
	CHECK(L.peer.sin_port == b_addr.sin_port);
	mav_link_close(&L);
	/* A configured peer never changes, silent or not; a stranger is never read. */
	at = open_udp(0, &a_addr);
	L.peer_quiet_ns = 1;
	CHECK_EQ_INT(send_hb(), 0);
	CHECK(arrives_at(a));
	usleep(2000);
	CHECK_EQ_INT(heartbeat_from(b, &at, NULL, 0), 0);
	CHECK_EQ_INT(L.strangers, 1);
	CHECK_EQ_INT(heartbeat_from(a, &at, NULL, 0), 1);
	usleep(2000);
	CHECK_EQ_INT(heartbeat_from(b, &at, NULL, 1), 0);
	CHECK(L.peer.sin_port == a_addr.sin_port);
	CHECK_EQ_INT(L.peer_changes, 0);
	mav_link_close(&L);
	/* With signing, the signature says who may speak: a frame from another
	 * address that verifies is read (a second component behind the same key),
	 * one that does not is refused as from anywhere; neither becomes the peer
	 * while the peer is there. */
	struct mav_signing sa, sb;
	memset(&sa, 0, sizeof sa);
	sa.enabled = 1;
	sa.link_id = 3;
	char err[64];
	mav_signing_load_key(&sa, keyfile, err, sizeof err);
	sb = sa;
	sb.link_id = 4;
	at = open_udp(1, NULL);
	L.peer_quiet_ns = 3600ull * 1000000000ull;
	CHECK_EQ_INT(heartbeat_from(a, &at, &sa, 0), 1);
	CHECK_EQ_INT(heartbeat_from(b, &at, &sb, 0), 1);
	CHECK(L.peer.sin_port == a_addr.sin_port);
	CHECK_EQ_INT(heartbeat_from(b, &at, NULL, 1), 0);
	CHECK_EQ_INT(L.rejected, 1);
	CHECK_EQ_INT(L.strangers, 0);
	CHECK_EQ_INT(send_hb(), 0);
	CHECK(arrives_at(a) && nothing_at(b));
	/* Silent, the peer gives way to a frame that verifies, not to one that does not. */
	L.peer_quiet_ns = 20000000ull;
	usleep(60000);
	CHECK_EQ_INT(heartbeat_from(b, &at, NULL, 2), 0);
	CHECK(L.peer.sin_port == a_addr.sin_port);
	CHECK_EQ_INT(heartbeat_from(b, &at, &sb, 3), 1);
	CHECK(L.peer.sin_port == b_addr.sin_port);
	mav_link_close(&L);
	close(a);
	close(b);
}

/* The tlog holds what went on the line, whole signed frames in the order
 * they went: a reader of it (pymavlink) must find every frame well formed. */
static void test_tlog_is_what_went(void)
{
	unlink(tlogfile);
	open_line(1);
	block_line();
	send_mc(100);
	send_disarm();
	send_mc(200);
	off_t before = lseek(L.tlog_fd, 0, SEEK_END);
	drain_and_decode();
	int on_line = R.mc + R.disarms + R.heartbeats;
	CHECK_EQ_INT(R.refused, 0);
	close_line();
	FILE *f = fopen(tlogfile, "rb");
	CHECK(f != NULL);
	if (!f)
		return;
	static uint8_t buf[4 << 20];
	size_t len = fread(buf, 1, sizeof buf, f);
	fclose(f);
	CHECK((off_t)len > before);
	struct mav_signing rx;
	memset(&rx, 0, sizeof rx);
	rx.enabled = 1;
	char err[64];
	mav_signing_load_key(&rx, keyfile, err, sizeof err);
	int frames = 0, bad = 0;
	for (size_t off = 0; off + 8 < len;) {
		struct mav_frame fr;
		const char *why = "";
		size_t used = mav_decode(buf + off + 8, len - off - 8, &fr, &rx, &why);
		if (used < MAV_HEADER_LEN || !fr.valid) {
			bad++;
			break;
		}
		frames++;
		off += 8 + used;
	}
	CHECK_EQ_INT(bad, 0);
	CHECK_EQ_INT(frames, on_line);
	unlink(tlogfile);
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(keyfile, sizeof keyfile, "%s/test-mav-link-key-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	int kfd = mkstemp(keyfile);
	if (kfd < 0 || write(kfd, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n", 65) != 65) {
		printf("cannot write %s: %s\n", keyfile, strerror(errno));
		return 2;
	}
	close(kfd);
	snprintf(tlogfile, sizeof tlogfile, "%s.tlog", keyfile);
	RUN_TEST(test_signed_in_the_order_written);
	RUN_TEST(test_a_disarm_is_never_dropped);
	RUN_TEST(test_a_queued_arm_does_not_outlive_its_window);
	RUN_TEST(test_line_gone_inside_a_handler);
	RUN_TEST(test_udp_peer);
	RUN_TEST(test_tlog_is_what_went);
	unlink(keyfile);
	unlink(tlogfile);
	TEST_MAIN_END();
}
