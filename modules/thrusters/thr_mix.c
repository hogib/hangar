// SPDX-License-Identifier: GPL-3.0-or-later
#include "thr_mix.h"

#include <math.h>

/* ArduPilot's motor factors for the vectored 6DOF frame, per output: roll,
 * pitch, yaw, throttle, forward, lateral (lib/sim/README.md). */
static const float F[THR_N][6] = {
	{ 0, 0, +1, 0, -1, +1 }, { 0, 0, -1, 0, -1, -1 }, { 0, 0, -1, 0, +1, +1 }, { 0, 0, +1, 0, +1, -1 },
	{ +1, -1, 0, -1, 0, 0 }, { -1, -1, 0, -1, 0, 0 }, { +1, +1, 0, -1, 0, 0 }, { -1, +1, 0, -1, 0, 0 },
};

static float clamp1(float v)
{
	return !isfinite(v) ? 0 : v > 1 ? 1 : v < -1 ? -1 : v;
}

float thr_mix(const float in[6], uint16_t pwm[THR_N])
{
	/* The wrench's order (surge sway heave roll pitch yaw) to the factors'. */
	const float u[6] = { clamp1(in[3]), clamp1(in[4]), clamp1(in[5]), clamp1(in[2]), clamp1(in[0]), clamp1(in[1]) };
	float out[THR_N], m = 0;
	for (int i = 0; i < THR_N; i++) {
		out[i] = 0;
		for (int j = 0; j < 6; j++)
			out[i] += F[i][j] * u[j];
		if (fabsf(out[i]) > m)
			m = fabsf(out[i]);
	}
	float scale = m > 1 ? 1 / m : 1;
	for (int i = 0; i < THR_N; i++)
		pwm[i] = (uint16_t)lrintf(1500.0f + 400.0f * out[i] * scale);
	return scale;
}
