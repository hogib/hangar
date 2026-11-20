// SPDX-License-Identifier: GPL-3.0-or-later
/* bridge.conf and the key file (modules/bridge/bridge_conf.c): bc_parse on the
 * input as text, then the lookups the bridge and the manager make on a parsed
 * configuration; and bc_load_key on the same bytes as a key file. Both files
 * are the operator's (root writes them), read by the bridge and by the root
 * manager. */
#include "fuzz.h"
#include "bridge_conf.h"

static struct bridge_conf c;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *text = fz_text(data, size);
	char err[200];
	if (bc_parse(text, size, &c, err, sizeof err) == 0) {
		FZ_CHECK(c.n_mirrors >= 0 && c.n_mirrors <= BC_MAX_MIRRORS);
		FZ_CHECK(c.n_remote >= 0 && c.n_remote <= BC_MAX_REMOTE);
		FZ_CHECK(strnlen(c.pair, sizeof c.pair) < sizeof c.pair);
		FZ_CHECK(strnlen(c.peer, sizeof c.peer) < sizeof c.peer);
		FZ_CHECK(strnlen(c.key_path, sizeof c.key_path) < sizeof c.key_path);
		static const char *const names[] = { "nav.depth", "cam.pilot", "safety.req", "safety.rep",
						     "hangar.vehicle.system", "a" };
		for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
			(void)bc_find(&c, names[i], 0);
			(void)bc_find(&c, names[i], 1);
		}
		(void)bc_remote_matches(&c, "safety", "arm");
		for (int i = 0; i < c.n_remote; i++)
			FZ_CHECK(strnlen(c.remote[i], sizeof c.remote[i]) < sizeof c.remote[i]);
		FZ_NOTE("parsed: %d mirrors, %d remote", c.n_mirrors, c.n_remote);
	} else {
		FZ_NOTE("refused: %s", err);
		FZ_CHECK(strnlen(err, sizeof err) < sizeof err);
	}
	(void)bc_is_glob(text);
	free(text);

	char path[64];
	unsigned char key[32];
	int fd = fz_file(data, size, path);
	(void)bc_load_key(path, key, err, sizeof err);
	(void)bc_load(path, &c, err, sizeof err);
	close(fd);
	return 0;
}
