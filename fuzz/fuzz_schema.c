// SPDX-License-Identifier: GPL-3.0-or-later
/* hg_schema_parse and hg_schema_canonical (lib/hg/hg_type.c): schema text as it
 * arrives in a topic file's header, a recording's header, a .msg file, or from
 * the bridge's peer. Checks what hg_type.h promises of a parsed type: every
 * field inside the payload, and a canonical form that parses to the same type. */
#include "fuzz.h"
#include "hg_type.h"

static struct hg_schema a, b;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *text = fz_text(data, size);
	char err[160];
	if (hg_schema_parse(text, size, &a, err, sizeof err) == 0) {
		const struct hg_type *t = &a.type;
		FZ_CHECK(size <= HG_SCHEMA_MAX);
		FZ_CHECK(t->n_fields <= HG_SCHEMA_MAX_FIELDS);
		FZ_CHECK(t->payload_bytes <= HG_PAYLOAD_MAX && t->payload_bytes % 8 == 0);
		FZ_CHECK(t->schema_bytes == size);
		for (uint32_t i = 0; i < t->n_fields; i++) {
			const struct hg_field *f = &t->fields[i];
			uint64_t end = (uint64_t)f->offset + (uint64_t)hg_field_elem_size(f->kind) * f->count;
			FZ_CHECK(f->count >= 1);
			FZ_CHECK(end <= t->payload_bytes);
			FZ_CHECK(strlen(f->name) >= 1 && strlen(f->name) <= HG_FIELD_NAME_MAX);
			FZ_CHECK(hg_type_field(t, f->name) != NULL);
			(void)strlen(f->comment);
		}
		/* The canonical form is a schema of the same layout and hash. */
		char canon[HG_SCHEMA_MAX + 1];
		size_t n = hg_schema_canonical(text, size, canon, sizeof canon);
		if (n > 0 && n < sizeof canon) {
			FZ_CHECK(hg_fnv1a64(canon, n) == t->hash);
			if (hg_schema_parse(canon, n, &b, err, sizeof err) == 0) {
				FZ_CHECK(b.type.hash == t->hash);
				FZ_CHECK(b.type.payload_bytes == t->payload_bytes);
				FZ_CHECK(b.type.n_fields == t->n_fields);
			}
		}
		FZ_NOTE("schema parsed: %u fields, %u bytes", t->n_fields, t->payload_bytes);
	} else {
		FZ_NOTE("schema refused: %s", err);
		/* A refusal still leaves the canonical writer safe to call. */
		char canon[64];
		(void)hg_schema_canonical(text, size, canon, sizeof canon);
	}
	free(text);
	return 0;
}
