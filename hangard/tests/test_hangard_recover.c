// SPDX-License-Identifier: GPL-3.0-or-later
// What the manager does with a state it did not make itself, or that changes
// under it: a module adopted under a manifest that is refused now, a
// refused module whose manifest was corrected while no manager ran, a reload
// and an exit arriving together, a shutdown with a module in back-off.
#include "hd_test.h"

static pid_t mgr;

/* The pid in a module's registry line, to kill what a failed test left. */
static void kill_leftover(const char *name)
{
	char path[1024], needle[64];
	tfmt(path, sizeof path, "%s/run/hangar/registry", T_root);
	const char *reg = read_file(path);
	tfmt(needle, sizeof needle, "\n%s ", name);
	const char *l = strstr(reg, needle);
	int pid = 0;
	if (l && sscanf(l + 1 + strlen(name), "%d", &pid) == 1 && pid > 1)
		kill(pid, SIGKILL);
}

/* A module that ignores SIGTERM, adopted by a manager whose manifest for it
 * is refused: it is stopped, killed, and listed refused with the reason;
 * the manager is idle afterwards. */
static void test_adopted_under_a_refused_manifest(void)
{
	manifest("stub", "[Module]\nExec=%s/stubborn\n[Restart]\nStopTimeout=300ms\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("stub", "ready", 5000));
	manager_kill9(mgr);
	manifest("stub", "[Module]\nExec=%s/stubborn\nNoSuchKey=1\n", T_toys);
	log_clear();
	mgr = manager_start();
	CHECK(wait_file_contains(T_log, "SIGKILL to its", 5000));
	sleep_ms(500);
	struct mstat m;
	CHECK(module_status("stub", &m));
	CHECK(strcmp(m.state, "refused") == 0);
	CHECK(strstr(m.status, "NoSuchKey") != NULL);
	CHECK_EQ_INT(m.pid, 0);
	CHECK_EQ_INT(m.failures, 0);
	long long t0 = th_cpu_ticks(mgr);
	sleep_ms(1000);
	CHECK(th_cpu_percent(t0, th_cpu_ticks(mgr), 1000) < 20.0);
	manager_stop(mgr);
	kill_leftover("stub");
	manifest_remove("stub");
	unlink(T_log);
}

/* Shutdown, first phase: an actuator that ignores SIGTERM takes its 2 s
 * StopTimeout; a module without a Neutral command is in back-off, and its
 * restart time passes meanwhile. Nothing is started, and the manager sleeps
 * until the actuator's deadline. */
static void test_shutdown_with_a_module_in_backoff(void)
{
	char ran[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran", T_root);
	manifest("act", "[Module]\nExec=%s/stubborn\nUser=hg-thrusters\n"
			"[Subscribe ctl.cmd]\n[Subscribe safety.state]\n"
			"[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\nInputs=ctl.cmd\n"
			"[Actuator]\nCommand=ctl.cmd\nNeutral=%s/neutral %s\n[Restart]\nStopTimeout=2s\n", T_toys, T_toys, ran);
	manifest("flap", "[Module]\nExec=%s/stubborn --exit 1\n[Restart]\nBackoff=0.1s..0.1s\nBurst=1000/60s\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("act", "ready", 5000));
	CHECK(wait_file_contains(T_log, "flap: backoff", 3000));
	kill(mgr, SIGTERM);
	CHECK(wait_file_contains(T_log, "shutting down", 2000));
	sleep_ms(200); /* flap's restart time has passed */
	long long t0 = th_cpu_ticks(mgr);
	sleep_ms(1000);
	double cpu = th_cpu_percent(t0, th_cpu_ticks(mgr), 1000);
	CHECK(cpu >= 0 && cpu < 20.0);
	CHECK_EQ_INT(wait_pid_ms(mgr, 5000), 0);
	CHECK(file_contains(ran, "neutral uid="));
	manifest_remove("act");
	manifest_remove("flap");
	unlink(T_log);
}

int main(void)
{
	hd_setup("recover", NULL);
	HD_RUN(test_adopted_under_a_refused_manifest);
	HD_RUN(test_shutdown_with_a_module_in_backoff);
	hd_teardown();
	TEST_MAIN_END();
}
