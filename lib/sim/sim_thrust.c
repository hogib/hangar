// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * T200 thrust table, thruster geometry, allocation, and the first-order
 * lags. See sim_thrust.h and README.md for provenance.
 */
#include "sim_thrust.h"
#include <math.h>
#include <string.h>

/* data/t200_thrust.inc is generated from data/t200_thrust.csv by
 * data/gen_t200_inc.sh; it defines sim_t200_volt_table[] and
 * sim_t200_newton_table[][] (newtons). */
#include "data/t200_thrust.inc"

const double *sim_t200_voltages(void) { return sim_t200_volt_table; }

double sim_t200_table(int row, int volt_index) {
  if (row < 0 || row >= SIM_T200_NROW || volt_index < 0 || volt_index >= SIM_T200_NVOLT)
    return 0.0;
  return sim_t200_newton_table[row][volt_index];
}

double sim_t200_thrust(double pwm_us, double voltage) {
  // NaN pulse widths or voltages (a half-initialized topic) read as neutral
  // rather than propagating into the dynamics.
  if (pwm_us != pwm_us || voltage != voltage)
    return 0.0;
  if (pwm_us <= SIM_T200_PWM_MIN)
    pwm_us = SIM_T200_PWM_MIN;
  if (pwm_us >= SIM_T200_PWM_MAX)
    pwm_us = SIM_T200_PWM_MAX;
  double x = (pwm_us - SIM_T200_PWM_MIN) / SIM_T200_PWM_STEP;
  int r0 = (int)floor(x);
  if (r0 >= SIM_T200_NROW - 1)
    r0 = SIM_T200_NROW - 2;
  double fr = x - r0;

  // The table ends at 10 and 20 V; outside it the nearest column is used
  // rather than extrapolated, since a battery past either end is a fault,
  // not a regime to model.
  if (voltage <= sim_t200_volt_table[0])
    voltage = sim_t200_volt_table[0];
  if (voltage >= sim_t200_volt_table[SIM_T200_NVOLT - 1])
    voltage = sim_t200_volt_table[SIM_T200_NVOLT - 1];
  int v0 = 0;
  while (v0 < SIM_T200_NVOLT - 2 && voltage > sim_t200_volt_table[v0 + 1])
    v0++;
  double fv = (voltage - sim_t200_volt_table[v0]) /
              (sim_t200_volt_table[v0 + 1] - sim_t200_volt_table[v0]);

  double a = sim_t200_newton_table[r0][v0] * (1.0 - fr) + sim_t200_newton_table[r0 + 1][v0] * fr;
  double b = sim_t200_newton_table[r0][v0 + 1] * (1.0 - fr) +
             sim_t200_newton_table[r0 + 1][v0 + 1] * fr;
  return a * (1.0 - fv) + b * fv;
}

/*
 * Geometry of the BlueROV2 heavy configuration, body frame x forward,
 * y right, z down, origin at the CG.
 *
 * Positions: Wu 2018, Table 4.2 "Moment arms of 8 thrusters of BlueROV2
 * Heavy", (lx, ly, lz) in mm, taken from the Blue Robotics heavy retrofit
 * drawings: T1..T4 (156, +-111, 85) and (-156, +-111, 85); T5..T8 (120,
 * +-218, 0) and (-120, +-218, 0). Converted to metres here. Wu's frame
 * origin is the CB, 0.02 m above the CG; the thruster z of 0.085 m is
 * carried over unchanged (a 2 cm question the diagonal model cannot
 * resolve anyway).
 *
 * Directions: the direction a pulse width above 1500 us pushes, from
 * ArduSub's frame definition, which is what thr.out means on the vehicle:
 * ArduPilot libraries/AP_Motors/AP_Motors6DOF.cpp, case
 * SUB_FRAME_VECTORED_6DOF (master, fetched 2026-11-07), motor factors
 * (roll, pitch, yaw, throttle, forward, lateral):
 *   1: (0, 0, +1, 0, -1, +1)   2: (0, 0, -1, 0, -1, -1)
 *   3: (0, 0, -1, 0, +1, +1)   4: (0, 0, +1, 0, +1, -1)
 *   5: (+1, -1, 0, -1, 0, 0)   6: (-1, -1, 0, -1, 0, 0)
 *   7: (+1, +1, 0, -1, 0, 0)   8: (-1, +1, 0, -1, 0, 0)
 * so the horizontal thrusters push outward along the +-45 degree
 * diagonals (forward and lateral factors) and the vertical ones push
 * down (throttle factor -1: positive throttle, up, drives them below
 * 1500 us). The moment columns these produce with the positions above
 * agree in sign with ArduSub's roll, pitch and yaw factors, and in
 * magnitude with Wu's eq. (4.62) (0.707, 0.06, 0.1888, 0.218, 0.120),
 * whose columns 1-4, 5 and 8 carry the opposite sign convention for a
 * positive thrust. The numbering is Blue Robotics' (1 front-right
 * horizontal, 2 front-left, 3 rear-right, 4 rear-left, 5-8 vertical in
 * the same order).
 */
#define S2 0.70710678118654752440 /* sqrt(1/2) */
static const struct sim_thruster_geometry heavy = {
    .pos = {{0.156, 0.111, 0.085},
            {0.156, -0.111, 0.085},
            {-0.156, 0.111, 0.085},
            {-0.156, -0.111, 0.085},
            {0.120, 0.218, 0.0},
            {0.120, -0.218, 0.0},
            {-0.120, 0.218, 0.0},
            {-0.120, -0.218, 0.0}},
    .dir = {{-S2, S2, 0.0},
            {-S2, -S2, 0.0},
            {S2, S2, 0.0},
            {S2, -S2, 0.0},
            {0.0, 0.0, 1.0},
            {0.0, 0.0, 1.0},
            {0.0, 0.0, 1.0},
            {0.0, 0.0, 1.0}},
    .polarity = {1, 1, 1, 1, 1, 1, 1, 1},
};

const struct sim_thruster_geometry *sim_geometry_bluerov2_heavy(void) { return &heavy; }

void sim_alloc_matrix(const struct sim_thruster_geometry *g, double T[6][SIM_NTHRUSTERS]) {
  for (int i = 0; i < SIM_NTHRUSTERS; i++) {
    const double *r = g->pos[i], *d = g->dir[i];
    double s = g->polarity[i];
    T[0][i] = s * d[0];
    T[1][i] = s * d[1];
    T[2][i] = s * d[2];
    T[3][i] = s * (r[1] * d[2] - r[2] * d[1]);
    T[4][i] = s * (r[2] * d[0] - r[0] * d[2]);
    T[5][i] = s * (r[0] * d[1] - r[1] * d[0]);
  }
}

void sim_alloc_wrench(const double *T, const double thrust[SIM_NTHRUSTERS],
                      double tau[6]) {
  for (int a = 0; a < 6; a++) {
    double acc = 0.0;
    for (int i = 0; i < SIM_NTHRUSTERS; i++)
      acc += T[a * SIM_NTHRUSTERS + i] * thrust[i];
    tau[a] = acc;
  }
}

void sim_thrusters_init(struct sim_thrusters *t, const struct sim_thruster_geometry *g,
                        double lag_s) {
  memset(t, 0, sizeof *t);
  t->lag_s = lag_s;
  sim_alloc_matrix(g, t->T);
}

void sim_thrusters_command(struct sim_thrusters *t, const uint16_t pwm[SIM_NTHRUSTERS],
                           double voltage) {
  for (int i = 0; i < SIM_NTHRUSTERS; i++)
    t->target[i] = sim_t200_thrust((double)pwm[i], voltage);
  if (t->lag_s <= 0.0)
    memcpy(t->thrust, t->target, sizeof t->thrust);
}

void sim_thrusters_step(struct sim_thrusters *t, double dt) {
  if (t->lag_s <= 0.0) {
    memcpy(t->thrust, t->target, sizeof t->thrust);
    return;
  }
  // Exact solution of lag * dT/dt = target - T over a step with the target
  // held, so the result does not depend on the step size and a 200 Hz run
  // and a 50 Hz fit see the same filter.
  double a = 1.0 - exp(-dt / t->lag_s);
  for (int i = 0; i < SIM_NTHRUSTERS; i++)
    t->thrust[i] += a * (t->target[i] - t->thrust[i]);
}

void sim_thrusters_wrench(const struct sim_thrusters *t, double tau[6]) {
  sim_alloc_wrench(&t->T[0][0], t->thrust, tau);
}

void sim_applied_wrench(const uint16_t pwm[SIM_NTHRUSTERS], float voltage, float out[6]) {
  // The same path as sim_thrusters_command + sim_thrusters_wrench with no
  // lag, kept in double until the end so the two agree to float rounding.
  double T[6][SIM_NTHRUSTERS], thrust[SIM_NTHRUSTERS], tau[6];
  sim_alloc_matrix(&heavy, T);
  for (int i = 0; i < SIM_NTHRUSTERS; i++)
    thrust[i] = sim_t200_thrust((double)pwm[i], (double)voltage);
  sim_alloc_wrench(&T[0][0], thrust, tau);
  for (int a = 0; a < 6; a++)
    out[a] = (float)tau[a];
}
