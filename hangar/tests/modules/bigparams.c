// SPDX-License-Identifier: GPL-3.0-or-later
/* bigparams: a module whose parameter type is read at run time from
 * $HANGAR_ROOT/usr/share/hangar/msg/bigparams.msg, the file the manager
 * reads for [Params] Type=bigparams. The suite writes that file long enough
 * that <module>.params cannot carry the type's text with its attributes
 * and carries the canonical lines instead (lib/hg/NOTES.md deviation 19).
 * Reports "params v<version>" on its status line. */
#include "toy.h"

#include "hg_call.h"
#include "hg_params.h"

int main(void)
{
	toy_init("bigparams");
	static struct hg_schema schema;
	static char text[HG_SCHEMA_MAX + 1], path[600], err[160];
	static unsigned char P[4096];
	const char *root = getenv("HANGAR_ROOT");
	snprintf(path, sizeof path, "%s/usr/share/hangar/msg/bigparams.msg", root ? root : "");
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(text, 1, HG_SCHEMA_MAX, f) : 0;
	if (f)
		fclose(f);
	if (!n || hg_schema_parse(text, n, &schema, err, sizeof err) < 0 || schema.type.payload_bytes > sizeof P) {
		fprintf(stderr, "bigparams: %s: %s\n", path, n ? err : "cannot read");
		return 4;
	}
	schema.type.name = "bigparams";
	if (hg_params_open(&schema.type, P, NULL, NULL) < 0) {
		fprintf(stderr, "bigparams: hg_params_open: %s\n", strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT];
	int ns = hg_call_subs(subs, HG_MAX_WAIT);
	if (ns < 0)
		return 4;
	hg_ready();
	while (!toy_stop) {
		char line[64];
		snprintf(line, sizeof line, "params v%llu", (unsigned long long)hg_params_version());
		hg_status(line);
		hg_alive();
		if (hg_wait(subs, ns, 100) & HG_READY_INTERRUPT)
			break;
		hg_serve();
	}
	return 0;
}
