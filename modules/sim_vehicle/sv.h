// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
 * sv: sim_vehicle's core (modules/sim_vehicle/README.md): lib/sim's model,
 * thrusters and sensors under one parameter set, a battery that drains with
 * thrust, and ArduPilot SITL's JSON interface frames. No I/O, no topics, no
 * clock: the module and the tests step it.
 */
#include "sim_model.h"
#include "sim_noise.h"
#include "sim_thrust.h"
#include "sim_vehicle_params.h"

#include <stddef.h>
#include <stdint.h>

struct sv {
	struct sim_vehicle_params P;
	struct sim_params p;
	struct sim_state s;
	struct sim_thrusters th;
	struct sim_rng rng;
	struct sim_imu_model imu;
	struct sim_channel depth_ch, att_ch;
	struct sim_delay d_gyro, d_accel, d_depth, d_att;
	double dt;
	uint16_t pwm[SIM_NTHRUSTERS]; /* the last command */
	double tau[6];                /* the last step's wrench */
	double used_ah;               /* charge drawn so far */
	double voltage, current;      /* the battery now */
};

/* The vehicle at rest, upright, at the surface, t = 0, thrusters stopped,
 * the battery full; `seed` seeds the sensor noise. */
void sv_init(struct sv *v, const struct sim_vehicle_params *P, uint64_t seed, double dt);
/* New parameters, live: the state, the thrusts and the charge are kept. */
void sv_set_params(struct sv *v, const struct sim_vehicle_params *P);
/* One step of v->dt under the pulse widths (0 counts as 1500): the thrusters'
 * targets from the table at the battery's voltage, their lags, the wrench,
 * the model, the battery, and the sensors' delay lines. */
void sv_step(struct sv *v, const uint16_t pwm[SIM_NTHRUSTERS]);

/* Sensor readings at the time of the last step, with each channel's noise,
 * bias and latency. Every call draws noise: the order of calls is part of
 * what a seed reproduces. */
void sv_imu(struct sv *v, double gyro[3], double accel[3]);
double sv_depth(struct sv *v);
void sv_attitude(struct sv *v, double rpy[3]);
/* The battery: voltage, current drawn, remaining fraction. */
void sv_battery(const struct sv *v, double *volt, double *amps, double *remaining);

/* ArduPilot SITL's JSON interface (libraries/SITL/SIM_JSON.{h,cpp} at
 * Sub-4.7.2): SITL sends its servo outputs as a binary packet to UDP 9002,
 * the physics answers with one JSON object between newlines. */
#define SV_JSON_PORT 9002
#define SV_SERVO16_MAGIC 18458
#define SV_SERVO32_MAGIC 29569
#define SV_SERVO16_SIZE (2 + 2 + 4 + 2 * 16)
#define SV_SERVO32_SIZE (2 + 2 + 4 + 2 * 32)
/* Parses a servo packet: 16 or 32 (the channels it carries), or -1 for a
 * datagram that is neither. pwm receives the first 16 channels. */
int sv_parse_servo(const void *buf, size_t len, uint16_t *frame_rate, uint32_t *frame_count, uint16_t pwm[16]);
/* The reply for the current state: "\n{...}\n", with the timestamp, the IMU
 * (with the IMU channels' noise, bias and latency), the position, the
 * attitude quaternion and the velocity (world NED), and the battery. The
 * length written, or -1 if cap is too small. */
int sv_json(struct sv *v, char *out, size_t cap);
