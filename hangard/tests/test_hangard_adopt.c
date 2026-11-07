// SPDX-License-Identifier: GPL-3.0-or-later
// Adoption (docs/03): the manager killed with SIGKILL, the modules keep
// publishing, a new manager adopts them from the registry, flags the topics
// of a module that died meanwhile and treats a registry pid that belongs to
// another process as dead; the system state survives; reload with a changed,
// a removed and an added manifest; re-execution keeping every module as a
// child and the manager's own topics' sequence numbers; the idle manager's
// wake-ups.
#include "hd_test.h"

static pid_t mgr;

static int ppid_of(pid_t pid)
{
	char path[64], buf[4096];
	tfmt(path, sizeof path, "/proc/%d/status", (int)pid);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	const char *p = strstr(buf, "\nPPid:");
	return p ? atoi(p + 6) : -1;
}

static long ctxt_switches(pid_t pid)
{
	char path[64], buf[4096];
	tfmt(path, sizeof path, "/proc/%d/status", (int)pid);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	long v = 0, nv = 0;
	const char *p = strstr(buf, "voluntary_ctxt_switches:");
	if (p)
		v = atol(p + 24);
	p = strstr(buf, "nonvoluntary_ctxt_switches:");
	if (p)
		nv = atol(p + 27);
	return v + nv;
}

static void write_manifests(void)
{
	manifest("t1", "[Module]\nExec=%s/talker --topic t1.out --rate 50\n[Publish t1.out]\nType=depth\nRate=50\n", T_toys);
	manifest("t2", "[Module]\nExec=%s/talker --topic t2.out --rate 50\n[Publish t2.out]\nType=depth\nRate=50\n"
		       "[Restart]\nBackoff=0.1s..0.2s\n", T_toys);
}

static void test_adoption(void)
{
	write_manifests();
	mgr = manager_start();
	CHECK(wait_state("t1", "ready", 5000));
	CHECK(wait_state("t2", "ready", 5000));
	struct mstat m1, m2;
	module_status("t1", &m1);
	module_status("t2", &m2);
	// Kill the manager under load: the modules keep publishing.
	manager_kill9(mgr);
	uint64_t s1 = topic_seq("t1.out");
	sleep_ms(300);
	uint64_t s2 = topic_seq("t1.out");
	CHECK(s2 > s1);
	CHECK(kill(m1.pid, 0) == 0 && kill(m2.pid, 0) == 0);
	// t2 dies while no manager runs; its topic is not flagged yet.
	kill(m2.pid, SIGKILL);
	sleep_ms(100);
	unsigned char h[4096];
	CHECK_EQ_INT(topic_header("t2.out", h), 0);
	CHECK(!(h32(h, H_FLAGS) & HG_FLAG_DIED));
	// A registry line whose pid now belongs to another process (pid 1's
	// start time differs from what we write) is treated as dead.
	char path[1024];
	tfmt(path, sizeof path, "%s/run/hangar/registry", T_root);
	const char *reg = read_file(path);
	static char newreg[8192];
	tfmt(newreg, sizeof newreg, "%sghost 1 12345 ready 0 0 0 0\n", reg);
	write_file(path, newreg);
	manifest("ghost", "[Module]\nExec=%s/eventer\n", T_toys);
	// A new manager adopts t1, finds t2 and ghost dead.
	mgr = manager_start();
	CHECK(wait_file_contains(T_log, "t1: adopted pid", 3000));
	CHECK(wait_file_contains(T_log, "t2: pid", 3000) && file_contains(T_log, "is gone (registry last written at"));
	CHECK(file_contains(T_log, "ghost: pid 1 is gone"));
	CHECK(wait_state("t1", "ready", 5000));
	struct mstat m;
	module_status("t1", &m);
	CHECK_EQ_INT(m.pid, m1.pid); // the same process
	CHECK(strcmp(m.last_exit, "-") == 0 || strcmp(m.last_exit, "unknown") == 0);
	// t2's topic carries bit 1 now, and t2 is restarted under its rules.
	CHECK_EQ_INT(topic_header("t2.out", h), 0);
	CHECK(h32(h, H_FLAGS) & HG_FLAG_DIED);
	CHECK(file_contains(T_log, "t2: backoff: died while no manager was running"));
	CHECK(wait_state("t2", "ready", 5000));
	module_status("t2", &m);
	CHECK(m.pid != m2.pid && m.failures == 1);
	CHECK(wait_state("ghost", "ready", 5000));
	// The adopted module's page still works: ready, status line.
	uint64_t inst = 0, ready = 0, alive = 0;
	char st[128];
	CHECK_EQ_INT(page_read("t1", &inst, &ready, &alive, st), 0);
	CHECK(ready == inst && strcmp(st, "publishing") == 0);
	// t1 keeps publishing in the same file, no replacement: its first_seq
	// is still 0 and its sequence continued.
	CHECK_EQ_INT(topic_header("t1.out", h), 0);
	CHECK_EQ_INT(h64(h, H_FIRST_SEQ), 0);
	CHECK(h64(h, H_WRITE_SEQ) > s2);
	// The adopted module's exit is handled although it is not our child.
	kill(m1.pid, SIGKILL);
	CHECK(wait_file_contains(T_log, "t1: pid", 3000));
	CHECK(wait_file_contains(T_log, "status unknown (adopted)", 3000));
	CHECK(wait_new_pid("t1", m1.pid, "ready", 5000));
	manifest_remove("ghost");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
}

static void test_system_state_kept(void)
{
	log_clear();
	// Degraded survives a manager crash: the state is in the registry.
	manifest("giveup", "[Module]\nExec=%s/crasher --topic g.out --crash-after 1\n[Publish g.out]\nType=depth\nRate=50\n"
			   "[Restart]\nBackoff=0.1s..0.1s\nBurst=1/60s\n", T_toys);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_state("giveup", "failed", 5000));
	CHECK(strcmp(system_state(), "degraded") == 0);
	manager_kill9(mgr);
	mgr = manager_start();
	CHECK(wait_file_contains(T_log, "registry:", 3000));
	CHECK(file_contains(T_log, "system degraded: giveup given up"));
	CHECK(strcmp(system_state(), "degraded") == 0);
	struct mstat m;
	CHECK(module_status("giveup", &m) && strcmp(m.state, "failed") == 0);
	CHECK(file_contains(T_log, "giveup: failed in the registry: left down"));
	manifest_remove("giveup");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	sleep_ms(200);
	CHECK(strcmp(system_state(), "nominal") == 0);
}

static void test_reload(void)
{
	log_clear();
	struct mstat m1, m2;
	module_status("t1", &m1);
	module_status("t2", &m2);
	// t1 unchanged, t2 changed (its rate), t3 added; later t1 removed.
	manifest("t2", "[Module]\nExec=%s/talker --topic t2.out --rate 20\n[Publish t2.out]\nType=depth\nRate=20\n"
		       "[Restart]\nBackoff=0.1s..0.2s\n", T_toys);
	manifest("t3", "[Module]\nExec=%s/listener --topic t2.out\n[Subscribe t2.out]\n", T_toys);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_state("t3", "ready", 5000));
	CHECK(wait_file_contains(T_log, "reload: t2 changed", 1000));
	CHECK(file_contains(T_log, "reload: t3 added"));
	CHECK(!file_contains(T_log, "reload: t1 changed"));
	CHECK(wait_state("t2", "ready", 5000));
	struct mstat m;
	module_status("t1", &m);
	CHECK_EQ_INT(m.pid, m1.pid); // untouched
	module_status("t2", &m);
	CHECK(m.pid != m2.pid && m.failures == 0);
	unsigned char h[4096];
	CHECK_EQ_INT(topic_header("t2.out", h), 0);
	CHECK_EQ_INT(h32(h, 60), 20000); // the new rate, in the new file
	// The wiring handed to t3 names the publisher's type and rate.
	const char *w = ctl("wiring t3");
	CHECK(strstr(w, "sub t2.out type=depth hash=f397a321cd6643a1 rate=20") != NULL);
	// A refused graph changes nothing.
	manifest("t4", "[Module]\nExec=%s/talker --topic t2.out\n[Publish t2.out]\nType=depth\nRate=20\n", T_toys);
	const char *r = ctl("reload");
	CHECK(strncmp(r, "err reload refused, nothing changed: ", 37) == 0);
	CHECK(strstr(r, "t2.out is also published by") != NULL);
	CHECK(!module_status("t4", &m));
	module_status("t2", &m);
	CHECK(strcmp(m.state, "ready") == 0);
	manifest_remove("t4");
	// Removed: stopped and gone from status.
	manifest_remove("t1");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_file_contains(T_log, "reload: t1 removed", 1000));
	for (int i = 0; i < 100 && module_status("t1", &m); i++)
		sleep_ms(20);
	CHECK(!module_status("t1", &m));
	CHECK(kill(m1.pid, 0) < 0);
	// SIGHUP reloads too.
	write_manifests();
	kill(mgr, SIGHUP);
	CHECK(wait_state("t1", "ready", 5000));
}

static void test_reexec(void)
{
	log_clear();
	struct mstat m1, m2;
	module_status("t1", &m1);
	module_status("t2", &m2);
	CHECK_EQ_INT(ppid_of(m1.pid), (int)mgr);
	uint64_t sys_seq = topic_seq("hangar.vehicle.system");
	// A reader of the manager's topic, across the re-exec.
	hg_sub *sub = hg_sub_open("hangar.vehicle.system", NULL);
	CHECK(strncmp(ctl("reexec"), "ok", 2) == 0);
	CHECK(wait_file_contains(T_log, "re-executed: ", 5000));
	CHECK(control_ready(5000));
	CHECK(kill(mgr, 0) == 0); // the same pid: exec in place
	CHECK(wait_state("t1", "ready", 5000));
	CHECK(wait_state("t2", "ready", 5000));
	struct mstat m;
	module_status("t1", &m);
	CHECK_EQ_INT(m.pid, m1.pid);
	module_status("t2", &m);
	CHECK_EQ_INT(m.pid, m2.pid);
	CHECK_EQ_INT(ppid_of(m1.pid), (int)mgr); // still children
	CHECK(!file_contains(T_log, "adopted pid"));
	// Its own topics continue their sequence numbers: the new file's
	// first_seq is the old write_seq, and a reader sees no gap.
	sleep_ms(1200);
	unsigned char h[4096];
	CHECK_EQ_INT(topic_header("hangar.vehicle.system", h), 0);
	CHECK(h64(h, H_FIRST_SEQ) >= sys_seq);
	CHECK(h64(h, H_WRITE_SEQ) > sys_seq);
	if (sub) {
		unsigned char buf[256];
		struct hg_info info;
		int msgs = 0;
		uint64_t gaps = 0;
		for (int i = 0; i < 20 && hg_next(sub, buf, sizeof buf, &info) == HG_MSG; i++) {
			msgs++;
			gaps += info.gap;
		}
		CHECK(msgs > 0);
		CHECK_EQ_INT(gaps, 0);
		hg_sub_close(sub);
	}
	// A module that exits after the re-exec is still reaped with its status.
	kill(m2.pid, SIGKILL);
	CHECK(wait_file_contains(T_log, "t2: pid", 3000) && wait_file_contains(T_log, "killed by signal 9", 3000));
	CHECK(!strstr(read_file(T_log), "status unknown"));
	CHECK(wait_new_pid("t2", m2.pid, "ready", 5000));
	// The restart-manager command does the same.
	static char out[4096];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "restart-manager", (char *)NULL), 0);
	CHECK(control_ready(5000));
	CHECK(wait_state("t1", "ready", 5000));
}

static void test_idle_wakeups(void)
{
	// With no rated topic and no watchdog the health tick stops; what is
	// left is the once-a-second publication of the manager's own topics
	// (docs/03 "Event loop"): a wake-up a second, no more.
	manifest_remove("t1");
	manifest_remove("t2");
	manifest("quiet", "[Module]\nExec=%s/eventer --alive-every 1000\n", T_toys);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_state("quiet", "ready", 5000));
	sleep_ms(500);
	long c0 = ctxt_switches(mgr);
	sleep_ms(3000);
	long c1 = ctxt_switches(mgr);
	printf("    idle manager: %ld context switches in 3 s (the 1 Hz publication accounts for 3)\n", c1 - c0);
	CHECK(c1 - c0 <= 4);
	CHECK(c1 - c0 >= 2);
	manifest_remove("quiet");
	CHECK(manager_stop(mgr) == 0);
}

int main(void)
{
	hd_setup("adopt", NULL);
	RUN_TEST(test_adoption);
	RUN_TEST(test_system_state_kept);
	RUN_TEST(test_reload);
	RUN_TEST(test_reexec);
	RUN_TEST(test_idle_wakeups);
	if (checks_failed)
		printf("--- hangard.log ---\n%s", read_file(T_log));
	hd_teardown();
	TEST_MAIN_END();
}
