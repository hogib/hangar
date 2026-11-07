// SPDX-License-Identifier: GPL-3.0-or-later
/* The recorder (docs/06 "The recorder") as a process, wired by a file, with
 * the toy chain publishing and this process as the module `tester` making
 * its calls: sessions started and stopped by call with their directory
 * layout, session.ini, the manifests and types, hangar.clock.hgr, every
 * message of a rated topic kept, every:N and meta applied by the first
 * matching [Record] section, the same seqs kept by every:N in two sessions,
 * the newest message of an event topic at the start of a session, the
 * calls themselves recorded with the reply naming its request; a ring too
 * shallow for the drain warned of and its losses recorded as gaps; the
 * black box dumped on `dump`, on failsafe and on a module's unasked exit,
 * within its memory bound with classes granted in order; AutoStart=armed on
 * safety.state; a mirrored topic's new epoch in a new file; exit 78 on a
 * bad Dir and on a bad recorder.conf. Under 30 s. */
#include "rec_test.h"

#include "depth.h"
#include "empty.h"
#include "hangar_modules.h"
#include "hangar_system.h"
#include "hg_call.h"
#include "recorder_session.h"
#include "recorder_start.h"
#include "recorder_status.h"
#include "safety_state.h"

static const char *R_bin, *R_toys;
static char rec_wiring[700], tester_wiring[700], conf_path[700];
static pid_t rec_pid, chain_pid;

static void write_wirings(void)
{
	rfmt(rec_wiring, sizeof rec_wiring, "%s/recorder.wiring", R_root);
	rfmt(tester_wiring, sizeof tester_wiring, "%s/tester.wiring", R_root);
	char text[4096];
	rfmt(text, sizeof text,
	     "hangar-wiring 1\nmodule recorder\nrole vehicle\n"
	     "subglob *\n"
	     "sub tester.req type=req:tester slots=16 implicit\n"
	     "pub recorder.status type=recorder_status hash=%016llx slots=16\n"
	     "pub recorder.session type=recorder_session hash=%016llx slots=16 sources=tester.req\n"
	     "pub recorder.rep type=rep:recorder slots=16 sources=tester.req implicit\n"
	     "serve start req=recorder_start hash=%016llx size=128 rep=empty hash=%016llx size=8 deadline=200ms allow=tester\n"
	     "serve stop req=empty hash=%016llx size=8 rep=empty hash=%016llx size=8 deadline=200ms allow=tester\n"
	     "serve dump req=empty hash=%016llx size=8 rep=empty hash=%016llx size=8 deadline=200ms allow=tester\n",
	     (unsigned long long)RECORDER_STATUS_TYPE.hash, (unsigned long long)RECORDER_SESSION_TYPE.hash,
	     (unsigned long long)RECORDER_START_TYPE.hash, (unsigned long long)EMPTY_TYPE.hash,
	     (unsigned long long)EMPTY_TYPE.hash, (unsigned long long)EMPTY_TYPE.hash, (unsigned long long)EMPTY_TYPE.hash,
	     (unsigned long long)EMPTY_TYPE.hash);
	CHECK_EQ_INT(write_file(rec_wiring, text), 0);
	rfmt(text, sizeof text,
	     "hangar-wiring 1\nmodule tester\nrole vehicle\n"
	     "pub tester.req type=req:tester slots=16 implicit\n"
	     "sub recorder.rep type=rep:recorder slots=16 implicit\n"
	     "sub recorder.status type=recorder_status hash=%016llx\n"
	     "sub recorder.session type=recorder_session hash=%016llx\n"
	     "pub safety.state type=safety_state hash=%016llx slots=16 rate=10\n"
	     "pub hangar.vehicle.system type=hangar_system hash=%016llx slots=16\n"
	     "pub hangar.vehicle.modules type=hangar_modules hash=%016llx slots=16\n"
	     "mirror mir.t type=any\n"
	     "call recorder.start req=recorder_start hash=%016llx size=128 rep=empty hash=%016llx size=8 remote=0\n"
	     "call recorder.stop req=empty hash=%016llx size=8 rep=empty hash=%016llx size=8 remote=0\n"
	     "call recorder.dump req=empty hash=%016llx size=8 rep=empty hash=%016llx size=8 remote=0\n",
	     (unsigned long long)RECORDER_STATUS_TYPE.hash, (unsigned long long)RECORDER_SESSION_TYPE.hash,
	     (unsigned long long)SAFETY_STATE_TYPE.hash, (unsigned long long)HANGAR_SYSTEM_TYPE.hash,
	     (unsigned long long)HANGAR_MODULES_TYPE.hash, (unsigned long long)RECORDER_START_TYPE.hash,
	     (unsigned long long)EMPTY_TYPE.hash, (unsigned long long)EMPTY_TYPE.hash, (unsigned long long)EMPTY_TYPE.hash,
	     (unsigned long long)EMPTY_TYPE.hash, (unsigned long long)EMPTY_TYPE.hash);
	CHECK_EQ_INT(write_file(tester_wiring, text), 0);
}

static void write_conf(const char *autostart, const char *blackbox_mem, const char *extra)
{
	rfmt(conf_path, sizeof conf_path, "%s/etc/hangar/recorder.conf", R_root);
	char text[2048];
	rfmt(text, sizeof text,
	     "# the suite's recorder.conf\n[Recorder]\nAutoStart=%s\nBlackBox=2s\nBlackBoxMemory=%s\nWriteBuffer=1MB\n"
	     "%s"
	     "[Record chain.b]\nPolicy=meta\n[Record chain.a]\nPolicy=every:2\n[Record chain.*]\nPolicy=full\n",
	     autostart, blackbox_mem, extra ? extra : "");
	CHECK_EQ_INT(write_file(conf_path, text), 0);
}

static pid_t start_recorder(const char *conf)
{
	return spawn("recorder", rec_wiring, R_bin, conf, (char *)NULL);
}

static hg_sub *status_sub;

/* The newest recorder.status, waiting up to timeout for one with the state. */
static int wait_status(int state, long timeout_ms, struct recorder_status *out)
{
	uint64_t t0 = now_ms();
	do {
		struct hg_info info;
		if (hg_latest(status_sub, out, sizeof *out, &info) == HG_MSG && out->state == state)
			return 1;
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

/* The library wants a request even for the 8-byte empty type. */
static hg_call_result call(const char *method, const void *req, const struct hg_type *rt)
{
	struct empty rep, none = { 0 };
	return hg_call(method, rt, req ? req : &none, &EMPTY_TYPE, &rep, 2000);
}

static void test_setup(void)
{
	R_bin = getenv("RECORDER_BIN");
	R_toys = getenv("RECORDER_TOYS");
	CHECK(R_bin && R_toys);
	write_wirings();
	write_conf("manual", "64KB", NULL);
	char toy[700];
	rfmt(toy, sizeof toy, "%s/prov_chain", R_toys);
	chain_pid = spawn("chain", NULL, toy, "--rate", "100", "--slots", "16", "--prefix", "chain", (char *)NULL);
	rec_pid = start_recorder(conf_path);
	setenv("HANGAR_WIRING_FILE", tester_wiring, 1);
	setenv("HANGAR_MODULE", "tester", 1);
	CHECK_EQ_INT(hg_init(), 0);
	status_sub = hg_sub_open("recorder.status", &RECORDER_STATUS_TYPE);
	CHECK(status_sub != NULL);
	struct recorder_status st;
	CHECK(wait_status(0, 5000, &st));
	CHECK_EQ_INT(st.state, 0);
	CHECK(wait_file_contains(log_path("recorder"), "chain.c: full", 3000));
	CHECK(wait_file_contains(log_path("recorder"), "chain.b: meta", 3000));
	CHECK(wait_file_contains(log_path("recorder"), "chain.a: every:2", 3000));
	CHECK(wait_file_contains(log_path("recorder"), "no group hangar-rec", 3000) || getenv("REC_HAVE_GROUP"));
}

static int open_rec(const char *session, const char *name, struct hgr_reader *r)
{
	char path[1024];
	rfmt(path, sizeof path, "%s/%s.hgr", session, name);
	int rc = hgr_open(r, path);
	if (rc < 0)
		printf("    %s: %s\n", path, strerror(errno));
	return rc;
}

static char session1[700];

static void test_session_by_call(void)
{
	struct recorder_start req;
	memset(&req, 0, sizeof req);
	rfmt(req.note, sizeof req.note, "the first session");
	hg_call_result r = call("recorder.start", &req, &RECORDER_START_TYPE);
	if (r.status != HG_OK)
		printf("    start: %s %s\n", hg_status_name(r.status), r.reason);
	CHECK_EQ_INT(r.status, HG_OK);
	struct recorder_status st;
	CHECK(wait_status(1, 2000, &st));
	CHECK(st.session[0] != 0);
	r = call("recorder.start", &req, &RECORDER_START_TYPE);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "already recording") != NULL);
	/* A mirrored topic with two epochs while the session runs. */
	unsigned char bootA[16], bootB[16];
	memset(bootA, 0xa1, 16);
	memset(bootB, 0xb2, 16);
	hg_pub *mir = hg_pub_open_mirror("mir.t", &DEPTH_TYPE, bootA, 1, NULL);
	CHECK(mir != NULL);
	struct depth d = { .depth_m = 1 };
	for (uint64_t s = 1; s <= 3 && mir; s++)
		CHECK_EQ_INT(hg_publish_seq(mir, s, 0, &d, sizeof d, hg_now_ns(), NULL, 0), 0);
	sleep_ms(300);
	if (mir)
		hg_pub_close(mir);
	mir = hg_pub_open_mirror("mir.t", &DEPTH_TYPE, bootB, 1, NULL);
	CHECK(mir != NULL);
	for (uint64_t s = 1; s <= 3 && mir; s++)
		CHECK_EQ_INT(hg_publish_seq(mir, s, HG_SLOT_DECIMATED, &d, sizeof d, hg_now_ns(), NULL, 0), 0);
	sleep_ms(1200);
	CHECK(wait_status(1, 1000, &st));
	CHECK(st.bytes_written > 4096 * 5);
	CHECK(st.topics >= 7);
	r = call("recorder.stop", NULL, &EMPTY_TYPE);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(wait_status(0, 2000, &st));
	r = call("recorder.stop", NULL, &EMPTY_TYPE);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "not recording") != NULL);
	if (mir)
		hg_pub_close(mir);
	sleep_ms(200);

	/* The session directory. */
	rfmt(session1, sizeof session1, "%s", find_session("20"));
	CHECK(session1[0] != 0);
	struct stat sst;
	CHECK(stat(session1, &sst) == 0 && (sst.st_mode & 07777) == 02770);
	char p[1024];
	rfmt(p, sizeof p, "%s/session.ini", session1);
	CHECK(file_contains(p, "Note=the first session"));
	CHECK(file_contains(p, "StartedBy=call:tester"));
	CHECK(file_contains(p, "StoppedBy=call:tester"));
	CHECK(file_contains(p, "WriteErrors=0"));
	CHECK(file_contains(p, "BootId="));
	rfmt(p, sizeof p, "%s/manifests", session1);
	CHECK(exists(p));
	rfmt(p, sizeof p, "%s/msg/depth.msg", session1);
	CHECK(exists(p));
	rfmt(p, sizeof p, "%s/video", session1);
	CHECK(exists(p));
	rfmt(p, sizeof p, "%s/weights", session1);
	CHECK(exists(p));
	struct hgr_reader r1;
	/* hangar.clock.hgr: once a second. */
	CHECK_EQ_INT(open_rec(session1, "hangar.clock", &r1), 0);
	CHECK(r1.count >= 1 && r1.count <= 3);
	CHECK_EQ_INT(r1.payload_bytes, 24);
	CHECK_EQ_INT(r1.h->rate_mhz, 1000);
	hgr_reader_close(&r1);
	/* chain.c, full: every message of the rated topic, in order, no gap. */
	CHECK_EQ_INT(open_rec(session1, "chain.c", &r1), 0);
	CHECK(r1.count >= 120 && r1.count <= 190);
	CHECK_EQ_INT(r1.h->policy, HGR_FULL);
	CHECK(strcmp(r1.h->policy_text, "full") == 0);
	CHECK(strcmp(r1.h->session, strrchr(session1, '/') + 1) == 0);
	CHECK_EQ_INT(r1.h->slot_count, 16);
	CHECK_EQ_INT(r1.h->rate_mhz, 100000);
	CHECK(strcmp(r1.h->writer, "prov_chain") == 0);
	uint64_t first_c = hgr_at(&r1, 0)->seq, first_c_x = 0;
	for (uint64_t i = 0; i < r1.count; i++) {
		const struct hgr_record *rec = hgr_at(&r1, i);
		CHECK_EQ_INT(rec->seq, first_c + i);
		CHECK_EQ_INT(rec->flags & HGR_REC_GAP, 0);
		CHECK(rec->n_inputs >= 2);
		for (uint32_t k = 0; k < rec->n_inputs; k++)
			if (rec->inputs[k].topic_id == hg_topic_id("chain.x") && !first_c_x)
				first_c_x = rec->inputs[k].seq;
	}
	hgr_reader_close(&r1);
	/* chain.a, every:2: the even seqs; chain.b, meta: headers alone. */
	CHECK_EQ_INT(open_rec(session1, "chain.a", &r1), 0);
	CHECK_EQ_INT(r1.h->policy, HGR_EVERY);
	CHECK_EQ_INT(r1.h->policy_n, 2);
	CHECK(r1.count >= 60);
	for (uint64_t i = 0; i < r1.count; i++)
		CHECK_EQ_INT(hgr_at(&r1, i)->seq % 2, 0);
	hgr_reader_close(&r1);
	CHECK_EQ_INT(open_rec(session1, "chain.b", &r1), 0);
	CHECK_EQ_INT(r1.h->policy, HGR_META);
	CHECK_EQ_INT(r1.record_bytes, 128);
	CHECK_EQ_INT(r1.payload_bytes, 40);
	CHECK(r1.count >= 120);
	CHECK(hgr_payload_at(&r1, 0) == NULL);
	hgr_reader_close(&r1);
	/* chain.x, an event topic: its newest message opens the session, so the
	 * first chain.c record's reference to it is in the file. */
	CHECK_EQ_INT(open_rec(session1, "chain.x", &r1), 0);
	CHECK(r1.count >= 2);
	CHECK(first_c_x >= hgr_at(&r1, 0)->seq);
	CHECK_EQ_INT(r1.h->rate_mhz, 0);
	hgr_reader_close(&r1);
	/* The calls: tester.req and recorder.rep, the reply naming its request. */
	struct hgr_reader rq, rp;
	CHECK_EQ_INT(open_rec(session1, "tester.req", &rq), 0);
	CHECK_EQ_INT(open_rec(session1, "recorder.rep", &rp), 0);
	CHECK(rq.count >= 1 && rp.count >= 1);
	int named = 0;
	for (uint64_t i = 0; i < rp.count; i++) {
		const struct hgr_record *rec = hgr_at(&rp, i);
		for (uint32_t k = 0; k < rec->n_inputs; k++)
			if (rec->inputs[k].topic_id == hg_topic_id("tester.req") && hgr_find_seq(&rq, rec->inputs[k].seq) >= 0)
				named++;
	}
	CHECK(named >= 1);
	hgr_reader_close(&rq);
	hgr_reader_close(&rp);
	/* recorder.session itself, with the start referencing the request. */
	CHECK_EQ_INT(open_rec(session1, "recorder.session", &r1), 0);
	CHECK(r1.count >= 1);
	CHECK(hgr_at(&r1, 0)->n_inputs == 1 && hgr_at(&r1, 0)->inputs[0].topic_id == hg_topic_id("tester.req"));
	hgr_reader_close(&r1);
	/* The mirrored topic: two epochs, two files, the second's decimation kept. */
	CHECK_EQ_INT(open_rec(session1, "mir.t", &r1), 0);
	CHECK_EQ_INT(r1.count, 3);
	CHECK_EQ_INT(r1.h->epoch, 0);
	CHECK(r1.h->origin_boot[0] == 0xa1);
	hgr_reader_close(&r1);
	CHECK_EQ_INT(open_rec(session1, "mir.t+1", &r1), 0);
	CHECK_EQ_INT(r1.count, 3);
	CHECK_EQ_INT(r1.h->epoch, 1);
	CHECK(r1.h->origin_boot[0] == 0xb2);
	CHECK_EQ_INT(hgr_at(&r1, 0)->seq, 1);
	CHECK(hgr_at(&r1, 1)->flags & HG_SLOT_DECIMATED);
	CHECK_EQ_INT(hgr_at(&r1, 1)->flags & HGR_REC_GAP, 0);
	hgr_reader_close(&r1);
}

/* A second session: every:2 keeps the even seqs again, whenever it starts;
 * a 2-slot topic at 200 Hz is warned of and its losses recorded as gaps. */
static void test_every_n_and_shallow_ring(void)
{
	char toy[700];
	rfmt(toy, sizeof toy, "%s/prov_chain", R_toys);
	pid_t fast = spawn("fast", NULL, toy, "--rate", "200", "--slots", "2", "--prefix", "fast", (char *)NULL);
	CHECK(wait_file_contains(log_path("recorder"), "fast.c: 2 slots at 200 Hz hold 5 ms, under the 60 ms", 3000));
	struct recorder_start req;
	memset(&req, 0, sizeof req);
	CHECK_EQ_INT(call("recorder.start", &req, &RECORDER_START_TYPE).status, HG_OK);
	sleep_ms(1000);
	struct recorder_status st;
	CHECK(wait_status(1, 1000, &st));
	CHECK(st.gaps > 0);
	CHECK_EQ_INT(call("recorder.stop", NULL, &EMPTY_TYPE).status, HG_OK);
	CHECK(wait_status(0, 2000, &st));
	stop_child(fast);
	sleep_ms(200);
	const char *s2 = find_session("20");
	CHECK(s2[0] && strcmp(s2, session1) != 0);
	struct hgr_reader r;
	CHECK_EQ_INT(open_rec(s2, "chain.a", &r), 0);
	for (uint64_t i = 0; i < r.count; i++)
		CHECK_EQ_INT(hgr_at(&r, i)->seq % 2, 0);
	hgr_reader_close(&r);
	CHECK_EQ_INT(open_rec(s2, "fast.c", &r), 0);
	CHECK(r.count >= 10); /* 2 slots: one message per 50 ms drain */
	uint64_t gaps = 0, gap_records = 0;
	for (uint64_t i = 1; i < r.count; i++) {
		const struct hgr_record *rec = hgr_at(&r, i), *prev = hgr_at(&r, i - 1);
		uint64_t missing = rec->seq - prev->seq - 1;
		if (missing) {
			CHECK(rec->flags & HGR_REC_GAP);
			CHECK_EQ_INT(rec->gap, missing);
			gaps += missing;
			gap_records++;
		} else {
			CHECK_EQ_INT(rec->flags & HGR_REC_GAP, 0);
		}
	}
	CHECK(gap_records > 0);
	CHECK_EQ_INT(gaps + r.count, hgr_at(&r, r.count - 1)->seq - hgr_at(&r, 0)->seq + 1);
	hgr_reader_close(&r);
}

/* The black box: dumped on `dump`, within 64 KB, classes in order. */
static void test_blackbox_dump(void)
{
	struct recorder_status st;
	CHECK(wait_status(0, 1000, &st));
	CHECK(st.blackbox_ns > 0 && st.blackbox_ns < 2500000000ull); /* 2 s asked, less granted */
	int before = count_sessions("blackbox-");
	CHECK_EQ_INT(call("recorder.dump", NULL, &EMPTY_TYPE).status, HG_OK);
	uint64_t t0 = now_ms();
	while (count_sessions("blackbox-") == before && now_ms() - t0 < 3000)
		sleep_ms(50);
	CHECK_EQ_INT(count_sessions("blackbox-"), before + 1);
	sleep_ms(300);
	const char *d = find_session("blackbox-");
	char p[1024];
	rfmt(p, sizeof p, "%s/session.ini", d);
	CHECK(wait_exists(p, 2000));
	CHECK(file_contains(p, "Kind=blackbox"));
	CHECK(file_contains(p, "Reason=call:tester"));
	CHECK(file_contains(p, "[BlackBox]"));
	CHECK(file_contains(p, "chain.c="));
	/* Every file's records within the memory (the budgets asked for far
	 * more than 64 KB), the event topics (class high) whole, the rated ones
	 * (normal) sharing what is left. */
	uint64_t total = 0, c_count = 0, x_count = 0;
	const char *names[] = { "chain.a", "chain.b", "chain.c", "chain.s", "chain.x", "recorder.status" };
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		struct hgr_reader r;
		if (open_rec(d, names[i], &r) < 0) {
			CHECK(!"a black-box file is missing");
			continue;
		}
		CHECK(strcmp(r.h->session, strrchr(d, '/') + 1) == 0);
		for (uint64_t k = 1; k < r.count; k++)
			CHECK(hgr_at(&r, k)->seq > hgr_at(&r, k - 1)->seq);
		if (i == 2)
			c_count = r.count;
		if (i == 4)
			x_count = r.count;
		hgr_reader_close(&r);
	}
	DIR *dd = opendir(d);
	struct dirent *e;
	int files = 0;
	while (dd && (e = readdir(dd))) {
		size_t n = strlen(e->d_name);
		if (n < 5 || strcmp(e->d_name + n - 4, ".hgr") != 0)
			continue;
		struct hgr_reader r;
		char fp[1024];
		rfmt(fp, sizeof fp, "%s/%s", d, e->d_name);
		if (hgr_open(&r, fp) == 0) {
			total += r.count * r.record_bytes;
			files++;
			hgr_reader_close(&r);
		}
	}
	if (dd)
		closedir(dd);
	CHECK(files >= 12);
	CHECK(total <= 65536);
	CHECK(total > 16384); /* the fast topics got shares they never filled: the toy is gone */
	/* chain.x is an event topic: its newest Slots (16) messages, however old. */
	CHECK_EQ_INT(x_count, 16);
	/* chain.c asked for 2 s at 100 Hz = 200 records and got a share. */
	CHECK(c_count > 10 && c_count < 200);
}

/* Failsafe (hangar.vehicle.system) and a module's unasked exit
 * (hangar.vehicle.modules) dump the black box; a dump refused while one
 * was just made (the debounce) is logged. */
static void test_blackbox_triggers(void)
{
	hg_pub *sys = hg_pub_open("hangar.vehicle.system", &HANGAR_SYSTEM_TYPE, NULL);
	hg_pub *mods = hg_pub_open("hangar.vehicle.modules", &HANGAR_MODULES_TYPE, NULL);
	CHECK(sys && mods);
	int before = count_sessions("blackbox-");
	struct hangar_system s;
	memset(&s, 0, sizeof s);
	s.state = 0;
	hg_publish(sys, &s, sizeof s, hg_now_ns());
	sleep_ms(2100); /* past the debounce of the dump by call */
	s.state = 2;
	rfmt(s.reason, sizeof s.reason, "test");
	hg_publish(sys, &s, sizeof s, hg_now_ns());
	uint64_t t0 = now_ms();
	while (count_sessions("blackbox-") == before && now_ms() - t0 < 3000)
		sleep_ms(50);
	CHECK_EQ_INT(count_sessions("blackbox-"), before + 1);
	CHECK(wait_file_contains(log_path("recorder"), "(failsafe)", 2000));
	struct hangar_modules m;
	memset(&m, 0, sizeof m);
	rfmt(m.name, sizeof m.name, "talker");
	m.state = 1;
	m.count = 1;
	hg_publish(mods, &m, sizeof m, hg_now_ns());
	sleep_ms(2100);
	m.state = 3;
	m.exit_known = 1;
	m.last_exit = -9;
	hg_publish(mods, &m, sizeof m, hg_now_ns());
	t0 = now_ms();
	while (count_sessions("blackbox-") == before + 1 && now_ms() - t0 < 3000)
		sleep_ms(50);
	CHECK_EQ_INT(count_sessions("blackbox-"), before + 2);
	CHECK(wait_file_contains(log_path("recorder"), "(module talker exited)", 2000));
	/* A stop by request (stopping, then stopped) is not an exit. */
	m.state = 7;
	hg_publish(mods, &m, sizeof m, hg_now_ns());
	m.state = 5;
	hg_publish(mods, &m, sizeof m, hg_now_ns());
	sleep_ms(300);
	CHECK_EQ_INT(count_sessions("blackbox-"), before + 2);
	hg_pub_close(sys);
	hg_pub_close(mods);
}

/* AutoStart=armed: a session while safety.state says armed. */
static void test_autostart_armed(void)
{
	stop_child(rec_pid);
	write_conf("armed", "256KB", NULL);
	rec_pid = start_recorder(conf_path);
	struct recorder_status st;
	CHECK(wait_status(0, 5000, &st));
	int before = count_sessions("20");
	hg_pub *saf = hg_pub_open("safety.state", &SAFETY_STATE_TYPE, NULL);
	CHECK(saf != NULL);
	struct safety_state ss;
	memset(&ss, 0, sizeof ss);
	ss.state = 0;
	hg_publish(saf, &ss, sizeof ss, hg_now_ns());
	sleep_ms(300);
	CHECK_EQ_INT(count_sessions("20"), before);
	ss.state = 1;
	ss.outputs_enabled = 1;
	hg_publish(saf, &ss, sizeof ss, hg_now_ns());
	CHECK(wait_status(1, 2000, &st));
	CHECK_EQ_INT(count_sessions("20"), before + 1);
	sleep_ms(500);
	ss.state = 0;
	ss.outputs_enabled = 0;
	hg_publish(saf, &ss, sizeof ss, hg_now_ns());
	CHECK(wait_status(0, 2000, &st));
	sleep_ms(200);
	const char *s = find_session("20");
	char p[1024];
	rfmt(p, sizeof p, "%s/session.ini", s);
	CHECK(file_contains(p, "StartedBy=autostart:armed"));
	CHECK(file_contains(p, "StoppedBy=autostart:disarmed"));
	/* safety.state itself was recorded, the armed message first. */
	struct hgr_reader r;
	CHECK_EQ_INT(open_rec(s, "safety.state", &r), 0);
	CHECK(r.count >= 2);
	CHECK_EQ_INT(((const struct safety_state *)hgr_payload_at(&r, 0))->state, 1);
	hgr_reader_close(&r);
	hg_pub_close(saf);
}

/* Exit status 78: a Dir with the wrong mode, and a conf that does not parse. */
static void test_refusals(void)
{
	char bad[700], conf2[700], text[1024];
	rfmt(bad, sizeof bad, "%s/badmode", R_root);
	mkdir_p(bad, 0755);
	rfmt(conf2, sizeof conf2, "%s/bad-dir.conf", R_root);
	rfmt(text, sizeof text, "[Recorder]\nAutoStart=manual\nDir=%s\n", bad);
	write_file(conf2, text);
	pid_t p = spawn("rec-baddir", rec_wiring, R_bin, conf2, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	CHECK(file_contains(log_path("rec-baddir"), "not 2770"));
	rfmt(conf2, sizeof conf2, "%s/bad-key.conf", R_root);
	write_file(conf2, "[Recorder]\nAutoStart=manual\nColour=red\n");
	p = spawn("rec-badkey", rec_wiring, R_bin, conf2, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	CHECK(file_contains(log_path("rec-badkey"), "line 3: unknown key Colour"));
	write_file(conf2, "[Recorder]\nBlackBoxMemory=lots\n");
	p = spawn("rec-badsize", rec_wiring, R_bin, conf2, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	write_file(conf2, "[Record *]\nPolicy=sometimes\n");
	p = spawn("rec-badpolicy", rec_wiring, R_bin, conf2, (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
	p = spawn("rec-noconf", rec_wiring, R_bin, "/nonexistent/recorder.conf", (char *)NULL);
	CHECK_EQ_INT(wait_pid_ms(p, 5000), 78);
}

static void test_teardown(void)
{
	CHECK_EQ_INT(stop_child(rec_pid), 0);
	CHECK(file_contains(log_path("recorder"), "stopped"));
	stop_child(chain_pid);
	hg_sub_close(status_sub);
}

int main(void)
{
	rec_setup("recorder");
	RUN_TEST(test_setup);
	RUN_TEST(test_session_by_call);
	RUN_TEST(test_every_n_and_shallow_ring);
	RUN_TEST(test_blackbox_dump);
	RUN_TEST(test_blackbox_triggers);
	RUN_TEST(test_autostart_armed);
	RUN_TEST(test_refusals);
	RUN_TEST(test_teardown);
	rec_teardown();
	TEST_MAIN_END();
}
