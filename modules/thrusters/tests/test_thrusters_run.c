// SPDX-License-Identifier: GPL-3.0-or-later
// hangar-thrusters under a manager, with the real safety module and no
// autopilot (docs/07 "The actuator gate", "Phase 2"): this process is the
// mux (it publishes ctl.wrench) and reads thr.cmd. test_thrusters checks the
// mixing; this checks what the binary does with the gate: neutral while
// disarmed whatever the command says, `arming` to `armed` at once without an
// autopilot, the command passed while armed and fresh, neutral when the
// command or safety.state goes stale and on a disarm, a neutral output that
// names no input, and neutral written before the module exits on SIGTERM.
// About 10 s.
#include "hd_test.h"

#include "safety_state.h"
#include "thruster_cmd.h"
#include "wrench.h"

static pid_t mgr;
static const char *T_thr, *T_safety;
static hg_pub *P_wrench;
static hg_sub *S_cmd, *S_state;
static struct thruster_cmd last_cmd;
static struct hg_info last_info;
static int have_cmd;
static safety_state last_state;
static int have_state;

/* What pump() publishes, and what it has read from thr.cmd since reset(). */
static struct {
	int publish;
	float heave;
	uint64_t next_ns;
	unsigned neutral, driven;
	unsigned neutral_with_inputs; /* a neutral message that names an input (docs/07: it names none) */
	unsigned driven_without_cmd;  /* a driven message that does not name ctl.wrench */
} T;

static void reset(void)
{
	T.neutral = T.driven = T.neutral_with_inputs = T.driven_without_cmd = 0;
}

static int is_neutral(const struct thruster_cmd *c)
{
	for (int i = 0; i < 8; i++)
		if (c->pwm[i] != 1500)
			return 0;
	return 1;
}

static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	do {
		uint64_t now = hg_now_ns();
		if (T.publish && now >= T.next_ns) {
			T.next_ns = now + 20000000u;
			struct wrench w = { .heave = T.heave };
			hg_publish(P_wrench, &w, sizeof w, now);
		}
		struct thruster_cmd c;
		struct hg_info info;
		while (hg_next(S_cmd, &c, sizeof c, &info) == HG_MSG) {
			last_cmd = c;
			last_info = info;
			have_cmd = 1;
			int from_cmd = 0;
			for (uint32_t i = 0; i < info.n_inputs && i < HG_MAX_REFS; i++)
				if (info.inputs[i].topic_id == hg_topic_id("ctl.wrench"))
					from_cmd = 1;
			if (is_neutral(&c)) {
				T.neutral++;
				T.neutral_with_inputs += info.n_inputs != 0;
			} else {
				T.driven++;
				T.driven_without_cmd += !from_cmd;
			}
		}
		safety_state s;
		while (hg_next(S_state, &s, sizeof s, &info) == HG_MSG) {
			last_state = s;
			have_state = 1;
		}
		sleep_ms(2);
	} while (hg_now_ns() < end);
}

/* Pumps until the newest thr.cmd is neutral (or driven): 1, or 0 at the bound. */
static int wait_cmd(int neutral, long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		pump(5);
		if (have_cmd && is_neutral(&last_cmd) == neutral)
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

static int wait_safety(int st, long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		pump(5);
		if (have_state && last_state.state == st)
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	printf("    safety.state is %d (reason '%s'), wanted %d\n", last_state.state, last_state.reason, st);
	return 0;
}

/* Pumps until the thrusters' status line holds this text. */
static int wait_status(const char *text, long timeout_ms)
{
	static struct mstat m;
	uint64_t t0 = now_ms();
	do {
		pump(20);
		if (module_status("thrusters", &m) && strstr(m.status, text))
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	printf("    thrusters' status is '%s', wanted '%s'\n", m.status, text);
	return 0;
}

static char cli_out[4096];

static int call(const char *method)
{
	char target[64];
	tfmt(target, sizeof target, "safety.%s", method);
	return run_cli(cli_out, sizeof cli_out, 5000, "call", target, (char *)NULL);
}

static void test_start(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *more[] = { "safety_params", "check_rep", "thruster_cmd", "thruster_out", NULL };
	for (int i = 0; more[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src, more[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, more[i]);
		CHECK_EQ_INT(copy_file(from, to), 0);
	}
	/* The safety module as a profile without ArduSub, sensors or a bridge
	 * has it (profiles/sim-depth-ff): no [Call mavlink.arm]. */
	char dir[700], params[800];
	tfmt(dir, sizeof dir, "%s/var/lib/hangar/safety", T_root);
	CHECK_EQ_INT(mkdir_p(dir), 0);
	tfmt(params, sizeof params, "%s/params", dir);
	write_file(params, "# hangar parameters: safety_params\n# version 1, set by test, 2026-11-21T00:00:00Z\n"
			   "leak_required = 0\nbattery_required = 0\ndepth_required = 0\ntether_enabled = 0\n"
			   "required = \"thrusters\"\n");
	manifest("safety", "[Module]\nDescription=Vehicle state\nExec=%s\n"
			   "[Subscribe hangar.vehicle.modules]\n[Subscribe hangar.vehicle.system]\n[Subscribe vehicle.leak]\n"
			   "[Subscribe power.battery]\n[Subscribe nav.depth]\n[Subscribe ardusub.state]\n[Subscribe mux.params]\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Params]\nType=safety_params\nAllow=admin\n"
			   "[Serve arm]\nRequest=empty\nReply=empty\nAllow=admin\n"
			   "[Serve disarm]\nRequest=empty\nReply=empty\nAllow=* admin\n"
			   "[Serve estop]\nRequest=empty\nReply=empty\nAllow=* admin\n"
			   "[Serve clear]\nRequest=empty\nReply=empty\nAllow=admin\n"
			   "[Serve check]\nRequest=empty\nReply=check_rep\nAllow=* admin\n"
			   "[Restart]\nPolicy=always\nBackoff=0.1s..0.2s\n",
		 T_safety);
	manifest("thrusters", "[Module]\nDescription=Thrusters\nExec=%s\nUser=hg-thrusters\n"
			      "[Subscribe ctl.wrench]\n[Subscribe safety.state]\n"
			      "[Publish thr.cmd]\nType=thruster_cmd\nRate=50\nActuator=yes\nInputs=ctl.wrench\n"
			      "[Publish thr.out]\nType=thruster_out\nRate=50\nInputs=ctl.wrench\n"
			      "[Actuator]\nCommand=ctl.wrench\nNeutral=%s --neutral\n",
		 T_thr, T_thr);
	/* This process: the mux's output, and a reader of the thrusters'. */
	manifest("feeder", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe thr.cmd]\n[Subscribe safety.state]\n"
			   "[Publish ctl.wrench]\nType=wrench\nRate=50\n");
	mgr = manager_start();
	CHECK(wait_state("safety", "ready", 5000));
	CHECK(wait_state("thrusters", "ready", 5000));
	CHECK(wait_state("feeder", "stopped", 5000));
	char path[600];
	CHECK_EQ_INT(wiring_to_file("feeder", path, sizeof path), 0);
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", "feeder", 1);
	CHECK_EQ_INT(hg_init(), 0);
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
	S_cmd = hg_sub_open("thr.cmd", &THRUSTER_CMD_TYPE);
	S_state = hg_sub_open("safety.state", &SAFETY_STATE_TYPE);
	P_wrench = hg_pub_open("ctl.wrench", &WRENCH_TYPE, NULL);
	CHECK(S_cmd && S_state && P_wrench);
}

static void test_disarmed(void)
{
	/* No command at all, disarmed: 1500 us on all eight, naming no input. */
	CHECK(wait_safety(0, 3000));
	CHECK(wait_cmd(1, 3000));
	reset();
	pump(500);
	CHECK(T.neutral >= 5);
	CHECK_EQ_INT(T.driven, 0);
	CHECK_EQ_INT(T.neutral_with_inputs, 0);
	CHECK(wait_status("gate closed (disarmed", 3000));
	/* A fresh, large command while disarmed moves nothing. */
	T.publish = 1;
	T.heave = 0.5f;
	reset();
	pump(600);
	CHECK(T.neutral >= 5);
	CHECK_EQ_INT(T.driven, 0);
	CHECK_EQ_INT(T.neutral_with_inputs, 0);
}

static void test_armed(void)
{
	/* Without an autopilot behind the actuators, arming leads to armed at
	 * once (docs/07 "Vehicle states"): nothing reports armed here. */
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(strstr(cli_out, "HG_OK") != NULL);
	if (!strstr(cli_out, "HG_OK"))
		printf("    arm: %s\n", cli_out);
	CHECK(wait_safety(1, 2000));
	CHECK(last_state.outputs_enabled);
	/* The gate opens: the command reaches thr.cmd, mixed, and names it. */
	CHECK(wait_cmd(0, 2000));
	reset();
	pump(400);
	CHECK(T.driven >= 5);
	CHECK_EQ_INT(T.neutral, 0);
	CHECK_EQ_INT(T.driven_without_cmd, 0);
	int moved = 0;
	for (int i = 0; i < 8; i++) {
		CHECK(last_cmd.pwm[i] >= 1100 && last_cmd.pwm[i] <= 1900);
		moved += last_cmd.pwm[i] != 1500;
	}
	CHECK_EQ_INT(moved, 4); /* heave alone: the four vertical thrusters */
	CHECK(wait_status("gate open", 3000));
}

static void test_command_stale(void)
{
	/* The mux dead (or hung): no ctl.wrench. Neutral after CommandTimeout
	 * (200 ms) and one output period, not before, and naming no input. */
	pump(100);
	T.publish = 0;
	uint64_t t0 = now_ms();
	CHECK(wait_cmd(1, 3000));
	uint64_t dt = now_ms() - t0;
	printf("    neutral %llu ms after the last command\n", (unsigned long long)dt);
	CHECK(dt >= 150 && dt < 2000);
	reset();
	pump(400);
	CHECK(T.neutral >= 5);
	CHECK_EQ_INT(T.driven, 0);
	CHECK_EQ_INT(T.neutral_with_inputs, 0);
	CHECK(wait_status("command stale", 3000));
	/* And it drives again with a fresh command: nothing latched. */
	T.publish = 1;
	CHECK(wait_cmd(0, 2000));
}

static void test_disarm(void)
{
	CHECK_EQ_INT(call("disarm"), 0);
	CHECK(wait_safety(0, 2000));
	CHECK(wait_cmd(1, 2000));
	reset();
	pump(400);
	CHECK(T.neutral >= 5);
	CHECK_EQ_INT(T.driven, 0);
	CHECK_EQ_INT(T.neutral_with_inputs, 0);
	CHECK(wait_status("gate closed (disarmed", 3000));
}

static void test_safety_hung(void)
{
	/* The safety module stopped in its tracks while armed: safety.state
	 * goes stale, which the gate reads as disarmed after SafetyTimeout
	 * (300 ms), with the command still fresh. */
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(wait_safety(1, 2000));
	CHECK(wait_cmd(0, 2000));
	pump(200);
	struct mstat m;
	CHECK(module_status("safety", &m) && m.pid > 0);
	kill(m.pid, SIGSTOP);
	uint64_t t0 = now_ms();
	CHECK(wait_cmd(1, 5000));
	uint64_t dt = now_ms() - t0;
	printf("    neutral %llu ms after the safety module hung\n", (unsigned long long)dt);
	CHECK(dt >= 150 && dt < 3000);
	reset();
	pump(300);
	CHECK_EQ_INT(T.driven, 0);
	CHECK_EQ_INT(T.neutral_with_inputs, 0);
	kill(m.pid, SIGKILL);
	/* The manager's restart starts disarmed: still neutral. */
	CHECK(wait_new_pid("safety", m.pid, "ready", 10000));
	have_state = 0;
	CHECK(wait_safety(0, 3000));
	reset();
	pump(300);
	CHECK_EQ_INT(T.driven, 0);
}

static void test_sigterm(void)
{
	/* Stopped while it drives: the module's last message is neutral
	 * (docs/07: "On SIGTERM an actuator module writes neutral before it
	 * exits"), and the manager runs its Neutral command. */
	CHECK_EQ_INT(call("arm"), 0);
	CHECK(wait_safety(1, 2000));
	CHECK(wait_cmd(0, 2000));
	pump(200);
	CHECK(!is_neutral(&last_cmd));
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "stop", "thrusters", (char *)NULL), 0);
	CHECK(wait_state("thrusters", "stopped", 5000));
	pump(200);
	CHECK(is_neutral(&last_cmd));
	char log[1024];
	tfmt(log, sizeof log, "%s/run/hangar/log/thrusters.log", T_root);
	CHECK(file_contains(log, "stopping: neutral sent"));
	CHECK(strcmp(system_state(), "nominal") == 0); /* the Neutral command ran and returned 0 */
	CHECK_EQ_INT(call("disarm"), 0);
}

static void test_shutdown(void)
{
	hg_pub_close(P_wrench);
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(void)
{
	T_thr = getenv("THRUSTERS_BIN");
	T_safety = getenv("SAFETY_BIN");
	if (!T_thr || !T_safety) {
		fprintf(stderr, "THRUSTERS_BIN and SAFETY_BIN must be set\n");
		return 2;
	}
	hd_setup("thrusters", NULL);
	RUN_TEST(test_start);
	RUN_TEST(test_disarmed);
	RUN_TEST(test_armed);
	RUN_TEST(test_command_stale);
	RUN_TEST(test_disarm);
	RUN_TEST(test_safety_hung);
	RUN_TEST(test_sigterm);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
