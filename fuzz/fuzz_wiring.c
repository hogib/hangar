// SPDX-License-Identifier: GPL-3.0-or-later
/* hg_wiring_parse (lib/hg/hg_wiring.c): the wiring text a module reads from the
 * descriptor the manager hands it. The manager writes it, so in a running
 * system it is trusted; a module started by hand (HANGAR_WIRING) reads whatever
 * file it is pointed at. Also the value grammar the same file exports:
 * hg_parse_duration, hg_parse_rate_mhz, hg_glob_match, on the input's lines. */
#include "fuzz.h"
#include "hg_wiring.h"

static struct hg_wiring w;

#define TERMINATED(a) FZ_CHECK(strnlen((a), sizeof(a)) < sizeof(a))

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *text = fz_text(data, size);
	char err[200];
	if (hg_wiring_parse(text, size, &w, err, sizeof err) == 0) {
		FZ_CHECK(w.n_subs >= 0 && w.n_subs <= HG_WIRING_MAX_SUBS);
		FZ_CHECK(w.n_globs >= 0 && w.n_globs <= HG_WIRING_MAX_GLOBS);
		FZ_CHECK(w.n_pubs >= 0 && w.n_pubs <= HG_WIRING_MAX_PUBS);
		FZ_CHECK(w.n_serves >= 0 && w.n_serves <= HG_WIRING_MAX_SERVES);
		FZ_CHECK(w.n_calls >= 0 && w.n_calls <= HG_WIRING_MAX_CALLS);
		FZ_CHECK(w.n_mirrors >= 0 && w.n_mirrors <= HG_WIRING_MAX_MIRRORS);
		TERMINATED(w.module);
		TERMINATED(w.role);
		TERMINATED(w.actuator.command);
		for (int i = 0; i < w.n_subs; i++) {
			TERMINATED(w.subs[i].topic);
			TERMINATED(w.subs[i].type);
			FZ_CHECK(hg_wiring_find_sub(&w, w.subs[i].topic) != NULL);
			(void)hg_wiring_glob_matches(&w, w.subs[i].topic);
			(void)hg_wiring_match_mirror(&w, w.subs[i].topic);
		}
		for (int i = 0; i < w.n_globs; i++) {
			TERMINATED(w.globs[i].pattern);
			FZ_CHECK(hg_wiring_find_glob(&w, w.globs[i].pattern) != NULL);
		}
		for (int i = 0; i < w.n_pubs; i++) {
			const struct hg_wiring_pub *p = &w.pubs[i];
			TERMINATED(p->topic);
			TERMINATED(p->type);
			FZ_CHECK(p->n_inputs >= 0 && p->n_inputs <= 4);
			FZ_CHECK(p->n_sources >= 0 && p->n_sources <= HG_WIRING_MAX_SOURCES);
			for (int k = 0; k < p->n_inputs; k++)
				TERMINATED(p->inputs[k]);
			for (int k = 0; k < p->n_sources; k++)
				TERMINATED(p->sources[k]);
			FZ_CHECK(hg_wiring_find_pub(&w, p->topic) != NULL);
		}
		for (int i = 0; i < w.n_serves; i++) {
			TERMINATED(w.serves[i].method);
			TERMINATED(w.serves[i].m.req_type);
			TERMINATED(w.serves[i].m.rep_type);
			FZ_CHECK(w.serves[i].n_allow >= 0 && w.serves[i].n_allow <= HG_WIRING_MAX_ALLOW);
			for (int k = 0; k < w.serves[i].n_allow; k++)
				TERMINATED(w.serves[i].allow[k]);
		}
		for (int i = 0; i < w.n_calls; i++) {
			TERMINATED(w.calls[i].target);
			TERMINATED(w.calls[i].m.req_type);
			TERMINATED(w.calls[i].m.rep_type);
		}
		for (int i = 0; i < w.n_mirrors; i++)
			TERMINATED(w.mirrors[i].pattern);
		FZ_NOTE("wiring parsed: %d subs %d pubs %d serves %d calls", w.n_subs, w.n_pubs, w.n_serves, w.n_calls);
	} else {
		FZ_NOTE("wiring refused: %s", err);
	}
	/* The value grammar, on the first two lines of the same input. */
	char *nl = strchr(text, '\n');
	if (nl) {
		*nl = 0;
		char *second = nl + 1, *nl2 = strchr(second, '\n');
		if (nl2)
			*nl2 = 0;
		(void)hg_glob_match(text, second);
		(void)hg_glob_match(second, text);
	}
	int64_t d = hg_parse_duration(text), r = hg_parse_rate_mhz(text);
	FZ_CHECK(d >= -1);
	FZ_CHECK(r >= -1 && r <= 0xffffffffll);
	free(text);
	return 0;
}
