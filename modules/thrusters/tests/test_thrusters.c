// SPDX-License-Identifier: GPL-3.0-or-later
/* The thrusters' mixing against lib/sim's thrusters (sim_applied_wrench, the
 * simulator's own command path): each axis alone pushes the vehicle the way
 * the wrench says, and nowhere else to speak of; a saturated mix scales down. */
#include "test_harness.h"
#include "thr_mix.h"
#include "sim_thrust.h"

#include <math.h>
#include <stdio.h>

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

int main(void)
{
	RUN_TEST(test_neutral);
	RUN_TEST(test_axes);
	RUN_TEST(test_heave_monotone_and_saturation);
	TEST_MAIN_END();
}
