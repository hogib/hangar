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

int main(void)
{
	hd_setup("spawn", NULL);
	HD_RUN(test_manager_killed_before_the_release);
	hd_teardown();
	TEST_MAIN_END();
}
