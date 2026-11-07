// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar-mavlink's entry point and the UDP link the three modes share. One
 * program rather than two: the vehicle's and the topside's module share the
 * framing, the signing, the configuration grammar and this link; which
 * module a process is comes from its wiring (the manifest names the role on
 * the Exec line), as every module's identity does. */
#define _GNU_SOURCE
#include "mavlink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mavlink_gen.h"

int mav_link_open(struct mav_link *l, const struct mavlink_conf *c, char *err, size_t errlen)
{
	memset(l, 0, sizeof l[0]);
	l->tlog_fd = -1;
	l->sysid = c->sysid;
	l->compid = c->compid;
	l->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (l->fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	int one = 1;
	setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(l->fd, (const struct sockaddr *)&c->bind, sizeof c->bind) < 0) {
		snprintf(err, errlen, "bind %s: %s", "the configured address", strerror(errno));
		close(l->fd);
		l->fd = -1;
		return -1;
	}
	if (c->have_peer) {
		l->peer = c->peer;
		l->have_peer = 1;
	}
	if (c->signing) {
		l->signing.enabled = 1;
		l->signing.link_id = 1;
		if (mav_signing_load_key(&l->signing, c->key_file, err, errlen) < 0) {
			close(l->fd);
			l->fd = -1;
			return -1;
		}
	}
	if (c->tlog[0]) {
		l->tlog_fd = open(c->tlog, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
		if (l->tlog_fd < 0) {
			snprintf(err, errlen, "tlog %s: %s", c->tlog, strerror(errno));
			close(l->fd);
			l->fd = -1;
			return -1;
		}
	}
	return 0;
}

void mav_link_close(struct mav_link *l)
{
	if (l->fd >= 0)
		close(l->fd);
	if (l->tlog_fd >= 0)
		close(l->tlog_fd);
	l->fd = l->tlog_fd = -1;
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

int mav_link_receive(struct mav_link *l, void (*fn)(void *ctx, const struct mav_frame *f), void *ctx)
{
	int valid = 0;
	for (;;) {
		uint8_t buf[2048];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(l->fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0)
			break;
		size_t off = 0;
		while (off < (size_t)n) {
			struct mav_frame f;
			const char *why;
			size_t used = mav_decode(buf + off, (size_t)n - off, &f, &l->signing, &why);
			if (used == 0)
				break; /* a truncated frame: the rest of the datagram is dropped */
			off += used;
			if (!f.valid) {
				if (used > 1) {
					l->rejected++;
					snprintf(l->last_reject, sizeof l->last_reject, "%s", why);
				}
				continue;
			}
			l->received++;
			valid++;
			if (!l->have_peer) {
				l->peer = from;
				l->have_peer = 1;
			}
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
