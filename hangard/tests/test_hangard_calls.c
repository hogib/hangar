// SPDX-License-Identifier: GPL-3.0-or-later
// Calls over topics (docs/04) under the manager: a toy server (echo,
// refuse, nap, secret, adminonly) and a toy caller, plus this process as a
// wired module (`tester`, its wiring fetched from the manager) calling in
// process. Round trips with provenance; every status code produced on
// purpose; the server restarting with requests waiting in the callers'
// rings; replies already present skipped; a request and a data topic
// reported in one hg_wait pass; admin calls through `hangar call`, the
// operator's name honoured on the admin topic alone.
#include "hd_test.h"

#include "depth.h"
#include "empty.h"
#include "hg_call.h"

static pid_t mgr;
static char caller_out[600];

static void write_manifests(void)
{
	manifest("talker", "[Module]\nExec=%s/talker --topic test.depth --rate 100\n"
			   "[Publish test.depth]\nType=depth\nRate=100\n", T_toys);
	manifest("server", "[Module]\nExec=%s/server --data test.depth --lazy 30\n"
			   "[Subscribe test.depth]\n"
			   "[Serve echo]\nRequest=depth\nReply=depth\n"
			   "[Serve refuse]\nRequest=depth\nReply=depth\n"
			   "[Serve nap]\nRequest=depth\nReply=depth\nDeadline=50ms\n"
			   "[Serve secret]\nRequest=depth\nReply=depth\nAllow=caller\n"
			   "[Serve adminonly]\nRequest=depth\nReply=depth\nAllow=admin\n"
			   "[Health]\nWatchdog=2s\n", T_toys);
	tfmt(caller_out, sizeof caller_out, "%s/caller.out", T_root);
	manifest("caller", "[Module]\nExec=%s/caller --call server.echo --every 50 --count 3 --out %s --topic caller.depth\n"
			   "[Publish caller.depth]\nType=depth\nRate=50\n"
			   "[Call server.echo]\n[Health]\nWatchdog=2s\n", T_toys, caller_out);
	/* This process plays tester and rawcaller: the manifests give them a
	 * wiring; the program exits at once and is not restarted. */
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Call server.echo]\n[Call server.refuse]\n[Call server.nap]\n"
			   "[Call nobody.ping]\nRequest=depth\nReply=depth\n"
			   "[Subscribe hangar.vehicle.admin.req]\n[Subscribe caller.req]\n");
	manifest("rawcaller", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n[Call server.echo]\n");
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

/* Reads every reply still in server.rep, from the oldest. */
struct rep_scan {
	int n;
	struct hg_call_reply_envelope env[16];
	struct hg_info info[16];
};

static void scan_server_rep(struct rep_scan *out)
{
	memset(out, 0, sizeof *out);
	hg_sub *s = hg_sub_open_from("server.rep", NULL, HG_OLDEST);
	if (!s) {
		printf("    cannot open server.rep: %s\n", strerror(errno));
		return;
	}
	unsigned char buf[HG_CALL_REP_ENVELOPE + 64];
	struct hg_info info;
	while (out->n < 16 && hg_next(s, buf, sizeof buf, &info) == HG_MSG) {
		memcpy(&out->env[out->n], buf, sizeof out->env[0]);
		out->info[out->n] = info;
		out->n++;
	}
	hg_sub_close(s);
}

static const struct hg_call_reply_envelope *find_reply(const struct rep_scan *sc, uint32_t caller_id,
							uint64_t call_id, const struct hg_info **info)
{
	for (int i = 0; i < sc->n; i++)
		if (sc->env[i].caller_id == caller_id && sc->env[i].call_id == call_id) {
			if (info)
				*info = &sc->info[i];
			return &sc->env[i];
		}
	return NULL;
}

static int count_replies(const struct rep_scan *sc, uint32_t caller_id, uint64_t call_id)
{
	int n = 0;
	for (int i = 0; i < sc->n; i++)
		if (sc->env[i].caller_id == caller_id && sc->env[i].call_id == call_id)
			n++;
	return n;
}

static const char *slog_path(void)
{
	static char slog[600];
	tfmt(slog, sizeof slog, "%s/run/hangar/log/server.log", T_root);
	return slog;
}

static int server_status(const char *key)
{
	struct mstat m;
	if (!module_status("server", &m))
		return -1;
	const char *p = strstr(m.status, key);
	return p ? atoi(p + strlen(key)) : -1;
}

/* ---- rawcaller: a child of this process, re-executed, publishing a
 * hand-made request envelope on its own .req topic (what a module could
 * do: the library's checks are the server's, not the caller's). ---- */

static int raw_child(int argc, char **argv)
{
	/* argv: --raw <method> <type-hex> <caller-field> <out-file> <call-id> */
	if (argc < 7)
		return 2;
	const char *method = argv[2], *caller = argv[4], *out = argv[5];
	unsigned long long type = strtoull(argv[3], NULL, 16), call_id = strtoull(argv[6], NULL, 10);
	become("rawcaller");
	static char text[HG_SCHEMA_MAX];
	static struct hg_schema schema;
	int n = hg_call_req_schema("rawcaller", 16, text, sizeof text);
	if (hg_schema_parse(text, (size_t)n, &schema, NULL, 0) < 0)
		return 3;
	schema.type.name = "req";
	hg_pub *p = hg_pub_open("rawcaller.req", &schema.type, NULL);
	hg_sub *rep = hg_sub_open("server.rep", NULL);
	if (!p || !rep) {
		fprintf(stderr, "rawcaller: open: %s\n", strerror(errno));
		return 3;
	}
	struct hg_call_request_envelope env;
	memset(&env, 0, sizeof env);
	env.call_id = call_id;
	env.deadline_ns = hg_now_ns() + 1000000000ull;
	env.request_type = type;
	env.target_id = hg_topic_id("server.rep");
	tfmt(env.method, sizeof env.method, "%s", method);
	tfmt(env.caller, sizeof env.caller, "%s", caller);
	unsigned char payload[HG_CALL_REQ_ENVELOPE + 16];
	memset(payload, 0, sizeof payload);
	memcpy(payload, &env, sizeof env);
	struct depth d = { .depth_m = 7 };
	memcpy(payload + HG_CALL_REQ_ENVELOPE, &d, sizeof d);
	hg_publish_with(p, payload, sizeof payload, hg_now_ns(), HG_NO_REFS, NULL, 0);
	uint64_t deadline = hg_now_ns() + 2000000000ull;
	FILE *f = fopen(out, "w");
	while (hg_now_ns() < deadline) {
		unsigned char buf[HG_CALL_REP_ENVELOPE + 64];
		struct hg_info info;
		if (hg_next(rep, buf, sizeof buf, &info) == HG_MSG) {
			struct hg_call_reply_envelope e;
			memcpy(&e, buf, sizeof e);
			if (e.caller_id == hg_pub_topic_id(p) && e.call_id == call_id) {
				if (f)
					fprintf(f, "%s %s\n", hg_status_name(e.status), e.reason);
				break;
			}
			continue;
		}
		hg_wait(&rep, 1, 200);
	}
	if (f)
		fclose(f);
	hg_pub_close(p);
	return 0;
}

static const char *raw_call(const char *method, unsigned long long type, const char *caller)
{
	static char out[600], buf[200];
	static int raw_calls;
	tfmt(out, sizeof out, "%s/raw.out", T_root);
	unlink(out);
	char hex[32], id[16];
	tfmt(hex, sizeof hex, "%016llx", type);
	tfmt(id, sizeof id, "%d", ++raw_calls);
	pid_t pid = fork();
	if (pid == 0) {
		execl("/proc/self/exe", "test_hangard_calls", "--raw", method, hex, caller, out, id, (char *)NULL);
		_exit(127);
	}
	int rc = wait_pid_ms(pid, 5000);
	if (rc != 0)
		printf("    rawcaller exited %d\n", rc);
	tfmt(buf, sizeof buf, "%s", read_file(out));
	return buf;
}

/* ---- Tests ------------------------------------------------------------- */

static void test_roundtrip_and_provenance(void)
{
	write_manifests();
	mgr = manager_start();
	CHECK(wait_state("server", "ready", 5000));
	CHECK(wait_state("caller", "ready", 5000));
	CHECK(wait_state("tester", "stopped", 5000));
	CHECK(wait_state("rawcaller", "stopped", 5000));
	// The toy caller's three calls answered, the module keeping its rate.
	CHECK(wait_file_contains(caller_out, "call 3 HG_OK", 5000));
	CHECK(!strstr(read_file(caller_out), "HG_E_"));
	CHECK(topic_seq("caller.depth") >= 3);
	// This process as tester: a round trip in process, the reply the request.
	become("tester");
	struct depth d = { .depth_m = 3.5f, .temp_c = 1, .pressure_mbar = 2 }, r;
	memset(&r, 0, sizeof r);
	hg_call_result res = hg_call("server.echo", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	CHECK_EQ_INT(res.status, HG_OK);
	CHECK(r.depth_m == 3.5f && r.pressure_mbar == 2);
	CHECK(res.received_ns != 0 && res.call_id == 1);
	// Every reply references the request it answers (docs/01 "Calls").
	struct rep_scan sc;
	scan_server_rep(&sc);
	CHECK(sc.n >= 4);
	int refd = 0;
	for (int i = 0; i < sc.n; i++) {
		const struct hg_info *in = &sc.info[i];
		if (in->n_inputs == 1 && in->inputs[0].seq > 0 &&
		    (in->inputs[0].topic_id == hg_topic_id("caller.req") || in->inputs[0].topic_id == hg_topic_id("tester.req")))
			refd++;
	}
	CHECK_EQ_INT(refd, sc.n);
	const struct hg_info *info = NULL;
	const struct hg_call_reply_envelope *e = find_reply(&sc, hg_topic_id("tester.req"), 1, &info);
	CHECK(e && e->status == HG_OK && info && info->inputs[0].topic_id == hg_topic_id("tester.req") && info->inputs[0].seq == 1);
	// A request and a data topic ready in one hg_wait pass: the lazy server
	// sleeps 30 ms before each wait, the data topic runs at 100 Hz.
	for (int i = 0; i < 20; i++) {
		res = hg_call("server.echo", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
		CHECK_EQ_INT(res.status, HG_OK);
	}
	sleep_ms(100);
	CHECK(server_status("both ") > 0);
	CHECK(server_status("data ") > 10);
	CHECK(server_status("served ") >= 24);
}

static void test_status_codes(void)
{
	struct depth d = { .depth_m = 9 }, r;
	// HG_E_REFUSED with the handler's reason.
	hg_call_result res = hg_call("server.refuse", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	CHECK_EQ_INT(res.status, HG_E_REFUSED);
	CHECK(strcmp(res.reason, "no: 9") == 0);
	// HG_E_NOROUTE at once: the wiring says nothing serves nobody.ping.
	uint64_t t0 = hg_now_ns();
	res = hg_call("nobody.ping", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	CHECK_EQ_INT(res.status, HG_E_NOROUTE);
	CHECK(hg_now_ns() - t0 < 100000000ull);
	CHECK(file_contains(T_log, "nobody.ping]: no module enabled here serves it"));
	// A call the module did not declare is refused by the library.
	hg_pending *pc;
	CHECK_EQ_INT(hg_call_start("server.secret", &DEPTH_TYPE, &d, &DEPTH_TYPE, &pc, 100), HG_E_NOMETHOD);
	CHECK(errno == EACCES);
	// HG_E_BADTYPE locally: the type is not the wiring's.
	CHECK_EQ_INT(hg_call_start("server.echo", &EMPTY_TYPE, &d, &DEPTH_TYPE, &pc, 100), HG_E_BADTYPE);
	// HG_E_BUSY for the sixteenth call in flight; the fifteen then answered.
	hg_pending *pcs[16];
	int busy = 0, started = 0;
	for (int i = 0; i < 16; i++) {
		int st = hg_call_start("server.echo", &DEPTH_TYPE, &d, &DEPTH_TYPE, &pcs[i], 2000);
		if (st == HG_E_BUSY)
			busy++;
		else if (st == HG_OK)
			started++;
	}
	CHECK_EQ_INT(busy, 1);
	CHECK_EQ_INT(started, 15);
	int done = 0;
	for (int round = 0; round < 200 && done < 15; round++) {
		done = 0;
		for (int i = 0; i < 15; i++) {
			if (!pcs[i])
				continue;
			hg_call_result rr = hg_call_poll(pcs[i], &r);
			if (rr.status == HG_PENDING)
				continue;
			CHECK_EQ_INT(rr.status, HG_OK);
			pcs[i] = NULL;
		}
		for (int i = 0; i < 15; i++)
			done += pcs[i] == NULL;
		if (done < 15)
			sleep_ms(10);
	}
	CHECK_EQ_INT(done, 15);
	// The handler that overruns its deadline is not interrupted; the overrun is logged.
	res = hg_call("server.nap", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	CHECK_EQ_INT(res.status, HG_OK);
	CHECK(wait_file_contains(slog_path(), "nap: handler overran its deadline", 2000));
	// Server side, from a module's hand-made envelopes: HG_E_NOMETHOD,
	// HG_E_BADTYPE, HG_E_DENIED (rawcaller is not in Allow of secret).
	CHECK(strncmp(raw_call("nosuch", DEPTH_TYPE.hash, ""), "HG_E_NOMETHOD no method nosuch", 30) == 0);
	CHECK(strncmp(raw_call("echo", 0x1234, ""), "HG_E_BADTYPE request type 0000000000001234", 42) == 0);
	CHECK(strncmp(raw_call("secret", DEPTH_TYPE.hash, ""), "HG_E_DENIED rawcaller is not in Allow of secret", 46) == 0);
	// The caller field set in a module's own request is ignored: still rawcaller.
	CHECK(strncmp(raw_call("adminonly", DEPTH_TYPE.hash, "admin:root"), "HG_E_DENIED rawcaller is not in Allow", 37) == 0);
	// HG_E_TIMEOUT: the server stopped; then HG_E_EXPIRED published by the
	// restarted server for the request that waited past its deadline.
	CHECK(strncmp(ctl("stop server"), "ok", 2) == 0);
	CHECK(wait_state("server", "stopped", 5000));
	t0 = hg_now_ns();
	hg_pending *late;
	CHECK_EQ_INT(hg_call_start("server.echo", &DEPTH_TYPE, &d, &DEPTH_TYPE, &late, 200), HG_OK);
	uint64_t late_id = 0;
	for (;;) {
		res = hg_call_poll(late, &r);
		if (res.status != HG_PENDING)
			break;
		sleep_ms(10);
	}
	late_id = res.call_id;
	CHECK_EQ_INT(res.status, HG_E_TIMEOUT);
	CHECK(hg_now_ns() - t0 >= 200000000ull && hg_now_ns() - t0 < 1000000000ull);
	CHECK(strncmp(ctl("start server"), "ok", 2) == 0);
	CHECK(wait_state("server", "ready", 5000));
	sleep_ms(300);
	struct rep_scan sc;
	scan_server_rep(&sc);
	const struct hg_call_reply_envelope *e = find_reply(&sc, hg_topic_id("tester.req"), late_id, NULL);
	CHECK(e && e->status == HG_E_EXPIRED);
	CHECK(e && strncmp(e->reason, "expired", 7) == 0);
}

static void test_server_restart(void)
{
	// Requests wait in the caller's ring while the server is down: the ones
	// still in time are served, the others published HG_E_EXPIRED; a second
	// restart skips what is already answered.
	CHECK(strncmp(ctl("stop server"), "ok", 2) == 0);
	CHECK(wait_state("server", "stopped", 5000));
	struct depth d = { .depth_m = 11 }, r;
	hg_pending *longs[2], *shorts[2];
	for (int i = 0; i < 2; i++)
		CHECK_EQ_INT(hg_call_start("server.echo", &DEPTH_TYPE, &d, &DEPTH_TYPE, &longs[i], 5000), HG_OK);
	for (int i = 0; i < 2; i++)
		CHECK_EQ_INT(hg_call_start("server.echo", &DEPTH_TYPE, &d, &DEPTH_TYPE, &shorts[i], 200), HG_OK);
	sleep_ms(400);
	uint64_t short_ids[2], long_ids[2];
	for (int i = 0; i < 2; i++) {
		hg_call_result res = hg_call_poll(shorts[i], &r);
		CHECK_EQ_INT(res.status, HG_E_TIMEOUT);
		short_ids[i] = res.call_id;
	}
	for (int i = 0; i < 2; i++)
		CHECK_EQ_INT(hg_call_poll(longs[i], &r).status, HG_PENDING);
	CHECK(strncmp(ctl("start server"), "ok", 2) == 0);
	CHECK(wait_state("server", "ready", 5000));
	for (int i = 0; i < 2; i++) {
		hg_call_result res;
		for (int k = 0; k < 300; k++) {
			res = hg_call_poll(longs[i], &r);
			if (res.status != HG_PENDING)
				break;
			sleep_ms(10);
		}
		CHECK_EQ_INT(res.status, HG_OK);
		CHECK(r.depth_m == 11);
		long_ids[i] = res.call_id;
	}
	sleep_ms(200);
	struct rep_scan sc;
	scan_server_rep(&sc);
	uint32_t me = hg_topic_id("tester.req");
	for (int i = 0; i < 2; i++) {
		const struct hg_call_reply_envelope *e = find_reply(&sc, me, short_ids[i], NULL);
		CHECK(e && e->status == HG_E_EXPIRED);
		e = find_reply(&sc, me, long_ids[i], NULL);
		CHECK(e && e->status == HG_OK);
	}
	// Restart again: the four requests are still in tester.req, their
	// replies in server.rep; none is run or answered again. (Older requests
	// whose replies have left the 16-slot ring, the raw ones among them, are
	// read again and, expired by now, published HG_E_EXPIRED: docs/04.) The
	// new instance's .rep file holds only what it published.
	CHECK(strncmp(ctl("restart server"), "ok", 2) == 0);
	sleep_ms(100);
	CHECK(wait_state("server", "ready", 5000));
	sleep_ms(400);
	CHECK(file_contains(slog_path(), "replies of the previous instance found in the ring"));
	scan_server_rep(&sc);
	for (int i = 0; i < 2; i++) {
		CHECK_EQ_INT(count_replies(&sc, me, short_ids[i]), 0);
		CHECK_EQ_INT(count_replies(&sc, me, long_ids[i]), 0);
	}
	for (int i = 0; i < sc.n; i++)
		CHECK(sc.env[i].status == HG_E_EXPIRED); /* nothing ran: only expired leftovers */
	CHECK_EQ_INT(server_status("served "), 0);
	// A new call after the restart is answered: the server reads on.
	hg_call_result res = hg_call("server.echo", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	CHECK_EQ_INT(res.status, HG_OK);
	sleep_ms(100);
	CHECK_EQ_INT(server_status("served "), 1);
}

static void test_admin_calls(void)
{
	// hangar call: the manager publishes on hangar.vehicle.admin.req with
	// admin:<user>; the command reads the reply from server.rep.
	static char out[4096];
	int rc = run_cli(out, sizeof out, 5000, "call", "server.echo", "depth_m=2.5", "pressure_mbar=1013", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "HG_OK") != NULL);
	CHECK(strcmp(cli_field(out, "depth_m"), "2.5") == 0);
	CHECK(strcmp(cli_field(out, "pressure_mbar"), "1013") == 0);
	CHECK(topic_seq("hangar.vehicle.admin.req") >= 1);
	// The operator passes where Allow names admin, and nowhere else.
	rc = run_cli(out, sizeof out, 5000, "call", "server.adminonly", "depth_m=1", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(out, "HG_OK") != NULL);
	rc = run_cli(out, sizeof out, 5000, "call", "server.secret", (char *)NULL);
	CHECK_EQ_INT(rc, 1);
	CHECK(strstr(out, "HG_E_DENIED: admin:") != NULL && strstr(out, "is not in Allow of secret"));
	// A method nobody serves is refused by the manager, nothing published.
	uint64_t seq = topic_seq("hangar.vehicle.admin.req");
	rc = run_cli(out, sizeof out, 5000, "call", "server.nosuch", (char *)NULL);
	CHECK_EQ_INT(rc, 1);
	CHECK_EQ_INT((long long)topic_seq("hangar.vehicle.admin.req"), (long long)seq);
	CHECK(file_contains(T_log, "no module serves server.nosuch"));
	rc = run_cli(out, sizeof out, 5000, "call", "nobody.echo", (char *)NULL);
	CHECK_EQ_INT(rc, 1);
	// JSON, and the refused reply's reason.
	rc = run_cli(out, sizeof out, 5000, "call", "server.refuse", "depth_m=4", "--json", (char *)NULL);
	CHECK_EQ_INT(rc, 1);
	CHECK(strstr(out, "\"status\":\"HG_E_REFUSED\"") && strstr(out, "\"reason\":\"no: 4\""));
	// The admin request topic is the manager's: the envelope names the caller.
	hg_sub *a = hg_sub_open_from("hangar.vehicle.admin.req", NULL, HG_OLDEST);
	CHECK(a != NULL);
	if (a) {
		static unsigned char buf[HG_CALL_REQ_ENVELOPE + 16384];
		struct hg_info info;
		int saw_admin = 0;
		while (hg_next(a, buf, sizeof buf, &info) == HG_MSG) {
			struct hg_call_request_envelope env;
			memcpy(&env, buf, sizeof env);
			if (strncmp(env.caller, "admin:", 6) == 0 && env.target_id == hg_topic_id("server.rep"))
				saw_admin++;
		}
		CHECK(saw_admin >= 4);
		hg_sub_close(a);
	}
	// The toy caller's request topic in the recording: decodable, each
	// request naming its method.
	hg_sub *c = hg_sub_open_from("caller.req", NULL, HG_OLDEST);
	CHECK(c != NULL);
	if (c) {
		unsigned char buf[HG_CALL_REQ_ENVELOPE + 16];
		struct hg_info info;
		int n = 0;
		while (hg_next(c, buf, sizeof buf, &info) == HG_MSG) {
			struct hg_call_request_envelope env;
			memcpy(&env, buf, sizeof env);
			CHECK(strcmp(env.method, "echo") == 0 && env.request_type == DEPTH_TYPE.hash && env.caller[0] == 0);
			n++;
		}
		CHECK_EQ_INT(n, 3);
		hg_sub_close(c);
	}
}

static void test_shutdown(void)
{
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--raw") == 0) {
		/* The re-executed child: the environment is the parent's. */
		tfmt(T_control, sizeof T_control, "%s", getenv("HD_CONTROL") ? getenv("HD_CONTROL") : "");
		tfmt(T_root, sizeof T_root, "%s", getenv("HD_ROOT") ? getenv("HD_ROOT") : "");
		return raw_child(argc, argv);
	}
	hd_setup("calls", NULL);
	setenv("HD_CONTROL", T_control, 1);
	setenv("HD_ROOT", T_root, 1);
	RUN_TEST(test_roundtrip_and_provenance);
	RUN_TEST(test_status_codes);
	RUN_TEST(test_server_restart);
	RUN_TEST(test_admin_calls);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
