// SPDX-License-Identifier: GPL-3.0-or-later
/* tools/sim/pilot.c: a scripted stand-in for QGroundControl, for flying the
 * simulator from a terminal (no display, no joystick). It behaves as QGC does: it
 * listens on UDP 14550, where the topside's mavlink module sends the vehicle's
 * HEARTBEAT (its conf's GCS=), learns the module's address from that first packet,
 * and answers there with a GCS HEARTBEAT at 1 Hz, MANUAL_CONTROL at 20 Hz and
 * COMMAND_LONG for arm and disarm; it prints what comes back (HEARTBEAT armed flag
 * and mode, STATUSTEXT, COMMAND_ACK).
 *
 *   pilot [--listen host:port] step... step: arm | disarm | hold S |
 *                                     fwd S | back S | down S | up S | left S |
 *                                     right S | yaw S | yawl S   (S seconds, at
 *                                     half stick: 500 of 1000)
 *   pilot arm hold 2 fwd 8 down 6 yaw 4 hold 2 disarm
 *
 * Built by make (modules/mavlink/mavlink.mk): builddir/pilot.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mav.h"
#include "mavlink_gen.h"

static int fd;
static struct sockaddr_in to;
static int have_to;
static uint8_t seq;
static int armed_seen = -1;
static uint32_t mode_seen;
static uint64_t t0;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void send_frame(uint32_t msgid, const uint8_t *payload, size_t len, uint8_t crc)
{
	if (!have_to)
		return; /* nobody to talk to yet: the vehicle's heartbeat names the address */
	uint8_t frame[MAV_MAX_FRAME];
	size_t n = mav_encode(frame, sizeof frame, seq++, 255, MAV_COMP_ID_MISSIONPLANNER, msgid, payload, len, crc, NULL);
	sendto(fd, frame, n, 0, (struct sockaddr *)&to, sizeof to);
}

#define SEND(NAME, lname, msgp)                                                           \
	do {                                                                              \
		uint8_t pl_[MAV_LEN_##NAME];                                              \
		mav_pack_##lname((msgp), pl_);                                            \
		send_frame(MAV_MSG_ID_##NAME, pl_, sizeof pl_, MAV_CRC_##NAME);           \
	} while (0)

static void heartbeat(void)
{
	struct mav_heartbeat h = { .type = MAV_TYPE_GCS, .autopilot = MAV_AUTOPILOT_INVALID,
				   .system_status = MAV_STATE_ACTIVE, .mavlink_version = 3 };
	SEND(HEARTBEAT, heartbeat, &h);
}

static void manual(int16_t x, int16_t y, int16_t z, int16_t r)
{
	struct mav_manual_control m = { .target = 1, .x = x, .y = y, .z = z, .r = r };
	SEND(MANUAL_CONTROL, manual_control, &m);
}

static void command(uint16_t cmd, float p1)
{
	struct mav_command_long c;
	memset(&c, 0, sizeof c);
	c.target_system = 1;
	c.target_component = 1;
	c.command = cmd;
	c.param1 = p1;
	SEND(COMMAND_LONG, command_long, &c);
}

static void stamp(void)
{
	printf("[%6.2f] ", (double)(now_ns() - t0) / 1e9);
}

static void on_frame(const struct mav_frame *f)
{
	switch (f->msgid) {
	case MAV_MSG_ID_HEARTBEAT: {
		struct mav_heartbeat h;
		mav_unpack_heartbeat(f->payload, f->len, &h);
		if (h.type == MAV_TYPE_GCS)
			break;
		int armed = (h.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
		if (armed != armed_seen || h.custom_mode != mode_seen) {
			stamp();
			printf("vehicle heartbeat: %s, mode %u\n", armed ? "ARMED" : "disarmed", h.custom_mode);
			armed_seen = armed;
			mode_seen = h.custom_mode;
		}
		break;
	}
	case MAV_MSG_ID_STATUSTEXT: {
		struct mav_statustext s;
		mav_unpack_statustext(f->payload, f->len, &s);
		char text[51];
		memcpy(text, s.text, 50);
		text[50] = 0;
		stamp();
		printf("statustext(%u): %s\n", s.severity, text);
		break;
	}
	case MAV_MSG_ID_COMMAND_ACK: {
		struct mav_command_ack a;
		mav_unpack_command_ack(f->payload, f->len, &a);
		stamp();
		printf("ack: command %u result %u\n", a.command, a.result);
		break;
	}
	default:
		break;
	}
}

static void receive(void)
{
	for (;;) {
		uint8_t buf[2048];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n <= 0)
			break;
		if (!have_to) {
			to = from;
			have_to = 1;
			stamp();
			printf("vehicle at %s:%u\n", inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port));
		}
		size_t off = 0;
		while (off < (size_t)n) {
			struct mav_frame f;
			const char *why;
			size_t used = mav_decode(buf + off, (size_t)n - off, &f, NULL, &why);
			if (used == 0)
				break;
			off += used;
			if (f.valid)
				on_frame(&f);
		}
	}
}

/* Runs one step for secs seconds, keeping the link alive at the step's stick. */
static void run(double secs, int16_t x, int16_t y, int16_t z, int16_t r)
{
	uint64_t end = now_ns() + (uint64_t)(secs * 1e9), next_hb = 0, next_mc = 0;
	while (now_ns() < end) {
		uint64_t now = now_ns();
		if (now >= next_hb) {
			heartbeat();
			next_hb = now + 1000000000ull;
		}
		if (now >= next_mc) {
			manual(x, y, z, r);
			next_mc = now + 50000000ull;
		}
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		poll(&pf, 1, 10);
		receive();
	}
}

int main(int argc, char **argv)
{
	const char *listen = "127.0.0.1:14550";
	int i = 1;
	if (argc > 2 && strcmp(argv[1], "--listen") == 0) {
		listen = argv[2];
		i = 3;
	}
	char host[64];
	int port = 14550;
	if (sscanf(listen, "%63[^:]:%d", host, &port) < 1) {
		fprintf(stderr, "pilot: bad --listen %s\n", listen);
		return 2;
	}
	struct sockaddr_in me;
	memset(&me, 0, sizeof me);
	me.sin_family = AF_INET;
	me.sin_port = htons((uint16_t)port);
	inet_pton(AF_INET, host, &me.sin_addr);
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(fd, (struct sockaddr *)&me, sizeof me) < 0) {
		perror("pilot: bind");
		return 1;
	}
	t0 = now_ns();
	/* Wait for the vehicle's heartbeat (1 Hz) to learn where to answer. */
	uint64_t until = now_ns() + 5000000000ull;
	while (!have_to && now_ns() < until)
		run(0.1, 0, 0, 500, 0);
	if (!have_to) {
		fprintf(stderr, "pilot: no vehicle heartbeat on %s within 5 s\n", listen);
		return 1;
	}
	run(1.0, 0, 0, 500, 0);
	for (; i < argc; i++) {
		const char *s = argv[i];
		double secs = i + 1 < argc ? atof(argv[i + 1]) : 0;
		stamp();
		if (strcmp(s, "arm") == 0) {
			printf("step: arm\n");
			command(MAV_CMD_COMPONENT_ARM_DISARM, 1);
			run(0.5, 0, 0, 500, 0);
		} else if (strcmp(s, "disarm") == 0) {
			printf("step: disarm\n");
			command(MAV_CMD_COMPONENT_ARM_DISARM, 0);
			run(1.5, 0, 0, 500, 0);
		} else if (strcmp(s, "hold") == 0) {
			printf("step: hold %gs\n", secs);
			run(secs, 0, 0, 500, 0);
			i++;
		} else if (strcmp(s, "fwd") == 0) {
			printf("step: forward %gs\n", secs);
			run(secs, 500, 0, 500, 0);
			i++;
		} else if (strcmp(s, "back") == 0) {
			printf("step: back %gs\n", secs);
			run(secs, -500, 0, 500, 0);
			i++;
		} else if (strcmp(s, "down") == 0) {
			printf("step: down %gs\n", secs);
			run(secs, 0, 0, 250, 0);
			i++;
		} else if (strcmp(s, "up") == 0) {
			printf("step: up %gs\n", secs);
			run(secs, 0, 0, 750, 0);
			i++;
		} else if (strcmp(s, "left") == 0) {
			printf("step: left %gs\n", secs);
			run(secs, 0, -500, 500, 0);
			i++;
		} else if (strcmp(s, "right") == 0) {
			printf("step: right %gs\n", secs);
			run(secs, 0, 500, 500, 0);
			i++;
		} else if (strcmp(s, "yaw") == 0) {
			printf("step: yaw right %gs\n", secs);
			run(secs, 0, 0, 500, 500);
			i++;
		} else if (strcmp(s, "yawl") == 0) {
			printf("step: yaw left %gs\n", secs);
			run(secs, 0, 0, 500, -500);
			i++;
		} else {
			fprintf(stderr, "pilot: unknown step %s\n", s);
			return 2;
		}
	}
	run(0.5, 0, 0, 500, 0);
	return 0;
}
