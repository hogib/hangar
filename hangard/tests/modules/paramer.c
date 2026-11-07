// SPDX-License-Identifier: GPL-3.0-or-later
/* paramer: a module with [Params] Type=depth_hold_params. Its validator
 * refuses enabled=1 with max_thrust=0. Serves set_params (the library
 * does) and reports "params v<version> by <changed_by> kp=<kp>
 * rate_hz=<rate_hz> changes=<n>[ restart-pending]" on its status line.
 * --exit-after-changes N exits 0 after N applied changes (for the
 * module-not-running cases). */
#include "toy.h"

#include "depth_hold_params.h"
#include "hg_call.h"
#include "hg_params.h"

static depth_hold_params P;

static int validate(const void *candidate, const void *current, char reason[64])
{
	(void)current;
	const depth_hold_params *c = candidate;
	if (c->enabled && c->max_thrust == 0) {
		snprintf(reason, 64, "enabled with max_thrust 0");
		return HG_E_REFUSED;
	}
	return HG_OK;
}

int main(int argc, char **argv)
{
	toy_init("paramer");
	long exit_after = toy_num(argc, argv, "--exit-after-changes", -1);
	if (hg_params_open(&DEPTH_HOLD_PARAMS_TYPE, &P, validate, NULL) < 0) {
		fprintf(stderr, "paramer: hg_params_open: %s\n", strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT];
	int n = hg_call_subs(subs, HG_MAX_WAIT);
	if (n < 0) {
		fprintf(stderr, "paramer: hg_call_subs: %s\n", strerror(errno));
		return 4;
	}
	hg_ready();
	long changes = 0;
	char line[128];
	for (;;) {
		snprintf(line, sizeof line, "params v%llu by %s kp=%g rate_hz=%u changes=%ld%s",
			 (unsigned long long)hg_params_version(), hg_params_changed_by(), (double)P.kp, P.rate_hz, changes,
			 hg_params_restart_pending() ? " restart-pending" : "");
		hg_status(line);
		hg_alive();
		if (toy_stop)
			break;
		unsigned r = hg_wait(subs, n, 100);
		if (r & HG_READY_INTERRUPT)
			break;
		hg_serve();
		if (hg_params_changed()) {
			changes++;
			if (exit_after >= 0 && changes >= exit_after) {
				snprintf(line, sizeof line, "params v%llu by %s kp=%g rate_hz=%u changes=%ld exiting",
					 (unsigned long long)hg_params_version(), hg_params_changed_by(), (double)P.kp,
					 P.rate_hz, changes);
				hg_status(line);
				return 0;
			}
		}
	}
	return 0;
}
