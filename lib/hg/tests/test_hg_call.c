// SPDX-License-Identifier: LGPL-3.0-or-later
// Calls and parameters without a manager (docs/04, docs/05): one process is
// the module `self`, wired by a file, and calls its own methods (start,
// serve, poll: hg_call would wait on itself). Round trips with provenance;
// every status code; eight requests per hg_serve pass; the caller token of
// a local, a mirrored and the admin topics (children re-executed as the
// module `hangar`); hg_call refused in an actuator module; the parameter
// grammar; hg_params_open, set_params through the library, @restart with
// no manager, the restarted instance publishing the requester's name, and
// exit status 78 on a bad saved file.
#include "hg_test.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "hg_call.h"
#include "hg_params.h"

static const char DH_SCHEMA[] =
	"# dh_params\n"
	"f32  kp = 1.5          [0, 10]   # N/m\n"
	"f32  ki = 0.1          [0, 2]    # N/(m s)\n"
	"f32  max_thrust = 0.6  [0, 1]    # fraction of full thrust\n"
	"f32  gains[3] = 1, 1, 0.5        # one value applies to every element\n"
	"u8   enabled = 0       [0, 1]\n"
	"char frame[16] = \"ned\"\n"
	"u32  rate_hz = 50      [10, 200] @restart\n"
	"i16  trim = -3         [-10, 10]\n";

struct dh {
	float kp, ki, max_thrust, gains[3];
	uint8_t enabled;
	char frame[16];
	uint8_t pad[3];
	uint32_t rate_hz;
	int16_t trim;
	uint8_t pad2[6];
};

static struct hg_schema dh_parsed, sp_parsed, spr_parsed, req_parsed, rep_parsed, adm_parsed;
static const struct hg_type *DH, *SP;
static char wiring_self[600], wiring_hangar[600], wiring_pilot[600], wiring_act[600], state_dir[600];
static char self_dir[600];

/* The parent writes the wiring files once; a re-executed child runs build()
 * only for the paths and the strings, since rewriting a file another child
 * is reading at that moment truncates it under the reader. */
static int child_process;

static void write_text(const char *path, const char *text)
{
	if (child_process)
		return;
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

static void build(void)
{
	char text[HG_SCHEMA_MAX];
	int n = snprintf(text, sizeof text, "%s", DH_SCHEMA);
	if (hg_schema_parse(text, (size_t)n, &dh_parsed, NULL, 0) < 0)
		exit(2);
	dh_parsed.type.name = "dh_params";
	DH = &dh_parsed.type;
	n = snprintf(text, sizeof text, "u64 base_version\n%s", DH_SCHEMA);
	if (hg_schema_parse(text, (size_t)n, &sp_parsed, NULL, 0) < 0)
		exit(2);
	sp_parsed.type.name = "set_params:dh_params";
	SP = &sp_parsed.type;
	if (hg_schema_parse("u64 version\nu8 restarting\n", 26, &spr_parsed, NULL, 0) < 0)
		exit(2);
	spr_parsed.type.name = "set_params_rep";
	if (spr_parsed.type.hash != 0xd748add7256bbe70ull)
		exit(2); /* msg/set_params_rep.msg's hash, which the manager generates too */
	/* req:self: the largest request among the calls is set_params' (8 + 64);
	 * rep:self: the largest reply among the methods served, 16 (the 256-byte
	 * words reply is on a call line: the caller's side of wrongrep). */
	n = hg_call_req_schema("self", SP->payload_bytes, text, sizeof text);
	if (hg_schema_parse(text, (size_t)n, &req_parsed, NULL, 0) < 0)
		exit(2);
	n = hg_call_rep_schema("self", 16, text, sizeof text);
	if (hg_schema_parse(text, (size_t)n, &rep_parsed, NULL, 0) < 0)
		exit(2);
	n = hg_call_req_schema("hangar", 16384, text, sizeof text);
	if (hg_schema_parse(text, (size_t)n, &adm_parsed, NULL, 0) < 0)
		exit(2);
	adm_parsed.type.name = "req:hangar";
	unsigned long long d = DEPTH_TYPE->hash, w = WORDS_TYPE->hash, sp = SP->hash, rq = req_parsed.type.hash,
			   rp = rep_parsed.type.hash, sr = 0xd748add7256bbe70ull /* set_params_rep */;
	static char body[8192];
	n = snprintf(body, sizeof body,
		     "hangar-wiring 1\nmodule self\nrole vehicle\n"
		     "sub self.req type=req:self hash=%016llx slots=16 implicit\n"
		     "sub self.rep type=rep:self hash=%016llx slots=16 implicit\n"
		     "sub ghost.rep slots=16 implicit\n"
		     "sub hangar.vehicle.admin.req type=req:hangar slots=16 implicit\n"
		     "sub hangar.topside.admin.req type=req:hangar slots=16 implicit\n"
		     "sub pilot.req slots=16\n"
		     "pub self.req type=req:self hash=%016llx slots=16 implicit\n"
		     "pub self.rep type=rep:self hash=%016llx slots=16 sources=self.req,hangar.vehicle.admin.req,hangar.topside.admin.req,pilot.req implicit\n"
		     "pub self.params type=params:self slots=16 sources=self.req implicit\n"
		     "%s"
		     "serve echo req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 deadline=100ms allow=self,admin\n"
		     "serve denied req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 deadline=100ms allow=someone\n"
		     "serve nohandler req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 deadline=100ms allow=*\n"
		     "serve wrongrep req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 deadline=100ms allow=*\n"
		     "serve topside req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 deadline=100ms allow=topside:*,topside:admin\n"
		     "serve set_params req=set_params:dh_params hash=%016llx size=%u rep=set_params_rep hash=%016llx size=16 deadline=100ms allow=*,admin\n"
		     "call self.echo req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 remote=0\n"
		     "call self.denied req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 remote=0\n"
		     "call self.nohandler req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 remote=0\n"
		     "call self.wrongrep req=depth hash=%016llx size=16 rep=words hash=%016llx size=256 remote=0\n"
		     "call self.set_params req=set_params:dh_params hash=%016llx size=%u rep=set_params_rep hash=%016llx size=16 remote=0\n"
		     "call ghost.echo req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 remote=0\n"
		     "call nobody.ping req=depth hash=%016llx size=16 rep=depth hash=%016llx size=16 remote=0 noroute=1\n",
		     rq, rp, rq, rp, "%s", d, d, d, d, d, d, d, d, d, d, sp, SP->payload_bytes, sr, d, d, d, d, d, d, d, w, sp,
		     SP->payload_bytes, sr, d, d, d, d);
	(void)n;
	static char full[8192];
	snprintf(full, sizeof full, body, "");
	snprintf(wiring_self, sizeof wiring_self, "%s/self.wiring", topic_dir);
	write_text(wiring_self, full);
	char act[200];
	snprintf(act, sizeof act, "pub act.out type=depth hash=%016llx slots=16 actuator\n", d);
	snprintf(full, sizeof full, body, act);
	snprintf(wiring_act, sizeof wiring_act, "%s/act.wiring", topic_dir);
	write_text(wiring_act, full);
	/* The manager's own request topics, local and mirrored from the topside;
	 * and a topside module's request topic, mirrored (its header's writer
	 * names the module, docs/01 "Mirrored topics": here the module itself
	 * creates it, standing in for the bridge). */
	snprintf(full, sizeof full,
		 "hangar-wiring 1\nmodule hangar\nrole vehicle\n"
		 "pub hangar.vehicle.admin.req type=req:hangar hash=%016llx slots=16\n"
		 "mirror hangar.topside.admin.req type=any\n",
		 (unsigned long long)adm_parsed.type.hash);
	snprintf(wiring_hangar, sizeof wiring_hangar, "%s/hangar.wiring", topic_dir);
	write_text(wiring_hangar, full);
	snprintf(wiring_pilot, sizeof wiring_pilot, "%s/pilot.wiring", topic_dir);
	write_text(wiring_pilot, "hangar-wiring 1\nmodule pilot\nrole topside\nmirror pilot.req type=any\n");
	snprintf(state_dir, sizeof state_dir, "%s/state", topic_dir);
	mkdir(state_dir, 0755);
	snprintf(self_dir, sizeof self_dir, "%s", topic_dir);
}

static void become_self(void)
{
	setenv("HANGAR_WIRING_FILE", wiring_self, 1);
	setenv("HANGAR_STATE_DIR", state_dir, 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		printf("    hg_init: %s\n", hg_init_error());
		exit(2);
	}
}

/* Handlers. */
static char last_caller[HG_CALL_CALLER];
static int echo_calls;

static int on_echo(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)reason;
	snprintf(last_caller, sizeof last_caller, "%s", c->caller);
	memcpy(rep, req, sizeof(struct depth));
	echo_calls++;
	return HG_OK;
}

static int on_refuse(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	(void)rep;
	snprintf(reason, HG_CALL_REASON, "not today");
	return HG_E_REFUSED;
}

/* start + serve + poll: the module answers itself. */
static hg_call_result self_call(const char *target, const struct hg_type *rq, const void *req,
				const struct hg_type *rp, void *rep, int timeout_ms, int *start_status)
{
	hg_pending *pc;
	hg_call_result r;
	memset(&r, 0, sizeof r);
	int st = hg_call_start(target, rq, req, rp, &pc, timeout_ms);
	if (start_status)
		*start_status = st;
	if (st != HG_OK) {
		r.status = st;
		return r;
	}
	for (int i = 0; i < 100; i++) {
		hg_serve();
		r = hg_call_poll(pc, rep);
		if (r.status != HG_PENDING)
			return r;
		sleep_ms(2);
	}
	return r;
}

/* ---- Tests ------------------------------------------------------------- */

static void test_tokens(void)
{
	CHECK(strcmp(hg_status_name(HG_E_NOROUTE), "HG_E_NOROUTE") == 0 && strcmp(hg_status_name(HG_OK), "HG_OK") == 0);
	const char allow[6][HG_WIRING_NAME] = { "safety", "topside:pilot_console", "*", "topside:*", "admin", "topside:admin" };
	// A token matches only its own form (docs/04 "Allow tokens").
	CHECK(hg_call_allowed("safety", allow, 1));
	CHECK(!hg_call_allowed("topside:safety", allow, 1));
	CHECK(!hg_call_allowed("safety", allow + 1, 1) && hg_call_allowed("topside:pilot_console", allow + 1, 1));
	CHECK(hg_call_allowed("anything", allow + 2, 1) && !hg_call_allowed("topside:anything", allow + 2, 1) &&
	      !hg_call_allowed("admin:x", allow + 2, 1));
	CHECK(hg_call_allowed("topside:nav", allow + 3, 1) && !hg_call_allowed("nav", allow + 3, 1) &&
	      !hg_call_allowed("topside:admin:x", allow + 3, 1) && !hg_call_allowed("vehicle:nav", allow + 3, 1));
	CHECK(hg_call_allowed("admin:oguz", allow + 4, 1) && !hg_call_allowed("topside:admin:oguz", allow + 4, 1) &&
	      !hg_call_allowed("admin", allow + 4, 1));
	CHECK(hg_call_allowed("topside:admin:oguz", allow + 5, 1) && !hg_call_allowed("admin:oguz", allow + 5, 1) &&
	      !hg_call_allowed("topside:admin", allow + 5, 1));
	CHECK(!hg_call_allowed("safety", allow, 0));
	// The generated types: envelope sizes, the payload the largest body.
	CHECK_EQ_INT(req_parsed.type.payload_bytes, 112 + SP->payload_bytes);
	CHECK_EQ_INT(rep_parsed.type.payload_bytes, 96 + 16);
	CHECK_EQ_INT(adm_parsed.type.payload_bytes, 112 + 16384);
	CHECK(req_parsed.fields[7].offset == 112 && strcmp(req_parsed.fields[7].name, "request") == 0);
}

static void test_roundtrip(void)
{
	become_self();
	CHECK_EQ_INT(hg_serve_method("echo", on_echo, (void *)0x1), 0);
	CHECK_EQ_INT(hg_serve_method("echo", on_echo, NULL), -1); /* EEXIST */
	CHECK(errno == EEXIST);
	CHECK_EQ_INT(hg_serve_method("nosuch", on_echo, NULL), -1);
	CHECK(errno == EACCES);
	CHECK_EQ_INT(hg_serve_method("wrongrep", on_echo, NULL), 0);
	CHECK_EQ_INT(hg_serve_method("denied", on_echo, NULL), 0);
	CHECK_EQ_INT(hg_serve_method("topside", on_echo, NULL), 0);
	struct depth d = { .depth_m = 2.5f, .temp_c = 3, .pressure_mbar = 4 }, r = { 0 };
	int st;
	hg_call_result res = self_call("self.echo", DEPTH_TYPE, &d, DEPTH_TYPE, &r, 500, &st);
	CHECK_EQ_INT(res.status, HG_OK);
	CHECK(r.depth_m == 2.5f && r.pressure_mbar == 4 && res.call_id == 1 && res.received_ns > 0);
	CHECK(strcmp(last_caller, "self") == 0);
	// The reply references the request (own output read back).
	hg_sub *rep = hg_sub_open_own("self.rep", NULL, HG_OLDEST);
	CHECK(rep != NULL);
	unsigned char buf[96 + 256];
	struct hg_info info;
	CHECK_EQ_INT(hg_next(rep, buf, sizeof buf, &info), HG_MSG);
	struct hg_call_reply_envelope env;
	memcpy(&env, buf, sizeof env);
	CHECK(env.call_id == 1 && env.caller_id == hg_topic_id("self.req") && env.status == HG_OK &&
	      env.reply_type == DEPTH_TYPE->hash);
	CHECK(info.n_inputs == 1 && info.inputs[0].topic_id == hg_topic_id("self.req") && info.inputs[0].seq == 1);
	hg_sub_close(rep);
	// The subscriptions the library holds, for the module's wait set.
	hg_sub *subs[16];
	int n = hg_call_subs(subs, 16);
	CHECK_EQ_INT(n, 6); /* self.req, hangar.vehicle.admin.req, hangar.topside.admin.req, pilot.req; self.rep, ghost.rep (none for nobody) */
	CHECK(hg_call_req_type() && hg_call_req_type()->hash == req_parsed.type.hash);
	CHECK(hg_call_rep_type() && hg_call_rep_type()->hash == rep_parsed.type.hash);
}

static void test_status_codes(void)
{
	struct depth d = { .depth_m = 1 }, r;
	int st;
	hg_pending *pc;
	// NOROUTE, undeclared, local BADTYPE.
	CHECK_EQ_INT(hg_call_start("nobody.ping", DEPTH_TYPE, &d, DEPTH_TYPE, &pc, 100), HG_E_NOROUTE);
	CHECK_EQ_INT(hg_call_start("self.secret", DEPTH_TYPE, &d, DEPTH_TYPE, &pc, 100), HG_E_NOMETHOD);
	CHECK(errno == EACCES);
	CHECK_EQ_INT(hg_call_start("self.echo", WORDS_TYPE, &d, DEPTH_TYPE, &pc, 100), HG_E_BADTYPE);
	CHECK_EQ_INT(hg_call_start("self.echo", DEPTH_TYPE, &d, WORDS_TYPE, &pc, 100), HG_E_BADTYPE);
	// DENIED: self is not in Allow of denied.
	hg_call_result res = self_call("self.denied", DEPTH_TYPE, &d, DEPTH_TYPE, &r, 500, &st);
	CHECK_EQ_INT(res.status, HG_E_DENIED);
	CHECK(strcmp(res.reason, "self is not in Allow of denied") == 0);
	// NOMETHOD: a method served by the wiring but with no handler registered.
	res = self_call("self.nohandler", DEPTH_TYPE, &d, DEPTH_TYPE, &r, 500, &st);
	CHECK_EQ_INT(res.status, HG_E_NOMETHOD);
	// Caller-side BADTYPE: the reply type differs from the caller's.
	uint64_t words[32];
	res = self_call("self.wrongrep", DEPTH_TYPE, &d, WORDS_TYPE, words, 500, &st);
	CHECK_EQ_INT(res.status, HG_E_BADTYPE);
	// TIMEOUT: ghost never answers (its .rep topic is absent).
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_call_start("ghost.echo", DEPTH_TYPE, &d, DEPTH_TYPE, &pc, 50), HG_OK);
	for (;;) {
		res = hg_call_poll(pc, &r);
		if (res.status != HG_PENDING)
			break;
		sleep_ms(5);
	}
	CHECK_EQ_INT(res.status, HG_E_TIMEOUT);
	CHECK(ms_since(t0) >= 50 && ms_since(t0) < 500);
	CHECK_EQ_INT(hg_call_poll(pc, &r).status, HG_E_TIMEOUT); /* no longer in flight */
	// BUSY: the sixteenth; then 8 per pass, the rest left in the ring.
	hg_pending *pcs[16];
	int busy = 0;
	for (int i = 0; i < 16; i++)
		if (hg_call_start("self.echo", DEPTH_TYPE, &d, DEPTH_TYPE, &pcs[i], 1000) == HG_E_BUSY)
			busy++;
	CHECK_EQ_INT(busy, 1);
	echo_calls = 0;
	CHECK_EQ_INT(hg_serve(), 8);
	CHECK_EQ_INT(echo_calls, 8);
	CHECK_EQ_INT(hg_serve(), 7);
	CHECK_EQ_INT(hg_serve(), 0);
	int ok = 0;
	for (int i = 0; i < 15; i++)
		ok += hg_call_poll(pcs[i], &r).status == HG_OK;
	CHECK_EQ_INT(ok, 15);
	// EXPIRED: served after its deadline, without the handler; the caller
	// has timed out and discards the late reply by call id.
	CHECK_EQ_INT(hg_call_start("self.echo", DEPTH_TYPE, &d, DEPTH_TYPE, &pc, 20), HG_OK);
	sleep_ms(40);
	res = hg_call_poll(pc, &r);
	CHECK_EQ_INT(res.status, HG_E_TIMEOUT);
	uint64_t late = res.call_id;
	echo_calls = 0;
	CHECK_EQ_INT(hg_serve(), 1);
	CHECK_EQ_INT(echo_calls, 0);
	hg_sub *rep = hg_sub_open_own("self.rep", NULL, HG_NEWEST);
	unsigned char buf[96 + 256];
	struct hg_info info;
	CHECK_EQ_INT(hg_latest(rep, buf, sizeof buf, &info), HG_MSG);
	struct hg_call_reply_envelope env;
	memcpy(&env, buf, sizeof env);
	CHECK(env.call_id == late && env.status == HG_E_EXPIRED && strncmp(env.reason, "expired", 7) == 0);
	hg_sub_close(rep);
	// A later call still works: the discarded reply did not stick anywhere.
	res = self_call("self.echo", DEPTH_TYPE, &d, DEPTH_TYPE, &r, 500, &st);
	CHECK_EQ_INT(res.status, HG_OK);
	// REFUSED with the handler's reason, through a handler swapped in.
	CHECK_EQ_INT(hg_serve_method("echo", on_refuse, NULL), -1); /* already registered: EEXIST */
}

static void envelope(struct hg_call_request_envelope *env, uint64_t call_id, const char *method, const char *caller)
{
	memset(env, 0, sizeof *env);
	env->call_id = call_id;
	env->deadline_ns = hg_now_ns() + 2000000000ull;
	env->request_type = DEPTH_TYPE->hash;
	env->target_id = hg_topic_id("self.rep");
	snprintf(env->method, sizeof env->method, "%s", method);
	snprintf(env->caller, sizeof env->caller, "%s", caller);
}

/* A child re-executed as the module hangar: one request on the local admin
 * topic (caller admin:oguz, method echo: allowed by admin) and two on the
 * topside's admin topic, mirrored (topside:admin:oguz: echo denied, topside
 * allowed), all to self. */
static int child_hangar(void)
{
	setenv("HANGAR_WIRING_FILE", wiring_hangar, 1);
	if (hg_init() < 0) {
		fprintf(stderr, "child hangar: hg_init: %s\n", hg_init_error());
		return 3;
	}
	hg_pub *adm = hg_pub_open("hangar.vehicle.admin.req", &adm_parsed.type, NULL);
	unsigned char boot[16];
	memset(boot, 0x5a, sizeof boot);
	hg_pub *tadm = hg_pub_open_mirror("hangar.topside.admin.req", &adm_parsed.type, boot, 1, NULL);
	if (!adm || !tadm) {
		fprintf(stderr, "child: open: %s\n", strerror(errno));
		return 4;
	}
	static unsigned char payload[112 + 16384];
	struct hg_call_request_envelope env;
	struct depth d = { .depth_m = 42 };
	memcpy(payload + 112, &d, sizeof d);
	envelope(&env, 1, "echo", "admin:oguz");
	memcpy(payload, &env, sizeof env);
	hg_publish_with(adm, payload, 112 + 16384, hg_now_ns(), HG_NO_REFS, NULL, 0);
	envelope(&env, 2, "echo", "admin:oguz");
	memcpy(payload, &env, sizeof env);
	hg_publish_seq(tadm, 1, 0, payload, 112 + 16384, hg_now_ns(), NULL, 0);
	envelope(&env, 3, "topside", "admin:oguz");
	memcpy(payload, &env, sizeof env);
	hg_publish_seq(tadm, 2, 0, payload, 112 + 16384, hg_now_ns(), NULL, 0);
	sleep_ms(300); /* the parent reads before the files are closed */
	hg_pub_close(adm);
	hg_pub_close(tadm);
	return 0;
}

/* A child as the topside module pilot, its request topic mirrored here:
 * topside:pilot passes topside:* (method topside) and not echo's list; the
 * caller field it sets is ignored, since the topic is not an admin topic. */
static int child_pilot(void)
{
	setenv("HANGAR_WIRING_FILE", wiring_pilot, 1);
	if (hg_init() < 0) {
		fprintf(stderr, "child pilot: hg_init: %s\n", hg_init_error());
		return 3;
	}
	unsigned char boot[16];
	memset(boot, 0x5a, sizeof boot);
	hg_pub *pilot = hg_pub_open_mirror("pilot.req", &req_parsed.type, boot, 1, NULL);
	if (!pilot) {
		fprintf(stderr, "child: open: %s\n", strerror(errno));
		return 4;
	}
	static unsigned char payload[112 + 16384];
	struct hg_call_request_envelope env;
	struct depth d = { .depth_m = 42 };
	memcpy(payload + 112, &d, sizeof d);
	envelope(&env, 4, "topside", "");
	memcpy(payload, &env, sizeof env);
	hg_publish_seq(pilot, 1, 0, payload, req_parsed.type.payload_bytes, hg_now_ns(), NULL, 0);
	envelope(&env, 5, "echo", "admin:root");
	memcpy(payload, &env, sizeof env);
	hg_publish_seq(pilot, 2, 0, payload, req_parsed.type.payload_bytes, hg_now_ns(), NULL, 0);
	sleep_ms(300);
	hg_pub_close(pilot);
	return 0;
}

static void test_caller_tokens(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		execl("/proc/self/exe", "test_hg_call", "--hangar", (char *)NULL);
		_exit(127);
	}
	pid_t pid2 = fork();
	if (pid2 == 0) {
		execl("/proc/self/exe", "test_hg_call", "--pilot", (char *)NULL);
		_exit(127);
	}
	/* Serve until the five requests are answered, or 2 s pass. */
	hg_sub *subs[16];
	int n = hg_call_subs(subs, 16);
	int served = 0;
	uint64_t t0 = hg_now_ns();
	while (served < 5 && ms_since(t0) < 2000) {
		hg_wait(subs, n, 50);
		served += hg_serve();
	}
	CHECK_EQ_INT(served, 5);
	CHECK_EQ_INT(wait_child(pid), 0);
	CHECK_EQ_INT(wait_child(pid2), 0);
	hg_sub *rep = hg_sub_open_own("self.rep", NULL, HG_OLDEST);
	unsigned char buf[96 + 256];
	struct hg_info info;
	int seen = 0;
	while (hg_next(rep, buf, sizeof buf, &info) == HG_MSG) {
		struct hg_call_reply_envelope env;
		memcpy(&env, buf, sizeof env);
		if (env.caller_id == hg_topic_id("hangar.vehicle.admin.req")) {
			CHECK(env.call_id == 1 && env.status == HG_OK);
			seen++;
		} else if (env.caller_id == hg_topic_id("hangar.topside.admin.req")) {
			if (env.call_id == 2) {
				CHECK(env.status == HG_E_DENIED && strcmp(env.reason, "topside:admin:oguz is not in Allow of echo") == 0);
				seen++;
			} else if (env.call_id == 3) {
				CHECK_EQ_INT(env.status, HG_OK);
				seen++;
			}
		} else if (env.caller_id == hg_topic_id("pilot.req")) {
			if (env.call_id == 4) {
				CHECK_EQ_INT(env.status, HG_OK);
				seen++;
			} else if (env.call_id == 5) {
				CHECK(env.status == HG_E_DENIED && strcmp(env.reason, "topside:pilot is not in Allow of echo") == 0);
				seen++;
			}
		}
	}
	CHECK_EQ_INT(seen, 5);
	hg_sub_close(rep);
	CHECK(strcmp(last_caller, "topside:pilot") == 0 || strcmp(last_caller, "topside:admin:oguz") == 0 ||
	      strcmp(last_caller, "admin:oguz") == 0); /* whichever allowed request came last */
	// The token function on the subscriptions themselves.
	for (int i = 0; i < n; i++) {
		struct hg_call_request_envelope env;
		memset(&env, 0, sizeof env);
		snprintf(env.caller, sizeof env.caller, "admin:zed");
		char tok[HG_CALL_CALLER];
		const char *name = hg_sub_name(subs[i]);
		hg_call_caller_token(subs[i], &env, "vehicle", tok);
		if (strcmp(name, "hangar.vehicle.admin.req") == 0)
			CHECK(strcmp(tok, "admin:zed") == 0);
		else if (strcmp(name, "hangar.topside.admin.req") == 0)
			CHECK(strcmp(tok, "topside:admin:zed") == 0);
		else if (strcmp(name, "pilot.req") == 0)
			CHECK(strcmp(tok, "topside:pilot") == 0);
		else if (strcmp(name, "self.req") == 0)
			CHECK(strcmp(tok, "self") == 0);
	}
}

static int child_actuator(void)
{
	setenv("HANGAR_WIRING_FILE", wiring_act, 1);
	if (hg_init() < 0)
		return 3;
	struct depth d = { 0 }, r;
	hg_call_result res = hg_call("self.echo", DEPTH_TYPE, &d, DEPTH_TYPE, &r, 100);
	return res.status == HG_E_REFUSED && strstr(res.reason, "actuator") ? 0 : 5;
}

static void test_actuator_refuses_hg_call(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		execl("/proc/self/exe", "test_hg_call", "--actuator", (char *)NULL);
		_exit(127);
	}
	CHECK_EQ_INT(wait_child(pid), 0);
}

/* ---- Parameters -------------------------------------------------------- */

static void test_param_grammar(void)
{
	struct hg_param_attr a[16];
	char err[160] = "";
	CHECK_EQ_INT(hg_params_attrs(DH, a, err, sizeof err), 0);
	CHECK(a[0].dflt_len == 3 && strncmp(a[0].dflt, "1.5", 3) == 0 && a[0].min_len == 1 && *a[0].min == '0' &&
	      a[0].max_len == 2 && strncmp(a[0].max, "10", 2) == 0 && !a[0].restart);
	CHECK(a[3].dflt_len == 9 && strncmp(a[3].dflt, "1, 1, 0.5", 9) == 0 && a[3].min_len == 0);
	CHECK(a[5].dflt_len == 5 && strncmp(a[5].dflt, "\"ned\"", 5) == 0);
	CHECK(a[6].restart == 1 && strncmp(a[6].min, "10", 2) == 0 && strncmp(a[6].max, "200", 3) == 0);
	CHECK(a[7].dflt_len == 2 && strncmp(a[7].dflt, "-3", 2) == 0 && strncmp(a[7].min, "-10", 3) == 0);
	// Attributes that do not parse.
	struct hg_schema bad;
	const char *bads[] = { "f32 x = 1 [0]\n", "char s[4] = ned\n", "f32 x [0, 1] = 2\n", "f32 x = 1 @reboot\n",
			       "char s[4] [0, 1]\n", NULL };
	for (int i = 0; bads[i]; i++) {
		CHECK_EQ_INT(hg_schema_parse(bads[i], strlen(bads[i]), &bad, NULL, 0), 0);
		CHECK_EQ_INT(hg_params_attrs(&bad.type, a, err, sizeof err), -1);
	}
	// Defaults, values, formatting, round trip.
	struct dh v;
	CHECK_EQ_INT(hg_params_defaults(DH, (hg_params_attrs(DH, a, NULL, 0), a), (unsigned char *)&v, err, sizeof err), 0);
	CHECK(v.kp == 1.5f && v.ki == 0.1f && v.gains[0] == 1 && v.gains[2] == 0.5f && v.enabled == 0 &&
	      strcmp(v.frame, "ned") == 0 && v.rate_hz == 50 && v.trim == -3);
	char out[128];
	hg_param_format(hg_type_field(DH, "ki"), (unsigned char *)&v, out, sizeof out);
	CHECK(strcmp(out, "0.1") == 0);
	hg_param_format(hg_type_field(DH, "gains"), (unsigned char *)&v, out, sizeof out);
	CHECK(strcmp(out, "1, 1, 0.5") == 0);
	hg_param_format(hg_type_field(DH, "frame"), (unsigned char *)&v, out, sizeof out);
	CHECK(strcmp(out, "\"ned\"") == 0);
	hg_param_format(hg_type_field(DH, "trim"), (unsigned char *)&v, out, sizeof out);
	CHECK(strcmp(out, "-3") == 0);
	const struct hg_field *g = hg_type_field(DH, "gains"), *f = hg_type_field(DH, "frame"),
			      *e = hg_type_field(DH, "enabled"), *t = hg_type_field(DH, "trim");
	CHECK_EQ_INT(hg_param_parse(g, "7", 1, (unsigned char *)&v, err, sizeof err), 0); /* one value for every element */
	CHECK(v.gains[0] == 7 && v.gains[2] == 7);
	CHECK_EQ_INT(hg_param_parse(g, "1,2", 3, (unsigned char *)&v, err, sizeof err), -1);
	CHECK_EQ_INT(hg_param_parse(g, " 1, 2 ,3 ", 9, (unsigned char *)&v, err, sizeof err), 0);
	CHECK(v.gains[1] == 2 && v.gains[2] == 3);
	CHECK_EQ_INT(hg_param_parse(f, "\"a \\\"q\\\" b\"", 11, (unsigned char *)&v, err, sizeof err), 0);
	CHECK(strcmp(v.frame, "a \"q\" b") == 0);
	hg_param_format(f, (unsigned char *)&v, out, sizeof out);
	CHECK(strcmp(out, "\"a \\\"q\\\" b\"") == 0);
	CHECK_EQ_INT(hg_param_parse(f, "ned", 3, (unsigned char *)&v, err, sizeof err), -1); /* unquoted */
	CHECK_EQ_INT(hg_param_parse(f, "\"0123456789abcdef\"", 18, (unsigned char *)&v, err, sizeof err), -1); /* 16: too long */
	CHECK_EQ_INT(hg_param_parse(e, "-1", 2, (unsigned char *)&v, err, sizeof err), -1); /* unsigned */
	CHECK_EQ_INT(hg_param_parse(e, "256", 3, (unsigned char *)&v, err, sizeof err), -1);
	CHECK_EQ_INT(hg_param_parse(e, "2", 1, (unsigned char *)&v, err, sizeof err), 0);
	CHECK_EQ_INT(hg_param_parse(e, "1.5", 3, (unsigned char *)&v, err, sizeof err), -1);
	CHECK_EQ_INT(hg_param_parse(t, "-40000", 6, (unsigned char *)&v, err, sizeof err), -1);
	// Ranges, per element.
	char reason[64];
	CHECK_EQ_INT(hg_param_check_range(e, &a[4], (unsigned char *)&v, reason, sizeof reason), -1);
	CHECK(strcmp(reason, "enabled 2 above 1") == 0);
	v.enabled = 1;
	CHECK_EQ_INT(hg_param_check_range(e, &a[4], (unsigned char *)&v, reason, sizeof reason), 0);
	v.trim = -11;
	CHECK_EQ_INT(hg_param_check_range(t, &a[7], (unsigned char *)&v, reason, sizeof reason), -1);
	CHECK(strcmp(reason, "trim -11 below -10") == 0);
	v.trim = 0;
	CHECK_EQ_INT(hg_param_check_range(g, &a[3], (unsigned char *)&v, reason, sizeof reason), 0); /* no range */
	// The saved file: format, then parse back; missing, unknown, repeated.
	struct hg_params_header hdr = { .version = 17, .restart_pending = 1 };
	snprintf(hdr.changed_by, sizeof hdr.changed_by, "topside:pilot_console");
	static char text[4096];
	int n = hg_params_file_format(DH, (unsigned char *)&v, &hdr, text, sizeof text);
	CHECK(n > 0 && strncmp(text, "# hangar parameters: dh_params\n# version 17, set by topside:pilot_console, ", 74) == 0);
	CHECK(strstr(text, ", restart\n") && strstr(text, "\ngains = 1, 2, 3\n") && strstr(text, "\ntrim = 0\n"));
	struct dh back;
	memset(&back, 0, sizeof back);
	struct hg_params_header h2;
	unsigned char present[16];
	CHECK_EQ_INT(hg_params_file_parse(DH, text, (size_t)n, (unsigned char *)&back, &h2, present, 1, err, sizeof err), 0);
	CHECK(memcmp(&back, &v, sizeof v) == 0);
	CHECK(h2.version == 17 && h2.have_version && h2.restart_pending && strcmp(h2.changed_by, "topside:pilot_console") == 0);
	CHECK(present[0] && present[7]);
	const char *partial = "# hangar parameters: dh_params\n# version 3, set by saved, 2026-01-01T00:00:00Z\n\nkp = 2 \n# a note\n";
	memcpy(&back, &v, sizeof v);
	CHECK_EQ_INT(hg_params_file_parse(DH, partial, strlen(partial), (unsigned char *)&back, &h2, present, 1, err, sizeof err), 0);
	CHECK(back.kp == 2 && back.trim == 0 && present[0] && !present[1] && h2.version == 3 && !h2.restart_pending &&
	      strcmp(h2.changed_by, "saved") == 0);
	const char *unknown = "# hangar parameters: dh_params\nbogus = 1\n";
	CHECK_EQ_INT(hg_params_file_parse(DH, unknown, strlen(unknown), (unsigned char *)&back, &h2, present, 1, err, sizeof err), -1);
	CHECK(strstr(err, "unknown field bogus") != NULL);
	const char *twice = "# hangar parameters: dh_params\nkp = 1\nkp = 2\n";
	CHECK_EQ_INT(hg_params_file_parse(DH, twice, strlen(twice), (unsigned char *)&back, &h2, present, 1, err, sizeof err), -1);
	const char *other = "# hangar parameters: other_params\nkp = 1\n";
	CHECK_EQ_INT(hg_params_file_parse(DH, other, strlen(other), (unsigned char *)&back, &h2, present, 1, err, sizeof err), -1);
	CHECK_EQ_INT(hg_params_file_parse(DH, "", 0, (unsigned char *)&back, &h2, present, 1, err, sizeof err), -1);
	CHECK_EQ_INT(hg_params_file_parse(DH, "kp = 3\n", 7, (unsigned char *)&back, &h2, present, 0, err, sizeof err), 0);
	CHECK(back.kp == 3);
	CHECK_EQ_INT(hg_params_file_parse(DH, "kp 3\n", 5, (unsigned char *)&back, &h2, present, 0, err, sizeof err), -1);
}

static struct dh P;
static int validator_calls;

static int validate(const void *cand, const void *cur, char reason[64])
{
	const struct dh *c = cand;
	(void)cur;
	validator_calls++;
	if (c->enabled && c->max_thrust == 0) {
		snprintf(reason, 64, "enabled with max_thrust 0");
		return HG_E_REFUSED;
	}
	return HG_OK;
}

struct sp_req {
	uint64_t base;
	struct dh p;
};

static hg_call_result set_dh(const struct sp_req *req, uint64_t *version, int *restarting)
{
	unsigned char rep[16] = { 0 };
	int st;
	hg_call_result r = self_call("self.set_params", SP, req, &spr_parsed.type, rep, 500, &st);
	memcpy(version, rep, 8);
	*restarting = rep[8];
	return r;
}

static void test_params(void)
{
	char path[700];
	snprintf(path, sizeof path, "%s/params", state_dir);
	unlink(path);
	CHECK_EQ_INT(hg_params_open(DH, &P, validate, (void *)0x7), 0);
	CHECK(hg_params_ctx() == (void *)0x7);
	CHECK(P.kp == 1.5f && P.rate_hz == 50 && hg_params_version() == 1 && strcmp(hg_params_changed_by(), "saved") == 0);
	CHECK_EQ_INT(validator_calls, 0); /* the defaults are not validated: there is no saved file */
	FILE *f = fopen(path, "r");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	// The topic, with the struct at offset 64.
	hg_sub *ps = hg_sub_open_own("self.params", NULL, HG_OLDEST);
	unsigned char buf[64 + 64];
	struct hg_info info;
	CHECK_EQ_INT(hg_next(ps, buf, sizeof buf, &info), HG_MSG);
	uint64_t ver;
	memcpy(&ver, buf, 8);
	CHECK(ver == 1 && strcmp((char *)buf + 8, "saved") == 0 && buf[56] == 0 && memcmp(buf + 64, &P, sizeof P) == 0);
	uint32_t slen;
	const char *schema = hg_sub_schema(ps, &slen);
	CHECK(schema && strstr(schema, "@restart") && strstr(schema, "u8   restart_pending"));
	// set_params through the library: applied whole, published, saved.
	struct sp_req req = { .base = 1, .p = P };
	req.p.kp = 2;
	req.p.frame[0] = 'e';
	uint64_t version;
	int restarting;
	hg_call_result r = set_dh(&req, &version, &restarting);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(version == 2 && !restarting && P.kp == 2 && P.frame[0] == 'e' && hg_params_changed() && !hg_params_changed());
	CHECK(strcmp(hg_params_changed_by(), "self") == 0);
	CHECK_EQ_INT(hg_next(ps, buf, sizeof buf, &info), HG_MSG);
	memcpy(&ver, buf, 8);
	CHECK(ver == 2 && strcmp((char *)buf + 8, "self") == 0 && info.n_inputs == 1 &&
	      info.inputs[0].topic_id == hg_topic_id("self.req"));
	// Stale, range, validator: refused, nothing changes.
	r = set_dh(&req, &version, &restarting);
	CHECK(r.status == HG_E_REFUSED && strcmp(r.reason, "stale: version is 2") == 0);
	req.base = 2;
	req.p.ki = 3;
	r = set_dh(&req, &version, &restarting);
	CHECK(r.status == HG_E_REFUSED && strcmp(r.reason, "ki 3 above 2") == 0);
	req.p.ki = 0.1f;
	req.p.gains[1] = -1;
	r = set_dh(&req, &version, &restarting); /* no range on gains: accepted */
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(P.gains[1] == -1 && hg_params_version() == 3 && hg_params_changed());
	CHECK_EQ_INT(hg_next(ps, buf, sizeof buf, &info), HG_MSG); /* version 3 */
	CHECK_EQ_INT(hg_next(ps, buf, sizeof buf, &info), HG_NONE);
	req.base = 3;
	req.p = P;
	req.p.enabled = 1;
	req.p.max_thrust = 0;
	r = set_dh(&req, &version, &restarting);
	CHECK(r.status == HG_E_REFUSED && strcmp(r.reason, "enabled with max_thrust 0") == 0);
	CHECK(P.enabled == 0 && hg_params_version() == 3 && !hg_params_changed());
	// @restart: saved, not applied; restart_pending published; with no
	// manager the restart is logged and stays pending; further sets refused.
	req.p = P;
	req.p.rate_hz = 120;
	r = set_dh(&req, &version, &restarting);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(version == 4 && restarting == 1 && P.rate_hz == 50 && hg_params_restart_pending() && !hg_params_changed());
	CHECK_EQ_INT(hg_next(ps, buf, sizeof buf, &info), HG_MSG);
	memcpy(&ver, buf, 8);
	CHECK(ver == 3 && buf[56] == 1 && strcmp((char *)buf + 8, "self") == 0);
	uint32_t rate;
	memcpy(&rate, buf + 64 + offsetof(struct dh, rate_hz), 4);
	CHECK_EQ_INT(rate, 50);
	req.base = 4;
	req.p.rate_hz = 50;
	r = set_dh(&req, &version, &restarting);
	CHECK(r.status == HG_E_REFUSED && strcmp(r.reason, "restart pending") == 0);
	static char text[4096];
	f = fopen(path, "r");
	size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
	if (f)
		fclose(f);
	text[n] = 0;
	CHECK(strstr(text, "# version 4, set by self, ") && strstr(text, ", restart\n") && strstr(text, "\nrate_hz = 120\n"));
	hg_sub_close(ps);
}

/* A new instance (re-executed): loads the saved file and reports what it
 * published: "version changed_by rate_hz", or exits 78 on a bad file. */
static int child_params(const char *out)
{
	become_self();
	static struct dh Q;
	if (hg_params_open(DH, &Q, validate, NULL) < 0)
		return 4;
	FILE *f = fopen(out, "w");
	if (f) {
		fprintf(f, "%llu %s %u %d\n", (unsigned long long)hg_params_version(), hg_params_changed_by(), Q.rate_hz,
			hg_params_restart_pending());
		fclose(f);
	}
	return 0;
}

static int child_run(const char *out, char *line, size_t len)
{
	pid_t pid = fork();
	if (pid == 0) {
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0)
			dup2(fd, 2);
		execl("/proc/self/exe", "test_hg_call", "--params", out, (char *)NULL);
		_exit(127);
	}
	int rc = wait_child(pid);
	line[0] = 0;
	FILE *f = fopen(out, "r");
	if (f) {
		if (!fgets(line, (int)len, f))
			line[0] = 0;
		fclose(f);
	}
	return rc;
}

static void test_params_restart_and_exit78(void)
{
	char path[700], out[700], line[160];
	snprintf(path, sizeof path, "%s/params", state_dir);
	snprintf(out, sizeof out, "%s/child.out", state_dir);
	// The instance the @restart change asked for: the new values, with the
	// requester's name, and the mark cleared.
	unlink(out);
	CHECK_EQ_INT(child_run(out, line, sizeof line), 0);
	CHECK(strcmp(line, "4 self 120 0\n") == 0);
	unlink(out);
	CHECK_EQ_INT(child_run(out, line, sizeof line), 0);
	CHECK(strcmp(line, "4 saved 120 0\n") == 0); /* a plain start says saved */
	// Bad saved files: exit 78, never loaded as defaults.
	const char *bad[] = { "# hangar parameters: dh_params\nkp = 1\nbogus = 2\n",
			      "# hangar parameters: dh_params\nkp = eleven\n",
			      "# hangar parameters: dh_params\nkp = 11\n",
			      "# hangar parameters: dh_params\nenabled = 1\nmax_thrust = 0\n",
			      "kp = 1\n",
			      "",
			      NULL };
	for (int i = 0; bad[i]; i++) {
		write_text(path, bad[i]);
		unlink(out);
		CHECK_EQ_INT(child_run(out, line, sizeof line), 78);
	}
	// A missing field takes its default; the version line is read.
	write_text(path, "# hangar parameters: dh_params\n# version 9, set by admin:x, now\nkp = 4\n");
	unlink(out);
	CHECK_EQ_INT(child_run(out, line, sizeof line), 0);
	CHECK(strcmp(line, "9 saved 50 0\n") == 0);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		/* A re-executed child: the topic directory and types from the
		 * environment the parent set. */
		snprintf(topic_dir, sizeof topic_dir, "%s", getenv("HANGAR_TOPIC_DIR") ? getenv("HANGAR_TOPIC_DIR") : "");
		if (hg_schema_parse(DEPTH_SCHEMA, strlen(DEPTH_SCHEMA), &depth_parsed, NULL, 0) < 0 ||
		    hg_schema_parse(WORDS_SCHEMA, strlen(WORDS_SCHEMA), &words_parsed, NULL, 0) < 0)
			return 2;
		DEPTH_TYPE = &depth_parsed.type;
		WORDS_TYPE = &words_parsed.type;
		child_process = 1;
		build();
		if (strcmp(argv[1], "--hangar") == 0)
			return child_hangar();
		if (strcmp(argv[1], "--pilot") == 0)
			return child_pilot();
		if (strcmp(argv[1], "--actuator") == 0)
			return child_actuator();
		if (strcmp(argv[1], "--params") == 0 && argc > 2)
			return child_params(argv[2]);
		return 2;
	}
	hg_test_setup("call");
	build();
	RUN_TEST(test_tokens);
	RUN_TEST(test_roundtrip);
	RUN_TEST(test_status_codes);
	RUN_TEST(test_caller_tokens);
	RUN_TEST(test_actuator_refuses_hg_call);
	RUN_TEST(test_param_grammar);
	RUN_TEST(test_params);
	RUN_TEST(test_params_restart_and_exit78);
	hg_test_teardown();
	TEST_MAIN_END();
}
