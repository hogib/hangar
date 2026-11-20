// SPDX-License-Identifier: GPL-3.0-or-later
#include "sv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void build_params(struct sv *v)
{
	const struct sim_vehicle_params *P = &v->P;
	struct sim_params p = sim_params_bluerov2_heavy();
	p.mass = P->mass;
	p.weight = P->mass * p.gravity;
	p.buoyancy = P->buoyancy;
	for (int i = 0; i < 6; i++) {
		p.d_lin[i] = P->d_lin[i];
		p.d_quad[i] = P->d_quad[i];
	}
	for (int i = 0; i < 3; i++)
		p.current[i] = P->current[i];
	p.floor_depth = P->floor_depth;
	if (P->payload_mass > 0) {
		const double pos[3] = { P->payload_pos[0], P->payload_pos[1], P->payload_pos[2] };
		sim_params_add_payload(&p, P->payload_mass, pos);
	}
	v->p = p;
	v->imu.gyro = (struct sim_channel){ P->gyro_noise, P->gyro_bias, P->imu_latency };
	v->imu.accel = (struct sim_channel){ P->accel_noise, P->accel_bias, P->imu_latency };
	v->depth_ch = (struct sim_channel){ P->depth_noise, P->depth_bias, P->depth_latency };
	v->att_ch = (struct sim_channel){ P->attitude_noise, P->attitude_bias, P->attitude_latency };
	v->th.lag_s = P->thruster_lag;
}

static void battery_update(struct sv *v)
{
	double remaining = 1.0 - v->used_ah / v->P.battery_ah;
	if (remaining < 0)
		remaining = 0;
	v->voltage = v->P.battery_empty_v + (v->P.battery_full_v - v->P.battery_empty_v) * remaining;
}

static void push_sensors(struct sv *v)
{
	double gyro[3], accel[3];
	sim_imu_truth(&v->p, &v->s, gyro, accel);
	sim_delay_push(&v->d_gyro, gyro);
	sim_delay_push(&v->d_accel, accel);
	const double depth[3] = { v->s.pos[2], 0, 0 };
	sim_delay_push(&v->d_depth, depth);
	double rpy[3];
	sim_quat_to_euler(v->s.q, &rpy[0], &rpy[1], &rpy[2]);
	sim_delay_push(&v->d_att, rpy);
}

void sv_init(struct sv *v, const struct sim_vehicle_params *P, uint64_t seed, double dt)
{
	memset(v, 0, sizeof *v);
	v->P = *P;
	v->dt = dt;
	sim_state_init(&v->s);
	sim_thrusters_init(&v->th, sim_geometry_bluerov2_heavy(), P->thruster_lag);
	sim_rng_seed(&v->rng, seed);
	sim_delay_init(&v->d_gyro);
	sim_delay_init(&v->d_accel);
	sim_delay_init(&v->d_depth);
	sim_delay_init(&v->d_att);
	for (int i = 0; i < SIM_NTHRUSTERS; i++)
		v->pwm[i] = SIM_T200_PWM_NEUTRAL;
	build_params(v);
	battery_update(v);
	v->current = P->idle_current;
	push_sensors(v);
}

void sv_set_params(struct sv *v, const struct sim_vehicle_params *P)
{
	v->P = *P;
	build_params(v);
	battery_update(v);
}

void sv_step(struct sv *v, const uint16_t pwm[SIM_NTHRUSTERS])
{
	for (int i = 0; i < SIM_NTHRUSTERS; i++)
		v->pwm[i] = pwm[i] ? pwm[i] : SIM_T200_PWM_NEUTRAL;
	sim_thrusters_command(&v->th, v->pwm, v->voltage);
	sim_thrusters_step(&v->th, v->dt);
	sim_thrusters_wrench(&v->th, v->tau);
	sim_step(&v->p, &v->s, v->tau, v->dt);
	double total = 0;
	for (int i = 0; i < SIM_NTHRUSTERS; i++)
		total += fabs(v->th.thrust[i]);
	v->current = v->P.idle_current + v->P.amps_per_newton * total;
	v->used_ah += v->current * v->dt / 3600.0;
	battery_update(v);
	push_sensors(v);
}

void sv_imu(struct sv *v, double gyro[3], double accel[3])
{
	double g[3], a[3];
	sim_delay_read(&v->d_gyro, v->imu.gyro.latency_s, v->dt, g);
	sim_delay_read(&v->d_accel, v->imu.accel.latency_s, v->dt, a);
	sim_imu_measure(&v->imu, g, a, &v->rng, gyro, accel);
}

double sv_depth(struct sv *v)
{
	double d[3];
	sim_delay_read(&v->d_depth, v->depth_ch.latency_s, v->dt, d);
	return sim_channel_apply(&v->depth_ch, d[0], &v->rng);
}

void sv_attitude(struct sv *v, double rpy[3])
{
	double a[3];
	sim_delay_read(&v->d_att, v->att_ch.latency_s, v->dt, a);
	for (int i = 0; i < 3; i++)
		rpy[i] = sim_channel_apply(&v->att_ch, a[i], &v->rng);
}

void sv_battery(const struct sv *v, double *volt, double *amps, double *remaining)
{
	double r = 1.0 - v->used_ah / v->P.battery_ah;
	*volt = v->voltage;
	*amps = v->current;
	*remaining = r < 0 ? 0 : r;
}

static uint16_t le16(const uint8_t *b)
{
	return (uint16_t)(b[0] | b[1] << 8);
}

int sv_parse_servo(const void *buf, size_t len, uint16_t *frame_rate, uint32_t *frame_count, uint16_t pwm[16])
{
	const uint8_t *b = buf;
	if (len < 8)
		return -1;
	uint16_t magic = le16(b);
	int n;
	if (magic == SV_SERVO16_MAGIC && len == SV_SERVO16_SIZE)
		n = 16;
	else if (magic == SV_SERVO32_MAGIC && len == SV_SERVO32_SIZE)
		n = 32;
	else
		return -1;
	*frame_rate = le16(b + 2);
	*frame_count = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
	for (int i = 0; i < 16; i++)
		pwm[i] = le16(b + 8 + 2 * i);
	return n;
}

int sv_json(struct sv *v, char *out, size_t cap)
{
	double gyro[3], accel[3], vel[3];
	sv_imu(v, gyro, accel);
	sim_quat_rotate(v->s.q, v->s.nu, vel); /* body linear velocity to world NED */
	int n = snprintf(out, cap,
			 "\n{\"timestamp\":%.6f,"
			 "\"imu\":{\"gyro\":[%.7f,%.7f,%.7f],\"accel_body\":[%.6f,%.6f,%.6f]},"
			 "\"position\":[%.6f,%.6f,%.6f],"
			 "\"quaternion\":[%.8f,%.8f,%.8f,%.8f],"
			 "\"velocity\":[%.6f,%.6f,%.6f],"
			 "\"battery\":{\"voltage\":%.3f,\"current\":%.3f}}\n",
			 v->s.t, gyro[0], gyro[1], gyro[2], accel[0], accel[1], accel[2], v->s.pos[0], v->s.pos[1],
			 v->s.pos[2], v->s.q[0], v->s.q[1], v->s.q[2], v->s.q[3], vel[0], vel[1], vel[2], v->voltage,
			 v->current);
	return n < 0 || (size_t)n >= cap ? -1 : n;
}
