// SPDX-License-Identifier: GPL-3.0-or-later
// The control socket against a caller that does not behave (docs/03
// "Control socket": one request per packet, one reply per request): any
// process in group hangar, so any module, can connect.
#include "hd_test.h"

static pid_t mgr;

/* A connection that sends nothing, kept open; -1 if the manager's backlog
 * is full at the moment. */
static int idle_conn(void)
{
	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s < 0)
		return -1;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	tfmt(sa.sun_path, sizeof sa.sun_path, "%s", T_control);
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(s);
		return -1;
	}
	return s;
}

/* 1 if the manager has closed its end. */
static int closed_by_peer(int s)
{
	char b[8];
	return recv(s, b, sizeof b, MSG_DONTWAIT) == 0;
}

/* Connections that never send a request: the manager closes each after a
 * moment, keeps answering meanwhile, and does not spin when they are more
 * than it has descriptors for (96 here, so that the test is cheap). */
static void test_idle_connections(void)
{
	manifest("m", "[Module]\nExec=%s/stubborn --polite\n", T_toys);
	T_nofile = 96;
	mgr = manager_start();
	T_nofile = 0;
	CHECK(wait_state("m", "ready", 5000));
	static int fds[400];
	int nf = 0;
	for (int i = 0; i < 20; i++)
		if ((fds[nf] = idle_conn()) >= 0)
			nf++;
	CHECK_EQ_INT(nf, 20);
	CHECK(strncmp(ctl_ms("status", 1000), "ok", 2) == 0);
	/* Left alone, each is closed: a request comes at once or not at all. */
	sleep_ms(3000);
	int closed = 0;
	for (int i = 0; i < nf; i++)
		closed += closed_by_peer(fds[i]);
	CHECK_EQ_INT(closed, nf);
	/* More than the manager can hold. */
	for (int i = 0; i < 300 && nf < 400; i++) {
		int c = idle_conn();
		if (c >= 0)
			fds[nf++] = c;
		else
			sleep_ms(2);
	}
	CHECK(nf > 200);
	long long t0 = th_cpu_ticks(mgr);
	int answered = strncmp(ctl_ms("status", 1000), "ok", 2) == 0;
	CHECK(answered);
	sleep_ms(1000);
	double cpu = th_cpu_percent(t0, th_cpu_ticks(mgr), 1000);
	CHECK(cpu >= 0 && cpu < 20.0);
	/* And it can still do what needs descriptors: a restart. */
	struct mstat m;
	int ok = answered && module_status("m", &m);
	ok = ok && strncmp(ctl_ms("restart m", 2000), "ok", 2) == 0;
	CHECK(ok);
	CHECK(ok && wait_new_pid("m", m.pid, "ready", 5000));
	for (int i = 0; i < nf; i++)
		close(fds[i]);
	if (answered) {
		manager_stop(mgr);
	} else {
		char reg[1024];
		int pid = 0;
		tfmt(reg, sizeof reg, "%s/run/hangar/registry", T_root);
		const char *l = strstr(read_file(reg), "\nm ");
		if (l)
			sscanf(l + 3, "%d", &pid);
		manager_kill9(mgr);
		if (pid > 1)
			kill(pid, SIGKILL); /* the module of the manager just killed */
		unlink(reg);
	}
	manifest_remove("m");
	unlink(T_log);
}

int main(void)
{
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 1024 && rl.rlim_max >= 1024) {
		rl.rlim_cur = 1024;
		setrlimit(RLIMIT_NOFILE, &rl);
	}
	hd_setup("control", NULL);
	HD_RUN(test_idle_connections);
	hd_teardown();
	TEST_MAIN_END();
}
