// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
 * sim_fit: the least-squares fit behind `hangar sim fit` (doc 14,
 * "Calibration"). From a recorded dive -- attitude and body velocity over
 * time with the pulse widths and the supply voltage that drove it -- it fits
 * buoyancy, the linear and quadratic damping of each axis and the thruster
 * lag, keeping every other coefficient of the baseline parameters (mass,
 * inertia, added mass, geometry) fixed.
 *
 * Method. Per axis i the model reads
 *
 *   M_ii nu_dot_i + C_i(nu) + g_i(q; W, B) + d_lin_i nu_i + d_quad_i |nu_i| nu_i = tau_i(lag)
 *
 * where nu_dot comes from central differences of the recorded velocity and
 * tau from the recorded pulse widths through the thrust table, the lag and
 * the allocation. Everything unknown except the lag enters linearly: for a
 * given lag the 13 unknowns (6 d_lin, 6 d_quad, B) solve one joint linear
 * least-squares problem over all axes and samples by the normal equations,
 * with a small ridge towards the baseline so that an axis the dive never
 * excited keeps its baseline value instead of going singular. The lag is
 * then the minimizer of the summed squared residual over [0, lag_max],
 * found by golden-section search. No external solver; no allocation.
 */
#include "sim_model.h"
#include "sim_thrust.h"
#include <stddef.h>
#include <stdint.h>

struct sim_fit_sample {
  double t;                        /* s, strictly increasing */
  double q[4];                     /* attitude, world <- body */
  double nu[6];                    /* body velocity */
  uint16_t pwm[SIM_NTHRUSTERS];    /* thr.out at t */
  double voltage;                  /* power.battery at t */
};

struct sim_fit_options {
  double lag_max_s;   /* search range for the lag; <= 0 selects 1.0 s */
  double ridge;       /* relative ridge weight; <= 0 selects 1e-6 */
  int fit_lag;        /* 0: keep the baseline lag, only the linear fit */
};

struct sim_fit_result {
  double buoyancy;          /* N */
  double d_lin[6];
  double d_quad[6];
  double lag_s;
  double rms_residual[6];   /* per axis, N or N m: the model's unexplained wrench */
  double rms_velocity[6];   /* per axis, for scale */
  size_t samples_used;      /* n - 2 (the end points have no central difference) */
};

/* Returns 0 on success, -1 when there are fewer than 3 samples or the normal
 * equations could not be solved. `base` supplies the fixed coefficients and
 * the starting values for the fitted ones; `geom` the thruster geometry. */
int sim_fit(const struct sim_params *base, const struct sim_thruster_geometry *geom,
            const struct sim_fit_sample *samples, size_t n, const struct sim_fit_options *opt,
            struct sim_fit_result *out);

/* The summed squared residual over all axes for one lag value: the function
 * the lag search minimizes, exposed for tests and the report. */
double sim_fit_sse(const struct sim_params *base, const struct sim_thruster_geometry *geom,
                   const struct sim_fit_sample *samples, size_t n, double lag_s, double ridge,
                   struct sim_fit_result *out);
