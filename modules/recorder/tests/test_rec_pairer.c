// SPDX-License-Identifier: GPL-3.0-or-later
/* The pairer (docs/06 "Live: the pairer") and the offline build (docs/06
 * "Offline") on the same run: the toy chain publishes, the recorder records
 * a session and the pairer publishes dataset.chain_ds and writes its sink
 * file at the same time; then `hangar dataset check` and `build` run over
 * the session, and the live and offline rows are compared: identical
 * wherever the build set no `late` flag, apart from the first row's reset;
 * every row in which the two differ carries `late`. Also pairer.status (rows,
 * misses, latency, slots per part), `hangar dataset schema` agreeing with the
 * row type's hash, `hangar dataset pairs`, and exit status 78 for a missing
 * spec, a bad pairer.conf and a manifest whose dataset outputs differ from
 * the configuration. Under 30 s. */
#include "rec_test.h"

#include "pairer_status.h"

static const char *R_recorder, *R_pairer, *R_hangar, *R_toys;
static pid_t rec_pid, pairer_pid, chain_pid;
static char session[700], sink[700], offline[700], spec_path[700];

static const char SPEC[] =
	"# chain_ds.dataset: the pairer suite's spec over the toy chain\n"
	"[Dataset]\n"
	"Anchor=chain.c\n"
	"AnchorRole=label\n"
	"Fields=surge heave\n"
	"Lateness=20ms\n"
	"Filter=mask\n"
	"ResetAfter=100ms\n"
	"Where=chain.c.heave >= 0.2 and chain.x.depth_m > 0\n"
	"\n"
	"[Input chain.a]\n"
	"Join=provenance\n"
	"\n"
	"[Input chain.b]\n"
	"Join=provenance\n"
	"Window=3\n"
	"Fields=gyro\n"
	"\n"
	"[Input chain.x]\n"
	"Join=provenance\n"
	"Role=filter\n"
	"\n"
	"[Input chain.s]\n"
	"Join=asof:0\n"
	"Role=label\n"
	"Fields=depth_m\n";

/* Runs a command line through /bin/sh, stdout into out; the exit status. */
static int run(char *out, size_t cap, const char *cmd)
{
	FILE *f = popen(cmd, "r");
	if (!f)
		return -1;
	size_t n = 0;
	int c;
	while ((c = fgetc(f)) != EOF && n + 1 < cap)
		out[n++] = (char)c;
	out[n] = 0;
	int st = pclose(f);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
}

static void test_setup(void)
{
	R_recorder = getenv("RECORDER_BIN");
	R_pairer = getenv("PAIRER_BIN");
	R_hangar = getenv("HANGAR_BIN");
	R_toys = getenv("RECORDER_TOYS");
	CHECK(R_recorder && R_pairer && R_hangar && R_toys);
	rfmt(spec_path, sizeof spec_path, "%s/etc/hangar/datasets/chain_ds.dataset", R_root);
	CHECK_EQ_INT(write_file(spec_path, SPEC), 0);
	char conf[700], text[1024];
	rfmt(sink, sizeof sink, "%s/sink", R_root);
	mkdir_p(sink, 0755);
	rfmt(conf, sizeof conf, "%s/etc/hangar/pairer.conf", R_root);
	rfmt(text, sizeof text, "[Pairer]\nDatasets=chain_ds\nDatasetDir=%s/etc/hangar/datasets\nSink=%s\n", R_root, sink);
	CHECK_EQ_INT(write_file(conf, text), 0);
	rfmt(conf, sizeof conf, "%s/etc/hangar/recorder.conf", R_root);
	CHECK_EQ_INT(write_file(conf, "[Recorder]\nAutoStart=boot\nBlackBox=0\n[Record *]\nPolicy=full\n"), 0);
	char toy[700];
	rfmt(toy, sizeof toy, "%s/prov_chain", R_toys);
	/* s stamped 35 ms before it is published: for the anchor three ticks
	 * earlier it is in time by its stamp and late by its publication. */
	chain_pid = spawn("chain", NULL, toy, "--rate", "100", "--slots", "64", "--prefix", "chain", "--s-lag-ms", "35",
			  (char *)NULL);
	rec_pid = spawn("recorder", NULL, R_recorder, conf, (char *)NULL);
	rfmt(conf, sizeof conf, "%s/etc/hangar/pairer.conf", R_root);
	pairer_pid = spawn("pairer", NULL, R_pairer, conf, (char *)NULL);
	char topic[700];
	rfmt(topic, sizeof topic, "%s/dataset.chain_ds", R_topics);
	CHECK(wait_exists(topic, 5000));
	CHECK(wait_file_contains(log_path("pairer"), "chain_ds: 4 parts", 3000));
	CHECK(wait_file_contains(log_path("recorder"), "started (autostart:boot)", 3000));
}

static hg_sub *rows_sub, *status_sub;

static void test_live_rows_and_status(void)
{
	rows_sub = hg_sub_open("dataset.chain_ds", NULL);
	status_sub = hg_sub_open_from("pairer.status", &PAIRER_STATUS_TYPE, HG_OLDEST);
	CHECK(rows_sub && status_sub);
	sleep_ms(2500);
	struct hg_topic_meta m;
	CHECK_EQ_INT(hg_sub_meta(rows_sub, &m), 0);
	CHECK_EQ_INT(m.slot_count, 256);
	CHECK_EQ_INT(m.rate_mhz, 0);
	uint32_t slen;
	const char *schema = hg_sub_schema(rows_sub, &slen);
	CHECK(schema && strstr(schema, "f32 chain.c.heave # role=label"));
	CHECK(schema && strstr(schema, "u64 chain.s._seq # role=label"));
	unsigned char *row = malloc(m.payload_bytes);
	struct hg_info info;
	CHECK_EQ_INT(hg_latest(rows_sub, row, m.payload_bytes, &info), HG_MSG);
	uint64_t aseq;
	memcpy(&aseq, row, 8);
	CHECK(aseq > 100);
	/* Unwired, the row cannot reference its anchor (no Inputs or Sources to
	 * allow it); under a manifest that lists the anchor among the dataset
	 * output's Sources it does (modules/pairer/README.md). */
	CHECK(info.n_inputs == 0 || (info.inputs[0].seq == aseq && info.inputs[0].topic_id == hg_topic_id("chain.c")));
	free(row);
	/* pairer.status: a round of five, the dataset's line and one per part. */
	struct pairer_status st;
	int seen_parts = 0, seen_ds = 0;
	uint64_t x_misses = 0, a_latency = 0;
	uint32_t a_slots = 0, a_needed = 0;
	for (int i = 0; i < 64 && hg_next(status_sub, &st, sizeof st, &info) == HG_MSG; i++) {
		CHECK(strcmp(st.dataset, "chain_ds") == 0);
		CHECK_EQ_INT(st.count, 5);
		if (!st.part[0]) {
			seen_ds++;
			CHECK(st.rows > 0 || i < 10);
		} else {
			seen_parts++;
			if (strcmp(st.part, "chain.x") == 0)
				x_misses = st.misses;
			if (strcmp(st.part, "chain.a") == 0) {
				a_latency = st.latency_ns;
				a_slots = st.slots;
				a_needed = st.needed_slots;
			}
		}
	}
	CHECK(seen_ds >= 1 && seen_parts >= 4);
	CHECK(x_misses > 50); /* three of four anchors do not reference chain.x */
	CHECK(a_latency > 0 && a_latency < 500000000);
	CHECK_EQ_INT(a_slots, 64);
	CHECK(a_needed >= 2 && a_needed <= 64);
}

static void test_stop_and_build(void)
{
	CHECK_EQ_INT(stop_child(pairer_pid), 0);
	CHECK(file_contains(log_path("pairer"), "stopped"));
	CHECK_EQ_INT(stop_child(rec_pid), 0);
	stop_child(chain_pid);
	rfmt(session, sizeof session, "%s", find_session("20"));
	CHECK(session[0] != 0);
	char cmd[2048], out[8192];
	rfmt(cmd, sizeof cmd, "%s dataset check %s %s", R_hangar, spec_path, session);
	int rc = run(out, sizeof out, cmd);
	if (rc != 1)
		printf("    check: rc %d\n%s", rc, out);
	CHECK_EQ_INT(rc, 1); /* WARN: no manifests under this root publish the anchor */
	CHECK(strstr(out, "can be built") != NULL);
	CHECK(strstr(out, "policy: usable") != NULL);
	CHECK(strstr(out, "WARN manifests") != NULL);
	rfmt(offline, sizeof offline, "%s/offline.hgr", R_root);
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o %s", R_hangar, spec_path, session, offline);
	rc = run(out, sizeof out, cmd);
	if (rc != 0)
		printf("    build: rc %d\n%s", rc, out);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, " rows (") != NULL);
	/* The schema the manager would generate, from the session's types. */
	rfmt(cmd, sizeof cmd, "%s dataset schema %s %s", R_hangar, spec_path, session);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 0);
	struct hgr_reader live;
	char sp[800];
	rfmt(sp, sizeof sp, "%s/dataset.chain_ds.hgr", sink);
	CHECK_EQ_INT(hgr_open(&live, sp), 0);
	struct hg_schema parsed;
	uint64_t live_hash = live.h->type_hash;
	CHECK_EQ_INT(hg_schema_parse(out, strlen(out), &parsed, NULL, 0), 0);
	CHECK(parsed.type.hash == live_hash);
	hgr_reader_close(&live);
	/* ... and from the message types by name, with the rates. */
	rfmt(cmd, sizeof cmd,
	     "%s dataset schema %s --msgdir %s/usr/share/hangar/msg --type chain.c=wrench --type chain.a=depth "
	     "--type chain.b=imu --type chain.x=depth --type chain.s=depth --rate chain.c=100 --rate chain.a=100 "
	     "--rate chain.b=100 --rate chain.s=20",
	     R_hangar, spec_path, R_root);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 0);
	CHECK_EQ_INT(hg_schema_parse(out, strlen(out), &parsed, NULL, 0), 0);
	CHECK(parsed.type.hash == live_hash);
	/* A spec with a part the session recorded otherwise is refused, naming it. */
	char spec2[700];
	rfmt(spec2, sizeof spec2, "%s/etc/hangar/datasets/other.dataset", R_root);
	write_file(spec2, "[Dataset]\nAnchor=chain.c\n[Input chain.a]\nJoin=provenance\n[Input nav.none]\nJoin=asof:0\n");
	rfmt(cmd, sizeof cmd, "%s dataset check %s %s 2>&1", R_hangar, spec2, session);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 2);
	CHECK(strstr(out, "FAIL session: part nav.none") != NULL);
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o /dev/null 2>&1", R_hangar, spec2, session);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 2);
	CHECK(strstr(out, "part nav.none") != NULL);
	/* pairs: a spec from a manifest copied into the session. */
	rfmt(sp, sizeof sp, "%s/manifests/chain.module", session);
	write_file(sp, "[Module]\nExec=/bin/true\n[Publish chain.c]\nType=wrench\nInputs=chain.a chain.b\n");
	rfmt(cmd, sizeof cmd, "%s dataset pairs %s chain", R_hangar, session);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 0);
	CHECK(strstr(out, "Anchor=chain.c") && strstr(out, "[Input chain.a]\nJoin=provenance") &&
	      strstr(out, "[Input chain.b]"));
}

static uint32_t r32(const unsigned char *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint64_t r64(const unsigned char *p, uint32_t off)
{
	uint64_t v;
	memcpy(&v, p + off, 8);
	return v;
}

/* Live and offline: identical except `late`, a miss in one row alone, and
 * the first rows' reset flags. */
static void test_live_equals_offline(void)
{
	struct hgr_reader live, off;
	char sp[800];
	rfmt(sp, sizeof sp, "%s/dataset.chain_ds.hgr", sink);
	CHECK_EQ_INT(hgr_open(&live, sp), 0);
	CHECK_EQ_INT(hgr_open(&off, offline), 0);
	CHECK(live.count > 100);
	CHECK(off.count >= live.count);
	CHECK_EQ_INT(live.payload_bytes, off.payload_bytes);
	CHECK(live.h->type_hash == off.h->type_hash);
	CHECK(strcmp(live.h->name, "dataset.chain_ds") == 0 && strcmp(off.h->name, "dataset.chain_ds") == 0);
	uint32_t rb = live.payload_bytes;
	/* The offsets of chain.s in the row, to leave out where late explains a difference. */
	struct hg_schema sch;
	CHECK_EQ_INT(hg_schema_parse(live.h->schema, live.h->schema_bytes, &sch, NULL, 0), 0);
	const struct hg_field *s_seq = hg_type_field(&sch.type, "chain.s._seq");
	const struct hg_field *s_depth = hg_type_field(&sch.type, "chain.s.depth_m");
	CHECK(s_seq && s_depth);
	uint32_t s_from = s_seq->offset, s_to = s_depth->offset + 4;
	int common = 0, late_rows = 0, differ = 0, miss_differ = 0;
	uint64_t first_live = r64(hgr_payload_at(&live, 0), 0), first_off = r64(hgr_payload_at(&off, 0), 0);
	CHECK(r32(hgr_payload_at(&off, 0), 36) & HG_DS_ROW_RESET);
	CHECK(r32(hgr_payload_at(&live, 0), 36) & HG_DS_ROW_RESET);
	unsigned char *a = malloc(rb), *b = malloc(rb);
	for (uint64_t i = 0; i < live.count; i++) {
		const unsigned char *lr = hgr_payload_at(&live, i);
		uint64_t aseq = r64(lr, 0);
		/* The offline rows are one per anchor in order: find this anchor. */
		int64_t j = -1;
		for (uint64_t k = 0; k < off.count; k++)
			if (r64(hgr_payload_at(&off, k), 0) == aseq) {
				j = (int64_t)k;
				break;
			}
		if (j < 0)
			continue;
		/* A window of 3 over the first rows of the recording reaches before
		 * its start: the build has fewer messages there (count says so). */
		if (aseq < first_off + 3)
			continue;
		common++;
		const unsigned char *orow = hgr_payload_at(&off, (uint64_t)j);
		memcpy(a, lr, rb);
		memcpy(b, orow, rb);
		uint32_t fa = r32(a, 36), fb = r32(b, 36), ma = r32(a, 32), mb = r32(b, 32);
		int late = (fb & HG_DS_ROW_LATE) != 0;
		int same = memcmp(a, b, rb) == 0;
		if (!same)
			differ++;
		if (late) {
			late_rows++;
			/* Then the rows may differ in chain.s alone, and its missing bit. */
			memset(a + s_from, 0, s_to - s_from);
			memset(b + s_from, 0, s_to - s_from);
			fb &= ~HG_DS_ROW_LATE;
			ma &= ~(1u << 3);
			mb &= ~(1u << 3);
		}
		if (ma != mb) {
			miss_differ++;
			printf("    anchor %llu: missing live %x offline %x\n", (unsigned long long)aseq, ma, mb);
		}
		if (aseq == first_live || aseq == first_off) {
			fa &= ~HG_DS_ROW_RESET;
			fb &= ~HG_DS_ROW_RESET;
		}
		memcpy(a + 36, &fa, 4);
		memcpy(b + 36, &fb, 4);
		memcpy(a + 32, &ma, 4);
		memcpy(b + 32, &mb, 4);
		if (memcmp(a, b, rb) != 0) {
			printf("    anchor %llu differs beyond late:", (unsigned long long)aseq);
			for (uint32_t k = 0; k < rb; k++)
				if (a[k] != b[k])
					printf(" %u(%02x/%02x)", k, a[k], b[k]);
			printf("\n");
		}
		CHECK(memcmp(a, b, rb) == 0);
		if (!same)
			CHECK(late);
		/* emitted_ns is the same value live and offline. */
		CHECK_EQ_INT(r64(lr, 40), r64(orow, 40));
	}
	printf("    %d common rows, %d late offline, %d differing, %d with a miss in one alone\n", common, late_rows,
	       differ, miss_differ);
	CHECK(common > 100);
	CHECK(late_rows > 0);
	CHECK(differ > 0);
	CHECK_EQ_INT(miss_differ, 0);
	free(a);
	free(b);
	hgr_reader_close(&live);
	hgr_reader_close(&off);
}

/* Exit status 78: a spec that does not exist, a bad pairer.conf, and a
 * manifest (wiring) whose dataset outputs differ from the configuration. */
static void test_refusals(void)
{
	char conf[700];
	rfmt(conf, sizeof conf, "%s/bad1.conf", R_root);
	write_file(conf, "[Pairer]\nDatasets=nothere\n");
	pid_t p = spawn("pairer-nospec", NULL, R_pairer, conf, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	CHECK(file_contains(log_path("pairer-nospec"), "nothere.dataset"));
	write_file(conf, "[Pairer]\nDatasets=chain_ds\nColour=red\n");
	p = spawn("pairer-badkey", NULL, R_pairer, conf, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	CHECK(file_contains(log_path("pairer-badkey"), "unknown key Colour"));
	write_file(conf, "[Pairer]\n");
	p = spawn("pairer-empty", NULL, R_pairer, conf, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	char wiring[700], text[1024];
	rfmt(wiring, sizeof wiring, "%s/pairer.wiring", R_root);
	rfmt(text, sizeof text,
	     "hangar-wiring 1\nmodule pairer\nsub chain.c\nsub chain.a\nsub chain.b\nsub chain.x\nsub chain.s\n"
	     "pub dataset.other type=dataset:other slots=256\npub pairer.status type=pairer_status hash=%016llx slots=256\n",
	     (unsigned long long)PAIRER_STATUS_TYPE.hash);
	write_file(wiring, text);
	rfmt(conf, sizeof conf, "%s/etc/hangar/pairer.conf", R_root);
	p = spawn("pairer-mismatch", wiring, R_pairer, conf, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	CHECK(file_contains(log_path("pairer-mismatch"), "dataset.other, which pairer.conf does not list"));
}

int main(void)
{
	rec_setup("pairer");
	RUN_TEST(test_setup);
	RUN_TEST(test_live_rows_and_status);
	RUN_TEST(test_stop_and_build);
	RUN_TEST(test_live_equals_offline);
	RUN_TEST(test_refusals);
	if (rows_sub)
		hg_sub_close(rows_sub);
	if (status_sub)
		hg_sub_close(status_sub);
	rec_teardown();
	TEST_MAIN_END();
}
