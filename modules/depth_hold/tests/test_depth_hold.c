// SPDX-License-Identifier: GPL-3.0-or-later
// depth_hold: first the controller alone against a simulated vehicle (16.5 kg
// with added mass, 160 N of vertical thrust at full heave behind a 0.1 s lag,
// linear and quadratic drag, a buoyancy step): a 1 m step's rise, overshoot
// and settling, the recovery from the buoyancy step, the target following the
// depth while it does not drive and a bumpless handover. Then the chain under
// a manager: nav, depth_hold and the mux, this process as the vehicle (it
// turns ctl.wrench into motion and the motion into nav.depth, nav.attitude and
// nav.imu) and as the pilot and safety: assisted holds the depth the pilot
// leaves it at, a pilot override and the new depth held after it, a set
// target reached, a failsafe hold in manual mode, a stale depth flagging the
// mux's command, residual mode (the controller still driving under a policy's
// residual, a set target reached), the idle CPU. Under 60 s.
#include "hd_test.h"

#include <math.h>

#include "attitude.h"
#include "depth.h"
#include "depth_set_req.h"
#include "depth_target.h"
#include "dh_ctl.h"
#include "empty.h"
#include "hg_call.h"
#include "imu.h"
#include "mux_state.h"
#include "policy_active.h"
#include "safety_state.h"
#include "set_mode_rep.h"
#include "set_mode_req.h"
#include "wrench.h"

/* ---- The simulated vehicle ------------------------------------------------- */

struct plant {
	double z, v;        /* m, m/s, positive down */
	double thrust;      /* N, positive up: the lagged response to the command */
	double net_weight;  /* N, positive down: weight minus buoyancy */
	double a;           /* m/s^2, the last acceleration, positive down */
};

#define MASS 16.5      /* kg, with added mass */
#define FULL 160.0     /* N of vertical thrust at heave 1 */
#define LAG 0.1        /* s, the thrusters' first-order lag */

/* dt seconds under a heave command (up positive), integrated at 1 ms. */
static void plant_step(struct plant *p, double heave, double dt)
{
	int n = (int)(dt / 0.001 + 0.5);
	for (int i = 0; i < n; i++) {
		double h = 0.001;
		p->thrust += (FULL * heave - p->thrust) * h / LAG;
		double drag = 8.0 * p->v + 20.0 * p->v * fabs(p->v);
		p->a = (p->net_weight - p->thrust - drag) / MASS;
		p->v += p->a * h;
		p->z += p->v * h;
	}
}

/* The defaults of msg/depth_control_params.msg. */
static const struct dh_gains G = { .kp = 4, .kd = 4, .accel_max = 1, .ff = 0.1, .ki = 0.1, .i_max = 0.3,
				   .i_band = 0.3, .max_thrust = 0.6 };

/* ---- The controller alone ------------------------------------------------------- */

static void test_step_response(void)
{
	struct plant p = { .z = 2.0, .net_weight = 1.0 };
	struct dh_ctl c;
	dh_init(&c);
	/* Hold 2 m for 20 s, enough for the integrator to find the 1 N. */
	for (int k = 0; k < 1000; k++) {
		struct dh_out o = dh_step(&c, &G, p.z, p.v, 1, 1, 0.02);
		plant_step(&p, o.heave, 0.02);
	}
	CHECK(fabs(p.z - 2.0) < 0.02);
	/* The step: 3 m. */
	dh_set(&c, 3.0);
	double t10 = -1, t90 = -1, peak = p.z, settle = 0;
	for (int k = 0; k < 1500; k++) {
		double t = (k + 1) * 0.02;
		struct dh_out o = dh_step(&c, &G, p.z, p.v, 1, 1, 0.02);
		plant_step(&p, o.heave, 0.02);
		if (t10 < 0 && p.z >= 2.1)
			t10 = t;
		if (t90 < 0 && p.z >= 2.9)
			t90 = t;
		if (p.z > peak)
			peak = p.z;
		if (fabs(p.z - 3.0) > 0.05)
			settle = t;
	}
	double overshoot = (peak - 3.0) * 100;
	printf("    a 1 m step: rise (10-90 %%) %.2f s, overshoot %.1f %%, settled within 5 cm after %.2f s\n",
	       t90 - t10, overshoot, settle);
	CHECK(t10 > 0 && t90 > 0);
	CHECK(t90 - t10 < 4.0);
	CHECK(overshoot < 10);
	CHECK(settle < 10);
	CHECK(fabs(p.z - 3.0) < 0.02);

	/* Buoyancy step: the vehicle 7 N lighter (a weight dropped) while holding. */
	p.net_weight = -6.0;
	double worst = 0, back = 0;
	for (int k = 0; k < 2000; k++) {
		double t = (k + 1) * 0.02;
		struct dh_out o = dh_step(&c, &G, p.z, p.v, 1, 1, 0.02);
		plant_step(&p, o.heave, 0.02);
		if (fabs(p.z - 3.0) > worst)
			worst = fabs(p.z - 3.0);
		if (fabs(p.z - 3.0) > 0.05)
			back = t;
	}
	printf("    a 7 N buoyancy step: %.3f m at worst, back within 5 cm after %.2f s, integrator %+.3f\n",
	       worst, back, c.integral);
	CHECK(worst < 0.5);
	CHECK(back < 30);
	CHECK(fabs(p.z - 3.0) < 0.05);
}

static void test_follow_and_handover(void)
{
	struct plant p = { .z = 4.0, .net_weight = 0.0 };
	struct dh_ctl c;
	dh_init(&c);
	/* Not driving: the pilot sinks the vehicle at 0.3 m/s; the target follows,
	 * the integrator learns nothing. */
	for (int k = 0; k < 250; k++) {
		struct dh_out o = dh_step(&c, &G, p.z, p.v, 0, 1, 0.02);
		(void)o;
		p.v = 0.3;
		p.z += 0.3 * 0.02;
		CHECK(fabs(c.target - p.z) < 0.01);
	}
	CHECK(c.integral == 0);
	/* Handed back at 5.5 m: the controller brakes and holds about where it got the axis. */
	double handed = p.z;
	struct dh_out first = dh_step(&c, &G, p.z, p.v, 1, 1, 0.02);
	CHECK(fabs(c.target - handed) < 0.01);
	CHECK(first.heave > 0); /* sinking: push up to brake */
	for (int k = 0; k < 1000; k++) {
		struct dh_out o = dh_step(&c, &G, p.z, p.v, 1, 1, 0.02);
		plant_step(&p, o.heave, 0.02);
	}
	printf("    handed the axis at %.2f m sinking 0.3 m/s: holds %.2f m\n", handed, p.z);
	CHECK(fabs(p.z - handed) < 0.3);
	CHECK(fabs(p.v) < 0.01);
	/* A set target while not driving stays until it next stops driving. */
	dh_set(&c, 6.0);
	dh_step(&c, &G, p.z, p.v, 0, 1, 0.02);
	CHECK(c.target == 6.0);
	dh_step(&c, &G, p.z, p.v, 1, 1, 0.02);
	CHECK(c.target == 6.0);
	dh_step(&c, &G, p.z, p.v, 0, 1, 0.02); /* stops driving: follows */
	CHECK(fabs(c.target - p.z) < 1e-9);
	/* Hold the current depth. */
	dh_set(&c, 9.0);
	dh_hold_current(&c);
	dh_step(&c, &G, p.z, p.v, 1, 1, 0.02);
	CHECK(fabs(c.target - p.z) < 1e-9);
	/* Outputs disabled: the integrator learns nothing even while driving. */
	double i0 = c.integral;
	for (int k = 0; k < 50; k++)
		dh_step(&c, &G, p.z + 1.0, 0, 1, 0, 0.02);
	CHECK(c.integral == i0);
}

/* ---- The chain under a manager ------------------------------------------------------ */

static pid_t mgr;
static const char *T_dh, *T_nav, *T_mux;
static hg_pub *P_depth, *P_att, *P_imu, *P_pilot, *P_safety, *P_active, *P_policy;
static hg_sub *S_cmd, *S_mux, *S_ctl, *S_target;

static struct {
	struct plant p;
	struct wrench pilot;
	uint8_t safety_state, safety_action, safety_enabled;
	int publish_depth;
	float residual;          /* policy.p.wrench's heave, published while policy_ref_seq */
	uint64_t policy_ref_seq; /* the policy.p.active message it names */
	uint64_t next_ns;
	struct wrench cmd;
	struct hg_info cmd_info, ctl_info;
	struct mux_state mux;
	struct depth_target target;
	int have_cmd, have_mux, have_target;
} V;

static void copy_types(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *types[] = { "attitude",   "nav_state",    "nav_params",   "depth_target", "depth_control_params",
				"depth_set_req", "mux_state", "mux_params", "set_mode_req", "set_mode_rep", "policy_active",
				NULL };
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

/* The vehicle at 50 Hz: ctl.wrench's heave moves it (only while safety says
 * outputs are enabled, as the gate would), its depth and a level attitude go
 * out, with the IMU's specific force from its acceleration. */
static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	do {
		uint64_t now = hg_now_ns();
		if (now >= V.next_ns) {
			V.next_ns = now + 20000000u;
			int gate = V.have_cmd && V.safety_enabled && !(V.cmd_info.flags & HG_SLOT_STALE);
			plant_step(&V.p, gate ? V.cmd.heave : 0.0, 0.02);
			if (V.publish_depth) {
				struct depth d = { .depth_m = (float)V.p.z, .temp_c = 10, .pressure_mbar = 1013 };
				hg_publish(P_depth, &d, sizeof d, now);
			}
			struct attitude a;
			memset(&a, 0, sizeof a);
			a.q[0] = 1;
			hg_publish(P_att, &a, sizeof a, now);
			struct imu m;
			memset(&m, 0, sizeof m);
			m.accel[2] = (float)(V.p.a - 9.80665);
			hg_publish(P_imu, &m, sizeof m, now);
			hg_publish(P_pilot, &V.pilot, sizeof V.pilot, now);
			if (V.policy_ref_seq) {
				struct wrench r;
				memset(&r, 0, sizeof r);
				r.heave = V.residual;
				struct hg_input ref = { .topic_id = hg_topic_id("policy.p.active"), .seq = V.policy_ref_seq };
				hg_publish_with(P_policy, &r, sizeof r, now, 0, &ref, 1);
			}
			struct safety_state s;
			memset(&s, 0, sizeof s);
			s.state = V.safety_state;
			s.action = V.safety_action;
			s.outputs_enabled = V.safety_enabled;
			hg_publish(P_safety, &s, sizeof s, now);
		}
		hg_sub *subs[2] = { S_cmd, S_mux };
		hg_wait(subs, 2, 5);
		struct hg_info info;
		while (hg_next(S_cmd, &V.cmd, sizeof V.cmd, &info) == HG_MSG) {
			V.cmd_info = info;
			V.have_cmd = 1;
		}
		while (hg_next(S_mux, &V.mux, sizeof V.mux, &info) == HG_MSG)
			V.have_mux = 1;
		struct wrench w;
		while (hg_next(S_ctl, &w, sizeof w, &info) == HG_MSG)
			V.ctl_info = info;
		while (hg_next(S_target, &V.target, sizeof V.target, &info) == HG_MSG)
			V.have_target = 1;
	} while (hg_now_ns() < end);
}

static hg_call_result call(const char *target, const struct hg_type *rq, const void *req,
			   const struct hg_type *rp, void *rep)
{
	hg_pending *pc;
	hg_call_result r = { .status = HG_E_NOROUTE };
	int st = hg_call_start(target, rq, req, rp, &pc, 2000);
	if (st != HG_OK) {
		r.status = st;
		return r;
	}
	do {
		pump(10);
		r = hg_call_poll(pc, rep);
	} while (r.status == HG_PENDING);
	return r;
}

static int set_mode_hash(const char *mode, uint64_t hash)
{
	set_mode_req req;
	memset(&req, 0, sizeof req);
	tfmt(req.mode, sizeof req.mode, "%s", mode);
	req.hash = hash;
	set_mode_rep rep;
	return call("mux.set_mode", &SET_MODE_REQ_TYPE, &req, &SET_MODE_REP_TYPE, &rep).status;
}

static int set_mode(const char *mode)
{
	return set_mode_hash(mode, 0);
}

/* Waits until the vehicle is still (|v| < 1 cm/s) for 1 s, or timeout_ms. */
static int settle(long timeout_ms)
{
	uint64_t t0 = now_ms(), still = 0;
	while (now_ms() - t0 < (uint64_t)timeout_ms) {
		pump(100);
		if (fabs(V.p.v) < 0.01) {
			if (!still)
				still = now_ms();
			if (now_ms() - still >= 1000)
				return 1;
		} else {
			still = 0;
		}
	}
	return 0;
}

static void test_chain(void)
{
	manifest("nav", "[Module]\nExec=%s\n"
			"[Subscribe nav.depth]\n[Subscribe nav.attitude]\n[Subscribe nav.imu]\n"
			"[Publish nav.state]\nType=nav_state\nRate=50\nInputs=nav.depth\nSources=nav.attitude nav.imu\n"
			"[Params]\nType=nav_params\nAllow=tester admin\n", T_nav);
	manifest("depth_hold", "[Module]\nExec=%s\n"
			       "[Subscribe nav.state]\n[Subscribe mux.state]\n[Subscribe safety.state]\n"
			       "[Publish ctl.depth_hold]\nType=wrench\nRate=50\nInputs=nav.state\n"
			       "[Publish ctl.depth_hold.target]\nType=depth_target\nRate=50\nInputs=nav.state\n"
			       "[Params]\nType=depth_control_params\nAllow=tester admin\n"
			       "[Serve set]\nRequest=depth_set_req\nReply=empty\nAllow=tester admin\n"
			       "[Serve hold]\nRequest=empty\nReply=empty\nAllow=tester admin\n", T_dh);
	manifest("mux", "[Module]\nExec=%s\n"
			"[Subscribe pilot.wrench]\n[Subscribe ctl.depth_hold]\n[Subscribe nav.depth]\n[Subscribe safety.state]\n"
			"[Subscribe policy.p.wrench]\n[Subscribe policy.p.active]\n"
			"[Publish ctl.wrench]\nType=wrench\nRate=50\nSources=pilot.wrench ctl.depth_hold policy.p.wrench nav.depth\n"
			"[Publish mux.state]\nType=mux_state\nRate=10\n"
			"[Params]\nType=mux_params\nAllow=tester admin\n"
			"[Serve set_mode]\nRequest=set_mode_req\nReply=set_mode_rep\nAllow=tester admin\n", T_mux);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe ctl.wrench]\n[Subscribe mux.state]\n[Subscribe ctl.depth_hold]\n"
			   "[Subscribe ctl.depth_hold.target]\n"
			   "[Publish nav.depth]\nType=depth\nRate=50\nStaleAfter=200ms\n"
			   "[Publish nav.attitude]\nType=attitude\nRate=50\nStaleAfter=200ms\n"
			   "[Publish nav.imu]\nType=imu\nRate=50\nStaleAfter=200ms\n"
			   "[Publish pilot.wrench]\nType=wrench\nRate=50\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Publish policy.p.active]\nType=policy_active\n"
			   "[Publish policy.p.wrench]\nType=wrench\nRate=50\nInputs=policy.p.active\n"
			   "[Call mux.set_mode]\n[Call depth_hold.set]\n[Call depth_hold.hold]\n");
	mgr = manager_start();
	CHECK(wait_state("tester", "stopped", 5000));
	become("tester");
	S_cmd = hg_sub_open("ctl.wrench", &WRENCH_TYPE);
	S_mux = hg_sub_open("mux.state", &MUX_STATE_TYPE);
	S_ctl = hg_sub_open("ctl.depth_hold", &WRENCH_TYPE);
	S_target = hg_sub_open("ctl.depth_hold.target", &DEPTH_TARGET_TYPE);
	P_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);
	P_att = hg_pub_open("nav.attitude", &ATTITUDE_TYPE, NULL);
	P_imu = hg_pub_open("nav.imu", &IMU_TYPE, NULL);
	P_pilot = hg_pub_open("pilot.wrench", &WRENCH_TYPE, NULL);
	P_safety = hg_pub_open("safety.state", &SAFETY_STATE_TYPE, NULL);
	P_active = hg_pub_open("policy.p.active", &POLICY_ACTIVE_TYPE, NULL);
	P_policy = hg_pub_open("policy.p.wrench", &WRENCH_TYPE, NULL);
	CHECK(S_cmd && S_mux && S_ctl && S_target && P_depth && P_att && P_imu && P_pilot && P_safety && P_active &&
	      P_policy);
	/* Armed, at 2 m, 3 N lighter than water: left alone it rises. */
	V.p = (struct plant){ .z = 2.0, .net_weight = -3.0 };
	V.safety_state = 1;
	V.safety_enabled = 1;
	V.publish_depth = 1;
	pump(300);
	CHECK(wait_state("nav", "ready", 5000) && wait_state("depth_hold", "ready", 5000) &&
	      wait_state("mux", "ready", 5000));

	// Manual: the pilot has heave (source 1), depth_hold idle, its target following the depth.
	pump(1000);
	CHECK(V.have_mux && V.mux.source_heave == 1);
	CHECK(V.have_target && !V.target.driving && V.target.latched);
	CHECK(fabs(V.target.target_m - V.target.depth_m) < 0.01);
	CHECK(V.p.z < 2.0); /* rising, nobody holding */

	// Assisted: depth_hold takes heave and holds the depth it was handed.
	CHECK_EQ_INT(set_mode("assisted"), HG_OK);
	pump(300);
	double handed = V.target.target_m;
	CHECK(V.mux.source_heave == 2 && V.target.driving);
	CHECK(settle(8000));
	printf("    assisted, 3 N light: handed the axis at %.2f m, holds %.3f m\n", handed, V.p.z);
	CHECK(fabs(V.p.z - handed) < 0.15);
	pump(3000); /* the integrator trims the buoyancy out */
	CHECK(fabs(V.p.z - handed) < 0.05);

	// The pilot pushes down (heave -0.4) for 1.5 s: the pilot's; released, the new depth held.
	double before = V.p.z;
	V.pilot.heave = -0.4f;
	pump(1500);
	CHECK(V.mux.source_heave == 1);
	CHECK(V.target.latched);
	V.pilot.heave = 0;
	pump(1500); /* the mux's override hold (1 s), then the controller */
	CHECK(V.mux.source_heave == 2);
	double new_target = V.target.target_m;
	CHECK(settle(8000));
	printf("    the pilot pushed down from %.2f m and let go: the controller holds %.3f m (target %.3f)\n",
	       before, V.p.z, new_target);
	CHECK(new_target > before + 0.2);
	CHECK(fabs(V.p.z - new_target) < 0.15);

	// A set target: 1 m deeper.
	depth_set_req sreq = { .depth_m = (float)(new_target + 1.0) };
	empty erep;
	CHECK_EQ_INT(call("depth_hold.set", &DEPTH_SET_REQ_TYPE, &sreq, &EMPTY_TYPE, &erep).status, HG_OK);
	CHECK(settle(10000));
	printf("    set to %.2f m: holds %.3f m\n", (double)sreq.depth_m, V.p.z);
	CHECK(fabs(V.p.z - sreq.depth_m) < 0.1);
	// Out of range: refused.
	sreq.depth_m = 5000;
	CHECK_EQ_INT(call("depth_hold.set", &DEPTH_SET_REQ_TYPE, &sreq, &EMPTY_TYPE, &erep).status, HG_E_REFUSED);

	// A failsafe hold in manual mode: the mux gives depth_hold the heave, the depth is kept.
	CHECK_EQ_INT(set_mode("manual"), HG_OK);
	pump(1500); /* rising again, nobody holding */
	V.safety_state = 2;
	V.safety_action = 2;
	pump(300);
	double fs_at = V.target.target_m;
	CHECK(V.mux.source_heave == 2 && V.target.driving);
	CHECK(settle(8000));
	printf("    failsafe hold in manual: handed at %.2f m, holds %.3f m\n", fs_at, V.p.z);
	CHECK(fabs(V.p.z - fs_at) < 0.15);
	V.safety_state = 1;
	V.safety_action = 0;
	CHECK_EQ_INT(set_mode("assisted"), HG_OK);
	pump(500);

	// The depth stops while depth_hold drives: its command is flagged, and so the mux's.
	V.publish_depth = 0;
	pump(700);
	CHECK(V.ctl_info.flags & HG_SLOT_STALE);
	CHECK(V.cmd_info.flags & HG_SLOT_STALE);
	V.publish_depth = 1;
	pump(700);
	CHECK(!(V.ctl_info.flags & HG_SLOT_STALE));
	CHECK(!(V.cmd_info.flags & HG_SLOT_STALE));

	// Residual: the mux names the policy as the heave's source (4) and adds its
	// residual to the controller's heave. depth_hold still drives: its target
	// stays, a set target is reached, the integrator absorbs the residual.
	CHECK(settle(10000));
	struct policy_active act;
	memset(&act, 0, sizeof act);
	act.version = 1;
	act.cause = 1;
	act.hash = 0xabcdef;
	act.swapped_ns = hg_now_ns();
	CHECK_EQ_INT(hg_publish(P_active, &act, sizeof act, hg_now_ns()), HG_OK);
	V.policy_ref_seq = 1;
	V.residual = 0.1f; /* pushes down, within the residual envelope (0.2) */
	pump(300);
	double held = V.target.target_m;
	CHECK_EQ_INT(set_mode_hash("residual:p", 0xabcdef), HG_OK);
	pump(500);
	CHECK(strcmp(V.mux.mode, "residual:p") == 0);
	CHECK_EQ_INT(V.mux.source_heave, 4);
	CHECK(V.mux.residual_active);
	CHECK(V.target.driving && !V.target.latched);
	CHECK(fabs(V.target.target_m - held) < 1e-6);
	CHECK(fabs(V.cmd.heave - (V.target.integral + 0.1)) < 0.15); /* the residual is in the command */
	CHECK(settle(25000));
	pump(3000);
	printf("    residual:p (+0.10 heave added): target %.3f m kept, holds %.3f m\n", held, V.p.z);
	CHECK(fabs(V.p.z - held) < 0.05);
	sreq.depth_m = (float)(held - 0.5);
	CHECK_EQ_INT(call("depth_hold.set", &DEPTH_SET_REQ_TYPE, &sreq, &EMPTY_TYPE, &erep).status, HG_OK);
	CHECK(settle(10000));
	printf("    residual:p, set to %.2f m: holds %.3f m\n", (double)sreq.depth_m, V.p.z);
	CHECK(fabs(V.target.target_m - sreq.depth_m) < 1e-6);
	CHECK(fabs(V.p.z - sreq.depth_m) < 0.1);
	CHECK_EQ_INT(V.mux.source_heave, 4);
	V.policy_ref_seq = 0;

	// Idle CPU: both modules sleep between their ticks.
	struct mstat m1, m2;
	CHECK(module_status("depth_hold", &m1) && module_status("nav", &m2));
	long long c1 = th_cpu_ticks(m1.pid), c2 = th_cpu_ticks(m2.pid);
	uint64_t a = hg_now_ns();
	pump(1000);
	long ms = (long)((hg_now_ns() - a) / 1000000u);
	double p1 = th_cpu_percent(c1, th_cpu_ticks(m1.pid), ms), p2 = th_cpu_percent(c2, th_cpu_ticks(m2.pid), ms);
	printf("    depth_hold %.1f %%, nav %.1f %% of a core at 50 Hz\n", p1, p2);
	CHECK(p1 >= 0 && p1 < 10 && p2 >= 0 && p2 < 10);
}

static void test_shutdown(void)
{
	hg_pub_close(P_depth);
	hg_pub_close(P_att);
	hg_pub_close(P_imu);
	hg_pub_close(P_pilot);
	hg_pub_close(P_safety);
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

/* Levelling and heading hold: the two commands' arithmetic (dh_ctl.h). The
 * flight that needs them is modules/thrusters' (test_thrusters, test_sim_direct). */
static void test_level_and_heading(void)
{
	/* Rolled to the right, or pitched up: a command the other way; turning
	 * that way already, a larger one. */
	CHECK(dh_level(1, 0, 0.5f, 0.1, 0) < 0);
	CHECK(fabs(dh_level(1, 0, 0.5f, 0.1, 0) + 0.1) < 1e-6);
	CHECK(dh_level(1, 0.2f, 0.5f, 0.1, 0.5) < dh_level(1, 0.2f, 0.5f, 0.1, 0));
	CHECK(dh_level(1, 0, 0.5f, 0, 0) == 0);
	CHECK(dh_level(5, 0, 0.5f, 1, 0) == -0.5f && dh_level(5, 0, 0.5f, -1, 0) == 0.5f);
	CHECK(dh_level(1, 1, 0.5f, NAN, 0) == 0 && dh_level(1, 1, 0.5f, 0, INFINITY) == 0);
	CHECK(dh_level(0, 0, 0.5f, 1, 1) == 0);
	/* The heading: clockwise positive, the short way round, across north
	 * and across any number of whole turns. */
	const double pi = 3.14159265358979323846;
	CHECK(fabs(dh_heading(1, 0, 0.5f, 0.2, 0.1, 0) - 0.1) < 1e-6);
	CHECK(fabs(dh_heading(1, 0, 0.5f, 0.1, 0.2, 0) + 0.1) < 1e-6);
	CHECK(fabs(dh_heading(1, 0, 0.5f, -pi + 0.05, pi - 0.05, 0) - 0.1) < 1e-6);
	CHECK(fabs(dh_heading(1, 0, 0.5f, pi - 0.05, -pi + 0.05, 0) + 0.1) < 1e-6);
	CHECK(fabs(dh_heading(1, 0, 0.5f, 0.1 + 6 * pi, 0.2, 0) + 0.1) < 1e-6);
	CHECK(dh_heading(1, 0.5f, 0.5f, 0, 0, 0.2) < 0);
	CHECK(dh_heading(5, 0, 0.5f, 1, 0, 0) == 0.5f);
	CHECK(dh_heading(1, 1, 0.5f, NAN, 0, 0) == 0);
}

int main(void)
{
	T_dh = getenv("DEPTH_HOLD_BIN");
	T_nav = getenv("NAV_BIN");
	T_mux = getenv("MUX_BIN");
	if (!T_dh || !T_nav || !T_mux) {
		fprintf(stderr, "DEPTH_HOLD_BIN, NAV_BIN and MUX_BIN must be set\n");
		return 2;
	}
	RUN_TEST(test_step_response);
	RUN_TEST(test_level_and_heading);
	RUN_TEST(test_follow_and_handover);
	hd_setup("depth_hold", NULL);
	copy_types();
	RUN_TEST(test_chain);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
