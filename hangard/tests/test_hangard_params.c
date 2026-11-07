// SPDX-License-Identifier: GPL-3.0-or-later
// Parameters (docs/05) under the manager, with the paramer toy
// (depth_hold_params) and this process as the wired module `tester`
// calling set_params in process: the defaults file written at the first
// start; set, stale, range and validator refusals leaving the values in
// force alone; two clients racing (a re-executed child as `tester2`);
// @restart: saved, restart_pending published, further sets refused while
// pending (the module standalone, with no manager to restart it), the new
// instance publishing with the requester's name, the restart not counted as
// a failure; exit 78 on a bad, headerless or empty saved file and the
// manager not restarting it; a missing field taking its default; a request
// queued through a restart applied when in time; `hangar param
// get/set/load/reset`; param-write on a stopped module; every attempt on
// paramer.rep referencing its request.
#include "hd_test.h"

#include "depth_hold_params.h"
#include "hg_call.h"
#include "hg_params.h"
#include "set_params_rep.h"

static pid_t mgr;
static struct hg_schema SP;              /* set_params:depth_hold_params */
static char params_file[600];

struct set_req {
	uint64_t base_version;
	depth_hold_params p;
};
_Static_assert(sizeof(struct set_req) == 56, "set_params request layout");

/* The newest paramer.params message. */
struct params_msg {
	uint64_t version;
	char changed_by[48];
	uint8_t restart_pending;
	uint8_t reserved[7];
	depth_hold_params p;
};
_Static_assert(sizeof(struct params_msg) == 64 + 48, "params topic layout");

static hg_sub *params_sub;

static int read_params(struct params_msg *out)
{
	if (!params_sub)
		params_sub = hg_sub_open("paramer.params", NULL);
	struct hg_info info;
	static struct params_msg last;
	enum hg_result r = hg_latest(params_sub, &last, sizeof last, &info);
	if (r == HG_MSG || (r == HG_NONE && last.version)) {
		*out = last;
		return 0;
	}
	return -1;
}

static int wait_params_version(uint64_t v, long timeout_ms)
{
	uint64_t t0 = now_ms();
	struct params_msg m;
	do {
		if (read_params(&m) == 0 && m.version == v)
			return 1;
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

static void become(const char *module)
{
	char path[600];
	if (wiring_to_file(module, path, sizeof path) < 0) {
		printf("    cannot fetch the wiring of %s\n", module);
		exit(2);
	}
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", module, 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		printf("    hg_init as %s: %s\n", module, hg_init_error());
		exit(2);
	}
}

static void build_types(void)
{
	static char text[HG_SCHEMA_MAX];
	int n = snprintf(text, sizeof text, "u64 base_version\n%.*s", (int)DEPTH_HOLD_PARAMS_TYPE.schema_bytes,
			 DEPTH_HOLD_PARAMS_TYPE.schema);
	if (hg_schema_parse(text, (size_t)n, &SP, NULL, 0) < 0) {
		printf("    cannot build set_params:depth_hold_params\n");
		exit(2);
	}
	SP.type.name = "set_params:depth_hold_params";
}

static hg_call_result set_params(const struct set_req *req, set_params_rep *rep)
{
	return hg_call("paramer.set_params", &SP.type, req, &SET_PARAMS_REP_TYPE, rep, 1000);
}

static const char *paramer_status(void)
{
	static struct mstat m;
	if (!module_status("paramer", &m))
		return "";
	return m.status;
}

static void write_manifests(void)
{
	manifest("paramer", "[Module]\nExec=%s/paramer\n[Params]\nType=depth_hold_params\n[Health]\nWatchdog=2s\n", T_toys);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Call paramer.set_params]\n[Subscribe paramer.params]\n");
	manifest("tester2", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			    "[Call paramer.set_params]\n[Subscribe paramer.params]\n");
	tfmt(params_file, sizeof params_file, "%s/var/lib/hangar/paramer/params", T_root);
}

/* ---- Tests ------------------------------------------------------------- */

static void test_first_start(void)
{
	write_manifests();
	mgr = manager_start();
	CHECK(wait_state("paramer", "ready", 5000));
	CHECK(wait_state("tester", "stopped", 5000));
	CHECK(wait_state("tester2", "stopped", 5000));
	become("tester");
	build_types();
	// The defaults file, written before the first publish.
	const char *f = read_file(params_file);
	CHECK(strncmp(f, "# hangar parameters: depth_hold_params\n# version 1, set by saved, ", 62) == 0);
	CHECK(strstr(f, "\nkp = 1.5\n") && strstr(f, "\nki = 0.1\n") && strstr(f, "\ngains = 1, 1, 0.5\n") &&
	      strstr(f, "\nenabled = 0\n") && strstr(f, "\nframe = \"ned\"\n") && strstr(f, "\nrate_hz = 50\n"));
	struct params_msg m;
	CHECK(wait_params_version(1, 2000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.version == 1 && strcmp(m.changed_by, "saved") == 0 && m.restart_pending == 0);
	CHECK(m.p.kp == 1.5f && m.p.gains[2] == 0.5f && strcmp(m.p.frame, "ned") == 0 && m.p.rate_hz == 50);
	CHECK(strstr(paramer_status(), "params v1 by saved kp=1.5") != NULL);
	// The topic's header schema carries the attributes: a UI builds its controls from it.
	uint32_t slen;
	const char *schema = hg_sub_schema(params_sub, &slen);
	CHECK(schema && strstr(schema, "u64  version") && strstr(schema, "f32  kp = 1.5          [0, 10]"));
}

static void test_set_and_refusals(void)
{
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	struct set_req req = { .base_version = m.version, .p = m.p };
	set_params_rep rep;
	// Accepted: applied whole, version 2, saved, published, changed_by tester.
	req.p.kp = 2.0f;
	req.p.gains[1] = 3;
	hg_call_result r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(rep.version == 2 && rep.restarting == 0);
	CHECK(wait_params_version(2, 2000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 2.0f && m.p.gains[1] == 3 && strcmp(m.changed_by, "tester") == 0 && !m.restart_pending);
	CHECK(wait_file_contains(params_file, "\nkp = 2\n", 1000));
	CHECK(file_contains(params_file, "# version 2, set by tester, "));
	CHECK(file_contains(params_file, "\ngains = 1, 3, 0.5\n"));
	sleep_ms(150);
	CHECK(strstr(paramer_status(), "params v2 by tester kp=2 rate_hz=50 changes=1") != NULL);
	// Stale: based on version 1.
	req.base_version = 1;
	req.p.kp = 2.5f;
	r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strcmp(r.reason, "stale: version is 2") == 0);
	// Out of range.
	req.base_version = 2;
	req.p.kp = 20;
	r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strcmp(r.reason, "kp 20 above 10") == 0);
	req.p.kp = 2.0f;
	req.p.rate_hz = 5;
	r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strcmp(r.reason, "rate_hz 5 below 10") == 0);
	// The validator.
	req.p.rate_hz = 50;
	req.p.enabled = 1;
	req.p.max_thrust = 0;
	r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strcmp(r.reason, "enabled with max_thrust 0") == 0);
	// Nothing changed: version 2, kp 2, in force and in the file.
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.version == 2 && m.p.kp == 2.0f && m.p.enabled == 0);
	CHECK(file_contains(params_file, "# version 2, set by tester, ") && file_contains(params_file, "\nenabled = 0\n"));
	// Every attempt is a request on tester.req and a reply on paramer.rep
	// referencing it.
	hg_sub *rs = hg_sub_open_from("paramer.rep", NULL, HG_OLDEST);
	CHECK(rs != NULL);
	if (rs) {
		unsigned char buf[HG_CALL_REP_ENVELOPE + 16];
		struct hg_info info;
		int n = 0, refd = 0, refused = 0;
		while (hg_next(rs, buf, sizeof buf, &info) == HG_MSG) {
			struct hg_call_reply_envelope e;
			memcpy(&e, buf, sizeof e);
			n++;
			if (info.n_inputs == 1 && info.inputs[0].topic_id == hg_topic_id("tester.req") && info.inputs[0].seq == (uint64_t)n)
				refd++;
			if (e.status == HG_E_REFUSED)
				refused++;
		}
		CHECK_EQ_INT(n, 5);
		CHECK_EQ_INT(refd, 5);
		CHECK_EQ_INT(refused, 4);
		hg_sub_close(rs);
	}
}

/* tester2, re-executed: reads the version, waits until the given monotonic
 * time so that both clients base their change on the same version, sets
 * kp, retries once when stale, and writes its outcomes. */
static int race_child(int argc, char **argv)
{
	if (argc < 4)
		return 2;
	uint64_t go = strtoull(argv[2], NULL, 10);
	const char *out = argv[3];
	become("tester2");
	build_types();
	struct params_msg m;
	if (read_params(&m) < 0)
		return 3;
	while (hg_now_ns() < go)
		sleep_ms(1);
	FILE *f = fopen(out, "w");
	for (int attempt = 0; attempt < 2; attempt++) {
		struct set_req req = { .base_version = m.version, .p = m.p };
		req.p.kp = 4.0f;
		set_params_rep rep;
		hg_call_result r = set_params(&req, &rep);
		if (f)
			fprintf(f, "%s %s version=%llu\n", hg_status_name(r.status), r.reason, (unsigned long long)rep.version);
		if (r.status != HG_E_REFUSED || strncmp(r.reason, "stale", 5) != 0)
			break;
		sleep_ms(50);
		read_params(&m);
	}
	if (f)
		fclose(f);
	return 0;
}

static void test_two_clients_racing(void)
{
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	uint64_t v0 = m.version;
	char out[600], go[32];
	tfmt(out, sizeof out, "%s/race.out", T_root);
	unlink(out);
	uint64_t go_ns = hg_now_ns() + 700000000ull;
	tfmt(go, sizeof go, "%llu", (unsigned long long)go_ns);
	pid_t pid = fork();
	if (pid == 0) {
		execl("/proc/self/exe", "test_hangard_params", "--race", go, out, (char *)NULL);
		_exit(127);
	}
	while (hg_now_ns() < go_ns)
		sleep_ms(1);
	struct set_req req = { .base_version = v0, .p = m.p };
	req.p.kp = 3.0f;
	set_params_rep rep;
	hg_call_result mine = set_params(&req, &rep);
	int rc = wait_pid_ms(pid, 5000);
	CHECK_EQ_INT(rc, 0);
	const char *theirs = read_file(out);
	printf("    tester: %s %s; tester2: %s", hg_status_name(mine.status), mine.reason, theirs);
	int mine_ok = mine.status == HG_OK;
	int theirs_stale_first = strncmp(theirs, "HG_E_REFUSED stale: version is", 30) == 0;
	int theirs_ok_first = strncmp(theirs, "HG_OK", 5) == 0;
	// One wins; the other is refused as stale and its retry succeeds.
	CHECK((mine_ok && theirs_stale_first && strstr(theirs, "\nHG_OK")) ||
	      (!mine_ok && strncmp(mine.reason, "stale", 5) == 0 && theirs_ok_first));
	CHECK(wait_params_version(v0 + (mine_ok && theirs_stale_first ? 2 : 1), 2000));
	if (!mine_ok) {
		CHECK_EQ_INT(read_params(&m), 0);
		req.base_version = m.version;
		req.p = m.p;
		req.p.kp = 3.0f;
		CHECK_EQ_INT(set_params(&req, &rep).status, HG_OK);
		CHECK(wait_params_version(v0 + 2, 2000));
	}
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.version == v0 + 2);
}

static void test_restart_field(void)
{
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	uint64_t v0 = m.version;
	struct mstat st;
	CHECK(module_status("paramer", &st));
	int old_pid = st.pid;
	// A change touching a @restart field: saved, not applied; restart_pending
	// published with the old values; the reply says restarting. The restart
	// replaces the topic's file, so the pending message is read through a
	// subscription opened before the change, which drains the old file first.
	hg_sub *all = hg_sub_open("paramer.params", NULL);
	struct params_msg pm;
	struct hg_info info;
	while (all && hg_next(all, &pm, sizeof pm, &info) == HG_MSG)
		;
	struct set_req req = { .base_version = v0, .p = m.p };
	req.p.rate_hz = 100;
	req.p.kp = 1.0f;
	set_params_rep rep;
	hg_call_result r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(rep.restarting == 1 && rep.version == v0 + 1);
	int saw_pending = 0;
	for (int k = 0; k < 50 && !saw_pending; k++) {
		while (all && hg_next(all, &pm, sizeof pm, &info) == HG_MSG)
			if (pm.restart_pending && pm.version == v0 && pm.p.rate_hz == 50 && pm.p.kp == m.p.kp &&
			    strcmp(pm.changed_by, "tester") == 0)
				saw_pending = 1;
		if (!saw_pending)
			sleep_ms(10);
	}
	hg_sub_close(all);
	CHECK(saw_pending);
	// The manager restarts it (not a failure); the new instance publishes
	// the new values with the requester's name.
	CHECK(wait_new_pid("paramer", old_pid, "ready", 5000));
	CHECK(wait_params_version(v0 + 1, 3000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.rate_hz == 100 && m.p.kp == 1.0f && !m.restart_pending && strcmp(m.changed_by, "tester") == 0);
	CHECK(module_status("paramer", &st) && st.failures == 0 && strcmp(st.state, "ready") == 0);
	CHECK(file_contains(T_log, "paramer: stopping: restart requested by the module"));
	// The mark is cleared: a later start says saved.
	CHECK(!file_contains(params_file, ", restart"));
	CHECK(file_contains(params_file, "\nrate_hz = 100\n"));
	sleep_ms(150);
	CHECK(strstr(paramer_status(), "by tester kp=1 rate_hz=100") != NULL);
}

static void test_refused_while_pending(void)
{
	// The module standalone, with no manager to restart it: the restart stays
	// pending and every further set_params is refused. The earlier requests
	// are still in tester.req, and their replies left paramer.rep with the
	// restarts: a fresh instance would serve again any still in time
	// (docs/04 "While the server restarts"), so their deadlines pass first.
	CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "stopped", 5000));
	sleep_ms(1100);
	char wpath[600], sdir[600];
	CHECK_EQ_INT(wiring_to_file("paramer", wpath, sizeof wpath), 0);
	tfmt(sdir, sizeof sdir, "%s/standalone-state", T_root);
	mkdir_p(sdir);
	char salog[600];
	tfmt(salog, sizeof salog, "%s/standalone.log", T_root);
	pid_t pid = fork();
	if (pid == 0) {
		setenv("HANGAR_WIRING_FILE", wpath, 1);
		setenv("HANGAR_MODULE", "paramer", 1);
		setenv("HANGAR_STATE_DIR", sdir, 1);
		unsetenv("HANGAR_CONTROL");
		char bin[600];
		tfmt(bin, sizeof bin, "%s/paramer", T_toys);
		int fd = open(salog, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0)
			dup2(fd, 2);
		execl(bin, "paramer", (char *)NULL);
		_exit(127);
	}
	int up = wait_params_version(1, 3000); /* its own first start, in its own state dir */
	CHECK(up);
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	if (!up)
		printf("    standalone paramer: version %llu seen; its log: %s\n", (unsigned long long)m.version, read_file(salog));
	CHECK(m.version == 1 && m.p.rate_hz == 50);
	struct set_req req = { .base_version = 1, .p = m.p };
	req.p.rate_hz = 120;
	set_params_rep rep;
	hg_call_result r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_OK);
	if (r.status != HG_OK)
		printf("    set_params: %s\n", r.reason);
	CHECK(rep.restarting == 1);
	CHECK(wait_file_contains(salog, "no manager to restart paramer", 2000));
	req.p.rate_hz = 50;
	req.p.kp = 9;
	r = set_params(&req, &rep);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strcmp(r.reason, "restart pending") == 0);
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.version == 1 && m.restart_pending == 1 && m.p.rate_hz == 50);
	char sfile[700];
	tfmt(sfile, sizeof sfile, "%s/params", sdir);
	CHECK(file_contains(sfile, "# version 2, set by tester, ") && file_contains(sfile, ", restart\n"));
	kill(pid, SIGTERM);
	CHECK_EQ_INT(wait_pid_ms(pid, 3000), 0);
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "ready", 5000));
}

static void test_bad_saved_file(void)
{
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	uint64_t v0 = m.version;
	// An unknown field: exit 78, failed with the reason, not restarted.
	CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "stopped", 5000));
	char text[1024];
	tfmt(text, sizeof text, "# hangar parameters: depth_hold_params\n# version %llu, set by saved, now\nkp = 2\nbogus = 1\n",
	     (unsigned long long)v0);
	write_file(params_file, text);
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "failed", 5000));
	struct mstat st;
	CHECK(module_status("paramer", &st) && strcmp(st.last_exit, "exit:78") == 0);
	CHECK(file_contains(T_log, "refused its configuration: saved parameters refused: line 4: unknown field bogus"));
	sleep_ms(700);
	CHECK(module_status("paramer", &st) && strcmp(st.state, "failed") == 0 && st.pid == 0);
	// A bad value, and a value out of range.
	write_file(params_file, "# hangar parameters: depth_hold_params\nkp = fast\n");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "failed", 5000));
	CHECK(file_contains(T_log, "saved parameters refused: line 2: kp: 'fast' is not a number"));
	write_file(params_file, "# hangar parameters: depth_hold_params\nkp = 11\n");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "failed", 5000));
	CHECK(file_contains(T_log, "saved parameters refused: kp 11 above 10"));
	// The validator, at start.
	write_file(params_file, "# hangar parameters: depth_hold_params\nenabled = 1\nmax_thrust = 0\n");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "failed", 5000));
	CHECK(file_contains(T_log, "saved parameters refused: enabled with max_thrust 0"));
	// No header line, and an empty file: never read as defaults.
	write_file(params_file, "kp = 2\n");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "failed", 5000));
	CHECK(file_contains(T_log, "saved parameters refused: line 1: expected '# hangar parameters: depth_hold_params'"));
	write_file(params_file, "");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "failed", 5000));
	CHECK(file_contains(T_log, "saved parameters refused: empty file"));
	// A missing field takes its default, and the log says so.
	write_file(params_file, "# hangar parameters: depth_hold_params\n# version 7, set by admin:x, now\nkp = 4\n");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "ready", 5000));
	CHECK(wait_params_version(7, 3000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 4 && m.p.ki == 0.1f && m.p.rate_hz == 50 && strcmp(m.changed_by, "saved") == 0);
	char plog[600];
	tfmt(plog, sizeof plog, "%s/run/hangar/log/paramer.log", T_root);
	CHECK(wait_file_contains(plog, "ki is not in the saved file: default taken", 2000));
}

static void test_request_through_restart(void)
{
	// A set_params published while the module restarts: applied by the new
	// instance if in time, published HG_E_EXPIRED otherwise.
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "stopped", 5000));
	struct set_req in_time = { .base_version = m.version, .p = m.p }, late = in_time;
	in_time.p.kp = 5;
	late.p.kp = 6;
	hg_pending *pl, *pi;
	CHECK_EQ_INT(hg_call_start("paramer.set_params", &SP.type, &late, &SET_PARAMS_REP_TYPE, &pl, 150), HG_OK);
	CHECK_EQ_INT(hg_call_start("paramer.set_params", &SP.type, &in_time, &SET_PARAMS_REP_TYPE, &pi, 5000), HG_OK);
	sleep_ms(300);
	set_params_rep rep;
	hg_call_result r = hg_call_poll(pl, &rep);
	CHECK_EQ_INT(r.status, HG_E_TIMEOUT);
	uint64_t late_id = r.call_id;
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "ready", 5000));
	for (int k = 0; k < 300; k++) {
		r = hg_call_poll(pi, &rep);
		if (r.status != HG_PENDING)
			break;
		sleep_ms(10);
	}
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(wait_params_version(m.version + 1, 2000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 5);
	sleep_ms(100);
	hg_sub *rs = hg_sub_open_from("paramer.rep", NULL, HG_OLDEST);
	int saw_expired = 0;
	unsigned char buf[HG_CALL_REP_ENVELOPE + 16];
	struct hg_info info;
	while (rs && hg_next(rs, buf, sizeof buf, &info) == HG_MSG) {
		struct hg_call_reply_envelope e;
		memcpy(&e, buf, sizeof e);
		if (e.call_id == late_id && e.caller_id == hg_topic_id("tester.req") && e.status == HG_E_EXPIRED)
			saw_expired = 1;
	}
	hg_sub_close(rs);
	CHECK(saw_expired);
}

static void test_cli(void)
{
	static char out[8192];
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	uint64_t v0 = m.version;
	// get: from the topic.
	int rc = run_cli(out, sizeof out, 5000, "param", "get", "paramer", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "# paramer.params: version") != NULL);
	CHECK(strcmp(cli_field(out, "kp"), "5") == 0);
	CHECK(strcmp(cli_field(out, "frame"), "\"ned\"") == 0);
	CHECK(strcmp(cli_field(out, "gains"), "[1 1 0.5]") == 0);
	rc = run_cli(out, sizeof out, 5000, "param", "get", "paramer", "rate_hz", "--json", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "\"params\":{\"rate_hz\":50}") != NULL);
	// set: read, modify, set_params as admin:<user>.
	rc = run_cli(out, sizeof out, 5000, "param", "set", "paramer", "kp=1.25", "frame=enu", "gains=2,2,2", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(wait_params_version(v0 + 1, 2000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 1.25f && strcmp(m.p.frame, "enu") == 0 && m.p.gains[2] == 2 && strncmp(m.changed_by, "admin:", 6) == 0);
	rc = run_cli(out, sizeof out, 5000, "param", "set", "paramer", "kp=99", (char *)NULL);
	CHECK_EQ_INT(rc, 1);
	CHECK(strstr(out, "HG_E_REFUSED: kp 99 above 10") != NULL);
	rc = run_cli(out, sizeof out, 5000, "param", "set", "paramer", "nosuch=1", (char *)NULL);
	CHECK_EQ_INT(rc, 1);
	// reset one field, then all.
	rc = run_cli(out, sizeof out, 5000, "param", "reset", "paramer", "kp", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(wait_params_version(v0 + 2, 2000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 1.5f && strcmp(m.p.frame, "enu") == 0);
	rc = run_cli(out, sizeof out, 5000, "param", "reset", "paramer", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(wait_params_version(v0 + 3, 2000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(strcmp(m.p.frame, "ned") == 0 && m.p.gains[2] == 0.5f && m.p.rate_hz == 50);
	// load: a saved file, as one change.
	char lf[600];
	tfmt(lf, sizeof lf, "%s/load.params", T_root);
	write_file(lf, "# hangar parameters: depth_hold_params\n# a file from elsewhere\nkp = 7.5\nki = 0.25\nframe = \"body\"\n");
	rc = run_cli(out, sizeof out, 5000, "param", "load", "paramer", lf, (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(wait_params_version(v0 + 4, 2000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 7.5f && m.p.ki == 0.25f && strcmp(m.p.frame, "body") == 0 && m.p.max_thrust == 0.6f);
	// A module that is not running: param-write, fsync'ed, version plus one,
	// set by admin:<user>; the module starts with it, changed_by saved.
	CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "stopped", 5000));
	rc = run_cli(out, sizeof out, 5000, "param", "set", "paramer", "kp=0.5", "enabled=1", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "not running: saved file written, version") != NULL);
	char needle[64];
	tfmt(needle, sizeof needle, "# version %llu, set by admin:", (unsigned long long)(v0 + 5));
	CHECK(file_contains(params_file, needle));
	CHECK(file_contains(params_file, "\nkp = 0.5\n") && file_contains(params_file, "\nenabled = 1\n") &&
	      file_contains(params_file, "\nframe = \"body\"\n"));
	// Refused by the ranges there too; and get falls back to the saved file.
	rc = run_cli(out, sizeof out, 5000, "param", "set", "paramer", "kp=50", (char *)NULL);
	CHECK_EQ_INT(rc, 1);
	rc = run_cli(out, sizeof out, 5000, "param", "get", "paramer", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "(not running); the saved file:") && strstr(out, "\nkp = 0.5\n"));
	rc = run_cli(out, sizeof out, 5000, "param", "reset", "paramer", "enabled", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(file_contains(params_file, "\nenabled = 0\n"));
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "ready", 5000));
	CHECK(wait_params_version(v0 + 6, 3000));
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 0.5f && m.p.enabled == 0 && strcmp(m.changed_by, "saved") == 0);
	// param-write while running is refused.
	CHECK(strncmp(ctl("param-write paramer\nkp = 1\n"), "err paramer is running", 22) == 0);
}

static void test_shutdown(void)
{
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--race") == 0) {
		tfmt(T_control, sizeof T_control, "%s", getenv("HD_CONTROL") ? getenv("HD_CONTROL") : "");
		tfmt(T_root, sizeof T_root, "%s", getenv("HD_ROOT") ? getenv("HD_ROOT") : "");
		return race_child(argc, argv);
	}
	hd_setup("params", NULL);
	setenv("HD_CONTROL", T_control, 1);
	setenv("HD_ROOT", T_root, 1);
	RUN_TEST(test_first_start);
	RUN_TEST(test_set_and_refusals);
	RUN_TEST(test_two_clients_racing);
	RUN_TEST(test_restart_field);
	RUN_TEST(test_refused_while_pending);
	RUN_TEST(test_bad_saved_file);
	RUN_TEST(test_request_through_restart);
	RUN_TEST(test_cli);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
