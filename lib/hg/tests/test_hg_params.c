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

int main(void)
{
	RUN_TEST(test_long_value_in_the_file);
	RUN_TEST(test_file_read_whole_or_refused);
	TEST_MAIN_END();
}
