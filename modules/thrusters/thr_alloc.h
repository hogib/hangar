// SPDX-License-Identifier: GPL-3.0-or-later
/* The thrusters' allocation from a frame (lib/sim/sim_frame.h): a normalized
 * wrench to pulse widths through the frame's own geometry, so that the wrench
 * asked for is the wrench applied, with no ArduSub in the chain.
 *
 *   wrench = T thrust           T from the frame, as the simulator builds it
 *   thrust = T+ wrench          the pseudo-inverse: the smallest thrusts that do it
 *   pulse width from thrust     the T200's table at the supply voltage, inverted
 *
 * A command of 1 on an axis is that axis's capacity: the largest wrench on
 * that axis alone, the same both ways, that no thruster's limit cuts short at
 * the voltage given. An axis the frame cannot produce has capacity 0 and its
 * command is ignored.
 *
 * Past the limits (README "Saturation"): heave, roll and pitch are served
 * first, scaled together if they alone are too much; surge, sway and yaw get
 * what is left, scaled together. Each group keeps its direction; holding
 * depth and attitude comes before moving. */
#ifndef THR_ALLOC_H
#define THR_ALLOC_H

#include "sim_frame.h"
#include "thr_mix.h"

struct thr_alloc {
	int n;                /* thrusters the frame describes */
	int rank;             /* of T: 6 when every axis can be produced */
	double voltage;       /* V, the supply the table is read at */
	double T[6][THR_N];   /* wrench (Fx Fy Fz Mx My Mz, body) = T thrust */
	double P[THR_N][6];   /* thrust = P wrench */
	double lo[THR_N];     /* N, each thruster's thrust at 1100 us (negative) */
	double hi[THR_N];     /* N, at 1900 us */
	double full[6];       /* N, N m: the wrench of a command of 1, in the wrench message's
	                         order (surge sway heave roll pitch yaw); 0: not attainable */
};

struct thr_alloc_out {
	uint16_t pwm[THR_N];
	double thrust[THR_N]; /* N, what the table gives at pwm[] */
	double asked[6];      /* body wrench asked for: Fx Fy Fz Mx My Mz, N and N m */
	double given[6];      /* body wrench of thrust[] */
	double scale_hold;    /* 1, or what heave, roll and pitch were scaled by */
	double scale_move;    /* 1, or what surge, sway and yaw were scaled by */
};

/* 0, or -1 when the frame has no thruster with a direction. */
int thr_alloc_init(struct thr_alloc *a, const struct sim_frame *f, double voltage);

/* in: as thr_mix (surge, sway, heave up, roll, pitch, yaw clockwise from
 * above; clamped to -1..1, non-finite is 0). */
void thr_alloc_mix(const struct thr_alloc *a, const float in[6], struct thr_alloc_out *out);

/* The pulse width whose thrust at `voltage` is nearest below-or-at `newtons`
 * in size: 1500 inside the dead band, 1100 or 1900 past the table's ends. */
uint16_t thr_alloc_pwm(double newtons, double voltage);

#endif
