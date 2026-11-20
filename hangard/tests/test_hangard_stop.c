// SPDX-License-Identifier: GPL-3.0-or-later
// Stops (docs/03 "Stopping a module", "Neutral commands"): a module that
// ignores SIGTERM is killed after StopTimeout and the exit is still the
// stop's, whether the operator asked for a stop or a restart, and whether
// or not the manager re-executed meanwhile.
#include "hd_test.h"

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#include <sys/syscall.h>

#include "hg_type.h"

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

static void end(const char *name)
{
	struct mstat m;
	int pid = module_status(name, &m) ? m.pid : 0;
	manager_stop(mgr);
	if (pid > 1)
		kill(pid, SIGKILL); /* ours: the pid the manager listed for it */
	kill_leftover(name);
	manifest_remove(name);
	if (!getenv("HD_KEEP"))
		unlink(T_log);
}

/* `hangar stop` on a module that needs SIGKILL: stopped, and it stays so. */
static void test_stop_needing_sigkill_stays_stopped(void)
{
	manifest("stub", "[Module]\nExec=%s/stubborn\n[Restart]\nBackoff=0.2s..1s\nStopTimeout=300ms\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("stub", "ready", 5000));
	CHECK(strncmp(ctl("stop stub"), "ok", 2) == 0);
	CHECK(wait_file_contains(T_log, "SIGKILL to its", 3000));
	sleep_ms(1200); /* past the back-off a counted failure would wait */
	struct mstat m;
	CHECK(module_status("stub", &m));
	CHECK(strcmp(m.state, "stopped") == 0);
	CHECK_EQ_INT(m.pid, 0);
	CHECK_EQ_INT(m.failures, 0);
	CHECK(!file_contains(T_log, "stub: backoff"));
	end("stub");
}

/* `hangar restart` on the same module: a new instance, no failure counted. */
static void test_restart_needing_sigkill_counts_no_failure(void)
{
	manifest("stub", "[Module]\nExec=%s/stubborn\n[Restart]\nBackoff=0.2s..1s\nStopTimeout=300ms\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("stub", "ready", 5000));
	struct mstat m;
	module_status("stub", &m);
	int pid1 = m.pid;
	CHECK(strncmp(ctl("restart stub"), "ok", 2) == 0);
	CHECK(wait_new_pid("stub", pid1, "ready", 5000));
	module_status("stub", &m);
	CHECK_EQ_INT(m.failures, 0);
	CHECK(!file_contains(T_log, "(failure 1)"));
	end("stub");
}

/* Starts hangard as the image a re-exec leaves: the forked process starts a
 * Legacy module's program as its own child, writes the hand-off state with
 * that child's pid and pidfd, and executes the manager, which so has the
 * module as its child. module_rest is the state's module line after the
 * name, pid and pidfd; extra is appended to the state as it is. */
static pid_t manager_start_as_successor(const char *name, char *const prog[], const char *module_rest,
					const char *extra, pid_t *child)
{
	int back[2];
	if (pipe(back) < 0)
		exit(2);
	pid_t pid = fork();
	if (pid < 0)
		exit(2);
	if (pid == 0) {
		close(back[0]);
		int fd = open(T_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		pid_t c = fork();
		if (c == 0) {
			close(back[1]);
			setpgid(0, 0);
			execv(prog[0], prog);
			_exit(127);
		}
		setpgid(c, c);
		if (write(back[1], &c, sizeof c) != (ssize_t)sizeof c)
			_exit(126);
		close(back[1]);
		int pidfd = (int)syscall(SYS_pidfd_open, c, 0u);
		int st = (int)syscall(SYS_memfd_create, "hangard-state", 0u);
		char text[2048], num[16];
		int n = tfmt(text, sizeof text, "hangar-reexec 1\nsystem nominal 1 \nmodule %s %d %d %s\n%s", name, (int)c,
			     pidfd, module_rest, extra ? extra : "");
		if (pidfd < 0 || st < 0 || write(st, text, (size_t)n) != n)
			_exit(126);
		fcntl(pidfd, F_SETFD, 0); /* a pidfd is close-on-exec as it comes */
		tfmt(num, sizeof num, "%d", st);
		setenv("HANGAR_REEXEC_FD", num, 1);
		execl(T_hangard, T_hangard, (char *)NULL);
		_exit(127);
	}
	close(back[1]);
	*child = 0;
	if (read(back[0], child, sizeof *child) != (ssize_t)sizeof *child)
		*child = 0;
	close(back[0]);
	if (!control_ready(5000))
		printf("    (the control socket did not appear within 5 s)\n");
	return pid;
}

static const char *sleep_path(void)
{
	return access("/bin/sleep", X_OK) == 0 ? "/bin/sleep" : "/usr/bin/sleep";
}

/* The FNV-1a 64 of a manifest as it is on disk: what the hand-off state
 * names, so that the successor takes the module as unchanged. */
static unsigned long long manifest_hash(const char *name)
{
	char path[1024];
	tfmt(path, sizeof path, "%s/%s.module", T_modules, name);
	const char *t = read_file(path);
	return (unsigned long long)hg_fnv1a64(t, strlen(t));
}

/* A re-exec between the SIGKILL of a stop and the exit: the state says
 * stopping, with no deadline left. The exit is the stop's in the new image
 * too. The state is as a manager before this one wrote it. */
static void test_stop_survives_reexec_after_sigkill(void)
{
	manifest("sleeper", "[Module]\nExec=%s 1000\nLegacy=yes\n[Restart]\nBackoff=0.2s..1s\n", sleep_path());
	char rest[512];
	unsigned long long now = (unsigned long long)hg_now_ns();
	/* starttime state since failures delay restart_at started ready exit_known
	 * exit_status gave_up adopted manifest wiring stop_deadline after_stop
	 * count_failure n_fail_times */
	tfmt(rest, sizeof rest, "0 stopping %llu 0 0 0 %llu %llu 0 0 0 0 %016llx %016llx 0 stopped 0 0", now, now, now,
	     manifest_hash("sleeper"), 0ull);
	char *prog[] = { (char *)sleep_path(), (char *)"1000", NULL };
	pid_t child = 0;
	mgr = manager_start_as_successor("sleeper", prog, rest, NULL, &child);
	CHECK(child > 0);
	struct mstat m;
	CHECK(module_status("sleeper", &m));
	CHECK(strcmp(m.state, "stopping") == 0);
	CHECK_EQ_INT(m.pid, (int)child);
	if (child > 0)
		kill(child, SIGKILL); /* as the SIGKILL the old image sent */
	CHECK(wait_file_contains(T_log, "sleeper: pid", 3000));
	sleep_ms(1200);
	CHECK(module_status("sleeper", &m));
	CHECK(strcmp(m.state, "stopped") == 0);
	CHECK_EQ_INT(m.pid, 0);
	CHECK_EQ_INT(m.failures, 0);
	CHECK(!file_contains(T_log, "sleeper: backoff"));
	CHECK(!file_contains(T_log, "sleeper: started"));
	end("sleeper");
}

/* A stop asked for again while one is under way does not move the SIGKILL:
 * a stop every 200 ms, StopTimeout 400 ms. */
static void test_repeated_stop_keeps_the_deadline(void)
{
	manifest("stub", "[Module]\nExec=%s/stubborn\n[Restart]\nPolicy=never\nStopTimeout=400ms\n", T_toys);
	mgr = manager_start();
	CHECK(wait_state("stub", "ready", 5000));
	for (int i = 0; i < 8; i++) {
		ctl("stop stub"); /* refused once it has stopped */
		sleep_ms(200);
	}
	CHECK(file_contains(T_log, "SIGKILL to its"));
	CHECK(wait_state("stub", "stopped", 2000));
	end("stub");
}

int main(void)
{
	hd_setup("stop", NULL);
	HD_RUN(test_stop_needing_sigkill_stays_stopped);
	HD_RUN(test_restart_needing_sigkill_counts_no_failure);
	HD_RUN(test_stop_survives_reexec_after_sigkill);
	HD_RUN(test_repeated_stop_keeps_the_deadline);
	hd_teardown();
	TEST_MAIN_END();
}
