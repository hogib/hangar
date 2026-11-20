// SPDX-License-Identifier: GPL-3.0-or-later
/* Manifests (hangard/manifest.c): mf_parse_text on each part of the input, the
 * parsed modules as one set through mf_set_check (types resolved against the
 * repository's msg/, the bridge's configuration from the vehicle profile), and
 * wiring_text for every accepted module, which lib/hg's parser must take back
 * (the manager hands exactly that text to the module it starts).
 *
 * Input: up to four manifests separated by a NUL byte. A part whose first line
 * is `#name <module>` is parsed under that name (safety, bridge and policy_*
 * have rules of their own); otherwise it is m0..m3. A first byte of 't' before
 * everything makes the set a topside's. */
#include "fuzz.h"
#include "hg_wiring.h"
#include "manifest.h"
#include "wiring.h"

#ifndef FUZZ_REPO
#define FUZZ_REPO "."
#endif

static struct mf_set set;
static struct hg_wiring w;
static char wtext[1 << 18];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *role = "vehicle";
	if (size && data[0] == 't') {
		role = "topside";
		data++;
		size--;
	}
	mf_set_init(&set, role, "", FUZZ_REPO "/msg");
	snprintf(set.bridge_conf_path, sizeof set.bridge_conf_path, "%s", FUZZ_REPO "/profiles/rov-vehicle/bridge.conf");
	for (int part = 0; part < 4 && size; part++) {
		const uint8_t *sep = memchr(data, 0, size);
		size_t len = sep ? (size_t)(sep - data) : size;
		char *text = fz_text(data, len);
		char name[MF_NAME], path[64], err[MF_ERR];
		snprintf(name, sizeof name, "m%d", part);
		if (strncmp(text, "#name ", 6) == 0) {
			char cand[MF_NAME];
			size_t k = strcspn(text + 6, "\n");
			if (k < sizeof cand) {
				memcpy(cand, text + 6, k);
				cand[k] = 0;
				if (mf_valid_module_name(cand))
					snprintf(name, sizeof name, "%s", cand);
			}
		}
		snprintf(path, sizeof path, "/etc/hangar/modules/%.20s.module", name);
		struct mf_module *m = calloc(1, sizeof *m);
		if (!m)
			abort();
		if (mf_parse_text(text, len, name, path, m, err, sizeof err) < 0) {
			FZ_CHECK(strnlen(err, sizeof err) < sizeof err);
			FZ_NOTE("%s: parse refused: %s", name, err);
			free(m);
		} else if (mf_set_add(&set, m) < 0) {
			free(m);
		}
		free(text);
		data += len + (sep ? 1 : 0);
		size -= len + (sep ? 1 : 0);
	}
	int accepted = mf_set_check(&set);
	FZ_CHECK(accepted >= 0 && accepted <= set.n_modules);
	for (int i = 0; i < set.n_modules; i++) {
		const struct mf_module *m = set.modules[i];
		FZ_CHECK(strnlen(m->reason, sizeof m->reason) < sizeof m->reason);
		FZ_NOTE("%s: %s %s", m->name, m->refused ? "refused:" : "accepted", m->reason);
		if (m->refused)
			continue;
		int n = wiring_text(&set, m, wtext, sizeof wtext);
		if (n < 0)
			continue;
		char err[200] = "";
		if (hg_wiring_parse(wtext, (size_t)n, &w, err, sizeof err) < 0) {
			fprintf(stderr, "the manager's own wiring is refused by lib/hg: %s\n%s\n", err, wtext);
			abort();
		}
		FZ_CHECK(strcmp(w.module, m->name) == 0);
	}
	mf_set_free(&set);
	return 0;
}
