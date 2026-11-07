// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
 * sim_thrust: what a pulse width means in newtons, and where it pushes.
 *
 * The T200's bollard thrust against ESC pulse width is a table, not a fit:
 * Blue Robotics' published measurements (data/t200_thrust.csv, 1100 to
 * 1900 us in 4 us steps at 10, 12, 14, 16, 18 and 20 V, with the ESC's
 * deadband around 1500 us as the zero rows), interpolated linearly in
 * pulse width and in voltage. The eight-thruster geometry of the BlueROV2
 * heavy configuration gives a 6x8 allocation matrix T (wrench = T thrust),
 * and sim_applied_wrench() runs the whole path from pulse widths to a
 * body-frame wrench. The same function serves the simulator's command path
 * and the depth-hold label's conversion of `thr.out`, so the two cannot
 * disagree about what a pulse width means.
 */
#include <stdint.h>

#define SIM_NTHRUSTERS 8
#define SIM_T200_PWM_MIN 1100
#define SIM_T200_PWM_MAX 1900
#define SIM_T200_PWM_STEP 4
#define SIM_T200_NROW 201 /* (1900 - 1100) / 4 + 1 */
#define SIM_T200_NVOLT 6
#define SIM_T200_PWM_NEUTRAL 1500

/* Thrust in newtons of one T200 at `pwm_us` and supply `voltage`, signed
 * (negative below 1500 us). Pulse width is clamped to [1100, 1900] and
 * voltage to the tabulated [10, 20] V; between table points both are
 * interpolated linearly. */
double sim_t200_thrust(double pwm_us, double voltage);

/* The tabulated voltages and the table itself (N), for tests and tools. */
const double *sim_t200_voltages(void);                 /* [SIM_T200_NVOLT] */
double sim_t200_table(int row, int volt_index);        /* row 0 = 1100 us */

/* Thruster placement: positions relative to the CG and the unit direction a
 * positive thrust (pulse width above 1500 us) pushes the vehicle, both in
 * the body frame (x forward, y right, z down). `polarity` is +1 or -1 per
 * thruster and mirrors ArduSub's MOT_n_DIRECTION: a vehicle whose thruster
 * n is installed reversed sets polarity[n] = -1 and keeps dir as the frame
 * defines it. */
struct sim_thruster_geometry {
  double pos[SIM_NTHRUSTERS][3];
  double dir[SIM_NTHRUSTERS][3];
  double polarity[SIM_NTHRUSTERS];
};

/* The BlueROV2 heavy configuration (sources in sim_thrust.c). */
const struct sim_thruster_geometry *sim_geometry_bluerov2_heavy(void);

/* T[6][8]: column i is [dir_i ; pos_i x dir_i] * polarity_i. */
void sim_alloc_matrix(const struct sim_thruster_geometry *g, double T[6][SIM_NTHRUSTERS]);

/* tau = T thrust. T is the 6x8 matrix as a flat row-major pointer (&T[0][0]):
 * a const pointer to a 2-D array cannot be formed from a non-const one before
 * C23 without a pedantic warning, a flat pointer can. */
void sim_alloc_wrench(const double *T, const double thrust[SIM_NTHRUSTERS],
                      double tau[6]);

/* Eight first-order lags: each thruster's thrust follows its commanded
 * value with time constant lag_s (tau_lag dT/dt = T_cmd - T, stepped
 * exactly). lag_s <= 0 means no lag: thrust equals the command at once. */
struct sim_thrusters {
  double lag_s;
  double thrust[SIM_NTHRUSTERS]; /* current, N */
  double target[SIM_NTHRUSTERS]; /* commanded, N */
  double T[6][SIM_NTHRUSTERS];
};

void sim_thrusters_init(struct sim_thrusters *t, const struct sim_thruster_geometry *g, double lag_s);
/* Sets the targets from pulse widths and the supply voltage through the table. */
void sim_thrusters_command(struct sim_thrusters *t, const uint16_t pwm[SIM_NTHRUSTERS], double voltage);
/* Advances the lags by dt seconds. */
void sim_thrusters_step(struct sim_thrusters *t, double dt);
/* The body-frame wrench of the current thrusts. */
void sim_thrusters_wrench(const struct sim_thrusters *t, double tau[6]);

/* Pulse widths straight to the body-frame wrench [Fx Fy Fz Mx My Mz] (N,
 * N m) with the heavy configuration's geometry and no lag: the steady state
 * of the command path above, and the depth-hold label's conversion of
 * thr.out. out[2] is the applied heave force. */
void sim_applied_wrench(const uint16_t pwm[SIM_NTHRUSTERS], float voltage, float out[6]);
