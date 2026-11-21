// SPDX-License-Identifier: LGPL-3.0-or-later
/* hg_schema_hash <file.msg>...: what lib/hg's run-time parser (hg_schema_parse,
 * the one that reads a topic's or a recording's header) makes of each file,
 * one line "<name> <hash-hex> <payload_bytes>", the format of
 * `msggen --print-hash` and `python3 -m hangar.schema --print-hash`, so that
 * `make check-msggen` can compare the three line by line: the generator
 * writes the hash into a type's descriptor and so into every topic header,
 * and this parser must find the same one in the header's text, or every
 * reader that checks (hgr_schema, hangar dataset build) refuses the file.
 * Exits 1, with the reason, on a file the parser refuses. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "hg_type.h"

int main(int argc, char **argv)
{
	static struct hg_schema s;
	static char text[HG_SCHEMA_MAX + 2];
	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f) {
			perror(argv[i]);
			return 1;
		}
		size_t n = fread(text, 1, sizeof text, f);
		fclose(f);
		char err[128] = "";
		if (hg_schema_parse(text, n, &s, err, sizeof err) < 0) {
			fprintf(stderr, "%s: %s\n", argv[i], err);
			return 1;
		}
		const char *stem = strrchr(argv[i], '/');
		stem = stem ? stem + 1 : argv[i];
		const char *dot = strrchr(stem, '.');
		printf("%.*s %016" PRIx64 " %u\n", (int)(dot ? dot - stem : (long)strlen(stem)), stem, s.type.hash,
		       s.type.payload_bytes);
	}
	return 0;
}
