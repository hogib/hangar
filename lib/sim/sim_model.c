// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The rigid-body model: see sim_model.h for the equation and the frames.
 * Everything here is a function of its arguments; the only state is the
 * caller's struct sim_state.
 */
#include "sim_model.h"
#include <math.h>
#include <string.h>

// M_PI is POSIX, not C11; -std=c11 -pedantic hides it.
#define SIM_PI 3.14159265358979323846

/* ---- small vector helpers ------------------------------------------------ */

static void cross(const double a[3], const double b[3], double o[3]) {
  o[0] = a[1] * b[2] - a[2] * b[1];
  o[1] = a[2] * b[0] - a[0] * b[2];
  o[2] = a[0] * b[1] - a[1] * b[0];
}

/* ---- quaternions (w, x, y, z), world <- body ----------------------------- */

void sim_quat_normalize(double q[4]) {
  double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  // A zero quaternion cannot arise from integration; guard anyway so a
  // caller handing in zeros gets the identity rather than NaNs.
  if (n <= 0.0) {
    q[0] = 1.0;
    q[1] = q[2] = q[3] = 0.0;
    return;
  }
  for (int i = 0; i < 4; i++)
    q[i] /= n;
}

// v' = v + 2 w (qv x v) + 2 qv x (qv x v), the expansion of q v q* for a
// unit quaternion; cheaper than forming the matrix for one vector.
void sim_quat_rotate(const double q[4], const double v[3], double out[3]) {
  const double qv[3] = {q[1], q[2], q[3]};
  double t[3], u[3];
  cross(qv, v, t);
  cross(qv, t, u);
  for (int i = 0; i < 3; i++)
    out[i] = v[i] + 2.0 * q[0] * t[i] + 2.0 * u[i];
}

void sim_quat_rotate_inv(const double q[4], const double v[3], double out[3]) {
  const double qc[4] = {q[0], -q[1], -q[2], -q[3]};
  sim_quat_rotate(qc, v, out);
}

// ZYX (yaw, pitch, roll) convention, as nav.attitude reports it.
void sim_quat_from_euler(double roll, double pitch, double yaw, double q[4]) {
  double cr = cos(roll / 2), sr = sin(roll / 2);
  double cp = cos(pitch / 2), sp = sin(pitch / 2);
  double cy = cos(yaw / 2), sy = sin(yaw / 2);
  q[0] = cr * cp * cy + sr * sp * sy;
  q[1] = sr * cp * cy - cr * sp * sy;
  q[2] = cr * sp * cy + sr * cp * sy;
  q[3] = cr * cp * sy - sr * sp * cy;
}

void sim_quat_to_euler(const double q[4], double *roll, double *pitch, double *yaw) {
  double w = q[0], x = q[1], y = q[2], z = q[3];
  double sinp = 2.0 * (w * y - z * x);
  if (sinp > 1.0)
    sinp = 1.0;
  if (sinp < -1.0)
    sinp = -1.0;
  if (roll)
    *roll = atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y));
  if (pitch)
    *pitch = asin(sinp);
  if (yaw)
    *yaw = atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
}

/* ---- parameters ---------------------------------------------------------- */

/*
 * BlueROV2 heavy configuration. Sources:
 *
 *  [Wu18] Chu-Jou Wu, "6-DoF Modelling and Control of a Remotely Operated
 *         Vehicle", MSc thesis, Flinders University, 2018.
 *         https://theses.flinders.edu.au/view/27aa0064-9de2-441c-8a17-655405d5fc2e/1
 *         Wu had no vehicle to test: the rigid-body values come from Blue
 *         Robotics' specification, the hydrodynamic ones from Sandøy's 2016
 *         NTNU identification of the (non-heavy) BlueROV, with roll and
 *         pitch set equal to yaw for want of data (Wu18 section 5.2). They
 *         are the published defaults doc 14 asks for, not measurements of
 *         this vehicle; `hangar sim fit` replaces them.
 *  [BR]   Blue Robotics, BlueROV2 Heavy Configuration Retrofit Kit page,
 *         https://bluerobotics.com/store/rov/bluerov2-upgrade-kits/brov2-heavy-retrofit/
 *         (fetched 2026-11-07): "Total ROV Weight (in air with ballast)
 *         11.5 kg (26 lb)", matching Wu18's 11.5 kg.
 */
struct sim_params sim_params_bluerov2_heavy(void) {
  struct sim_params p;
  memset(&p, 0, sizeof p);
  p.mass = 11.5;        // Wu18 Table 5.1 "m 11.5 (kg)"; BR "11.5 kg"
  p.inertia[0] = 0.16;  // Wu18 Table 5.1 "Ix 0.16 (kg m2)" (assumed = Iz)
  p.inertia[1] = 0.16;  // Wu18 Table 5.1 "Iy 0.16 (kg m2)" (assumed = Iz)
  p.inertia[2] = 0.16;  // Wu18 Table 5.1 "Iz 0.16 (kg m2)" (from Sandøy 2016)
  // Wu18 Table 5.2, printed as negative derivatives; stored positive.
  p.added_mass[0] = 5.5;   // "X_udot -5.5 (kg)"
  p.added_mass[1] = 12.7;  // "Y_vdot -12.7 (kg)"
  p.added_mass[2] = 14.57; // "Z_wdot -14.57 (kg)"
  p.added_mass[3] = 0.12;  // "K_pdot -0.12 (kg m2/rad)" (assumed = yaw)
  p.added_mass[4] = 0.12;  // "M_qdot -0.12 (kg m2/rad)" (assumed = yaw)
  p.added_mass[5] = 0.12;  // "N_rdot -0.12 (kg m2/rad)"
  // Wu18 Table 5.3, same sign convention.
  p.d_lin[0] = 4.03;   // "X_u -4.03 (Ns/m)"
  p.d_lin[1] = 6.22;   // "Y_v -6.22 (Ns/m)"
  p.d_lin[2] = 5.18;   // "Z_w -5.18 (Ns/m)"
  p.d_lin[3] = 0.07;   // "K_p -0.07 (Ns/rad)" (assumed = yaw)
  p.d_lin[4] = 0.07;   // "M_q -0.07 (Ns/rad)" (assumed = yaw)
  p.d_lin[5] = 0.07;   // "N_r -0.07 (Ns/rad)"
  p.d_quad[0] = 18.18; // "X_u|u| -18.18 (Ns2/m2)"
  p.d_quad[1] = 21.66; // "Y_v|v| -21.66 (Ns2/m2)"
  p.d_quad[2] = 36.99; // "Z_w|w| -36.99 (Ns2/m2)"
  p.d_quad[3] = 1.55;  // "K_p|p| -1.55 (Ns2/rad2)" (assumed = yaw)
  p.d_quad[4] = 1.55;  // "M_q|q| -1.55 (Ns2/rad2)" (assumed = yaw)
  p.d_quad[5] = 1.55;  // "N_r|r| -1.55 (Ns2/rad2)"
  p.weight = 112.8;    // Wu18 eq. (5.16) "W = 11.5 x 9.81 = 112.8 (N)"
  p.buoyancy = 114.8;  // Wu18 eq. (5.17) "B = 0.2 x 9.81 + 112.8 = 114.8 (N)":
                       // net buoyancy 0.2 kg from the BR specification
  // Wu18 Table 5.1: body origin at the CB, "rg [0, 0, 0.02]^T (m)", i.e. the
  // CG 0.02 m below the CB. Our origin is the CG, so the CB is at -0.02 in
  // z (down). Wu calls 0.02 m "a representative value", chosen because
  // Sandøy's measured 0.00019 m is implausible for a passively stable ROV.
  p.cb[0] = 0.0;
  p.cb[1] = 0.0;
  p.cb[2] = -0.02;
  p.gravity = 9.81;    // the g Wu18 used for W and B above
  p.current_period = 0.0;
  p.floor_depth = 0.0; // none until sim_vehicle sets one
  return p;
}

void sim_params_add_payload(struct sim_params *p, double mass, const double pos[3]) {
  if (mass <= 0.0)
    return;
  double total = p->mass + mass;
  // The combined CG moves towards the payload by mass/total of its offset;
  // every body-frame position (the CB, the payload itself) shifts the other
  // way by that amount.
  double shift[3];
  for (int i = 0; i < 3; i++)
    shift[i] = pos[i] * mass / total;
  for (int i = 0; i < 3; i++)
    p->cb[i] -= shift[i];
  // Parallel-axis terms, diagonal part only (the model keeps the inertia
  // diagonal): the old body about the new CG, plus the payload as a point.
  double r_old[3] = {-shift[0], -shift[1], -shift[2]};
  double r_new[3] = {pos[0] - shift[0], pos[1] - shift[1], pos[2] - shift[2]};
  for (int i = 0; i < 3; i++) {
    double o2 = 0.0, n2 = 0.0;
    for (int j = 0; j < 3; j++)
      if (j != i) {
        o2 += r_old[j] * r_old[j];
        n2 += r_new[j] * r_new[j];
      }
    p->inertia[i] += p->mass * o2 + mass * n2;
  }
  p->mass = total;
  p->weight += mass * p->gravity;
}

/* ---- state ----------------------------------------------------------------- */

void sim_state_init(struct sim_state *s) {
  memset(s, 0, sizeof *s);
  s->q[0] = 1.0;
}

void sim_current_at(const struct sim_params *p, double t, double out[3]) {
  for (int i = 0; i < 3; i++) {
    out[i] = p->current[i];
    if (p->current_period > 0.0)
      out[i] += p->current_amp[i] * sin(2.0 * SIM_PI * t / p->current_period);
  }
}

/* ---- dynamics ------------------------------------------------------------ */

void sim_model_coriolis(const struct sim_params *p, const struct sim_state *s, double nu_r[6],
                        double c[6], double e_b[3]) {
  const double *v = s->nu, *w = s->nu + 3;
  // The current is irrotational, so only the linear part of nu is relative.
  double vc_w[3], vc_b[3];
  sim_current_at(p, s->t, vc_w);
  sim_quat_rotate_inv(s->q, vc_w, vc_b);
  for (int i = 0; i < 3; i++) {
    nu_r[i] = v[i] - vc_b[i];
    nu_r[3 + i] = w[i];
  }
  // Rigid body about the CG: [m w x v ; w x (I w)].
  double Iw[3] = {p->inertia[0] * w[0], p->inertia[1] * w[1], p->inertia[2] * w[2]};
  double wxv[3], wxIw[3];
  cross(w, v, wxv);
  cross(w, Iw, wxIw);
  // Added mass on the relative velocity, Fossen's C_A for a diagonal M_A:
  // [w x (A1 v_r) ; v_r x (A1 v_r) + w x (A2 w)].
  double a1[3] = {p->added_mass[0] * nu_r[0], p->added_mass[1] * nu_r[1],
                  p->added_mass[2] * nu_r[2]};
  double a2[3] = {p->added_mass[3] * w[0], p->added_mass[4] * w[1], p->added_mass[5] * w[2]};
  double wxa1[3], vxa1[3], wxa2[3];
  cross(w, a1, wxa1);
  cross(nu_r, a1, vxa1);
  cross(w, a2, wxa2);
  for (int i = 0; i < 3; i++) {
    c[i] = p->mass * wxv[i] + wxa1[i];
    c[3 + i] = wxIw[i] + vxa1[i] + wxa2[i];
  }
  const double down[3] = {0.0, 0.0, 1.0};
  sim_quat_rotate_inv(s->q, down, e_b);
}

void sim_model_accel(const struct sim_params *p, const struct sim_state *s, const double tau[6],
                     double nu_dot[6]) {
  double nu_r[6], c[6], e_b[3];
  sim_model_coriolis(p, s, nu_r, c, e_b);
  // Restoring wrench on the right-hand side: weight down at the CG,
  // buoyancy up at the CB.
  double rest[6];
  double minus_b[3] = {-p->buoyancy * e_b[0], -p->buoyancy * e_b[1], -p->buoyancy * e_b[2]};
  cross(p->cb, minus_b, rest + 3);
  for (int i = 0; i < 3; i++)
    rest[i] = (p->weight - p->buoyancy) * e_b[i];
  for (int i = 0; i < 6; i++) {
    double m = (i < 3 ? p->mass : p->inertia[i - 3]) + p->added_mass[i];
    double d = (p->d_lin[i] + p->d_quad[i] * fabs(nu_r[i])) * nu_r[i];
    nu_dot[i] = (tau[i] + rest[i] - c[i] - d) / m;
  }
}

/* The ODE's right-hand side over the packed state y = [pos(3) q(4) nu(6)]. */
#define NY 13

static void pack(const struct sim_state *s, double y[NY]) {
  memcpy(y, s->pos, 3 * sizeof(double));
  memcpy(y + 3, s->q, 4 * sizeof(double));
  memcpy(y + 7, s->nu, 6 * sizeof(double));
}

static void unpack(const double y[NY], struct sim_state *s) {
  memcpy(s->pos, y, 3 * sizeof(double));
  memcpy(s->q, y + 3, 4 * sizeof(double));
  memcpy(s->nu, y + 7, 6 * sizeof(double));
}

static void deriv(const struct sim_params *p, double t, const double y[NY], const double tau[6],
                  double dy[NY]) {
  struct sim_state s;
  s.t = t;
  unpack(y, &s);
  // The intermediate quaternions of an RK4 step drift from unit length by
  // O(dt^2); rotating with them unnormalized would scale the velocity by
  // that drift, so normalize the working copy (the stored one is
  // normalized after the step).
  sim_quat_normalize(s.q);
  sim_quat_rotate(s.q, s.nu, dy); // pos_dot = R v
  const double *w = s.nu + 3;
  const double *q = s.q;
  // q_dot = 1/2 q (x) (0, w)
  dy[3] = -0.5 * (q[1] * w[0] + q[2] * w[1] + q[3] * w[2]);
  dy[4] = 0.5 * (q[0] * w[0] + q[2] * w[2] - q[3] * w[1]);
  dy[5] = 0.5 * (q[0] * w[1] + q[3] * w[0] - q[1] * w[2]);
  dy[6] = 0.5 * (q[0] * w[2] + q[1] * w[1] - q[2] * w[0]);
  sim_model_accel(p, &s, tau, dy + 7);
}

// Stops the vertical world velocity pointing out of the water column at a
// limit, leaving the horizontal and angular motion alone: the vehicle slides
// along the surface or the floor rather than bouncing.
static void clamp_vertical(struct sim_state *s, int at_surface) {
  double vw[3];
  sim_quat_rotate(s->q, s->nu, vw);
  if ((at_surface && vw[2] < 0.0) || (!at_surface && vw[2] > 0.0)) {
    vw[2] = 0.0;
    sim_quat_rotate_inv(s->q, vw, s->nu);
  }
}

void sim_step(const struct sim_params *p, struct sim_state *s, const double tau[6], double dt) {
  double y0[NY], k1[NY], k2[NY], k3[NY], k4[NY], yt[NY];
  pack(s, y0);
  deriv(p, s->t, y0, tau, k1);
  memcpy(s->nu_dot, k1 + 7, 6 * sizeof(double));
  for (int i = 0; i < NY; i++)
    yt[i] = y0[i] + 0.5 * dt * k1[i];
  deriv(p, s->t + 0.5 * dt, yt, tau, k2);
  for (int i = 0; i < NY; i++)
    yt[i] = y0[i] + 0.5 * dt * k2[i];
  deriv(p, s->t + 0.5 * dt, yt, tau, k3);
  for (int i = 0; i < NY; i++)
    yt[i] = y0[i] + dt * k3[i];
  deriv(p, s->t + dt, yt, tau, k4);
  for (int i = 0; i < NY; i++)
    yt[i] = y0[i] + dt / 6.0 * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]);
  unpack(yt, s);
  sim_quat_normalize(s->q);
  s->t += dt;

  if (s->pos[2] < 0.0) {
    s->pos[2] = 0.0;
    clamp_vertical(s, 1);
  }
  if (p->floor_depth > 0.0 && s->pos[2] > p->floor_depth) {
    s->pos[2] = p->floor_depth;
    clamp_vertical(s, 0);
  }
}

double sim_energy(const struct sim_params *p, const struct sim_state *s) {
  double kinetic = 0.0;
  for (int i = 0; i < 6; i++) {
    double m = (i < 3 ? p->mass : p->inertia[i - 3]) + p->added_mass[i];
    kinetic += 0.5 * m * s->nu[i] * s->nu[i];
  }
  double cb_w[3];
  sim_quat_rotate(s->q, p->cb, cb_w);
  double potential = (p->buoyancy - p->weight) * s->pos[2] + p->buoyancy * cb_w[2];
  return kinetic + potential;
}
