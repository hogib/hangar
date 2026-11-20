// SPDX-License-Identifier: LGPL-3.0-or-later
// The parameter grammar of hg_params.h without a module: values to text and
// back, the saved file written and parsed. (set_params and the loading at
// start need a manager: hangard's params suite and hangar's own.)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
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

int main(void)
{
	RUN_TEST(test_long_value_in_the_file);
	TEST_MAIN_END();
}
