// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
 * sim_model: the rigid-body physics of hangar's vehicle simulator
 * (docs/14-simulation.md, "Physics"). Pure arithmetic: no allocation, no
 * I/O, no topics; sim_vehicle wraps it as a module.
 *
 * Fossen's equation in the body frame with every matrix diagonal, as the
 * published BlueROV2 identifications take them:
 *
 *   (M_RB + M_A) nu_dot + C_RB(nu) nu + C_A(nu_r) nu_r + D(nu_r) nu_r + g(eta) = tau
 *
 * nu = [u v w p q r] is the body velocity, nu_r = nu - R^T v_current the
 * velocity relative to the water (added-mass Coriolis and damping act on
 * it), tau the thruster wrench. The body frame is NED-style: x forward,
 * y right (starboard), z down, origin at the centre of gravity, so M_RB is
 * exactly diagonal and the restoring moment comes from the centre of
 * buoyancy's offset `cb` alone. The world frame is NED too, z positive
 * down, so depth is pos[2].
 *
 * Integration is classical RK4 at a caller-given step (200 Hz is the
 * default rate) with the attitude quaternion renormalized after each step.
 * Everything is double: the model is deterministic bit for bit given the
 * same inputs on the same machine, which the replay tests of doc 14 rely
 * on.
 */
#include <stddef.h>

#define SIM_DEFAULT_RATE_HZ 200.0
#define SIM_DEFAULT_DT (1.0 / SIM_DEFAULT_RATE_HZ)
#define SIM_G_N 9.80665 /* standard gravity, N/kg */

/* Every coefficient of the model. sim_params_bluerov2_heavy() fills it with
 * the published heavy-configuration values; each one's source is named
 * there. Units are SI throughout; all damping and added-mass entries are
 * positive (the literature prints them as negative hydrodynamic
 * derivatives, -X_u etc.; the sign is folded in here). */
struct sim_params {
  double mass;            /* rigid-body mass, kg */
  double inertia[3];      /* Ix, Iy, Iz about the CG, kg m^2 */
  double added_mass[6];   /* -X_udot, -Y_vdot, -Z_wdot, -K_pdot, -M_qdot, -N_rdot */
  double d_lin[6];        /* linear damping per axis, N s/m and N m s/rad */
  double d_quad[6];       /* quadratic damping per axis, N s^2/m^2, N m s^2/rad^2 */
  double weight;          /* W, N (mass * g, kept separate so B-W is explicit) */
  double buoyancy;        /* B, N */
  double cb[3];           /* centre of buoyancy relative to the CG, body frame, m
                             (z down: a CB above the CG has cb[2] < 0) */
  double gravity;         /* m/s^2, for the IMU and the energy bookkeeping */
  double current[3];      /* water velocity in the world frame, NED m/s */
  double current_amp[3];  /* amplitude of a slow sinusoidal variation, m/s */
  double current_period;  /* its period, s (<= 0: constant current) */
  double floor_depth;     /* sea floor depth, m (<= 0: no floor) */
};

/* The truth state. q is the unit quaternion (w, x, y, z) rotating body
 * vectors into the world frame. nu_dot is the acceleration computed during
 * the last step, kept for the IMU model. */
struct sim_state {
  double t;         /* s */
  double pos[3];    /* world NED, m; pos[2] is depth */
  double q[4];      /* attitude, world <- body */
  double nu[6];     /* body velocity: u v w (m/s), p q r (rad/s) */
  double nu_dot[6]; /* body acceleration at the last step's start */
};

/* The BlueROV2 heavy configuration's published defaults. */
struct sim_params sim_params_bluerov2_heavy(void);

/* Adds a rigid payload of `mass` kg at `pos` (body frame relative to the
 * current CG, m): mass, weight, inertia (parallel-axis, diagonal part only)
 * and the CB offset follow the shifted CG. The payload is assumed to have no
 * buoyancy of its own, so adding one makes the vehicle heavier in water
 * unless buoyancy is raised too; this is how doc 09's adaptive depth-hold
 * experiment is driven. */
void sim_params_add_payload(struct sim_params *p, double mass, const double pos[3]);

/* At rest, upright, at the surface, t = 0. */
void sim_state_init(struct sim_state *s);

/* Water velocity (world frame) at time t. */
void sim_current_at(const struct sim_params *p, double t, double out[3]);

/* nu_dot for the state and the body-frame wrench tau (N, N m). Exposed for
 * tests and for sim_fit, which evaluates the model's residual. */
void sim_model_accel(const struct sim_params *p, const struct sim_state *s,
                     const double tau[6], double nu_dot[6]);

/* The pieces sim_fit needs separately: nu_r, the velocity relative to the
 * water; c = C_RB(nu) nu + C_A(nu_r) nu_r; and e_b, the world's down axis
 * expressed in the body frame (the restoring wrench is (W - B) e_b for the
 * forces and cb x (-B e_b) for the moments). */
void sim_model_coriolis(const struct sim_params *p, const struct sim_state *s, double nu_r[6],
                        double c[6], double e_b[3]);

/* One RK4 step of dt seconds under the constant wrench tau. Applies the
 * surface and floor limits afterwards: depth is clamped at 0 (and at
 * floor_depth) with the world-frame vertical velocity component pointing
 * out of the water column zeroed. */
void sim_step(const struct sim_params *p, struct sim_state *s, const double tau[6], double dt);

/* Kinetic (1/2 nu^T (M_RB + M_A) nu) plus potential energy, the potential
 * being that of weight and buoyancy: (B - W) z + B (R cb)_z. Only
 * meaningful in still water; with no thrust it must decay monotonically. */
double sim_energy(const struct sim_params *p, const struct sim_state *s);

/* Quaternion helpers (w, x, y, z), shared with sim_noise and sim_fit. */
void sim_quat_from_euler(double roll, double pitch, double yaw, double q[4]);
void sim_quat_to_euler(const double q[4], double *roll, double *pitch, double *yaw);
void sim_quat_rotate(const double q[4], const double v[3], double out[3]);     /* body -> world */
void sim_quat_rotate_inv(const double q[4], const double v[3], double out[3]); /* world -> body */
void sim_quat_normalize(double q[4]);
