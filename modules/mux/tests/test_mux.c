// SPDX-License-Identifier: GPL-3.0-or-later
// The mux under a manager (docs/07 "Tests"): this process is `tester`, every
// control source at once (pilot.wrench, ctl.depth_hold, policy.p.wrench and
// .active, nav.depth, safety.state) and the caller of mux.set_mode. The
// per-axis choice in manual and assisted, the pilot's override and its 1 s
// hold, the envelope and the rate limit, the depth band with its nav.depth
// reference, the residual envelope, promotion by hash, a message from another
// model as no contribution, every demotion rule, the modes refused, the stale
// flag following the sources used and not the unused, the failsafe actions,
// mux.state. Under 30 s.
#include "hd_test.h"

#include <math.h>

#include "depth.h"
#include "hg_call.h"
#include "mux_state.h"
#include "policy_active.h"
#include "safety_state.h"
#include "set_mode_rep.h"
#include "set_mode_req.h"
#include "wrench.h"

static pid_t mgr;
static const char *T_mux;
static hg_pub *P_pilot, *P_ctl, *P_policy, *P_active, *P_depth, *P_safety;
static hg_sub *S_cmd, *S_state;
static struct wrench last_cmd;
static struct hg_info last_cmd_info;
static struct mux_state last_st;
static int have_cmd, have_st;

static struct {
	int publish_pilot, publish_ctl, publish_policy, publish_depth;
	struct wrench pilot, ctl, policy;
	int ctl_stale, policy_stale;
	uint64_t policy_ref_seq;      /* 0: the newest active; else that one */
	float depth_m;
	uint8_t safety_state, safety_action, safety_enabled;
	uint64_t next_ns;
} T = { .publish_pilot = 1, .publish_depth = 1, .depth_m = 2.0f, .safety_state = 1, .safety_enabled = 1 };

static void copy_types(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *types[] = { "mux_state", "mux_params", "policy_active", "set_mode_req", "set_mode_rep", NULL };
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
	manifest("mux", "[Module]\nExec=%s\n"
			"[Subscribe pilot.wrench]\n[Subscribe ctl.depth_hold]\n[Subscribe nav.depth]\n[Subscribe safety.state]\n"
			"[Subscribe policy.p.wrench]\n[Subscribe policy.p.active]\n"
			"[Publish ctl.wrench]\nType=wrench\nRate=50\nSources=pilot.wrench ctl.depth_hold policy.p.wrench nav.depth\n"
			"[Publish mux.state]\nType=mux_state\nRate=10\n"
			"[Params]\nType=mux_params\nAllow=tester admin\n"
			"[Serve set_mode]\nRequest=set_mode_req\nReply=set_mode_rep\nAllow=tester admin\n"
			"[Restart]\nOnGiveUp=failsafe\nBackoff=0.1s..0.2s\n", T_mux);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe ctl.wrench]\n[Subscribe mux.state]\n"
			   "[Publish pilot.wrench]\nType=wrench\nRate=50\n"
			   "[Publish ctl.depth_hold]\nType=wrench\nRate=50\n"
			   "[Publish policy.p.active]\nType=policy_active\n"
			   "[Publish policy.p.wrench]\nType=wrench\nRate=50\nInputs=policy.p.active\n"
			   "[Publish nav.depth]\nType=depth\nRate=50\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Call mux.set_mode]\n");
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
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
}

static void read_outputs(void)
{
	struct wrench w;
	struct hg_info info;
	while (hg_next(S_cmd, &w, sizeof w, &info) == HG_MSG) {
		last_cmd = w;
		last_cmd_info = info;
		have_cmd = 1;
	}
	struct mux_state st;
	while (hg_next(S_state, &st, sizeof st, &info) == HG_MSG) {
		last_st = st;
		have_st = 1;
	}
}

/* Publishes the sources at 50 Hz (safety.state every tick as well, which is
 * more than its 10 Hz and harmless) and reads the outputs, for ms. */
static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	do {
		uint64_t now = hg_now_ns();
		if (now >= T.next_ns) {
			T.next_ns = now + 20000000u;
			if (T.publish_pilot)
				hg_publish(P_pilot, &T.pilot, sizeof T.pilot, now);
			if (T.publish_ctl)
				hg_publish_with(P_ctl, &T.ctl, sizeof T.ctl, now, T.ctl_stale ? HG_STALE : 0, NULL, 0);
			if (T.publish_policy) {
				if (T.policy_ref_seq) {
					struct hg_input ref = { .topic_id = hg_topic_id("policy.p.active"), .seq = T.policy_ref_seq };
					hg_publish_with(P_policy, &T.policy, sizeof T.policy, now, T.policy_stale ? HG_STALE : 0, &ref, 1);
				} else {
					hg_publish_with(P_policy, &T.policy, sizeof T.policy, now, T.policy_stale ? HG_STALE : 0, NULL, 0);
				}
			}
			if (T.publish_depth) {
				struct depth d = { .depth_m = T.depth_m, .temp_c = 10, .pressure_mbar = 1013 };
				hg_publish(P_depth, &d, sizeof d, now);
			}
			struct safety_state s;
			memset(&s, 0, sizeof s);
			s.state = T.safety_state;
			s.action = T.safety_action;
			s.outputs_enabled = T.safety_enabled;
			hg_publish(P_safety, &s, sizeof s, now);
		}
		hg_sub *subs[2] = { S_cmd, S_state };
		hg_wait(subs, 2, 5);
		read_outputs();
	} while (hg_now_ns() < end);
}

static void publish_active(uint64_t hash, uint8_t cause)
{
	struct policy_active a;
	memset(&a, 0, sizeof a);
	a.version = 1;
	a.cause = cause;
	a.hash = hash;
	a.swapped_ns = hg_now_ns();
	tfmt(a.dataset, sizeof a.dataset, "depth_hold");
	hg_publish(P_active, &a, sizeof a, hg_now_ns());
}

static hg_call_result set_mode(const char *mode, uint64_t hash)
{
	set_mode_req req;
	memset(&req, 0, sizeof req);
	tfmt(req.mode, sizeof req.mode, "%s", mode);
	req.hash = hash;
	set_mode_rep rep;
	hg_pending *pc;
	hg_call_result r = { .status = HG_E_NOROUTE };
	int st = hg_call_start("mux.set_mode", &SET_MODE_REQ_TYPE, &req, &SET_MODE_REP_TYPE, &pc, 2000);
	if (st != HG_OK) {
		r.status = st;
		return r;
	}
	do {
		pump(10);
		r = hg_call_poll(pc, &rep);
	} while (r.status == HG_PENDING);
	if (r.status == HG_OK)
		tfmt(r.reason, sizeof r.reason, "%s", rep.mode);
	return r;
}

static int near(float a, float b)
{
	return fabsf(a - b) < 0.02f;
}

static void zero_pilot(void)
{
	memset(&T.pilot, 0, sizeof T.pilot);
}

/* ---- Tests ----------------------------------------------------------------- */

static void test_start(void)
{
	write_manifests();
	mgr = manager_start();
	CHECK(wait_state("tester", "stopped", 5000));
	become("tester");
	S_cmd = hg_sub_open("ctl.wrench", &WRENCH_TYPE);
	S_state = hg_sub_open("mux.state", &MUX_STATE_TYPE);
	P_pilot = hg_pub_open("pilot.wrench", &WRENCH_TYPE, NULL);
	P_ctl = hg_pub_open("ctl.depth_hold", &WRENCH_TYPE, NULL);
	P_active = hg_pub_open("policy.p.active", &POLICY_ACTIVE_TYPE, NULL);
	P_policy = hg_pub_open("policy.p.wrench", &WRENCH_TYPE, NULL);
	P_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);
	P_safety = hg_pub_open("safety.state", &SAFETY_STATE_TYPE, NULL);
	CHECK(S_cmd && S_state && P_pilot && P_ctl && P_active && P_policy && P_depth && P_safety);
	/* The mux is ready once ctl.wrench and mux.state have published. */
	CHECK(wait_state("mux", "ready", 5000));
	pump(300);
	CHECK(have_cmd && have_st);
	CHECK(strcmp(last_st.mode, "manual") == 0);
	CHECK(near(last_cmd.surge, 0) && near(last_cmd.heave, 0));
	/* 50 Hz: about 15 messages in 300 ms. */
	uint64_t s0 = last_cmd_info.seq;
	pump(400);
	uint64_t n = last_cmd_info.seq - s0;
	CHECK(n >= 15 && n <= 25);
	printf("    %llu ctl.wrench messages in 400 ms\n", (unsigned long long)n);
}

static void test_manual_and_envelope(void)
{
	/* The pilot drives every axis; the controller is ignored in manual. */
	T.pilot.surge = 0.5f;
	T.pilot.yaw = -0.25f;
	T.publish_ctl = 1;
	T.ctl.heave = 0.4f;
	pump(300);
	CHECK(near(last_cmd.surge, 0.5f) && near(last_cmd.yaw, -0.25f) && near(last_cmd.heave, 0));
	CHECK_EQ_INT(last_st.source_surge, 1);
	CHECK_EQ_INT(last_st.source_heave, 1);
	CHECK_EQ_INT(last_st.limits, 0);
	/* Only the pilot is referenced. */
	CHECK_EQ_INT(last_cmd_info.n_inputs, 1);
	CHECK_EQ_INT(last_cmd_info.inputs[0].topic_id, hg_topic_id("pilot.wrench"));
	CHECK(!(last_cmd_info.flags & HG_SLOT_STALE));
	/* The envelope: 1.5 is held at max (1), and the limit bit says so. */
	T.pilot.surge = 1.5f;
	pump(300);
	CHECK(near(last_cmd.surge, 1.0f));
	CHECK(last_st.limits & 1u);
	/* The rate limit: a step from 1 to -1 takes 2 / rate_max (8/s) = 250 ms. */
	T.pilot.surge = -1.0f;
	pump(60);
	CHECK(last_cmd.surge > -0.9f && last_cmd.surge < 0.9f);
	CHECK(last_st.limits & (1u << 6));
	pump(400);
	CHECK(near(last_cmd.surge, -1.0f));
	zero_pilot();
	pump(300);
	CHECK(near(last_cmd.surge, 0));
	/* A stale pilot counts as zero and is not referenced; the output is unflagged. */
	T.pilot.surge = 0.5f;
	pump(200);
	T.publish_pilot = 0;
	pump(300);
	CHECK(near(last_cmd.surge, 0));
	CHECK_EQ_INT(last_st.source_surge, 0);
	CHECK_EQ_INT(last_cmd_info.n_inputs, 0);
	CHECK(!(last_cmd_info.flags & HG_SLOT_STALE));
	T.publish_pilot = 1;
	zero_pilot();
	pump(200);
}

static void test_assisted_and_override(void)
{
	hg_call_result r = set_mode("assisted", 0);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(strcmp(r.reason, "assisted") == 0);
	/* Heave from the controller while the pilot leaves it alone; surge the pilot's. */
	T.ctl.heave = 0.4f;
	T.pilot.surge = 0.3f;
	pump(300);
	CHECK(near(last_cmd.heave, 0.4f) && near(last_cmd.surge, 0.3f));
	CHECK_EQ_INT(last_st.source_heave, 2);
	CHECK_EQ_INT(last_st.source_surge, 1);
	CHECK_EQ_INT(last_cmd_info.n_inputs, 2);
	/* The pilot's heave above the deadband takes the axis at once... */
	T.pilot.heave = -0.3f;
	pump(200);
	CHECK(near(last_cmd.heave, -0.3f));
	CHECK_EQ_INT(last_st.source_heave, 1);
	/* ...and keeps it for 1 s after letting go. */
	T.pilot.heave = 0;
	uint64_t t0 = now_ms();
	pump(500);
	CHECK(near(last_cmd.heave, 0));
	CHECK_EQ_INT(last_st.source_heave, 1);
	while (last_st.source_heave != 2 && now_ms() - t0 < 3000)
		pump(20);
	uint64_t dt = now_ms() - t0;
	CHECK_EQ_INT(last_st.source_heave, 2);
	CHECK(dt >= 900 && dt < 1500);
	printf("    override held %llu ms after release\n", (unsigned long long)dt);
	pump(200);
	CHECK(near(last_cmd.heave, 0.4f));
	/* A stale controller (computed from stale inputs) flags ctl.wrench while
	 * it drives; the pilot's axes alone do not. */
	T.ctl_stale = 1;
	pump(200);
	CHECK(last_cmd_info.flags & HG_SLOT_STALE);
	T.ctl_stale = 0;
	pump(200);
	CHECK(!(last_cmd_info.flags & HG_SLOT_STALE));
	/* The controller stops: its axis falls back to the pilot, nothing flagged. */
	T.publish_ctl = 0;
	pump(300);
	CHECK_EQ_INT(last_st.source_heave, 1);
	CHECK(near(last_cmd.heave, 0));
	CHECK(!(last_cmd_info.flags & HG_SLOT_STALE));
	T.publish_ctl = 1;
	pump(200);
	CHECK_EQ_INT(last_st.source_heave, 2);
}

static void test_depth_band(void)
{
	/* depth_max 5: at 6 m a command pushing down (heave < 0) is zeroed, the
	 * limit bit set and nav.depth referenced; a command up passes. */
	char out[2048];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "mux", "depth_max=5", (char *)NULL), 0);
	T.depth_m = 6.0f;
	T.pilot.heave = -0.5f;
	pump(300);
	CHECK(near(last_cmd.heave, 0));
	CHECK(last_st.limits & (1u << 12));
	int depth_ref = 0;
	for (uint32_t i = 0; i < last_cmd_info.n_inputs; i++)
		if (last_cmd_info.inputs[i].topic_id == hg_topic_id("nav.depth"))
			depth_ref = 1;
	CHECK(depth_ref);
	T.pilot.heave = 0.5f;
	pump(300);
	CHECK(near(last_cmd.heave, 0.5f));
	CHECK(!(last_st.limits & (1u << 12)));
	T.depth_m = 2.0f;
	T.pilot.heave = 0;
	pump(1200); /* the override hold ends */
}

static void test_residual(void)
{
	/* No running model: residual refused. A wrong hash: refused. */
	hg_call_result r = set_mode("residual:p", 0x1234);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "no running model") != NULL);
	publish_active(0xabcdef, 1);
	T.publish_policy = 1;
	T.policy.heave = 0.5f;
	pump(200);
	r = set_mode("residual:p", 0x1234);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "not the running model") != NULL);
	r = set_mode("residual:q", 0xabcdef);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	/* The right hash: promoted. The contribution is held within 20% of the
	 * envelope (0.2 on heave): 0.4 + 0.2 = 0.6, the residual limit bit set. */
	r = set_mode("residual:p", 0xabcdef);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(strcmp(r.reason, "residual:p") == 0);
	pump(300);
	CHECK(near(last_cmd.heave, 0.6f));
	CHECK_EQ_INT(last_st.source_heave, 4);
	CHECK_EQ_INT(last_st.residual_active, 1);
	CHECK(last_st.limits & (1u << (13 + 2)));
	CHECK(strcmp(last_st.mode, "residual:p") == 0);
	int policy_ref = 0;
	for (uint32_t i = 0; i < last_cmd_info.n_inputs; i++)
		if (last_cmd_info.inputs[i].topic_id == hg_topic_id("policy.p.wrench"))
			policy_ref = 1;
	CHECK(policy_ref);
	/* Within the residual envelope: added as is. */
	T.policy.heave = 0.1f;
	pump(300);
	CHECK(near(last_cmd.heave, 0.5f));
	CHECK(!(last_st.limits & (1u << 15)));
	/* The pilot's override wins over both. */
	T.pilot.heave = 0.3f;
	pump(200);
	CHECK(near(last_cmd.heave, 0.3f));
	CHECK_EQ_INT(last_st.source_heave, 1);
	T.pilot.heave = 0;
	pump(1300);
	CHECK_EQ_INT(last_st.source_heave, 4);
	/* A new active message (a load) with the same hash: the model swapped in
	 * drives without demotion. */
	publish_active(0xabcdef, 1);
	pump(300);
	CHECK(strcmp(last_st.mode, "residual:p") == 0);
	CHECK(near(last_cmd.heave, 0.5f));
	/* A message referencing the old active message is no contribution, and
	 * no demotion: the mode stays, the controller drives alone. */
	T.policy_ref_seq = hg_pub_next_seq(P_active) - 2;
	pump(300);
	CHECK(strcmp(last_st.mode, "residual:p") == 0);
	CHECK(near(last_cmd.heave, 0.4f));
	CHECK_EQ_INT(last_st.residual_active, 0);
	T.policy_ref_seq = 0;
	pump(300);
	CHECK(near(last_cmd.heave, 0.5f));
	/* A model with another hash swapped in by a load drives in its place;
	 * a later promotion names that model. */
	publish_active(0x777777, 1);
	pump(300);
	CHECK(strcmp(last_st.mode, "residual:p") == 0);
	CHECK(near(last_cmd.heave, 0.5f));
	const uint64_t running = 0x777777;
	/* Demotion: the output flagged as computed from stale inputs. */
	T.policy_stale = 1;
	pump(200);
	CHECK(strcmp(last_st.mode, "assisted") == 0);
	CHECK(near(last_cmd.heave, 0.4f));
	T.policy_stale = 0;
	struct mstat m;
	module_status("mux", &m);
	CHECK(strstr(m.status, "demoted") != NULL && strstr(m.status, "stale") != NULL);
	pump(100); /* the status round trip paused the sources: fresh again before promoting */
	/* Demotion: the policy stops publishing (skips a tick). */
	r = set_mode("residual:p", running);
	CHECK_EQ_INT(r.status, HG_OK);
	pump(200);
	CHECK(strcmp(last_st.mode, "residual:p") == 0);
	T.publish_policy = 0;
	pump(200);
	CHECK(strcmp(last_st.mode, "assisted") == 0);
	T.publish_policy = 1;
	/* Demotion: not finite. */
	r = set_mode("residual:p", running);
	CHECK_EQ_INT(r.status, HG_OK);
	pump(100);
	T.policy.heave = NAN;
	pump(200);
	CHECK(strcmp(last_st.mode, "assisted") == 0);
	T.policy.heave = 0.1f;
	/* Demotion: held at the residual envelope for more than demote_saturation
	 * (20%) of demote_window (1 s here). */
	char out[2048];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "mux", "demote_window=1", (char *)NULL), 0);
	r = set_mode("residual:p", running);
	CHECK_EQ_INT(r.status, HG_OK);
	pump(300);
	CHECK(strcmp(last_st.mode, "residual:p") == 0);
	T.policy.heave = 0.9f;
	uint64_t t0 = now_ms();
	while (strcmp(last_st.mode, "assisted") != 0 && now_ms() - t0 < 3000)
		pump(20);
	uint64_t dt = now_ms() - t0;
	CHECK(strcmp(last_st.mode, "assisted") == 0);
	CHECK(dt >= 150 && dt < 1000);
	printf("    saturation demotion after %llu ms\n", (unsigned long long)dt);
	T.policy.heave = 0.1f;
	/* A policy's output is never applied outside residual: assisted now. */
	T.policy.heave = 0.9f;
	pump(300);
	CHECK(near(last_cmd.heave, 0.4f));
	CHECK_EQ_INT(last_st.residual_active, 0);
	/* Later modes refused. */
	r = set_mode("auto", 0);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "later") != NULL);
	r = set_mode("policy:p", 0xabcdef);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	r = set_mode("nonsense", 0);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strcmp(last_st.mode, "assisted") == 0);
}

static void test_failsafe_actions(void)
{
	/* surface: heave up at surface_thrust (0.5), every other axis zero,
	 * whatever the sources say; the pilot's override holds. */
	T.pilot.surge = 0.4f;
	T.ctl.heave = -0.3f;
	T.safety_state = 2;
	T.safety_action = 1;
	T.safety_enabled = 1;
	pump(400);
	CHECK(near(last_cmd.heave, 0.5f));
	CHECK(near(last_cmd.surge, 0.4f));
	CHECK_EQ_INT(last_st.source_surge, 1);
	CHECK_EQ_INT(last_st.source_heave, 0);
	T.pilot.surge = 0;
	pump(1200);
	CHECK(near(last_cmd.surge, 0));
	/* hold: heave from the depth controller while it is fresh, else zero. */
	T.safety_action = 2;
	pump(300);
	CHECK(near(last_cmd.heave, -0.3f));
	CHECK_EQ_INT(last_st.source_heave, 2);
	T.publish_ctl = 0;
	pump(300);
	CHECK(near(last_cmd.heave, 0));
	T.publish_ctl = 1;
	/* neutral: everything zero. */
	T.safety_action = 0;
	T.safety_enabled = 0;
	T.pilot.yaw = 0.5f;
	pump(300);
	CHECK(near(last_cmd.heave, 0) && near(last_cmd.yaw, 0.5f)); /* the override still passes; the gate is closed */
	T.pilot.yaw = 0;
	T.safety_state = 1;
	T.safety_enabled = 1;
	pump(200);
}

static void test_shutdown(void)
{
	CHECK_EQ_INT(manager_stop(mgr), 0);
	CHECK(file_contains(T_log, "mux: pid"));
}

int main(void)
{
	T_mux = getenv("MUX_BIN");
	if (!T_mux) {
		fprintf(stderr, "MUX_BIN must be set\n");
		return 2;
	}
	hd_setup("mux", NULL);
	copy_types();
	RUN_TEST(test_start);
	RUN_TEST(test_manual_and_envelope);
	RUN_TEST(test_assisted_and_override);
	RUN_TEST(test_depth_band);
	RUN_TEST(test_residual);
	RUN_TEST(test_failsafe_actions);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
