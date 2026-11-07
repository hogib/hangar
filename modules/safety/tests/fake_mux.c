// SPDX-License-Identifier: GPL-3.0-or-later
/* fake_mux: a stand-in for the mux in the safety suite, enabled under the
 * name `mux`: it has the mux's parameters (mux.params, the depth band the
 * `depth` trigger reads) and nothing else, and is ready at once. */
#include "mod.h"

#include "hg_call.h"
#include "hg_params.h"
#include "mux_params.h"

static mux_params P;

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	mod_init("fake_mux");
	if (hg_params_open(&MUX_PARAMS_TYPE, &P, NULL, NULL) < 0) {
		mod_log("fake_mux", "hg_params_open: %s", strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT];
	int n = hg_call_subs(subs, HG_MAX_WAIT);
	if (n < 0)
		return 4;
	hg_ready();
	hg_status("fake mux: parameters only");
	while (!mod_stop) {
		unsigned r = hg_wait(subs, n, 200);
		if (r & HG_READY_INTERRUPT)
			break;
		hg_serve();
		hg_alive();
	}
	return 0;
}
