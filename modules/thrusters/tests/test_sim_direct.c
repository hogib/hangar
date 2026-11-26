// SPDX-License-Identifier: GPL-3.0-or-later
// profiles/sim-direct flown under a manager: the profile's own manifests,
// parameters and frame file, with this build's binaries; safety, mux, nav,
// depth_hold, thrusters and sim_vehicle, no mavlink module and no ArduSub.
// This process is the pilot (it publishes pilot.wrench) and reads the
// simulator's truth, thr.cmd and ctl.wrench.
//
// What is judged here is that the processes do together what the fixed-step
// flight of test_thrusters does in one: arming without an autopilot, a depth
// reached and held, the vehicle going forward under surge, sideways under
// sway and round under yaw while it stays level, at depth and on its heading, every pulse-width set traced to the
// ctl.wrench it names and found to be that wrench's allocation, neutral on a
// disarm and when the safety module is killed. The bounds are wide: the
// simulator keeps pace with the clock here, on a machine that may be busy,
// and the motion's numbers are test_thrusters'. Durations are counted in
// simulated seconds (sim.pose's t). About 75 s.
#include "hd_test.h"

#include <math.h>

#include "mod_frame.h"
#include "mux_state.h"
#include "safety_state.h"
#include "sim_pose.h"
#include "thr_alloc.h"
#include "thruster_cmd.h"
#include "wrench.h"

#define PI 3.14159265358979323846
#define DEG (180.0 / PI)

static pid_t mgr;
static const char *T_bin;
static hg_pub *P_pilot;
static hg_sub *S_pose, *S_cmd, *S_ctl, *S_state, *S_mux;
static struct thr_alloc A;
static uint32_t ID_ctl, ID_pilot, ID_dh;

static struct {
	struct wrench pilot;
	uint64_t next_ns;
	struct sim_pose pose;
	int have_pose;
	double yaw_last, yaw_total;                 /* rad, unwrapped */
	double max_roll, max_pitch, min_depth, max_depth; /* since mark() */
	struct thruster_cmd cmd;
	struct hg_info cmd_info;
	int have_cmd;
	safety_state st;
	int have_state;
	struct mux_state mux;
	int have_mux;
	/* thr.cmd since mark(). */
	unsigned neutral, driven;
	unsigned neutral_with_inputs; /* a neutral message that names an input */
	unsigned driven_without_ctl;  /* a driven one that does not name ctl.wrench */
	unsigned traced;              /* its ctl.wrench read back, and its allocation the pulse widths sent */
	unsigned mismatched;          /* read back, and not */
	unsigned gone;                /* no longer in ctl.wrench's ring when looked for */
	unsigned from_pilot, from_controller; /* traced wrenches that name pilot.wrench, ctl.depth_hold */
} F;

static void mark(void)
{
	F.max_roll = F.max_pitch = 0;
	F.min_depth = 1e9;
	F.max_depth = -1e9;
	F.neutral = F.driven = F.neutral_with_inputs = F.driven_without_ctl = 0;
	F.traced = F.mismatched = F.gone = F.from_pilot = F.from_controller = 0;
}

static int is_neutral(const struct thruster_cmd *c)
{
	for (int i = 0; i < 8; i++)
		if (c->pwm[i] != 1500)
			return 0;
	return 1;
}

/* A driven thr.cmd: the ctl.wrench message it names, read back by its seq,
 * allocated here through the profile's frame, against the pulse widths. */
static void trace(const struct thruster_cmd *c, const struct hg_info *info)
{
	for (uint32_t i = 0; i < info->n_inputs && i < HG_MAX_REFS; i++) {
		if (info->inputs[i].topic_id != ID_ctl)
			continue;
		struct wrench w;
		struct hg_info wi;
		if (hg_read_seq(S_ctl, info->inputs[i].seq, &w, sizeof w, &wi) != HG_MSG) {
			F.gone++;
			return;
		}
		const float in[6] = { w.surge, w.sway, w.heave, w.roll, w.pitch, w.yaw };
		struct thr_alloc_out o;
		thr_alloc_mix(&A, in, &o);
		if (memcmp(o.pwm, c->pwm, sizeof o.pwm) == 0)
			F.traced++;
		else
			F.mismatched++;
		for (uint32_t k = 0; k < wi.n_inputs && k < HG_MAX_REFS; k++) {
			F.from_pilot += wi.inputs[k].topic_id == ID_pilot;
			F.from_controller += wi.inputs[k].topic_id == ID_dh;
		}
		return;
	}
	F.driven_without_ctl++;
}

static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	do {
		uint64_t now = hg_now_ns();
		if (now >= F.next_ns) {
			F.next_ns = now + 20000000u;
			hg_publish(P_pilot, &F.pilot, sizeof F.pilot, now);
		}
		struct hg_info info;
		struct sim_pose p;
		while (hg_next(S_pose, &p, sizeof p, &info) == HG_MSG) {
			if (F.have_pose) {
				double d = (double)p.yaw - F.yaw_last;
				if (d > PI)
					d -= 2 * PI;
				if (d < -PI)
					d += 2 * PI;
				F.yaw_total += d;
			}
			F.yaw_last = p.yaw;
			F.pose = p;
			F.have_pose = 1;
			F.max_roll = fmax(F.max_roll, fabs((double)p.roll));
			F.max_pitch = fmax(F.max_pitch, fabs((double)p.pitch));
			F.min_depth = fmin(F.min_depth, p.pos[2]);
			F.max_depth = fmax(F.max_depth, p.pos[2]);
		}
		struct thruster_cmd c;
		while (hg_next(S_cmd, &c, sizeof c, &info) == HG_MSG) {
			F.cmd = c;
			F.cmd_info = info;
			F.have_cmd = 1;
			if (is_neutral(&c) && info.n_inputs == 0) {
				F.neutral++;
			} else if (is_neutral(&c) && !(info.flags & HG_NO_REFS)) {
				/* A wrench of zero, allocated: neutral pulse widths that do
				 * name their command. */
				F.driven++;
				trace(&c, &info);
			} else if (is_neutral(&c)) {
				F.neutral++;
				F.neutral_with_inputs++;
			} else {
				F.driven++;
				trace(&c, &info);
			}
		}
		safety_state s;
		while (hg_next(S_state, &s, sizeof s, &info) == HG_MSG) {
			F.st = s;
			F.have_state = 1;
		}
		struct mux_state m;
		while (hg_next(S_mux, &m, sizeof m, &info) == HG_MSG) {
			F.mux = m;
			F.have_mux = 1;
		}
		sleep_ms(2);
	} while (hg_now_ns() < end);
}

/* Pumps for `seconds` of simulated time; 0 when the simulator did not get
 * there in eight times as long on the clock. */
static int fly(double seconds)
{
	double t0 = F.pose.t;
	uint64_t w0 = now_ms();
	while (F.pose.t - t0 < seconds) {
		pump(20);
		if (now_ms() - w0 > (uint64_t)(seconds * 8000) + 2000) {
			printf("    the simulator advanced %.2f s of %.2f in %llu ms\n", F.pose.t - t0, seconds,
			       (unsigned long long)(now_ms() - w0));
			return 0;
		}
	}
	return 1;
}

static int wait_safety(int st, long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		pump(10);
		if (F.have_state && F.st.state == st)
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	printf("    safety.state is %d (reason '%s'), wanted %d\n", F.st.state, F.st.reason, st);
	return 0;
}

/* Pumps until every thr.cmd over 300 ms was neutral and named nothing. */
static int wait_all_neutral(long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		mark();
		pump(300);
		if (F.neutral >= 5 && F.driven == 0)
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

static char cli_out[4096];

static int cli_call(const char *target, const char *arg)
{
	int r = run_cli(cli_out, sizeof cli_out, 5000, "call", target, arg, (char *)NULL);
	if (r != 0 || !strstr(cli_out, "HG_OK"))
		printf("    hangar call %s %s: %d, %s\n", target, arg ? arg : "", r, cli_out);
	return r == 0 && strstr(cli_out, "HG_OK") ? 0 : -1;
}

/* A file of the profile into the root, /usr/lib/hangar/ and /etc/hangar/
 * rewritten to this build and this root, as tools/sim's scripts do. */
static void install(const char *name, const char *to_dir, const char *to_name)
{
	char from[256], to[1024];
	tfmt(from, sizeof from, "profiles/sim-direct/%s", name);
	const char *t = read_file(from);
	if (!t) {
		printf("    cannot read %s (the suite runs from the repository root)\n", from);
		exit(2);
	}
	static char text[16384], tmp[16384];
	tfmt(text, sizeof text, "%s", t);
	char etc[600], lib[600];
	tfmt(etc, sizeof etc, "%s/etc/hangar/", T_root);
	tfmt(lib, sizeof lib, "%s/", T_bin);
	const char *subs[2][2] = { { "/usr/lib/hangar/", lib }, { "/etc/hangar/", etc } };
	for (int k = 0; k < 2; k++) {
		char *at = text;
		while ((at = strstr(at, subs[k][0])) != NULL) {
			size_t off = (size_t)(at - text);
			*at = 0;
			tfmt(tmp, sizeof tmp, "%s%s%s", text, subs[k][1], at + strlen(subs[k][0]));
			tfmt(text, sizeof text, "%s", tmp);
			at = text + off + strlen(subs[k][1]);
		}
	}
	if (mkdir_p(to_dir) < 0) {
		printf("    cannot create %s\n", to_dir);
		exit(2);
	}
	tfmt(to, sizeof to, "%s/%s", to_dir, to_name);
	if (write_file(to, text) < 0) {
		printf("    cannot write %s\n", to);
		exit(2);
	}
}

static void test_start(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *more[] = { "safety_params", "check_rep",	 "thruster_cmd",  "thruster_out", "attitude",
			       "battery",	"nav_state",	 "nav_params",	  "depth_target", "depth_control_params",
			       "depth_set_req", "mux_state",	 "mux_params",	  "set_mode_req", "set_mode_rep",
			       "sim_pose",	"sim_velocity", "sim_thrust_truth", "sim_vehicle_params", NULL };
	for (int i = 0; more[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src, more[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, more[i]);
		CHECK_EQ_INT(copy_file(from, to), 0);
	}
	char etc[600], dir[700];
	tfmt(etc, sizeof etc, "%s/etc/hangar", T_root);
	static const char *const mods[] = { "safety", "mux", "nav", "depth_hold", "thrusters", "sim_vehicle", NULL };
	for (int i = 0; mods[i]; i++) {
		char name[64];
		tfmt(name, sizeof name, "%s.module", mods[i]);
		install(name, T_modules, name);
	}
	install("bluerov2-heavy.frame", etc, "bluerov2-heavy.frame");
	static const char *const params[] = { "safety", "mux", "depth_hold", "sim_vehicle", NULL };
	for (int i = 0; params[i]; i++) {
		char name[64];
		tfmt(name, sizeof name, "%s.params", params[i]);
		tfmt(dir, sizeof dir, "%s/var/lib/hangar/%s", T_root, params[i]);
		install(name, dir, "params");
	}
	/* This process: the pilot, and a reader. */
	manifest("pilot", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			  "[Subscribe sim.pose]\n[Subscribe thr.cmd]\n[Subscribe ctl.wrench]\n"
			  "[Subscribe safety.state]\n[Subscribe mux.state]\n"
			  "[Publish pilot.wrench]\nType=wrench\nRate=50\n");
	mgr = manager_start();
	static const char *const up[] = { "sim_vehicle", "nav", "depth_hold", "mux", "thrusters", "safety", NULL };
	for (int i = 0; up[i]; i++) {
		int ok = wait_state(up[i], "ready", 10000);
		if (!ok) {
			char log[1024];
			tfmt(log, sizeof log, "%s/run/hangar/log/%s.log", T_root, up[i]);
			const char *t = read_file(log);
			printf("    %s is not ready; its log:\n%s\n", up[i], t ? t : "(none)");
		}
		CHECK(ok);
	}
	CHECK(wait_state("pilot", "stopped", 5000));
	char path[600];
	CHECK_EQ_INT(wiring_to_file("pilot", path, sizeof path), 0);
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", "pilot", 1);
	CHECK_EQ_INT(hg_init(), 0);
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
	S_pose = hg_sub_open("sim.pose", &SIM_POSE_TYPE);
	S_cmd = hg_sub_open("thr.cmd", &THRUSTER_CMD_TYPE);
	S_ctl = hg_sub_open("ctl.wrench", &WRENCH_TYPE);
	S_state = hg_sub_open("safety.state", &SAFETY_STATE_TYPE);
	S_mux = hg_sub_open("mux.state", &MUX_STATE_TYPE);
	P_pilot = hg_pub_open("pilot.wrench", &WRENCH_TYPE, NULL);
	CHECK(S_pose && S_cmd && S_ctl && S_state && S_mux && P_pilot);
	ID_ctl = hg_topic_id("ctl.wrench");
	ID_pilot = hg_topic_id("pilot.wrench");
	ID_dh = hg_topic_id("ctl.depth_hold");
	/* The allocation the module was given, built here from the same file. */
	struct sim_frame f;
	char err[160] = "";
	CHECK_EQ_INT(mod_frame_load("profiles/sim-direct/bluerov2-heavy.frame", &f, err, sizeof err), 0);
	CHECK_EQ_INT(thr_alloc_init(&A, &f, 16.0), 0);
	/* Both modules say which frame they hold. */
	char log[1024];
	tfmt(log, sizeof log, "%s/run/hangar/log/thrusters.log", T_root);
	CHECK(wait_file_contains(log, "frame bluerov2-heavy: 8 thrusters, rank 6", 3000));
	CHECK(file_contains(log, "allocated through the frame"));
	tfmt(log, sizeof log, "%s/run/hangar/log/sim_vehicle.log", T_root);
	CHECK(wait_file_contains(log, "frame bluerov2-heavy, 8 thrusters", 3000));
	CHECK(!file_contains(log, "compiled in"));
	mark();
	pump(500);
	CHECK(F.have_pose);
}

static void test_disarmed(void)
{
	CHECK(wait_safety(0, 5000));
	/* A full command while disarmed moves nothing, and names nothing. */
	F.pilot.surge = 1;
	CHECK(wait_all_neutral(5000));
	CHECK_EQ_INT(F.neutral_with_inputs, 0);
	F.pilot.surge = 0;
	pump(200);
}

static void test_arm_and_dive(void)
{
	/* No autopilot: arming leads to armed at once (docs/07 "Phase 2"). */
	CHECK_EQ_INT(cli_call("safety.arm", NULL), 0);
	CHECK(wait_safety(1, 3000));
	CHECK(F.st.outputs_enabled);
	CHECK_EQ_INT(cli_call("mux.set_mode", "mode=assisted"), 0);
	CHECK_EQ_INT(cli_call("depth_hold.set", "depth_m=2"), 0);
	mark();
	double t0 = F.pose.t, reached = -1;
	while (F.pose.t - t0 < 40 && reached < 0) {
		if (!fly(0.5))
			break;
		if (fabs(F.pose.pos[2] - 2.0) < 0.1)
			reached = F.pose.t - t0;
	}
	printf("    within 0.1 m of 2 m after %.1f s\n", reached);
	CHECK(reached > 0);
	CHECK(fly(8));
	mark();
	CHECK(fly(4));
	printf("    held between %.3f and %.3f m for 4 s; roll within %.2f deg, pitch within %.2f deg\n", F.min_depth,
	       F.max_depth, F.max_roll * DEG, F.max_pitch * DEG);
	CHECK(F.min_depth > 1.8 && F.max_depth < 2.2);
	CHECK(F.max_roll * DEG < 5 && F.max_pitch * DEG < 5);
	/* The controller has heave, roll, pitch and yaw (mux.params' controller_axes). */
	CHECK(F.have_mux && F.mux.source_heave == 2 && F.mux.source_roll == 2 && F.mux.source_pitch == 2 &&
	      F.mux.source_yaw == 2);
}

static void test_surge(void)
{
	double x0 = F.pose.pos[0], y0 = F.pose.pos[1], yaw0 = F.yaw_total;
	mark();
	F.pilot.surge = 0.5f;
	CHECK(fly(8));
	double fwd = (F.pose.pos[0] - x0) * cos(F.yaw_last) + (F.pose.pos[1] - y0) * sin(F.yaw_last);
	printf("    surge 0.5 for 8 s: %.2f m forward, |pitch| up to %.2f deg, depth %.2f to %.2f m, heading %+.1f deg\n",
	       fwd, F.max_pitch * DEG, F.min_depth, F.max_depth, (F.yaw_total - yaw0) * DEG);
	CHECK(fwd > 2.0);
	CHECK(fabs(F.yaw_total - yaw0) * DEG < 15); /* the heading held (depth_hold.params) */
	CHECK(F.max_pitch * DEG < 10);
	CHECK(F.min_depth > 1.6 && F.max_depth < 2.4);
	CHECK(F.have_mux && F.mux.source_surge == 1 && F.mux.source_heave == 2);
	/* Every pulse-width set of the leg was a ctl.wrench's allocation, and
	 * those wrenches name the pilot and the controller. */
	printf("    thr.cmd: %u driven, %u traced to their ctl.wrench, %u not its allocation, %u no longer readable\n",
	       F.driven, F.traced, F.mismatched, F.gone);
	CHECK(F.driven > 100);
	CHECK_EQ_INT(F.mismatched, 0);
	CHECK_EQ_INT(F.driven_without_ctl, 0);
	CHECK(F.traced > F.driven / 2);
	CHECK(F.from_pilot > F.traced / 2);
	CHECK(F.from_controller > F.traced / 2);
	F.pilot.surge = 0;
	CHECK(fly(6));
}

static void test_sway(void)
{
	double x0 = F.pose.pos[0], y0 = F.pose.pos[1], yaw0 = F.yaw_total;
	mark();
	F.pilot.sway = 0.5f;
	CHECK(fly(6));
	double right = -(F.pose.pos[0] - x0) * sin(F.yaw_last) + (F.pose.pos[1] - y0) * cos(F.yaw_last);
	printf("    sway 0.5 for 6 s: %.2f m to the right, |roll| up to %.2f deg, depth %.2f to %.2f m, heading %+.1f deg\n",
	       right, F.max_roll * DEG, F.min_depth, F.max_depth, (F.yaw_total - yaw0) * DEG);
	CHECK(right > 1.5);
	CHECK(fabs(F.yaw_total - yaw0) * DEG < 15);
	CHECK(F.max_roll * DEG < 10);
	CHECK(F.min_depth > 1.6 && F.max_depth < 2.4);
	CHECK_EQ_INT(F.mismatched, 0);
	F.pilot.sway = 0;
	CHECK(fly(6));
}

static void test_yaw(void)
{
	double yaw0 = F.yaw_total, t0 = F.pose.t;
	mark();
	F.pilot.yaw = 0.1f;
	while (F.yaw_total - yaw0 < 2 * PI && F.pose.t - t0 < 40)
		if (!fly(0.1))
			break;
	double took = F.pose.t - t0;
	F.pilot.yaw = 0;
	CHECK(fly(6));
	printf("    yaw 0.1: a full turn clockwise in %.1f s, %.0f deg in all; depth %.2f to %.2f m, roll within %.2f, pitch within %.2f deg\n",
	       took, (F.yaw_total - yaw0) * DEG, F.min_depth, F.max_depth, F.max_roll * DEG, F.max_pitch * DEG);
	CHECK((F.yaw_total - yaw0) * DEG >= 360 && (F.yaw_total - yaw0) * DEG < 540);
	CHECK(took < 30);
	CHECK(F.min_depth > 1.6 && F.max_depth < 2.4);
	CHECK(F.max_roll * DEG < 10 && F.max_pitch * DEG < 10);
	CHECK_EQ_INT(F.mismatched, 0);
	/* Let go, the heading it then had is held. */
	double held = F.yaw_total;
	CHECK(fly(4));
	printf("    4 s later the heading is %+.1f deg from where the turn ended\n", (F.yaw_total - held) * DEG);
	CHECK(fabs(F.yaw_total - held) * DEG < 10);
	CHECK(F.have_mux && F.mux.source_yaw == 2);
}

static void test_disarm(void)
{
	F.pilot.surge = 0.5f;
	pump(500);
	CHECK(!is_neutral(&F.cmd));
	CHECK_EQ_INT(cli_call("safety.disarm", NULL), 0);
	CHECK(wait_safety(0, 3000));
	/* The pilot still pushing: nothing moves, and the outputs name nothing. */
	CHECK(wait_all_neutral(5000));
	CHECK_EQ_INT(F.neutral_with_inputs, 0);
	F.pilot.surge = 0;
	/* Let go, the vehicle floats up: 2 N lighter than water. */
	double d0 = F.pose.pos[2];
	CHECK(fly(6));
	printf("    disarmed at %.2f m: %.2f m six seconds later\n", d0, F.pose.pos[2]);
	CHECK(F.pose.pos[2] < d0 - 0.05);
}

static void test_safety_killed(void)
{
	CHECK_EQ_INT(cli_call("safety.arm", NULL), 0);
	CHECK(wait_safety(1, 3000));
	F.pilot.surge = 0.5f;
	pump(800);
	CHECK(!is_neutral(&F.cmd));
	struct mstat m;
	CHECK(module_status("safety", &m) && m.pid > 0);
	uint64_t t0 = now_ms();
	kill(m.pid, SIGKILL);
	/* safety.state stale: the gate reads it as disarmed (SafetyTimeout). */
	CHECK(wait_all_neutral(8000));
	printf("    all neutral within %llu ms of the safety module being killed\n",
	       (unsigned long long)(now_ms() - t0));
	CHECK_EQ_INT(F.neutral_with_inputs, 0);
	/* Restarted, it is disarmed: still neutral, the pilot still pushing. */
	CHECK(wait_new_pid("safety", m.pid, "ready", 10000));
	F.have_state = 0;
	CHECK(wait_safety(0, 5000));
	CHECK(wait_all_neutral(5000));
	F.pilot.surge = 0;
}

static void test_shutdown(void)
{
	hg_pub_close(P_pilot);
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(void)
{
	T_bin = getenv("HANGAR_BUILD");
	if (!T_bin) {
		fprintf(stderr, "HANGAR_BUILD (the directory of the modules' binaries) must be set\n");
		return 2;
	}
	hd_setup("direct", NULL);
	RUN_TEST(test_start);
	RUN_TEST(test_disarmed);
	RUN_TEST(test_arm_and_dive);
	RUN_TEST(test_surge);
	RUN_TEST(test_sway);
	RUN_TEST(test_yaw);
	RUN_TEST(test_disarm);
	RUN_TEST(test_safety_killed);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
