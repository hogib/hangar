// SPDX-License-Identifier: GPL-3.0-or-later
// The link of modules/mavlink/mavlink_main.c by itself, with no manager and
// no module around it: the serial line's transmit queue against a
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
	RUN_TEST(test_tlog_is_what_went);
	unlink(keyfile);
	unlink(tlogfile);
	TEST_MAIN_END();
}
