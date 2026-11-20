// SPDX-License-Identifier: GPL-3.0-or-later
/* The hangar command (docs/12-tools.md) run as a program: echo and hz on
 * topics published by children of this suite, call and param against a
 * stand-in manager that answers what the test tells it to, and param under
 * the real manager with the toy modules. Each test here is the regression
 * test of one review finding of 2026-11-20 (the D numbers of its comment). */
#include "hd_test.h"

#include <sys/resource.h>
#include <sys/time.h>

#include "hg_params.h"
#include "hg_type.h"

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

int main(void)
{
	hd_setup("cli", NULL);
	mkdir_p(T_topics);
	RUN_TEST(test_hz_large_payload);
	hd_teardown();
	TEST_MAIN_END();
}
