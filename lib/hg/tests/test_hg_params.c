// SPDX-License-Identifier: LGPL-3.0-or-later
// The parameter grammar of hg_params.h without a module: values to text and
// back, the saved file written and parsed. (set_params and the loading at
// start need a manager: hangard's params suite and hangar's own.)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hg_params.h"
#include "test_harness.h"

static struct hg_schema S;

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
	char dir[] = "/tmp/hg-params-XXXXXX", path[64], fifo[64];
	if (!mkdtemp(dir)) {
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

int main(void)
{
	RUN_TEST(test_floats_are_finite);
	RUN_TEST(test_long_value_in_the_file);
	RUN_TEST(test_file_read_whole_or_refused);
	TEST_MAIN_END();
}
