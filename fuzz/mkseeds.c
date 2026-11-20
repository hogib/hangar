// SPDX-License-Identifier: GPL-3.0-or-later
/* Writes the seeds that are not plain copies of files in the tree: the binary
 * ones (a recording, a topic file, datagrams, MAVLink frames), made with the
 * code that writes the real thing, and the composed ones (several parts in
 * one input). `make fuzz-seeds` runs it from the repository root; its output,
 * fuzz/corpus/<harness>/seed-*, is checked in, so nothing regenerates it
 * during a build. The text seeds beside them are copies of msg/, profiles/
 * and modules/ files, made by hand. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bridge_conf.h"
#include "depth.h"
#include "fuzz_bridge.h"
#include "fuzz_mavlink.h"
#include "h264.h"
#include "wrench.h"
#include "hg_record.h"
#include "hg_topic.h"
#include "hg_type.h"

/* fuzz_mavlink.h's link is the modules'; only its framing and key are used here. */
int mav_link_receive(struct mav_link *l, void (*fn)(void *ctx, const struct mav_frame *f), void *ctx)
{
	(void)l, (void)fn, (void)ctx;
	return 0;
}

int mav_link_service(struct mav_link *l, uint64_t now_ns)
{
	(void)l, (void)now_ns;
	return 0;
}

struct buf {
	unsigned char *p;
	size_t n, cap;
};

static void add(struct buf *b, const void *d, size_t n)
{
	if (b->n + n > b->cap) {
		b->cap = (b->n + n) * 2 + 4096;
		b->p = realloc(b->p, b->cap);
		if (!b->p)
			abort();
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void adds(struct buf *b, const char *s)
{
	add(b, s, strlen(s));
}

static void add_file(struct buf *b, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "mkseeds: %s: %s\n", path, strerror(errno));
		exit(1);
	}
	unsigned char tmp[4096];
	size_t k;
	while ((k = fread(tmp, 1, sizeof tmp, f)) > 0)
		add(b, tmp, k);
	fclose(f);
}

/* Set for seeds whose bytes hold the time and the pid of their making (a topic
 * file's header): one that is there already is left as it is, so that a run
 * changes nothing that is checked in. Remove the file to have it made again. */
static int keep_existing;

static void put(const char *harness, const char *name, struct buf *b)
{
	char path[512];
	snprintf(path, sizeof path, "fuzz/corpus/%s", harness);
	mkdir(path, 0777);
	snprintf(path, sizeof path, "fuzz/corpus/%s/seed-%s", harness, name);
	if (keep_existing && access(path, F_OK) == 0) {
		b->n = 0;
		return;
	}
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(b->p, 1, b->n, f) != b->n || fclose(f) != 0) {
		fprintf(stderr, "mkseeds: %s: %s\n", path, strerror(errno));
		exit(1);
	}
	b->n = 0;
}

static char tmpdir[256];

/* ---- wiring, params, manifest: text ---------------------------------------- */

static const char WIRING[] =
	"hangar-wiring 1\n"
	"module depth_hold\n"
	"role vehicle\n"
	"# a comment line\n"
	"sub nav.depth type=depth hash=9f3a61c20b7e4d15 rate=50\n"
	"sub nav.imu type=imu hash=0c41e2aa93f81d70 rate=50\n"
	"sub hangar.vehicle.admin.req type=req:hangar hash=0000000000000001 slots=16 implicit\n"
	"subglob *.req implicit\n"
	"subglob nav.*\n"
	"sub safety.rep type=rep:safety hash=0000000000000002 slots=16 implicit\n"
	"pub ctl.depth_hold type=wrench hash=77d0b3a1c5e29f04 slots=16 rate=50 stale_after=60ms inputs=nav.depth,nav.imu\n"
	"pub depth_hold.req type=req:depth_hold hash=0000000000000003 slots=16 implicit\n"
	"pub depth_hold.rep type=rep:depth_hold hash=0000000000000004 slots=16 implicit sources=*.req,hangar.vehicle.admin.req\n"
	"pub depth_hold.state type=state hash=0000000000000005 rate=0.5 inputs=vehicle:cam.pilot,ctl.depth_hold\n"
	"serve reset req=empty hash=000000000000000a size=8 rep=empty hash=000000000000000b size=8 deadline=100ms allow=topside:pilot_console,admin\n"
	"call safety.disarm req=empty hash=000000000000000c size=8 rep=empty hash=000000000000000d size=16 remote=0\n"
	"call topside.ping req=ping hash=000000000000000e size=24 rep=pong hash=000000000000000f size=32 remote=1\n"
	"mirror cam.* type=any rate=30 stale_after=100ms\n"
	"mirror topside.ping.rep type=any\n"
	"actuator command=ctl.wrench command_timeout=200ms safety_timeout=300ms\n";

static void text_seeds(void)
{
	struct buf b = { 0 };
	adds(&b, WIRING);
	put("wiring", "every-line", &b);
	adds(&b, "2.5ms\nnav.*\nnav.depth\n");
	put("wiring", "values", &b);

	/* params: <schema> NUL <file> */
	add_file(&b, "msg/depth_hold_params.msg");
	add(&b, "", 1);
	adds(&b, "# hangar parameters: depth_hold_params\n# version 3, set by admin:pilot, 2026-11-09T00:00:00Z\n"
		 "kp = 2.5\ngains = 1, 2, 3\nframe = \"ned\"\nenabled = 1\nrate_hz = 100\n");
	put("params", "depth_hold", &b);
	add_file(&b, "msg/safety_params.msg");
	add(&b, "", 1);
	add_file(&b, "profiles/rov-vehicle/safety.params");
	put("params", "safety", &b);
	add_file(&b, "msg/depth_control_params.msg");
	add(&b, "", 1);
	add_file(&b, "profiles/sim-depth-ff/depth_hold.params");
	put("params", "depth_control", &b);

	/* manifest: sets of several modules, under the names the checks know. */
	static const char *const vehicle[] = { "safety", "mux", "mavlink", "recorder" };
	for (size_t i = 0; i < 4; i++) {
		char path[256], line[64];
		snprintf(line, sizeof line, "#name %s\n", vehicle[i]);
		snprintf(path, sizeof path, "profiles/rov-vehicle/%s.module", vehicle[i]);
		adds(&b, line);
		add_file(&b, path);
		if (i < 3)
			add(&b, "", 1);
	}
	put("manifest", "set-rov-vehicle", &b);
	adds(&b, "#name bridge\n");
	add_file(&b, "profiles/rov-vehicle/bridge.module.off");
	add(&b, "", 1);
	adds(&b, "#name console\n");
	add_file(&b, "profiles/rov-vehicle/console.module");
	put("manifest", "set-bridge", &b);
	adds(&b, "t#name topside_mavlink\n");
	add_file(&b, "profiles/rov-topside/topside_mavlink.module");
	add(&b, "", 1);
	adds(&b, "#name joystick\n");
	add_file(&b, "profiles/rov-topside/joystick.module.off");
	put("manifest", "set-topside", &b);
	static const char *const sim[] = { "policy_depth_ff", "pairer", "depth_hold", "nav" };
	for (size_t i = 0; i < 4; i++) {
		char path[256], line[64];
		snprintf(line, sizeof line, "#name %s\n", sim[i]);
		snprintf(path, sizeof path, "profiles/sim-depth-ff/%s.module", sim[i]);
		adds(&b, line);
		add_file(&b, path);
		if (i < 3)
			add(&b, "", 1);
	}
	put("manifest", "set-sim-depth-ff", &b);
	free(b.p);
}

/* ---- recording: <spec> NUL <.hgr> ------------------------------------------- */

static void recording_file(struct buf *b, int meta)
{
	char path[300];
	snprintf(path, sizeof path, "%s/r.hgr", tmpdir);
	unlink(path);
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	snprintf(m.name, sizeof m.name, "nav.depth");
	snprintf(m.writer, sizeof m.writer, "mavlink");
	m.type_hash = DEPTH_TYPE.hash;
	m.payload_bytes = DEPTH_TYPE.payload_bytes;
	m.slot_count = 16;
	m.rate_mhz = 50000;
	m.stale_after_ns = 60000000;
	m.topic_id = hg_topic_id(m.name);
	struct hgr_header h;
	hgr_header_init(&h, &m, DEPTH_TYPE.schema, DEPTH_TYPE.schema_bytes, meta ? HGR_META : HGR_FULL, 1,
			meta ? "meta" : "full", "20260101-000000-seed", 0);
	/* The two times the header takes from the clock, fixed: the same bytes
	 * on every run. */
	h.created_real_ns = 1767225600000000000ull;
	h.created_mono_ns = 1000000000ull;
	struct hgr_writer w;
	if (hgr_create(&w, path, &h) < 0)
		abort();
	for (int i = 1; i <= 6; i++) {
		struct depth d = { .depth_m = (float)i, .temp_c = 12.5f, .pressure_mbar = 1013.0f + 100.0f * (float)i };
		struct hg_info info = { .seq = (uint64_t)i, .stamp_ns = (uint64_t)i * 20000000,
					.publish_ns = (uint64_t)i * 20000000 + 1000, .n_inputs = 1 };
		info.inputs[0].topic_id = m.topic_id;
		info.inputs[0].seq = (uint64_t)i;
		struct hgr_record r;
		hgr_record_init(&r, &info, i == 3 ? 2 : 0, 0);
		if (hgr_append(&w, &r, meta ? NULL : &d) < 0)
			abort();
	}
	hgr_close(&w);
	add_file(b, path);
	unlink(path);
}

static void recording_seeds(void)
{
	struct buf b = { 0 };
	adds(&b, "[Dataset]\nAnchor=nav.depth\nAnchorRole=input\nFields=depth_m temp_c\nFilter=mask\nResetAfter=200ms\n"
		 "Where=nav.depth.depth_m > 0.5 and (later.pressure_mbar < 2000 or nav.depth.temp_c != 0)\n\n"
		 "[Input later]\nTopic=nav.depth\nJoin=asof:20ms\nRole=label\nFields=pressure_mbar\n\n"
		 "[Input window]\nTopic=nav.depth\nJoin=asof:0\nWindow=3\n\n"
		 "[Input prov]\nTopic=nav.depth\nJoin=provenance\n");
	add(&b, "", 1);
	recording_file(&b, 0);
	put("recording", "depth-full", &b);
	adds(&b, "[Dataset]\nAnchor=nav.depth\nRate=every:2\nMissing=drop\nLateness=100ms\n");
	add(&b, "", 1);
	recording_file(&b, 0);
	put("recording", "depth-every2", &b);
	adds(&b, "[Dataset]\nAnchor=nav.depth\nRate=rate:12.5\n");
	add(&b, "", 1);
	recording_file(&b, 1);
	put("recording", "depth-meta", &b);
	free(b.p);
}

/* ---- topic: a topic file as the library writes it ------------------------------ */

static void topic_seeds(void)
{
	struct buf b = { 0 };
	char path[300];
	keep_existing = 1;
	setenv("HANGAR_TOPIC_DIR", tmpdir, 1);
	unsetenv("HANGAR_CONTROL");
	hg_pub *p = hg_pub_open("nav.depth", &DEPTH_TYPE, &(struct hg_pub_opts){ .slots = 4, .rate_mhz = 50000 });
	if (!p)
		abort();
	snprintf(path, sizeof path, "%s/nav.depth", tmpdir);
	for (int i = 1; i <= 3; i++) {
		struct depth d = { .depth_m = (float)i, .temp_c = 12.5f, .pressure_mbar = 1013.0f };
		hg_publish(p, &d, sizeof d, (uint64_t)i * 20000000);
	}
	add_file(&b, path);
	put("topic", "depth-3-of-4", &b);
	for (int i = 4; i <= 9; i++) { /* the ring of 4 has wrapped */
		struct depth d = { .depth_m = (float)i, .temp_c = 12.5f, .pressure_mbar = 1013.0f };
		hg_publish(p, &d, sizeof d, (uint64_t)i * 20000000);
	}
	add_file(&b, path);
	put("topic", "depth-wrapped", &b);
	hg_pub_close(p);
	add_file(&b, path);
	put("topic", "depth-closed", &b);
	static const char *const left[] = { "nav.depth", ".nav.depth.wake", ".dir" };
	for (size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof path, "%s/%s", tmpdir, left[i]);
		unlink(path);
	}
	free(b.p);
	keep_existing = 0;
}

/* ---- bridge: a script of datagrams (fuzz_bridge.h) ------------------------------ */

static void le32(struct buf *b, uint32_t v)
{
	add(b, &v, 4);
}

static void le64(struct buf *b, uint64_t v)
{
	add(b, &v, 8);
}

static void op(struct buf *out, int code, const struct buf *body)
{
	unsigned char h[3] = { (unsigned char)code, (unsigned char)(body->n & 0xff), (unsigned char)(body->n >> 8) };
	add(out, h, 3);
	add(out, body->p, body->n);
}

/* A record of the reliable stream: u32 length (type and body), u8 type, body. */
static void record(struct buf *out, int type, const struct buf *body)
{
	le32(out, (uint32_t)(1 + body->n));
	unsigned char t = (unsigned char)type;
	add(out, &t, 1);
	add(out, body->p, body->n);
}

static void name8(struct buf *b, const char *name)
{
	unsigned char l = (unsigned char)strlen(name);
	add(b, &l, 1);
	add(b, name, l);
}

/* A message as build_msg writes it. */
static void message(struct buf *b, const char *topic, uint64_t seq, const void *payload, uint32_t full, uint32_t used)
{
	name8(b, topic);
	le64(b, seq);
	le32(b, 0);
	le64(b, 1000000000ull + seq * 20000000);
	le64(b, 1000000000ull + seq * 20000000 + 1000);
	unsigned char nref = 1;
	add(b, &nref, 1);
	le32(b, hg_topic_id("nav.imu"));
	le32(b, 0);
	le64(b, seq);
	le32(b, full);
	le32(b, used);
	add(b, payload, used);
}

static void bridge_seeds(void)
{
	struct buf s = { 0 }, body = { 0 }, stream = { 0 };
	struct bridge_conf c;
	char err[200];
	if (bc_parse(FZB_CONF, strlen(FZB_CONF), &c, err, sizeof err) < 0)
		abort();
	struct depth d = { .depth_m = 2.5f, .temp_c = 12.5f, .pressure_mbar = 1260.0f };
	unsigned char mode;

	/* A link that is up, on the topside: the info record, a schema, messages
	 * on the stream and as datagrams, a ping, a pong, an ack, a tick. */
	mode = FZB_KEYED | FZB_CLOCK;
	add(&s, &mode, 1);
	char info[96];
	memset(info, 0, sizeof info);
	snprintf(info, 32, "dev");
	memcpy(info + 64, c.hash, 32);
	add(&body, info, sizeof info);
	record(&stream, 1, &body);
	body.n = 0;
	name8(&body, "nav.depth");
	name8(&body, "mavlink");
	le32(&body, 16);
	le32(&body, DEPTH_TYPE.schema_bytes);
	add(&body, DEPTH_TYPE.schema, DEPTH_TYPE.schema_bytes);
	record(&stream, 2, &body);
	body.n = 0;
	message(&body, "nav.depth", 7, &d, DEPTH_TYPE.payload_bytes, 12);
	record(&stream, 3, &body);
	op(&s, FZB_STREAM, &stream);
	body.n = 0;
	unsigned char kp[2] = { 7 /* K_MSG */, 3 };
	add(&body, kp, 2);
	message(&body, "nav.depth", 8, &d, DEPTH_TYPE.payload_bytes, 12);
	op(&s, FZB_SEALED, &body);
	body.n = 0;
	kp[0] = 3; /* K_PING */
	add(&body, kp, 2);
	le64(&body, 123456789);
	op(&s, FZB_SEALED, &body);
	body.n = 0;
	le64(&body, 5000000);
	le64(&body, 5000100);
	op(&s, FZB_PONG, &body);
	body.n = 0;
	kp[0] = 6; /* K_ACK */
	add(&body, kp, 2);
	le64(&body, 101);
	op(&s, FZB_SEALED, &body);
	body.n = 0;
	op(&s, FZB_TICK, &body);
	put("bridge", "link-up", &s);

	/* The same stream cut in two datagrams inside a record, and a record
	 * larger than any this side takes, passed over. */
	mode = FZB_KEYED | FZB_CLOCK | FZB_VEHICLE;
	add(&s, &mode, 1);
	struct buf part = { 0 };
	add(&part, stream.p, 150);
	op(&s, FZB_STREAM, &part);
	part.n = 0;
	add(&part, stream.p + 150, stream.n - 150);
	le32(&part, (1u << 20) + 5000);
	add(&part, "\x63", 1);
	op(&s, FZB_STREAM, &part);
	part.n = 0;
	for (int i = 0; i < 100; i++)
		le64(&part, 0);
	op(&s, FZB_STREAM, &part);
	put("bridge", "stream-split", &s);

	/* A call's request and reply, whose times are translated. */
	mode = FZB_KEYED | FZB_CLOCK | FZB_MIRROR_OK;
	add(&s, &mode, 1);
	stream.n = 0;
	/* A request topic's layout: the envelope of hg_call.h (112 bytes), then a body. */
	static const char req_schema[] = "u64 call_id\nu64 deadline_ns\nu64 request_type\nu32 target_id\nu32 reserved\n"
					 "char method[32]\nchar caller[48]\nu8 body[16]\n";
	body.n = 0;
	name8(&body, "safety.req");
	name8(&body, "console");
	le32(&body, 16);
	le32(&body, sizeof req_schema - 1);
	add(&body, req_schema, sizeof req_schema - 1);
	record(&stream, 2, &body);
	body.n = 0;
	unsigned char env[128];
	memset(env, 0, sizeof env);
	env[0] = 1;
	env[8] = 0x80;
	env[12] = 0x77;
	memcpy(env + 32, "arm", 3);
	message(&body, "safety.req", 1, env, 128, 40);
	record(&stream, 3, &body);
	op(&s, FZB_STREAM, &stream);
	put("bridge", "call", &s);

	/* A handshake from nothing: a first hello, a hello that echoes our
	 * nonce, a confirm under the pending keys, then a ping on the new link. */
	mode = FZB_OFFERED | FZB_VEHICLE;
	add(&s, &mode, 1);
	unsigned char hello[1 + 92];
	memset(hello, 0, sizeof hello);
	hello[0] = 1 | 4;
	memset(hello + 1 + 40, 0xa1, 16);
	memset(hello + 1 + 56, 0xb2, 16);
	body.n = 0;
	add(&body, hello, sizeof hello);
	op(&s, FZB_HELLO, &body);
	hello[0] = 1 | 2 | 4;
	body.n = 0;
	add(&body, hello, sizeof hello);
	op(&s, FZB_HELLO, &body);
	body.n = 0;
	kp[0] = 2; /* K_CONFIRM */
	add(&body, kp, 2);
	for (int i = 0; i < 4; i++)
		le64(&body, 0);
	op(&s, FZB_PENDING, &body);
	body.n = 0;
	kp[0] = 3;
	add(&body, kp, 2);
	le64(&body, 42);
	op(&s, FZB_SEALED, &body);
	put("bridge", "handshake", &s);

	/* Raw datagrams: what anyone on the network can send. */
	mode = FZB_OFFERED | FZB_VEHICLE;
	add(&s, &mode, 1);
	unsigned char raw[24 + 92 + 16];
	memset(raw, 0, sizeof raw);
	memcpy(raw, "HGB1", 4);
	raw[4] = 1;
	raw[12] = 92;
	raw[16] = 1;
	body.n = 0;
	add(&body, raw, sizeof raw);
	op(&s, FZB_RAW, &body);
	raw[4] = 5;
	body.n = 0;
	add(&body, raw, sizeof raw);
	op(&s, FZB_RAW, &body);
	body.n = 0;
	adds(&body, "HANGAR/1 role=topside pair=fuzz port=5614 id=0badcafe");
	op(&s, FZB_BEACON_TAG, &body);
	body.n = 0;
	adds(&body, "HANGAR/1 role=topside pair=fuzz port=5614 id=0badcafe tag=00112233445566778899aabbccddeeff");
	op(&s, FZB_BEACON, &body);
	body.n = 0;
	op(&s, FZB_TICK, &body);
	put("bridge", "raw", &s);
	free(s.p);
	free(body.p);
	free(stream.p);
	free(part.p);
}

/* ---- mavlink: a mode byte, then frames as mav_encode writes them ------------------- */

static uint8_t mav_seq;

static void frame(struct buf *b, uint32_t msgid, const uint8_t *payload, struct mav_signing *sign)
{
	const struct mav_msg_info *mi = mav_msg_info(msgid);
	uint8_t out[MAV_MAX_FRAME];
	size_t n = mav_encode(out, sizeof out, mav_seq++, 1, 1, msgid, payload, mi->len, mi->crc_extra, sign);
	add(b, out, n);
}

/* Every message the modules know, each with a payload of no meaning. */
static void all_messages(struct buf *b, struct mav_signing *sign)
{
	for (size_t i = 0; i < MAV_MSGS_COUNT; i++) {
		uint8_t pl[MAV_MAX_PAYLOAD];
		for (size_t k = 0; k < sizeof pl; k++)
			pl[k] = (uint8_t)(k * 7 + i + 1);
		frame(b, MAV_MSGS[i].id, pl, sign);
	}
}

/* What ArduSub sends while it is asked to arm and refuses, with a parameter. */
static void vehicle_story(struct buf *b, struct mav_signing *sign)
{
	uint8_t pl[MAV_MAX_PAYLOAD];
	struct mav_heartbeat h = { .type = MAV_TYPE_SUBMARINE, .custom_mode = 19, .system_status = 3 };
	mav_pack_heartbeat(&h, pl);
	frame(b, MAV_MSG_ID_HEARTBEAT, pl, sign);
	struct mav_statustext st = { .severity = 2 };
	memcpy(st.text, "PreArm: Battery 1 low voltage failsafe", 38);
	mav_pack_statustext(&st, pl);
	frame(b, MAV_MSG_ID_STATUSTEXT, pl, sign);
	struct mav_command_ack a = { .command = MAV_CMD_COMPONENT_ARM_DISARM, .result = 4 };
	mav_pack_command_ack(&a, pl);
	frame(b, MAV_MSG_ID_COMMAND_ACK, pl, sign);
	struct mav_param_value p = { .param_value = 1.0f, .param_count = 3, .param_index = 0, .param_type = 9 };
	memcpy(p.param_id, "SYSID_MYGCS", 11);
	mav_pack_param_value(&p, pl);
	frame(b, MAV_MSG_ID_PARAM_VALUE, pl, sign);
	struct mav_attitude at = { .time_boot_ms = 5000, .roll = 0.1f, .pitch = -0.2f, .yaw = 1.5f };
	mav_pack_attitude(&at, pl);
	frame(b, MAV_MSG_ID_ATTITUDE, pl, sign);
	h.base_mode = MAV_MODE_FLAG_SAFETY_ARMED;
	mav_pack_heartbeat(&h, pl);
	frame(b, MAV_MSG_ID_HEARTBEAT, pl, sign);
}

/* What QGroundControl sends: sticks and buttons, an arm, a mode, parameters. */
static void topside_story(struct buf *b, struct mav_signing *sign)
{
	uint8_t pl[MAV_MAX_PAYLOAD];
	struct mav_manual_control m = { .x = 500, .y = -500, .z = 500, .r = 100, .buttons = 2, .target = 1 };
	mav_pack_manual_control(&m, pl);
	frame(b, MAV_MSG_ID_MANUAL_CONTROL, pl, sign);
	m.buttons = 4 | 8;
	m.enabled_extensions = 3;
	mav_pack_manual_control(&m, pl);
	frame(b, MAV_MSG_ID_MANUAL_CONTROL, pl, sign);
	struct mav_command_long c = { .command = MAV_CMD_COMPONENT_ARM_DISARM, .param1 = 1.0f, .target_system = 1 };
	mav_pack_command_long(&c, pl);
	frame(b, MAV_MSG_ID_COMMAND_LONG, pl, sign);
	c.command = MAV_CMD_DO_SET_MODE;
	mav_pack_command_long(&c, pl);
	frame(b, MAV_MSG_ID_COMMAND_LONG, pl, sign);
	memset(pl, 0, sizeof pl);
	frame(b, MAV_MSG_ID_PARAM_REQUEST_LIST, pl, sign);
	frame(b, MAV_MSG_ID_SET_MODE, pl, sign);
	frame(b, MAV_MSG_ID_MISSION_REQUEST_LIST, pl, sign);
	frame(b, MAV_MSG_ID_PARAM_SET, pl, sign);
}

static void mavlink_seeds(void)
{
	struct buf b = { 0 };
	struct mav_signing key;
	unsigned char mode;
	static const char *const who[] = { "mavlink_vehicle", "mavlink_topside" };
	for (int t = 0; t < 2; t++) {
		void (*story)(struct buf *, struct mav_signing *) = t ? topside_story : vehicle_story;
		mode = t ? 0 : FZM_ANY_ID;
		add(&b, &mode, 1);
		story(&b, NULL);
		put(who[t], "udp", &b);
		mode = FZM_ANY_ID | FZM_REPAIR;
		add(&b, &mode, 1);
		all_messages(&b, NULL);
		put(who[t], "udp-all-messages", &b);
		mode = FZM_SIGNING | FZM_REPAIR;
		add(&b, &mode, 1);
		fzm_key(&key);
		story(&b, &key);
		all_messages(&b, &key);
		put(who[t], "udp-signed", &b);
		mode = FZM_SERIAL | FZM_ANY_ID | 0x10; /* a serial line read 23 bytes at a time, with noise between frames */
		add(&b, &mode, 1);
		adds(&b, "noise\xfd\x03");
		story(&b, NULL);
		adds(&b, "\xfd\xfd\xfd");
		all_messages(&b, NULL);
		put(who[t], "serial-pieces", &b);
		mode = FZM_SERIAL | FZM_SIGNING | FZM_REPAIR | 0x20;
		add(&b, &mode, 1);
		fzm_key(&key);
		story(&b, &key);
		put(who[t], "serial-signed", &b);
	}
	free(b.p);
}

/* ---- video: the test clip as a camera gives it, and as RTP ---------------------- */

static void rtp_collect(void *ctx, const uint8_t *pkt, size_t len)
{
	struct buf *b = ctx;
	unsigned char h[2] = { (unsigned char)(len & 0xff), (unsigned char)(len >> 8) };
	add(b, h, 2);
	add(b, pkt, len);
}

static void video_seeds(void)
{
	struct buf clip = { 0 }, s = { 0 };
	add_file(&clip, "modules/video_relay/tests/testsrc-320x240-60.h264");
	/* The first frames are enough: an IDR with its SPS and PPS, then P frames. */
	size_t keep = 0;
	for (int i = 0; i < 6 && keep < clip.n; i++)
		keep += h264_au_len(clip.p + keep, clip.n - keep);
	unsigned char head[2] = { 1, 200 }; /* camera bytes, 1200-byte packets */
	add(&s, head, 2);
	add(&s, clip.p, keep);
	put("video", "camera", &s);
	head[1] = 10; /* 60-byte packets: everything is FU-A fragments */
	add(&s, head, 2);
	add(&s, clip.p, keep);
	put("video", "camera-small-packets", &s);
	for (int small = 0; small < 2; small++) {
		struct rtp_h264 r;
		rtp_h264_init(&r, 0x48414e47, 96, small ? 100 : 1200);
		unsigned char mode = 0;
		add(&s, &mode, 1);
		size_t off = 0;
		uint32_t ts = 0;
		while (off < keep) {
			size_t n = h264_au_len(clip.p + off, keep - off);
			if (!n)
				break;
			rtp_h264_send_au(&r, clip.p + off, n, ts, rtp_collect, &s, NULL);
			ts += 3000;
			off += n;
		}
		put("video", small ? "rtp-fragments" : "rtp", &s);
	}
	free(clip.p);
	free(s.p);
}

/* ---- conf: a byte choosing the reader (fuzz_conf.c), then the file ------------------- */

static void conf_file(int which, const char *name, const char *path, const char *text)
{
	struct buf b = { 0 };
	unsigned char w = (unsigned char)which;
	add(&b, &w, 1);
	if (path)
		add_file(&b, path);
	else
		adds(&b, text);
	put("conf", name, &b);
	free(b.p);
}

static void conf_seeds(void)
{
	conf_file(0, "hangard-vehicle", "profiles/rov-vehicle/hangard.conf", NULL);
	conf_file(0, "hangard-topside", "profiles/rov-topside/hangard.conf", NULL);
	conf_file(0, "hangard-every-key", NULL,
		  "# hangard.conf\nRole=vehicle\nPrivilegedModule=safety\nModuleDir=/etc/hangar/modules\n"
		  "MsgDir=/usr/share/hangar/msg\nTopicDir=/dev/shm/hangar\nBridgeConf=/etc/hangar/bridge.conf\n"
		  "DatasetDir=/etc/hangar/datasets\nRecordingDir=/var/lib/hangar/recordings\nHealthTick=100ms\n"
		  "MaxModules=64\nLogFallbackDir=/run/hangar/log\nLogFallbackCap=256KB\n");
	conf_file(1, "registry", NULL,
		  "hangar-registry 2\nsystem: degraded 123456789 mux: gave up after 3 failures\n"
		  "safety 1234 5678 ready 0 0.000 1a2b3c4d 5e6f7a8b 2.500 -\n"
		  "mux 1240 5700 backoff 3 4.000 1a2b3c4d 5e6f7a8b 2.000 -\n"
		  "mavlink 1250 5800 stopping 0 0.000 0 0 5.000 stopped\n"
		  "old 99 1 failed 9 0.5\n");
	conf_file(2, "mavlink-udp", "profiles/rov-vehicle/mavlink.conf", NULL);
	conf_file(2, "mavlink-serial", "profiles/rov-vehicle/mavlink.conf.serial", NULL);
	conf_file(2, "mavlink-example", "modules/mavlink/mavlink.conf", NULL);
	conf_file(3, "mavlink-topside", "profiles/rov-topside/topside_mavlink.conf", NULL);
	conf_file(3, "mavlink-topside-example", "modules/mavlink/topside_mavlink.conf", NULL);
	conf_file(4, "ardusub-params", "modules/mavlink/ardusub.params", NULL);
	conf_file(5, "recorder", "profiles/rov-vehicle/recorder.conf", NULL);
	conf_file(5, "recorder-example", "modules/recorder/recorder.conf", NULL);
	conf_file(6, "joystick", "profiles/rov-topside/joystick.conf", NULL);
	conf_file(6, "joystick-example", "modules/joystick/joystick.conf", NULL);
	conf_file(7, "dump", "profiles/rov-vehicle/ardusub.params", NULL);
	conf_file(7, "dump-typed", NULL, "# NAME VALUE TYPE\nSURFACE_DEPTH -10 9\nSYSID_MYGCS 255 4\nFS_GCS_ENABLE 0 2  # off\n");
	conf_file(7, "dump-mavros-yaml", NULL,
		  "/mavros/param:\n  ros__parameters:\n    use_sim_time: false\n    SURFACE_DEPTH: -10.0\n    FS_GCS_ENABLE: 0\n");
	conf_file(8, "backup", NULL, "# hangar parameter backup\nSURFACE_DEPTH -10 9\nFS_GCS_ENABLE 0 2\nJS_GAIN_DEFAULT 0.5 9\n");
}

/* ---- control: a mode byte (fuzz_control.c), then requests ------------------------ */

static void request(struct buf *b, const char *line, const void *body, size_t body_len)
{
	size_t n = strlen(line) + body_len;
	unsigned char h[2] = { (unsigned char)(n & 0xff), (unsigned char)(n >> 8) };
	add(b, h, 2);
	adds(b, line);
	if (body_len)
		add(b, body, body_len);
}

static void control_seeds(void)
{
	struct buf b = { 0 };
	char line[400];
	unsigned char mode;

	/* An operator in hangar-admin: everything a `hangar` command asks. */
	mode = 1 | 2;
	add(&b, &mode, 1);
	request(&b, "status\n", NULL, 0);
	request(&b, "paths", NULL, 0);
	request(&b, "wiring safety", NULL, 0);
	request(&b, "method safety.arm", NULL, 0);
	request(&b, "method mux.set_params", NULL, 0);
	request(&b, "start mux", NULL, 0);
	request(&b, "restart mux\n", NULL, 0);
	request(&b, "stop mux", NULL, 0);
	request(&b, "reload", NULL, 0);
	request(&b, "clear-failsafe", NULL, 0);
	put("control", "admin", &b);

	mode = 1;
	add(&b, &mode, 1);
	unsigned char body[16] = { 0 };
	request(&b, "call safety.arm deadline=500 body=8\n", body, 8);
	request(&b, "call depth_hold.set deadline=250 type=0 body=8\n", body, 8);
	request(&b, "call nosuch.method body=0\n", NULL, 0);
	request(&b, "param-read safety", NULL, 0);
	request(&b, "param-write safety\nleak_required = 0\nrequired = \"mux\"\n", NULL, 0);
	request(&b, "param-read safety", NULL, 0);
	request(&b, "param-write depth_hold\nnot_a_field = 1\n", NULL, 0);
	request(&b, "param-write", NULL, 0);
	put("control", "admin-call-params", &b);

	/* A module: its page, its outputs, a restart of itself. */
	mode = 2 | (3 << 2); /* mux */
	add(&b, &mode, 1);
	request(&b, "page", NULL, 0);
	snprintf(line, sizeof line, "open ctl.wrench hash=%016llx payload=%u schema=%u\n", (unsigned long long)WRENCH_TYPE.hash,
		 WRENCH_TYPE.payload_bytes, WRENCH_TYPE.schema_bytes);
	request(&b, line, WRENCH_TYPE.schema, WRENCH_TYPE.schema_bytes);
	request(&b, line, WRENCH_TYPE.schema, WRENCH_TYPE.schema_bytes);
	snprintf(line, sizeof line, "open nav.depth hash=%016llx payload=%u schema=%u\n", (unsigned long long)DEPTH_TYPE.hash,
		 DEPTH_TYPE.payload_bytes, DEPTH_TYPE.schema_bytes);
	request(&b, line, DEPTH_TYPE.schema, DEPTH_TYPE.schema_bytes);
	request(&b, "status", NULL, 0);
	request(&b, "start safety", NULL, 0);
	request(&b, "restart-self", NULL, 0);
	put("control", "module", &b);

	/* The bridge: a topic of the other side, with its boot id and writer. */
	mode = 2 | (1 << 2);
	add(&b, &mode, 1);
	snprintf(line, sizeof line,
		 "open pilot.wrench hash=%016llx payload=%u schema=%u boot=00112233445566778899aabbccddeeff first=7 "
		 "writer=joystick slots=16\n",
		 (unsigned long long)WRENCH_TYPE.hash, WRENCH_TYPE.payload_bytes, WRENCH_TYPE.schema_bytes);
	request(&b, line, WRENCH_TYPE.schema, WRENCH_TYPE.schema_bytes);
	snprintf(line, sizeof line,
		 "open nav.depth hash=%016llx payload=%u schema=%u boot=00112233445566778899aabbccddeeff slots=4\n",
		 (unsigned long long)DEPTH_TYPE.hash, DEPTH_TYPE.payload_bytes, DEPTH_TYPE.schema_bytes);
	request(&b, line, DEPTH_TYPE.schema, DEPTH_TYPE.schema_bytes);
	snprintf(line, sizeof line,
		 "open hangar.vehicle.system hash=%016llx payload=%u schema=%u boot=00000000000000000000000000000000\n",
		 (unsigned long long)DEPTH_TYPE.hash, DEPTH_TYPE.payload_bytes, DEPTH_TYPE.schema_bytes);
	request(&b, line, DEPTH_TYPE.schema, DEPTH_TYPE.schema_bytes);
	put("control", "bridge", &b);

	/* A stranger: refused by name, and a connection that says nothing. */
	mode = 16;
	add(&b, &mode, 1);
	request(&b, "status", NULL, 0);
	request(&b, "stop safety", NULL, 0);
	request(&b, "clear-failsafe", NULL, 0);
	request(&b, "", NULL, 0);
	request(&b, "nonsense with arguments\n", NULL, 0);
	put("control", "stranger", &b);
	free(b.p);
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(tmpdir, sizeof tmpdir, "%s/hg-mkseeds-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(tmpdir))
		abort();
	text_seeds();
	recording_seeds();
	topic_seeds();
	bridge_seeds();
	mavlink_seeds();
	video_seeds();
	conf_seeds();
	control_seeds();
	rmdir(tmpdir);
	return 0;
}
