// SPDX-License-Identifier: GPL-3.0-or-later
/* mod_frame: a frame file (lib/sim/sim_frame.h) read for a module. The
 * thrusters module and the simulator both take `--frame FILE` and both come
 * here, so the two read one file one way. */
#ifndef MOD_FRAME_H
#define MOD_FRAME_H

#include "sim_frame.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* `path` NULL: the compiled-in BlueROV2 heavy configuration. 0, or -1 with
 * the reason in err. */
static inline int mod_frame_load(const char *path, struct sim_frame *f, char *err, size_t errlen)
{
	if (!path) {
		sim_frame_bluerov2_heavy(f);
		return 0;
	}
	FILE *fp = fopen(path, "r");
	if (!fp) {
		snprintf(err, errlen, "%s", strerror(errno));
		return -1;
	}
	char text[8192];
	size_t n = fread(text, 1, sizeof text, fp);
	int more = n == sizeof text && fgetc(fp) != EOF;
	fclose(fp);
	if (more) {
		snprintf(err, errlen, "larger than %zu bytes", sizeof text);
		return -1;
	}
	return sim_frame_parse(text, n, f, err, errlen);
}

#endif
