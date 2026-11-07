// SPDX-License-Identifier: GPL-3.0-or-later
// Health and states (docs/03 "States"): staleness makes a module unhealthy
// and restarts it with back-off doubling and ResetAfter; Burst gives up,
// OnGiveUp=degrade shows on hangar.<role>.system; exit status 78 is failed
// at once; a module never ready is unhealthy after StartGrace; the watchdog;
// an actuator's Neutral command runs when it is killed; failsafe on a
// give-up and clear-failsafe; the shutdown order.
#include "hd_test.h"

#include "hg_type.h"

static pid_t mgr;

static void test_stale_restart_and_backoff(void)
{
	// Publishes 20 messages at 50 Hz (0.4 s), then stays silent: stale after
	// 60 ms + StaleGrace 200 ms -> unhealthy -> stopped -> back-off 0.1 s,
	// doubling: 0.1, 0.2, 0.4 s; the delays are in the registry.
	manifest("stale", "[Module]\nExec=%s/talker --topic stale.out --rate 50 --stop-after 20\n"
			  "[Publish stale.out]\nType=depth\nRate=50\n"
			  "[Health]\nStartGrace=2s\nStaleGrace=200ms\n"
			  "[Restart]\nBackoff=0.1s..1s\nResetAfter=20s\nBurst=10/60s\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("stale", "ready", 5000));
	struct mstat m;
	module_status("stale", &m);
	int pid1 = m.pid;
	CHECK(wait_file_contains(T_log, "stale: unhealthy: stale.out stale for", 3000));
	CHECK(wait_file_contains(T_log, "restarting in 0.1 s (failure 1)", 3000));
	CHECK(wait_file_contains(T_log, "restarting in 0.2 s (failure 2)", 6000));
	CHECK(wait_file_contains(T_log, "restarting in 0.4 s (failure 3)", 6000));
	CHECK(wait_state("stale", "ready", 5000));
	module_status("stale", &m);
	CHECK(m.pid != pid1 && m.failures >= 3);
	// The registry carries the current delay.
	char path[1024];
	tfmt(path, sizeof path, "%s/run/hangar/registry", T_root);
	const char *reg = read_file(path);
	const char *l = strstr(reg, "\nstale ");
	CHECK(l != NULL);
	if (l) {
		char name[32], state[32];
		int pid;
		unsigned long long start;
		unsigned fails;
		double delay = 0;
		sscanf(l + 1, "%31s %d %llu %31s %u %lf", name, &pid, &start, state, &fails, &delay);
		CHECK(delay >= 0.4 - 1e-9);
	}
	manifest_remove("stale");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(manager_stop(mgr) == 0);
	unlink(T_log);
}

static void test_reset_after(void)
{
	// One failure, then healthy past ResetAfter: the count and the delay
	// start over.
	manifest("steady", "[Module]\nExec=%s/talker --topic steady.out --rate 50\n"
			   "[Publish steady.out]\nType=depth\nRate=50\n"
			   "[Restart]\nBackoff=0.1s..1s\nResetAfter=1s\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("steady", "ready", 5000));
	struct mstat m;
	module_status("steady", &m);
	kill(m.pid, SIGKILL);
	CHECK(wait_file_contains(T_log, "steady: backoff: pid", 3000));
	CHECK(wait_state("steady", "ready", 5000));
	module_status("steady", &m);
	CHECK_EQ_INT(m.failures, 1);
	CHECK(wait_file_contains(T_log, "steady: healthy for 1 s: failure count reset", 4000));
	module_status("steady", &m);
	CHECK_EQ_INT(m.failures, 0);
	CHECK(manager_stop(mgr) == 0);
	manifest_remove("steady");
	unlink(T_log);
}

/* The system state from hangar.vehicle.system, decoded by its schema. */
static int system_topic_state(void)
{
	hg_sub *s = hg_sub_open("hangar.vehicle.system", NULL);
	if (!s)
		return -1;
	unsigned char buf[256];
	struct hg_info info;
	int state = -1;
	for (int i = 0; i < 50 && state < 0; i++) {
		if (hg_latest(s, buf, sizeof buf, &info) == HG_MSG) {
			uint32_t len;
			const char *text = hg_sub_schema(s, &len);
			struct hg_schema *sc = calloc(1, sizeof *sc);
			if (sc && hg_schema_parse(text, len, sc, NULL, 0) == 0) {
				const struct hg_field *f = hg_type_field(&sc->type, "state");
				if (f)
					state = buf[f->offset];
			}
			free(sc);
		} else {
			sleep_ms(20);
		}
	}
	hg_sub_close(s);
	return state;
}

static void test_burst_degrade_and_exit78(void)
{
	// Burst=2/60s: failures 1 and 2 restart, the third gives up: failed,
	// system degraded (OnGiveUp=degrade), on the topic and in status.
	manifest("crasher", "[Module]\nExec=%s/crasher --topic crash.out --crash-after 1\n"
			    "[Publish crash.out]\nType=depth\nRate=50\n"
			    "[Restart]\nBackoff=0.1s..0.2s\nBurst=2/60s\n", T_toys);
	manifest("badconf", "[Module]\nExec=%s/eventer --exit-78\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("crasher", "failed", 8000));
	CHECK(wait_file_contains(T_log, "crasher: failed: 3 failures within 60 s: given up", 2000));
	CHECK(file_contains(T_log, "restarting in 0.1 s (failure 1)"));
	CHECK(file_contains(T_log, "restarting in 0.2 s (failure 2)"));
	CHECK(strcmp(system_state(), "degraded") == 0);
	CHECK_EQ_INT(system_topic_state(), 1); // 1: degraded
	struct mstat m;
	module_status("crasher", &m);
	CHECK(strncmp(m.last_exit, "signal:6", 8) == 0); // abort
	// Exit status 78: failed at once, with the status line as the reason.
	CHECK(wait_state("badconf", "failed", 5000));
	module_status("badconf", &m);
	CHECK(strcmp(m.last_exit, "exit:78") == 0);
	CHECK(file_contains(T_log, "badconf: failed: exit status 78: refused its configuration: saved parameters refused"));
	CHECK(!file_contains(T_log, "badconf: backoff"));
	// A failed module stays down until hangar restart; then degraded ends.
	manifest("crasher", "[Module]\nExec=%s/crasher --topic crash.out --crash-after -1\n"
			    "[Publish crash.out]\nType=depth\nRate=50\n", T_toys);
	manifest_remove("badconf");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_state("crasher", "ready", 5000));
	CHECK(strcmp(system_state(), "nominal") == 0);
	CHECK_EQ_INT(system_topic_state(), 0);
	CHECK(manager_stop(mgr) == 0);
	manifest_remove("crasher");
	unlink(T_log);
}

static void test_never_ready_and_watchdog(void)
{
	manifest("sleepy", "[Module]\nExec=%s/neverready\n[Health]\nStartGrace=500ms\n[Restart]\nBackoff=0.1s..0.1s\n", T_toys);
	manifest("beat", "[Module]\nExec=%s/eventer --alive-every 50 --stop-alive-after 10\n"
			 "[Health]\nWatchdog=300ms\n[Restart]\nBackoff=0.1s..0.1s\n", T_toys);
	mgr = manager_start();
	CHECK(wait_file_contains(T_log, "sleepy: unhealthy: not ready 0.5 s after start", 3000));
	CHECK(wait_file_contains(T_log, "sleepy: backoff:", 3000));
	// An event-only module: ready by hg_ready(), then its watchdog lapses.
	CHECK(wait_file_contains(T_log, "beat: ready", 3000));
	CHECK(wait_file_contains(T_log, "beat: unhealthy: watchdog: no hg_alive() for", 3000));
	CHECK(wait_file_contains(T_log, "beat: backoff:", 3000));
	// A module stopped while unhealthy is SIGTERMed and exits 0 (the toys
	// handle SIGTERM); the exit is logged as such.
	CHECK(file_contains(T_log, "sleepy: pid") && file_contains(T_log, "exited with status 0"));
	CHECK(manager_stop(mgr) == 0);
	manifest_remove("sleepy");
	manifest_remove("beat");
	unlink(T_log);
}

static void test_actuator_neutral_and_failsafe(void)
{
	char ran[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran", T_root);
	unlink(ran);
	// An actuator module (dedicated user, [Actuator] with a Neutral command)
	// and a plain talker. Killed, the actuator's Neutral runs; given up, the
	// system is in failsafe until clear-failsafe, which needs it restarted.
	manifest("actuator", "[Module]\nExec=%s/actuator --rate 50\nUser=hg-thrusters\n"
			     "[Subscribe ctl.cmd]\n[Subscribe safety.state]\n"
			     "[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\nInputs=ctl.cmd\n"
			     "[Actuator]\nCommand=ctl.cmd\nNeutral=%s/neutral %s\n"
			     "[Restart]\nBackoff=0.1s..0.2s\nBurst=2/60s\n", T_toys, T_toys, ran);
	manifest("talker", "[Module]\nExec=%s/talker --topic test.depth --rate 20\n[Publish test.depth]\nType=depth\nRate=20\n",
		 T_toys);
	mgr = manager_start();
	CHECK(wait_state("actuator", "ready", 5000));
	CHECK(wait_state("talker", "ready", 5000));
	// The Neutral record exists while it runs.
	char rec[1024];
	tfmt(rec, sizeof rec, "%s/run/hangar/neutral/actuator", T_root);
	const char *r = read_file(rec);
	CHECK(strncmp(r, "hangar-neutral 1\n", 17) == 0);
	CHECK(strstr(r, "\nargc 2\n") != NULL && strstr(r, "/neutral\n") != NULL);
	struct stat st;
	CHECK(stat(rec, &st) == 0 && (st.st_mode & 0777) == 0600);
	// Actuator defaults: OnGiveUp=failsafe, StopTimeout 500 ms (in the log
	// when it is killed after SIGTERM: not exercised here), a wiring with
	// the actuator's output.
	const char *w = ctl("wiring actuator");
	CHECK(strstr(w, "pub thr.cmd type=depth") && strstr(w, "inputs=ctl.cmd"));
	// SIGKILL: Neutral runs before anything else, then the module restarts.
	struct mstat m;
	module_status("actuator", &m);
	kill(m.pid, SIGKILL);
	CHECK(wait_file_contains(ran, "neutral uid=", 3000));
	CHECK(file_contains(ran, "module=actuator"));
	CHECK(wait_file_contains(T_log, "actuator: Neutral command ran", 2000));
	// The order in the log: the Neutral line precedes the exit line.
	const char *log = read_file(T_log);
	const char *neutral = strstr(log, "actuator: Neutral command ran");
	const char *exited = strstr(log, "actuator: pid");
	CHECK(neutral && exited && neutral < exited);
	CHECK(wait_new_pid("actuator", m.pid, "ready", 5000));
	// Two more kills: Burst=2/60s is exceeded, OnGiveUp=failsafe.
	module_status("actuator", &m);
	kill(m.pid, SIGKILL);
	CHECK(wait_new_pid("actuator", m.pid, "ready", 5000));
	module_status("actuator", &m);
	kill(m.pid, SIGKILL);
	CHECK(wait_state("actuator", "failed", 5000));
	CHECK(strcmp(system_state(), "failsafe") == 0);
	CHECK_EQ_INT(system_topic_state(), 2);
	int lines = 0;
	for (const char *p = read_file(ran); (p = strstr(p, "neutral uid=")); p += 12)
		lines++;
	CHECK_EQ_INT(lines, 3); // once per exit
	// clear-failsafe is refused while the module is failed, accepted after
	// its restart.
	const char *c = ctl("clear-failsafe");
	CHECK(strncmp(c, "err actuator is failed: restart it first", 40) == 0);
	CHECK(strncmp(ctl("restart actuator"), "ok", 2) == 0);
	CHECK(wait_state("actuator", "ready", 5000));
	CHECK(strcmp(system_state(), "failsafe") == 0); // latched
	CHECK(strncmp(ctl("clear-failsafe"), "ok", 2) == 0);
	CHECK(strcmp(system_state(), "nominal") == 0);
	// Shutdown order: the actuator (a module with a Neutral command) is
	// stopped and gone before the talker is signalled.
	CHECK(manager_stop(mgr) == 0);
	log = read_file(T_log);
	const char *act_stopped = strstr(log, "actuator: stopped: pid");
	const char *phase2 = strstr(log, "shutting down: the other modules");
	const char *talker_stopping = strstr(log, "talker: stopping: shutdown");
	CHECK(act_stopped && phase2 && talker_stopping);
	CHECK(act_stopped && phase2 && act_stopped < phase2);
	CHECK(phase2 && talker_stopping && phase2 < talker_stopping);
	// Neutral ran once more as the actuator exited at shutdown, and the
	// record is gone with the registry.
	lines = 0;
	for (const char *p = read_file(ran); (p = strstr(p, "neutral uid=")); p += 12)
		lines++;
	CHECK_EQ_INT(lines, 4);
	CHECK(access(rec, F_OK) < 0);
	if (geteuid() != 0)
		printf("    (not root: the Neutral command ran as this user, not hg-thrusters; the user switch is skipped)\n");
	manifest_remove("actuator");
	manifest_remove("talker");
	unlink(T_log);
}

static void test_failing_neutral(void)
{
	// A Neutral command that fails puts the system in failsafe.
	manifest("badact", "[Module]\nExec=%s/actuator --rate 50\nUser=hg-bad\n"
			   "[Subscribe ctl.cmd]\n[Subscribe safety.state]\n"
			   "[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\n"
			   "[Actuator]\nCommand=ctl.cmd\nNeutral=%s/neutral %s/neutral-bad --fail\n", T_toys, T_toys, T_root);
	mgr = manager_start();
	CHECK(wait_state("badact", "ready", 5000));
	struct mstat m;
	module_status("badact", &m);
	kill(m.pid, SIGKILL);
	CHECK(wait_file_contains(T_log, "badact: Neutral command failed (exit 1): failsafe", 3000));
	CHECK(wait_file_contains(T_log, "system failsafe: Neutral command failed", 1000));
	CHECK(strcmp(system_state(), "failsafe") == 0);
	CHECK(manager_stop(mgr) == 0);
	manifest_remove("badact");
	unlink(T_log);
}

int main(void)
{
	hd_setup("health", NULL);
	RUN_TEST(test_stale_restart_and_backoff);
	RUN_TEST(test_reset_after);
	RUN_TEST(test_burst_degrade_and_exit78);
	RUN_TEST(test_never_ready_and_watchdog);
	RUN_TEST(test_actuator_neutral_and_failsafe);
	RUN_TEST(test_failing_neutral);
	if (checks_failed)
		printf("--- hangard.log (last) ---\n%s", read_file(T_log));
	hd_teardown();
	TEST_MAIN_END();
}
