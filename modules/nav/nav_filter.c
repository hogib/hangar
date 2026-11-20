// SPDX-License-Identifier: GPL-3.0-or-later
/* nav_filter: see nav_filter.h. */
#include "nav_filter.h"

#include <string.h>

void nav_filter_init(struct nav_filter *f, double accel_noise, double bias_walk, double depth_noise)
{
	memset(f, 0, sizeof *f);
	f->accel_noise = accel_noise;
	f->bias_walk = bias_walk;
	f->depth_noise = depth_noise;
}

/* The third row of R (body to NED) from q = w x y z: how each body axis
 * projects on the vertical, down positive. */
static void down_row(const float q[4], float r[3])
{
	float w = q[0], x = q[1], y = q[2], z = q[3];
	r[0] = 2.0f * (x * z - w * y);
	r[1] = 2.0f * (y * z + w * x);
	r[2] = 1.0f - 2.0f * (x * x + y * y);
}

float nav_vertical_accel(const float q[4], const float f_body[3])
{
	float r[3];
	down_row(q, r);
	/* f = R^T (a - g): a_down = (R f)_z + g. At rest and level f = (0, 0, -g). */
	return r[0] * f_body[0] + r[1] * f_body[1] + r[2] * f_body[2] + NAV_G;
}

void nav_body_accel(const float q[4], const float f_body[3], float out[3])
{
	float r[3];
	down_row(q, r);
	/* R^T g, g = (0, 0, g) in NED, is g times the third row of R. */
	for (int i = 0; i < 3; i++)
		out[i] = f_body[i] + NAV_G * r[i];
}

void nav_filter_predict(struct nav_filter *f, double dt, int have_accel, double a_down)
{
	if (!f->started || dt <= 0)
		return;
	double *x = f->x;
	double a = have_accel ? a_down - x[2] : 0.0;
	x[0] += x[1] * dt + 0.5 * a * dt * dt;
	x[1] += a * dt;
	/* F = [1 dt -dt^2/2; 0 1 -dt; 0 0 1] with the IMU (the bias enters the
	 * acceleration), [1 dt 0; 0 1 0; 0 0 1] without. */
	double F[3][3] = { { 1, dt, have_accel ? -0.5 * dt * dt : 0 },
			   { 0, 1, have_accel ? -dt : 0 },
			   { 0, 0, 1 } };
	double FP[3][3], Pn[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double s = 0;
			for (int k = 0; k < 3; k++)
				s += F[i][k] * f->P[k][j];
			FP[i][j] = s;
		}
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double s = 0;
			for (int k = 0; k < 3; k++)
				s += FP[i][k] * F[j][k];
			Pn[i][j] = s;
		}
	/* Process noise: white acceleration through G = (dt^2/2, dt, 0), and the
	 * bias's random walk. */
	double G[3] = { 0.5 * dt * dt, dt, 0 };
	double qa = f->accel_noise * f->accel_noise;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			Pn[i][j] += qa * G[i] * G[j];
	if (have_accel)
		Pn[2][2] += f->bias_walk * f->bias_walk * dt;
	memcpy(f->P, Pn, sizeof Pn);
}

void nav_filter_depth(struct nav_filter *f, double z)
{
	double r = f->depth_noise * f->depth_noise;
	if (!f->started) {
		f->x[0] = z;
		f->x[1] = 0;
		f->x[2] = 0;
		memset(f->P, 0, sizeof f->P);
		f->P[0][0] = r;
		f->P[1][1] = 1.0;  /* (m/s)^2: not known yet */
		f->P[2][2] = 0.25; /* (m/s^2)^2 */
		f->started = 1;
		return;
	}
	/* H = (1 0 0). */
	double S = f->P[0][0] + r;
	if (S <= 0)
		return;
	double K[3] = { f->P[0][0] / S, f->P[1][0] / S, f->P[2][0] / S };
	double y = z - f->x[0];
	for (int i = 0; i < 3; i++)
		f->x[i] += K[i] * y;
	double P0[3] = { f->P[0][0], f->P[0][1], f->P[0][2] };
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			f->P[i][j] -= K[i] * P0[j];
	/* Kept symmetric against rounding. */
	for (int i = 0; i < 3; i++)
		for (int j = i + 1; j < 3; j++) {
			double m = 0.5 * (f->P[i][j] + f->P[j][i]);
			f->P[i][j] = f->P[j][i] = m;
		}
}
