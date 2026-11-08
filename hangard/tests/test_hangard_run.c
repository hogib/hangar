// SPDX-License-Identifier: GPL-3.0-or-later
// The manager with two toy modules, as an ordinary user under $HANGAR_ROOT:
// start, ready via the page, the control socket's status/start/stop/wiring,
// `page` refused to a non-module, `open` refused for an undeclared topic, a
// wrong type and a second writer, the registry's contents, the journald
// stream protocol against a stand-in socket, the fallback log, the privileged
// steps skipped (or exercised as root), `hangar topics|echo|hz|show|run`.
#include "hd_test.h"

static pid_t mgr;

static void write_manifests(void)
{
	manifest("talker", "[Module]\nDescription=toy talker\nExec=%s/talker --topic test.depth --rate 50\n"
			   "[Publish test.depth]\nType=depth\nRate=50\n", T_toys);
	manifest("listener", "[Module]\nExec=%s/listener --topic test.depth\n[Subscribe test.depth]\n"
			     "[Health]\nWatchdog=2s\n", T_toys);
}

/* A stand-in for /run/systemd/journal/stdout: a listening stream socket. */
static int journal_listen(void)
{
	char path[1024];
	tfmt(path, sizeof path, "%s/run/systemd/journal", T_root);
	mkdir_p(path);
	tfmt(path, sizeof path, "%s/run/systemd/journal/stdout", T_root);
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);
	unlink(path);
	if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(s, 16) < 0) {
		printf("    cannot listen on %s: %s\n", path, strerror(errno));
		close(s);
		return -1;
	}
	return s;
}

static void test_start_and_ready(void)
{
	write_manifests();
	int js = journal_listen();
	mgr = manager_start();
	CHECK(wait_state("talker", "ready", 5000));
	CHECK(wait_state("listener", "ready", 5000));
	struct mstat m;
	CHECK(module_status("talker", &m) && m.pid > 0 && m.failures == 0);
	CHECK(strcmp(m.status, "publishing") == 0);
	// Ready via the page: ready_instance == instance, status line written.
	uint64_t inst = 0, ready = 0, alive = 0;
	char st[128] = "";
	CHECK_EQ_INT(page_read("talker", &inst, &ready, &alive, st), 0);
	CHECK(inst != 0 && ready == inst);
	CHECK_EQ_INT(page_read("listener", &inst, &ready, &alive, st), 0);
	CHECK(ready == inst && alive != 0);
	CHECK(strstr(st, "messages") != NULL);
	// The system is nominal and the topic exists with the module as writer.
	CHECK(strcmp(system_state(), "nominal") == 0);
	unsigned char h[4096];
	CHECK_EQ_INT(topic_header("test.depth", h), 0);
	CHECK(memcmp(h + H_WRITER, "talker", 7) == 0);
	CHECK((int)h32(h, H_WRITER_PID) == m.pid);
	CHECK(h64(h, H_WRITE_SEQ) > 0);
	// The journald streams: one connection per stream, the header lines of
	// the protocol (identifier, unit, priority, level prefix, forwards).
	static int conns[8];
	int got = 0, saw_err_stream = 0;
	for (int i = 0; i < 100 && got < 4; i++) {
		int c = accept4(js, NULL, NULL, SOCK_NONBLOCK);
		if (c < 0) {
			sleep_ms(20);
			continue;
		}
		sleep_ms(50);
		char buf[512] = "";
		ssize_t n = read(c, buf, sizeof buf - 1);
		if (n > 0) {
			buf[n] = 0;
			CHECK(strncmp(buf, "talker\n\n", 8) == 0 || strncmp(buf, "listener\n\n", 10) == 0);
			const char *p = strstr(buf, "\n\n");
			if (p) {
				CHECK(strncmp(p + 2, "6\n1\n0\n0\n0\n", 10) == 0 || strncmp(p + 2, "3\n1\n0\n0\n0\n", 10) == 0);
				if (strncmp(p + 2, "3\n", 2) == 0)
					saw_err_stream = 1;
			}
			got++;
		}
		conns[got < 8 ? got : 7] = c; /* kept open: a closed peer would SIGPIPE the module */
	}
	CHECK_EQ_INT(got, 4);
	CHECK(saw_err_stream);
	CHECK(conns[0] >= 0);
	close(js);
	char jpath[1024];
	tfmt(jpath, sizeof jpath, "%s/run/systemd/journal/stdout", T_root);
	unlink(jpath); /* later starts take the fallback log */
	// Not root: the privileged steps are skipped and the log names them.
	if (geteuid() != 0) {
		CHECK(file_contains(T_log, "skipping step 2 (cgroup)"));
		CHECK(file_contains(T_log, "skipping steps 7-9 (capabilities, user hangar, group hangar)"));
		printf("    (not root: user switching, scheduling and capabilities skipped, as logged)\n");
	} else {
		printf("    (root: privileged steps attempted)\n");
	}
}

static void test_control_requests(void)
{
	// status is readable; the wiring is the text handed to the module.
	const char *r = ctl("wiring listener");
	CHECK(strncmp(r, "ok\nhangar-wiring 1\nmodule listener\n", 35) == 0);
	CHECK(strstr(r, "sub test.depth type=depth hash=") != NULL);
	r = ctl("wiring nobody");
	CHECK(strncmp(r, "err no module nobody", 20) == 0);
	// page from a process that is not a module's main process.
	r = ctl("page");
	CHECK(strncmp(r, "err pid ", 8) == 0 && strstr(r, "not a module's main process"));
	// open from a non-module.
	r = ctl("open test.depth hash=0000000000000001 payload=16 schema=0\n");
	CHECK(strstr(r, "not a module's main process") != NULL);
	// paths: where the manager keeps things, the topic directory among them.
	r = ctl("paths");
	CHECK(strncmp(r, "ok\nroot ", 8) == 0);
	CHECK(strstr(r, "\ntopics /") != NULL && strstr(r, "\nmodules /") != NULL && strstr(r, "\nmsg /") != NULL &&
	      strstr(r, "\nlog /") != NULL);
	// An unknown request.
	r = ctl("bogus");
	CHECK(strncmp(r, "err unknown request bogus", 25) == 0);
	// stop, start, restart.
	struct mstat m;
	module_status("talker", &m);
	int pid = m.pid;
	r = ctl("stop talker");
	CHECK(strncmp(r, "ok", 2) == 0);
	CHECK(wait_state("talker", "stopped", 5000));
	CHECK(module_status("talker", &m));
	if (m.pid != 0 || strcmp(m.last_exit, "exit:0") != 0)
		printf("    talker after stop: state=%s pid=%d last_exit=%s status='%s'\n", m.state, m.pid, m.last_exit, m.status);
	CHECK(m.pid == 0 && strcmp(m.last_exit, "exit:0") == 0);
	// Its topic is flagged: the writer is gone (bit 1).
	unsigned char h[4096];
	CHECK_EQ_INT(topic_header("test.depth", h), 0);
	CHECK(h32(h, H_FLAGS) & HG_FLAG_DIED);
	r = ctl("stop talker");
	CHECK(strncmp(r, "err talker is not running", 25) == 0);
	r = ctl("start talker");
	CHECK(strncmp(r, "ok", 2) == 0);
	CHECK(wait_state("talker", "ready", 5000));
	CHECK(module_status("talker", &m) && m.pid != pid && m.pid > 0);
	// Sequence numbers continue across the restart: the new file's
	// first_seq is the old write_seq.
	CHECK_EQ_INT(topic_header("test.depth", h), 0);
	CHECK(h64(h, H_FIRST_SEQ) > 0);
	CHECK(h64(h, H_WRITE_SEQ) >= h64(h, H_FIRST_SEQ));
	CHECK(!(h32(h, H_FLAGS) & HG_FLAG_DIED));
	r = ctl("start talker");
	CHECK(strstr(r, "is running") != NULL);
	pid = m.pid;
	r = ctl("restart talker");
	CHECK(strncmp(r, "ok", 2) == 0);
	sleep_ms(300);
	CHECK(wait_state("talker", "ready", 5000));
	CHECK(module_status("talker", &m) && m.pid != pid);
	CHECK_EQ_INT(m.failures, 0); // a restart on request is not a failure
}

static void test_registry(void)
{
	char path[1024];
	tfmt(path, sizeof path, "%s/run/hangar/registry", T_root);
	const char *reg = read_file(path);
	CHECK(strncmp(reg, "hangar-registry 2\n", 18) == 0);
	CHECK(strstr(reg, "\nsystem: nominal ") != NULL);
	struct mstat m;
	module_status("talker", &m);
	char line[128];
	tfmt(line, sizeof line, "\ntalker %d ", m.pid);
	const char *l = strstr(reg, line);
	CHECK(l != NULL);
	if (l) {
		char name[32], state[32];
		int pid;
		unsigned long long start, mh, wh;
		unsigned fails;
		double delay;
		CHECK_EQ_INT(sscanf(l + 1, "%31s %d %llu %31s %u %lf %llx %llx", name, &pid, &start, state, &fails, &delay,
				    &mh, &wh), 8);
		CHECK(start > 0 && strcmp(state, "ready") == 0 && fails == 0 && delay == 0);
		CHECK(mh != 0 && wh != 0);
	}
	struct stat st;
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
	// The Neutral directory is empty: no module here has a Neutral command.
	tfmt(path, sizeof path, "%s/run/hangar/neutral/talker", T_root);
	CHECK(access(path, F_OK) < 0);
	// The pages are 0640 and the control socket 0660.
	tfmt(path, sizeof path, "%s/run/hangar/modules/talker", T_root);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640 && st.st_size == 4096);
	CHECK(stat(T_control, &st) == 0 && (st.st_mode & 0777) == 0660);
	// Topic files 0640, wake files and .dir 0660 (docs/01 Permissions).
	tfmt(path, sizeof path, "%s/test.depth", T_topics);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
	tfmt(path, sizeof path, "%s/.test.depth.wake", T_topics);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0660);
	tfmt(path, sizeof path, "%s/.dir", T_topics);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0660);
}

static void test_open_refusals(void)
{
	// Undeclared: a talker whose code opens a topic its manifest does not list.
	manifest("rogue", "[Module]\nExec=%s/talker --topic other.topic --rate 10\n[Publish rogue.out]\nType=depth\nRate=10\n"
			  "[Restart]\nPolicy=never\n", T_toys);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_state("rogue", "stopped", 5000));
	struct mstat m;
	CHECK(module_status("rogue", &m) && strcmp(m.last_exit, "exit:4") == 0);
	char log[1024];
	tfmt(log, sizeof log, "%s/run/hangar/log/rogue.log", T_root);
	CHECK(wait_file_contains(log, "hg_pub_open(other.topic): Permission denied", 2000));
	// The library refuses it from the wiring before asking; a module that
	// lies about its type is refused by the manager: the manifest says
	// wrench, the program publishes depth.
	manifest("liar", "[Module]\nExec=%s/talker --topic liar.out --rate 10\n[Publish liar.out]\nType=wrench\nRate=10\n"
			 "[Restart]\nPolicy=never\n", T_toys);
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	CHECK(wait_state("liar", "stopped", 5000));
	tfmt(log, sizeof log, "%s/run/hangar/log/liar.log", T_root);
	CHECK(wait_file_contains(log, "hg_pub_open(liar.out): Protocol wrong type", 2000));
	// A second writer: another live process holds the topic. The test
	// stands in for it by writing its own pid into the header of the
	// talker's file (same user, so it can), then has the talker restart:
	// the manager refuses the open, naming the holder.
	char path[1024];
	tfmt(path, sizeof path, "%s/test.depth", T_topics);
	int fd = open(path, O_RDWR | O_CLOEXEC);
	CHECK(fd >= 0);
	uint32_t me = (uint32_t)getpid(), zero = 0;
	CHECK(pwrite(fd, &me, 4, H_WRITER_PID) == 4);
	CHECK(pwrite(fd, &zero, 4, H_FLAGS) == 4);
	close(fd);
	// The talker is stopped first (its file then carries bit 1, which we
	// clear), so that the next start meets a "live" holder.
	CHECK(strncmp(ctl("stop talker"), "ok", 2) == 0);
	CHECK(wait_state("talker", "stopped", 5000));
	fd = open(path, O_RDWR | O_CLOEXEC);
	CHECK(pwrite(fd, &me, 4, H_WRITER_PID) == 4);
	CHECK(pwrite(fd, &zero, 4, H_FLAGS) == 4);
	close(fd);
	CHECK(strncmp(ctl("start talker"), "ok", 2) == 0);
	CHECK(wait_file_contains(T_log, "test.depth already has a writer", 6000));
	tfmt(log, sizeof log, "%s/run/hangar/log/talker.log", T_root);
	CHECK(wait_file_contains(log, "hg_pub_open(test.depth): Device or resource busy", 3000));
	// Give the file back: the talker's next attempt succeeds.
	fd = open(path, O_RDWR | O_CLOEXEC);
	uint32_t died = HG_FLAG_DIED;
	CHECK(pwrite(fd, &died, 4, H_FLAGS) == 4);
	close(fd);
	CHECK(strncmp(ctl("restart talker"), "ok", 2) == 0);
	CHECK(wait_state("talker", "ready", 8000));
	manifest_remove("rogue");
	manifest_remove("liar");
	CHECK(strncmp(ctl("reload"), "ok", 2) == 0);
	sleep_ms(200);
	CHECK(!module_status("rogue", &m));
}

static void test_cli(void)
{
	static char out[65536];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "status", (char *)NULL), 0);
	CHECK(strstr(out, "system   nominal") != NULL);
	CHECK(strstr(out, "talker") && strstr(out, "ready") && strstr(out, "publishing"));
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "topics", (char *)NULL), 0);
	CHECK(strstr(out, "test.depth") && strstr(out, "f397a321cd6643a1") && strstr(out, "50 Hz") && strstr(out, "talker"));
	CHECK(strstr(out, "hangar.vehicle.system") && strstr(out, "hangar.vehicle.modules") &&
	      strstr(out, "hangar.vehicle.admin.req"));
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "echo", "test.depth", "--count", "3", (char *)NULL), 0);
	CHECK(strstr(out, "--- seq ") && strstr(out, "depth_m") && strstr(out, "# m, positive down") &&
	      strstr(out, "pressure_mbar"));
	int seqs = 0;
	for (const char *p = out; (p = strstr(p, "--- seq ")); p += 8)
		seqs++;
	CHECK_EQ_INT(seqs, 3);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "echo", "test.depth", "--json", "--count", "1", "--fields", "depth_m",
			     (char *)NULL), 0);
	CHECK(strstr(out, "{\"seq\":") && strstr(out, "\"depth_m\":") && !strstr(out, "temp_c"));
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "hz", "test.depth", "--seconds", "1", (char *)NULL), 0);
	CHECK(strstr(out, "test.depth: ") && strstr(out, " Hz") && strstr(out, "jitter"));
	double hz = 0;
	const char *p = strstr(out, "test.depth: ");
	if (p)
		sscanf(p + 12, "%lf", &hz);
	CHECK(hz > 40 && hz < 60);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "show", "listener", (char *)NULL), 0);
	CHECK(strstr(out, "hangar-wiring 1\nmodule listener\n") && strstr(out, "ready      yes") &&
	      strstr(out, "status     ") && strstr(out, "messages"));
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "nosuch", (char *)NULL), 2);
	// hangar run: the module in the foreground, wired from its manifest,
	// in a private topic directory under $XDG_RUNTIME_DIR.
	pid_t pid = fork();
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		dup2(nul, 1);
		dup2(nul, 2);
		unsetenv("HANGAR_TOPIC_DIR");
		execl(T_hangar, T_hangar, "run", "talker", (char *)NULL);
		_exit(127);
	}
	char priv[1024];
	tfmt(priv, sizeof priv, "%s/xdg/hangar-run/talker/test.depth", T_root);
	int appeared = 0;
	for (int i = 0; i < 100 && !appeared; i++) {
		sleep_ms(20);
		appeared = access(priv, F_OK) == 0;
	}
	CHECK(appeared);
	uint64_t s1 = 0, s2 = 0;
	unsigned char h[4096];
	char path[1024];
	tfmt(path, sizeof path, "%s/xdg/hangar-run/talker", T_root);
	setenv("HANGAR_TOPIC_DIR", path, 1);
	if (topic_header("test.depth", h) == 0)
		s1 = h64(h, H_WRITE_SEQ);
	sleep_ms(200);
	if (topic_header("test.depth", h) == 0)
		s2 = h64(h, H_WRITE_SEQ);
	setenv("HANGAR_TOPIC_DIR", T_topics, 1);
	CHECK(s2 > s1);
	kill(pid, SIGTERM);
	CHECK_EQ_INT(wait_pid_ms(pid, 3000), 0);
	tfmt(path, sizeof path, "%s/xdg/hangar-run/talker/.wiring", T_root);
	CHECK(file_contains(path, "pub test.depth type=depth hash=f397a321cd6643a1 slots=16 rate=50"));
}

static void test_shutdown(void)
{
	int rc = manager_stop(mgr);
	CHECK_EQ_INT(rc, 0);
	CHECK(file_contains(T_log, "talker: stopped: pid"));
	CHECK(file_contains(T_log, "hangard: stopped"));
	char path[1024];
	tfmt(path, sizeof path, "%s/run/hangar/registry", T_root);
	CHECK(access(path, F_OK) < 0); // removed at shutdown
	CHECK(access(T_control, F_OK) < 0);
	struct mstat m;
	(void)m;
	// The modules are gone.
	tfmt(path, sizeof path, "%s/run/hangar/log/talker.log", T_root);
	CHECK(file_contains(path, "talker: SIGTERM after"));
}

int main(void)
{
	hd_setup("run", NULL);
	RUN_TEST(test_start_and_ready);
	RUN_TEST(test_control_requests);
	RUN_TEST(test_registry);
	RUN_TEST(test_open_refusals);
	RUN_TEST(test_cli);
	RUN_TEST(test_shutdown);
	if (checks_failed)
		printf("--- hangard.log ---\n%s", read_file(T_log));
	hd_teardown();
	TEST_MAIN_END();
}
