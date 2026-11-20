// SPDX-License-Identifier: GPL-3.0-or-later
/* depth_hold: the classical depth controller (modules/depth_hold/README.md;
 * docs/07 "The mux", docs/09 "Example: adaptive depth hold"). Reads nav.state
 * and publishes, at 50 Hz, ctl.depth_hold (a wrench, heave only) and
 * ctl.depth_hold.target (the depth held and the vertical acceleration wanted,
 * accel_z, which docs/09's dataset joins and its policy reads).
 *
 * It always publishes; whether its heave drives is the mux's choice: in
 * `assisted` while the pilot leaves the heave stick alone, in `residual:<name>`
 * with a policy's bounded residual added to it, and in a failsafe `hold`. It
 * reads that choice from mux.state (source_heave 2, or 4 in residual): while
 * it does not drive, its target follows the depth and its integrator is held,
 * so that a handover holds the depth it happens at (dh_ctl.h). nav.state is
 * its Inputs topic: a stale depth flags its output, and a flagged output that
 * drives makes the gate go neutral (docs/07). Before the first nav.state its
 * outputs are zeros flagged stale.
 *
 * Calls: `set` (a depth to hold) and `hold` (the depth it is at). */
#include "mod.h"

#include "depth_control_params.h"
#include "depth_set_req.h"
#include "depth_target.h"
#include "dh_ctl.h"
#include "empty.h"
#include "hg_call.h"
#include "hg_params.h"
#include "mux_state.h"
#include "nav_state.h"
#include "safety_state.h"
#include "wrench.h"

#include <math.h>

#define WHO "depth_hold"
#define PERIOD_NS 20000000ull /* 50 Hz */

static depth_control_params P;
static struct dh_ctl C;
static hg_sub *sub_nav, *sub_mux, *sub_safety;
static hg_pub *pub_cmd, *pub_target;

static struct {
	struct nav_state nav;
	struct mux_state mux;
	struct safety_state safety;
	struct hg_info nav_info, mux_info, safety_info;
	int have_nav, have_mux, have_safety;
	uint64_t last_ns;
	char status[160];
} D;

static struct dh_gains gains(void)
{
	return (struct dh_gains){ .kp = P.kp, .kd = P.kd, .accel_max = P.accel_max, .ff = P.ff, .ki = P.ki,
				  .i_max = P.i_max, .i_band = P.i_band, .max_thrust = P.max_thrust };
}

static int on_set(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)rep;
	const depth_set_req *r = req;
	if (!isfinite(r->depth_m) || r->depth_m < -1.0f || r->depth_m > 1000.0f) {
		mod_fmt(reason, HG_CALL_REASON, "depth %g m out of range -1 to 1000", (double)r->depth_m);
		return HG_E_REFUSED;
	}
	dh_set(&C, r->depth_m);
	mod_log(WHO, "target %.2f m by %s", (double)r->depth_m, c->caller);
	return HG_OK;
}

static int on_hold(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	(void)reason;
	dh_hold_current(&C);
	mod_log(WHO, "hold the current depth, by %s", c->caller);
	return HG_OK;
}

static void tick(uint64_t now)
{
	mod_latest(sub_nav, &D.nav, sizeof D.nav, &D.nav_info, &D.have_nav);
	mod_latest(sub_mux, &D.mux, sizeof D.mux, &D.mux_info, &D.have_mux);
	mod_latest(sub_safety, &D.safety, sizeof D.safety, &D.safety_info, &D.have_safety);
	int mux_ok = D.have_mux && mod_fresh(sub_mux, &D.mux_info, now);
	/* 2: a controller; 4: a policy, which in residual mode is the controller's
	 * heave with the policy's residual added (msg/mux_state.msg, mux.c). */
	int driving = mux_ok && (D.mux.source_heave == 2 || D.mux.source_heave == 4);
	int enabled = D.have_safety && mod_fresh(sub_safety, &D.safety_info, now) && D.safety.outputs_enabled;
	int nav_ok = D.have_nav && isfinite(D.nav.depth_m) && isfinite(D.nav.vel_z) &&
		     !(D.nav_info.flags & HG_SLOT_STALE) && mod_fresh(sub_nav, &D.nav_info, now);

	struct wrench w;
	memset(&w, 0, sizeof w);
	struct depth_target t;
	memset(&t, 0, sizeof t);
	if (!D.have_nav) {
		/* No state yet: nothing to hold, and nothing a reader may take. */
		hg_publish_with(pub_cmd, &w, sizeof w, now, HG_STALE | HG_NO_REFS, NULL, 0);
		hg_publish_with(pub_target, &t, sizeof t, now, HG_STALE | HG_NO_REFS, NULL, 0);
		D.last_ns = now;
		return;
	}
	double dt = D.last_ns ? (double)(now - D.last_ns) / 1e9 : 0.0;
	if (dt > 0.2)
		dt = 0.2;
	D.last_ns = now;
	struct dh_gains g = gains();
	/* A stale state still steps the controller (its output is flagged through
	 * the reference, docs/01), but the integrator learns nothing from it. */
	struct dh_out o = dh_step(&C, &g, D.nav.depth_m, D.nav.vel_z, driving, enabled && nav_ok, dt);
	w.heave = o.heave;
	t.target_m = (float)C.target;
	t.depth_m = D.nav.depth_m;
	t.vel_z = D.nav.vel_z;
	t.accel_z = o.accel_z;
	t.integral = (float)C.integral;
	t.driving = (uint8_t)driving;
	t.latched = (uint8_t)C.follow;
	/* nav.state's reference is the Inputs default: the newest read. */
	hg_publish(pub_cmd, &w, sizeof w, now);
	hg_publish(pub_target, &t, sizeof t, now);

	char line[160];
	mod_fmt(line, sizeof line, "%s target %.2f m, depth %.2f m, heave %+.2f (I %+.2f)%s",
		driving ? "driving:" : "idle:", t.target_m, t.depth_m, w.heave, t.integral,
		nav_ok ? "" : "; nav.state stale");
	if (strcmp(line, D.status) != 0) {
		mod_fmt(D.status, sizeof D.status, "%s", line);
		hg_status(line);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	mod_init(WHO);
	if (hg_params_open(&DEPTH_CONTROL_PARAMS_TYPE, &P, NULL, NULL) < 0) {
		mod_log(WHO, "hg_params_open: %s", strerror(errno));
		return 4;
	}
	dh_init(&C);
	sub_nav = hg_sub_open("nav.state", &NAV_STATE_TYPE);
	sub_mux = hg_sub_open("mux.state", &MUX_STATE_TYPE);
	sub_safety = hg_sub_open("safety.state", &SAFETY_STATE_TYPE);
	if (!sub_nav || !sub_mux || !sub_safety) {
		mod_log(WHO, "hg_sub_open: %s", strerror(errno));
		return 4;
	}
	pub_cmd = hg_pub_open("ctl.depth_hold", &WRENCH_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_target = hg_pub_open("ctl.depth_hold.target", &DEPTH_TARGET_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	if (!pub_cmd || !pub_target) {
		mod_log(WHO, "hg_pub_open: %s", strerror(errno));
		return 4;
	}
	if (hg_serve_method("set", on_set, NULL) < 0 || hg_serve_method("hold", on_hold, NULL) < 0) {
		mod_log(WHO, "hg_serve_method: %s", strerror(errno));
		return 4;
	}
	/* The wait set is the call topics alone: the inputs are read at the tick
	 * (hangar#1). */
	hg_sub *subs[HG_MAX_WAIT];
	int n = hg_call_subs(subs, HG_MAX_WAIT);
	if (n < 0) {
		mod_log(WHO, "hg_call_subs: %s", strerror(errno));
		return 4;
	}
	mod_log(WHO, "ctl.depth_hold at 50 Hz; kp %g kd %g ff %g ki %g", (double)P.kp, (double)P.kd, (double)P.ff,
		(double)P.ki);
	uint64_t now = hg_now_ns(), next = now;
	while (!mod_stop) {
		now = hg_now_ns();
		int timeout = next > now ? (int)((next - now + 999999u) / 1000000u) : 0;
		unsigned r = n ? hg_wait(subs, n, timeout) : mod_sleep_ms(timeout);
		if (r & HG_READY_INTERRUPT)
			break;
		hg_serve();
		if (hg_params_changed())
			mod_log(WHO, "parameters v%llu by %s", (unsigned long long)hg_params_version(),
				hg_params_changed_by());
		now = hg_now_ns();
		if (now >= next) {
			tick(now);
			next += PERIOD_NS;
			if (next < now)
				next = now + PERIOD_NS;
		}
	}
	/* A last command of zero: whoever reads it next sees no push. */
	struct wrench zero;
	memset(&zero, 0, sizeof zero);
	hg_publish_with(pub_cmd, &zero, sizeof zero, hg_now_ns(), HG_STALE | HG_NO_REFS, NULL, 0);
	mod_log(WHO, "stopping");
	return 0;
}
