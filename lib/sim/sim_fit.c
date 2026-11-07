// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Least-squares calibration. See sim_fit.h for the method.
 */
#include "sim_fit.h"
#include <math.h>
#include <string.h>

#define NX 13 /* 6 d_lin, 6 d_quad, B */
#define IX_B 12

/* Gaussian elimination with partial pivoting on the NX x NX system
 * A x = b, in place. Returns -1 on a zero pivot. */
static int solve(double A[NX][NX], double b[NX], double x[NX]) {
  for (int k = 0; k < NX; k++) {
    int piv = k;
    for (int i = k + 1; i < NX; i++)
      if (fabs(A[i][k]) > fabs(A[piv][k]))
        piv = i;
    if (fabs(A[piv][k]) < 1e-300)
      return -1;
    if (piv != k) {
      for (int j = 0; j < NX; j++) {
        double t = A[k][j];
        A[k][j] = A[piv][j];
        A[piv][j] = t;
      }
      double t = b[k];
      b[k] = b[piv];
      b[piv] = t;
    }
    for (int i = k + 1; i < NX; i++) {
      double f = A[i][k] / A[k][k];
      if (f == 0.0)
        continue;
      for (int j = k; j < NX; j++)
        A[i][j] -= f * A[k][j];
      b[i] -= f * b[k];
    }
  }
  for (int i = NX - 1; i >= 0; i--) {
    double acc = b[i];
    for (int j = i + 1; j < NX; j++)
      acc -= A[i][j] * x[j];
    x[i] = acc / A[i][i];
  }
  return 0;
}

/* One sample's contribution: the regressor rows for each axis. For axis i
 * the row is [nu_i at column i, |nu_i| nu_i at column 6+i, b_i at column
 * 12] and the target y_i; both are returned so a second pass can compute
 * the residual with the solution in hand. */
struct row {
  double lin[6], quad[6], b[6], y[6];
};

static void thrust_wrench(const struct sim_fit_sample *s, double lag_s, double prev[SIM_NTHRUSTERS],
                          int first, double dt, double T[6][SIM_NTHRUSTERS], double tau[6]) {
  double target[SIM_NTHRUSTERS];
  for (int i = 0; i < SIM_NTHRUSTERS; i++)
    target[i] = sim_t200_thrust((double)s->pwm[i], s->voltage);
  if (first || lag_s <= 0.0) {
    // The recording starts in a steady state as far as the fit can know.
    memcpy(prev, target, sizeof target);
  } else {
    double a = 1.0 - exp(-dt / lag_s);
    for (int i = 0; i < SIM_NTHRUSTERS; i++)
      prev[i] += a * (target[i] - prev[i]);
  }
  sim_alloc_wrench(&T[0][0], prev, tau);
}

static void make_row(const struct sim_params *base, const struct sim_fit_sample *s,
                     const double nu_dot[6], const double tau[6], struct row *r) {
  struct sim_state st;
  memset(&st, 0, sizeof st);
  st.t = s->t;
  memcpy(st.q, s->q, sizeof st.q);
  memcpy(st.nu, s->nu, sizeof st.nu);
  double nu_r[6], c[6], e_b[3];
  sim_model_coriolis(base, &st, nu_r, c, e_b);
  // Restoring wrench split into its W part (known, moved to y) and its B
  // part (unknown, a regressor): forces (W - B) e_b, moments cb x (-B e_b).
  double cbxe[3] = {base->cb[1] * e_b[2] - base->cb[2] * e_b[1],
                    base->cb[2] * e_b[0] - base->cb[0] * e_b[2],
                    base->cb[0] * e_b[1] - base->cb[1] * e_b[0]};
  for (int i = 0; i < 6; i++) {
    double m = (i < 3 ? base->mass : base->inertia[i - 3]) + base->added_mass[i];
    r->lin[i] = nu_r[i];
    r->quad[i] = fabs(nu_r[i]) * nu_r[i];
    r->y[i] = tau[i] - m * nu_dot[i] - c[i];
    if (i < 3) {
      r->b[i] = e_b[i];
      r->y[i] += base->weight * e_b[i];
    } else {
      r->b[i] = cbxe[i - 3];
    }
  }
}

/* Walks the recording once, calling fn with the regression row of every
 * sample that has a central difference (1..n-2). The lag filter runs over
 * all samples. The wrench paired with sample k is the mean of the wrenches
 * held over [t_{k-1}, t_k] and [t_k, t_{k+1}]: the central difference is the
 * mean acceleration over that window, and a pulse width is held from its
 * sample to the next, so the mean wrench over the window is what that
 * acceleration answers to. Pairing the wrench at t_k instead biases the lag
 * by half a sample. */
static void for_each_row(const struct sim_params *base, const struct sim_thruster_geometry *geom,
                         const struct sim_fit_sample *samples, size_t n, double lag_s,
                         void (*fn)(const struct row *, void *), void *ctx) {
  double T[6][SIM_NTHRUSTERS];
  sim_alloc_matrix(geom, T);
  double prev[SIM_NTHRUSTERS] = {0};
  double tau_prev[6] = {0}, tau[6];
  for (size_t k = 0; k < n; k++) {
    double dt = k ? samples[k].t - samples[k - 1].t : 0.0;
    memcpy(tau_prev, tau, sizeof tau);
    thrust_wrench(&samples[k], lag_s, prev, k == 0, dt, T, tau);
    if (k == 0 || k + 1 >= n)
      continue;
    double span = samples[k + 1].t - samples[k - 1].t;
    double nu_dot[6], tau_mean[6];
    for (int i = 0; i < 6; i++) {
      nu_dot[i] = (samples[k + 1].nu[i] - samples[k - 1].nu[i]) / span;
      tau_mean[i] = 0.5 * (tau_prev[i] + tau[i]);
    }
    struct row r;
    make_row(base, &samples[k], nu_dot, tau_mean, &r);
    fn(&r, ctx);
  }
}

struct normal_eq {
  double AtA[NX][NX];
  double Aty[NX];
};

// Each row has only three nonzeros per axis, so the products are written
// out rather than looped over NX.
static void accumulate(const struct row *r, void *ctx) {
  struct normal_eq *ne = ctx;
  for (int i = 0; i < 6; i++) {
    int cl = i, cq = 6 + i;
    double l = r->lin[i], q = r->quad[i], b = r->b[i], y = r->y[i];
    ne->AtA[cl][cl] += l * l;
    ne->AtA[cl][cq] += l * q;
    ne->AtA[cl][IX_B] += l * b;
    ne->AtA[cq][cl] += q * l;
    ne->AtA[cq][cq] += q * q;
    ne->AtA[cq][IX_B] += q * b;
    ne->AtA[IX_B][cl] += b * l;
    ne->AtA[IX_B][cq] += b * q;
    ne->AtA[IX_B][IX_B] += b * b;
    ne->Aty[cl] += l * y;
    ne->Aty[cq] += q * y;
    ne->Aty[IX_B] += b * y;
  }
}

struct residuals {
  const double *x;
  double sse[6], ssv[6];
  size_t used;
};

static void residual(const struct row *r, void *ctx) {
  struct residuals *rs = ctx;
  for (int i = 0; i < 6; i++) {
    double e = r->y[i] - rs->x[i] * r->lin[i] - rs->x[6 + i] * r->quad[i] - rs->x[IX_B] * r->b[i];
    rs->sse[i] += e * e;
    rs->ssv[i] += r->lin[i] * r->lin[i];
  }
  rs->used++;
}

double sim_fit_sse(const struct sim_params *base, const struct sim_thruster_geometry *geom,
                   const struct sim_fit_sample *samples, size_t n, double lag_s, double ridge,
                   struct sim_fit_result *out) {
  // Pass 1: the normal equations.
  struct normal_eq ne;
  memset(&ne, 0, sizeof ne);
  for_each_row(base, geom, samples, n, lag_s, accumulate, &ne);
  double (*AtA)[NX] = ne.AtA;
  double *Aty = ne.Aty;

  // Ridge towards the baseline: (AtA + lambda I) x = Aty + lambda x0, with
  // lambda a small fraction of the mean diagonal, so an axis with no
  // excitation returns its baseline value and a well-excited one is moved
  // by a negligible amount.
  double x0[NX];
  for (int i = 0; i < 6; i++) {
    x0[i] = base->d_lin[i];
    x0[6 + i] = base->d_quad[i];
  }
  x0[IX_B] = base->buoyancy;
  double trace = 0.0;
  for (int i = 0; i < NX; i++)
    trace += AtA[i][i];
  double lambda = ridge * trace / NX;
  if (lambda <= 0.0)
    lambda = 1e-300; // keeps a wholly empty column solvable
  for (int i = 0; i < NX; i++) {
    AtA[i][i] += lambda;
    Aty[i] += lambda * x0[i];
  }
  double x[NX];
  if (solve(AtA, Aty, x) != 0) {
    out->samples_used = 0;
    return INFINITY;
  }

  // Pass 2: residuals with the solution.
  struct residuals rs;
  memset(&rs, 0, sizeof rs);
  rs.x = x;
  for_each_row(base, geom, samples, n, lag_s, residual, &rs);
  size_t used = rs.used;
  double total = 0.0;
  for (int i = 0; i < 6; i++) {
    out->d_lin[i] = x[i];
    out->d_quad[i] = x[6 + i];
    out->rms_residual[i] = used ? sqrt(rs.sse[i] / (double)used) : 0.0;
    out->rms_velocity[i] = used ? sqrt(rs.ssv[i] / (double)used) : 0.0;
    total += rs.sse[i];
  }
  out->buoyancy = x[IX_B];
  out->lag_s = lag_s;
  out->samples_used = used;
  return total;
}

int sim_fit(const struct sim_params *base, const struct sim_thruster_geometry *geom,
            const struct sim_fit_sample *samples, size_t n, const struct sim_fit_options *opt,
            struct sim_fit_result *out) {
  memset(out, 0, sizeof *out);
  if (n < 3)
    return -1;
  double lag_max = opt && opt->lag_max_s > 0.0 ? opt->lag_max_s : 1.0;
  double ridge = opt && opt->ridge > 0.0 ? opt->ridge : 1e-6;
  int fit_lag = opt ? opt->fit_lag : 1;

  if (!fit_lag) {
    double s = sim_fit_sse(base, geom, samples, n, 0.0, ridge, out);
    return s == INFINITY ? -1 : 0;
  }

  // Golden-section search for the lag on [0, lag_max]. The residual is
  // smooth in the lag and, for a dive that excites the thrusters at all,
  // unimodal; 40 iterations bring the bracket below 1e-7 of lag_max.
  const double gr = 0.6180339887498949;
  double a = 0.0, b = lag_max;
  double c = b - gr * (b - a), d = a + gr * (b - a);
  struct sim_fit_result rc, rd;
  double fc = sim_fit_sse(base, geom, samples, n, c, ridge, &rc);
  double fd = sim_fit_sse(base, geom, samples, n, d, ridge, &rd);
  for (int it = 0; it < 40; it++) {
    if (fc < fd) {
      b = d;
      d = c;
      fd = fc;
      rd = rc;
      c = b - gr * (b - a);
      fc = sim_fit_sse(base, geom, samples, n, c, ridge, &rc);
    } else {
      a = c;
      c = d;
      fc = fd;
      rc = rd;
      d = a + gr * (b - a);
      fd = sim_fit_sse(base, geom, samples, n, d, ridge, &rd);
    }
  }
  *out = fc < fd ? rc : rd;
  // A lag within the search's resolution of zero is no lag: report it as
  // such so the simulator takes the direct path.
  if (out->lag_s < 1e-6)
    out->lag_s = 0.0;
  return out->samples_used ? 0 : -1;
}
