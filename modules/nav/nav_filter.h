// SPDX-License-Identifier: GPL-3.0-or-later
/* nav_filter: the vertical motion of the vehicle, estimated from its depth and
 * its accelerometer (modules/nav/README.md). A Kalman filter of three states,
 * all positive down: depth z (m), vertical velocity v (m/s), and the
 * accelerometer's vertical bias b (m/s^2). The prediction integrates the
 * vertical acceleration the IMU measures, rotated into the vertical by the
 * attitude, gravity added back; each new depth corrects it. Without the IMU
 * (use_imu 0, or no fresh attitude and IMU) the prediction is constant
 * velocity, the process noise standing in for the unknown acceleration.
 *
 * No allocation, no hangar dependency: the module and its suite call it
 * alike. */
#ifndef NAV_FILTER_H
#define NAV_FILTER_H

#include <stdint.h>

#define NAV_G 9.80665f

struct nav_filter {
	double x[3];      /* z, v, b */
	double P[3][3];   /* covariance */
	int started;      /* a first depth seen */
	/* Parameters (nav_params): */
	double accel_noise; /* m/s^2 */
	double bias_walk;   /* m/s^3 */
	double depth_noise; /* m */
};

void nav_filter_init(struct nav_filter *f, double accel_noise, double bias_walk, double depth_noise);

/* The vertical acceleration, positive down, gravity removed, from a body
 * specific force (an accelerometer's reading, gravity included) and the
 * attitude as a unit quaternion w x y z, body to NED. */
float nav_vertical_accel(const float q[4], const float f_body[3]);

/* The body acceleration with gravity removed: f + R^T g. */
void nav_body_accel(const float q[4], const float f_body[3], float out[3]);

/* Moves the estimate dt seconds on. have_accel: a_down (m/s^2, positive down,
 * gravity removed) is measured; otherwise constant velocity and the bias left
 * as it is. Nothing happens before the first depth. */
void nav_filter_predict(struct nav_filter *f, double dt, int have_accel, double a_down);

/* Corrects the estimate with a depth (m, positive down). The first one starts
 * the filter at that depth, at rest. */
void nav_filter_depth(struct nav_filter *f, double z);

#endif
