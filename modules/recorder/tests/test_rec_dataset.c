// SPDX-License-Identifier: GPL-3.0-or-later
/* Datasets (docs/06 "Datasets"): the spec parser on the doc's examples and
 * on every refused form; the row type's schema and layout; Where; the join
 * on a provenance chain a -> b -> c published in this process with
 * hg_publish_with, with a Sources topic referenced every fourth message
 * (missing otherwise), a stale range, a reused reference, windows at the
 * start of a stream, an asof part published after its stamp, and a part
 * gone from a 4-slot ring; the reset rule; Rate; the same rows from the
 * rings and from recordings of the same messages, identical except `late`
 * and the ring's misses. With HGR_FIXTURE_DIR set, writes chain.dataset and
 * dataset.chain.hgr there for python/tests/test_dataset.py. */
#include "rec_test.h"

#include "depth.h"
#include "hg_manager_stub.h"
#include "imu.h"
#include "wrench.h"

static char wiring_path[600], files[600];

/* ---- The spec ---------------------------------------------------------------- */

static const char DEPTH_FF[] =
	"# /etc/hangar/datasets/depth_ff.dataset\n"
	"[Dataset]\n"
	"Anchor=nav.state\n"
	"AnchorRole=input\n"
	"Fields=depth vel_z pitch roll\n"
	"Lateness=0\n"
	"Filter=mask\n"
	"ResetAfter=200ms\n"
	"# Outputs enabled, and the vehicle vertically still (m/s): what was applied held it.\n"
	"Where=safety.state.outputs_enabled == 1 and nav.state.vel_z > -0.05 and nav.state.vel_z < 0.05\n"
	"\n"
	"[Input ctl.depth_hold.target]\n"
	"Join=asof:0\n"
	"Fields=accel_z\n"
	"\n"
	"[Input ctl.depth_hold]\n"
	"Join=asof:0\n"
	"Role=label\n"
	"Fields=heave\n"
	"\n"
	"[Input thr.out]\n"
	"Join=asof:0\n"
	"Role=label\n"
	"\n"
	"[Input safety.state]\n"
	"Join=asof:0\n"
	"Role=filter\n"
	"Fields=outputs_enabled\n";

static const char CLONE[] =
	"[Dataset]\n"
	"Anchor=ctl.depth_hold\n"
	"AnchorRole=label\n"
	"\n"
	"# The depth message the controller computed this output from.\n"
	"[Input nav.depth]\n"
	"Join=provenance\n"
	"\n"
	"[Input nav.imu]\n"
	"Join=provenance\n"
	"Window=8\n"
	"Fields=gyro accel\n";

static void test_spec_examples(void)
{
	struct hg_ds_spec s;
	char err[300];
	CHECK_EQ_INT(hg_ds_parse(DEPTH_FF, strlen(DEPTH_FF), "depth_ff", &s, err, sizeof err), 0);
	CHECK(strcmp(s.name, "depth_ff") == 0);
	CHECK(strcmp(s.anchor, "nav.state") == 0);
	CHECK_EQ_INT(s.anchor_role, HG_DS_INPUT);
	CHECK_EQ_INT(s.n_anchor_fields, 4);
	CHECK(strcmp(s.anchor_fields[1], "vel_z") == 0);
	CHECK_EQ_INT(s.lateness_ns, 0);
	CHECK_EQ_INT(s.filter, HG_DS_MASK);
	CHECK_EQ_INT(s.missing_drop, 0);
	CHECK_EQ_INT(s.reset_after_ns, 200000000);
	CHECK_EQ_INT(s.rate_every, 1);
	CHECK_EQ_INT(s.n_parts, 4);
	CHECK(strcmp(s.parts[0].name, "ctl.depth_hold.target") == 0 && strcmp(s.parts[0].topic, "ctl.depth_hold.target") == 0);
	CHECK_EQ_INT(s.parts[0].join, HG_DS_ASOF);
	CHECK_EQ_INT(s.parts[0].offset_ns, 0);
	CHECK_EQ_INT(s.parts[0].role, HG_DS_INPUT);
	CHECK_EQ_INT(s.parts[1].role, HG_DS_LABEL);
	CHECK_EQ_INT(s.parts[2].n_fields, 0);
	CHECK_EQ_INT(s.parts[3].role, HG_DS_FILTER);
	CHECK(strstr(s.where, "outputs_enabled == 1") != NULL);
	CHECK_EQ_INT(hg_ds_wait_ns(&s), 0);
	CHECK_EQ_INT(hg_ds_policy_usable(&s, NULL, 0), 1);

	CHECK_EQ_INT(hg_ds_parse(CLONE, strlen(CLONE), "depth_hold_clone", &s, err, sizeof err), 0);
	CHECK_EQ_INT(s.anchor_role, HG_DS_LABEL);
	CHECK_EQ_INT(s.lateness_ns, 100000000);
	CHECK_EQ_INT(s.filter, HG_DS_DROP);
	CHECK_EQ_INT(s.parts[1].window, 8);
	CHECK_EQ_INT(s.parts[1].n_fields, 2);
	CHECK(strcmp(s.parts[1].fields[1], "accel") == 0);
	CHECK_EQ_INT(hg_ds_wait_ns(&s), 0);

	/* A positive asof offset makes the row wait; an input one disqualifies a policy. */
	static const char LOOK[] = "[Dataset]\nAnchor=a.b\n[Input future]\nTopic=a.b\nJoin=asof:50ms\n"
				   "[Input lbl]\nTopic=a.c\nJoin=asof:+30ms\nRole=label\n[Input back]\nTopic=a.d\nJoin=asof:-20ms\n";
	CHECK_EQ_INT(hg_ds_parse(LOOK, strlen(LOOK), "look", &s, err, sizeof err), 0);
	CHECK_EQ_INT(s.parts[0].offset_ns, 50000000);
	CHECK_EQ_INT(s.parts[1].offset_ns, 30000000);
	CHECK_EQ_INT(s.parts[2].offset_ns, -20000000);
	CHECK_EQ_INT(hg_ds_wait_ns(&s), 150000000);
	char bad[200];
	CHECK_EQ_INT(hg_ds_policy_usable(&s, bad, sizeof bad), 0);
	CHECK(strcmp(bad, "future back") == 0); /* back: -20 + 100 ms of Lateness waits too */
	s.lateness_ns = 20000000;
	CHECK_EQ_INT(hg_ds_policy_usable(&s, bad, sizeof bad), 0);
	CHECK(strcmp(bad, "future") == 0); /* back: -20 + 20 = 0 is still fine */
	/* Rate forms. */
	static const char RATE[] = "[Dataset]\nAnchor=a.b\nRate=every:4\nMissing=drop\n";
	CHECK_EQ_INT(hg_ds_parse(RATE, strlen(RATE), "r", &s, err, sizeof err), 0);
	CHECK_EQ_INT(s.rate_every, 4);
	CHECK_EQ_INT(s.missing_drop, 1);
	static const char RATE2[] = "[Dataset]\nAnchor=a.b\nRate=rate:12.5\n";
	CHECK_EQ_INT(hg_ds_parse(RATE2, strlen(RATE2), "r", &s, err, sizeof err), 0);
	CHECK_EQ_INT(s.rate_kind, HG_DS_RATE_HZ);
	CHECK_EQ_INT(s.rate_mhz, 12500);
}

static void refused(const char *text, const char *expect)
{
	struct hg_ds_spec s;
	char err[300] = "";
	int rc = hg_ds_parse(text, strlen(text), "bad", &s, err, sizeof err);
	if (rc == 0 || !strstr(err, expect))
		printf("    FAIL expected a refusal mentioning '%s' for:\n%s    got rc %d: %s\n", expect, text, rc, err);
	CHECK(rc < 0 && errno == EINVAL && strstr(err, expect));
}

static void test_spec_refusals(void)
{
	refused("Anchor=a.b\n", "before any section");
	refused("[Dataset]\n", "Anchor= is required");
	refused("[Input x]\nJoin=provenance\n", "Anchor= is required");
	refused("[Dataset]\nAnchor=a.b\n[Dataset]\n", "given twice");
	refused("[Dataset x]\nAnchor=a.b\n", "no argument");
	refused("[Dataset]\nAnchor=a.b\nAnchor=a.c\n", "given twice");
	refused("[Dataset]\nAnchor=Nav.State\n", "not a topic name");
	refused("[Dataset]\nAnchor=a.b\nAnchorRole=filter\n", "label or input");
	refused("[Dataset]\nAnchor=a.b\nAnchorRole=output\n", "label or input");
	refused("[Dataset]\nAnchor=a.b\nFields=\n", "empty");
	refused("[Dataset]\nAnchor=a.b\nFields=x x\n", "listed twice");
	refused("[Dataset]\nAnchor=a.b\nFields=a.b\n", "bad field name");
	refused("[Dataset]\nAnchor=a.b\nRate=every:0\n", "every:N");
	refused("[Dataset]\nAnchor=a.b\nRate=every:x\n", "every:N");
	refused("[Dataset]\nAnchor=a.b\nRate=rate:0\n", "rate:<Hz>");
	refused("[Dataset]\nAnchor=a.b\nRate=4\n", "every:N or rate:<Hz>");
	refused("[Dataset]\nAnchor=a.b\nWhere=\n", "Where: empty");
	refused("[Dataset]\nAnchor=a.b\nWhere=a.b.x\n", "comparison");
	refused("[Dataset]\nAnchor=a.b\nWhere=a.b.x == \n", "number");
	refused("[Dataset]\nAnchor=a.b\nWhere=a.b.x == 1 and\n", "<part>.<field>");
	refused("[Dataset]\nAnchor=a.b\nWhere=(a.b.x == 1\n", "')'");
	refused("[Dataset]\nAnchor=a.b\nWhere=a.b.x == 1 1\n", "unexpected");
	refused("[Dataset]\nAnchor=a.b\nWhere=a.b.x = 1\n", "comparison");
	refused("[Dataset]\nAnchor=a.b\nWhere=a.b.x == 1 && a.b.y == 2\n", "Where");
	refused("[Dataset]\nAnchor=a.b\nFilter=keep\n", "drop or mask");
	refused("[Dataset]\nAnchor=a.b\nMissing=zero\n", "flag or drop");
	refused("[Dataset]\nAnchor=a.b\nResetAfter=soon\n", "duration");
	refused("[Dataset]\nAnchor=a.b\nLateness=-5ms\n", "duration");
	refused("[Dataset]\nAnchor=a.b\nColour=red\n", "unknown key");
	refused("[Dataset]\nAnchor=a.b\n[Output x]\n", "unknown section");
	refused("[Dataset]\nAnchor=a.b\n[Input]\nJoin=provenance\n", "needs a part name");
	refused("[Dataset]\nAnchor=a.b\n[Input X]\nJoin=provenance\n", "needs a part name");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\n", "needs Join=");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=nearest\n", "provenance or asof");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=asof:\n", "provenance or asof");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=asof:5\n", "provenance or asof");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\nJoin=provenance\n", "given twice");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\nWindow=0\n", "Window");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\nWindow=65\n", "Window");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\nRole=anchor\n", "input, label or filter");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\nTopic=Bad\n", "not a topic name");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\nLateness=1s\n", "unknown key");
	refused("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\n[Input x]\nJoin=provenance\n", "given twice");
	refused("[Dataset]\nAnchor=a.b\nthis is not a key\n", "expected [Section] or Key=Value");
	refused("[Dataset\nAnchor=a.b\n", "']'");
	refused("[Dataset]\nAnchor=a.b\n=x\n", "empty key");
	/* The name itself. */
	struct hg_ds_spec s;
	char err[300];
	CHECK(hg_ds_parse(CLONE, strlen(CLONE), "Bad-Name", &s, err, sizeof err) < 0);
	CHECK(hg_ds_parse(CLONE, strlen(CLONE), "", &s, err, sizeof err) < 0);
	/* A file: the stem is the name, and it must end in .dataset. */
	char p[700];
	rfmt(p, sizeof p, "%s/depth_ff.dataset", files);
	write_file(p, DEPTH_FF);
	CHECK_EQ_INT(hg_ds_read_file(p, &s, err, sizeof err), 0);
	CHECK(strcmp(s.name, "depth_ff") == 0);
	rfmt(p, sizeof p, "%s/depth_ff.ini", files);
	write_file(p, DEPTH_FF);
	CHECK(hg_ds_read_file(p, &s, err, sizeof err) < 0 && strstr(err, "<name>.dataset"));
	CHECK(hg_ds_read_file("/nonexistent.dataset", &s, err, sizeof err) < 0);
}

/* ---- The row type ------------------------------------------------------------- */

static const char CHAIN_SPEC[] =
	"# chain.dataset: the suite's spec over the toy chain (modules/recorder/tests)\n"
	"[Dataset]\n"
	"Anchor=t.c\n"
	"AnchorRole=label\n"
	"Fields=surge heave\n"
	"Lateness=0\n"
	"Filter=mask\n"
	"ResetAfter=100ms\n"
	"Where=t.c.heave >= 0.2 and t.x.depth_m > 0\n"
	"\n"
	"[Input t.a]\n"
	"Join=provenance\n"
	"\n"
	"[Input t.b]\n"
	"Join=provenance\n"
	"Window=3\n"
	"Fields=gyro\n"
	"\n"
	"[Input t.x]\n"
	"Join=provenance\n"
	"Role=filter\n"
	"\n"
	"[Input t.s]\n"
	"Join=asof:0\n"
	"Role=label\n"
	"Fields=depth_m\n"
	"\n"
	"[Input t.g]\n"
	"Join=provenance\n";

static struct hg_ds_topic_info ti(const struct hg_type *t, const char *name, uint32_t rate_mhz)
{
	struct hg_ds_topic_info i = { t, hg_topic_id(name), rate_mhz, rate_mhz ? 3 * 1000000000000ull / rate_mhz : 0 };
	return i;
}

static struct hg_ds_row_type RT;

static void bind_chain(struct hg_ds_row_type *rt)
{
	struct hg_ds_spec s;
	char err[300];
	CHECK_EQ_INT(hg_ds_parse(CHAIN_SPEC, strlen(CHAIN_SPEC), "chain", &s, err, sizeof err), 0);
	struct hg_ds_topic_info anchor = ti(&WRENCH_TYPE, "t.c", 50000);
	struct hg_ds_topic_info parts[5] = { ti(&DEPTH_TYPE, "t.a", 50000), ti(&IMU_TYPE, "t.b", 50000),
					     ti(&DEPTH_TYPE, "t.x", 0), ti(&DEPTH_TYPE, "t.s", 10000),
					     ti(&DEPTH_TYPE, "t.g", 50000) };
	int rc = hg_ds_bind(&s, &anchor, parts, rt, err, sizeof err);
	if (rc < 0)
		printf("    bind: %s\n", err);
	CHECK_EQ_INT(rc, 0);
}

static void test_row_type(void)
{
	bind_chain(&RT);
	const struct hg_type *t = &RT.schema.type;
	/* The header, then the anchor's fields, then the parts in order. */
	const struct hg_field *f;
	CHECK((f = hg_type_field(t, "anchor_seq")) && f->offset == 0 && f->kind == HG_U64);
	CHECK((f = hg_type_field(t, "boot_id")) && f->offset == 16 && f->count == 16);
	CHECK((f = hg_type_field(t, "missing")) && f->offset == 32);
	CHECK((f = hg_type_field(t, "flags")) && f->offset == 36);
	CHECK((f = hg_type_field(t, "emitted_ns")) && f->offset == 40);
	CHECK((f = hg_type_field(t, "t.c.surge")) && f->offset == 48 && f->kind == HG_F32);
	CHECK((f = hg_type_field(t, "t.c.heave")) && f->offset == 52);
	CHECK(hg_type_field(t, "t.c.yaw") == NULL);
	CHECK((f = hg_type_field(t, "t.a._seq")) && f->offset == 56);
	CHECK((f = hg_type_field(t, "t.a._stamp_ns")) && f->offset == 64);
	CHECK((f = hg_type_field(t, "t.a._count")) && f->offset == 72);
	CHECK((f = hg_type_field(t, "t.a._flags")) && f->offset == 76);
	CHECK((f = hg_type_field(t, "t.a.depth_m")) && f->offset == 80);
	CHECK((f = hg_type_field(t, "t.a.pressure_mbar")) && f->offset == 88);
	CHECK((f = hg_type_field(t, "t.b._seq")) && f->offset == 96);
	CHECK((f = hg_type_field(t, "t.b.0.gyro")) && f->offset == 120 && f->count == 3);
	CHECK((f = hg_type_field(t, "t.b.1.gyro")) && f->offset == 132);
	CHECK((f = hg_type_field(t, "t.b.2.gyro")) && f->offset == 144);
	CHECK(hg_type_field(t, "t.b.gyro") == NULL && hg_type_field(t, "t.b.0.accel") == NULL);
	CHECK((f = hg_type_field(t, "t.x._seq")) && f->offset == 160);
	CHECK((f = hg_type_field(t, "t.x.depth_m")) && f->offset == 184);
	CHECK((f = hg_type_field(t, "t.s._seq")) && f->offset == 200);
	CHECK((f = hg_type_field(t, "t.s.depth_m")) && f->offset == 224);
	CHECK((f = hg_type_field(t, "t.g._seq")) && f->offset == 232);
	CHECK((f = hg_type_field(t, "t.g.pressure_mbar")) && f->offset == 264);
	CHECK_EQ_INT(RT.row_bytes, 272);
	CHECK_EQ_INT(t->payload_bytes, 272);
	CHECK_EQ_INT(t->n_fields, 6 + 2 + 7 + 7 + 7 + 5 + 7);
	/* Roles and the source comments travel in the schema text. */
	CHECK(strstr(t->schema, "f32 t.c.surge # role=label -1 to 1, forward positive"));
	CHECK(strstr(t->schema, "u64 t.a._seq # role=input"));
	CHECK(strstr(t->schema, "f32 t.b.2.gyro[3] # role=input rad/s, body x y z"));
	CHECK(strstr(t->schema, "f32 t.x.depth_m # role=filter m, positive down"));
	CHECK(strstr(t->schema, "u64 anchor_seq # role=meta"));
	CHECK(t->schema_bytes < HG_SCHEMA_MAX);
	/* Immediate joins: provenance, and asof with offset + Lateness <= 0. */
	CHECK(RT.parts[0].immediate && RT.parts[1].immediate && RT.parts[3].immediate);
	CHECK_EQ_INT(RT.wait_ns, 0);
	CHECK_EQ_INT(RT.scratch_bytes, 40);
	/* Where named the anchor and part 2 (t.x). */
	CHECK_EQ_INT(RT.n_where, 3);
	CHECK_EQ_INT(RT.where_parts, 1u << 2);
	CHECK_EQ_INT(RT.where[0].part, -1);
	CHECK_EQ_INT(RT.where[0].row_off, 52);
	CHECK_EQ_INT(RT.where[1].part, 2);
	CHECK_EQ_INT(RT.where[2].op, HG_DS_OP_AND);

	/* Refusals at bind. */
	struct hg_ds_spec s;
	char err[300];
	static struct hg_ds_row_type rt2;
	struct hg_ds_topic_info anchor = ti(&WRENCH_TYPE, "t.c", 50000);
	struct hg_ds_topic_info parts[1] = { ti(&DEPTH_TYPE, "t.a", 50000) };
	static const char NOFIELD[] = "[Dataset]\nAnchor=t.c\nFields=surge altitude\n[Input t.a]\nJoin=provenance\n";
	CHECK_EQ_INT(hg_ds_parse(NOFIELD, strlen(NOFIELD), "x", &s, err, sizeof err), 0);
	CHECK(hg_ds_bind(&s, &anchor, parts, &rt2, err, sizeof err) < 0 && strstr(err, "no field altitude"));
	static const char NOPFIELD[] = "[Dataset]\nAnchor=t.c\n[Input t.a]\nJoin=provenance\nFields=salinity\n";
	CHECK_EQ_INT(hg_ds_parse(NOPFIELD, strlen(NOPFIELD), "x", &s, err, sizeof err), 0);
	CHECK(hg_ds_bind(&s, &anchor, parts, &rt2, err, sizeof err) < 0 && strstr(err, "part t.a: t.a has no field salinity"));
	static const char RATEEV[] = "[Dataset]\nAnchor=t.c\nRate=rate:10\n";
	CHECK_EQ_INT(hg_ds_parse(RATEEV, strlen(RATEEV), "x", &s, err, sizeof err), 0);
	struct hg_ds_topic_info ev = ti(&WRENCH_TYPE, "t.c", 0);
	CHECK(hg_ds_bind(&s, &ev, parts, &rt2, err, sizeof err) < 0 && strstr(err, "event topic"));
	CHECK_EQ_INT(hg_ds_bind(&s, &anchor, parts, &rt2, err, sizeof err), 0);
	CHECK_EQ_INT(rt2.spec.rate_every, 5); /* 50 Hz / 10 Hz */
	static const char WBAD[] = "[Dataset]\nAnchor=t.c\nWhere=t.c.surge > 0 and nav.depth.x < 1\n[Input t.a]\nJoin=provenance\n";
	CHECK_EQ_INT(hg_ds_parse(WBAD, strlen(WBAD), "x", &s, err, sizeof err), 0);
	CHECK(hg_ds_bind(&s, &anchor, parts, &rt2, err, sizeof err) < 0 && strstr(err, "names no part"));
	static const char WARR[] = "[Dataset]\nAnchor=t.c\nWhere=t.a.depth_m > 0 and t.b.gyro > 1\n[Input t.a]\nJoin=provenance\n[Input t.b]\nJoin=provenance\n";
	struct hg_ds_topic_info parts2[2] = { ti(&DEPTH_TYPE, "t.a", 50000), ti(&IMU_TYPE, "t.b", 50000) };
	CHECK_EQ_INT(hg_ds_parse(WARR, strlen(WARR), "x", &s, err, sizeof err), 0);
	CHECK(hg_ds_bind(&s, &anchor, parts2, &rt2, err, sizeof err) < 0 && strstr(err, "array"));
	static const char WKEPT[] = "[Dataset]\nAnchor=t.c\nWhere=t.a.temp_c > 0\n[Input t.a]\nJoin=provenance\nFields=depth_m\n";
	CHECK_EQ_INT(hg_ds_parse(WKEPT, strlen(WKEPT), "x", &s, err, sizeof err), 0);
	CHECK(hg_ds_bind(&s, &anchor, parts, &rt2, err, sizeof err) < 0 && strstr(err, "Fields="));
	/* Too many row fields: 64 x 3 imu fields + ... passes 128. */
	static const char BIG[] = "[Dataset]\nAnchor=t.c\n[Input t.b]\nJoin=provenance\nWindow=64\n";
	struct hg_ds_topic_info partb[1] = { ti(&IMU_TYPE, "t.b", 50000) };
	CHECK_EQ_INT(hg_ds_parse(BIG, strlen(BIG), "x", &s, err, sizeof err), 0);
	CHECK(hg_ds_bind(&s, &anchor, partb, &rt2, err, sizeof err) < 0);
	/* Manifests: a provenance join must be in the anchor's Inputs or Sources. */
	char mdir[700], mp[800];
	rfmt(mdir, sizeof mdir, "%s/manifests", files);
	mkdir_p(mdir, 0755);
	rfmt(mp, sizeof mp, "%s/chain.module", mdir);
	write_file(mp, "[Module]\nExec=/bin/true\n[Publish t.c]\nType=wrench\nInputs=t.a t.b\nSources=t.x t.g\n"
			"[Publish t.a]\nType=depth\n");
	CHECK_EQ_INT(hg_ds_parse(CHAIN_SPEC, strlen(CHAIN_SPEC), "chain", &s, err, sizeof err), 0);
	CHECK_EQ_INT(hg_ds_check_manifests(&s, mdir, err, sizeof err), 0);
	rfmt(s.parts[4].topic, sizeof s.parts[4].topic, "t.other");
	CHECK(hg_ds_check_manifests(&s, mdir, err, sizeof err) < 0 && errno == EINVAL && strstr(err, "t.other"));
	rfmt(s.anchor, sizeof s.anchor, "t.none");
	CHECK(hg_ds_check_manifests(&s, mdir, err, sizeof err) < 0 && errno == ENOENT);
}

static void test_where(void)
{
	unsigned char row[272];
	memset(row, 0, sizeof row);
	float v;
	v = 0.5f;
	memcpy(row + 52, &v, 4); /* t.c.heave */
	v = 3.0f;
	memcpy(row + 184, &v, 4); /* t.x.depth_m */
	CHECK_EQ_INT(hg_ds_where(&RT, row), 1);
	v = 0.1f;
	memcpy(row + 52, &v, 4);
	CHECK_EQ_INT(hg_ds_where(&RT, row), 0);
	v = 0.2f;
	memcpy(row + 52, &v, 4);
	CHECK_EQ_INT(hg_ds_where(&RT, row), 1);
	/* A missing part named in Where makes it false. */
	uint32_t missing = 1u << 2;
	memcpy(row + 32, &missing, 4);
	CHECK_EQ_INT(hg_ds_where(&RT, row), 0);
	missing = 1u << 0;
	memcpy(row + 32, &missing, 4);
	CHECK_EQ_INT(hg_ds_where(&RT, row), 1);
	/* or, parentheses, every operator and every kind. */
	struct hg_ds_spec s;
	char err[300];
	static struct hg_ds_row_type rt2;
	static const char W[] = "[Dataset]\nAnchor=t.c\nWhere=(t.c.surge < 1 or t.c.yaw >= 2) and t.b.temp_c != 30 and t.a.pressure_mbar <= 5\n"
				"[Input t.a]\nJoin=provenance\n[Input t.b]\nJoin=provenance\nWindow=2\n";
	struct hg_ds_topic_info anchor = ti(&WRENCH_TYPE, "t.c", 50000);
	struct hg_ds_topic_info parts2[2] = { ti(&DEPTH_TYPE, "t.a", 50000), ti(&IMU_TYPE, "t.b", 50000) };
	CHECK_EQ_INT(hg_ds_parse(W, strlen(W), "w", &s, err, sizeof err), 0);
	CHECK_EQ_INT(hg_ds_bind(&s, &anchor, parts2, &rt2, err, sizeof err), 0);
	CHECK_EQ_INT(rt2.n_where, 7);
	unsigned char *r2 = calloc(1, rt2.row_bytes);
	const struct hg_field *surge = hg_type_field(&rt2.schema.type, "t.c.surge");
	const struct hg_field *yaw = hg_type_field(&rt2.schema.type, "t.c.yaw");
	const struct hg_field *temp = hg_type_field(&rt2.schema.type, "t.b.1.temp_c"); /* the last of the window */
	const struct hg_field *pres = hg_type_field(&rt2.schema.type, "t.a.pressure_mbar");
	CHECK(surge && yaw && temp && pres);
	CHECK_EQ_INT(rt2.where[3].row_off, temp->offset); /* postfix: surge yaw or temp and pres and */
	v = 5;
	memcpy(r2 + surge->offset, &v, 4);
	v = 2;
	memcpy(r2 + yaw->offset, &v, 4);
	v = 29;
	memcpy(r2 + temp->offset, &v, 4);
	v = 5;
	memcpy(r2 + pres->offset, &v, 4);
	CHECK_EQ_INT(hg_ds_where(&rt2, r2), 1);
	v = 1.9f;
	memcpy(r2 + yaw->offset, &v, 4);
	CHECK_EQ_INT(hg_ds_where(&rt2, r2), 0);
	v = 0.5f;
	memcpy(r2 + surge->offset, &v, 4);
	CHECK_EQ_INT(hg_ds_where(&rt2, r2), 1);
	v = 30;
	memcpy(r2 + temp->offset, &v, 4);
	CHECK_EQ_INT(hg_ds_where(&rt2, r2), 0);
	free(r2);
}

/* ---- The chain: live and recorded ---------------------------------------------- */

struct topic {
	const char *name;
	const struct hg_type *type;
	uint32_t rate_mhz;
	uint32_t slots;
	hg_pub *pub;
	hg_sub *sub;
	struct hgr_writer w;
};

static struct topic T[6] = {
	{ "t.a", &DEPTH_TYPE, 50000, 64, NULL, NULL, { 0 } }, { "t.b", &IMU_TYPE, 50000, 64, NULL, NULL, { 0 } },
	{ "t.c", &WRENCH_TYPE, 50000, 64, NULL, NULL, { 0 } },
	{ "t.x", &DEPTH_TYPE, 0, 16, NULL, NULL, { 0 } },     { "t.s", &DEPTH_TYPE, 10000, 64, NULL, NULL, { 0 } },
	{ "t.g", &DEPTH_TYPE, 50000, 4, NULL, NULL, { 0 } },
};
enum { TA, TB, TC, TX, TS, TG };

static void write_wiring(void)
{
	rfmt(wiring_path, sizeof wiring_path, "%s/dstest.wiring", R_root);
	FILE *f = fopen(wiring_path, "w");
	CHECK(f != NULL);
	fprintf(f, "hangar-wiring 1\nmodule dstest\n");
	for (int i = 0; i < 6; i++) {
		fprintf(f, "pub %s type=%s hash=%016llx slots=%u", T[i].name, T[i].type->name,
			(unsigned long long)T[i].type->hash, T[i].slots);
		if (T[i].rate_mhz)
			fprintf(f, " rate=%g", T[i].rate_mhz / 1000.0);
		if (i == TB)
			fprintf(f, " inputs=t.a");
		if (i == TC)
			fprintf(f, " inputs=t.a,t.b sources=t.x,t.g");
		fprintf(f, "\n");
	}
	fclose(f);
	setenv("HANGAR_WIRING_FILE", wiring_path, 1);
	CHECK_EQ_INT(hg_init(), 0);
}

/* Publishes on topic i and appends the message, as read back, to its file. */
static uint64_t pub(int i, const void *payload, size_t len, uint64_t stamp, unsigned flags,
		    const struct hg_input *refs, int n)
{
	struct topic *t = &T[i];
	int rc = hg_publish_with(t->pub, payload, len, stamp, flags, refs, n);
	if (rc < 0)
		printf("    publish %s: %s\n", t->name, strerror(errno));
	CHECK_EQ_INT(rc, 0);
	uint64_t seq = hg_pub_next_seq(t->pub) - 1;
	unsigned char buf[64];
	struct hg_info info;
	CHECK_EQ_INT(hg_read_seq(t->sub, seq, buf, sizeof buf, &info), HG_MSG);
	struct hgr_record r;
	hgr_record_init(&r, &info, 0, 0);
	CHECK_EQ_INT(hgr_append(&t->w, &r, buf), 0);
	return seq;
}

#define TICKS 40
static unsigned char *live_rows[TICKS + 1];
static struct hg_ds_joined live_jr[TICKS + 1][HG_DS_MAX_PARTS];
static struct hg_ds_state live_state;
static int live_keep[TICKS + 1];
static int live_n;
static unsigned char boot[16];

static uint32_t row32(const unsigned char *row, uint32_t off)
{
	uint32_t v;
	memcpy(&v, row + off, 4);
	return v;
}

static uint64_t row64(const unsigned char *row, uint32_t off)
{
	uint64_t v;
	memcpy(&v, row + off, 8);
	return v;
}

static float rowf(const unsigned char *row, uint32_t off)
{
	float v;
	memcpy(&v, row + off, 4);
	return v;
}

/* Joins anchor message n from the rings, as the pairer does. */
static void live_join(uint64_t seq)
{
	unsigned char payload[64];
	struct hg_info info;
	CHECK_EQ_INT(hg_read_seq(T[TC].sub, seq, payload, sizeof payload, &info), HG_MSG);
	unsigned char *row = live_rows[seq];
	hg_ds_row_begin(&RT, row, &info, payload, boot);
	unsigned char scratch[64];
	for (int p = 0; p < RT.spec.n_parts; p++) {
		struct hg_ds_source src = hg_ds_source_sub(T[p == 0 ? TA : p == 1 ? TB : p == 2 ? TX : p == 3 ? TS : TG].sub);
		hg_ds_join(&RT, p, row, &info, &src, scratch, &live_jr[seq][p]);
	}
	live_keep[seq] = hg_ds_row_finish(&RT, &live_state, row, live_jr[seq], info.gap > 0, info.epoch);
	live_n++;
}

static void test_chain_live(void)
{
	write_wiring();
	hg_ds_boot_id(boot);
	for (int i = 0; i < 6; i++) {
		T[i].pub = hg_pub_open(T[i].name, T[i].type, NULL);
		T[i].sub = hg_sub_open_own(T[i].name, NULL, HG_OLDEST);
		if (!T[i].pub || !T[i].sub)
			printf("    %s: %s\n", T[i].name, strerror(errno));
		CHECK(T[i].pub && T[i].sub);
		struct hg_topic_meta m;
		CHECK_EQ_INT(hg_sub_meta(T[i].sub, &m), 0);
		uint32_t slen;
		const char *schema = hg_sub_schema(T[i].sub, &slen);
		struct hgr_header h;
		hgr_header_init(&h, &m, schema, slen, HGR_FULL, 1, "full", "dstest", 0);
		char path[700];
		rfmt(path, sizeof path, "%s/%s.hgr", files, T[i].name);
		CHECK_EQ_INT(hgr_create(&T[i].w, path, &h), 0);
	}
	for (int n = 0; n <= TICKS; n++)
		live_rows[n] = calloc(1, RT.row_bytes);
	uint32_t ida = hg_pub_topic_id(T[TA].pub), idb = hg_pub_topic_id(T[TB].pub), idx = hg_pub_topic_id(T[TX].pub),
		 idg = hg_pub_topic_id(T[TG].pub);
	uint64_t b_seq = 0, x_seq = 0;
	for (int n = 1; n <= TICKS; n++) {
		uint64_t stamp = hg_now_ns() - 1000000;
		struct depth da = { .depth_m = (float)n * 0.01f, .temp_c = 20, .pressure_mbar = (float)n };
		uint64_t a_seq = pub(TA, &da, sizeof da, stamp, 0, NULL, 0);
		if (n != 30) { /* tick 30: b not published, c reuses b(29) */
			struct imu im = { .gyro = { (float)n, 1, 2 }, .accel = { 0, 0, 9.81f }, .temp_c = 30 };
			struct hg_input ra = { .topic_id = ida, .seq = a_seq };
			b_seq = pub(TB, &im, sizeof im, stamp, n >= 11 && n < 19 ? HG_STALE : 0, &ra, 1);
		}
		int with_x = n % 4 == 0;
		if (with_x) {
			struct depth dx = { .depth_m = (float)n, .temp_c = 1, .pressure_mbar = 2 };
			x_seq = pub(TX, &dx, sizeof dx, stamp, 0, NULL, 0);
		}
		struct depth dg = { .depth_m = (float)n * 2, .temp_c = 3, .pressure_mbar = 4 };
		uint64_t g_seq = pub(TG, &dg, sizeof dg, stamp, 0, NULL, 0);
		struct wrench w = { .surge = (float)n, .heave = (float)(n % 7) * 0.1f, .yaw = 0.5f };
		struct hg_input refs[4] = { { ida, 0, a_seq }, { idb, 0, b_seq }, { idg, 0, g_seq }, { idx, 0, x_seq } };
		uint64_t c_seq = pub(TC, &w, sizeof w, stamp, 0, refs, with_x ? 4 : 3);
		CHECK_EQ_INT(c_seq, (uint64_t)n);
		/* The first half is joined at once, before s(n) is published (a
		 * pairer running ahead of a late publisher); the second half is
		 * joined after the batch, when t.g's 4-slot ring has moved on. */
		if (n <= 20)
			live_join(c_seq);
		if (n % 5 == 0) {
			struct depth ds = { .depth_m = (float)n, .temp_c = 2, .pressure_mbar = 3 };
			pub(TS, &ds, sizeof ds, stamp - 500000, 0, NULL, 0);
		}
		sleep_ms(20); /* 50 Hz: the asof stamps and ResetAfter are measured in it */
	}
	for (int n = 21; n <= TICKS; n++)
		live_join((uint64_t)n);
	CHECK_EQ_INT(live_n, TICKS);
	for (int i = 0; i < 6; i++)
		hgr_close(&T[i].w);

	/* What the rows say. */
	for (int n = 1; n <= TICKS; n++) {
		const unsigned char *row = live_rows[n];
		uint32_t missing = row32(row, 32), flags = row32(row, 36);
		CHECK_EQ_INT(row64(row, 0), (uint64_t)n);
		CHECK(memcmp(row + 16, boot, 16) == 0);
		struct hg_info ai;
		unsigned char ap[64];
		CHECK_EQ_INT(hg_read_seq(T[TC].sub, (uint64_t)n, ap, sizeof ap, &ai), HG_MSG);
		CHECK_EQ_INT(row64(row, 40), ai.publish_ns); /* emitted = anchor publish + a wait of 0 */
		CHECK_EQ_INT(row64(row, 8), ai.stamp_ns);
		CHECK(rowf(row, 48) == (float)n);
		/* t.a by provenance: present, the message named. */
		CHECK_EQ_INT(row64(row, 56), (uint64_t)n);
		CHECK_EQ_INT(row32(row, 72), 1);
		CHECK(rowf(row, 80) == (float)n * 0.01f);
		/* t.b: a window of 3, fewer at the start; stale in 11..18; reused at 30. */
		uint32_t bcount = row32(row, 112);
		uint64_t bseq = row64(row, 96);
		uint32_t bflags = row32(row, 116);
		/* b's seqs run on past the skipped tick: seq k is tick k below 30, k + 1 from 30. */
#define BTICK(k) ((float)((k) < 30 ? (k) : (k) + 1))
		CHECK_EQ_INT(bseq, n < 30 ? (uint64_t)n : (uint64_t)n - 1);
		CHECK_EQ_INT(bcount, bseq >= 3 ? 3 : (uint32_t)bseq);
		CHECK(rowf(row, 144) == BTICK(bseq));
		if (bseq >= 2)
			CHECK(rowf(row, 132) == BTICK(bseq - 1));
		if (bseq >= 3)
			CHECK(rowf(row, 120) == BTICK(bseq - 2));
		else
			CHECK(rowf(row, 120) == 0);
		CHECK_EQ_INT((bflags & HG_SLOT_STALE) != 0, n >= 11 && n < 19);
		CHECK_EQ_INT((bflags & HG_DS_PART_REUSED) != 0, n == 30);
		CHECK_EQ_INT(live_jr[n][1].stale, n >= 11 && n < 19);
		/* t.x: referenced every fourth message, missing otherwise. */
		CHECK_EQ_INT((missing >> 2) & 1, n % 4 != 0);
		if (n % 4 == 0) {
			CHECK(rowf(row, 184) == (float)n);
			CHECK_EQ_INT(row64(row, 160), (uint64_t)n / 4);
		} else {
			CHECK_EQ_INT(row64(row, 160), 0);
			CHECK(rowf(row, 184) == 0);
		}
		/* Where: heave >= 0.2 and x present; else masked, never dropped. */
		int where = n % 7 >= 2 && n % 4 == 0;
		CHECK_EQ_INT(live_keep[n], 1);
		CHECK_EQ_INT((flags & HG_DS_ROW_MASKED) != 0, !where);
		/* Reset: the first row; while t.b has been stale for 100 ms (5 ticks
		 * of 20 ms, roughly); and, in the second half, while t.g (an input)
		 * has been missing that long. The ticks at the boundaries depend on
		 * the sleep's jitter and are left out. */
		int reset_expected = n == 1 || (n >= 16 && n < 19) || (n >= 27 && n < 37);
		if ((n < 15 || n > 19) && (n < 25 || n > 26) && n != 37)
			CHECK_EQ_INT((flags & HG_DS_ROW_RESET) != 0, reset_expected);
		CHECK_EQ_INT((flags & HG_DS_ROW_LATE), 0);
		/* t.s (asof, Lateness 0): joined at once in the first half, so a
		 * message published after its stamp is not there yet. */
		int smiss = (missing >> 3) & 1;
		if (n <= 20) {
			uint64_t prev = (uint64_t)((n - 1) / 5) * 5;
			CHECK_EQ_INT(smiss, prev == 0);
			if (prev)
				CHECK(rowf(row, 224) == (float)prev);
		} else {
			CHECK_EQ_INT(smiss, 0);
			CHECK(rowf(row, 224) == (float)((n / 5) * 5));
		}
		/* t.g: 4 slots; the second half was joined after 20 more messages. */
		int gmiss = (missing >> 4) & 1;
		if (n <= 20) {
			CHECK_EQ_INT(gmiss, 0);
			CHECK(rowf(row, 256) == (float)n * 2);
		} else {
			/* 4 slots hold 37..40 once 40 is published. */
			CHECK_EQ_INT(gmiss, n < 37);
		}
	}
}

static int count_flag(unsigned char **rows, int n, uint32_t bit)
{
	int k = 0;
	for (int i = 1; i <= n; i++)
		k += (row32(rows[i], 36) & bit) != 0;
	return k;
}

static void test_chain_offline_matches(void)
{
	struct hgr_reader rd[6];
	for (int i = 0; i < 6; i++) {
		char path[700];
		rfmt(path, sizeof path, "%s/%s.hgr", files, T[i].name);
		CHECK_EQ_INT(hgr_open(&rd[i], path), 0);
	}
	CHECK_EQ_INT(rd[TC].count, TICKS);
	struct hg_ds_source src[5] = { hg_ds_source_file(&rd[TA]), hg_ds_source_file(&rd[TB]), hg_ds_source_file(&rd[TX]),
				       hg_ds_source_file(&rd[TS]), hg_ds_source_file(&rd[TG]) };
	unsigned char *off[TICKS + 1];
	struct hg_ds_state st;
	memset(&st, 0, sizeof st);
	unsigned char scratch[64];
	int late_n = 0, differ_n = 0, miss_differ = 0;
	/* The fixture for python: the rows as hangar dataset build writes them. */
	const char *fx = getenv("HGR_FIXTURE_DIR");
	struct hgr_writer fw;
	int have_fx = 0;
	if (fx && *fx) {
		struct hg_topic_meta m;
		memset(&m, 0, sizeof m);
		rfmt(m.name, sizeof m.name, "dataset.chain");
		rfmt(m.writer, sizeof m.writer, "test_rec_dataset");
		m.topic_id = hg_topic_id(m.name);
		m.type_hash = RT.schema.type.hash;
		m.payload_bytes = RT.row_bytes;
		struct hgr_header h;
		hgr_header_init(&h, &m, RT.schema.type.schema, RT.schema.type.schema_bytes, HGR_FULL, 1, "full", "dstest", 0);
		memset(h.origin_boot, 0, 16);
		char path[700];
		rfmt(path, sizeof path, "%s/dataset.chain.hgr", fx);
		unlink(path);
		have_fx = hgr_create(&fw, path, &h) == 0;
		rfmt(path, sizeof path, "%s/chain.dataset", fx);
		write_file(path, CHAIN_SPEC);
		printf("    writing %s/dataset.chain.hgr\n", fx);
	}
	for (uint64_t i = 0; i < rd[TC].count; i++) {
		struct hg_info info;
		hgr_info(&rd[TC], i, &info);
		int n = (int)info.seq;
		off[n] = calloc(1, RT.row_bytes);
		hg_ds_row_begin(&RT, off[n], &info, hgr_payload_at(&rd[TC], i), boot);
		uint64_t emitted = row64(off[n], 40);
		struct hg_ds_joined jr[5];
		int late = 0;
		for (int p = 0; p < 5; p++) {
			hg_ds_join(&RT, p, off[n], &info, &src[p], scratch, &jr[p]);
			if (!jr[p].missing && jr[p].publish_ns > emitted)
				late = 1;
		}
		CHECK_EQ_INT(hg_ds_row_finish(&RT, &st, off[n], jr, info.gap > 0, info.epoch), 1);
		if (late) {
			uint32_t f = row32(off[n], 36) | HG_DS_ROW_LATE;
			memcpy(off[n] + 36, &f, 4);
			late_n++;
		}
		/* Identical, except where late is set or one of the two marked a
		 * miss (docs/06 "Offline"): here t.s published after the live row
		 * closed (late offline; row 5 even marks it missing live), and t.g
		 * gone from its ring before the live join (missing live alone),
		 * which also moves the reset flag of the rows after it. */
		const unsigned char *live = live_rows[n];
		uint32_t lm = row32(live, 32), om = row32(off[n], 32);
		int same = memcmp(live, off[n], RT.row_bytes) == 0;
		if (!same)
			differ_n++;
		unsigned char a[272], b[272];
		memcpy(a, live, 272);
		memcpy(b, off[n], 272);
		uint32_t fa = row32(a, 36), fb = row32(b, 36);
		if (late) {
			/* They may differ in t.s, its missing bit and the late flag. */
			memset(a + 200, 0, 32);
			memset(b + 200, 0, 32);
			fb &= ~HG_DS_ROW_LATE;
			lm &= ~(1u << 3);
			om &= ~(1u << 3);
		}
		if (lm != om) {
			miss_differ++;
			/* Only t.g, only the live row, only in the second half; and the
			 * reset flag then follows the live row's misses. */
			CHECK_EQ_INT(lm ^ om, 1u << 4);
			CHECK(n > 20 && n < 37);
			CHECK_EQ_INT(om & (1u << 4), 0);
			memset(a + 232, 0, 40);
			memset(b + 232, 0, 40);
			lm &= ~(1u << 4);
			om &= ~(1u << 4);
			fa &= ~HG_DS_ROW_RESET;
			fb &= ~HG_DS_ROW_RESET;
		} else if (n > 20 && n < 42) {
			/* A row after a run of live misses: its reset may differ too. */
			fa &= ~HG_DS_ROW_RESET;
			fb &= ~HG_DS_ROW_RESET;
		}
		memcpy(a + 32, &lm, 4);
		memcpy(b + 32, &om, 4);
		memcpy(a + 36, &fa, 4);
		memcpy(b + 36, &fb, 4);
		if (memcmp(a, b, 272) != 0) {
			printf("    row %d differs beyond what late and the misses explain:", n);
			for (int k = 0; k < 272; k++)
				if (a[k] != b[k])
					printf(" %d(%02x/%02x)", k, a[k], b[k]);
			printf("\n");
		}
		CHECK(memcmp(a, b, 272) == 0);
		if (same)
			CHECK(!late || n > 20);
		CHECK_EQ_INT(row64(live, 40), emitted);
		if (have_fx) {
			struct hg_info ri = { .result = HG_MSG, .seq = i + 1, .stamp_ns = info.stamp_ns, .publish_ns = emitted,
					      .n_inputs = 1 };
			ri.inputs[0].topic_id = rd[TC].h->topic_id;
			ri.inputs[0].seq = info.seq;
			struct hgr_record rec;
			hgr_record_init(&rec, &ri, 0, 0);
			hgr_append(&fw, &rec, off[n]);
		}
	}
	if (have_fx)
		hgr_close(&fw);
	/* Every fifth s was published after its row closed: late offline; in
	 * the first half the live row went without it. */
	CHECK_EQ_INT(late_n, 8);
	CHECK(count_flag(off, TICKS, HG_DS_ROW_LATE) == 8);
	CHECK(differ_n >= 4 + 16);
	CHECK_EQ_INT(miss_differ, 16); /* t.g gone for 21..36 */
	CHECK_EQ_INT(count_flag(off, TICKS, HG_DS_ROW_RESET), 4);
	CHECK(count_flag(live_rows, TICKS, HG_DS_ROW_RESET) > 4);
	for (int n = 1; n <= TICKS; n++)
		free(off[n]);
	for (int i = 0; i < 6; i++)
		hgr_reader_close(&rd[i]);
}

/* Rate=every:N keeps the same anchors whenever the stream is joined from,
 * and Filter=drop removes the rows Where fails. */
static void test_rate_and_drop(void)
{
	struct hg_ds_spec s;
	char err[300];
	static const char R[] = "[Dataset]\nAnchor=t.c\nRate=every:4\nWhere=t.c.heave >= 0.2\nFilter=drop\n[Input t.a]\nJoin=provenance\n";
	CHECK_EQ_INT(hg_ds_parse(R, strlen(R), "r", &s, err, sizeof err), 0);
	static struct hg_ds_row_type rt;
	struct hg_ds_topic_info anchor = ti(&WRENCH_TYPE, "t.c", 50000);
	struct hg_ds_topic_info parts[1] = { ti(&DEPTH_TYPE, "t.a", 50000) };
	CHECK_EQ_INT(hg_ds_bind(&s, &anchor, parts, &rt, err, sizeof err), 0);
	unsigned char *row = calloc(1, rt.row_bytes), scratch[64];
	struct hgr_reader ra, rc;
	char path[700];
	rfmt(path, sizeof path, "%s/t.a.hgr", files);
	CHECK_EQ_INT(hgr_open(&ra, path), 0);
	rfmt(path, sizeof path, "%s/t.c.hgr", files);
	CHECK_EQ_INT(hgr_open(&rc, path), 0);
	struct hg_ds_source src = hg_ds_source_file(&ra);
	struct hg_ds_state st;
	memset(&st, 0, sizeof st);
	int kept = 0, seen = 0;
	const struct hg_field *heave = hg_type_field(&rt.schema.type, "t.c.heave");
	CHECK(heave && heave->offset == 56);
	/* From the start, and from message 7 on: the same anchors are chosen. */
	for (int start = 0; start < 2; start++) {
		memset(&st, 0, sizeof st);
		for (uint64_t i = start ? 6 : 0; i < rc.count; i++) {
			struct hg_info info;
			hgr_info(&rc, i, &info);
			if (!hg_ds_rate_keeps(&rt, info.seq))
				continue;
			CHECK_EQ_INT(info.seq % 4, 0);
			seen++;
			hg_ds_row_begin(&rt, row, &info, hgr_payload_at(&rc, i), boot);
			struct hg_ds_joined jr[1];
			hg_ds_join(&rt, 0, row, &info, &src, scratch, &jr[0]);
			if (hg_ds_row_finish(&rt, &st, row, jr, 0, 0)) {
				kept++;
				CHECK(rowf(row, heave->offset) >= 0.2f);
				CHECK_EQ_INT((row32(row, 36) & HG_DS_ROW_MASKED), 0);
			} else {
				CHECK(rowf(row, heave->offset) < 0.2f);
			}
		}
	}
	CHECK_EQ_INT(seen, 10 + 9);
	CHECK(kept < seen);
	hgr_reader_close(&ra);
	hgr_reader_close(&rc);
	free(row);
}

/* The reset rule by itself: a gap in the anchor and a new epoch. */
static void test_reset_rule(void)
{
	struct hg_ds_state st;
	memset(&st, 0, sizeof st);
	unsigned char *row = calloc(1, RT.row_bytes);
	struct hg_ds_joined jr[5];
	memset(jr, 0, sizeof jr);
	struct hg_info info = { .seq = 1, .stamp_ns = 1000, .publish_ns = 1001 };
	unsigned char payload[64];
	memset(payload, 0, sizeof payload);
	float v = 0.5f;
	memcpy(payload + 8, &v, 4); /* heave */
	struct hg_input ref = { .topic_id = hg_topic_id("t.x"), .seq = 1 };
	info.n_inputs = 1;
	info.inputs[0] = ref;
	hg_ds_row_begin(&RT, row, &info, payload, boot);
	hg_ds_row_finish(&RT, &st, row, jr, 0, 0);
	CHECK(row32(row, 36) & HG_DS_ROW_RESET);
	info.seq = 2;
	hg_ds_row_begin(&RT, row, &info, payload, boot);
	hg_ds_row_finish(&RT, &st, row, jr, 0, 0);
	CHECK(!(row32(row, 36) & HG_DS_ROW_RESET));
	info.seq = 5;
	hg_ds_row_begin(&RT, row, &info, payload, boot);
	hg_ds_row_finish(&RT, &st, row, jr, 1, 0);
	CHECK(row32(row, 36) & HG_DS_ROW_RESET);
	info.seq = 6;
	hg_ds_row_begin(&RT, row, &info, payload, boot);
	hg_ds_row_finish(&RT, &st, row, jr, 0, 1);
	CHECK(row32(row, 36) & HG_DS_ROW_RESET);
	info.seq = 7;
	hg_ds_row_begin(&RT, row, &info, payload, boot);
	hg_ds_row_finish(&RT, &st, row, jr, 0, 1);
	CHECK(!(row32(row, 36) & HG_DS_ROW_RESET));
	/* An input part missing for ResetAfter (100 ms) on anchor stamps; a
	 * label part missing never resets. */
	jr[0].missing = 1;
	for (int k = 0; k < 6; k++) {
		info.seq = 8 + (uint64_t)k;
		info.stamp_ns = 1000 + (uint64_t)k * 20000000;
		hg_ds_row_begin(&RT, row, &info, payload, boot);
		hg_ds_row_finish(&RT, &st, row, jr, 0, 1);
		CHECK_EQ_INT((row32(row, 36) & HG_DS_ROW_RESET) != 0, k >= 5);
	}
	jr[0].missing = 0;
	jr[3].missing = 1; /* t.s is a label */
	info.seq = 20;
	info.stamp_ns = 1000 + 1000000000;
	hg_ds_row_begin(&RT, row, &info, payload, boot);
	hg_ds_row_finish(&RT, &st, row, jr, 0, 1);
	CHECK(!(row32(row, 36) & HG_DS_ROW_RESET));
	/* Missing=drop drops a row with any part missing. */
	struct hg_ds_row_type *rt2 = calloc(1, sizeof *rt2);
	*rt2 = RT;
	rt2->spec.missing_drop = 1;
	uint32_t m = 1u << 3;
	memcpy(row + 32, &m, 4);
	CHECK_EQ_INT(hg_ds_row_finish(rt2, &st, row, jr, 0, 1), 0);
	m = 0;
	memcpy(row + 32, &m, 4);
	CHECK_EQ_INT(hg_ds_row_finish(rt2, &st, row, jr, 0, 1), 1);
	free(rt2);
	free(row);
}

static void test_teardown(void)
{
	for (int i = 0; i < 6; i++) {
		hg_pub_close(T[i].pub);
		hg_sub_close(T[i].sub);
	}
	for (int n = 0; n <= TICKS; n++)
		free(live_rows[n]);
}

int main(void)
{
	rec_setup("dataset");
	rfmt(files, sizeof files, "%s/files", R_root);
	mkdir_p(files, 0755);
	RUN_TEST(test_spec_examples);
	RUN_TEST(test_spec_refusals);
	RUN_TEST(test_row_type);
	RUN_TEST(test_where);
	RUN_TEST(test_chain_live);
	RUN_TEST(test_chain_offline_matches);
	RUN_TEST(test_rate_and_drop);
	RUN_TEST(test_reset_rule);
	RUN_TEST(test_teardown);
	rec_teardown();
	TEST_MAIN_END();
}
