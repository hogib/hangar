// SPDX-License-Identifier: GPL-3.0-or-later
/* Parameter files (lib/hg/hg_params.c): the saved file under
 * /var/lib/hangar/params/ that a module reads at start and the manager reads
 * and rewrites for `hangar params` (docs/05), and the attribute grammar of a
 * parameter type's schema (= default, [min, max], @restart).
 *
 * Input: `<schema text> NUL <file text>`. The schema is parsed as a type, its
 * attributes and defaults read, then the file parsed over the defaults, every
 * field range-checked, and the values written back as a file, which must
 * parse to the same bytes. A schema whose payload is over 64 KB is skipped
 * (the buffers here, not a limit of the library). */
#include "fuzz.h"
#include "hg_params.h"
#include "hg_type.h"

#define PAYLOAD_CAP (64u << 10)

static struct hg_schema sch;
static struct hg_param_attr attrs[HG_SCHEMA_MAX_FIELDS];
static unsigned char values[PAYLOAD_CAP], back[PAYLOAD_CAP], present[HG_SCHEMA_MAX_FIELDS];
static char out[1 << 20];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const uint8_t *sep = memchr(data, 0, size);
	size_t slen = sep ? (size_t)(sep - data) : size;
	char *schema = fz_text(data, slen);
	char *file = fz_text(sep ? sep + 1 : data + size, sep ? size - slen - 1 : 0);
	size_t flen = sep ? size - slen - 1 : 0;
	char err[256], reason[HG_PARAMS_REASON];

	if (hg_schema_parse(schema, slen, &sch, err, sizeof err) < 0 || sch.type.payload_bytes > PAYLOAD_CAP) {
		FZ_NOTE("schema refused: %s", err);
		goto done;
	}
	const struct hg_type *t = &sch.type;
	if (hg_params_attrs(t, attrs, err, sizeof err) < 0) {
		FZ_NOTE("attributes refused: %s", err);
		goto done;
	}
	memset(values, 0, t->payload_bytes);
	if (hg_params_defaults(t, attrs, values, err, sizeof err) < 0) {
		FZ_NOTE("defaults refused: %s", err);
		goto done;
	}
	struct hg_params_header hdr;
	memset(&hdr, 0, sizeof hdr);
	if (hg_params_file_parse(t, file, flen, values, &hdr, present, 0, err, sizeof err) < 0) {
		FZ_CHECK(strnlen(err, sizeof err) < sizeof err);
		FZ_NOTE("file refused: %s", err);
		goto done;
	}
	FZ_NOTE("file parsed: version %llu", (unsigned long long)hdr.version);
	FZ_CHECK(strnlen(hdr.changed_by, sizeof hdr.changed_by) < sizeof hdr.changed_by);
	for (uint32_t i = 0; i < t->n_fields; i++) {
		char one[256];
		(void)hg_param_check_range(&t->fields[i], &attrs[i], values, reason, sizeof reason);
		(void)hg_param_format(&t->fields[i], values, one, sizeof one);
		FZ_CHECK(strnlen(one, sizeof one) < sizeof one);
	}
	/* What the formatter writes, the parser reads back to the same values. */
	int n = hg_params_file_format(t, values, &hdr, out, sizeof out);
	FZ_CHECK(n >= 0);
	if ((size_t)n < sizeof out) {
		struct hg_params_header hdr2;
		memset(back, 0, t->payload_bytes);
		if (hg_params_defaults(t, attrs, back, err, sizeof err) < 0)
			abort();
		if (hg_params_file_parse(t, out, (size_t)n, back, &hdr2, NULL, 0, err, sizeof err) < 0) {
			fprintf(stderr, "the formatter's own file is refused: %s\n%s\n", err, out);
			abort();
		}
		FZ_CHECK(memcmp(values, back, t->payload_bytes) == 0);
	}
done:
	free(schema);
	free(file);
	return 0;
}
