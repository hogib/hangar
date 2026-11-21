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
	CHECK_EQ_INT(write_file(conf, "[Recorder]\nAutoStart=boot\nBlackBox=0\nMinFree=0\n[Record *]\nPolicy=full\n"), 0);
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

/* A recorder and a pairer sleep between their reads: before the fix of
 * 2026-11-19 each spun at a full core on its wait (the recorder on its *
 * glob, read only at each drain; the pairer on the parts of a row, read only
 * by stamp and sequence). Here at the chain's 100 Hz, more than at rest. */
static void check_cpu(const char *name, pid_t pid, double limit)
{
	long long t0 = th_cpu_ticks(pid);
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	sleep_ms(1000);
	clock_gettime(CLOCK_MONOTONIC, &b);
	long ms = (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
	double pct = th_cpu_percent(t0, th_cpu_ticks(pid), ms);
	printf("    %s: %.1f %% of a core\n", name, pct);
	CHECK(pct >= 0 && pct < limit);
}

/* A dataset that cannot bind yet: its anchor is published (a chain of its
 * own, c at 100 Hz) and one of its parts' topics does not exist. The pairer waited on
 * the anchor without ever reading it while unbound, so the wait returned at
 * once, always: 96 % of a core measured, for as long as the topic stayed
 * away (on the vehicle: until the topside is up). The limit is far from both
 * numbers: under 1 % now. */
static void test_unbound_does_not_spin(void)
{
	char path[700], text[1024];
	rfmt(path, sizeof path, "%s/etc/hangar/datasets/waiting.dataset", R_root);
	CHECK_EQ_INT(write_file(path, "[Dataset]\nAnchor=wait.c\nAnchorRole=label\nFields=surge heave\nLateness=0\n\n"
				      "[Input wait.a]\nJoin=provenance\n\n"
				      "[Input never.published]\nJoin=asof:0\n"), 0);
	char toy[700], topic[700];
	rfmt(toy, sizeof toy, "%s/prov_chain", R_toys);
	pid_t chain = spawn("chain-waiting", NULL, toy, "--rate", "100", "--slots", "64", "--prefix", "wait", (char *)NULL);
	rfmt(topic, sizeof topic, "%s/wait.c", R_topics);
	CHECK(chain > 0 && wait_exists(topic, 5000));
	rfmt(path, sizeof path, "%s/etc/hangar/pairer-waiting.conf", R_root);
	rfmt(text, sizeof text, "[Pairer]\nDatasets=waiting\nDatasetDir=%s/etc/hangar/datasets\n", R_root);
	CHECK_EQ_INT(write_file(path, text), 0);
	pid_t pid = spawn("pairer-waiting", NULL, R_pairer, path, (char *)NULL);
	CHECK(pid > 0);
	CHECK(wait_file_contains(log_path("pairer-waiting"), "1 datasets", 5000));
	sleep_ms(300);
	check_cpu("pairer with a dataset whose part is absent, the anchor at 100 Hz", pid, 30.0);
	CHECK_EQ_INT(wait_pid_ms(pid, 0), -1); /* still running, waiting for the topic */
	/* The library's own spin detector said so in the log. */
	CHECK(!file_contains(log_path("pairer-waiting"), "returned as news"));
	CHECK_EQ_INT(stop_child(pid), 0);
	stop_child(chain);
}

static void test_live_rows_and_status(void)
{
	rows_sub = hg_sub_open("dataset.chain_ds", NULL);
	status_sub = hg_sub_open_from("pairer.status", &PAIRER_STATUS_TYPE, HG_OLDEST);
	CHECK(rows_sub && status_sub);
	sleep_ms(1500);
	check_cpu("pairer, rows at 100 Hz", pairer_pid, 25);
	check_cpu("recorder, beside it", rec_pid, 10);
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
	/* Normally the recording holds every anchor the pairer saw and more;
	 * under heavy load either process may have stalled past a 64-slot ring,
	 * so the counts are reported and the common rows are what is compared. */
	printf("    %llu live rows, %llu offline rows\n", (unsigned long long)live.count, (unsigned long long)off.count);
	CHECK(off.count > 100);
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

/* The rows' boot_id is the boot that numbered the anchor: for a local
 * anchor the recording's, which session.ini names (BootId=), not the boot of
 * the machine the build runs on. The comparison above cannot tell the two
 * apart, since the suite builds on the boot that recorded; here the
 * session's BootId is checked against the live rows, then rewritten to
 * another boot's (a session built after a reboot, or on the topside) and
 * built again. Before, every row carried the build machine's boot id. A
 * session with no BootId builds rows with boot_id zero, and says so. */
static void test_build_takes_the_sessions_boot(void)
{
	char ini[800], cmd[2400], out[8192], built[800];
	rfmt(ini, sizeof ini, "%s/session.ini", session);
	static char text[8192];
	rfmt(text, sizeof text, "%s", read_file(ini));
	char *b = strstr(text, "\nBootId=");
	CHECK(b != NULL);
	if (!b)
		return;
	b += 8;
	unsigned char here[16], rec[16], other[16];
	hg_ds_boot_id(here);
	for (int i = 0; i < 16; i++) {
		unsigned v = 0;
		CHECK_EQ_INT(sscanf(b + 2 * i, "%2x", &v), 1);
		rec[i] = (unsigned char)v;
		other[i] = (unsigned char)(0xa0 + i);
	}
	CHECK(memcmp(rec, here, 16) == 0); /* recorded on this boot */
	struct hgr_reader r;
	CHECK_EQ_INT(hgr_open(&r, offline), 0);
	CHECK(r.count > 0 && memcmp((const unsigned char *)hgr_payload_at(&r, 0) + 16, rec, 16) == 0);
	hgr_reader_close(&r);
	/* The same session, as if recorded on another boot. */
	memcpy(b, "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", 32);
	CHECK_EQ_INT(write_file(ini, text), 0);
	rfmt(built, sizeof built, "%s/offline-other.hgr", R_root);
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o %s 2>&1", R_hangar, spec_path, session, built);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 0);
	CHECK(strstr(out, "boot") == NULL); /* nothing to warn of */
	CHECK_EQ_INT(hgr_open(&r, built), 0);
	CHECK(r.count > 100);
	int wrong = 0;
	for (uint64_t i = 0; i < r.count; i++)
		wrong += memcmp((const unsigned char *)hgr_payload_at(&r, i) + 16, other, 16) != 0;
	CHECK_EQ_INT(wrong, 0);
	hgr_reader_close(&r);
	/* No BootId in session.ini: zero, with a warning; the build still succeeds. */
	memcpy(b - 7, "Xoot", 4);
	CHECK_EQ_INT(write_file(ini, text), 0);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 0);
	CHECK(strstr(out, "names no BootId") != NULL);
	CHECK_EQ_INT(hgr_open(&r, built), 0);
	wrong = 0;
	for (uint64_t i = 0; i < r.count; i++)
		for (int k = 0; k < 16; k++)
			wrong += ((const unsigned char *)hgr_payload_at(&r, i))[16 + k] != 0;
	CHECK_EQ_INT(wrong, 0);
	hgr_reader_close(&r);
	memcpy(b - 7, "Boot", 4);
	memcpy(b, "", 0);
	for (int i = 0; i < 16; i++)
		rfmt(cmd, sizeof cmd, "%02x", rec[i]), memcpy(b + 2 * i, cmd, 2);
	CHECK_EQ_INT(write_file(ini, text), 0);
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

/* `hangar dataset pairs` on the longest Inputs line a manifest can hold
 * (1023 bytes, the ini reader's bound, which is also the size of the
 * buffer pairs keeps the list in): every name is there, the last one too.
 * One byte more is refused by the reader with its line, not cut. */
static void test_pairs_long_inputs(void)
{
	char dir[700], mp[800], cmd[1700];
	static char text[4096], out[32768];
	rfmt(dir, sizeof dir, "%s/long-session/manifests", R_root);
	mkdir_p(dir, 0755);
	rfmt(mp, sizeof mp, "%s/wide.module", dir);
	for (int extra = 0; extra < 2; extra++) {
		size_t n = (size_t)rfmt(text, sizeof text, "[Module]\nExec=/bin/true\n[Publish wide.out]\nType=wrench\nInputs=");
		size_t start = n;
		int names = 0;
		for (; n - start + 10 <= 1023 - 6; names++)
			n += (size_t)rfmt(text + n, sizeof text - n, "in.t%04d ", names);
		while (n - start < 1023 - 6)
			text[n++] = ' ';
		n += (size_t)rfmt(text + n, sizeof text - n, "z.last%s\n", extra ? "x" : "");
		CHECK_EQ_INT(n - start - 1, 1023 + (size_t)extra);
		CHECK_EQ_INT(write_file(mp, text), 0);
		rfmt(cmd, sizeof cmd, "%s dataset pairs %s/long-session wide 2>&1", R_hangar, R_root);
		int rc = run(out, sizeof out, cmd);
		if (!extra) {
			CHECK_EQ_INT(rc, 0);
			CHECK(strstr(out, "[Input in.t0000]\nJoin=provenance") != NULL);
			CHECK(strstr(out, "[Input z.last]\nJoin=provenance") != NULL);
			int inputs = 0;
			for (const char *p = out; (p = strstr(p, "[Input ")); p += 7)
				inputs++;
			CHECK_EQ_INT(inputs, names + 1);
		} else {
			CHECK_EQ_INT(rc, 2);
			CHECK(strstr(out, "line 5: value too long") != NULL);
		}
	}
}

/* A one-topic session in <root>/<name>: t.a.hgr with three records of an
 * 8-byte payload, the header's schema, hash, policy and policy text as
 * given (a damaged or hand-made file). */
static void bad_session(char *dir, size_t len, const char *name, const char *schema, uint64_t hash,
			enum hgr_policy policy, const char *policy_text_fill)
{
	rfmt(dir, len, "%s/%s", R_root, name);
	mkdir_p(dir, 0755);
	char path[800];
	rfmt(path, sizeof path, "%s/t.a.hgr", dir);
	unlink(path);
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	rfmt(m.name, sizeof m.name, "t.a");
	m.payload_bytes = 8;
	m.type_hash = hash;
	m.topic_id = hg_topic_id("t.a");
	m.rate_mhz = 10000;
	struct hgr_header h;
	hgr_header_init(&h, &m, schema, (uint32_t)strlen(schema), policy, policy == HGR_EVERY ? 2 : 1, "full", "s", 0);
	if (policy_text_fill) {
		memset(h.policy_text, policy_text_fill[0], sizeof h.policy_text); /* no NUL in the field */
		memset(h.session, policy_text_fill[0], sizeof h.session);
	}
	struct hgr_writer w;
	CHECK_EQ_INT(hgr_create(&w, path, &h), 0);
	for (uint64_t seq = 1; seq <= 3; seq++) {
		struct hg_info info = { .seq = seq, .stamp_ns = seq * 100000000ull, .publish_ns = seq * 100000000ull };
		struct hgr_record r;
		hgr_record_init(&r, &info, 0, 0);
		CHECK_EQ_INT(hgr_append(&w, &r, &seq), 0);
	}
	hgr_close(&w);
	if (policy_text_fill) {
		/* ... and none in the reserved bytes between it and the schema. */
		char fill[512 - 280];
		memset(fill, policy_text_fill[0], sizeof fill);
		int fd = open(path, O_WRONLY);
		CHECK(pwrite(fd, fill, sizeof fill, 280) == (ssize_t)sizeof fill);
		close(fd);
	}
}

/* `hangar dataset` copies fields at the schema's offsets out of records
 * sized by the header's payload_bytes, so a header whose schema describes
 * another type than its payload_bytes and type_hash say must be refused
 * when the file is opened: with an 8-byte payload and a 4 MB field in the
 * schema, the build was killed by SIGSEGV. The header's text fields are
 * printed bounded: a damaged one need not hold a NUL. */
static void test_damaged_recordings(void)
{
	char spec[700], dir[700], cmd[2400];
	static char out[8192];
	rfmt(spec, sizeof spec, "%s/bad.dataset", R_root);
	write_file(spec, "[Dataset]\nAnchor=t.a\nAnchorRole=input\n");
	struct hg_schema sc;
	const char big[] = "u64 x\nu8 big[4194304]\n", good[] = "u64 x\n";
	CHECK_EQ_INT(hg_schema_parse(big, sizeof big - 1, &sc, NULL, 0), 0);
	/* The schema says 4 MB, the header 8 bytes. */
	bad_session(dir, sizeof dir, "bad-size", big, sc.type.hash, HGR_FULL, NULL);
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o %s/bad.hgr 2>&1", R_hangar, spec, dir, R_root);
	int rc = run(out, sizeof out, cmd);
	printf("    a 4 MB schema over an 8-byte payload: exit %d: %.160s", rc, out[0] ? out : "\n");
	CHECK_EQ_INT(rc, 2);
	CHECK(strstr(out, "t.a.hgr") && strstr(out, "payload"));
	rfmt(cmd, sizeof cmd, "%s dataset check %s %s 2>&1", R_hangar, spec, dir);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 2);
	CHECK(strstr(out, "FAIL session: anchor t.a") != NULL);
	/* The right size, another type's hash. */
	CHECK_EQ_INT(hg_schema_parse(good, sizeof good - 1, &sc, NULL, 0), 0);
	bad_session(dir, sizeof dir, "bad-hash", good, sc.type.hash ^ 1, HGR_FULL, NULL);
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o %s/bad.hgr 2>&1", R_hangar, spec, dir, R_root);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 2);
	CHECK(strstr(out, "type hash") != NULL);
	/* A sound file builds. */
	bad_session(dir, sizeof dir, "sound", good, sc.type.hash, HGR_FULL, NULL);
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o %s/bad.hgr 2>&1", R_hangar, spec, dir, R_root);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 0);
	CHECK(strstr(out, "3 rows") != NULL);
	/* Text fields without a NUL: the refusal prints policy_text, 32 bytes,
	 * and not what follows it in the header up to the schema's end. */
	bad_session(dir, sizeof dir, "bad-text", good, sc.type.hash, HGR_EVERY, "P");
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o %s/bad.hgr 2>&1", R_hangar, spec, dir, R_root);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 2);
	CHECK(strstr(out, "PPPP") != NULL && strstr(out, "not full") != NULL);
	CHECK(strstr(out, "u64 x") == NULL);
	rfmt(cmd, sizeof cmd, "%s dataset check %s %s 2>&1", R_hangar, spec, dir);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 2);
	CHECK(strstr(out, "PPPP") != NULL && strstr(out, "u64 x") == NULL);
	/* ... and a session name without one, copied into the built file's header. */
	bad_session(dir, sizeof dir, "bad-name", good, sc.type.hash, HGR_FULL, "S");
	rfmt(cmd, sizeof cmd, "%s dataset build %s %s -o %s/bad.hgr 2>&1", R_hangar, spec, dir, R_root);
	CHECK_EQ_INT(run(out, sizeof out, cmd), 0);
	char built[800];
	rfmt(built, sizeof built, "%s/bad.hgr", R_root);
	struct hgr_reader r;
	CHECK_EQ_INT(hgr_open(&r, built), 0);
	CHECK_EQ_INT(strlen(r.h->session), 63);
	hgr_reader_close(&r);
}

/* A part whose writer restarts with another type while the pairer runs: the
 * row type bound at start no longer describes what the part's file holds.
 * The pairer says so, drops the rows that waited and binds again: the
 * dataset topic is replaced by one of the new row type, and the sink goes
 * on in dataset.<name>+1.hgr. A writer that restarts with the same type (the
 * anchor here) changes nothing. Before, the binding was for the life of the
 * process and the new type's bytes were copied into rows by the old type's
 * offsets, with nothing said. */
static void test_part_retyped(void)
{
	char path[700], text[1024], dir[700];
	rfmt(path, sizeof path, "%s/etc/hangar/datasets/retyped.dataset", R_root);
	CHECK_EQ_INT(write_file(path, "[Dataset]\nAnchor=rt.a\nAnchorRole=input\nLateness=0\n\n"
				      "[Input rt.p]\nJoin=asof:0\n"), 0);
	rfmt(dir, sizeof dir, "%s/sink-retyped", R_root);
	mkdir_p(dir, 0755);
	rfmt(path, sizeof path, "%s/etc/hangar/pairer-retyped.conf", R_root);
	rfmt(text, sizeof text, "[Pairer]\nDatasets=retyped\nDatasetDir=%s/etc/hangar/datasets\nSink=%s\n", R_root, dir);
	CHECK_EQ_INT(write_file(path, text), 0);
	pid_t pid = spawn("pairer-retyped", NULL, R_pairer, path, (char *)NULL);
	CHECK(pid > 0);
	CHECK(wait_file_contains(log_path("pairer-retyped"), "1 datasets", 5000));
	char toy[700];
	rfmt(toy, sizeof toy, "%s/retype", R_toys);
	/* The part first, so that every anchor message finds one: 0.6 s of each type. */
	pid_t part = spawn("retype-part", NULL, toy, "--topic", "rt.p", "--first", "u64 v", "--then", "f64 x\\nf64 y",
			   "--count", "6", "--gap-ms", "100", (char *)NULL);
	sleep_ms(50);
	pid_t anchor = spawn("retype-anchor", NULL, toy, "--topic", "rt.a", "--first", "u64 v", "--then", "u64 v", "--count",
			     "12", "--gap-ms", "50", (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(part, 5000), 0);
	CHECK_EQ_INT(wait_pid_ms(anchor, 5000), 0);
	CHECK(wait_file_contains(log_path("pairer-retyped"), "retyped: 1 parts, 88-byte rows", 5000));
	CHECK(wait_file_contains(log_path("pairer-retyped"), "retyped: the writer of rt.p restarted with another type", 5000));
	CHECK_EQ_INT(wait_pid_ms(pid, 0), -1); /* still running */
	CHECK(!file_contains(log_path("pairer-retyped"), "the writer of rt.a restarted"));
	CHECK(file_contains(log_path("pairer-retyped"), "retyped: 1 parts, 96-byte rows"));
	/* The topic is of the row type bound now. */
	hg_sub *rows = hg_sub_open("dataset.retyped", NULL);
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	unsigned char row[96];
	struct hg_info info;
	CHECK(rows != NULL);
	CHECK_EQ_INT(hg_latest(rows, row, sizeof row, &info), HG_MSG);
	CHECK_EQ_INT(hg_sub_meta(rows, &m), 0);
	CHECK_EQ_INT(m.payload_bytes, 96);
	uint32_t slen = 0;
	const char *schema = hg_sub_schema(rows, &slen);
	CHECK(schema && strstr(schema, "f64 rt.p.y"));
	/* The last row: anchor message 24, the part's last message, 12, whose
	 * last byte the toy sets to 13: byte 7 of y, at the end of the row. */
	CHECK_EQ_INT(r64(row, 0), 24);
	CHECK_EQ_INT(r32(row, 32), 0);
	CHECK_EQ_INT(row[48], 24);
	CHECK_EQ_INT(r64(row, 56), 12);
	CHECK_EQ_INT(row[80], 12);
	CHECK_EQ_INT(row[95], 13);
	if (rows)
		hg_sub_close(rows);
	CHECK_EQ_INT(stop_child(pid), 0);
	/* The sink: the rows of the first row type in one file, of the second in the next. */
	struct hgr_reader r;
	rfmt(path, sizeof path, "%s/dataset.retyped.hgr", dir);
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.payload_bytes, 88);
	CHECK(r.count >= 8 && r.count <= 12);
	for (uint64_t i = 0; i < r.count; i++) {
		const unsigned char *lr = hgr_payload_at(&r, i);
		/* v of the part, as the old type has it: a message of the first
		 * six, the toy's k in its first byte and k + 1 in its last. */
		CHECK(r32(lr, 32) != 0 || (r64(lr, 56) >= 1 && r64(lr, 56) <= 6 && lr[80] == r64(lr, 56) && lr[87] == lr[80] + 1));
	}
	hgr_reader_close(&r);
	rfmt(path, sizeof path, "%s/dataset.retyped+1.hgr", dir);
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.payload_bytes, 96);
	CHECK(r.count >= 8 && r.count <= 13);
	if (r.count)
		CHECK(r32(hgr_payload_at(&r, 0), 36) & HG_DS_ROW_RESET);
	for (uint64_t i = 0; i < r.count; i++) {
		const unsigned char *lr = hgr_payload_at(&r, i);
		CHECK_EQ_INT(r32(lr, 32), 0);
		CHECK(r64(lr, 56) >= 7 && r64(lr, 56) <= 12);
		CHECK_EQ_INT(lr[80], r64(lr, 56));
		CHECK_EQ_INT(lr[95], r64(lr, 56) + 1);
	}
	hgr_reader_close(&r);
}

/* More anchors waiting than Pending holds: the oldest row is closed before
 * its emitted_ns, with what has arrived, so it may differ from the build's
 * row with no flag in either. The pairer counts such rows in pairer.status
 * (closed_early) and logs them, at most once a second and at exit; and it
 * refuses to start (78) on a spec whose wait at the anchor's declared rate
 * needs more than Pending, naming the number. Before, the count was kept
 * and shown nowhere. */
static void test_pending_overflow(void)
{
	char path[700], text[1024];
	/* Rows wait 600 ms (asof:+500ms and the default Lateness); the anchor is
	 * an event topic, so nothing is known at bind. */
	rfmt(path, sizeof path, "%s/etc/hangar/datasets/crowded.dataset", R_root);
	CHECK_EQ_INT(write_file(path, "[Dataset]\nAnchor=crowd.a\nAnchorRole=input\n\n"
				      "[Input ahead]\nTopic=crowd.a\nJoin=asof:+500ms\nRole=label\n"), 0);
	rfmt(path, sizeof path, "%s/etc/hangar/pairer-crowded.conf", R_root);
	rfmt(text, sizeof text, "[Pairer]\nDatasets=crowded\nDatasetDir=%s/etc/hangar/datasets\nPending=4\n", R_root);
	CHECK_EQ_INT(write_file(path, text), 0);
	pid_t pid = spawn("pairer-crowded", NULL, R_pairer, path, (char *)NULL);
	CHECK(wait_file_contains(log_path("pairer-crowded"), "1 datasets", 5000));
	char toy[700];
	rfmt(toy, sizeof toy, "%s/retype", R_toys);
	/* 40 anchors 10 ms apart: ten times what Pending=4 holds for 600 ms. */
	pid_t pub = spawn("crowd", NULL, toy, "--topic", "crowd.a", "--first", "u64 v", "--then", "u64 v", "--count", "20",
			  "--gap-ms", "10", (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(pub, 5000), 0);
	hg_sub *st = hg_sub_open_from("pairer.status", &PAIRER_STATUS_TYPE, HG_OLDEST);
	CHECK(st != NULL);
	struct pairer_status ps;
	struct hg_info info;
	uint64_t early = 0, rows = 0, t0 = now_ms();
	while (now_ms() - t0 < 3000 && rows < 40) {
		while (hg_next(st, &ps, sizeof ps, &info) == HG_MSG)
			if (strcmp(ps.dataset, "crowded") == 0 && !ps.part[0]) {
				early = ps.closed_early;
				rows = ps.rows;
			}
		sleep_ms(50);
	}
	CHECK_EQ_INT(rows, 40);
	CHECK_EQ_INT(early, 36); /* all but the four that were still waiting when the anchors stopped */
	CHECK(wait_file_contains(log_path("pairer-crowded"), "crowded: 36 rows closed before their emitted_ns", 2000) ||
	      file_contains(log_path("pairer-crowded"), "rows closed before their emitted_ns"));
	CHECK(file_contains(log_path("pairer-crowded"), "Pending=4"));
	CHECK_EQ_INT(stop_child(pid), 0);
	CHECK(file_contains(log_path("pairer-crowded"), "crowded: 36 rows in all were closed before their emitted_ns"));
	if (st)
		hg_sub_close(st);
	/* The same wait on an anchor with a declared rate: known at bind, refused. */
	rfmt(path, sizeof path, "%s/etc/hangar/datasets/crowded2.dataset", R_root);
	CHECK_EQ_INT(write_file(path, "[Dataset]\nAnchor=crowd.r\nAnchorRole=input\n\n"
				      "[Input ahead]\nTopic=crowd.r\nJoin=asof:+500ms\nRole=label\n"), 0);
	rfmt(path, sizeof path, "%s/etc/hangar/pairer-crowded2.conf", R_root);
	rfmt(text, sizeof text, "[Pairer]\nDatasets=crowded2\nDatasetDir=%s/etc/hangar/datasets\nPending=4\n", R_root);
	CHECK_EQ_INT(write_file(path, text), 0);
	pub = spawn("crowd-rated", NULL, toy, "--topic", "crowd.r", "--first", "u64 v", "--then", "u64 v", "--count", "100",
		    "--gap-ms", "20", "--rate", "50", (char *)NULL);
	pid = spawn("pairer-crowded2", NULL, R_pairer, path, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(pid, 5000), 78);
	/* 600 ms at 50 Hz is 30 rows, and two for the pairer's own latency. */
	CHECK(file_contains(log_path("pairer-crowded2"), "Pending=4"));
	CHECK(file_contains(log_path("pairer-crowded2"), "Pending=32"));
	stop_child(pub);
}

/* A mirrored anchor whose sender rebooted: a new epoch, numbered by another
 * boot. The rows of each epoch carry that epoch's boot id, and the first row
 * of the new one carries reset. Before, the boot id was taken once, when the
 * dataset was bound, and every later row carried the first sender's. */
static void test_anchor_epoch_boot(void)
{
	char path[700], text[1024], dir[700];
	rfmt(path, sizeof path, "%s/etc/hangar/datasets/epochs.dataset", R_root);
	CHECK_EQ_INT(write_file(path, "[Dataset]\nAnchor=mir.a\nAnchorRole=input\n"), 0);
	rfmt(dir, sizeof dir, "%s/sink-epochs", R_root);
	mkdir_p(dir, 0755);
	rfmt(path, sizeof path, "%s/etc/hangar/pairer-epochs.conf", R_root);
	rfmt(text, sizeof text, "[Pairer]\nDatasets=epochs\nDatasetDir=%s/etc/hangar/datasets\nSink=%s\n", R_root, dir);
	CHECK_EQ_INT(write_file(path, text), 0);
	unsigned char boot_a[16], boot_b[16];
	memset(boot_a, 0xa1, 16);
	memset(boot_b, 0xb2, 16);
	static struct hg_schema sch;
	CHECK_EQ_INT(hg_schema_parse("u64 v\n", 6, &sch, NULL, 0), 0);
	hg_pub *mir = hg_pub_open_mirror("mir.a", &sch.type, boot_a, 1, NULL);
	CHECK(mir != NULL);
	if (!mir)
		return;
	pid_t pid = spawn("pairer-epochs", NULL, R_pairer, path, (char *)NULL);
	CHECK(wait_file_contains(log_path("pairer-epochs"), "epochs: 0 parts", 5000));
	for (uint64_t s = 1; s <= 3; s++) {
		CHECK_EQ_INT(hg_publish_seq(mir, s, 0, &s, sizeof s, hg_now_ns(), NULL, 0), 0);
		sleep_ms(50);
	}
	sleep_ms(200);
	hg_pub_close(mir);
	mir = hg_pub_open_mirror("mir.a", &sch.type, boot_b, 1, NULL);
	CHECK(mir != NULL);
	for (uint64_t s = 1; s <= 3 && mir; s++) {
		CHECK_EQ_INT(hg_publish_seq(mir, s, 0, &s, sizeof s, hg_now_ns(), NULL, 0), 0);
		sleep_ms(50);
	}
	sleep_ms(300);
	CHECK_EQ_INT(stop_child(pid), 0);
	if (mir)
		hg_pub_close(mir);
	struct hgr_reader r;
	rfmt(path, sizeof path, "%s/dataset.epochs.hgr", dir);
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.count, 6);
	for (uint64_t i = 0; i < r.count && i < 6; i++) {
		const unsigned char *row = hgr_payload_at(&r, i);
		CHECK_EQ_INT(r64(row, 0), i % 3 + 1);
		CHECK(memcmp(row + 16, i < 3 ? boot_a : boot_b, 16) == 0);
		CHECK_EQ_INT((r32(row, 36) & HG_DS_ROW_RESET) != 0, i % 3 == 0);
	}
	hgr_reader_close(&r);
}

int main(void)
{
	rec_setup("pairer");
	RUN_TEST(test_setup);
	RUN_TEST(test_live_rows_and_status);
	RUN_TEST(test_stop_and_build);
	RUN_TEST(test_live_equals_offline);
	RUN_TEST(test_build_takes_the_sessions_boot);
	RUN_TEST(test_refusals);
	RUN_TEST(test_pairs_long_inputs);
	RUN_TEST(test_damaged_recordings);
	RUN_TEST(test_part_retyped);
	RUN_TEST(test_pending_overflow);
	RUN_TEST(test_anchor_epoch_boot);
	RUN_TEST(test_unbound_does_not_spin); /* last: its pairer publishes pairer.status too */
	if (rows_sub)
		hg_sub_close(rows_sub);
	if (status_sub)
		hg_sub_close(status_sub);
	rec_teardown();
	TEST_MAIN_END();
}
