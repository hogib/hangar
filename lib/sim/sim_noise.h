// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
 * sim_noise: a seeded generator and the sensor models sim_vehicle uses to
 * turn the truth state into nav.imu, nav.depth and nav.attitude (doc 14:
 * "each with configurable noise, bias and latency"). Pure functions over
 * explicit state: the generator is a struct the caller owns, so two
 * simulators with the same seed produce the same readings, and nothing here
 * reads a clock.
 */
#include "sim_model.h"
#include <stdint.h>

/* xorshift64 (Marsaglia 2003). Small, fast, and good enough for sensor
 * noise; the seed is the whole state, so --seed reproduces a run. */
struct sim_rng {
  uint64_t s;
};

void sim_rng_seed(struct sim_rng *r, uint64_t seed);
uint64_t sim_rng_u64(struct sim_rng *r);
double sim_rng_uniform(struct sim_rng *r); /* [0, 1) */
double sim_rng_gauss(struct sim_rng *r);   /* mean 0, sd 1 (polar Box-Muller) */

/* One scalar channel's imperfections. Noise is white Gaussian with
 * standard deviation `noise_sd`; `bias` is a constant offset; `latency_s`
 * is how old the reading is (realized through sim_delay below). */
struct sim_channel {
  double noise_sd;
  double bias;
  double latency_s;
};

double sim_channel_apply(const struct sim_channel *c, double truth, struct sim_rng *r);

/* A delay line of 3-vectors for latency: push one sample per step, read the
 * one `latency_s` old (rounded to whole steps, clamped to the capacity).
 * Before the line has filled, the oldest available sample is returned. */
#define SIM_DELAY_MAX 256 /* 1.28 s at 200 Hz */
struct sim_delay {
  double buf[SIM_DELAY_MAX][3];
  int head;  /* next write index */
  int count; /* samples held */
};

void sim_delay_init(struct sim_delay *d);
void sim_delay_push(struct sim_delay *d, const double v[3]);
void sim_delay_read(const struct sim_delay *d, double latency_s, double dt, double out[3]);

/* The IMU: gyro reads body rates, the accelerometer reads specific force
 * (acceleration minus gravity) in the body frame; at rest and level that is
 * (0, 0, -g) in NED. The channels are per-axis-identical. */
struct sim_imu_model {
  struct sim_channel gyro;  /* rad/s */
  struct sim_channel accel; /* m/s^2 */
};

void sim_imu_truth(const struct sim_params *p, const struct sim_state *s, double gyro[3],
                   double accel[3]);
void sim_imu_measure(const struct sim_imu_model *m, const double gyro_truth[3],
                     const double accel_truth[3], struct sim_rng *r, double gyro[3],
                     double accel[3]);

/* Depth from a pressure sensor, in metres (noise and bias in metres). */
double sim_depth_measure(const struct sim_channel *c, const struct sim_state *s, struct sim_rng *r);

/* Attitude as roll, pitch, yaw (rad), each with the same channel. */
void sim_attitude_measure(const struct sim_channel *c, const struct sim_state *s,
                          struct sim_rng *r, double rpy[3]);

/* Noise-free channels: the defaults sim_vehicle starts from. */
struct sim_channel sim_channel_ideal(void);
/* Typical values for a Navigator-class sensor set (estimates; see README). */
struct sim_imu_model sim_imu_typical(void);
struct sim_channel sim_depth_typical(void);
struct sim_channel sim_attitude_typical(void);
