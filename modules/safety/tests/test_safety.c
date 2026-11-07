// SPDX-License-Identifier: GPL-3.0-or-later
// The safety module under a manager (docs/07 "Tests"): this process plays
// the vehicle's mavlink module (it serves mavlink.arm and publishes
// ardusub.state, vehicle.leak, power.battery and nav.depth, toy inputs), a
// fake mux carries mux.params, the toy talker stands for depth_hold and the
// toy crasher for a module given up with OnGiveUp=failsafe. Every state
// transition and refused transition, each trigger and the combination rule,
// the latch across a restart, clear against the manager, and the Allow
// lists through a raw caller. Under 30 s.
#include "hd_test.h"

#include "ardusub_state.h"
#include "arm_rep.h"
#include "arm_req.h"
#include "battery.h"
#include "depth.h"
#include "empty.h"
#include "hg_call.h"
#include "leak.h"
#include "safety_state.h"

static pid_t mgr;
static const char *T_safety, *T_fake_mux;
static hg_pub *P_ap, *P_leak, *P_bat, *P_depth;
static hg_sub *S_state;
static safety_state last_state;
static int have_state;

/* What the toy inputs say; pump() publishes them. */
static struct {
	int publish_inputs;     /* 0 until the first checks have seen them absent */
	int leak_wet, publish_leak;
	float batt_v;
	float depth_m;
	uint8_t ap_armed, ap_params;
	uint32_t ap_mode;
	uint8_t ap_result;
	char ap_reason[100];
	char ap_failsafe[50];
	int ap_frozen;          /* last_rx_ns stops advancing: ArduSub silent */
	uint64_t ap_last_rx;
	int arm_mode;           /* 0 accept, 1 refuse, 2 accept and never report armed */
	int arms_served;
	uint8_t arm_saw_state;  /* safety.state's state when the arm request came */
	uint64_t next_sensor_ns, next_ap_ns;
} T = { .publish_leak = 1, .batt_v = 16.0f, .depth_m = 1.0f, .ap_mode = 19, .ap_params = 1, .ap_result = 255 };

static void copy_types(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *types[] = { "safety_params", "mux_params", "leak", "battery", "ardusub_state", "arm_req",
				"arm_rep", "check_rep", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src, types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		if (copy_file(from, to) < 0) {
			fprintf(stderr, "cannot copy %s\n", from);
			exit(2);
		}
	}
}

static void write_manifests(void)
{
	manifest("safety", "[Module]\nDescription=Vehicle state\nExec=%s\n"
			   "[Subscribe hangar.vehicle.modules]\n[Subscribe hangar.vehicle.system]\n"
			   "[Subscribe vehicle.leak]\n[Subscribe power.battery]\n[Subscribe nav.depth]\n"
			   "[Subscribe ardusub.state]\n[Subscribe mux.params]\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Params]\nType=safety_params\nAllow=topside:pilot_console admin topside:admin\n"
			   "[Serve arm]\nRequest=empty\nReply=empty\nAllow=topside:pilot_console topside:topside_mavlink admin topside:admin\n"
			   "[Serve disarm]\nRequest=empty\nReply=empty\nAllow=* topside:* admin topside:admin\n"
			   "[Serve estop]\nRequest=empty\nReply=empty\nAllow=* topside:* admin topside:admin\n"
			   "[Serve clear]\nRequest=empty\nReply=empty\nAllow=topside:pilot_console topside:topside_mavlink admin topside:admin\n"
			   "[Serve check]\nRequest=empty\nReply=check_rep\nAllow=* topside:* admin topside:admin\n"
			   "[Call mavlink.arm]\n[Restart]\nPolicy=always\nBackoff=0.1s..0.2s\n", T_safety);
	manifest("mux", "[Module]\nExec=%s\n[Params]\nType=mux_params\nAllow=* admin\n[Health]\nWatchdog=5s\n", T_fake_mux);
	manifest("depth_hold", "[Module]\nExec=%s/talker --topic ctl.depth_hold --rate 50\n"
			       "[Publish ctl.depth_hold]\nType=depth\nRate=50\n", T_toys);
	/* This process: the mavlink module's topics and method, with a program
	 * that exits at once and is never restarted. */
	manifest("mavlink", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			    "[Subscribe safety.state]\n"
			    "[Publish ardusub.state]\nType=ardusub_state\nRate=10\nInputs=safety.state\n"
			    "[Publish vehicle.leak]\nType=leak\nRate=10\n"
			    "[Publish power.battery]\nType=battery\nRate=10\n"
			    "[Publish nav.depth]\nType=depth\nRate=50\n"
			    "[Serve arm]\nRequest=arm_req\nReply=arm_rep\nAllow=safety\n");
	manifest("rawcaller", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n[Call safety.disarm]\n");
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
	/* The library has read the wiring; the hangar command this process
	 * runs must not (it would be refused safety.rep as a module). */
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
}

/* ---- The raw caller: a child publishing a hand-made request to safety ---- */

static int raw_child(int argc, char **argv)
{
	/* argv: --raw <method> <out-file> <call-id> */
	if (argc < 5)
		return 2;
	const char *method = argv[2], *out = argv[3];
	unsigned long long call_id = strtoull(argv[4], NULL, 10);
	become("rawcaller");
	static char text[HG_SCHEMA_MAX];
	static struct hg_schema schema;
	int n = hg_call_req_schema("rawcaller", 8, text, sizeof text);
	if (hg_schema_parse(text, (size_t)n, &schema, NULL, 0) < 0)
		return 3;
	schema.type.name = "req";
	hg_pub *p = hg_pub_open("rawcaller.req", &schema.type, NULL);
	hg_sub *rep = hg_sub_open("safety.rep", NULL);
	if (!p || !rep) {
		fprintf(stderr, "rawcaller: open: %s\n", strerror(errno));
		return 3;
	}
	struct hg_call_request_envelope env;
	memset(&env, 0, sizeof env);
	env.call_id = call_id;
	env.deadline_ns = hg_now_ns() + 1000000000ull;
	env.request_type = EMPTY_TYPE.hash;
	env.target_id = hg_topic_id("safety.rep");
	tfmt(env.method, sizeof env.method, "%s", method);
	unsigned char payload[HG_CALL_REQ_ENVELOPE + 8];
	memset(payload, 0, sizeof payload);
	memcpy(payload, &env, sizeof env);
	hg_publish_with(p, payload, sizeof payload, hg_now_ns(), HG_NO_REFS, NULL, 0);
	uint64_t deadline = hg_now_ns() + 2000000000ull;
	FILE *f = fopen(out, "w");
	while (hg_now_ns() < deadline) {
		unsigned char buf[HG_CALL_REP_ENVELOPE + 256];
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

static void pump(long ms);

static const char *raw_call(const char *method)
{
	static char out[600], buf[200];
	static int raw_calls;
	tfmt(out, sizeof out, "%s/raw.out", T_root);
	unlink(out);
	char id[16];
	tfmt(id, sizeof id, "%d", ++raw_calls);
	pid_t pid = fork();
	if (pid == 0) {
		execl("/proc/self/exe", "test_safety", "--raw", method, out, id, (char *)NULL);
		_exit(127);
	}
	/* The inputs keep flowing while the child waits for its reply. */
	int rc = -1;
	uint64_t t0 = now_ms();
	while (rc == -1 && now_ms() - t0 < 5000) {
		pump(20);
		int st;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid)
			rc = WIFEXITED(st) ? WEXITSTATUS(st) : -2;
	}
	if (rc != 0)
		printf("    rawcaller exited %d\n", rc);
	tfmt(buf, sizeof buf, "%s", read_file(out));
	return buf;
}

/* ---- This process as mavlink ---------------------------------------------- */


static int on_arm(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	T.arms_served++;
	if (getenv("SAFETY_DEBUG"))
		printf("      [%llu ms] arm request served\n", (unsigned long long)(hg_now_ns() / 1000000u));
	safety_state s;
	struct hg_info info;
	/* docs/10 step 3: the newest safety.state must say arming. */
	if (hg_latest(S_state, &s, sizeof s, &info) == HG_MSG || have_state) {
		if (info.result == HG_MSG) {
			last_state = s;
			have_state = 1;
		}
		T.arm_saw_state = last_state.state;
	}
	if (T.arm_mode == 1) {
		tfmt(reason, HG_CALL_REASON, "PreArm: Compass not calibrated");
		return HG_E_REFUSED;
	}
	((arm_rep *)rep)->accepted = 1;
	return HG_OK;
}

static void read_state(void)
{
	safety_state s;
	struct hg_info info;
	enum hg_result rr;
	while ((rr = hg_next(S_state, &s, sizeof s, &info)) == HG_MSG) {
		if (getenv("SAFETY_DEBUG"))
			printf("      [%llu ms] safety.state seq %llu state %d reason '%s'\n",
			       (unsigned long long)(hg_now_ns() / 1000000u), (unsigned long long)info.seq, s.state, s.reason);
		last_state = s;
		have_state = 1;
	}
	if (getenv("SAFETY_DEBUG") && rr != HG_NONE)
		printf("      [%llu ms] hg_next(safety.state) = %d errno %s\n", (unsigned long long)(hg_now_ns() / 1000000u),
		       (int)rr, strerror(errno));
}

static void publish_ap(void)
{
	read_state(); /* the reference ardusub.state carries: the newest safety.state */
	struct ardusub_state ap;
	memset(&ap, 0, sizeof ap);
	ap.armed = T.ap_armed;
	ap.system_status = 4;
	ap.params = T.ap_params;
	ap.mode = T.ap_mode;
	if (!T.ap_frozen)
		T.ap_last_rx = hg_now_ns();
	ap.last_rx_ns = T.ap_last_rx;
	ap.arm_attempt = (uint32_t)T.arms_served;
	ap.arm_result = T.ap_result;
	tfmt(ap.arm_reason, sizeof ap.arm_reason, "%s", T.ap_reason);
	tfmt(ap.failsafe, sizeof ap.failsafe, "%s", T.ap_failsafe);
	hg_publish(P_ap, &ap, sizeof ap, hg_now_ns());
}

/* Runs the fake modules for ms: serves arm, publishes the inputs at their
 * rates, reads safety.state. */
static void pump(long ms)
{
	uint64_t t0 = hg_now_ns(), end = t0 + (uint64_t)ms * 1000000u;
	hg_sub *subs[8];
	int n = hg_call_subs(subs, 8);
	do {
		uint64_t now = hg_now_ns();
		if (now >= T.next_sensor_ns && T.publish_inputs) {
			T.next_sensor_ns = now + 20000000u;
			if (T.publish_leak) {
				struct leak l = { .wet = (uint8_t)T.leak_wet };
				hg_publish(P_leak, &l, sizeof l, now);
			}
			struct battery b = { .voltage_v = T.batt_v, .current_a = 1, .remaining = 0.8f };
			hg_publish(P_bat, &b, sizeof b, now);
			struct depth d = { .depth_m = T.depth_m, .temp_c = 10, .pressure_mbar = 1013 + 100 * T.depth_m };
			hg_publish(P_depth, &d, sizeof d, now);
		}
		if (now >= T.next_ap_ns && T.publish_inputs) {
			T.next_ap_ns = now + 100000000u;
			publish_ap();
		}
		if (n > 0)
			hg_wait(subs, n, 5);
		else
			sleep_ms(5);
		hg_serve();
		read_state();
	} while (hg_now_ns() < end);
}

/* Pumps until safety.state's state is st (and, if action >= 0, its action). */
static int wait_safety(int st, int action, long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		pump(10);
		if (have_state && last_state.state == st && (action < 0 || last_state.action == action))
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	printf("    safety.state is %d (action %d, triggers %u, reason '%s'), wanted %d\n", last_state.state,
	       last_state.action, last_state.triggers, last_state.reason, st);
	return 0;
}

static char cli_out[8192];

static int call(const char *method)
{
	char target[64];
	tfmt(target, sizeof target, "safety.%s", method);
	return run_cli(cli_out, sizeof cli_out, 5000, "call", target, (char *)NULL);
}

static int param_set(const char *module, const char *kv1, const char *kv2, const char *kv3, const char *kv4)
{
	return run_cli(cli_out, sizeof cli_out, 5000, "param", "set", module, kv1, kv2, kv3, kv4, (char *)NULL);
}

/* The safety module's status line. */
static const char *safety_status(void)
{
	static struct mstat m;
	module_status("safety", &m);
	return m.status;
}

/* ---- Tests ----------------------------------------------------------------- */

static void test_start(void)
{
	write_manifests();
	mgr = manager_start();
	CHECK(wait_state("safety", "ready", 5000));
	CHECK(wait_state("mux", "ready", 5000));
	CHECK(wait_state("depth_hold", "ready", 5000));
	CHECK(wait_state("mavlink", "stopped", 5000));
	become("mavlink");
	S_state = hg_sub_open("safety.state", &SAFETY_STATE_TYPE);
	P_ap = hg_pub_open("ardusub.state", &ARDUSUB_STATE_TYPE, NULL);
	P_leak = hg_pub_open("vehicle.leak", &LEAK_TYPE, NULL);
	P_bat = hg_pub_open("power.battery", &BATTERY_TYPE, NULL);
	P_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);
	CHECK(S_state && P_ap && P_leak && P_bat && P_depth);
	CHECK_EQ_INT(hg_serve_method("arm", on_arm, NULL), 0);
	CHECK(wait_safety(0, -1, 3000));
	CHECK(strcmp(last_state.reason, "start") == 0);
	CHECK(!last_state.outputs_enabled);
	CHECK(strncmp(safety_status(), "disarmed", 8) == 0);
	/* With nothing published yet, the checklist names what is missing; the
	 * default required list wants mux and mavlink, and mavlink is stopped. */
	CHECK_EQ_INT(call("check"), 0);
	CHECK(strcmp(cli_field(cli_out, "ok"), "0") == 0);
	CHECK(strstr(cli_out, "mavlink not ready") != NULL);
	CHECK(strstr(cli_out, "mux not ready") == NULL);
	CHECK(strstr(cli_out, "vehicle.leak absent") != NULL);
	CHECK(strstr(cli_out, "ardusub.state absent") != NULL);
	/* Shorter timeouts for the suite; mux is the one module required. */
	CHECK_EQ_INT(param_set("safety", "required=mux", "battery_time=0.5", "hold_time=1", "arm_timeout=1"), 0);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "param", "set", "safety", "autopilot_silence=1", (char *)NULL), 0);
	/* arm is refused while the checklist fails, naming the first failure. */
	CHECK_EQ_INT(call("arm"), 1);
	CHECK(strstr(cli_out, "HG_E_REFUSED") != NULL);
	CHECK(strstr(cli_out, "vehicle.leak absent") != NULL);
	pump(100);
	CHECK(strstr(last_state.reason, "arm refused") != NULL);
}

static void test_arm_disarm(void)
{
	T.publish_inputs = 1;
	pump(400); /* the inputs flow: dry, 16 V, 1 m, ArduSub disarmed in MANUAL */
	CHECK_EQ_INT(call("check"), 0);
	CHECK(strcmp(cli_field(cli_out, "ok"), "1") == 0);
	T.arm_saw_state = 99;
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(strstr(cli_out, "HG_OK") != NULL);
	/* arming first, then the call: the module saw arming before the request. */
	CHECK(wait_safety(3, -1, 1000));
	CHECK(!last_state.outputs_enabled);
	CHECK(strncmp(last_state.by, "admin:", 6) == 0);
	pump(100);
	CHECK_EQ_INT(T.arms_served, 1);
	CHECK_EQ_INT(T.arm_saw_state, 3);
	/* ArduSub reports armed: armed. */
	T.ap_armed = 1;
	T.ap_result = 0;
	CHECK(wait_safety(1, -1, 1000));
	CHECK(last_state.outputs_enabled);
	CHECK(strncmp(safety_status(), "armed by admin:", 15) == 0);
	/* arm again: idempotent. */
	CHECK_EQ_INT(call("arm"), 0);
	pump(50);
	CHECK_EQ_INT(T.arms_served, 1);
	/* disarm: disarmed, and the fake ArduSub follows. */
	CHECK_EQ_INT(call("disarm"), 0);
	CHECK(wait_safety(0, -1, 1000));
	CHECK(!last_state.outputs_enabled);
	T.ap_armed = 0;
	pump(200);
	CHECK_EQ_INT(last_state.state, 0);
}

static void test_arm_refusals(void)
{
	/* The handler refuses: back to disarmed with its reason. */
	T.arm_mode = 1;
	T.ap_result = 255;
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(wait_safety(0, -1, 2000));
	CHECK(strstr(last_state.reason, "PreArm: Compass") != NULL);
	if (!strstr(last_state.reason, "PreArm: Compass"))
		printf("    reason: %s\n", last_state.reason);
	/* ArduSub's refusal through ardusub.state: arm_result and the text. */
	T.arm_mode = 2;
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(wait_safety(3, -1, 1000));
	T.ap_result = 4; /* MAV_RESULT_FAILED */
	tfmt(T.ap_reason, sizeof T.ap_reason, "PreArm: Hardware safety switch");
	CHECK(wait_safety(0, -1, 2000));
	CHECK(strstr(last_state.reason, "ArduSub refused") != NULL && strstr(last_state.reason, "safety switch") != NULL);
	/* No report at all: disarmed after arm_timeout (1 s here). */
	T.ap_result = 255;
	T.ap_reason[0] = 0;
	uint64_t t0 = now_ms();
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(wait_safety(3, -1, 1000));
	CHECK(wait_safety(0, -1, 3000));
	uint64_t dt = now_ms() - t0;
	CHECK(strstr(last_state.reason, "no armed report") != NULL);
	CHECK(dt >= 900 && dt < 2500);
	printf("    arming given up after %llu ms\n", (unsigned long long)dt);
	T.arm_mode = 0;
}

static void arm_ok(void)
{
	T.arm_mode = 0;
	T.ap_result = 255;
	T.ap_reason[0] = 0;
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(wait_safety(3, -1, 1000));
	T.ap_armed = 1;
	T.ap_result = 0;
	CHECK(wait_safety(1, -1, 1000));
}

static void clear_ok(void)
{
	CHECK_EQ_INT(call("clear"), 0);
	if (strstr(cli_out, "HG_OK") == NULL)
		printf("    clear: %s\n", cli_out);
	CHECK(wait_safety(0, -1, 1000));
}

static void test_autopilot_trigger(void)
{
	/* ArduSub disarms itself while hangar is armed: autopilot, neutral. */
	arm_ok();
	T.ap_armed = 0;
	tfmt(T.ap_failsafe, sizeof T.ap_failsafe, "Battery failsafe");
	CHECK(wait_safety(2, 0, 1000));
	CHECK_EQ_INT(last_state.triggers, 1u << 5);
	CHECK(!last_state.outputs_enabled);
	CHECK(strstr(last_state.reason, "disarmed itself") != NULL && strstr(last_state.reason, "Battery failsafe") != NULL);
	CHECK(strcmp(last_state.by, "autopilot") == 0);
	/* arm is refused in failsafe; clear works once ArduSub is disarmed in MANUAL. */
	CHECK_EQ_INT(call("arm"), 1);
	CHECK(strstr(cli_out, "clear first") != NULL);
	T.ap_failsafe[0] = 0;
	pump(200);
	clear_ok();
	/* ArduSub armed while hangar is disarmed, judged against the
	 * safety.state the message references: disagreement at the first message. */
	pump(200);
	T.ap_armed = 1;
	CHECK(wait_safety(2, 0, 1000));
	CHECK(strstr(last_state.reason, "armed while hangar was disarmed") != NULL);
	T.ap_armed = 0;
	pump(200);
	clear_ok();
	/* A mode other than MANUAL while armed. clear is refused while it lasts. */
	arm_ok();
	T.ap_mode = 2;
	CHECK(wait_safety(2, 0, 1000));
	CHECK(strstr(last_state.reason, "not MANUAL") != NULL);
	T.ap_armed = 0;
	pump(200);
	CHECK_EQ_INT(call("clear"), 1);
	CHECK(strstr(cli_out, "active: autopilot") != NULL);
	T.ap_mode = 19;
	pump(200);
	clear_ok();
	/* Silence: no message from ArduSub for autopilot_silence (1 s here). */
	arm_ok();
	T.ap_frozen = 1;
	uint64_t t0 = now_ms();
	CHECK(wait_safety(2, 0, 3000));
	uint64_t dt = now_ms() - t0;
	CHECK(strstr(last_state.reason, "silent") != NULL);
	CHECK(dt >= 800 && dt < 2000);
	printf("    silence fired after %llu ms\n", (unsigned long long)dt);
	T.ap_frozen = 0;
	T.ap_armed = 0;
	pump(200);
	clear_ok();
}

static void test_triggers_and_actions(void)
{
	/* leak: surface, outputs on; the pilot's override and the mux carry it out. */
	arm_ok();
	T.leak_wet = 1;
	CHECK(wait_safety(2, 1, 1000));
	CHECK_EQ_INT(last_state.triggers, 1u << 2);
	CHECK(last_state.outputs_enabled);
	CHECK(strstr(last_state.reason, "water") != NULL);
	/* estop joins: neutral wins, both bits set, latched. */
	CHECK_EQ_INT(call("estop"), 0);
	CHECK(wait_safety(2, 0, 1000));
	CHECK_EQ_INT(last_state.triggers, (1u << 2) | (1u << 6));
	CHECK(!last_state.outputs_enabled);
	T.ap_armed = 0; /* the mavlink module disarms ArduSub when outputs go off */
	/* clear refused while the probe is wet; accepted once dry (estop is an
	 * event, not an active condition). */
	CHECK_EQ_INT(call("clear"), 1);
	CHECK(strstr(cli_out, "active: leak") != NULL);
	T.leak_wet = 0;
	pump(200);
	clear_ok();
	CHECK_EQ_INT(last_state.triggers, 0);
	/* battery: below battery_min for battery_time (0.5 s here), surface. */
	arm_ok();
	T.batt_v = 10.0f;
	uint64_t t0 = now_ms();
	CHECK(wait_safety(2, 1, 2000));
	uint64_t dt = now_ms() - t0;
	CHECK_EQ_INT(last_state.triggers, 1u << 3);
	CHECK(dt >= 400 && dt < 1500);
	printf("    battery fired after %llu ms\n", (unsigned long long)dt);
	T.batt_v = 16.0f;
	T.ap_armed = 0;
	pump(200);
	clear_ok();
	/* depth: beyond the mux's band by depth_margin (0.5 m). */
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "param", "set", "mux", "depth_max=5", (char *)NULL), 0);
	arm_ok();
	T.depth_m = 5.4f;
	pump(300);
	CHECK_EQ_INT(last_state.state, 1);
	T.depth_m = 5.6f;
	CHECK(wait_safety(2, 1, 1000));
	CHECK_EQ_INT(last_state.triggers, 1u << 4);
	CHECK(strstr(last_state.reason, "beyond") != NULL);
	T.depth_m = 1.0f;
	T.ap_armed = 0;
	pump(200);
	clear_ok();
	/* input: vehicle.leak stops; stale after three periods (300 ms); neutral. */
	arm_ok();
	T.publish_leak = 0;
	t0 = now_ms();
	CHECK(wait_safety(2, 0, 2000));
	dt = now_ms() - t0;
	CHECK_EQ_INT(last_state.triggers, 1u << 7);
	CHECK(strstr(last_state.reason, "vehicle.leak stale") != NULL);
	CHECK(dt >= 250 && dt < 1000);
	printf("    input fired after %llu ms\n", (unsigned long long)dt);
	T.publish_leak = 1;
	T.ap_armed = 0;
	pump(400);
	clear_ok();
	/* hold: with the depth controller not ready, neutral at once. */
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "param", "set", "safety", "action_leak=2", (char *)NULL), 0);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "stop", "depth_hold", (char *)NULL), 0);
	CHECK(wait_state("depth_hold", "stopped", 3000));
	pump(1200); /* the manager's modules round says so */
	arm_ok();
	T.leak_wet = 1;
	CHECK(wait_safety(2, 0, 1000));
	CHECK_EQ_INT(last_state.triggers, 1u << 2);
	T.leak_wet = 0;
	T.ap_armed = 0;
	pump(200);
	clear_ok();
	/* hold with the controller ready: outputs on for hold_time (1 s), then neutral. */
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "start", "depth_hold", (char *)NULL), 0);
	CHECK(wait_state("depth_hold", "ready", 3000));
	pump(1200);
	arm_ok();
	T.leak_wet = 1;
	t0 = now_ms();
	CHECK(wait_safety(2, 2, 1000));
	CHECK(last_state.outputs_enabled);
	CHECK(wait_safety(2, 0, 3000));
	dt = now_ms() - t0;
	CHECK(!last_state.outputs_enabled);
	CHECK(strstr(last_state.reason, "hold ended") != NULL);
	CHECK(dt >= 900 && dt < 2000);
	printf("    hold ended after %llu ms\n", (unsigned long long)dt);
	/* A disarm during failsafe is never refused: outputs stay off, the latch holds. */
	CHECK_EQ_INT(call("disarm"), 0);
	pump(100);
	CHECK_EQ_INT(last_state.state, 2);
	CHECK(!last_state.outputs_enabled);
	T.leak_wet = 0;
	T.ap_armed = 0;
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "param", "set", "safety", "action_leak=1", (char *)NULL), 0);
}

static void test_latch(void)
{
	/* Still in failsafe from the hold test. A restart comes back in it. */
	struct mstat before;
	CHECK(module_status("safety", &before) && before.pid > 0);
	uint32_t triggers = last_state.triggers;
	CHECK_EQ_INT(last_state.state, 2);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "restart", "safety", (char *)NULL), 0);
	CHECK(wait_new_pid("safety", before.pid, "ready", 5000));
	have_state = 0;
	CHECK(wait_safety(2, 0, 2000));
	/* The latched triggers are back; the inputs went stale while nothing
	 * pumped them during the restart, so input may have joined. */
	CHECK_EQ_INT(last_state.triggers & triggers, triggers);
	CHECK(strcmp(last_state.by, "latch") == 0);
	CHECK(!last_state.outputs_enabled);
	char path[700];
	tfmt(path, sizeof path, "%s/var/lib/hangar/safety/failsafe", T_root);
	CHECK(file_contains(path, "state 2"));
	CHECK_EQ_INT(call("arm"), 1);
	CHECK(strstr(cli_out, "clear first") != NULL);
	pump(300);
	clear_ok();
	CHECK(access(path, F_OK) < 0);
	/* And a clean restart starts disarmed. */
	module_status("safety", &before);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "restart", "safety", (char *)NULL), 0);
	CHECK(wait_new_pid("safety", before.pid, "ready", 5000));
	have_state = 0;
	CHECK(wait_safety(0, -1, 2000));
	CHECK(strcmp(last_state.reason, "start") == 0);
}

static void test_module_trigger(void)
{
	/* A module given up with OnGiveUp=failsafe: the system's failsafe is the
	 * module trigger, in every state. clear asks the manager, which refuses
	 * while the module is failed and accepts once it runs again. */
	manifest("crasher", "[Module]\nExec=%s/crasher --topic test.crash --rate 2 --crash-after %d --exit-code 1\n"
			    "[Publish test.crash]\nType=depth\nRate=2\n"
			    "[Restart]\nBackoff=0.1s..0.2s\nBurst=1/60s\nOnGiveUp=failsafe\n", T_toys, 2);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "reload", (char *)NULL), 0);
	uint64_t t0 = now_ms();
	while (strcmp(system_state(), "failsafe") != 0 && now_ms() - t0 < 10000)
		pump(50);
	CHECK(strcmp(system_state(), "failsafe") == 0);
	CHECK(wait_safety(2, 0, 3000));
	CHECK_EQ_INT(last_state.triggers, 1u << 0);
	CHECK(strstr(last_state.reason, "system failsafe") != NULL);
	CHECK_EQ_INT(call("clear"), 1);
	CHECK(strstr(cli_out, "crasher is failed") != NULL);
	/* The same module, now running long enough for the clear: a reload keeps
	 * it failed (the manifest changed, but a failed module is not started by
	 * a reload); hangar start runs it. */
	manifest("crasher", "[Module]\nExec=%s/crasher --topic test.crash --rate 2 --crash-after %d --exit-code 1\n"
			    "[Publish test.crash]\nType=depth\nRate=2\n"
			    "[Restart]\nBackoff=0.1s..0.2s\nBurst=1/60s\nOnGiveUp=failsafe\n", T_toys, 200);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "reload", (char *)NULL), 0);
	pump(300);
	/* A changed manifest is restarted by the reload; a failed module left
	 * unchanged would need hangar start. Either way it runs again. */
	struct mstat cm;
	if (!(module_status("crasher", &cm) && (strcmp(cm.state, "ready") == 0 || strcmp(cm.state, "starting") == 0)))
		CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "start", "crasher", (char *)NULL), 0);
	CHECK(wait_state("crasher", "ready", 3000));
	pump(300);
	clear_ok();
	CHECK(strcmp(system_state(), "nominal") == 0);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "stop", "crasher", (char *)NULL), 0);
	manifest_remove("crasher");
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "reload", (char *)NULL), 0);
}

static void test_allow(void)
{
	/* A module the list does not name is denied arm and clear; any module
	 * may disarm, estop and check. */
	const char *r = raw_call("arm");
	CHECK(strncmp(r, "HG_E_DENIED", 11) == 0);
	if (strncmp(r, "HG_E_DENIED", 11) != 0)
		printf("    raw arm: %s\n", r);
	r = raw_call("clear");
	CHECK(strncmp(r, "HG_E_DENIED", 11) == 0);
	r = raw_call("check");
	CHECK(strncmp(r, "HG_OK", 5) == 0);
	r = raw_call("disarm");
	CHECK(strncmp(r, "HG_OK", 5) == 0);
	r = raw_call("estop");
	CHECK(strncmp(r, "HG_OK", 5) == 0);
	CHECK(wait_safety(2, 0, 1000));
	CHECK_EQ_INT(last_state.triggers, 1u << 6);
	CHECK(strcmp(last_state.by, "rawcaller") == 0);
	clear_ok();
	/* set_params: admin is listed (used throughout); a module is not. */
	r = raw_call("set_params");
	CHECK(strncmp(r, "HG_E_DENIED", 11) == 0 || strncmp(r, "HG_E_BADTYPE", 12) == 0);
}

static void test_shutdown(void)
{
	hg_pub_close(P_ap);
	hg_pub_close(P_leak);
	hg_pub_close(P_bat);
	hg_pub_close(P_depth);
	CHECK_EQ_INT(manager_stop(mgr), 0);
	CHECK(file_contains(T_log, "safety: pid"));
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--raw") == 0) {
		/* The re-executed child: the environment is the parent's. */
		tfmt(T_control, sizeof T_control, "%s", getenv("HD_CONTROL") ? getenv("HD_CONTROL") : "");
		tfmt(T_root, sizeof T_root, "%s", getenv("HD_ROOT") ? getenv("HD_ROOT") : "");
		return raw_child(argc, argv);
	}
	T_safety = getenv("SAFETY_BIN");
	T_fake_mux = getenv("FAKE_MUX_BIN");
	if (!T_safety || !T_fake_mux) {
		fprintf(stderr, "SAFETY_BIN and FAKE_MUX_BIN must be set\n");
		return 2;
	}
	hd_setup("safety", NULL);
	setenv("HD_CONTROL", T_control, 1);
	setenv("HD_ROOT", T_root, 1);
	copy_types();
	RUN_TEST(test_start);
	RUN_TEST(test_arm_disarm);
	RUN_TEST(test_arm_refusals);
	RUN_TEST(test_autopilot_trigger);
	RUN_TEST(test_triggers_and_actions);
	RUN_TEST(test_latch);
	RUN_TEST(test_module_trigger);
	RUN_TEST(test_allow);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
