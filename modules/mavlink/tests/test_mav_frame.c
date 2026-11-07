// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
// MAVLink 2 framing, CRC, CRC_EXTRA and signing against known-good bytes:
// the CRC-16/MCRF4XX check value, the CRC_EXTRA of every message the
// generator emits against the reference C library's constants, byte-exact
// packets produced by pymavlink 2.4.50 in a one-off run while this was
// written (checked in here; pymavlink is not a dependency), SHA-256's test
// vector, a signed packet reproduced bit for bit, and the signing rules of
// message_signing.html: an unsigned or wrongly signed frame refused, a
// timestamp that does not rise refused, one a minute behind refused.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mav.h"
#include "mavlink_gen.h"
#include "sha256.h"
#include "test_harness.h"

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
	size_t n = 0;
	for (; hex[0] && hex[1] && n < cap; hex += 2) {
		unsigned v;
		if (sscanf(hex, "%2x", &v) != 1)
			break;
		out[n++] = (uint8_t)v;
	}
	return n;
}

static void test_crc(void)
{
	/* CRC-16/MCRF4XX check value of "123456789" is 0x6f91. */
	CHECK_EQ_INT(mav_crc((const uint8_t *)"123456789", 9, 0xffff), 0x6f91);
	/* The generator's CRC_EXTRA against mavlink/c_library_v2's constants. */
	CHECK_EQ_INT(MAV_CRC_HEARTBEAT, 50);
	CHECK_EQ_INT(MAV_CRC_SYS_STATUS, 124);
	CHECK_EQ_INT(MAV_CRC_SET_MODE, 89);
	CHECK_EQ_INT(MAV_CRC_PARAM_REQUEST_READ, 214);
	CHECK_EQ_INT(MAV_CRC_PARAM_REQUEST_LIST, 159);
	CHECK_EQ_INT(MAV_CRC_PARAM_VALUE, 220);
	CHECK_EQ_INT(MAV_CRC_PARAM_SET, 168);
	CHECK_EQ_INT(MAV_CRC_SCALED_IMU, 170);
	CHECK_EQ_INT(MAV_CRC_SCALED_PRESSURE, 115);
	CHECK_EQ_INT(MAV_CRC_ATTITUDE, 39);
	CHECK_EQ_INT(MAV_CRC_SERVO_OUTPUT_RAW, 222);
	CHECK_EQ_INT(MAV_CRC_MISSION_REQUEST_LIST, 132);
	CHECK_EQ_INT(MAV_CRC_MISSION_COUNT, 221);
	CHECK_EQ_INT(MAV_CRC_REQUEST_DATA_STREAM, 148);
	CHECK_EQ_INT(MAV_CRC_MANUAL_CONTROL, 243);
	CHECK_EQ_INT(MAV_CRC_RC_CHANNELS_OVERRIDE, 124);
	CHECK_EQ_INT(MAV_CRC_VFR_HUD, 20);
	CHECK_EQ_INT(MAV_CRC_COMMAND_INT, 158);
	CHECK_EQ_INT(MAV_CRC_COMMAND_LONG, 152);
	CHECK_EQ_INT(MAV_CRC_COMMAND_ACK, 143);
	CHECK_EQ_INT(MAV_CRC_SCALED_PRESSURE2, 195);
	CHECK_EQ_INT(MAV_CRC_BATTERY_STATUS, 154);
	CHECK_EQ_INT(MAV_CRC_NAMED_VALUE_FLOAT, 170);
	CHECK_EQ_INT(MAV_CRC_STATUSTEXT, 83);
	/* Lengths, the same source. */
	CHECK_EQ_INT(MAV_LEN_MANUAL_CONTROL, 30);
	CHECK_EQ_INT(MAV_MIN_LEN_MANUAL_CONTROL, 11);
	CHECK_EQ_INT(MAV_LEN_HEARTBEAT, 9);
	CHECK_EQ_INT(MAV_LEN_STATUSTEXT, 54);
	CHECK_EQ_INT(MAV_MIN_LEN_STATUSTEXT, 51);
	CHECK_EQ_INT(MAV_LEN_BATTERY_STATUS, 54);
	CHECK_EQ_INT(MAV_MIN_LEN_BATTERY_STATUS, 36);
	CHECK_EQ_INT(MAV_LEN_COMMAND_ACK, 10);
	CHECK_EQ_INT(MAV_MIN_LEN_COMMAND_ACK, 3);
	CHECK_EQ_INT(MAV_MIN_LEN_SERVO_OUTPUT_RAW, 21);
	CHECK_EQ_INT(MAV_LEN_SERVO_OUTPUT_RAW, 37);
}

/* Encodes m through its pack function and compares with the pymavlink bytes. */
#define VECTOR(NAME, lname, SEQ, init, hex)                                                                   \
	do {                                                                                                  \
		struct mav_##lname m = init;                                                                  \
		uint8_t payload[MAV_LEN_##NAME], frame[MAV_MAX_FRAME], want[MAV_MAX_FRAME];                   \
		size_t n = mav_pack_##lname(&m, payload);                                                     \
		CHECK_EQ_INT(n, MAV_LEN_##NAME);                                                              \
		size_t fl = mav_encode(frame, sizeof frame, SEQ, 255, 190, MAV_MSG_ID_##NAME, payload, n,     \
				       MAV_CRC_##NAME, NULL);                                                 \
		size_t wl = unhex(hex, want, sizeof want);                                                    \
		CHECK_EQ_INT(fl, wl);                                                                         \
		CHECK_MEM_EQ(frame, want, wl);                                                                \
		struct mav_frame f;                                                                           \
		const char *why;                                                                              \
		CHECK_EQ_INT(mav_decode(want, wl, &f, NULL, &why), wl);                                       \
		CHECK(f.valid);                                                                               \
		CHECK_EQ_INT(f.msgid, MAV_MSG_ID_##NAME);                                                     \
		CHECK_EQ_INT(f.seq, SEQ);                                                                     \
		CHECK_EQ_INT(f.sysid, 255);                                                                   \
		struct mav_##lname back;                                                                      \
		uint8_t again[MAV_LEN_##NAME];                                                                \
		mav_unpack_##lname(f.payload, f.len, &back);                                                  \
		mav_pack_##lname(&back, again);                                                               \
		CHECK_MEM_EQ(again, payload, sizeof payload); /* the round trip, padding aside */            \
	} while (0)

static void test_vectors(void)
{
	VECTOR(HEARTBEAT, heartbeat, 7,
	       ((struct mav_heartbeat){ .custom_mode = 19, .type = MAV_TYPE_SUBMARINE,
					.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA, .base_mode = 0x81,
					.system_status = MAV_STATE_ACTIVE, .mavlink_version = 3 }),
	       "fd09000007ffbe000000130000000c038104037203");
	VECTOR(MANUAL_CONTROL, manual_control, 8,
	       ((struct mav_manual_control){ .target = 1, .x = 100, .y = -200, .z = 500, .r = -1000 }),
	       "fd0b000008ffbe450000640038fff40118fc000001cd15");
	VECTOR(MANUAL_CONTROL, manual_control, 9,
	       ((struct mav_manual_control){ .target = 1, .x = 100, .y = -200, .z = 500, .r = -1000,
					     .enabled_extensions = 3, .s = 250, .t = -250 }),
	       "fd12000009ffbe450000640038fff40118fc000001000003fa0006ff89c8");
	VECTOR(COMMAND_LONG, command_long, 10,
	       ((struct mav_command_long){ .param1 = 1.0f, .command = MAV_CMD_COMPONENT_ARM_DISARM,
					   .target_system = 1, .target_component = 1 }),
	       "fd2000000affbe4c00000000803f00000000000000000000000000000000000000000000000090010101109c");
	VECTOR(STATUSTEXT, statustext, 11,
	       ((struct mav_statustext){ .severity = MAV_SEVERITY_CRITICAL, .text = "PreArm: Compass not calibrated" }),
	       "fd1f00000bffbefd00000250726541726d3a20436f6d70617373206e6f742063616c69627261746564ed20");
	VECTOR(PARAM_VALUE, param_value, 12,
	       ((struct mav_param_value){ .param_value = 255.0f, .param_count = 900, .param_index = 42,
					  .param_id = "SYSID_MYGCS", .param_type = MAV_PARAM_TYPE_INT16 }),
	       "fd1900000cffbe16000000007f4384032a0053595349445f4d594743530000000000049a94");
	VECTOR(COMMAND_ACK, command_ack, 13,
	       ((struct mav_command_ack){ .command = MAV_CMD_COMPONENT_ARM_DISARM, .result = MAV_RESULT_ACCEPTED }),
	       "fd0200000dffbe4d00009001476f");
	VECTOR(ATTITUDE, attitude, 14,
	       ((struct mav_attitude){ .time_boot_ms = 123456, .roll = 0.1f, .pitch = -0.2f, .yaw = 1.5f,
				       .rollspeed = 0.01f, .pitchspeed = 0.02f, .yawspeed = 0.03f }),
	       "fd1c00000effbe1e000040e20100cdcccc3dcdcc4cbe0000c03f0ad7233c0ad7a33c8fc2f53ca866");
}

static void test_decode_errors(void)
{
	uint8_t buf[MAV_MAX_FRAME];
	struct mav_frame f;
	const char *why;
	size_t n = unhex("fd0b000008ffbe450000640038fff40118fc000001cd15", buf, sizeof buf);
	/* A truncated datagram: nothing consumed. */
	CHECK_EQ_INT(mav_decode(buf, n - 1, &f, NULL, &why), 0);
	/* A flipped payload byte: bad CRC, the frame consumed and refused. */
	buf[12] ^= 1;
	CHECK_EQ_INT(mav_decode(buf, n, &f, NULL, &why), n);
	CHECK(!f.valid && strcmp(why, "bad CRC") == 0);
	buf[12] ^= 1;
	/* A message id this header does not know: refused. */
	buf[8] = 0x77;
	CHECK_EQ_INT(mav_decode(buf, n, &f, NULL, &why), n);
	CHECK(!f.valid && strcmp(why, "unknown message id") == 0);
	buf[8] = 0;
	/* Not a start byte: one byte skipped. */
	buf[0] = 0xfe;
	CHECK_EQ_INT(mav_decode(buf, n, &f, NULL, &why), 1);
	CHECK(!f.valid);
	buf[0] = 0xfd;
	/* Two frames in one datagram decode one after the other. */
	uint8_t two[2 * MAV_MAX_FRAME];
	memcpy(two, buf, n);
	memcpy(two + n, buf, n);
	size_t used = mav_decode(two, 2 * n, &f, NULL, &why);
	CHECK_EQ_INT(used, n);
	CHECK(f.valid);
	CHECK_EQ_INT(mav_decode(two + used, 2 * n - used, &f, NULL, &why), n);
	CHECK(f.valid);
	/* A payload entirely zero keeps its first byte. */
	uint8_t z[MAV_LEN_HEARTBEAT] = { 0 };
	size_t fl = mav_encode(buf, sizeof buf, 1, 1, 1, MAV_MSG_ID_HEARTBEAT, z, sizeof z, MAV_CRC_HEARTBEAT, NULL);
	CHECK_EQ_INT(fl, 13); /* header 10, one payload byte, CRC 2 */
	CHECK_EQ_INT(buf[1], 1);
	CHECK_EQ_INT(mav_decode(buf, fl, &f, NULL, &why), fl);
	CHECK(f.valid && f.len == 1);
	struct mav_heartbeat hb;
	mav_unpack_heartbeat(f.payload, f.len, &hb);
	CHECK(hb.custom_mode == 0 && hb.type == 0);
}

static void test_sha256_and_signing(void)
{
	uint8_t d[32];
	sha256_of("abc", 3, d);
	uint8_t want[32];
	unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
	CHECK_MEM_EQ(d, want, 32);
	/* The empty string. */
	sha256_of("", 0, d);
	unhex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
	CHECK_MEM_EQ(d, want, 32);
	/* A two-block message. */
	sha256_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
	unhex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", want, 32);
	CHECK_MEM_EQ(d, want, 32);

	/* pymavlink's signed HEARTBEAT: key 00..1f, link 1, timestamp 0x0102030405. */
	struct mav_signing s;
	memset(&s, 0, sizeof s);
	s.enabled = 1;
	for (int i = 0; i < 32; i++)
		s.key[i] = (uint8_t)i;
	s.link_id = 1;
	s.fake_now = 0x0102030405ull;
	struct mav_heartbeat hb = { .type = MAV_TYPE_GCS, .autopilot = MAV_AUTOPILOT_INVALID,
				    .system_status = MAV_STATE_ACTIVE, .mavlink_version = 3 };
	uint8_t payload[MAV_LEN_HEARTBEAT], frame[MAV_MAX_FRAME], ref[MAV_MAX_FRAME];
	mav_pack_heartbeat(&hb, payload);
	size_t fl = mav_encode(frame, sizeof frame, 9, 255, 190, MAV_MSG_ID_HEARTBEAT, payload, sizeof payload,
			       MAV_CRC_HEARTBEAT, &s);
	size_t rl = unhex("fd09010009ffbe0000000000000006080004030e6e01050403020100c0b4c62d4dde", ref, sizeof ref);
	CHECK_EQ_INT(fl, rl);
	CHECK_MEM_EQ(frame, ref, rl);
	CHECK_EQ_INT(s.last_sent, 0x0102030405ull);
	/* The next frame's timestamp is greater even when the clock stands. */
	mav_encode(frame, sizeof frame, 10, 255, 190, MAV_MSG_ID_HEARTBEAT, payload, sizeof payload, MAV_CRC_HEARTBEAT, &s);
	CHECK_EQ_INT(s.last_sent, 0x0102030406ull);

	/* The receiver: the same key verifies the reference frame. */
	struct mav_signing r;
	memset(&r, 0, sizeof r);
	r.enabled = 1;
	memcpy(r.key, s.key, 32);
	r.fake_now = 0x0102030405ull;
	struct mav_frame f;
	const char *why;
	CHECK_EQ_INT(mav_decode(ref, rl, &f, &r, &why), rl);
	CHECK(f.valid && f.is_signed && f.link_id == 1 && f.timestamp == 0x0102030405ull);
	/* The same frame again: its timestamp does not rise. */
	CHECK_EQ_INT(mav_decode(ref, rl, &f, &r, &why), rl);
	CHECK(!f.valid && strcmp(why, "timestamp not newer than the stream's last") == 0);
	/* The second frame (timestamp +1) passes; a flipped signature byte does not. */
	CHECK_EQ_INT(mav_decode(frame, fl, &f, &r, &why), fl);
	CHECK(f.valid);
	frame[fl - 1] ^= 1;
	CHECK_EQ_INT(mav_decode(frame, fl, &f, &r, &why), fl);
	CHECK(!f.valid && strcmp(why, "bad signature") == 0);
	frame[fl - 1] ^= 1;
	/* A wrong key: bad signature. An unsigned frame: refused on a signed link. */
	struct mav_signing wrong = r;
	wrong.key[0] ^= 0xff;
	mav_encode(frame, sizeof frame, 11, 255, 190, MAV_MSG_ID_HEARTBEAT, payload, sizeof payload, MAV_CRC_HEARTBEAT, &s);
	CHECK_EQ_INT(mav_decode(frame, fl, &f, &wrong, &why), fl);
	CHECK(!f.valid && strcmp(why, "bad signature") == 0);
	uint8_t plain[MAV_MAX_FRAME];
	size_t pl = mav_encode(plain, sizeof plain, 12, 255, 190, MAV_MSG_ID_HEARTBEAT, payload, sizeof payload,
			       MAV_CRC_HEARTBEAT, NULL);
	CHECK_EQ_INT(mav_decode(plain, pl, &f, &r, &why), pl);
	CHECK(!f.valid && strcmp(why, "unsigned frame on a signed link") == 0);
	/* Without signing on the receiver, both the signed and the plain frame pass. */
	CHECK_EQ_INT(mav_decode(plain, pl, &f, NULL, &why), pl);
	CHECK(f.valid);
	CHECK_EQ_INT(mav_decode(ref, rl, &f, NULL, &why), rl);
	CHECK(f.valid);
	/* A frame more than a minute behind the local clock, on a new stream: refused. */
	struct mav_signing late;
	memset(&late, 0, sizeof late);
	late.enabled = 1;
	memcpy(late.key, s.key, 32);
	late.fake_now = 0x0102030405ull + MAV_TIMESTAMP_SLACK + 1;
	CHECK_EQ_INT(mav_decode(ref, rl, &f, &late, &why), rl);
	CHECK(!f.valid && strcmp(why, "timestamp more than a minute behind") == 0);
	CHECK_EQ_INT(late.rejected, 1);
	/* One a little behind is accepted; the clock then follows the sender. */
	late.fake_now = 0x0102030405ull + MAV_TIMESTAMP_SLACK;
	CHECK_EQ_INT(mav_decode(ref, rl, &f, &late, &why), rl);
	CHECK(f.valid);
	/* The real clock: units since 2015 are far past zero and rising. */
	struct mav_signing live;
	memset(&live, 0, sizeof live);
	uint64_t t0 = mav_timestamp_now(&live);
	CHECK(t0 > 300ull * 365 * 24 * 3600 * 100000 / 100); /* more than three years' worth */
	CHECK(mav_timestamp_now(&live) >= t0);
}

static void test_key_file(void)
{
	char path[] = "/tmp/hangar-mavkey-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	const char hex[] = "000102030405060708090a0b0c0d0e0f 101112131415161718191a1b1c1d1e1f\n";
	CHECK_EQ_INT(write(fd, hex, sizeof hex - 1), (int)sizeof hex - 1);
	close(fd);
	struct mav_signing s;
	memset(&s, 0, sizeof s);
	char err[128];
	CHECK_EQ_INT(mav_signing_load_key(&s, path, err, sizeof err), 0);
	for (int i = 0; i < 32; i++)
		CHECK_EQ_INT(s.key[i], i);
	/* Raw 32 bytes. */
	fd = open(path, O_WRONLY | O_TRUNC);
	uint8_t raw[32];
	for (int i = 0; i < 32; i++)
		raw[i] = (uint8_t)(31 - i);
	CHECK_EQ_INT(write(fd, raw, 32), 32);
	close(fd);
	CHECK_EQ_INT(mav_signing_load_key(&s, path, err, sizeof err), 0);
	CHECK_EQ_INT(s.key[0], 31);
	/* Too short: refused with the path in the reason. */
	fd = open(path, O_WRONLY | O_TRUNC);
	CHECK_EQ_INT(write(fd, "abc", 3), 3);
	close(fd);
	CHECK_EQ_INT(mav_signing_load_key(&s, path, err, sizeof err), -1);
	CHECK(strstr(err, path) != NULL);
	unlink(path);
	CHECK_EQ_INT(mav_signing_load_key(&s, path, err, sizeof err), -1);
}

int main(void)
{
	RUN_TEST(test_crc);
	RUN_TEST(test_vectors);
	RUN_TEST(test_decode_errors);
	RUN_TEST(test_sha256_and_signing);
	RUN_TEST(test_key_file);
	TEST_MAIN_END();
}
