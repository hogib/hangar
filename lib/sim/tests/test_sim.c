// SPDX-License-Identifier: GPL-3.0-or-later
// The physics tests of docs/14-simulation.md ("Tests"), against hand-computed
// cases: each one pins a property the model must have, not a particular
// trajectory, so a retuned coefficient does not break the suite but a wrong
// sign or a lost conservation law does.
#include "sim_fit.h"
#include "sim_model.h"
#include "sim_noise.h"
#include "sim_thrust.h"
#include "test_harness.h"
#include <math.h>
#include <stdlib.h>

#define DT SIM_DEFAULT_DT
#define SIM_PI 3.14159265358979323846 /* M_PI is POSIX, not C11 */
static const double ZERO6[6] = {0, 0, 0, 0, 0, 0};

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static void run(const struct sim_params *p, struct sim_state *s, const double tau[6],
                double seconds, double dt) {
  long n = (long)(seconds / dt + 0.5);
  for (long i = 0; i < n; i++)
    sim_step(p, s, tau, dt);
}

/* ---- quaternions ----------------------------------------------------------- */

static void test_quaternion_round_trip(void) {
  double q[4], r, pch, y;
  sim_quat_from_euler(0.3, -0.2, 1.1, q);
  sim_quat_to_euler(q, &r, &pch, &y);
  CHECK(near(r, 0.3, 1e-12));
  CHECK(near(pch, -0.2, 1e-12));
  CHECK(near(y, 1.1, 1e-12));
  // Rotating a vector out and back is the identity.
  double v[3] = {1.0, -2.0, 0.5}, w[3], u[3];
  sim_quat_rotate(q, v, w);
  sim_quat_rotate_inv(q, w, u);
  for (int i = 0; i < 3; i++)
    CHECK(near(u[i], v[i], 1e-12));
  // Roll of +90 degrees takes the body y axis (right) to world down.
  sim_quat_from_euler((SIM_PI / 2), 0, 0, q);
  double ey[3] = {0, 1, 0};
  sim_quat_rotate(q, ey, w);
  CHECK(near(w[0], 0, 1e-12) && near(w[1], 0, 1e-12) && near(w[2], 1, 1e-12));
}

/* ---- buoyancy and the limits ------------------------------------------------- */

// Neutral buoyancy holds depth: a vehicle with W = B at rest stays put, to
// rounding, through a minute of stepping.
static void test_neutral_buoyancy_holds_depth(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  p.buoyancy = p.weight;
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 5.0;
  run(&p, &s, ZERO6, 60.0, DT);
  CHECK(near(s.pos[2], 5.0, 1e-9));
  for (int i = 0; i < 6; i++)
    CHECK(near(s.nu[i], 0.0, 1e-12));
  CHECK(near(s.t, 60.0, 1e-6));
}

// Positive buoyancy brings the vehicle to the surface, where depth stops at
// 0 and the vertical velocity is zeroed; negative buoyancy takes it to the
// floor.
static void test_surface_and_floor_limits(void) {
  struct sim_params p = sim_params_bluerov2_heavy(); // B > W as published
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 2.0;
  run(&p, &s, ZERO6, 60.0, DT);
  CHECK(s.pos[2] == 0.0);
  CHECK(near(s.nu[2], 0.0, 1e-12));

  p.buoyancy = p.weight - 2.0;
  p.floor_depth = 10.0;
  sim_state_init(&s);
  s.pos[2] = 8.0;
  run(&p, &s, ZERO6, 60.0, DT);
  CHECK(s.pos[2] == 10.0);
  CHECK(near(s.nu[2], 0.0, 1e-12));
}

/* ---- terminal velocity ------------------------------------------------------- */

// A constant surge force F reaches the speed where damping balances it,
// d_lin u + d_quad u^2 = F; the analytic root is the check.
static void test_terminal_velocity_matches_damping(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  p.buoyancy = p.weight;
  const double F = 20.0;
  double dl = p.d_lin[0], dq = p.d_quad[0];
  double u_term = (-dl + sqrt(dl * dl + 4.0 * dq * F)) / (2.0 * dq);
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 10.0;
  double tau[6] = {F, 0, 0, 0, 0, 0};
  run(&p, &s, tau, 60.0, DT);
  CHECK(near(s.nu[0], u_term, 1e-9 * u_term));
  // No other axis moved: the force passes through the CG, upright.
  for (int i = 1; i < 6; i++)
    CHECK(near(s.nu[i], 0.0, 1e-12));
  CHECK(near(s.pos[2], 10.0, 1e-9));

  // Same in heave, where the test is against the vehicle's own damping axis
  // and the restoring force is zero at W = B.
  double dlw = p.d_lin[2], dqw = p.d_quad[2];
  double w_term = (-dlw + sqrt(dlw * dlw + 4.0 * dqw * F)) / (2.0 * dqw);
  sim_state_init(&s);
  s.pos[2] = 10.0;
  p.floor_depth = 0.0;
  double tauz[6] = {0, 0, F, 0, 0, 0};
  run(&p, &s, tauz, 60.0, DT);
  CHECK(near(s.nu[2], w_term, 1e-9 * w_term));
}

// A uniform current moves a drifting vehicle with the water: it ends at rest
// relative to the water, i.e. moving at the current's speed. The current is
// along the body's x axis: a current with two components would exercise the
// Munk moment (v_r x A1 v_r, from the anisotropic added mass), yaw the body,
// and make the body-frame velocity a rotated copy of the current. The
// relative velocity decays with the linear damping's time constant,
// (m + A)/d_lin = 4.2 s, so two minutes brings it below 1e-12.
static void test_current_carries_vehicle(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  p.buoyancy = p.weight;
  p.current[0] = 0.3;
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 10.0;
  run(&p, &s, ZERO6, 120.0, DT);
  CHECK(near(s.nu[0], 0.3, 1e-9));
  for (int i = 1; i < 6; i++)
    CHECK(near(s.nu[i], 0.0, 1e-12));
  CHECK(near(s.pos[0], 0.3 * 120.0, 2.0)); // minus the spin-up distance
  CHECK(near(s.pos[2], 10.0, 1e-9));
}

/* ---- restoring moment ---------------------------------------------------------- */

// The buoyancy above the CG rights a tilted vehicle: the initial angular
// acceleration opposes the tilt, and the tilt decays to nothing.
static void test_restoring_moment_rights_vehicle(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  p.buoyancy = p.weight;
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 10.0;
  sim_quat_from_euler(30.0 * SIM_PI / 180.0, 20.0 * SIM_PI / 180.0, 0.0, s.q);
  double nu_dot[6];
  sim_model_accel(&p, &s, ZERO6, nu_dot);
  CHECK(nu_dot[3] < 0.0); // rolled right: moment rolls it left
  CHECK(nu_dot[4] < 0.0); // nose up: moment pitches it down
  // The restoring stiffness is B * BG: at a small roll a, moment = -B BG a.
  struct sim_state s2;
  sim_state_init(&s2);
  s2.pos[2] = 10.0;
  sim_quat_from_euler(0.01, 0.0, 0.0, s2.q);
  sim_model_accel(&p, &s2, ZERO6, nu_dot);
  double expected = -p.buoyancy * 0.02 * sin(0.01) / (p.inertia[0] + p.added_mass[3]);
  CHECK(near(nu_dot[3], expected, 1e-9));

  run(&p, &s, ZERO6, 90.0, DT);
  double r, pch;
  sim_quat_to_euler(s.q, &r, &pch, NULL);
  CHECK(fabs(r) < 0.5 * SIM_PI / 180.0);
  CHECK(fabs(pch) < 0.5 * SIM_PI / 180.0);
  CHECK(fabs(s.nu[3]) < 1e-3 && fabs(s.nu[4]) < 1e-3);
}

/* ---- energy ----------------------------------------------------------------- */

// With no thrust in still water the only non-conservative term is damping,
// so the total energy must never rise. Coriolis terms are skew-symmetric
// and the restoring force is a potential; a sign slip in either shows up
// here as an energy gain.
static void test_energy_decays_monotonically(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  p.buoyancy = p.weight; // keep it off the surface for the whole run
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 20.0;
  sim_quat_from_euler(0.6, -0.4, 0.2, s.q);
  double nu0[6] = {0.8, -0.5, 0.3, 1.5, -1.0, 2.0};
  memcpy(s.nu, nu0, sizeof nu0);
  double e_prev = sim_energy(&p, &s), e0 = e_prev;
  int violations = 0;
  for (int i = 0; i < 200 * 30; i++) {
    sim_step(&p, &s, ZERO6, DT);
    double e = sim_energy(&p, &s);
    if (e > e_prev + 1e-9 * fabs(e0))
      violations++;
    e_prev = e;
  }
  CHECK_EQ_INT(violations, 0);
  // And it did decay: the kinetic energy and the tilt are essentially gone,
  // leaving the potential minimum (B * cb_z, negative).
  double e_min = p.buoyancy * p.cb[2];
  CHECK(near(e_prev, e_min, 1e-3 * fabs(e_min)));
  CHECK(e0 > e_prev);
}

/* ---- RK4 order ------------------------------------------------------------------- */

static double state_distance(const struct sim_state *a, const struct sim_state *b) {
  double d = 0.0;
  for (int i = 0; i < 3; i++)
    d += (a->pos[i] - b->pos[i]) * (a->pos[i] - b->pos[i]);
  for (int i = 0; i < 4; i++)
    d += (a->q[i] - b->q[i]) * (a->q[i] - b->q[i]);
  for (int i = 0; i < 6; i++)
    d += (a->nu[i] - b->nu[i]) * (a->nu[i] - b->nu[i]);
  return sqrt(d);
}

// Halving the step divides the error against a fine reference by about
// 2^4. The ratio drifts from 16 as the fine reference's own error enters,
// so the accepted band is wide but excludes any lower order (8 for RK3, 4
// for a midpoint method, 2 for Euler).
static void test_rk4_convergence_order(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  p.buoyancy = p.weight + 1.0;
  double tau[6] = {12.0, -4.0, 6.0, 0.4, -0.3, 0.5};
  struct sim_state ref, a, b;
  sim_state_init(&ref);
  ref.pos[2] = 30.0;
  sim_quat_from_euler(0.4, 0.3, -0.5, ref.q);
  double nu0[6] = {0.5, 0.2, -0.3, 0.7, -0.6, 0.9};
  memcpy(ref.nu, nu0, sizeof nu0);
  a = ref;
  b = ref;
  const double T = 2.0;
  run(&p, &ref, tau, T, 1.0 / 6400.0);
  run(&p, &a, tau, T, 1.0 / 200.0);
  run(&p, &b, tau, T, 1.0 / 400.0);
  double ea = state_distance(&a, &ref), eb = state_distance(&b, &ref);
  CHECK(ea > 0.0 && eb > 0.0);
  double ratio = ea / eb;
  CHECK(ratio > 11.0 && ratio < 22.0);
  if (!(ratio > 11.0 && ratio < 22.0))
    printf("    error at 200 Hz %.3e, at 400 Hz %.3e, ratio %.2f\n", ea, eb, ratio);
}

/* ---- thrust table and allocation ------------------------------------------------- */

// The C table is generated from the CSV; this reads the CSV back and checks
// every entry (kgf * 9.80665) so the two cannot drift apart.
static void test_t200_inc_matches_csv(void) {
  FILE *f = fopen("lib/sim/data/t200_thrust.csv", "r");
  if (!f) {
    printf("    cannot open lib/sim/data/t200_thrust.csv (run from the repository root)\n");
    CHECK(f != NULL);
    return;
  }
  char line[512];
  int rows = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == 'p')
      continue;
    char *end;
    long pwm = strtol(line, &end, 10);
    CHECK_EQ_INT(pwm, SIM_T200_PWM_MIN + rows * SIM_T200_PWM_STEP);
    for (int v = 0; v < SIM_T200_NVOLT; v++) {
      CHECK(*end == ',');
      double kgf = strtod(end + 1, &end);
      double n = kgf * SIM_G_N;
      CHECK(near(sim_t200_table(rows, v), n, 1e-7));
    }
    rows++;
  }
  fclose(f);
  CHECK_EQ_INT(rows, SIM_T200_NROW);
  const double *volts = sim_t200_voltages();
  CHECK(volts[1] == 12.0 && volts[2] == 14.0 && volts[3] == 16.0);
}

static void test_t200_lookup(void) {
  // The deadband: zero at 1500 and for some width either side, at every
  // tabulated voltage; sign follows the side.
  for (int v = 10; v <= 20; v += 2) {
    CHECK(sim_t200_thrust(1500, v) == 0.0);
    CHECK(sim_t200_thrust(1476, v) == 0.0);
    CHECK(sim_t200_thrust(1524, v) == 0.0);
    CHECK(sim_t200_thrust(1900, v) > 0.0);
    CHECK(sim_t200_thrust(1100, v) < 0.0);
    // Reverse is weaker than forward on a T200.
    CHECK(fabs(sim_t200_thrust(1100, v)) < sim_t200_thrust(1900, v));
  }
  // Published full-throttle figures: 3.71 kgf at 12 V, 5.25 kgf at 16 V.
  CHECK(near(sim_t200_thrust(1900, 12.0) / SIM_G_N, 3.714, 0.01));
  CHECK(near(sim_t200_thrust(1900, 16.0) / SIM_G_N, 5.245, 0.01));
  // Interpolation: midway in pulse width and voltage is the mean of the
  // four corners.
  double c00 = sim_t200_thrust(1700, 12.0), c10 = sim_t200_thrust(1704, 12.0);
  double c01 = sim_t200_thrust(1700, 14.0), c11 = sim_t200_thrust(1704, 14.0);
  CHECK(near(sim_t200_thrust(1702, 13.0), 0.25 * (c00 + c10 + c01 + c11), 1e-12));
  // Clamping outside the table.
  CHECK(sim_t200_thrust(2000, 12.0) == sim_t200_thrust(1900, 12.0));
  CHECK(sim_t200_thrust(1000, 12.0) == sim_t200_thrust(1100, 12.0));
  CHECK(sim_t200_thrust(1900, 25.0) == sim_t200_thrust(1900, 20.0));
  CHECK(sim_t200_thrust(1900, 5.0) == sim_t200_thrust(1900, 10.0));
  // Voltage matters: more volts, more thrust at full throttle.
  CHECK(sim_t200_thrust(1900, 16.0) > sim_t200_thrust(1900, 12.0));
}

// The allocation matrix reproduces the moment arms of Wu's eq. (4.62) in
// magnitude, and the signs of ArduSub's VECTORED_6DOF factors.
static void test_allocation_matrix(void) {
  double T[6][SIM_NTHRUSTERS];
  sim_alloc_matrix(sim_geometry_bluerov2_heavy(), T);
  const double s = sqrt(0.5);
  // Magnitudes from Wu (4.62), rounded as printed there.
  for (int i = 0; i < 4; i++) {
    CHECK(near(fabs(T[0][i]), 0.707, 1e-3));
    CHECK(near(fabs(T[1][i]), 0.707, 1e-3));
    CHECK(T[2][i] == 0.0);
    CHECK(near(fabs(T[3][i]), 0.06, 1e-3));
    CHECK(near(fabs(T[4][i]), 0.06, 1e-3));
    CHECK(near(fabs(T[5][i]), 0.1888, 1e-4));
  }
  for (int i = 4; i < 8; i++) {
    CHECK(T[0][i] == 0.0 && T[1][i] == 0.0);
    CHECK(near(fabs(T[2][i]), 1.0, 1e-12));
    CHECK(near(fabs(T[3][i]), 0.218, 1e-12));
    CHECK(near(fabs(T[4][i]), 0.120, 1e-12));
    CHECK(T[5][i] == 0.0);
  }
  // Signs: ArduSub factors (forward, lateral, yaw) for 1..4 and (throttle,
  // roll, pitch) for 5..8.
  const double fwd[4] = {-1, -1, 1, 1}, lat[4] = {1, -1, 1, -1}, yaw[4] = {1, -1, -1, 1};
  for (int i = 0; i < 4; i++) {
    CHECK(near(T[0][i], fwd[i] * s, 1e-12));
    CHECK(near(T[1][i], lat[i] * s, 1e-12));
    CHECK(T[5][i] * yaw[i] > 0.0);
  }
  const double roll[4] = {1, -1, 1, -1}, pitch[4] = {-1, -1, 1, 1};
  for (int i = 4; i < 8; i++) {
    CHECK(T[2][i] == 1.0); // throttle factor -1: positive pulse pushes down
    CHECK(T[3][i] * roll[i - 4] > 0.0);
    CHECK(T[4][i] * pitch[i - 4] > 0.0);
  }
  // All four horizontals forward (1 and 2 reversed) give surge, no sway, no
  // roll, no yaw, and -- because the horizontal thrusters sit 0.085 m
  // below the CG (Wu Table 4.2) -- a nose-up pitching moment of
  // 4 x 10 N x 0.707 x 0.085 m. ArduSub's mixer ignores that coupling; the
  // physics does not, and it is real on the vehicle.
  double thrust[8] = {-10, -10, 10, 10, 0, 0, 0, 0}, tau[6];
  sim_alloc_wrench(&T[0][0], thrust, tau);
  CHECK(near(tau[0], 40.0 * s, 1e-9));
  CHECK(near(tau[1], 0.0, 1e-9));
  CHECK(near(tau[2], 0.0, 1e-9));
  CHECK(near(tau[3], 0.0, 1e-9));
  CHECK(near(tau[4], 40.0 * s * 0.085, 1e-9));
  CHECK(near(tau[5], 0.0, 1e-9));
  // All four verticals equal give pure heave.
  double up[8] = {0, 0, 0, 0, -5, -5, -5, -5};
  sim_alloc_wrench(&T[0][0], up, tau);
  CHECK(near(tau[2], -20.0, 1e-9));
  for (int a = 0; a < 6; a++)
    if (a != 2)
      CHECK(near(tau[a], 0.0, 1e-9));
}

// Doc 14: the same pulse width gives the same force in sim_vehicle's
// command path and in the depth-hold label's conversion of thr.out, over the
// whole table. Each thruster alone at every pulse width and several
// voltages, both paths, to float rounding (sim_applied_wrench returns
// floats).
static void test_applied_wrench_equals_command_path(void) {
  const double volts[] = {12.0, 14.0, 16.0, 11.0, 13.3, 17.9};
  struct sim_thrusters th;
  sim_thrusters_init(&th, sim_geometry_bluerov2_heavy(), 0.0);
  long compared = 0, bad = 0;
  for (size_t vi = 0; vi < sizeof volts / sizeof volts[0]; vi++) {
    for (int i = 0; i < SIM_NTHRUSTERS; i++) {
      for (int pwm = SIM_T200_PWM_MIN; pwm <= SIM_T200_PWM_MAX; pwm++) {
        uint16_t cmd[8] = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
        cmd[i] = (uint16_t)pwm;
        float label[6];
        sim_applied_wrench(cmd, (float)volts[vi], label);
        double tau[6];
        sim_thrusters_command(&th, cmd, volts[vi]);
        sim_thrusters_step(&th, DT);
        sim_thrusters_wrench(&th, tau);
        for (int a = 0; a < 6; a++) {
          compared++;
          double scale = fabs(tau[a]) > 1.0 ? fabs(tau[a]) : 1.0;
          if (fabs((double)label[a] - tau[a]) > 1e-6 * scale)
            bad++;
        }
      }
    }
  }
  CHECK_EQ_INT(bad, 0);
  CHECK(compared > 200000);
  // With a lag, the command path converges to the same steady state.
  sim_thrusters_init(&th, sim_geometry_bluerov2_heavy(), 0.1);
  uint16_t cmd[8] = {1700, 1300, 1650, 1500, 1800, 1200, 1550, 1450};
  sim_thrusters_command(&th, cmd, 14.0);
  for (int k = 0; k < 200 * 5; k++)
    sim_thrusters_step(&th, DT);
  double tau[6];
  float label[6];
  sim_thrusters_wrench(&th, tau);
  sim_applied_wrench(cmd, 14.0f, label);
  for (int a = 0; a < 6; a++)
    CHECK(near(tau[a], (double)label[a], 1e-5));
}

// The lag is an exact first-order response: after one time constant the
// thrust has covered 1 - 1/e of the way, whatever the step size.
static void test_thruster_lag(void) {
  struct sim_thrusters a, b;
  sim_thrusters_init(&a, sim_geometry_bluerov2_heavy(), 0.2);
  sim_thrusters_init(&b, sim_geometry_bluerov2_heavy(), 0.2);
  uint16_t cmd[8] = {1800, 1800, 1800, 1800, 1800, 1800, 1800, 1800};
  sim_thrusters_command(&a, cmd, 16.0);
  sim_thrusters_command(&b, cmd, 16.0);
  for (int k = 0; k < 40; k++)
    sim_thrusters_step(&a, 0.005);
  sim_thrusters_step(&b, 0.2);
  double full = sim_t200_thrust(1800, 16.0);
  CHECK(near(a.thrust[0], full * (1.0 - exp(-1.0)), 1e-9));
  CHECK(near(b.thrust[0], a.thrust[0], 1e-9));
}

/* ---- noise and determinism ---------------------------------------------------------- */

static void test_rng_and_channels(void) {
  struct sim_rng r1, r2;
  sim_rng_seed(&r1, 7);
  sim_rng_seed(&r2, 7);
  for (int i = 0; i < 1000; i++)
    CHECK(sim_rng_u64(&r1) == sim_rng_u64(&r2));
  // Uniform in [0, 1) with a mean near 1/2; Gaussian with sd near 1.
  double su = 0.0, sg = 0.0, sg2 = 0.0;
  const int N = 200000;
  for (int i = 0; i < N; i++) {
    double u = sim_rng_uniform(&r1);
    CHECK(u >= 0.0 && u < 1.0);
    su += u;
    double g = sim_rng_gauss(&r1);
    sg += g;
    sg2 += g * g;
  }
  CHECK(near(su / N, 0.5, 0.005));
  CHECK(near(sg / N, 0.0, 0.01));
  CHECK(near(sqrt(sg2 / N), 1.0, 0.01));
  // Seed 0 is not the fixed point.
  sim_rng_seed(&r1, 0);
  CHECK(sim_rng_u64(&r1) != 0);

  struct sim_channel c = {0.0, 0.25, 0.0};
  CHECK(sim_channel_apply(&c, 1.0, &r1) == 1.25);
  c.noise_sd = 0.1;
  double acc = 0.0;
  for (int i = 0; i < 20000; i++)
    acc += sim_channel_apply(&c, 1.0, &r1);
  CHECK(near(acc / 20000, 1.25, 0.005));
}

static void test_delay_line(void) {
  struct sim_delay d;
  sim_delay_init(&d);
  double out[3];
  sim_delay_read(&d, 0.1, DT, out);
  CHECK(out[0] == 0.0);
  for (int i = 0; i < 100; i++) {
    double v[3] = {i, 2.0 * i, 3.0 * i};
    sim_delay_push(&d, v);
  }
  sim_delay_read(&d, 0.0, DT, out);
  CHECK(out[0] == 99.0);
  sim_delay_read(&d, 0.05, DT, out); // 10 samples back
  CHECK(out[0] == 89.0 && out[1] == 178.0);
  sim_delay_read(&d, 10.0, DT, out); // longer than held: the oldest
  CHECK(out[0] == 0.0);
  // Past the capacity it wraps and keeps the most recent SIM_DELAY_MAX.
  for (int i = 100; i < 1000; i++) {
    double v[3] = {i, 0, 0};
    sim_delay_push(&d, v);
  }
  sim_delay_read(&d, 0.0, DT, out);
  CHECK(out[0] == 999.0);
  sim_delay_read(&d, 10.0, DT, out);
  CHECK(out[0] == 1000.0 - SIM_DELAY_MAX);
}

// At rest and level the accelerometer reads -g on z and the gyro zero; the
// specific force follows the attitude.
static void test_imu_truth(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  struct sim_state s;
  sim_state_init(&s);
  double gyro[3], accel[3];
  sim_imu_truth(&p, &s, gyro, accel);
  CHECK(near(accel[2], -p.gravity, 1e-12) && near(accel[0], 0, 1e-12) && near(accel[1], 0, 1e-12));
  CHECK(gyro[0] == 0.0 && gyro[1] == 0.0 && gyro[2] == 0.0);
  sim_quat_from_euler((SIM_PI / 2), 0, 0, s.q); // rolled right: gravity along -y in body
  sim_imu_truth(&p, &s, gyro, accel);
  CHECK(near(accel[1], -p.gravity, 1e-12) && near(accel[2], 0.0, 1e-12));
  // Depth and attitude with ideal channels are the truth.
  struct sim_rng r;
  sim_rng_seed(&r, 1);
  struct sim_channel ideal = sim_channel_ideal();
  s.pos[2] = 3.25;
  CHECK(sim_depth_measure(&ideal, &s, &r) == 3.25);
  double rpy[3];
  sim_attitude_measure(&ideal, &s, &r, rpy);
  CHECK(near(rpy[0], (SIM_PI / 2), 1e-12));
}

// Same seed, same inputs: identical trajectories and readings, bit for bit.
static void test_determinism(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  struct sim_state a, b;
  struct sim_rng ra, rb;
  struct sim_thrusters ta, tb;
  sim_state_init(&a);
  sim_state_init(&b);
  a.pos[2] = b.pos[2] = 20.0;
  sim_rng_seed(&ra, 42);
  sim_rng_seed(&rb, 42);
  sim_thrusters_init(&ta, sim_geometry_bluerov2_heavy(), 0.1);
  sim_thrusters_init(&tb, sim_geometry_bluerov2_heavy(), 0.1);
  struct sim_imu_model imu = sim_imu_typical();
  double ga[3], aa[3], gb[3], ab[3], ta6[6], tb6[6];
  int same = 1;
  for (int k = 0; k < 2000; k++) {
    uint16_t cmd[8];
    for (int i = 0; i < 8; i++)
      cmd[i] = (uint16_t)(1300 + (sim_rng_u64(&ra) % 400));
    // Feed rb the same draws by advancing it identically.
    for (int i = 0; i < 8; i++)
      sim_rng_u64(&rb);
    sim_thrusters_command(&ta, cmd, 15.0);
    sim_thrusters_command(&tb, cmd, 15.0);
    sim_thrusters_step(&ta, DT);
    sim_thrusters_step(&tb, DT);
    sim_thrusters_wrench(&ta, ta6);
    sim_thrusters_wrench(&tb, tb6);
    sim_step(&p, &a, ta6, DT);
    sim_step(&p, &b, tb6, DT);
    double g0[3], a0[3];
    sim_imu_truth(&p, &a, g0, a0);
    sim_imu_measure(&imu, g0, a0, &ra, ga, aa);
    sim_imu_truth(&p, &b, g0, a0);
    sim_imu_measure(&imu, g0, a0, &rb, gb, ab);
    if (memcmp(&a, &b, sizeof a) || memcmp(ga, gb, sizeof ga) || memcmp(aa, ab, sizeof aa))
      same = 0;
  }
  CHECK(same);
  CHECK(a.t > 9.0); // and it actually ran
}

/* ---- the fit ------------------------------------------------------------------------- */

// A synthetic dive with known parameters, recorded at the simulation rate,
// then fitted from the published defaults. Random thruster commands held
// for half-second segments excite every axis; deep enough never to reach
// the surface.
#define FIT_N (200 * 90)
static struct sim_fit_sample fit_samples[FIT_N];

static void test_fit_recovers_parameters(void) {
  struct sim_params truth = sim_params_bluerov2_heavy();
  truth.buoyancy = truth.weight + 1.7;
  const double scale_lin[6] = {1.4, 0.8, 1.25, 1.6, 0.7, 1.3};
  const double scale_quad[6] = {0.75, 1.3, 0.9, 1.5, 0.8, 1.2};
  for (int i = 0; i < 6; i++) {
    truth.d_lin[i] *= scale_lin[i];
    truth.d_quad[i] *= scale_quad[i];
  }
  const double lag_true = 0.15;

  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 60.0;
  struct sim_thrusters th;
  sim_thrusters_init(&th, sim_geometry_bluerov2_heavy(), lag_true);
  struct sim_rng r;
  sim_rng_seed(&r, 2024);
  uint16_t cmd[8] = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
  double min_depth = 1e9, max_depth = 0.0;
  for (int k = 0; k < FIT_N; k++) {
    // The first half second is neutral: the thrusters start from zero and
    // the fit assumes the recording starts in a steady state.
    if (k % 100 == 0 && k > 0) {
      // Commands outside the deadband, moderate so the vehicle stays slow
      // enough for the quadratic term not to swamp the linear one.
      for (int i = 0; i < 8; i++) {
        double u = sim_rng_uniform(&r);
        cmd[i] = (uint16_t)(u < 0.5 ? 1340 + 120.0 * u * 2.0 : 1540 + 120.0 * (u - 0.5) * 2.0);
      }
      // Bias the verticals towards up a little: the vehicle is 1.7 N light,
      // so on average it rises; this keeps the depth bounded.
      if (k % 400 == 0)
        for (int i = 4; i < 8; i++)
          cmd[i] = 1520 + (uint16_t)(60.0 * sim_rng_uniform(&r));
    }
    sim_thrusters_command(&th, cmd, 15.0);
    struct sim_fit_sample *smp = &fit_samples[k];
    smp->t = s.t;
    memcpy(smp->q, s.q, sizeof smp->q);
    memcpy(smp->nu, s.nu, sizeof smp->nu);
    memcpy(smp->pwm, cmd, sizeof smp->pwm);
    smp->voltage = 15.0;
    double tau[6];
    // The thruster lag steps before the body so the wrench the body sees at
    // t is the thrust at the end of this step, as sim_vehicle will do it.
    sim_thrusters_step(&th, DT);
    sim_thrusters_wrench(&th, tau);
    sim_step(&truth, &s, tau, DT);
    if (s.pos[2] < min_depth)
      min_depth = s.pos[2];
    if (s.pos[2] > max_depth)
      max_depth = s.pos[2];
  }
  CHECK(min_depth > 1.0); // never touched the surface: the data is clean
  CHECK(max_depth < 200.0);

  struct sim_params base = sim_params_bluerov2_heavy(); // the wrong starting point
  struct sim_fit_options opt = {1.0, 1e-9, 1};
  struct sim_fit_result res;
  int rc = sim_fit(&base, sim_geometry_bluerov2_heavy(), fit_samples, FIT_N, &opt, &res);
  CHECK_EQ_INT(rc, 0);
  CHECK_EQ_INT((long long)res.samples_used, FIT_N - 2);

  printf("    fit  true -> recovered (rel. error)\n");
  printf("    B      %8.3f -> %8.3f (%+.2e)\n", truth.buoyancy, res.buoyancy,
         (res.buoyancy - truth.buoyancy) / truth.buoyancy);
  printf("    lag    %8.3f -> %8.3f (%+.2e)\n", lag_true, res.lag_s, (res.lag_s - lag_true) / lag_true);
  const char *axes = "uvwpqr";
  for (int i = 0; i < 6; i++)
    printf("    %c lin  %8.3f -> %8.3f (%+.2e)   quad %8.3f -> %8.3f (%+.2e)   rms res %.2e  rms vel %.3f\n",
           axes[i], truth.d_lin[i], res.d_lin[i], (res.d_lin[i] - truth.d_lin[i]) / truth.d_lin[i],
           truth.d_quad[i], res.d_quad[i], (res.d_quad[i] - truth.d_quad[i]) / truth.d_quad[i],
           res.rms_residual[i], res.rms_velocity[i]);

  // Tolerances: the regression uses central differences of a 200 Hz record,
  // so the residual is the discretization error of the acceleration, small
  // but not zero (about 1e-3 N here). Buoyancy and the lag come back to
  // better than 1e-4 relative, the damping of every axis to better than
  // 0.5%; the bounds below leave a margin for other compilers' rounding
  // (FMA contraction changes the last bits of the trajectory).
  CHECK(near(res.buoyancy, truth.buoyancy, 0.02));
  CHECK(near(res.lag_s, lag_true, 0.02 * lag_true));
  for (int i = 0; i < 6; i++) {
    CHECK(near(res.d_lin[i], truth.d_lin[i], 0.01 * truth.d_lin[i]));
    CHECK(near(res.d_quad[i], truth.d_quad[i], 0.01 * truth.d_quad[i]));
    CHECK(res.rms_residual[i] < 0.01);
  }
  // The lag search really found a minimum: the residual at the true lag is
  // no larger than at a wrong one.
  struct sim_fit_result tmp;
  double at_true = sim_fit_sse(&base, sim_geometry_bluerov2_heavy(), fit_samples, FIT_N, lag_true, 1e-9, &tmp);
  double at_wrong = sim_fit_sse(&base, sim_geometry_bluerov2_heavy(), fit_samples, FIT_N, 0.5, 1e-9, &tmp);
  CHECK(at_true < at_wrong);

  // Fewer than three samples is refused.
  CHECK_EQ_INT(sim_fit(&base, sim_geometry_bluerov2_heavy(), fit_samples, 2, &opt, &res), -1);
}

// An axis the dive never excites keeps its baseline value rather than
// going singular: here the vehicle only heaves. (Surge would not do: the
// horizontal thrusters sit below the CG and pitch the vehicle, and once it
// moves along x the Munk moment amplifies rounding noise into sway and yaw.
// Heave through the CG, upright, couples to nothing.)
static void test_fit_unexcited_axis_keeps_baseline(void) {
  struct sim_params truth = sim_params_bluerov2_heavy();
  truth.buoyancy = truth.weight;
  truth.d_lin[2] = 6.0;
  truth.d_quad[2] = 30.0;
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 20.0;
  const int n = 200 * 20;
  for (int k = 0; k < n; k++) {
    uint16_t cmd[8] = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
    // All four verticals equal, three levels in turn, all downward so the
    // surface is never reached (no floor is set).
    int step = (k / 200) % 3;
    for (int i = 4; i < 8; i++)
      cmd[i] = (uint16_t)(1560 + 60 * step);
    struct sim_fit_sample *smp = &fit_samples[k];
    smp->t = s.t;
    memcpy(smp->q, s.q, sizeof smp->q);
    memcpy(smp->nu, s.nu, sizeof smp->nu);
    memcpy(smp->pwm, cmd, sizeof smp->pwm);
    smp->voltage = 16.0;
    float w[6];
    sim_applied_wrench(cmd, 16.0f, w);
    double tau[6];
    for (int a = 0; a < 6; a++)
      tau[a] = w[a];
    sim_step(&truth, &s, tau, DT);
  }
  // It moved, down, and nothing else moved.
  CHECK(s.nu[2] > 0.1);
  for (int i = 0; i < 6; i++)
    if (i != 2)
      CHECK(near(s.nu[i], 0.0, 1e-12));
  struct sim_params base = sim_params_bluerov2_heavy();
  struct sim_fit_options opt = {0.0, 0.0, 0};
  struct sim_fit_result res;
  CHECK_EQ_INT(sim_fit(&base, sim_geometry_bluerov2_heavy(), fit_samples, (size_t)n, &opt, &res), 0);
  CHECK(near(res.d_lin[2], 6.0, 0.3));
  CHECK(near(res.d_quad[2], 30.0, 1.5));
  for (int i = 0; i < 6; i++) {
    if (i == 2)
      continue;
    CHECK(near(res.d_lin[i], base.d_lin[i], 1e-6 * base.d_lin[i]));
    CHECK(near(res.d_quad[i], base.d_quad[i], 1e-6 * base.d_quad[i]));
  }
  CHECK(res.lag_s == 0.0);
}

/* ---- payload ------------------------------------------------------------------------- */

static void test_add_payload(void) {
  struct sim_params p = sim_params_bluerov2_heavy();
  double pos[3] = {0.1, 0.0, 0.05};
  double m0 = p.mass, w0 = p.weight, cb0 = p.cb[2];
  sim_params_add_payload(&p, 0.5, pos);
  CHECK(near(p.mass, m0 + 0.5, 1e-12));
  CHECK(near(p.weight, w0 + 0.5 * p.gravity, 1e-12));
  // The CG moved towards the payload (forward and down), so the CB is now
  // further above and behind it.
  CHECK(p.cb[0] < 0.0);
  CHECK(p.cb[2] < cb0);
  CHECK(p.inertia[1] > 0.16 && p.inertia[2] > 0.16);
  // With buoyancy unchanged the vehicle is now heavy and sinks.
  struct sim_state s;
  sim_state_init(&s);
  s.pos[2] = 5.0;
  run(&p, &s, ZERO6, 10.0, DT);
  CHECK(s.pos[2] > 5.5);
}

int main(void) {
  RUN_TEST(test_quaternion_round_trip);
  RUN_TEST(test_neutral_buoyancy_holds_depth);
  RUN_TEST(test_surface_and_floor_limits);
  RUN_TEST(test_terminal_velocity_matches_damping);
  RUN_TEST(test_current_carries_vehicle);
  RUN_TEST(test_restoring_moment_rights_vehicle);
  RUN_TEST(test_energy_decays_monotonically);
  RUN_TEST(test_rk4_convergence_order);
  RUN_TEST(test_t200_inc_matches_csv);
  RUN_TEST(test_t200_lookup);
  RUN_TEST(test_allocation_matrix);
  RUN_TEST(test_applied_wrench_equals_command_path);
  RUN_TEST(test_thruster_lag);
  RUN_TEST(test_rng_and_channels);
  RUN_TEST(test_delay_line);
  RUN_TEST(test_imu_truth);
  RUN_TEST(test_determinism);
  RUN_TEST(test_fit_recovers_parameters);
  RUN_TEST(test_fit_unexcited_axis_keeps_baseline);
  RUN_TEST(test_add_payload);
  TEST_MAIN_END();
}
