// SPDX-License-Identifier: GPL-3.0-or-later
/* The hangar command (docs/12-tools.md) run as a program: echo and hz on
 * topics published by children of this suite, call and param against a
 * stand-in manager that answers what the test tells it to, and param under
 * the real manager with the toy modules. Each test here is the regression
 * test of one review finding of 2026-11-20 (the D numbers of its comment). */
#include "hd_test.h"

#include <sys/resource.h>
#include <sys/time.h>

#include "depth_hold_params.h"
#include "hg_call.h"
#include "hg_params.h"
#include "hg_type.h"
#include "set_params_rep.h"

/* Runs hangar with stdout and stderr in a file; the exit status (128 + n
 * for signal n, -1 for a timeout, after SIGKILL) and the CPU seconds used. */
static int run_hangar(const char *outfile, long timeout_ms, double *cpu_s, char *const argv[])
{
	pid_t pid = fork();
	if (pid == 0) {
		int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fd, 1);
		dup2(fd, 2);
		struct rlimit rl = { 64u << 20, 64u << 20 }; /* a runaway print stays small */
		setrlimit(RLIMIT_FSIZE, &rl);
		execv(T_hangar, argv);
		_exit(127);
	}
	uint64_t t0 = now_ms();
	int st = 0, rc;
	struct rusage ru;
	memset(&ru, 0, sizeof ru);
	for (;;) {
		pid_t r = wait4(pid, &st, WNOHANG, &ru);
		if (r == pid) {
			rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
			break;
		}
		if (now_ms() - t0 > (uint64_t)timeout_ms) {
			kill(pid, SIGKILL);
			wait4(pid, &st, 0, &ru);
			rc = -1;
			break;
		}
		sleep_ms(10);
	}
	if (cpu_s)
		*cpu_s = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec +
			 (double)ru.ru_stime.tv_usec / 1e6;
	return rc;
}

static const char *out_path(void)
{
	static char p[600];
	tfmt(p, sizeof p, "%s/hangar.out", T_root);
	return p;
}

/* A child that publishes on a topic of its own, standalone, as a module
 * run by hand would: `count` messages of `text`'s type, one every
 * period_ms, the first four bytes of each set to its number. */
static pid_t publisher(const char *topic, const char *text, uint32_t rate_mhz, int count, long period_ms)
{
	pid_t pid = fork();
	if (pid != 0)
		return pid;
	static struct hg_schema sc;
	if (hg_schema_parse(text, strlen(text), &sc, NULL, 0) < 0)
		_exit(3);
	struct hg_pub_opts o = { .rate_mhz = rate_mhz, .slots = 4 };
	hg_pub *p = hg_pub_open(topic, &sc.type, &o);
	unsigned char *buf = calloc(1, sc.type.payload_bytes);
	if (!p || !buf)
		_exit(4);
	for (int i = 1; i <= count; i++) {
		memcpy(buf, &i, sizeof i);
		hg_publish(p, buf, sc.type.payload_bytes, hg_now_ns());
		sleep_ms(period_ms);
	}
	_exit(0);
}

static void reap(pid_t pid)
{
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

/* D3: `hangar hz` read into a 64 KB buffer and gave up on a larger payload
 * without reading: the unread message kept hg_wait returning at once, so
 * every frame topic showed 0 Hz at a full core. */
static void test_hz_large_payload(void)
{
	pid_t pub = publisher("big.t", "u8 data[100000]\n", 20000, 200, 50); /* 20 Hz for 10 s */
	sleep_ms(300);
	char *argv[] = { (char *)T_hangar, "hz", "big.t", "--seconds", "3", NULL };
	double cpu = 0;
	int rc = run_hangar(out_path(), 10000, &cpu, argv);
	reap(pub);
	const char *o = read_file(out_path());
	printf("    hz on a 100 kB payload at 20 Hz: exit %d, %.2f s of CPU, first line: %.*s\n", rc, cpu,
	       (int)strcspn(o, "\n"), o);
	CHECK_EQ_INT(rc, 0);
	CHECK(cpu < 1.0);
	int lines = 0;
	for (const char *l = o; (l = strstr(l, "big.t: ")); l += 7, lines++) {
		double hz = atof(l + 7);
		CHECK(hz > 14 && hz < 26); /* 20 Hz; wide for a loaded machine, and 0 was the fault */
	}
	CHECK_EQ_INT(lines, 3);
}

/* A stand-in manager on the control socket: every request's first line is
 * appended to <root>/requests, and answered from the table (the first entry
 * whose prefix the request starts with), or "err stand-in manager". */
struct answer {
	const char *prefix, *reply;
};

static const char *requests_path(void)
{
	static char p[600];
	tfmt(p, sizeof p, "%s/requests", T_root);
	return p;
}

static pid_t stand_in_manager(const struct answer *table)
{
	char dir[600];
	tfmt(dir, sizeof dir, "%s/run/hangar", T_root);
	mkdir_p(dir);
	unlink(requests_path());
	unlink(T_control);
	pid_t pid = fork();
	if (pid != 0) {
		for (int i = 0; i < 200 && access(T_control, F_OK) < 0; i++)
			sleep_ms(10);
		return pid;
	}
	int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	tfmt(sa.sun_path, sizeof sa.sun_path, "%s", T_control);
	if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(s, 8) < 0)
		_exit(1);
	for (;;) {
		int c = accept(s, NULL, NULL);
		if (c < 0)
			continue;
		static char req[HG_CONTROL_MAX + 1];
		ssize_t n = recv(c, req, HG_CONTROL_MAX, 0);
		if (n > 0) {
			req[n] = 0;
			FILE *f = fopen(requests_path(), "a");
			if (f) {
				fprintf(f, "%.*s\n", (int)strcspn(req, "\n"), req);
				fclose(f);
			}
			const char *rep = "err stand-in manager\n";
			for (const struct answer *a = table; a->prefix; a++)
				if (strncmp(req, a->prefix, strlen(a->prefix)) == 0) {
					rep = a->reply;
					break;
				}
			send(c, rep, strlen(rep), MSG_NOSIGNAL);
		}
		close(c);
	}
}

static void stand_in_stop(pid_t pid)
{
	reap(pid);
	unlink(T_control);
}

/* 1 if a request starting with prefix reached the stand-in manager. */
static int requested(const char *prefix)
{
	const char *r = read_file(requests_path());
	size_t n = strlen(prefix);
	for (const char *l = r; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
		if (strncmp(l, prefix, n) == 0)
			return 1;
	return 0;
}

/* D4: method_info and params_saved returned 1 for a reply they could not
 * parse and their callers tested < 0: after the error message the command
 * went on with a zeroed struct, sent a call with an empty body, and took
 * the module as not running and sent param-write. */
static void test_malformed_reply_stops_the_command(void)
{
	static const struct answer bad[] = { { "method ", "ok\nreq=x nonsense\n\n" },
					     { "param-read ", "ok\ntype=x nonsense\n\n" },
					     { NULL, NULL } };
	pid_t mgr = stand_in_manager(bad);
	char *call[] = { (char *)T_hangar, "call", "srv.arm", "--timeout", "300", NULL };
	int rc = run_hangar(out_path(), 5000, NULL, call);
	CHECK_EQ_INT(rc, 1);
	CHECK(file_contains(out_path(), "method: malformed reply line"));
	CHECK(requested("method srv.arm"));
	CHECK(!requested("call "));
	char pfile[600];
	tfmt(pfile, sizeof pfile, "%s/load.params", T_root);
	write_file(pfile, "kp = 2\n");
	char *load[] = { (char *)T_hangar, "param", "load", "srv", pfile, NULL };
	CHECK_EQ_INT(run_hangar(out_path(), 5000, NULL, load), 1);
	char *set[] = { (char *)T_hangar, "param", "set", "srv", "kp=2", NULL };
	CHECK_EQ_INT(run_hangar(out_path(), 5000, NULL, set), 1);
	CHECK(requested("method srv.set_params"));
	CHECK(!requested("param-write"));
	char *get[] = { (char *)T_hangar, "param", "get", "srv", NULL };
	CHECK_EQ_INT(run_hangar(out_path(), 5000, NULL, get), 1);
	CHECK(file_contains(out_path(), "param-read: malformed reply line"));
	CHECK(!file_contains(out_path(), "it has not run yet"));
	char *reset[] = { (char *)T_hangar, "param", "reset", "srv", NULL };
	CHECK_EQ_INT(run_hangar(out_path(), 5000, NULL, reset), 1);
	CHECK(!requested("param-write"));
	stand_in_stop(mgr);
}

/* ---- Under the manager --------------------------------------------------------
 * paramer (hangard's toy, depth_hold_params) and this process wired as the
 * module `tester`, which may call paramer.set_params: the requests
 * `hangar param` cannot build are made by hand here. From test_manager_up
 * on this process is a wired module: the tests that publish topics of their
 * own run before it. */

static pid_t mgr;
static struct hg_schema SP; /* set_params:depth_hold_params */
static char params_file[600];

struct set_req {
	uint64_t base_version;
	depth_hold_params p;
};
struct params_msg {
	uint64_t version;
	char changed_by[48];
	uint8_t restart_pending;
	uint8_t reserved[7];
	depth_hold_params p;
};
static hg_sub *params_sub;

/* The newest paramer.params message. */
static int read_params(struct params_msg *out)
{
	if (!params_sub)
		params_sub = hg_sub_open("paramer.params", NULL);
	struct hg_info info;
	static struct params_msg last;
	for (int i = 0; i < 100; i++) {
		enum hg_result r = hg_latest(params_sub, &last, sizeof last, &info);
		if (r == HG_MSG || (r == HG_NONE && last.version)) {
			*out = last;
			return 0;
		}
		sleep_ms(20);
	}
	return -1;
}

static hg_call_result set_params(const struct set_req *req)
{
	set_params_rep rep;
	memset(&rep, 0, sizeof rep);
	return hg_call("paramer.set_params", &SP.type, req, &SET_PARAMS_REP_TYPE, &rep, 1000);
}

static void test_manager_up(void)
{
	manifest("paramer", "[Module]\nExec=%s/paramer\n[Params]\nType=depth_hold_params\n[Health]\nWatchdog=2s\n", T_toys);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Call paramer.set_params]\n[Subscribe paramer.params]\n");
	tfmt(params_file, sizeof params_file, "%s/var/lib/hangar/paramer/params", T_root);
	mgr = manager_start();
	CHECK(wait_state("paramer", "ready", 5000));
	CHECK(wait_state("tester", "stopped", 5000));
	char path[600];
	if (wiring_to_file("tester", path, sizeof path) < 0) {
		printf("    no wiring for tester\n");
		exit(2);
	}
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", "tester", 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		printf("    hg_init: %s\n", hg_init_error());
		exit(2);
	}
	static char text[HG_SCHEMA_MAX];
	int n = tfmt(text, sizeof text, "u64 base_version\n%.*s", (int)DEPTH_HOLD_PARAMS_TYPE.schema_bytes,
		     DEPTH_HOLD_PARAMS_TYPE.schema);
	CHECK_EQ_INT(hg_schema_parse(text, (size_t)n, &SP, NULL, 0), 0);
	SP.type.name = "set_params:depth_hold_params";
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK_EQ_INT(m.version, 1);
}

/* D5: set_params took a char field filled to its last byte, with no NUL.
 * It was saved as 16 characters, and the saved file is then refused at the
 * next start ("text of 16 bytes; at most 15"): exit 78, failed until
 * someone edits the file. `hangar param set` cannot build such a request;
 * any caller the Allow list admits can. */
static void test_char_field_without_nul_refused(void)
{
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	struct set_req req = { .base_version = m.version, .p = m.p };
	memset(req.p.frame, 'X', sizeof req.p.frame);
	hg_call_result r = set_params(&req);
	printf("    frame = 16 x 'X', no NUL: %s (%s)\n", hg_status_name(r.status), r.reason);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "frame") != NULL);
	CHECK(!file_contains(params_file, "XXXX"));
	struct params_msg after;
	CHECK_EQ_INT(read_params(&after), 0);
	CHECK_EQ_INT(after.version, m.version);
	/* 15 characters and the NUL are the most the field holds. */
	memset(req.p.frame, 'Y', sizeof req.p.frame - 1);
	req.p.frame[sizeof req.p.frame - 1] = 0;
	r = set_params(&req);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(file_contains(params_file, "frame = \"YYYYYYYYYYYYYYY\"\n"));
	/* And the module starts again from what it saved. */
	struct mstat st;
	CHECK(module_status("paramer", &st));
	CHECK(strncmp(ctl("restart paramer"), "ok", 2) == 0);
	CHECK(wait_new_pid("paramer", st.pid, "ready", 5000));
}

static void test_manager_down(void)
{
	if (params_sub)
		hg_sub_close(params_sub);
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(void)
{
	hd_setup("cli", NULL);
	mkdir_p(T_topics);
	RUN_TEST(test_hz_large_payload);
	RUN_TEST(test_malformed_reply_stops_the_command);
	RUN_TEST(test_manager_up);
	RUN_TEST(test_char_field_without_nul_refused);
	RUN_TEST(test_manager_down);
	hd_teardown();
	TEST_MAIN_END();
}
