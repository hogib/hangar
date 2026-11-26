// SPDX-License-Identifier: GPL-3.0-or-later
/* The thrusters' two mixings against lib/sim's thrusters (sim_applied_wrench,
 * the simulator's own command path). ArduSub's factors (thr_mix): each axis
 * alone pushes the vehicle the way the wrench says, and nowhere else to speak
 * of; a saturated mix scales down. The allocation through a frame
 * (thr_alloc): the wrench asked for is the wrench applied, on every axis and
 * with none across; its capacities, its order past the limits, a reversed
 * thruster, a frame that cannot pitch; and lib/sim's model flown under surge
 * with each, at a fixed step. */
#include "test_harness.h"
#include "dh_ctl.h"
#include "thr_alloc.h"
#include "thr_mix.h"
#include "sim_model.h"
#include "sim_thrust.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define VOLT 16.0
#define DEG (180.0 / 3.14159265358979323846)

static void wrench_of(const float in[6], float out[6])
{
	uint16_t pwm[THR_N];
	thr_mix(in, pwm);
	sim_applied_wrench(pwm, 16.0f, out);
}

static void test_neutral(void)
{
	uint16_t pwm[THR_N];
	const float z[6] = { 0 };
	CHECK(thr_mix(z, pwm) == 1);
	for (int i = 0; i < THR_N; i++)
		CHECK_EQ_INT(pwm[i], 1500);
	const float bad[6] = { NAN, INFINITY, -INFINITY, 0, 0, 0 };
	thr_mix(bad, pwm);
	for (int i = 0; i < THR_N; i++)
		CHECK_EQ_INT(pwm[i], 1500);
}

/* Body frame of the wrench out: x forward, y right, z down; moments about them. */
static void test_axes(void)
{
	static const char *name[6] = { "surge", "sway", "heave", "roll", "pitch", "yaw" };
	/* The body component each wrench axis should drive, and its sign: heave up
	 * is -z, yaw clockwise from above is +Mz. */
	static const int comp[6] = { 0, 1, 2, 3, 4, 5 };
	static const float sign[6] = { +1, +1, -1, +1, +1, +1 };
	for (int a = 0; a < 6; a++) {
		float in[6] = { 0 }, w[6];
		in[a] = 0.5f;
		wrench_of(in, w);
		float main = w[comp[a]] * sign[a];
		float cross = 0;
		for (int k = 0; k < 3; k++) /* forces against forces, moments against moments */
			if (k + (a >= 3 ? 3 : 0) != comp[a])
				cross = fmaxf(cross, fabsf(w[k + (a >= 3 ? 3 : 0)]));
		printf("    %-5s 0.5: %+.2f along its axis, %.2f across\n", name[a], (double)main, (double)cross);
		CHECK(main > 1.0f);
		if (a < 3) /* forces: a translation does not push sideways */
			CHECK(cross < 0.05f * main);
	}
}

static void test_heave_monotone_and_saturation(void)
{
	float w1[6], w2[6];
	wrench_of((float[6]){ 0, 0, 0.2f, 0, 0, 0 }, w1);
	wrench_of((float[6]){ 0, 0, 0.4f, 0, 0, 0 }, w2);
	printf("    heave 0.2: %.2f N up, 0.4: %.2f N up\n", (double)-w1[2], (double)-w2[2]);
	CHECK(-w2[2] > -w1[2]);
	uint16_t pwm[THR_N];
	float s = thr_mix((float[6]){ 1, 0, 0, 0, 0, 1 }, pwm);
	CHECK(fabsf(s - 0.5f) < 1e-6f);
	for (int i = 0; i < THR_N; i++)
		CHECK(pwm[i] >= 1100 && pwm[i] <= 1900);
}

/* ---- the allocation through a frame ------------------------------------------ */

static struct thr_alloc heavy_alloc(void)
{
	struct sim_frame f;
	sim_frame_bluerov2_heavy(&f);
	struct thr_alloc a;
	CHECK_EQ_INT(thr_alloc_init(&a, &f, VOLT), 0);
	return a;
}

static void test_alloc_capacity(void)
{
	struct thr_alloc a = heavy_alloc();
	CHECK_EQ_INT(a.n, 8);
	CHECK_EQ_INT(a.rank, 6);
	printf("    at %.0f V one T200 gives %.1f to %+.1f N; a command of 1 is surge %.1f N, sway %.1f N, heave %.1f N,\n"
	       "    roll %.2f N m, pitch %.2f N m, yaw %.2f N m\n",
	       VOLT, a.lo[0], a.hi[0], a.full[0], a.full[1], a.full[2], a.full[3], a.full[4], a.full[5]);
	/* Heave is the four verticals at the weaker of their two directions. */
	double weak = fmin(-a.lo[0], a.hi[0]);
	CHECK(fabs(a.full[2] - 4 * weak) < 1e-6);
	/* Surge and sway: the diagonals are symmetric, so the two are equal;
	 * each is less than the four horizontals' 4 x weak x cos 45, since
	 * the verticals take a share to cancel the horizontals' pitch or roll
	 * moment and the frame's own asymmetry is paid for. */
	CHECK(fabs(a.full[0] - a.full[1]) < 1e-6);
	CHECK(a.full[0] > 80 && a.full[0] <= 4 * weak * sqrt(0.5) + 1e-6);
	for (int m = 3; m < 6; m++)
		CHECK(a.full[m] > 5 && a.full[m] < 60);
	/* T T+ is the identity when every axis can be produced. */
	for (int r = 0; r < 6; r++)
		for (int c = 0; c < 6; c++) {
			double v = 0;
			for (int i = 0; i < THR_N; i++)
				v += a.T[r][i] * a.P[i][c];
			CHECK(fabs(v - (r == c)) < 1e-9);
		}
}

/* The wrench a pulse-width set applies, by the simulator's own path. */
static void applied(const uint16_t pwm[THR_N], double w[6])
{
	float f[6];
	sim_applied_wrench(pwm, (float)VOLT, f);
	for (int k = 0; k < 6; k++)
		w[k] = f[k];
}

static void test_alloc_gives_what_is_asked(void)
{
	static const char *name[6] = { "surge", "sway", "heave", "roll", "pitch", "yaw" };
	struct thr_alloc a = heavy_alloc();
	double worst_f = 0, worst_m = 0;
	for (int m = 0; m < 6; m++)
		for (int step = -4; step <= 4; step++) {
			float in[6] = { 0 };
			in[m] = step / 4.0f;
			struct thr_alloc_out o;
			thr_alloc_mix(&a, in, &o);
			CHECK(o.scale_hold == 1 && o.scale_move == 1);
			double w[6];
			applied(o.pwm, w);
			for (int k = 0; k < 6; k++) {
				/* The module's own account of what it gave is the simulator's. */
				CHECK(fabs(w[k] - o.given[k]) < 1e-3);
				double e = fabs(w[k] - o.asked[k]);
				if (k < 3)
					worst_f = fmax(worst_f, e);
				else
					worst_m = fmax(worst_m, e);
			}
			if (step == 2)
				printf("    %-5s 0.5: asked %+7.2f, applied %+7.2f\n", name[m], o.asked[m], w[m]);
		}
	/* What is left is the pulse width's whole microsecond. */
	printf("    worst difference between asked and applied, an axis at a time: %.3f N, %.4f N m\n", worst_f, worst_m);
	CHECK(worst_f < 0.5);
	CHECK(worst_m < 0.08);
}

static void test_alloc_agrees_with_the_factors_in_sign(void)
{
	/* The two mixings read a wrench the same way round on every axis. */
	struct thr_alloc a = heavy_alloc();
	for (int m = 0; m < 6; m++) {
		float in[6] = { 0 }, old[6];
		in[m] = 0.5f;
		wrench_of(in, old);
		struct thr_alloc_out o;
		thr_alloc_mix(&a, in, &o);
		CHECK(old[m] * o.given[m] > 0);
	}
	/* Where they differ: surge by the factors comes with the horizontals'
	 * pitch moment, 0.085 N m a newton; allocated, with none. */
	float in[6] = { 0.5f, 0, 0, 0, 0, 0 }, old[6];
	wrench_of(in, old);
	struct thr_alloc_out o;
	thr_alloc_mix(&a, in, &o);
	printf("    surge 0.5 by ArduSub's factors: %.1f N with %+.2f N m of pitch; allocated: %.1f N with %+.3f N m\n",
	       (double)old[0], (double)old[4], o.given[0], o.given[4]);
	CHECK(fabs(old[4] - 0.085f * old[0]) < 0.02f);
	CHECK(old[4] > 2.0f);
	CHECK(fabs(o.given[4]) < 0.08);
}

static void test_alloc_past_the_limits(void)
{
	struct thr_alloc a = heavy_alloc();
	struct thr_alloc_out o;
	/* Full surge beside full heave: the heave is given whole, the surge
	 * gets what the verticals have left for its pitch correction. */
	thr_alloc_mix(&a, (float[6]){ 1, 0, 1, 0, 0, 0 }, &o);
	printf("    surge 1 + heave 1: heave x%.2f, surge x%.2f\n", o.scale_hold, o.scale_move);
	CHECK(o.scale_hold == 1);
	CHECK(o.scale_move < 0.05); /* at full heave the verticals have nothing left */
	CHECK(fabs(o.given[2] - o.asked[2]) < 0.5);
	CHECK(fabs(o.given[4]) < 0.1); /* and still no pitch moment */
	/* Up to half the heave, the whole surge. */
	thr_alloc_mix(&a, (float[6]){ 1, 0, 0.5f, 0, 0, 0 }, &o);
	printf("    surge 1 + heave 0.5: heave x%.2f, surge x%.2f\n", o.scale_hold, o.scale_move);
	CHECK(o.scale_hold == 1 && o.scale_move > 0.99);
	thr_alloc_mix(&a, (float[6]){ 1, 0, 0.75f, 0, 0, 0 }, &o);
	printf("    surge 1 + heave 0.75: heave x%.2f, surge x%.2f\n", o.scale_hold, o.scale_move);
	CHECK(o.scale_hold == 1 && o.scale_move > 0.4 && o.scale_move < 0.6);
	/* Surge, sway and yaw together: scaled as one, their proportions kept. */
	thr_alloc_mix(&a, (float[6]){ 1, 0.5f, 0, 0, 0, 1 }, &o);
	printf("    surge 1 + sway 0.5 + yaw 1: x%.2f\n", o.scale_move);
	CHECK(o.scale_hold == 1 && o.scale_move < 1 && o.scale_move > 0.2);
	CHECK(fabs(o.given[0] - o.scale_move * o.asked[0]) < 0.6);
	CHECK(fabs(o.given[1] - o.scale_move * o.asked[1]) < 0.6);
	CHECK(fabs(o.given[5] - o.scale_move * o.asked[5]) < 0.1);
	/* Heave, roll and pitch too much by themselves: scaled as one, and
	 * nothing is left for moving. */
	thr_alloc_mix(&a, (float[6]){ 1, 0, 1, 1, 1, 0 }, &o);
	printf("    heave 1 + roll 1 + pitch 1 (+ surge 1): x%.2f, surge x%.2f\n", o.scale_hold, o.scale_move);
	CHECK(o.scale_hold < 1 && o.scale_hold > 0.2);
	for (int k = 2; k < 5; k++)
		CHECK(fabs(o.given[k] - o.scale_hold * o.asked[k]) < (k == 2 ? 0.6 : 0.1));
	/* Whatever is asked, no thruster past its range. */
	for (int bits = 0; bits < 729; bits++) {
		float in[6];
		int b = bits;
		for (int m = 0; m < 6; m++, b /= 3)
			in[m] = (float)(b % 3 - 1);
		thr_alloc_mix(&a, in, &o);
		for (int i = 0; i < THR_N; i++)
			CHECK(o.pwm[i] >= 1100 && o.pwm[i] <= 1900);
		CHECK(o.scale_hold >= 0 && o.scale_hold <= 1 && o.scale_move >= 0 && o.scale_move <= 1);
	}
}

static void test_alloc_pulse_width(void)
{
	CHECK_EQ_INT(thr_alloc_pwm(0, VOLT), 1500);
	CHECK_EQ_INT(thr_alloc_pwm(NAN, VOLT), 1500);
	CHECK_EQ_INT(thr_alloc_pwm(1e-4, VOLT), 1500);
	CHECK_EQ_INT(thr_alloc_pwm(1000, VOLT), 1900);
	CHECK_EQ_INT(thr_alloc_pwm(-1000, VOLT), 1100);
	double lo = sim_t200_thrust(1100, VOLT), hi = sim_t200_thrust(1900, VOLT), worst = 0;
	int last = 1100;
	for (double f = lo; f <= hi; f += 0.05) {
		int pwm = thr_alloc_pwm(f, VOLT);
		CHECK(pwm >= last); /* more thrust never takes a shorter pulse */
		last = pwm;
		if (fabs(f) > 0.05)
			CHECK((pwm > 1500) == (f > 0));
		worst = fmax(worst, fabs(sim_t200_thrust(pwm, VOLT) - f));
	}
	printf("    thrust to pulse width and back: within %.3f N over the whole range\n", worst);
	CHECK(worst < 0.25);
	/* Another supply voltage, another pulse width for the same thrust. */
	CHECK(thr_alloc_pwm(20, 12.0) > thr_alloc_pwm(20, 16.0));
}

static void test_alloc_reversed_thruster(void)
{
	struct sim_frame f;
	sim_frame_bluerov2_heavy(&f);
	struct thr_alloc straight, mirrored;
	CHECK_EQ_INT(thr_alloc_init(&straight, &f, VOLT), 0);
	f.geometry.polarity[2] = -1; /* thruster 3 installed the other way round */
	CHECK_EQ_INT(thr_alloc_init(&mirrored, &f, VOLT), 0);
	struct thr_alloc_out a, b;
	const float in[6] = { 0.3f, 0, 0, 0, 0, 0.2f };
	thr_alloc_mix(&straight, in, &a);
	thr_alloc_mix(&mirrored, in, &b);
	/* The same wrench, thruster 3 driven the other way. */
	CHECK((a.pwm[2] > 1500) != (b.pwm[2] > 1500));
	CHECK(a.pwm[2] != 1500 && b.pwm[2] != 1500);
	for (int k = 0; k < 6; k++)
		CHECK(fabs(b.given[k] - b.asked[k]) < (k < 3 ? 0.5 : 0.08));
}

/* A made-up frame of six: four vectored horizontals in the origin's plane,
 * two verticals abeam. It cannot pitch. Not a vehicle's geometry. */
static const char SIX[] = "[Frame]\nName=six\n"
			  "[Thruster 1]\nPosition=0.2 0.1 0\nDirection=-1 1 0\n"
			  "[Thruster 2]\nPosition=0.2 -0.1 0\nDirection=-1 -1 0\n"
			  "[Thruster 3]\nPosition=-0.2 0.1 0\nDirection=1 1 0\n"
			  "[Thruster 4]\nPosition=-0.2 -0.1 0\nDirection=1 -1 0\n"
			  "[Thruster 5]\nPosition=0 0.15 0\nDirection=0 0 1\n"
			  "[Thruster 6]\nPosition=0 -0.15 0\nDirection=0 0 1\n";

static void test_alloc_six_thrusters(void)
{
	struct sim_frame f;
	char err[120];
	CHECK_EQ_INT(sim_frame_parse(SIX, sizeof SIX - 1, &f, err, sizeof err), 0);
	struct thr_alloc a;
	CHECK_EQ_INT(thr_alloc_init(&a, &f, VOLT), 0);
	CHECK_EQ_INT(a.n, 6);
	CHECK_EQ_INT(a.rank, 5);
	printf("    six: surge %.1f N, sway %.1f N, heave %.1f N, roll %.2f, pitch %.2f, yaw %.2f N m\n", a.full[0],
	       a.full[1], a.full[2], a.full[3], a.full[4], a.full[5]);
	CHECK(a.full[4] == 0); /* pitch: not this frame's to give */
	for (int m = 0; m < 6; m++)
		if (m != 4)
			CHECK(a.full[m] > 1);
	struct thr_alloc_out o;
	thr_alloc_mix(&a, (float[6]){ 0, 0, 0, 0, 1, 0 }, &o);
	for (int i = 0; i < THR_N; i++)
		CHECK_EQ_INT(o.pwm[i], 1500);
	thr_alloc_mix(&a, (float[6]){ 0.5f, 0, 0.5f, 0, 1, 0 }, &o);
	CHECK(o.given[0] > 0.4 * a.full[0] && -o.given[2] > 0.4 * a.full[2]);
	CHECK_EQ_INT(o.pwm[6], 1500);
	CHECK_EQ_INT(o.pwm[7], 1500);
	int moved = 0;
	for (int i = 0; i < 6; i++)
		moved += o.pwm[i] != 1500;
	CHECK_EQ_INT(moved, 6);
}

static void test_alloc_neutral(void)
{
	struct thr_alloc a = heavy_alloc();
	struct thr_alloc_out o;
	thr_alloc_mix(&a, (float[6]){ 0 }, &o);
	for (int i = 0; i < THR_N; i++)
		CHECK_EQ_INT(o.pwm[i], 1500);
	thr_alloc_mix(&a, (float[6]){ NAN, INFINITY, -INFINITY, NAN, NAN, NAN }, &o);
	/* Non-finite is zero; an infinity is not a finite number either. */
	for (int i = 0; i < THR_N; i++)
		CHECK_EQ_INT(o.pwm[i], 1500);
	/* A command past 1 is 1. */
	struct thr_alloc_out one, two;
	thr_alloc_mix(&a, (float[6]){ 0, 0, 1, 0, 0, 0 }, &one);
	thr_alloc_mix(&a, (float[6]){ 0, 0, 2, 0, 0, 0 }, &two);
	CHECK(memcmp(one.pwm, two.pwm, sizeof one.pwm) == 0);
}

/* ---- lib/sim's model flown under surge, at a fixed step ---------------------- */

struct flight {
	double max_pitch;  /* rad, the largest |pitch| while the command was held */
	double late_pitch; /* the largest in the last 10 s of the hold */
	double end_pitch;  /* 20 s after the command was let go */
	double speed;      /* m/s forward at the end of the hold */
	int tumbled;       /* the vehicle's top pointed down at some time */
};

/* 40 s of `surge`, then 20 s of nothing, 200 Hz, commands at 50 Hz, the
 * thrusters' 50 ms lag; level and at 20 m to start, neutrally buoyant so that
 * pitch is the only question. `measured`: von Benzon's translational damping
 * in place of Wu's. `allocated`: thr_alloc in place of ArduSub's factors. */
static struct flight fly_surge(float surge, int measured, int allocated)
{
	struct sim_frame f;
	sim_frame_bluerov2_heavy(&f);
	struct thr_alloc a;
	thr_alloc_init(&a, &f, VOLT);
	struct sim_params p = sim_params_bluerov2_heavy();
	if (measured)
		sim_params_damping_vonbenzon(&p);
	p.buoyancy = p.weight;
	struct sim_state s;
	sim_state_init(&s);
	s.pos[2] = 20;
	struct sim_thrusters th;
	sim_thrusters_init(&th, &f.geometry, 0.05);
	struct flight r = { 0 };
	uint16_t pwm[THR_N];
	for (int i = 0; i < 60 * 200; i++) {
		if (i % 4 == 0) {
			const float in[6] = { i < 40 * 200 ? surge : 0, 0, 0, 0, 0, 0 };
			if (allocated) {
				struct thr_alloc_out o;
				thr_alloc_mix(&a, in, &o);
				memcpy(pwm, o.pwm, sizeof pwm);
			} else {
				thr_mix(in, pwm);
			}
			sim_thrusters_command(&th, pwm, VOLT);
		}
		sim_thrusters_step(&th, SIM_DEFAULT_DT);
		double tau[6], pitch;
		sim_thrusters_wrench(&th, tau);
		sim_step(&p, &s, tau, SIM_DEFAULT_DT);
		sim_quat_to_euler(s.q, NULL, &pitch, NULL);
		const double top[3] = { 0, 0, -1 };
		double top_w[3];
		sim_quat_rotate(s.q, top, top_w);
		if (top_w[2] > 0)
			r.tumbled = 1;
		if (i < 40 * 200) {
			r.max_pitch = fmax(r.max_pitch, fabs(pitch));
			if (i >= 30 * 200)
				r.late_pitch = fmax(r.late_pitch, fabs(pitch));
			r.speed = s.nu[0];
		}
		r.end_pitch = fabs(pitch);
	}
	return r;
}

static void show(const char *what, float surge, const struct flight *r)
{
	printf("    %-44s surge %.2f: %.2f m/s, |pitch| up to %5.1f deg (%5.1f in the last 10 s), %s, %.2f deg 20 s after\n",
	       what, (double)surge, r->speed, r->max_pitch * DEG, r->late_pitch * DEG,
	       r->tumbled ? "TUMBLED" : "upright", r->end_pitch * DEG);
}

/* The defect as it stood (modules/sim_vehicle/README.md "Left out, and
 * found", 2026-11-20): ArduSub's factors on lib/sim's published defaults. If
 * this ever stops tumbling the model has changed, and lib/sim/README.md
 * "Pitch under surge" with it. */
static void test_surge_before(void)
{
	struct flight r = fly_surge(0.5f, 0, 0);
	show("ArduSub's factors, Wu's damping", 0.5f, &r);
	CHECK(r.tumbled);
	/* The allocation takes the thrusters' own moment away, and with Wu's
	 * damping that is not enough: the speed it reaches is past the model's
	 * Munk speed (lib/sim's suite), and it goes over all the same. */
	r = fly_surge(1.0f, 0, 1);
	show("allocated, Wu's damping", 1.0f, &r);
	CHECK(r.tumbled);
	/* Nor is the measured damping by itself, with the factors' moment. */
	r = fly_surge(0.5f, 1, 0);
	show("ArduSub's factors, von Benzon's damping", 0.5f, &r);
	CHECK(r.tumbled);
}

/* What profiles/sim-direct flies: the allocation, and von Benzon's measured
 * translational damping. Bounded at every surge up to full, upright
 * throughout, level again when the command is let go. The bounds are the
 * model's, with its published 2 cm between the centres of gravity and
 * buoyancy; they are not a vehicle's (lib/sim/README.md says what would
 * settle that). */
static void test_surge_after(void)
{
	static const struct {
		float surge;
		double bound_deg;
	} c[] = { { 0.1f, 2 }, { 0.25f, 5 }, { 0.5f, 15 }, { 1.0f, 40 } };
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		struct flight r = fly_surge(c[i].surge, 1, 1);
		show("allocated, von Benzon's damping", c[i].surge, &r);
		CHECK(!r.tumbled);
		CHECK(r.max_pitch * DEG < c[i].bound_deg);
		CHECK(r.end_pitch * DEG < 0.5);
		CHECK(r.speed > 0.1);
	}
}

/* ---- the whole flight, at a fixed step ---------------------------------------- */

/* profiles/sim-direct without its processes: lib/sim's model with von
 * Benzon's damping and its 2 N of buoyancy, the thrusters' lag, the allocation
 * at 50 Hz, and depth_hold's controller (the profile's gains) on heave, roll,
 * pitch and, while the pilot leaves it alone, yaw, as the mux's assisted mode
 * has it there. No noise and no clock: the same numbers
 * on every run. The suite under a manager (test_sim_direct) flies the
 * processes; this one is where the vehicle's motion is judged. */
struct rig {
	struct thr_alloc a;
	struct sim_params p;
	struct sim_state s;
	struct sim_thrusters th;
	struct dh_ctl dh;
	uint16_t pwm[THR_N];
	long step;
	double yaw_last, yaw_total;            /* rad, unwrapped since the start */
	double max_roll, max_pitch, max_depth_err; /* since rig_mark() */
	double hold;
	float level_kp, level_kd; /* depth_hold's levelling; 0: none */
	double heading;           /* rad, the heading held while the pilot leaves yaw alone */
	int turning;
	int tumbled;
};

static void rig_mark(struct rig *r)
{
	r->max_roll = r->max_pitch = r->max_depth_err = 0;
}

static void rig_init(struct rig *r)
{
	struct sim_frame f;
	sim_frame_bluerov2_heavy(&f);
	memset(r, 0, sizeof *r);
	thr_alloc_init(&r->a, &f, VOLT);
	r->p = sim_params_bluerov2_heavy();
	sim_params_damping_vonbenzon(&r->p);
	sim_state_init(&r->s);
	sim_thrusters_init(&r->th, &f.geometry, 0.05);
	dh_init(&r->dh);
	/* profiles/sim-direct/depth_hold.params. */
	r->level_kp = 0.5f;
	r->level_kd = 0.1f;
	rig_mark(r);
}

/* `seconds` under the pilot's surge, sway and yaw; the heave is the depth
 * controller's. */
static void rig_fly(struct rig *r, double seconds, float surge, float sway, float yaw)
{
	/* msg/depth_control_params.msg's defaults with profiles/sim-direct's
	 * depth_hold.params (accel_max 4, i_band 0.6). */
	static const struct dh_gains G = { .kp = 4, .kd = 4, .accel_max = 4, .ff = 0.1f, .ki = 0.1f, .i_max = 0.3f,
					   .i_band = 0.6f, .max_thrust = 0.6f };
	long n = lrint(seconds * 200);
	for (long i = 0; i < n; i++, r->step++) {
		if (r->step % 4 == 0) {
			double vw[3];
			sim_quat_rotate(r->s.q, r->s.nu, vw);
			struct dh_out o = dh_step(&r->dh, &G, r->s.pos[2], vw[2], 1, 1, 0.02);
			double roll_now, pitch_now;
			sim_quat_to_euler(r->s.q, &roll_now, &pitch_now, NULL);
			const float in[6] = { surge,
					      sway,
					      o.heave,
					      dh_level(r->level_kp, r->level_kd, 0.5f, roll_now, r->s.nu[3]),
					      dh_level(r->level_kp, r->level_kd, 0.5f, pitch_now, r->s.nu[4]),
					      /* The pilot's while he turns; else the heading
					       * he left it on, held (heading_kp 0.5, heading_kd 0.1). */
					      yaw != 0 ? yaw : dh_heading(0.5f, 0.1f, 0.5f, r->heading, r->yaw_total, r->s.nu[5]) };
			if (yaw != 0 || r->turning)
				r->heading = r->yaw_total;
			r->turning = yaw != 0;
			struct thr_alloc_out out;
			thr_alloc_mix(&r->a, in, &out);
			memcpy(r->pwm, out.pwm, sizeof r->pwm);
			sim_thrusters_command(&r->th, r->pwm, VOLT);
		}
		sim_thrusters_step(&r->th, SIM_DEFAULT_DT);
		double tau[6], roll, pitch, yaw_now;
		sim_thrusters_wrench(&r->th, tau);
		sim_step(&r->p, &r->s, tau, SIM_DEFAULT_DT);
		sim_quat_to_euler(r->s.q, &roll, &pitch, &yaw_now);
		double d = yaw_now - r->yaw_last;
		if (d > 3.14159265358979323846)
			d -= 2 * 3.14159265358979323846;
		if (d < -3.14159265358979323846)
			d += 2 * 3.14159265358979323846;
		r->yaw_total += d;
		r->yaw_last = yaw_now;
		r->max_roll = fmax(r->max_roll, fabs(roll));
		r->max_pitch = fmax(r->max_pitch, fabs(pitch));
		r->max_depth_err = fmax(r->max_depth_err, fabs(r->s.pos[2] - r->hold));
		const double top[3] = { 0, 0, -1 };
		double top_w[3];
		sim_quat_rotate(r->s.q, top, top_w);
		if (top_w[2] > 0)
			r->tumbled = 1;
	}
}

/* Why the profile levels. Holding depth and surging at half command, the
 * vehicle is past the speed at which the model's pitch is stable; the depth
 * controller, pushing along a tilted axis, takes it over. With levelling the
 * same flight stays within a degree. */
static void test_levelling_is_needed(void)
{
	static struct rig r;
	for (int level = 0; level < 2; level++) {
		rig_init(&r);
		if (!level)
			r.level_kp = r.level_kd = 0;
		r.hold = 2.0;
		dh_set(&r.dh, r.hold);
		rig_fly(&r, 15, 0, 0, 0);
		rig_mark(&r);
		rig_fly(&r, 30, 0.5f, 0, 0);
		printf("    depth held, surge 0.5 for 30 s, %s: |pitch| up to %.1f deg, depth within %.2f m, %s\n",
		       level ? "levelled" : "not levelled", r.max_pitch * DEG, r.max_depth_err,
		       r.tumbled ? "TUMBLED" : "upright");
		if (level) {
			CHECK(!r.tumbled);
			CHECK(r.max_pitch * DEG < 3);
			CHECK(r.max_depth_err < 0.1);
		} else {
			CHECK(r.max_pitch * DEG > 45);
		}
	}
}

static void test_flight(void)
{
	static struct rig r;
	rig_init(&r);
	/* Down to 2 m and hold. */
	r.hold = 2.0;
	dh_set(&r.dh, r.hold);
	rig_fly(&r, 15, 0, 0, 0);
	printf("    dive: %.3f m after 15 s (target %.1f)\n", r.s.pos[2], r.hold);
	CHECK(fabs(r.s.pos[2] - r.hold) < 0.05);

	/* Surge: half command for 8 s, 8 s of nothing, then the whole of it. */
	double x0 = r.s.pos[0], y0 = r.s.pos[1], roll, pitch;
	static const struct {
		float surge;
		double least_m;
	} leg[2] = { { 0.5f, 3.5 }, { 1.0f, 5.0 } };
	for (int k = 0; k < 2; k++) {
		x0 = r.s.pos[0], y0 = r.s.pos[1];
		rig_mark(&r);
		rig_fly(&r, 8, leg[k].surge, 0, 0);
		printf("    surge %.1f for 8 s: %.2f m forward, %.3f m aside, %.2f m/s, |pitch| up to %.2f deg, depth within %.3f m\n",
		       (double)leg[k].surge, r.s.pos[0] - x0, r.s.pos[1] - y0, r.s.nu[0], r.max_pitch * DEG,
		       r.max_depth_err);
		CHECK(r.s.pos[0] - x0 > leg[k].least_m);
		CHECK(fabs(r.s.pos[1] - y0) < 0.05);
		CHECK(r.max_pitch * DEG < 3);
		CHECK(r.max_depth_err < 0.1);
		rig_fly(&r, 8, 0, 0, 0);
		sim_quat_to_euler(r.s.q, &roll, &pitch, NULL);
		printf("    8 s after: pitch %.2f deg, %.3f m/s\n", pitch * DEG, r.s.nu[0]);
		CHECK(fabs(pitch) * DEG < 0.5 && fabs(r.s.nu[0]) < 0.02);
	}

	/* Sway. */
	x0 = r.s.pos[0], y0 = r.s.pos[1];
	double yaw0 = r.yaw_total;
	rig_mark(&r);
	rig_fly(&r, 8, 0, 0.5f, 0);
	printf("    sway 0.5 for 8 s: %.2f m to the right, %.3f m ahead, |roll| up to %.2f deg, heading %+.1f deg, depth within %.3f m\n",
	       r.s.pos[1] - y0, r.s.pos[0] - x0, r.max_roll * DEG, (r.yaw_total - yaw0) * DEG, r.max_depth_err);
	CHECK(r.s.pos[1] - y0 > 3.0);
	CHECK(fabs(r.s.pos[0] - x0) < 0.2);
	CHECK(r.max_roll * DEG < 3);
	CHECK(fabs(r.yaw_total - yaw0) * DEG < 5);
	CHECK(r.max_depth_err < 0.1);
	rig_fly(&r, 8, 0, 0, 0);

	/* A full turn, clockwise seen from above, and stopped. */
	x0 = r.s.pos[0], y0 = r.s.pos[1];
	yaw0 = r.yaw_total;
	rig_mark(&r);
	double took = 0;
	while (r.yaw_total - yaw0 < 2 * 3.14159265358979323846 && took < 60) {
		rig_fly(&r, 0.1, 0, 0, 0.1f);
		took += 0.1;
	}
	rig_fly(&r, 8, 0, 0, 0);
	sim_quat_to_euler(r.s.q, &roll, &pitch, NULL);
	printf("    yaw 0.1: a full turn in %.1f s, %.0f deg when it had stopped, %.2f m from where it began, depth within %.3f m\n",
	       took, (r.yaw_total - yaw0) * DEG, hypot(r.s.pos[0] - x0, r.s.pos[1] - y0), r.max_depth_err);
	CHECK(took < 15);
	/* Here the heading is held from the moment the pilot lets go, so the
	 * vehicle comes back to where it was then; under the mux it coasts for
	 * override_hold first (test_sim_direct). */
	CHECK((r.yaw_total - yaw0) * DEG > 355 && (r.yaw_total - yaw0) * DEG < 450);
	CHECK(fabs(r.s.nu[5]) < 0.02);
	CHECK(hypot(r.s.pos[0] - x0, r.s.pos[1] - y0) < 0.3);
	CHECK(r.max_depth_err < 0.05);
	printf("    at the end: depth %.3f m, roll %.2f deg, pitch %.2f deg\n", r.s.pos[2], roll * DEG, pitch * DEG);
	CHECK(fabs(r.s.pos[2] - r.hold) < 0.02);
	CHECK(fabs(roll) * DEG < 0.5 && fabs(pitch) * DEG < 0.5);
	CHECK(!r.tumbled);
}

int main(void)
{
	RUN_TEST(test_neutral);
	RUN_TEST(test_axes);
	RUN_TEST(test_heave_monotone_and_saturation);
	RUN_TEST(test_alloc_capacity);
	RUN_TEST(test_alloc_gives_what_is_asked);
	RUN_TEST(test_alloc_agrees_with_the_factors_in_sign);
	RUN_TEST(test_alloc_past_the_limits);
	RUN_TEST(test_alloc_pulse_width);
	RUN_TEST(test_alloc_reversed_thruster);
	RUN_TEST(test_alloc_six_thrusters);
	RUN_TEST(test_alloc_neutral);
	RUN_TEST(test_surge_before);
	RUN_TEST(test_surge_after);
	RUN_TEST(test_levelling_is_needed);
	RUN_TEST(test_flight);
	TEST_MAIN_END();
}
