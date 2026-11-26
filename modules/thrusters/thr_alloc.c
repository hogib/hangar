// SPDX-License-Identifier: GPL-3.0-or-later
#include "thr_alloc.h"

#include <math.h>
#include <string.h>

/* The wrench message's axes are the body wrench's in the same order, but heave
 * is up positive and the body's z is down. */
static const double SIGN[6] = { 1, 1, -1, 1, 1, 1 };
static const int HOLD[6] = { 0, 0, 1, 1, 1, 0 }; /* heave, roll, pitch */

/* Cyclic Jacobi on a symmetric 6x6: a becomes diagonal (the eigenvalues), v's
 * columns the eigenvectors. Small, exact enough, and the same on every
 * machine. */
static void jacobi6(double a[6][6], double v[6][6])
{
	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 6; j++)
			v[i][j] = i == j;
	for (int sweep = 0; sweep < 64; sweep++) {
		double off = 0;
		for (int i = 0; i < 6; i++)
			for (int j = i + 1; j < 6; j++)
				off += a[i][j] * a[i][j];
		if (off < 1e-30)
			break;
		for (int p = 0; p < 6; p++)
			for (int q = p + 1; q < 6; q++) {
				if (fabs(a[p][q]) < 1e-300)
					continue;
				double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
				double t = (theta >= 0 ? 1 : -1) / (fabs(theta) + sqrt(theta * theta + 1));
				double c = 1 / sqrt(t * t + 1), s = t * c;
				for (int k = 0; k < 6; k++) {
					double akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 6; k++) {
					double apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 6; k++) {
					double vkp = v[k][p], vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
	}
}

/* The largest s in [0, cap] with lo <= base + s d <= hi for every thruster;
 * base is within the limits. */
static double room(const struct thr_alloc *a, const double base[THR_N], const double d[THR_N], double cap)
{
	double s = cap;
	for (int i = 0; i < THR_N; i++) {
		if (d[i] > 1e-12) {
			double m = (a->hi[i] - base[i]) / d[i];
			if (m < s)
				s = m;
		} else if (d[i] < -1e-12) {
			double m = (a->lo[i] - base[i]) / d[i];
			if (m < s)
				s = m;
		}
	}
	return s > 0 ? s : 0;
}

int thr_alloc_init(struct thr_alloc *a, const struct sim_frame *f, double voltage)
{
	memset(a, 0, sizeof *a);
	a->n = f->n;
	a->voltage = voltage;
	sim_alloc_matrix(&f->geometry, a->T);
	for (int i = 0; i < THR_N; i++) {
		/* A thruster the frame does not describe pushes nothing and is
		 * never asked to. */
		int used = i < f->n;
		a->lo[i] = used ? sim_t200_thrust(SIM_T200_PWM_MIN, voltage) : 0;
		a->hi[i] = used ? sim_t200_thrust(SIM_T200_PWM_MAX, voltage) : 0;
		if (!used)
			for (int k = 0; k < 6; k++)
				a->T[k][i] = 0;
	}
	/* T+ = T^T (T T^T)+, the inverse taken through the eigenvectors so that
	 * a frame which cannot produce every axis still gets the least-squares
	 * answer on those it can. */
	double g[6][6], v[6][6];
	for (int r = 0; r < 6; r++)
		for (int c = 0; c < 6; c++) {
			g[r][c] = 0;
			for (int i = 0; i < THR_N; i++)
				g[r][c] += a->T[r][i] * a->T[c][i];
		}
	jacobi6(g, v);
	double top = 0;
	for (int k = 0; k < 6; k++)
		if (g[k][k] > top)
			top = g[k][k];
	if (top <= 0)
		return -1;
	double ginv[6][6] = { { 0 } };
	for (int k = 0; k < 6; k++) {
		if (g[k][k] <= 1e-9 * top)
			continue;
		a->rank++;
		for (int r = 0; r < 6; r++)
			for (int c = 0; c < 6; c++)
				ginv[r][c] += v[r][k] * v[c][k] / g[k][k];
	}
	for (int i = 0; i < THR_N; i++)
		for (int c = 0; c < 6; c++) {
			a->P[i][c] = 0;
			for (int r = 0; r < 6; r++)
				a->P[i][c] += a->T[r][i] * ginv[r][c];
		}
	/* Each axis's capacity: how far its column of T+ can be scaled, the
	 * same both ways, before a thruster's limit. An axis whose column does
	 * not give the axis back is one the frame cannot produce. */
	const double zero[THR_N] = { 0 };
	for (int m = 0; m < 6; m++) {
		int k = m;
		double col[THR_N], neg[THR_N], back = 0;
		for (int i = 0; i < THR_N; i++) {
			col[i] = a->P[i][k];
			neg[i] = -col[i];
			back += a->T[k][i] * col[i];
		}
		if (back < 0.999) {
			a->full[m] = 0;
			continue;
		}
		double up = room(a, zero, col, HUGE_VAL), down = room(a, zero, neg, HUGE_VAL);
		a->full[m] = up < down ? up : down;
		if (!isfinite(a->full[m]))
			a->full[m] = 0;
	}
	return 0;
}

uint16_t thr_alloc_pwm(double newtons, double voltage)
{
	if (!isfinite(newtons) || fabs(newtons) < 1e-3)
		return SIM_T200_PWM_NEUTRAL;
	double lo, hi;
	if (newtons > 0) {
		if (newtons >= sim_t200_thrust(SIM_T200_PWM_MAX, voltage))
			return SIM_T200_PWM_MAX;
		lo = SIM_T200_PWM_NEUTRAL;
		hi = SIM_T200_PWM_MAX;
		/* The table rises with the pulse width above the dead band. */
		for (int it = 0; it < 24; it++) {
			double mid = 0.5 * (lo + hi);
			if (sim_t200_thrust(mid, voltage) < newtons)
				lo = mid;
			else
				hi = mid;
		}
	} else {
		if (newtons <= sim_t200_thrust(SIM_T200_PWM_MIN, voltage))
			return SIM_T200_PWM_MIN;
		lo = SIM_T200_PWM_MIN;
		hi = SIM_T200_PWM_NEUTRAL;
		for (int it = 0; it < 24; it++) {
			double mid = 0.5 * (lo + hi);
			if (sim_t200_thrust(mid, voltage) < newtons)
				lo = mid;
			else
				hi = mid;
		}
	}
	return (uint16_t)lrint(0.5 * (lo + hi));
}

static double clamp1(float v)
{
	return !isfinite(v) ? 0 : v > 1 ? 1 : v < -1 ? -1 : (double)v;
}

void thr_alloc_mix(const struct thr_alloc *a, const float in[6], struct thr_alloc_out *out)
{
	double hold[6] = { 0 }, move[6] = { 0 };
	memset(out, 0, sizeof *out);
	for (int m = 0; m < 6; m++) {
		double w = SIGN[m] * a->full[m] * clamp1(in[m]);
		out->asked[m] = w;
		(HOLD[m] ? hold : move)[m] = w;
	}
	double th[THR_N], tm[THR_N];
	for (int i = 0; i < THR_N; i++) {
		th[i] = tm[i] = 0;
		for (int k = 0; k < 6; k++) {
			th[i] += a->P[i][k] * hold[k];
			tm[i] += a->P[i][k] * move[k];
		}
	}
	const double zero[THR_N] = { 0 };
	out->scale_hold = room(a, zero, th, 1);
	for (int i = 0; i < THR_N; i++)
		th[i] *= out->scale_hold;
	out->scale_move = room(a, th, tm, 1);
	for (int i = 0; i < THR_N; i++) {
		double f = th[i] + out->scale_move * tm[i];
		f = f > a->hi[i] ? a->hi[i] : f < a->lo[i] ? a->lo[i] : f;
		out->pwm[i] = i < a->n ? thr_alloc_pwm(f, a->voltage) : SIM_T200_PWM_NEUTRAL;
		out->thrust[i] = i < a->n ? sim_t200_thrust(out->pwm[i], a->voltage) : 0;
	}
	for (int k = 0; k < 6; k++)
		for (int i = 0; i < THR_N; i++)
			out->given[k] += a->T[k][i] * out->thrust[i];
}
