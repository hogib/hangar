// SPDX-License-Identifier: GPL-3.0-or-later
// Starting a module (docs/03 "Starting a module", "Neutral commands"): what
// must hold before the child is released, and what it is handed.
#include "hd_test.h"

static pid_t mgr;

/* The state letter and parent of a process, from /proc/<pid>/stat. */
static int proc_stat(pid_t pid, char *state, int *ppid)
{
	char path[64], buf[1024];
	tfmt(path, sizeof path, "/proc/%d/stat", (int)pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	const char *p = strrchr(buf, ')');
	return p && sscanf(p + 1, " %c %d", state, ppid) == 2 ? 0 : -1;
}

/* A child of parent, or 0. */
static pid_t child_of(pid_t parent)
{
	DIR *d = opendir("/proc");
	if (!d)
		return 0;
	struct dirent *e;
	pid_t found = 0;
	while (!found && (e = readdir(d))) {
		int pid = atoi(e->d_name), ppid = 0;
		char st;
		if (pid > 0 && proc_stat(pid, &st, &ppid) == 0 && ppid == (int)parent)
			found = pid;
	}
	closedir(d);
	return found;
}

static int gone(pid_t pid)
{
	char st = 0;
	int ppid;
	return proc_stat(pid, &st, &ppid) < 0 || st == 'Z';
}

/* docs/03, step 1 and "Tests": a manager killed between clone3 and the
 * registry write: the child never runs, and does not stay either. The
 * manager is held in the registry write by a FIFO in the temporary file's
 * place, and killed there. */
static void test_manager_killed_before_the_release(void)
{
	char fifo[1024];
	tfmt(fifo, sizeof fifo, "%s/run/hangar/.registry.tmp", T_root);
	mgr = manager_start();
	CHECK(mkfifo(fifo, 0600) == 0);
	manifest("m", "[Module]\nExec=%s/stubborn --polite\n", T_toys);
	kill(mgr, SIGHUP);
	pid_t child = 0;
	for (int i = 0; i < 100 && !(child = child_of(mgr)); i++)
		sleep_ms(20);
	CHECK(child > 0);
	sleep_ms(100);
	CHECK(!file_contains(T_log, "m: started"));
	manager_kill9(mgr);
	int left = 1;
	for (int i = 0; i < 100 && child > 0 && (left = !gone(child)); i++)
		sleep_ms(20);
	CHECK(!left);
	if (child > 0 && left)
		kill(child, SIGKILL); /* ours: the manager's only child */
	unlink(fifo);
	manifest_remove("m");
	unlink(T_log);
	char reg[1024];
	tfmt(reg, sizeof reg, "%s/run/hangar/registry", T_root);
	unlink(reg);
}

/* docs/03 "Neutral commands": the record is written before the child is
 * released, so a module never runs without one. The record's directory is
 * made read-only (a full /run does the same): the actuator is not started,
 * each attempt is a failure, and after Burst it is given up, which for an
 * actuator is failsafe. */
static void test_no_module_without_its_neutral_record(void)
{
	char ran[1024], dir[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran", T_root);
	tfmt(dir, sizeof dir, "%s/run/hangar/neutral", T_root);
	mgr = manager_start();
	CHECK(chmod(dir, 0555) == 0);
	manifest("act", "[Module]\nExec=%s/actuator --rate 50\nUser=hg-thrusters\n"
			"[Subscribe ctl.cmd]\n[Subscribe safety.state]\n"
			"[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\nInputs=ctl.cmd\n"
			"[Actuator]\nCommand=ctl.cmd\nNeutral=%s/neutral %s\n", T_toys, T_toys, ran);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_file_contains(T_log, "act: cannot write the Neutral record", 2000));
	struct mstat m;
	int ran_ever = 0;
	for (int i = 0; i < 25; i++) {
		if (module_status("act", &m) && (m.pid > 0 || strcmp(m.state, "ready") == 0 || strcmp(m.state, "starting") == 0))
			ran_ever = 1;
		sleep_ms(20);
	}
	CHECK(!ran_ever);
	CHECK(!file_contains(T_log, "act: started"));
	CHECK(wait_state("act", "failed", 8000));
	CHECK(strcmp(system_state(), "failsafe") == 0);
	chmod(dir, 0755);
	manager_stop(mgr);
	manifest_remove("act");
	unlink(T_log);
}

/* The record belongs to the instance: one started from a manifest without
 * a Neutral has none, though its predecessor had; a module removed by a
 * reload leaves none behind. */
static void test_no_record_outlives_its_instance(void)
{
	char ran[1024], rec[1024];
	tfmt(ran, sizeof ran, "%s/neutral-ran2", T_root);
	tfmt(rec, sizeof rec, "%s/run/hangar/neutral/act", T_root);
	const char *with = "[Module]\nExec=%s/actuator --rate 50\nUser=hg-thrusters\n"
			   "[Subscribe ctl.cmd]\n[Subscribe safety.state]\n"
			   "[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\nInputs=ctl.cmd\n"
			   "[Actuator]\nCommand=ctl.cmd\nNeutral=%s/neutral %s\n";
	manifest("act", with, T_toys, T_toys, ran);
	mgr = manager_start();
	CHECK(wait_state("act", "ready", 5000));
	CHECK(access(rec, F_OK) == 0);
	struct mstat m;
	module_status("act", &m);
	manifest("act", "[Module]\nExec=%s/stubborn --polite\n", T_toys);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_new_pid("act", m.pid, "ready", 5000));
	CHECK(access(rec, F_OK) < 0);
	/* Back to an actuator, then removed. */
	module_status("act", &m);
	manifest("act", with, T_toys, T_toys, ran);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_new_pid("act", m.pid, "ready", 5000));
	CHECK(access(rec, F_OK) == 0);
	manifest_remove("act");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	for (int i = 0; i < 150 && module_status("act", &m); i++)
		sleep_ms(20);
	CHECK(!module_status("act", &m));
	CHECK(access(rec, F_OK) < 0);
	manager_stop(mgr);
	unlink(T_log);
}

/* The module's environment: the manager's HANGAR_* variables whatever the
 * manager's own environment holds (300 variables of 50 bytes here, more than
 * the child's is built for), the manifest's Environment= over an inherited
 * variable of the same name, and no HANGAR_* name from a manifest. */
static void test_environment(void)
{
	char envf[1024], name[32];
	tfmt(envf, sizeof envf, "%s/env-out", T_root);
	static char big[40];
	memset(big, 'x', sizeof big - 1);
	for (int i = 0; i < 300; i++) {
		tfmt(name, sizeof name, "HD_TEST_PAD_%d", i);
		setenv(name, big, 1);
	}
	setenv("FOO", "inherited", 1);
	manifest("stub", "[Module]\nExec=%s/stubborn --polite --env %s\nEnvironment=FOO=bar\n", T_toys, envf);
	manifest("bad", "[Module]\nExec=%s/stubborn --polite\nEnvironment=HANGAR_WIRING=\nEnvironment=FOO=bar\n", T_toys);
	mgr = manager_start();
	for (int i = 0; i < 300; i++) {
		tfmt(name, sizeof name, "HD_TEST_PAD_%d", i);
		unsetenv(name);
	}
	unsetenv("FOO");
	CHECK(wait_state("stub", "ready", 3000));
	CHECK(file_contains(envf, "MODULE=stub CONTROL=set"));
	CHECK(file_contains(envf, "FOO=bar"));
	CHECK(!file_contains(envf, "WIRING=(unset)"));
	CHECK(file_contains(T_log, "stub: the environment does not fit"));
	struct mstat m;
	CHECK(module_status("bad", &m));
	CHECK(strcmp(m.state, "refused") == 0);
	CHECK(strstr(m.status, "HANGAR_") != NULL);
	manager_stop(mgr);
	manifest_remove("stub");
	manifest_remove("bad");
	unlink(T_log);
}

int main(void)
{
	hd_setup("spawn", NULL);
	HD_RUN(test_manager_killed_before_the_release);
	HD_RUN(test_no_module_without_its_neutral_record);
	HD_RUN(test_no_record_outlives_its_instance);
	HD_RUN(test_environment);
	hd_teardown();
	TEST_MAIN_END();
}
