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

static int count(const char *hay, const char *needle)
{
	int n = 0;
	for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle))
		n++;
	return n;
}

/* An actuator's manifest: `stubborn` with a Neutral command that appends a
 * line to ran. */
static void actuator_manifest(const char *name, const char *flags, const char *ran, const char *restart)
{
	manifest(name, "[Module]\nExec=%s/stubborn%s\nUser=hg-thrusters\n"
		       "[Subscribe ctl.cmd]\n[Subscribe safety.state]\n"
		       "[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\nInputs=ctl.cmd\n"
		       "[Actuator]\nCommand=ctl.cmd\nNeutral=%s/neutral %s\n%s", T_toys, flags, T_toys, ran, restart);
}

/* docs/03 "Neutral commands": at every exit, from the record. An actuator
 * whose manifest a reload removed is stopped, and its Neutral command runs. */
static void test_neutral_runs_when_manifest_removed(void)
{
	char ran[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran1", T_root);
	unlink(ran);
	actuator_manifest("act", " --polite", ran, "");
	mgr = manager_start();
	CHECK(wait_state("act", "ready", 5000));
	manifest_remove("act");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_file_contains(T_log, "act: stopped: pid", 3000));
	CHECK_EQ_INT(count(read_file(ran), "neutral uid="), 1);
	end("act");
}

/* The same, the module ignoring SIGTERM: the SIGKILL comes after the 500 ms
 * of an actuator, which the instance was started with, not the 5 s of a
 * module nothing is known about; then the Neutral command. */
static void test_removed_actuator_keeps_its_stop_timeout(void)
{
	char ran[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran2", T_root);
	unlink(ran);
	actuator_manifest("act", "", ran, "");
	mgr = manager_start();
	CHECK(wait_state("act", "ready", 5000));
	manifest_remove("act");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	uint64_t t0 = now_ms();
	CHECK(wait_file_contains(T_log, "SIGKILL to its", 8000));
	uint64_t kill_ms = now_ms() - t0;
	CHECK(kill_ms < 1500);
	CHECK(wait_file_contains(T_log, "act: stopped: pid", 3000));
	CHECK_EQ_INT(count(read_file(ran), "neutral uid="), 1);
	end("act");
}

/* A manifest edited to have no Neutral any more: the instance that was
 * started with one is restarted by the reload, and neutralized as it exits. */
static void test_neutral_runs_when_manifest_drops_neutral(void)
{
	char ran[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran3", T_root);
	unlink(ran);
	actuator_manifest("act", " --polite", ran, "");
	mgr = manager_start();
	CHECK(wait_state("act", "ready", 5000));
	struct mstat m;
	module_status("act", &m);
	manifest("act", "[Module]\nExec=%s/stubborn --polite\n", T_toys);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_new_pid("act", m.pid, "ready", 5000));
	CHECK_EQ_INT(count(read_file(ran), "neutral uid="), 1);
	end("act");
}

/* The instance's StopTimeout (2 s here) crosses a re-exec: removed after
 * it, the module is killed 2 s after the reload, not after the 500 ms an
 * actuator with a record and no manifest would get. */
static void test_stop_timeout_survives_reexec(void)
{
	char ran[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran4", T_root);
	unlink(ran);
	actuator_manifest("act", "", ran, "[Restart]\nStopTimeout=2s\n");
	mgr = manager_start();
	CHECK(wait_state("act", "ready", 5000));
	CHECK(strncmp(ctl("reexec"), "ok", 2) == 0);
	CHECK(wait_file_contains(T_log, "re-executed", 3000));
	manifest_remove("act");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	uint64_t t0 = now_ms();
	CHECK(wait_file_contains(T_log, "SIGKILL to its", 8000));
	uint64_t kill_ms = now_ms() - t0;
	CHECK(kill_ms > 1700 && kill_ms < 3200);
	CHECK(wait_file_contains(T_log, "act: stopped: pid", 3000));
	CHECK_EQ_INT(count(read_file(ran), "neutral uid="), 1);
	end("act");
}

/* And a crash: the registry carries it for the manager that adopts the
 * module and finds its manifest gone. A registry of a manager before this
 * one has no such field: then the module's Neutral record says it takes an
 * actuator's 500 ms. */
static void test_stop_timeout_survives_crash(void)
{
	for (int old = 0; old < 2; old++) {
		char ran[1024], reg[1024];
		tfmt(ran, sizeof ran, "%s/neutral-ran5%d", T_root, old);
		tfmt(reg, sizeof reg, "%s/run/hangar/registry", T_root);
		unlink(ran);
		actuator_manifest("act", "", ran, "[Restart]\nStopTimeout=2s\n");
		mgr = manager_start();
		CHECK(wait_state("act", "ready", 5000));
		manager_kill9(mgr);
		if (old) {
			/* The eight fields of the registry before: name, pid, start
			 * time, state, failures, delay, the two hashes. */
			char text[4096], out[4096];
			tfmt(text, sizeof text, "%s", read_file(reg));
			size_t o = 0;
			for (char *l = strtok(text, "\n"); l; l = strtok(NULL, "\n")) {
				int sp = 0;
				size_t len = strlen(l);
				if (strncmp(l, "act ", 4) == 0)
					for (size_t i = 0; i < len; i++)
						if (l[i] == ' ' && ++sp == 8) {
							len = i;
							break;
						}
				o += (size_t)tfmt(out + o, sizeof out - o, "%.*s\n", (int)len, l);
			}
			CHECK(write_file(reg, out) == 0);
		}
		manifest_remove("act");
		log_clear();
		mgr = manager_start();
		uint64_t t0 = now_ms();
		CHECK(wait_file_contains(T_log, "act: no manifest any more: stopping", 3000));
		CHECK(wait_file_contains(T_log, "SIGKILL to its", 8000));
		uint64_t kill_ms = now_ms() - t0;
		if (old)
			CHECK(kill_ms < 1500);
		else
			CHECK(kill_ms > 1700 && kill_ms < 3500);
		CHECK(wait_file_contains(T_log, "Neutral command ran", 3000));
		CHECK_EQ_INT(count(read_file(ran), "neutral uid="), 1);
		end("act");
	}
}

int main(void)
{
	hd_setup("stop", NULL);
	HD_RUN(test_stop_needing_sigkill_stays_stopped);
	HD_RUN(test_restart_needing_sigkill_counts_no_failure);
	HD_RUN(test_stop_survives_reexec_after_sigkill);
	HD_RUN(test_repeated_stop_keeps_the_deadline);
	HD_RUN(test_neutral_runs_when_manifest_removed);
	HD_RUN(test_removed_actuator_keeps_its_stop_timeout);
	HD_RUN(test_neutral_runs_when_manifest_drops_neutral);
	HD_RUN(test_stop_timeout_survives_reexec);
	HD_RUN(test_stop_timeout_survives_crash);
	hd_teardown();
	TEST_MAIN_END();
}
