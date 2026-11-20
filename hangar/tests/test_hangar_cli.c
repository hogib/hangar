// SPDX-License-Identifier: GPL-3.0-or-later
/* The hangar command (docs/12-tools.md) run as a program: echo and hz on
 * topics published by children of this suite, call and param against a
 * stand-in manager that answers what the test tells it to, and param under
 * the real manager with the toy modules. Each test here is the regression
 * test of one review finding of 2026-11-20 (the D numbers of its comment). */
#include "hd_test.h"

#include <math.h>
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

/* hz divided the sum of a window's intervals by messages - 1, but from the
 * second window on the interval to the window's first message is in the sum
 * too (the last stamp carries over): a 2 Hz topic showed a mean interval of
 * 1000 ms, a 4 Hz one 333 ms. The intervals are counted now. And the first
 * window counted the message already on the topic when hz started. */
static void test_hz_mean_interval(void)
{
	pid_t pub = publisher("slow.t", "u32 n\n", 2000, 20, 500); /* 2 Hz for 10 s */
	sleep_ms(700);
	char *argv[] = { (char *)T_hangar, "hz", "slow.t", "--seconds", "4", NULL };
	int rc = run_hangar(out_path(), 10000, NULL, argv);
	reap(pub);
	const char *o = read_file(out_path());
	printf("%s", o);
	CHECK_EQ_INT(rc, 0);
	int with_mean = 0;
	for (const char *l = o; (l = strstr(l, "interval mean ")); l += 14, with_mean++) {
		double ms = atof(l + 14);
		CHECK(ms > 400 && ms < 700); /* 500 ms; 1000 was the fault */
	}
	CHECK(with_mean >= 3);
	/* The message that was there when hz started is not one of the first
	 * second's: 2 Hz, not 3 messages in it. */
	double first = strncmp(o, "slow.t: ", 8) == 0 ? atof(o + 8) : -1;
	CHECK(first > 0.5 && first < 2.6); /* 3 messages in the 1.1 s of a window were 2.73 */
}

/* --count, --seconds and --timeout went through atol: "--count x" was 0,
 * and echo exited 0 at once having shown nothing; "--seconds 2s" was 2; a
 * --timeout past 2^63 ns overflowed the deadline. Each takes a whole number
 * in its range now and says so otherwise. */
static void test_numeric_options(void)
{
	static const struct {
		const char *argv[6];
		const char *says;
	} bad[] = {
		{ { "echo", "none.t", "--count", "x" }, "--count" },
		{ { "echo", "none.t", "--count", "0" }, "--count" },
		{ { "echo", "none.t", "--count", "3x" }, "--count" },
		{ { "echo", "none.t", "--count", "-1" }, "--count" },
		{ { "echo", "none.t", "--count", "" }, "--count" },
		{ { "hz", "none.t", "--seconds", "2s" }, "--seconds" },
		{ { "hz", "none.t", "--seconds", "0" }, "--seconds" },
		{ { "hz", "none.t", "--seconds", "99999999999999999999" }, "--seconds" },
		{ { "call", "srv.arm", "--timeout", "abc" }, "--timeout" },
		{ { "call", "srv.arm", "--timeout", "0" }, "--timeout" },
		{ { "call", "srv.arm", "--timeout", "9223372036854775807" }, "--timeout" },
		{ { "call", "srv.arm", "--timeout", "86400001" }, "--timeout" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		char *argv[8] = { (char *)T_hangar };
		int n = 1;
		for (int k = 0; k < 6 && bad[i].argv[k]; k++)
			argv[n++] = (char *)bad[i].argv[k];
		argv[n] = NULL;
		int rc = run_hangar(out_path(), 3000, NULL, argv);
		const char *o = read_file(out_path());
		char want[64];
		tfmt(want, sizeof want, "%s takes a whole number", bad[i].says);
		if (rc != 1 || !strstr(o, want))
			printf("    hangar %s %s %s '%s': exit %d, says: %s%s", bad[i].argv[0], bad[i].argv[1], bad[i].argv[2],
			       bad[i].argv[3], rc, o, *o ? "" : "\n");
		CHECK_EQ_INT(rc, 1);
		CHECK(strstr(o, want) != NULL);
	}
	/* In range: taken (a day is the longest timeout; nobody answers here). */
	char *ok[] = { (char *)T_hangar, "call", "srv.arm", "--timeout", "86400000", NULL };
	CHECK_EQ_INT(run_hangar(out_path(), 3000, NULL, ok), 1);
	CHECK(file_contains(out_path(), "cannot reach the manager"));
}

/* ---- JSON, as RFC 8259 has it ---------------------------------------------------
 * A strict reader for what --json prints: 1 if [*p, end) starts with one
 * JSON value, *p then after it. Strings: no raw control character, only the
 * escapes of the grammar, valid UTF-8. Numbers: the grammar's, so no nan,
 * inf or leading +. */
static int js_value(const char **p, const char *end, int depth);

static void js_ws(const char **p, const char *end)
{
	while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r'))
		(*p)++;
}

static int js_string(const char **p, const char *end)
{
	const unsigned char *s = (const unsigned char *)*p, *e = (const unsigned char *)end;
	if (s >= e || *s != '"')
		return 0;
	for (s++; s < e && *s != '"';) {
		if (*s < 0x20)
			return 0;
		if (*s == '\\') {
			if (s + 1 >= e)
				return 0;
			if (s[1] == 'u') {
				if (s + 6 > e)
					return 0;
				for (int i = 2; i < 6; i++)
					if (!strchr("0123456789abcdefABCDEF", s[i]))
						return 0;
				s += 6;
			} else if (strchr("\"\\/bfnrt", s[1])) {
				s += 2;
			} else {
				return 0;
			}
		} else if (*s < 0x80) {
			s++;
		} else {
			int n = (*s & 0xe0) == 0xc0 ? 1 : (*s & 0xf0) == 0xe0 ? 2 : (*s & 0xf8) == 0xf0 ? 3 : -1;
			if (n < 0 || s + n >= e || *s == 0xc0 || *s == 0xc1)
				return 0;
			for (int i = 1; i <= n; i++)
				if ((s[i] & 0xc0) != 0x80)
					return 0;
			s += n + 1;
		}
	}
	if (s >= e)
		return 0;
	*p = (const char *)s + 1;
	return 1;
}

static int js_number(const char **p, const char *end)
{
	const char *s = *p;
	if (s < end && *s == '-')
		s++;
	if (s >= end || *s < '0' || *s > '9')
		return 0;
	if (*s == '0')
		s++;
	else
		while (s < end && *s >= '0' && *s <= '9')
			s++;
	if (s < end && *s == '.') {
		const char *d = ++s;
		while (s < end && *s >= '0' && *s <= '9')
			s++;
		if (s == d)
			return 0;
	}
	if (s < end && (*s == 'e' || *s == 'E')) {
		s++;
		if (s < end && (*s == '+' || *s == '-'))
			s++;
		const char *d = s;
		while (s < end && *s >= '0' && *s <= '9')
			s++;
		if (s == d)
			return 0;
	}
	*p = s;
	return 1;
}

static int js_value(const char **p, const char *end, int depth)
{
	js_ws(p, end);
	if (*p >= end || depth > 8)
		return 0;
	char c = **p;
	if (c == '{' || c == '[') {
		char close = c == '{' ? '}' : ']';
		(*p)++;
		js_ws(p, end);
		if (*p < end && **p == close) {
			(*p)++;
			return 1;
		}
		for (;;) {
			js_ws(p, end);
			if (c == '{') {
				if (!js_string(p, end))
					return 0;
				js_ws(p, end);
				if (*p >= end || *(*p)++ != ':')
					return 0;
			}
			if (!js_value(p, end, depth + 1))
				return 0;
			js_ws(p, end);
			if (*p >= end)
				return 0;
			char sep = *(*p)++;
			if (sep == close)
				return 1;
			if (sep != ',')
				return 0;
		}
	}
	if (c == '"')
		return js_string(p, end);
	static const char *const words[] = { "true", "false", "null" };
	for (int i = 0; i < 3; i++) {
		size_t n = strlen(words[i]);
		if ((size_t)(end - *p) >= n && strncmp(*p, words[i], n) == 0) {
			*p += n;
			return 1;
		}
	}
	return js_number(p, end);
}

/* Every line of text is one JSON value and nothing else; the number of
 * lines, or -1 at the first line that is not. */
static int json_lines(const char *text)
{
	int lines = 0;
	for (const char *l = text; *l;) {
		const char *e = strchr(l, '\n');
		if (!e)
			e = l + strlen(l);
		const char *p = l;
		if (!js_value(&p, e, 0) || (js_ws(&p, e), p != e))
			return -1;
		lines++;
		l = *e ? e + 1 : e;
	}
	return lines;
}

/* --json printed a char field as it is and a float with %g: a quote, a
 * backslash or a control character in the text, and nan or inf, made the
 * line something no JSON reader takes. Text is escaped now (a byte that is
 * not UTF-8 becomes U+FFFD), and a float that is not finite is null. */
static void test_json_is_json(void)
{
	/* The reader itself, on what it must refuse. */
	CHECK_EQ_INT(json_lines("{\"a\":1,\"b\":[1,2.5e-3,-0],\"c\":\"x\\n\\u00e9\",\"d\":null}\n"), 1);
	CHECK_EQ_INT(json_lines("{\"a\":nan}\n"), -1);
	CHECK_EQ_INT(json_lines("{\"a\":\"x\"y\"}\n"), -1);
	CHECK_EQ_INT(json_lines("{\"a\":\"x\ty\"}\n"), -1);
	CHECK_EQ_INT(json_lines("{\"a\":\"\xff\"}\n"), -1);
	pid_t pub = fork();
	if (pub == 0) {
		static struct hg_schema sc;
		const char text[] = "char name[24]\nchar raw[4]\nf32 f[4]\nf64 d\nu8 n\n";
		struct {
			char name[24], raw[4];
			float f[4];
			double d;
			uint8_t n;
		} v;
		memset(&v, 0, sizeof v);
		if (hg_schema_parse(text, sizeof text - 1, &sc, NULL, 0) < 0 || sc.type.payload_bytes != sizeof v)
			_exit(3);
		memcpy(v.name, "say \"hi\"\\ \n\ttab \x01 end", 22);
		memcpy(v.raw, "\xff\xc3\xa9z", 4); /* a stray byte, then e-acute, no NUL */
		v.f[0] = NAN;
		v.f[1] = INFINITY;
		v.f[2] = -INFINITY;
		v.f[3] = 1.5f;
		v.d = NAN;
		v.n = 7;
		struct hg_pub_opts o = { .slots = 4 };
		hg_pub *p = hg_pub_open("json.t", &sc.type, &o);
		for (int i = 0; p && i < 40; i++) {
			hg_publish(p, &v, sizeof v, hg_now_ns());
			sleep_ms(100);
		}
		_exit(0);
	}
	sleep_ms(400);
	char *argv[] = { (char *)T_hangar, "echo", "json.t", "--json", "--rate", "--count", "2", NULL };
	int rc = run_hangar(out_path(), 8000, NULL, argv);
	reap(pub);
	const char *o = read_file(out_path());
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(json_lines(o), 2);
	CHECK(strstr(o, "\"name\":\"say \\\"hi\\\"\\\\ \\n\\ttab \\u0001 end\"") != NULL);
	CHECK(strstr(o, "\"raw\":\"\\ufffd\xc3\xa9z\"") != NULL);
	CHECK(strstr(o, "\"f\":[null,null,null,1.5]") != NULL);
	CHECK(strstr(o, "\"d\":null,\"n\":7}") != NULL);
	/* The plain form is for reading and stays as it was. */
	char out[4096];
	CHECK_EQ_INT(run_cli(out, sizeof out, 3000, "echo", "json.t", "--count", "1", (char *)NULL), 0);
	CHECK(strstr(cli_field(out, "f"), "[nan inf -inf 1.5]") != NULL);
}

/* `hangar echo` parsed the header's schema again only when its length
 * changed. A topic replaced by a writer with another type of the same
 * length (a renamed field, two fields swapped, a frame of another size with
 * as many digits) was decoded with the old layout and names. */
static void test_echo_follows_a_replaced_schema(void)
{
	pid_t pub = fork();
	if (pub == 0) {
		static struct hg_schema a, b;
		const char ta[] = "u32 left\nu32 right\n", tb[] = "u32 right\nu32 left\n";
		_Static_assert(sizeof ta == sizeof tb, "the two texts are of one length");
		if (hg_schema_parse(ta, sizeof ta - 1, &a, NULL, 0) < 0 || hg_schema_parse(tb, sizeof tb - 1, &b, NULL, 0) < 0)
			_exit(3);
		struct hg_pub_opts o = { .slots = 4 };
		uint32_t v[2] = { 11, 22 };
		hg_pub *p = hg_pub_open("swap.t", &a.type, &o);
		if (!p)
			_exit(4);
		hg_publish(p, v, sizeof v, hg_now_ns());
		sleep_ms(1500);
		hg_pub_close(p);
		/* The same name, the other type: the file is replaced. */
		p = hg_pub_open("swap.t", &b.type, &o);
		if (!p)
			_exit(5);
		v[0] = 33; /* right */
		v[1] = 44; /* left */
		for (int i = 0; i < 20; i++) {
			hg_publish(p, v, sizeof v, hg_now_ns());
			sleep_ms(100);
		}
		_exit(0);
	}
	sleep_ms(500);
	char *argv[] = { (char *)T_hangar, "echo", "swap.t", "--json", "--count", "2", NULL };
	int rc = run_hangar(out_path(), 8000, NULL, argv);
	reap(pub);
	const char *o = read_file(out_path());
	printf("    echo across the replacement:\n%s", o);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(o, "\"left\":11,\"right\":22") != NULL);
	CHECK(strstr(o, "\"right\":33,\"left\":44") != NULL);
	CHECK(strstr(o, "\"left\":33") == NULL);
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
			   "[Call paramer.set_params]\n[Subscribe paramer.params]\n"
			   /* the hangar commands this suite runs inherit its wiring */
			   "[Subscribe big.params]\n[Subscribe paramer.rep]\n[Subscribe big.rep]\n");
	tfmt(params_file, sizeof params_file, "%s/var/lib/hangar/paramer/params", T_root);
	/* big: a parameter type whose text, with the topic's own lines before
	 * it, passes the 3584 bytes of a topic header's schema. */
	static char big[HG_SCHEMA_MAX], msg[600];
	size_t bn = (size_t)tfmt(big, sizeof big, "# msg/bigparams.msg: written by hangar's suite\n"
						 "f32  kp = 1.5 [0, 10]   # N/m\n"
						 "u32  n = 7 [1, 100]\n"
						 "char label[8] = \"abc\"\n"
						 "f32  free_gain = 2.5    # no range\n");
	while (bn < 3450) {
		memset(big + bn, '#', 68); /* comment lines */
		big[bn + 1] = ' ';
		big[bn + 68] = '\n';
		bn += 69;
	}
	big[bn] = 0;
	tfmt(msg, sizeof msg, "%s/bigparams.msg", T_msg);
	CHECK_EQ_INT(write_file(msg, big), 0);
	const char *toys = getenv("HANGAR_TOYS");
	CHECK(toys != NULL);
	/* never: a module with parameters that has not run and has no saved file. */
	manifest("never", "[Module]\nExec=/nonexistent/never\n[Restart]\nPolicy=never\n[Params]\nType=depth_hold_params\n");
	manifest("big", "[Module]\nExec=%s/bigparams\n[Params]\nType=bigparams\n[Health]\nWatchdog=2s\n", toys ? toys : "");
	mgr = manager_start();
	CHECK(wait_state("big", "ready", 5000));
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

/* A float without a range took NaN and inf: from a request's bytes, which
 * no parser sees, and from `hangar param set` and a saved file, which
 * strtod reads "nan" for. gains[3] of depth_hold_params has no range. */
static void test_non_finite_parameters_refused(void)
{
	struct params_msg m, after;
	CHECK_EQ_INT(read_params(&m), 0);
	struct set_req req = { .base_version = m.version, .p = m.p };
	req.p.gains[1] = NAN;
	hg_call_result r = set_params(&req);
	printf("    gains[1] = NaN in a request: %s (%s)\n", hg_status_name(r.status), r.reason);
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "gains[1]") != NULL);
	req.p = m.p;
	req.p.gains[2] = INFINITY;
	CHECK_EQ_INT(set_params(&req).status, HG_E_REFUSED);
	char out[4096];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "gains=nan", (char *)NULL), 1);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "gains=1,inf,2", (char *)NULL), 1);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "gains=1e39", (char *)NULL), 1);
	CHECK_EQ_INT(read_params(&after), 0);
	CHECK_EQ_INT(after.version, m.version);
	CHECK(memcmp(&after.p, &m.p, sizeof m.p) == 0);
	CHECK(!file_contains(params_file, "nan") && !file_contains(params_file, "inf"));
	/* A saved file with one stops the start. */
	CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "stopped", 5000));
	char saved[2048];
	tfmt(saved, sizeof saved, "%s", read_file(params_file));
	write_file(params_file, "# hangar parameters: depth_hold_params\ngains = 1, nan, 1\n");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "failed", 5000));
	CHECK(file_contains(T_log, "saved parameters refused: line 2: gains[1]: nan is not a finite number"));
	/* ... and the manager's param-write does not put one there. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "gains=inf", (char *)NULL), 1);
	write_file(params_file, saved);
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "ready", 5000));
	for (int i = 0; i < 100 && (read_params(&after) < 0 || after.version != m.version); i++)
		sleep_ms(20);
}

/* A parameter file of `total` bytes: the first line (may be ""), comment
 * lines, and "kp = 1.25" placed so that byte `cut` falls after "kp = 1". */
static void long_params_file(const char *path, const char *first, size_t cut)
{
	static char file[20000];
	size_t n = (size_t)tfmt(file, sizeof file, "%s", first);
	size_t at = cut - strlen("kp = 1");
	while (n + 64 <= at) {
		memset(file + n, '#', 63);
		file[n + 63] = '\n';
		n += 64;
	}
	if (at > n) {
		memset(file + n, '#', at - n - 1);
		file[at - 1] = '\n';
		n = at;
	}
	n += (size_t)tfmt(file + n, sizeof file - n, "kp = 1.25\n");
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, file, n) != (ssize_t)n)
		printf("    cannot write %s\n", path);
	if (fd >= 0)
		close(fd);
}

/* D7: `hangar param load` and the module's loading at start took one
 * read() of 14335 bytes as the whole file. A longer file was cut there, and
 * a line cut inside a number still parses: the file said kp = 1.25 and the
 * module ran with kp = 1. Both read to the end now and refuse a file that
 * does not fit. */
static void test_long_parameter_file_refused(void)
{
	size_t cut = HG_SCHEMA_MAX * 4 - 1;
	char path[600], out[4096];
	tfmt(path, sizeof path, "%s/long.params", T_root);
	long_params_file(path, "", cut);
	struct params_msg m;
	CHECK_EQ_INT(read_params(&m), 0);
	float kp0 = m.p.kp;
	uint64_t v0 = m.version;
	int rc = run_cli(out, sizeof out, 5000, "param", "load", "paramer", path, (char *)NULL);
	sleep_ms(200);
	CHECK_EQ_INT(read_params(&m), 0);
	printf("    param load of a file 4 bytes too long, ending in kp = 1.25: exit %d, kp in force %g\n", rc, (double)m.p.kp);
	CHECK_EQ_INT(rc, 1);
	CHECK(m.p.kp == kp0 && m.version == v0);
	/* One that fits is applied as written. */
	long_params_file(path, "", cut - 4);
	rc = run_cli(out, sizeof out, 5000, "param", "load", "paramer", path, (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	sleep_ms(200);
	CHECK_EQ_INT(read_params(&m), 0);
	CHECK(m.p.kp == 1.25f && m.version == v0 + 1);
	/* The module's side: its saved file, at start. */
	CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "stopped", 5000));
	long_params_file(params_file, "# hangar parameters: depth_hold_params\n", cut);
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	int failed = wait_state("paramer", "failed", 5000);
	CHECK(failed);
	CHECK(file_contains(T_log, "saved parameters refused: the file is longer than 14335 bytes"));
	if (!failed) {
		CHECK_EQ_INT(read_params(&m), 0);
		printf("    paramer started from a saved file 4 bytes too long: kp %g\n", (double)m.p.kp);
		CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
		CHECK(wait_state("paramer", "stopped", 5000));
	}
	write_file(params_file, "# hangar parameters: depth_hold_params\n# version 9, set by admin:x, now\nkp = 1.5\n");
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "ready", 5000));
	for (int i = 0; i < 100 && (read_params(&m) < 0 || m.version != 9); i++)
		sleep_ms(20);
	CHECK_EQ_INT(m.version, 9);
}

/* `hangar param load` parses its file against the set_params request type,
 * whose first field is base_version: a line `base_version = N` in the file
 * was taken, over the version just read from the topic. `param set` already
 * refused it; a file is refused the same way now. */
static void test_load_refuses_base_version(void)
{
	struct params_msg m, after;
	CHECK_EQ_INT(read_params(&m), 0);
	char path[600], text[200], out[4096];
	tfmt(path, sizeof path, "%s/base.params", T_root);
	tfmt(text, sizeof text, "# hangar parameters: depth_hold_params\nbase_version = %llu\nki = 0.7\n",
	     (unsigned long long)m.version);
	write_file(path, text);
	char *load[] = { (char *)T_hangar, "param", "load", "paramer", path, NULL };
	int rc = run_hangar(out_path(), 5000, NULL, load);
	sleep_ms(200);
	CHECK_EQ_INT(read_params(&after), 0);
	printf("    a file with base_version = %llu, the version in force: exit %d, ki %g, says: %s",
	       (unsigned long long)m.version, rc, (double)after.p.ki, read_file(out_path()));
	CHECK_EQ_INT(rc, 1);
	CHECK(file_contains(out_path(), "base_version is not a parameter"));
	CHECK(after.version == m.version && after.p.ki == m.p.ki);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "base_version=1", "ki=0.7", (char *)NULL), 1);
	/* The same file without the line loads. */
	write_file(path, "# hangar parameters: depth_hold_params\nki = 0.7\n");
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "load", "paramer", path, (char *)NULL), 0);
	sleep_ms(200);
	CHECK_EQ_INT(read_params(&after), 0);
	CHECK(after.version == m.version + 1 && after.p.ki == 0.7f);
}

/* D10: `hangar param reset` took the defaults from the schema in the header
 * of <module>.params whenever the module ran. For a type whose text does
 * not fit there beside the topic's own lines the header carries the
 * canonical lines, without attributes, and reset sent 0 for every field:
 * zeros, not the defaults. The attributes come from the manager's
 * param-read now, which has the type's text whole. */
static void test_reset_takes_the_types_defaults(void)
{
	char out[4096];
	/* The premise: this topic's header has no attributes. */
	unsigned char h[4096];
	CHECK_EQ_INT(topic_header("big.params", h), 0);
	CHECK(memmem(h + 512, 3584, "f32 kp\n", 7) != NULL);
	CHECK(memmem(h + 512, 3584, "1.5", 3) == NULL);
	char *set[] = { (char *)T_hangar, "param", "set", "big", "kp=3", "n=50", "label=xyz", "free_gain=9", NULL };
	int rc = run_hangar(out_path(), 5000, NULL, set);
	if (rc)
		printf("    param set big: exit %d: %s\n", rc, read_file(out_path()));
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "big", (char *)NULL), 0);
	CHECK(strcmp(cli_field(out, "kp"), "3") == 0);
	CHECK(strcmp(cli_field(out, "n"), "50") == 0);
	/* One field, then all of them. */
	rc = run_cli(out, sizeof out, 5000, "param", "reset", "big", "kp", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "big", (char *)NULL), 0);
	printf("    after `param reset big kp` (default 1.5): kp %s\n", cli_field(out, "kp"));
	CHECK(strcmp(cli_field(out, "kp"), "1.5") == 0);
	CHECK(strcmp(cli_field(out, "n"), "50") == 0);
	rc = run_cli(out, sizeof out, 5000, "param", "reset", "big", (char *)NULL);
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "big", (char *)NULL), 0);
	CHECK(strcmp(cli_field(out, "kp"), "1.5") == 0);
	CHECK(strcmp(cli_field(out, "n"), "7") == 0);
	CHECK(strcmp(cli_field(out, "label"), "\"abc\"") == 0);
	CHECK(strcmp(cli_field(out, "free_gain"), "2.5") == 0);
	/* A field the type does not have. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "reset", "big", "nosuch", (char *)NULL), 1);
	/* paramer's type fits its header: reset as before. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "ki=0.5", (char *)NULL), 0);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "reset", "paramer", "ki", (char *)NULL), 0);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "paramer", "ki", (char *)NULL), 0);
	CHECK(strcmp(cli_field(out, "ki"), "0.1") == 0);
}

/* The same for call and param: a reason is text from a module, and `param
 * get --json` on a module that is not running printed "# ..." lines and
 * the saved file as it is. Every form is one JSON object now. */
static void test_json_from_call_and_param(void)
{
	char out[4096];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "paramer", "--json", (char *)NULL), 0);
	CHECK_EQ_INT(json_lines(out), 1);
	CHECK(strstr(out, "{\"running\":true,\"version\":") && strstr(out, "\"params\":{\"kp\":"));
	/* A refusal's reason, and a reason with a quote in it. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "kp=11", "--json", (char *)NULL), 1);
	CHECK_EQ_INT(json_lines(out), 1);
	CHECK(strstr(out, "\"status\":\"HG_E_REFUSED\",\"reason\":\"kp 11 above 10\"") != NULL);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "paramer", "frame=a\\\"b", "--json", (char *)NULL), 0);
	CHECK_EQ_INT(json_lines(out), 1);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "paramer", "frame", "--json", (char *)NULL), 0);
	CHECK_EQ_INT(json_lines(out), 1);
	CHECK(strstr(out, "\"params\":{\"frame\":\"a\\\"b\"}}") != NULL);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "call", "paramer.nosuch", "--json", (char *)NULL), 1);
	/* Not running, with a saved file. */
	CHECK(strncmp(ctl("stop paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "stopped", 5000));
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "paramer", "--json", (char *)NULL), 0);
	if (json_lines(out) != 1)
		printf("    param get --json, not running: %s", out);
	CHECK_EQ_INT(json_lines(out), 1);
	CHECK(strstr(out, "{\"running\":false,\"saved\":true,\"version\":") != NULL);
	CHECK(strstr(out, "\"frame\":\"a\\\"b\"") != NULL);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "paramer", "kp", "--json", (char *)NULL), 0);
	CHECK_EQ_INT(json_lines(out), 1);
	/* The text form is the file, as before. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "paramer", (char *)NULL), 0);
	CHECK(strstr(out, "(not running); the saved file:\n# hangar parameters: depth_hold_params\n") != NULL);
	/* Never run: the defaults. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "never", "--json", (char *)NULL), 0);
	if (json_lines(out) != 1)
		printf("    param get --json, never run: %s", out);
	CHECK_EQ_INT(json_lines(out), 1);
	CHECK(strstr(out, "{\"running\":false,\"saved\":false,") && strstr(out, "\"kp\":1.5,"));
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "never", (char *)NULL), 0);
	CHECK(strstr(out, "it has not run yet; the defaults apply") && strcmp(cli_field(out, "kp"), "1.5") == 0);
	CHECK(strncmp(ctl("start paramer"), "ok", 2) == 0);
	CHECK(wait_state("paramer", "ready", 5000));
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
	RUN_TEST(test_hz_mean_interval);
	RUN_TEST(test_numeric_options);
	RUN_TEST(test_echo_follows_a_replaced_schema);
	RUN_TEST(test_json_is_json);
	RUN_TEST(test_malformed_reply_stops_the_command);
	RUN_TEST(test_manager_up);
	RUN_TEST(test_char_field_without_nul_refused);
	RUN_TEST(test_non_finite_parameters_refused);
	RUN_TEST(test_long_parameter_file_refused);
	RUN_TEST(test_load_refuses_base_version);
	RUN_TEST(test_reset_takes_the_types_defaults);
	RUN_TEST(test_json_from_call_and_param);
	RUN_TEST(test_manager_down);
	hd_teardown();
	TEST_MAIN_END();
}
