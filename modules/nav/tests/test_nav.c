// SPDX-License-Identifier: GPL-3.0-or-later
// nav: first the filter alone against a known vertical motion (a sine in
// depth, the vehicle tilted, the accelerometer biased and noisy, the depth
// noisy): the velocity's error, its lag behind a step in velocity, the bias
// found, with the IMU and without. Then the module under a manager, this
// process as the vehicle's sensors (nav.depth, nav.attitude, nav.imu): 50 Hz,
// the descent rate, the references (the IMU only while it is used), a stale
// IMU that does not flag nav.state and a stale depth that does, zeros flagged
// before the first depth, a parameter change, the idle CPU. Under 30 s.
#include "hd_test.h"

#include <math.h>

#include "attitude.h"
#include "depth.h"
#include "imu.h"
#include "nav_filter.h"
#include "nav_state.h"

static pid_t mgr;
static const char *T_nav;

/* ---- A deterministic noise ------------------------------------------------- */

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static double uniform(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (double)(rng >> 11) / 9007199254740992.0;
}

static double gauss(void)
{
	double u = uniform(), v = uniform();
	if (u < 1e-12)
		u = 1e-12;
	return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

/* The quaternion (w x y z, body to NED) of a roll and a pitch, no yaw. */
static void quat(double roll, double pitch, float q[4])
{
	double cr = cos(roll / 2), sr = sin(roll / 2), cp = cos(pitch / 2), sp = sin(pitch / 2);
	q[0] = (float)(cr * cp);
	q[1] = (float)(sr * cp);
	q[2] = (float)(cr * sp);
	q[3] = (float)(-sr * sp);
}

/* The specific force a body accelerometer reads when the vehicle accelerates
 * a_down (m/s^2, positive down) in NED: f = R^T (a - g), a = (0, 0, a_down). */
static void specific_force(const float q[4], double a_down, float f[3])
{
	float w = q[0], x = q[1], y = q[2], z = q[3];
	float r[3] = { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
	for (int i = 0; i < 3; i++)
		f[i] = (float)((a_down - NAV_G) * r[i]);
}

/* ---- The filter alone ---------------------------------------------------------- */

/* 60 s at 50 Hz of z(t) = 2 + 0.5 sin(0.5 t), tilted 10 deg in roll and 5 in
 * pitch, the accelerometer's body z biased +0.2 m/s^2 and every axis noisy
 * (0.05 m/s^2), the depth noisy (0.02 m). The RMS velocity error after 10 s. */
static double sine_run(int use_imu, double *bias_err)
{
	struct nav_filter f;
	nav_filter_init(&f, 0.5, 0.01, 0.02);
	float q[4];
	quat(10 * M_PI / 180, 5 * M_PI / 180, q);
	double dt = 0.02, se = 0;
	int n = 0;
	float r33 = 1 - 2 * (q[1] * q[1] + q[2] * q[2]);
	for (int k = 0; k < 3000; k++) {
		double t = k * dt;
		double z = 2 + 0.5 * sin(0.5 * t), v = 0.25 * cos(0.5 * t), a = -0.125 * sin(0.5 * t);
		float fb[3];
		specific_force(q, a, fb);
		fb[2] += 0.2f;
		for (int i = 0; i < 3; i++)
			fb[i] += (float)(0.05 * gauss());
		nav_filter_predict(&f, dt, use_imu, nav_vertical_accel(q, fb));
		nav_filter_depth(&f, z + 0.02 * gauss());
		if (t >= 10) {
			se += (f.x[1] - v) * (f.x[1] - v);
			n++;
		}
	}
	if (bias_err)
		*bias_err = fabs(f.x[2] - 0.2 * r33);
	return sqrt(se / n);
}

/* From rest to 0.3 m/s down at once (a jerk the depth alone sees late): the
 * time until the estimate is within 10 % of it. Level, no bias, small noise. */
static double step_lag(int use_imu)
{
	struct nav_filter f;
	nav_filter_init(&f, 0.5, 0.01, 0.02);
	float q[4];
	quat(0, 0, q);
	double dt = 0.02, z = 5;
	for (int k = 0; k < 500; k++) { /* 10 s at rest to settle */
		float fb[3];
		specific_force(q, 0, fb);
		nav_filter_predict(&f, dt, use_imu, nav_vertical_accel(q, fb));
		nav_filter_depth(&f, z);
	}
	/* The step: one tick of 15 m/s^2 (0.3 m/s in 20 ms), then constant. */
	for (int k = 0; k < 500; k++) {
		double a = k == 0 ? 15.0 : 0.0;
		z += 0.3 * dt;
		float fb[3];
		specific_force(q, a, fb);
		nav_filter_predict(&f, dt, use_imu, nav_vertical_accel(q, fb));
		nav_filter_depth(&f, z);
		if (fabs(f.x[1] - 0.3) < 0.03)
			return (k + 1) * dt;
	}
	return 99;
}

static void test_filter(void)
{
	// At rest and level the accelerometer reads -g on its z: no vertical acceleration.
	float q[4], fb[3];
	quat(0, 0, q);
	specific_force(q, 0, fb);
	CHECK(fabsf(nav_vertical_accel(q, fb)) < 1e-4f);
	// Tilted and accelerating 1 m/s^2 down: 1 m/s^2 found, and gravity gone from the body.
	quat(20 * M_PI / 180, -15 * M_PI / 180, q);
	specific_force(q, 1.0, fb);
	CHECK(fabsf(nav_vertical_accel(q, fb) - 1.0f) < 1e-4f);
	float ab[3];
	nav_body_accel(q, fb, ab);
	CHECK(fabsf(sqrtf(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]) - 1.0f) < 1e-4f);

	double bias_err = 0;
	double rms_imu = sine_run(1, &bias_err);
	rng = 0x9e3779b97f4a7c15ull;
	double rms_depth = sine_run(0, NULL);
	printf("    a sine of 0.5 m at 0.5 rad/s, tilted, biased: velocity RMS error %.4f m/s with the IMU, "
	       "%.4f depth only; the bias found within %.4f m/s^2\n", rms_imu, rms_depth, bias_err);
	CHECK(rms_imu < 0.025);
	CHECK(rms_imu < rms_depth);
	CHECK(bias_err < 0.03);
	double lag_imu = step_lag(1), lag_depth = step_lag(0);
	printf("    a step to 0.3 m/s: within 10 %% after %.2f s with the IMU, %.2f s depth only\n", lag_imu,
	       lag_depth);
	CHECK(lag_imu <= 0.1);
	CHECK(lag_depth < 5);
	CHECK(lag_imu < lag_depth);
}

/* ---- The module ------------------------------------------------------------------- */

static hg_pub *P_depth, *P_att, *P_imu;
static hg_sub *S_state;
static struct nav_state last;
static struct hg_info last_info;
static int have_last;
static struct {
	int publish_depth, publish_att, publish_imu;
	double z, v; /* the vehicle: depth and a constant descent rate */
	uint64_t next_ns, t0;
} V = { .publish_depth = 1, .publish_att = 1, .publish_imu = 1, .z = 3.0, .v = 0.2 };

static void copy_types(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *types[] = { "attitude", "nav_state", "nav_params", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src, types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		if (copy_file(from, to) < 0) {
			fprintf(stderr, "cannot copy %s\n", from);
			exit(2);
		}
	}
}

static void become(const char *module)
{
	char path[600];
	if (wiring_to_file(module, path, sizeof path) < 0) {
		printf("    cannot fetch the wiring of %s\n", module);
		exit(2);
	}
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", module, 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		printf("    hg_init as %s: %s\n", module, hg_init_error());
		exit(2);
	}
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
}

/* The sensors at 50 Hz, the vehicle descending at V.v, level; nav.state read. */
static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	do {
		uint64_t now = hg_now_ns();
		if (now >= V.next_ns) {
			double dt = V.next_ns ? (double)(now - V.next_ns + 20000000u) / 1e9 : 0.02;
			V.next_ns = now + 20000000u;
			V.z += V.v * dt;
			float q[4];
			quat(0, 0, q);
			if (V.publish_depth) {
				struct depth d = { .depth_m = (float)V.z, .temp_c = 10, .pressure_mbar = 1013 };
				hg_publish(P_depth, &d, sizeof d, now);
			}
			if (V.publish_att) {
				struct attitude a;
				memset(&a, 0, sizeof a);
				memcpy(a.q, q, sizeof a.q);
				hg_publish(P_att, &a, sizeof a, now);
			}
			if (V.publish_imu) {
				struct imu m;
				memset(&m, 0, sizeof m);
				specific_force(q, 0, m.accel);
				hg_publish(P_imu, &m, sizeof m, now);
			}
		}
		hg_wait(&S_state, 1, 5);
		struct nav_state st;
		struct hg_info info;
		while (hg_next(S_state, &st, sizeof st, &info) == HG_MSG) {
			last = st;
			last_info = info;
			have_last = 1;
		}
	} while (hg_now_ns() < end);
}

static int refs_to(const struct hg_info *info, const char *topic)
{
	uint32_t id = hg_topic_id(topic);
	for (uint32_t i = 0; i < info->n_inputs; i++)
		if (info->inputs[i].topic_id == id)
			return 1;
	return 0;
}

static void test_module(void)
{
	manifest("nav", "[Module]\nExec=%s\n"
			"[Subscribe nav.depth]\n[Subscribe nav.attitude]\n[Subscribe nav.imu]\n"
			"[Publish nav.state]\nType=nav_state\nRate=50\nInputs=nav.depth\nSources=nav.attitude nav.imu\n"
			"[Params]\nType=nav_params\nAllow=tester admin\n"
			"[Restart]\nBackoff=0.1s..0.2s\n", T_nav);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe nav.state]\n"
			   "[Publish nav.depth]\nType=depth\nRate=50\nStaleAfter=200ms\n"
			   "[Publish nav.attitude]\nType=attitude\nRate=50\nStaleAfter=200ms\n"
			   "[Publish nav.imu]\nType=imu\nRate=50\nStaleAfter=200ms\n");
	mgr = manager_start();
	CHECK(wait_state("tester", "stopped", 5000));
	become("tester");
	S_state = hg_sub_open("nav.state", &NAV_STATE_TYPE);
	CHECK(S_state != NULL);
	// Before any sensor: ready (it publishes), zeros flagged stale.
	CHECK(wait_state("nav", "ready", 5000));
	struct hg_info info;
	struct nav_state st;
	uint64_t t0 = now_ms();
	while (hg_next(S_state, &st, sizeof st, &info) != HG_MSG && now_ms() - t0 < 1000)
		hg_wait(&S_state, 1, 20);
	CHECK(info.flags & HG_SLOT_STALE);
	CHECK(st.depth_m == 0 && st.vel_z == 0);

	P_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);
	P_att = hg_pub_open("nav.attitude", &ATTITUDE_TYPE, NULL);
	P_imu = hg_pub_open("nav.imu", &IMU_TYPE, NULL);
	CHECK(P_depth && P_att && P_imu);
	// Descending at 0.2 m/s: found within 3 s, 50 Hz, every input referenced.
	pump(3000);
	CHECK(have_last);
	printf("    descending at 0.20 m/s: nav.state says depth %.3f m (true %.3f), vel_z %.3f m/s\n",
	       (double)last.depth_m, V.z, (double)last.vel_z);
	CHECK(fabs(last.vel_z - 0.2) < 0.03);
	CHECK(fabs(last.depth_m - V.z) < 0.05);
	CHECK(!(last_info.flags & HG_SLOT_STALE));
	CHECK(refs_to(&last_info, "nav.depth") && refs_to(&last_info, "nav.attitude") &&
	      refs_to(&last_info, "nav.imu"));
	uint64_t s0 = last_info.seq;
	pump(1000);
	uint64_t n = last_info.seq - s0;
	printf("    %llu nav.state messages in 1 s\n", (unsigned long long)n);
	CHECK(n >= 45 && n <= 55);

	// The IMU stops: the depth alone, not flagged, the IMU no longer referenced.
	V.publish_imu = 0;
	pump(1000);
	CHECK(!(last_info.flags & HG_SLOT_STALE));
	CHECK(refs_to(&last_info, "nav.depth") && !refs_to(&last_info, "nav.imu"));
	CHECK(fabs(last.vel_z - 0.2) < 0.05);
	V.publish_imu = 1;
	pump(500);
	CHECK(refs_to(&last_info, "nav.imu"));

	// The depth stops: nav.state goes on, flagged as computed from a stale input.
	V.publish_depth = 0;
	pump(600);
	CHECK(last_info.flags & HG_SLOT_STALE);
	V.publish_depth = 1;
	pump(500);
	CHECK(!(last_info.flags & HG_SLOT_STALE));

	// A parameter: use_imu 0 through hangar param, taken live.
	char out[2048];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "nav", "use_imu=0", (char *)NULL), 0);
	pump(600);
	CHECK(!refs_to(&last_info, "nav.imu"));
	CHECK(fabs(last.vel_z - 0.2) < 0.05);

	// Idle: the module sleeps between its ticks.
	struct mstat m;
	CHECK(module_status("nav", &m) && m.pid > 0);
	long long c0 = th_cpu_ticks(m.pid);
	uint64_t a = hg_now_ns();
	pump(1000);
	double pct = th_cpu_percent(c0, th_cpu_ticks(m.pid), (long)((hg_now_ns() - a) / 1000000u));
	printf("    nav: %.1f %% of a core at 50 Hz\n", pct);
	CHECK(pct >= 0 && pct < 10);
}

static void test_shutdown(void)
{
	hg_pub_close(P_depth);
	hg_pub_close(P_att);
	hg_pub_close(P_imu);
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(void)
{
	T_nav = getenv("NAV_BIN");
	if (!T_nav) {
		fprintf(stderr, "NAV_BIN must be set\n");
		return 2;
	}
	hd_setup("nav", NULL);
	copy_types();
	RUN_TEST(test_filter);
	RUN_TEST(test_module);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
