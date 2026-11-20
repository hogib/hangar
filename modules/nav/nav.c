// SPDX-License-Identifier: GPL-3.0-or-later
/* nav: the vehicle's state, nav.state, at 50 Hz (modules/nav/README.md): the
 * depth and vertical velocity from nav_filter (nav.depth corrected, nav.imu's
 * acceleration rotated by nav.attitude predicted), the attitude and rates as
 * nav.attitude gives them, the body acceleration with gravity removed. The
 * anchor of docs/09's depth-hold dataset, and the depth controller's input.
 *
 * nav.depth is the Inputs topic, nav.attitude and nav.imu the Sources: a tick
 * references the attitude and the IMU only when it used them, so a stale IMU
 * the filter does without does not flag nav.state, and a stale depth does
 * (docs/01, stale inputs). Before the first depth nav.state is published as
 * zeros flagged stale: the module is ready and the manager's staleness check
 * holds, and no reader takes it for a measurement. */
#include "mod.h"

#include "attitude.h"
#include "depth.h"
#include "hg_call.h"
#include "hg_params.h"
#include "imu.h"
#include "nav_filter.h"
#include "nav_params.h"
#include "nav_state.h"

#include <math.h>

#define WHO "nav"
#define PERIOD_NS 20000000ull /* 50 Hz */

static nav_params P;
static struct nav_filter F;
static hg_sub *sub_depth, *sub_att, *sub_imu;
static hg_pub *pub_state;

static struct {
	struct attitude att;
	struct imu imu;
	struct hg_info att_info, imu_info, depth_info;
	int have_att, have_imu;
	uint64_t depths;      /* depth messages applied */
	uint64_t last_predict_ns;
	int used_imu;
	char status[160];
} N;

static int finite3(const float v[3])
{
	return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static int finite_q(const float q[4])
{
	float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	return isfinite(n) && n > 0.5f && n < 1.5f;
}

static void apply_params(void)
{
	F.accel_noise = P.accel_noise;
	F.bias_walk = P.bias_walk;
	F.depth_noise = P.depth_noise;
}

static void tick(uint64_t now)
{
	/* The attitude and the IMU: the newest of each, used while fresh. */
	mod_latest(sub_att, &N.att, sizeof N.att, &N.att_info, &N.have_att);
	mod_latest(sub_imu, &N.imu, sizeof N.imu, &N.imu_info, &N.have_imu);
	int att_ok = N.have_att && mod_fresh(sub_att, &N.att_info, now) && finite_q(N.att.q) &&
		     !(N.att_info.flags & HG_SLOT_STALE);
	int imu_ok = N.have_imu && mod_fresh(sub_imu, &N.imu_info, now) && finite3(N.imu.accel) &&
		     !(N.imu_info.flags & HG_SLOT_STALE);
	int use_accel = P.use_imu && att_ok && imu_ok;
	double a_down = use_accel ? nav_vertical_accel(N.att.q, N.imu.accel) : 0.0;

	/* Predict to now, then each depth that arrived since the last tick. */
	double dt = N.last_predict_ns ? (double)(now - N.last_predict_ns) / 1e9 : 0.0;
	if (dt > 0.2)
		dt = 0.2; /* a stalled loop: the uncertainty grows by the limit, not by the gap */
	nav_filter_predict(&F, dt, use_accel, a_down);
	N.last_predict_ns = now;
	struct depth d;
	struct hg_info di;
	while (hg_next(sub_depth, &d, sizeof d, &di) == HG_MSG) {
		if (!isfinite(d.depth_m))
			continue;
		nav_filter_depth(&F, d.depth_m);
		N.depth_info = di;
		N.depths++;
	}

	struct nav_state st;
	memset(&st, 0, sizeof st);
	st.depth_m = (float)F.x[0];
	st.vel_z = (float)F.x[1];
	if (att_ok) {
		st.roll = N.att.roll;
		st.pitch = N.att.pitch;
		st.yaw = N.att.yaw;
		memcpy(st.rates, N.att.rates, sizeof st.rates);
	}
	if (use_accel)
		nav_body_accel(N.att.q, N.imu.accel, st.accel);

	if (!F.started) {
		hg_publish_with(pub_state, &st, sizeof st, now, HG_STALE | HG_NO_REFS, NULL, 0);
	} else {
		struct hg_input refs[2];
		int n = 0;
		if (att_ok)
			refs[n++] = (struct hg_input){ .topic_id = hg_sub_topic_id(sub_att), .seq = N.att_info.seq };
		if (use_accel)
			refs[n++] = (struct hg_input){ .topic_id = hg_sub_topic_id(sub_imu), .seq = N.imu_info.seq };
		/* nav.depth's reference is the Inputs default: the newest read. */
		hg_publish_with(pub_state, &st, sizeof st, now, 0, refs, n);
	}
	N.used_imu = use_accel;

	char line[160];
	if (!F.started)
		mod_fmt(line, sizeof line, "waiting for nav.depth");
	else
		mod_fmt(line, sizeof line, "depth %.2f m, vel_z %+.2f m/s, bias %+.3f m/s^2; %s%s", st.depth_m,
			st.vel_z, F.x[2], use_accel ? "imu" : "depth only",
			att_ok ? "" : ", no attitude");
	if (strcmp(line, N.status) != 0) {
		mod_fmt(N.status, sizeof N.status, "%s", line);
		hg_status(line);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	mod_init(WHO);
	if (hg_params_open(&NAV_PARAMS_TYPE, &P, NULL, NULL) < 0) {
		mod_log(WHO, "hg_params_open: %s", strerror(errno));
		return 4;
	}
	nav_filter_init(&F, P.accel_noise, P.bias_walk, P.depth_noise);
	sub_depth = hg_sub_open("nav.depth", &DEPTH_TYPE);
	sub_att = hg_sub_open("nav.attitude", &ATTITUDE_TYPE);
	sub_imu = hg_sub_open("nav.imu", &IMU_TYPE);
	if (!sub_depth || !sub_att || !sub_imu) {
		mod_log(WHO, "hg_sub_open: %s", strerror(errno));
		return 4;
	}
	pub_state = hg_pub_open("nav.state", &NAV_STATE_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	if (!pub_state) {
		mod_log(WHO, "hg_pub_open(nav.state): %s", strerror(errno));
		return 4;
	}
	/* The wait set is the call topics alone (set_params): the inputs are read
	 * at the tick, and in the set they would be news between two ticks and
	 * make hg_wait return at once (hangar#1). */
	hg_sub *subs[HG_MAX_WAIT];
	int n = hg_call_subs(subs, HG_MAX_WAIT);
	if (n < 0) {
		mod_log(WHO, "hg_call_subs: %s", strerror(errno));
		return 4;
	}
	mod_log(WHO, "nav.state at 50 Hz from nav.depth, nav.attitude and nav.imu; use_imu %u", P.use_imu);
	uint64_t now = hg_now_ns(), next = now;
	while (!mod_stop) {
		now = hg_now_ns();
		int timeout = next > now ? (int)((next - now + 999999u) / 1000000u) : 0;
		unsigned r = n ? hg_wait(subs, n, timeout) : mod_sleep_ms(timeout);
		if (r & HG_READY_INTERRUPT)
			break;
		hg_serve();
		if (hg_params_changed()) {
			apply_params();
			mod_log(WHO, "parameters v%llu by %s", (unsigned long long)hg_params_version(),
				hg_params_changed_by());
		}
		now = hg_now_ns();
		if (now >= next) {
			tick(now);
			next += PERIOD_NS;
			if (next < now)
				next = now + PERIOD_NS;
		}
	}
	mod_log(WHO, "stopping");
	hg_pub_close(pub_state);
	return 0;
}
