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

#include "depth.h"
#include "hg_record.h"
#include "hg_topic.h"
#include "hg_type.h"

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

static void put(const char *harness, const char *name, struct buf *b)
{
	char path[512];
	snprintf(path, sizeof path, "fuzz/corpus/%s", harness);
	mkdir(path, 0777);
	snprintf(path, sizeof path, "fuzz/corpus/%s/seed-%s", harness, name);
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
	rmdir(tmpdir);
	return 0;
}
