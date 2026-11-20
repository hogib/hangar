// SPDX-License-Identifier: LGPL-3.0-or-later
// The parameter grammar of hg_params.h without a module: values to text and
// back, the saved file written and parsed. (set_params and the loading at
// start need a manager: hangard's params suite and hangar's own.)
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hg_params.h"
#include "test_harness.h"

static struct hg_schema S;

/* A directory of this test's own under the suites' base, on tmpfs. */
static char *scratch_dir(char *out, size_t len)
{
	const char *base = getenv("HANGAR_TEST_BASE");
	char b[200];
	if (base && *base)
		snprintf(b, sizeof b, "%s", base);
	else
		snprintf(b, sizeof b, "/dev/shm/hangar-test-%ld", (long)getuid());
	mkdir(b, 0755);
	snprintf(out, len, "%s/hg-params-XXXXXX", b);
	return mkdtemp(out);
}

static const struct hg_type *type_of(const char *text, const char *name)
{
	if (hg_schema_parse(text, strlen(text), &S, NULL, 0) < 0) {
		printf("    the schema does not parse: %s\n", text);
		exit(2);
	}
	S.type.name = name;
	return &S.type;
}

/* A field whose text is longer than a schema (3584 bytes) was cut at that
 * length inside hg_params_file_format, which still returned success: the
 * file was written and no longer loaded ("182 values for 220 elements").
 * The value goes straight into the file's buffer now; the only bound is
 * the file's, which the return value reports like snprintf. */
static void test_long_value_in_the_file(void)
{
	const struct hg_type *t = type_of("f64 table[220]\nu32 after = 7\n", "table_params");
	struct {
		double table[220];
		uint32_t after;
	} v, back;
	memset(&v, 0, sizeof v);
	for (int i = 0; i < 220; i++)
		v.table[i] = 0.123456789012345 + i;
	v.after = 7;
	struct hg_params_header hdr = { .version = 3 }, hdr2;
	snprintf(hdr.changed_by, sizeof hdr.changed_by, "saved");
	static char file[HG_SCHEMA_MAX * 4];
	int n = hg_params_file_format(t, (const unsigned char *)&v, &hdr, file, sizeof file);
	CHECK(n > HG_SCHEMA_MAX && n < (int)sizeof file);
	CHECK_EQ_INT((int)strlen(file), n);
	char err[200] = "";
	memset(&back, 0, sizeof back);
	int rc = hg_params_file_parse(t, file, (size_t)n, (unsigned char *)&back, &hdr2, NULL, 1, err, sizeof err);
	if (rc < 0)
		printf("    a file of %d bytes from the formatter: %s\n", n, err);
	CHECK_EQ_INT(rc, 0);
	CHECK(memcmp(&v, &back, sizeof v) == 0);
	CHECK_EQ_INT(hdr2.version, 3);
	/* One field alone, into a buffer too small: the needed length, and
	 * what fits NUL-terminated. */
	char small[64];
	int need = hg_param_format(&t->fields[0], (const unsigned char *)&v, small, sizeof small);
	CHECK(need > HG_SCHEMA_MAX);
	CHECK_EQ_INT(strlen(small), sizeof small - 1);
	/* A file that does not fit says so by its length, as before. */
	t = type_of("f64 table[1000]\n", "table_params");
	static double big[1000];
	for (int i = 0; i < 1000; i++)
		big[i] = 0.123456789012345 + i;
	n = hg_params_file_format(t, (const unsigned char *)big, &hdr, file, sizeof file);
	CHECK(n >= (int)sizeof file);
}

/* A saved file is read to its end, however the reads come back, and a file
 * longer than the buffer is refused: one read() took what it got as the
 * whole file, and a file cut at the buffer's size can still parse ("kp =
 * 1.25" cut after the 1). */
static void test_file_read_whole_or_refused(void)
{
	char dir[300], path[320], fifo[320];
	if (!scratch_dir(dir, sizeof dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	snprintf(path, sizeof path, "%s/params", dir);
	snprintf(fifo, sizeof fifo, "%s/fifo", dir);
	static char text[4096], buf[1024];
	memset(text, '#', sizeof text);
	size_t len = 77;
	/* The longest file that fits a buffer of 1024 is 1023 bytes. */
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(write(fd, text, 1023) == 1023);
	close(fd);
	CHECK_EQ_INT(hg_params_file_read(path, buf, sizeof buf, &len), 0);
	CHECK_EQ_INT(len, 1023);
	CHECK_EQ_INT(buf[1023], 0);
	CHECK_EQ_INT(strlen(buf), 1023);
	fd = open(path, O_WRONLY | O_APPEND);
	CHECK(write(fd, "1", 1) == 1);
	close(fd);
	errno = 0;
	CHECK_EQ_INT(hg_params_file_read(path, buf, sizeof buf, &len), -1);
	CHECK_EQ_INT(errno, EFBIG);
	CHECK_EQ_INT(truncate(path, 0), 0);
	CHECK_EQ_INT(hg_params_file_read(path, buf, sizeof buf, &len), 0);
	CHECK_EQ_INT(len, 0);
	CHECK_EQ_INT(buf[0], 0);
	unlink(path);
	errno = 0;
	CHECK_EQ_INT(hg_params_file_read(path, buf, sizeof buf, &len), -1);
	CHECK_EQ_INT(errno, ENOENT);
	/* Reads that come back short: a FIFO fed in two parts. */
	CHECK_EQ_INT(mkfifo(fifo, 0600), 0);
	pid_t pid = fork();
	if (pid == 0) {
		int w = open(fifo, O_WRONLY);
		if (write(w, "kp = 1", 6) != 6)
			_exit(1);
		usleep(150000);
		if (write(w, ".25\n", 4) != 4)
			_exit(1);
		_exit(0);
	}
	CHECK_EQ_INT(hg_params_file_read(fifo, buf, sizeof buf, &len), 0);
	CHECK_EQ_INT(len, 10);
	CHECK(strcmp(buf, "kp = 1.25\n") == 0);
	int st;
	waitpid(pid, &st, 0);
	unlink(fifo);
	rmdir(dir);
}

/* A parameter is a finite number. strtod reads "nan" and "inf", and 1e39
 * becomes inf as an f32 without ERANGE: a field with a range refused them
 * by comparison (only NaN, and inf below an infinite bound), a field
 * without one took them, from a file, a request or `hangar param set`. */
static void test_floats_are_finite(void)
{
	const struct hg_type *t = type_of("f32 gain = 1\nf64 wide\nf32 ranged = 1 [0, 10]\nf32 g[3]\n", "float_params");
	struct {
		float gain;
		double wide;
		float ranged;
		float g[3];
	} v;
	memset(&v, 0, sizeof v);
	v.gain = 2;
	char err[160];
	static const char *const bad[] = { "nan", "NaN", "-nan", "inf", "-inf", "+inf", "infinity", "1e39", "-1e39", "nan(1)" };
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		err[0] = 0;
		int rc = hg_param_parse(&t->fields[0], bad[i], strlen(bad[i]), (unsigned char *)&v, err, sizeof err);
		if (rc == 0)
			printf("    f32 gain: \"%s\" parsed as %g\n", bad[i], (double)v.gain);
		CHECK_EQ_INT(rc, -1);
		CHECK(strstr(err, "gain") != NULL);
		CHECK(v.gain == 2); /* nothing written on a refusal */
	}
	CHECK_EQ_INT(hg_param_parse(&t->fields[1], "nan", 3, (unsigned char *)&v, err, sizeof err), -1);
	CHECK_EQ_INT(hg_param_parse(&t->fields[1], "inf", 3, (unsigned char *)&v, err, sizeof err), -1);
	CHECK_EQ_INT(hg_param_parse(&t->fields[1], "1e309", 5, (unsigned char *)&v, err, sizeof err), -1);
	CHECK_EQ_INT(hg_param_parse(&t->fields[3], "1, inf, 2", 9, (unsigned char *)&v, err, sizeof err), -1);
	CHECK(strstr(err, "g[1]") != NULL);
	/* What a float holds still parses. */
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], "3.4e38", 6, (unsigned char *)&v, err, sizeof err), 0);
	CHECK(v.gain > 3.3e38f);
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], "-0.5", 4, (unsigned char *)&v, err, sizeof err), 0);
	CHECK(v.gain == -0.5f);
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], "1e-30", 5, (unsigned char *)&v, err, sizeof err), 0);
	CHECK_EQ_INT(hg_param_parse(&t->fields[1], "1e39", 4, (unsigned char *)&v, err, sizeof err), 0);
	CHECK(v.wide == 1e39);
	/* The bytes of a request: no range to fail, and still refused. */
	static struct hg_param_attr attrs[8];
	CHECK_EQ_INT(hg_params_attrs(t, attrs, err, sizeof err), 0);
	char reason[64];
	v.gain = 1;
	v.ranged = 1;
	for (uint32_t i = 0; i < t->n_fields; i++)
		CHECK_EQ_INT(hg_param_check_range(&t->fields[i], &attrs[i], (unsigned char *)&v, reason, sizeof reason), 0);
	v.gain = NAN;
	reason[0] = 0;
	CHECK_EQ_INT(hg_param_check_range(&t->fields[0], &attrs[0], (unsigned char *)&v, reason, sizeof reason), -1);
	CHECK(strstr(reason, "gain") && strstr(reason, "finite"));
	v.gain = INFINITY;
	CHECK_EQ_INT(hg_param_check_range(&t->fields[0], &attrs[0], (unsigned char *)&v, reason, sizeof reason), -1);
	v.gain = 1;
	v.wide = -INFINITY;
	CHECK_EQ_INT(hg_param_check_range(&t->fields[1], &attrs[1], (unsigned char *)&v, reason, sizeof reason), -1);
	v.wide = 0;
	v.g[2] = NAN;
	reason[0] = 0;
	CHECK_EQ_INT(hg_param_check_range(&t->fields[3], &attrs[3], (unsigned char *)&v, reason, sizeof reason), -1);
	CHECK(strstr(reason, "g[2]") != NULL);
	v.g[2] = 0;
	v.ranged = NAN; /* with a range: refused before, and still */
	CHECK_EQ_INT(hg_param_check_range(&t->fields[2], &attrs[2], (unsigned char *)&v, reason, sizeof reason), -1);
	/* A saved file with one. */
	struct hg_params_header hdr;
	const char file[] = "# hangar parameters: float_params\ngain = nan\n";
	err[0] = 0;
	CHECK_EQ_INT(hg_params_file_parse(t, file, sizeof file - 1, (unsigned char *)&v, &hdr, NULL, 1, err, sizeof err), -1);
	CHECK(strstr(err, "line 2") != NULL);
}

/* An array longer than 1792 elements, as the formatter writes it: every value
 * comes back. The parser kept the parts of a list in a table of 1792 and
 * refused its own formatter's file for anything longer (found by
 * fuzz/fuzz_params.c). The one-value-for-all form, and the counts that are
 * neither, are as before. */
static void test_long_list(void)
{
	const struct hg_type *t = type_of("u16 lut[2500]\n", "lut_params");
	static uint16_t v[2504], back[2504];
	static char text[1 << 16], err[200];
	for (int i = 0; i < 2500; i++)
		v[i] = (uint16_t)(i * 7);
	int n = hg_param_format(&t->fields[0], (const unsigned char *)v, text, sizeof text);
	CHECK(n > 2500 && n < (int)sizeof text);
	memset(back, 0xff, sizeof back);
	err[0] = 0;
	int rc = hg_param_parse(&t->fields[0], text, (size_t)n, (unsigned char *)back, err, sizeof err);
	if (rc < 0)
		printf("    %s\n", err);
	CHECK_EQ_INT(rc, 0);
	CHECK(memcmp(v, back, 2500 * sizeof v[0]) == 0);
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], " 9 ", 3, (unsigned char *)back, err, sizeof err), 0);
	CHECK(back[0] == 9 && back[1791] == 9 && back[1792] == 9 && back[2499] == 9);
	err[0] = 0;
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], "1, 2, 3", 7, (unsigned char *)back, err, sizeof err), -1);
	CHECK(strstr(err, "3 values for 2500 elements") != NULL);
	CHECK(back[0] == 9);
	text[n] = ',';
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], text, (size_t)n + 1, (unsigned char *)back, err, sizeof err), -1);
	CHECK(strstr(err, "2501 values for 2500 elements") != NULL);
	/* The largest array a field's values are parsed through is 32 KB: one
	 * that fits is a parameter type, a larger one is refused as a type
	 * (its values could be formatted into a file and not parsed back).
	 * Text has its own path and no such bound. */
	static struct hg_param_attr attrs[4];
	t = type_of("f64 k[4096]\n", "fits");
	CHECK_EQ_INT(hg_params_attrs(t, attrs, err, sizeof err), 0);
	t = type_of("f32 k[10656]\n", "too_large");
	err[0] = 0;
	CHECK_EQ_INT(hg_params_attrs(t, attrs, err, sizeof err), -1);
	CHECK(strstr(err, "a parameter array is at most 32768 bytes") != NULL);
	t = type_of("char note[40000]\n", "text");
	CHECK_EQ_INT(hg_params_attrs(t, attrs, err, sizeof err), 0);
}

/* close() for this suite's own process, lib/hg's objects included: when
 * told to, it fails the close of a parameter file's temporary (as a quota
 * or a network file system does at close: the descriptor is released all
 * the same), and exits 99 if that descriptor is then closed again. */
static int fail_tmp_close;

int close(int fd)
{
	static int failed_fd = -1;
	if (fd >= 0 && fd == failed_fd)
		_exit(99);
	if (fail_tmp_close) {
		char link[64], target[700];
		snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
		ssize_t n = readlink(link, target, sizeof target - 1);
		if (n > 0) {
			target[n] = 0;
			if (strstr(target, "/params.tmp.")) {
				syscall(SYS_close, fd);
				failed_fd = fd;
				errno = EDQUOT;
				return -1;
			}
		}
	}
	return (int)syscall(SYS_close, fd);
}

/* save_file, on a close that fails: `w != n || close(fd) < 0` and then
 * close(fd) again in the error path closed the descriptor twice (whatever
 * had been opened on that number in between, in a module with threads),
 * and reported EIO in place of the close's errno. The first start of a
 * module here, with nowhere to write: exit 78 with the reason, no file,
 * no temporary left, one close. */
static void test_save_closes_once(void)
{
	const struct hg_type *t = type_of("f32 kp = 1.5 [0, 10]\n", "p");
	static struct hg_schema req;
	const char reqtext[] = "u64 base_version\nf32 kp = 1.5 [0, 10]\n";
	CHECK_EQ_INT(hg_schema_parse(reqtext, sizeof reqtext - 1, &req, NULL, 0), 0);
	char dir[300], wiring[320], state[320], log[320], text[1024];
	if (!scratch_dir(dir, sizeof dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	snprintf(wiring, sizeof wiring, "%s/wiring", dir);
	snprintf(state, sizeof state, "%s/params", dir);
	snprintf(log, sizeof log, "%s/log", dir);
	snprintf(text, sizeof text,
		 "hangar-wiring 1\nmodule self\nrole vehicle\n"
		 "pub self.params type=params:self slots=16 implicit\n"
		 "serve set_params req=set_params:p hash=%016llx size=%u rep=set_params_rep hash=d748add7256bbe70 size=16 "
		 "deadline=100ms allow=*\n",
		 (unsigned long long)req.type.hash, req.type.payload_bytes);
	int fd = open(wiring, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
	close(fd);
	fflush(stdout); /* not twice, by a child that leaves through exit() */
	pid_t pid = fork();
	if (pid == 0) {
		setenv("HANGAR_WIRING_FILE", wiring, 1);
		setenv("HANGAR_TOPIC_DIR", dir, 1);
		setenv("HANGAR_STATE_DIR", dir, 1);
		unsetenv("HANGAR_CONTROL");
		unsetenv("HANGAR_WIRING");
		int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		dup2(lf, 2);
		static float P[2];
		fail_tmp_close = 1;
		int rc = hg_params_open(t, P, NULL, NULL);
		fprintf(stderr, "hg_params_open returned %d (%s)\n", rc, strerror(errno));
		_exit(rc == 0 ? 0 : 1);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	static char said[2048];
	fd = open(log, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, said, sizeof said - 1) : 0;
	if (fd >= 0)
		close(fd);
	said[n > 0 ? n : 0] = 0;
	int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	printf("    first start with a failing close: exit %d%s; it said: %s", code,
	       code == 99 ? " (the descriptor was closed twice)" : "", said[0] ? said : "\n");
	CHECK_EQ_INT(code, 78);
	CHECK(strstr(said, "cannot write") && strstr(said, strerror(EDQUOT)));
	CHECK(access(state, F_OK) < 0);
	/* Nothing left behind but the wiring, the log and what lib/hg keeps
	 * in a topic directory. */
	int temporaries = 0;
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' && (!e->d_name[1] || e->d_name[1] == '.'))
			continue;
		if (strstr(e->d_name, ".tmp."))
			temporaries++;
		char p[600];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		unlink(p);
	}
	if (d)
		closedir(d);
	CHECK_EQ_INT(temporaries, 0);
	rmdir(dir);
}

/* A module started in a child on a wiring written here, with `validate` and
 * a state directory `dir` (created by the caller): its exit status, and what
 * it wrote to stderr in said. */
static int start_module(const struct hg_type *t, const char *reqtext, hg_params_validate validate, const char *dir,
			char *said, size_t cap)
{
	static struct hg_schema req;
	if (hg_schema_parse(reqtext, strlen(reqtext), &req, NULL, 0) < 0)
		return -2;
	char wiring[320], log[320], text[1024];
	snprintf(wiring, sizeof wiring, "%s/wiring", dir);
	snprintf(log, sizeof log, "%s/log", dir);
	snprintf(text, sizeof text,
		 "hangar-wiring 1\nmodule self\nrole vehicle\n"
		 "pub self.params type=params:self slots=16 implicit\n"
		 "pub self.rep type=rep:self slots=16 implicit\n"
		 "serve set_params req=set_params:p hash=%016llx size=%u rep=set_params_rep hash=d748add7256bbe70 size=16 "
		 "deadline=100ms allow=*\n",
		 (unsigned long long)req.type.hash, req.type.payload_bytes);
	int fd = open(wiring, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
		return -2;
	close(fd);
	fflush(stdout); /* not twice, by a child that leaves through exit() */
	pid_t pid = fork();
	if (pid == 0) {
		setenv("HANGAR_WIRING_FILE", wiring, 1);
		setenv("HANGAR_TOPIC_DIR", dir, 1);
		setenv("HANGAR_STATE_DIR", dir, 1);
		unsetenv("HANGAR_CONTROL");
		unsetenv("HANGAR_WIRING");
		int lf = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		dup2(lf, 2);
		static float P[4];
		int rc = hg_params_open(t, P, validate, NULL);
		fprintf(stderr, "hg_params_open returned %d (%s), kp %g\n", rc, strerror(errno), (double)P[0]);
		_exit(rc == 0 ? 0 : 1);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	fd = open(log, O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, said, cap - 1) : 0;
	if (fd >= 0)
		close(fd);
	said[n > 0 ? n : 0] = 0;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void rm_dir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char p[600];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		unlink(p);
	}
	if (d)
		closedir(d);
	rmdir(dir);
}

static int wants_kp_of_two(const void *candidate, const void *current, char reason[64])
{
	(void)current;
	if (*(const float *)candidate < 2.0f) {
		snprintf(reason, 64, "kp under 2 makes the loop too slow");
		return -11; /* HG_E_REFUSED, whatever its number: anything but HG_OK refuses */
	}
	return 0;
}

/* The first start saved the type's defaults without the range check or the
 * module's validator, which every later start runs on the saved file. A
 * module whose validator refuses its own defaults (kp = 1.5 where it wants 2)
 * ran once on them, wrote them, and at its second start was refused its own
 * file: exit 78 the day after it worked. The defaults meet the validator at
 * the first start now: 78 at once, saying so, and nothing written. */
static void test_defaults_meet_the_validator(void)
{
	const char typetext[] = "f32 kp = 1.5 [0, 10]\n", reqtext[] = "u64 base_version\nf32 kp = 1.5 [0, 10]\n";
	const struct hg_type *t = type_of(typetext, "p");
	char dir[300], state[320], said[2048];
	if (!scratch_dir(dir, sizeof dir)) {
		CHECK(!"mkdtemp");
		return;
	}
	snprintf(state, sizeof state, "%s/params", dir);
	int code = start_module(t, reqtext, wants_kp_of_two, dir, said, sizeof said);
	printf("    first start, defaults the validator refuses: exit %d; it said: %s", code, said[0] ? said : "\n");
	CHECK_EQ_INT(code, 78);
	CHECK(strstr(said, "the defaults of p are refused: kp under 2") != NULL);
	CHECK(access(state, F_OK) < 0); /* nothing saved for a second start to trip over */
	/* The same at a second start, as before: nothing has changed its answer. */
	code = start_module(t, reqtext, wants_kp_of_two, dir, said, sizeof said);
	CHECK_EQ_INT(code, 78);
	/* With no validator, or one that takes them, the first start is as it was. */
	code = start_module(t, reqtext, NULL, dir, said, sizeof said);
	CHECK_EQ_INT(code, 0);
	CHECK(strstr(said, "first start: defaults written") != NULL && strstr(said, "kp 1.5") != NULL);
	CHECK(access(state, F_OK) == 0);
	rm_dir(dir);
}

/* A quoted text for a char field larger than a schema (3584 bytes) was
 * unescaped into a buffer of that size and cut there, before its length was
 * compared with the field's: 5000 characters for a char[8192] were taken,
 * success returned, and 3584 of them stored. */
static void test_long_quoted_text(void)
{
	const struct hg_type *t = type_of("char note[8192]\n", "p");
	static unsigned char P[8192];
	static char text[10000], err[200];
	size_t n = 5000;
	text[0] = '"';
	for (size_t i = 0; i < n; i++)
		text[1 + i] = (char)('a' + i % 26);
	text[1 + n] = '"';
	memset(P, 0x7f, sizeof P);
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], text, n + 2, P, err, sizeof err), 0);
	CHECK_EQ_INT(strnlen((const char *)P, sizeof P), n);
	CHECK(memcmp(P, text + 1, n) == 0 && P[n] == 0 && P[sizeof P - 1] == 0);
	/* Escapes count as the one byte they stand for, at any length. */
	memset(text, 0, sizeof text);
	text[0] = '"';
	size_t u = 1;
	for (int i = 0; i < 4000; i++) {
		text[u++] = '\\';
		text[u++] = i % 2 ? 'n' : '"';
	}
	text[u++] = '"';
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], text, u, P, err, sizeof err), 0);
	CHECK_EQ_INT(strnlen((const char *)P, sizeof P), 4000);
	CHECK(P[0] == '"' && P[1] == '\n' && P[3999] == '\n');
	/* One byte too many for the field is refused with both numbers, and the
	 * values in force are not touched. */
	n = 8192;
	text[0] = '"';
	memset(text + 1, 'z', n);
	text[1 + n] = '"';
	memset(P, 0x7f, sizeof P);
	err[0] = 0;
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], text, n + 2, P, err, sizeof err), -1);
	CHECK(strstr(err, "text of 8192 bytes; at most 8191") != NULL);
	if (!strstr(err, "8192"))
		printf("    it said: %s\n", err);
	CHECK(P[0] == 0x7f && P[5000] == 0x7f);
	/* A NUL byte inside the quotes is refused: the text would end there for
	 * every reader, with more behind it in the field's bytes. */
	static const char nul[] = "\"ab\0cd\"";
	err[0] = 0;
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], nul, sizeof nul - 1, P, err, sizeof err), -1);
	CHECK(strstr(err, "NUL") != NULL);
	CHECK(P[0] == 0x7f);
	n = 8191;
	text[1 + n] = '"';
	CHECK_EQ_INT(hg_param_parse(&t->fields[0], text, n + 2, P, err, sizeof err), 0);
	CHECK_EQ_INT(strnlen((const char *)P, sizeof P), 8191);
}

/* hg_schema_canonical parsed into one static struct: two threads calling
 * it at once (a module with a worker thread, the console beside the
 * library's helper) wrote each other's fields under the other's loop. Each
 * thread here asks for its own type's canonical text many times and counts
 * the answers that are not it; under ThreadSanitizer (make check-tsan) the
 * first pair of calls is enough. */
static const char canon_a[] = "u8 a\n", canon_b[] = "f32 x[3]\nu64 when\nchar name[16]\ni16 y\nf64 z[2]\nu32 n\n";

static void *canon_thread(void *arg)
{
	const char *want = arg;
	static _Thread_local char out[512];
	long wrong = 0;
	for (int i = 0; i < 20000; i++) {
		size_t n = hg_schema_canonical(want, strlen(want), out, sizeof out);
		if (n != strlen(want) || strcmp(out, want) != 0)
			wrong++;
	}
	return (void *)wrong;
}

static void test_canonical_from_two_threads(void)
{
	char out[512];
	CHECK_EQ_INT(hg_schema_canonical(canon_a, sizeof canon_a - 1, out, sizeof out), sizeof canon_a - 1);
	CHECK(strcmp(out, canon_a) == 0);
	CHECK_EQ_INT(hg_schema_canonical(canon_b, sizeof canon_b - 1, out, sizeof out), sizeof canon_b - 1);
	CHECK(strcmp(out, canon_b) == 0);
	pthread_t ta, tb;
	void *wa = NULL, *wb = NULL;
	CHECK_EQ_INT(pthread_create(&ta, NULL, canon_thread, (void *)canon_a), 0);
	CHECK_EQ_INT(pthread_create(&tb, NULL, canon_thread, (void *)canon_b), 0);
	pthread_join(ta, &wa);
	pthread_join(tb, &wb);
	CHECK_EQ_INT((long)wa, 0);
	CHECK_EQ_INT((long)wb, 0);
	if (wa || wb)
		printf("    %ld and %ld of 20000 canonical texts were not the caller's\n", (long)wa, (long)wb);
}

int main(void)
{
	RUN_TEST(test_long_quoted_text);
	RUN_TEST(test_long_list);
	RUN_TEST(test_canonical_from_two_threads);
	RUN_TEST(test_save_closes_once);
	RUN_TEST(test_defaults_meet_the_validator);
	RUN_TEST(test_floats_are_finite);
	RUN_TEST(test_long_value_in_the_file);
	RUN_TEST(test_file_read_whole_or_refused);
	TEST_MAIN_END();
}
