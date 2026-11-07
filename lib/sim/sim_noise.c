// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Seeded noise and the sensor models. See sim_noise.h.
 */
#include "sim_noise.h"
#include <math.h>
#include <string.h>

/* ---- xorshift64 ------------------------------------------------------------ */

void sim_rng_seed(struct sim_rng *r, uint64_t seed) {
  // xorshift has one fixed point, zero; map it (and only it) elsewhere so
  // --seed 0 is as good a seed as any other.
  r->s = seed ? seed : 0x9E3779B97F4A7C15ull;
}

uint64_t sim_rng_u64(struct sim_rng *r) {
  uint64_t x = r->s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  r->s = x;
  return x;
}

double sim_rng_uniform(struct sim_rng *r) {
  // Top 53 bits: exactly the doubles in [0, 1) with uniform spacing.
  return (double)(sim_rng_u64(r) >> 11) * (1.0 / 9007199254740992.0);
}

double sim_rng_gauss(struct sim_rng *r) {
  // Marsaglia's polar method. One of the pair is thrown away so that each
  // call consumes a deterministic, state-only amount of randomness (no
  // cached second value to make two generators with the same seed diverge
  // when their call sequences interleave differently).
  for (;;) {
    double u = 2.0 * sim_rng_uniform(r) - 1.0;
    double v = 2.0 * sim_rng_uniform(r) - 1.0;
    double s = u * u + v * v;
    if (s > 0.0 && s < 1.0)
      return u * sqrt(-2.0 * log(s) / s);
  }
}

/* ---- channels and latency --------------------------------------------------- */

double sim_channel_apply(const struct sim_channel *c, double truth, struct sim_rng *r) {
  double n = c->noise_sd > 0.0 ? c->noise_sd * sim_rng_gauss(r) : 0.0;
  return truth + c->bias + n;
}

void sim_delay_init(struct sim_delay *d) { memset(d, 0, sizeof *d); }

void sim_delay_push(struct sim_delay *d, const double v[3]) {
  memcpy(d->buf[d->head], v, sizeof d->buf[0]);
  d->head = (d->head + 1) % SIM_DELAY_MAX;
  if (d->count < SIM_DELAY_MAX)
    d->count++;
}

void sim_delay_read(const struct sim_delay *d, double latency_s, double dt, double out[3]) {
  if (d->count == 0) {
    out[0] = out[1] = out[2] = 0.0;
    return;
  }
  int back = 0;
  if (latency_s > 0.0 && dt > 0.0)
    back = (int)floor(latency_s / dt + 0.5);
  // The newest sample is `back` 0; a latency longer than what is held reads
  // the oldest sample rather than wrapping round to the newest.
  if (back > d->count - 1)
    back = d->count - 1;
  int idx = (d->head - 1 - back + SIM_DELAY_MAX) % SIM_DELAY_MAX;
  memcpy(out, d->buf[idx], sizeof d->buf[0]);
}

/* ---- sensors ----------------------------------------------------------------- */

void sim_imu_truth(const struct sim_params *p, const struct sim_state *s, double gyro[3],
                   double accel[3]) {
  const double *v = s->nu, *w = s->nu + 3;
  memcpy(gyro, w, 3 * sizeof(double));
  // Specific force: the body-frame acceleration of the CG (v_dot + w x v,
  // the derivative of a vector expressed in a rotating frame) minus
  // gravity; the accelerometer cannot tell the two apart.
  double g_w[3] = {0.0, 0.0, p->gravity}, g_b[3];
  sim_quat_rotate_inv(s->q, g_w, g_b);
  double wxv[3] = {w[1] * v[2] - w[2] * v[1], w[2] * v[0] - w[0] * v[2],
                   w[0] * v[1] - w[1] * v[0]};
  for (int i = 0; i < 3; i++)
    accel[i] = s->nu_dot[i] + wxv[i] - g_b[i];
}

void sim_imu_measure(const struct sim_imu_model *m, const double gyro_truth[3],
                     const double accel_truth[3], struct sim_rng *r, double gyro[3],
                     double accel[3]) {
  for (int i = 0; i < 3; i++)
    gyro[i] = sim_channel_apply(&m->gyro, gyro_truth[i], r);
  for (int i = 0; i < 3; i++)
    accel[i] = sim_channel_apply(&m->accel, accel_truth[i], r);
}

double sim_depth_measure(const struct sim_channel *c, const struct sim_state *s,
                         struct sim_rng *r) {
  return sim_channel_apply(c, s->pos[2], r);
}

void sim_attitude_measure(const struct sim_channel *c, const struct sim_state *s,
                          struct sim_rng *r, double rpy[3]) {
  double truth[3];
  sim_quat_to_euler(s->q, truth, truth + 1, truth + 2);
  for (int i = 0; i < 3; i++)
    rpy[i] = sim_channel_apply(c, truth[i], r);
}

struct sim_channel sim_channel_ideal(void) {
  struct sim_channel c = {0.0, 0.0, 0.0};
  return c;
}

/*
 * Typical values. These are engineering estimates for a Navigator-class
 * sensor set, not datasheet figures fetched for this file (README.md says
 * so): roughly a MEMS gyro's noise over a 100 Hz bandwidth, a MEMS
 * accelerometer's, a Bar30's few-millimetre resolution with a small
 * calibration offset, and an attitude estimate good to about half a
 * degree. sim_vehicle exposes each as a parameter; nothing downstream
 * should depend on these particular numbers.
 */
struct sim_imu_model sim_imu_typical(void) {
  struct sim_imu_model m;
  m.gyro.noise_sd = 7e-4;   // rad/s (~0.04 deg/s rms)
  m.gyro.bias = 2e-3;       // rad/s (~0.1 deg/s)
  m.gyro.latency_s = 0.005; // one sample at 200 Hz
  m.accel.noise_sd = 0.01;  // m/s^2 (~1 mg)
  m.accel.bias = 0.02;      // m/s^2
  m.accel.latency_s = 0.005;
  return m;
}

struct sim_channel sim_depth_typical(void) {
  struct sim_channel c = {0.005, 0.02, 0.02}; // m, m, s
  return c;
}

struct sim_channel sim_attitude_typical(void) {
  struct sim_channel c = {0.0087, 0.0, 0.02}; // rad (~0.5 deg), rad, s
  return c;
}
