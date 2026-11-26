// SPDX-License-Identifier: GPL-3.0-or-later
/* dh_ctl: the depth controller of the depth_hold module
 * (modules/depth_hold/README.md). All positive down except the heave command,
 * which is up positive (msg/wrench.msg).
 *
 *   accel_z = clamp(kp (target - depth) - kd vel_z, ±accel_max)   the acceleration wanted
 *   heave   = clamp(-ff accel_z + I, ±max_thrust)                   the command
 *   I      -= ki (target - depth) dt, clamped to ±i_max             only while driving,
 *                                    within i_band of the target and with accel_z unclamped
 *
 * While the controller does not drive (the mux gives the heave axis to the
 * pilot, or to nobody), its target follows the depth and its integrator is
 * held: when the mux hands the axis back (the stick released in assisted, a
 * failsafe hold) it holds the depth it is at, with the trim it had learnt. A
 * set target holds until the controller, having driven with it, stops driving.
 * The integrator's band keeps it from winding up on a long approach.
 *
 * No allocation, no hangar dependency: the module and its suite call it
 * alike. */
#ifndef DH_CTL_H
#define DH_CTL_H

struct dh_gains {
	float kp, kd, accel_max, ff, ki, i_max, i_band, max_thrust;
};

struct dh_ctl {
	double target;   /* m, positive down */
	double integral; /* heave, up positive */
	int follow;      /* 1: the target follows the depth */
	int was_driving;
	int started;
};

struct dh_out {
	float heave;     /* the command, -1 to 1, up positive */
	float accel_z;   /* m/s^2 wanted, positive down */
};

void dh_init(struct dh_ctl *c);
/* A target to hold from now; hold_current: the depth of the next step. */
void dh_set(struct dh_ctl *c, double target);
void dh_hold_current(struct dh_ctl *c);
/* One step: the depth and vertical velocity (positive down), whether the mux
 * has the controller driving the heave axis, whether the outputs are enabled
 * (the integrator learns only then), dt in seconds. */
struct dh_out dh_step(struct dh_ctl *c, const struct dh_gains *g, double depth, double vel_z, int driving,
		      int outputs_enabled, double dt);

/* Levelling, for a vehicle with no autopilot behind it to keep it upright
 * (README "Levelling"): the roll or pitch command, -max to max, that turns an
 * angle (rad) and its rate (rad/s) back to level,
 *
 *   command = clamp(-kp angle - kd rate, +-max)
 *
 * No state: there is nothing to trim, level is where the centre of buoyancy
 * already pulls. A non-finite angle or rate gives 0. */
float dh_level(float kp, float kd, float max, double angle, double rate);

/* Heading hold, likewise: the yaw command (clockwise from above positive)
 * that turns the heading `yaw` to `target` the short way round, both in rad
 * from north, any number of turns apart,
 *
 *   command = clamp(kp wrap(target - yaw) - kd rate, +-max)
 *
 * The module keeps the target: the heading the vehicle had when the mux last
 * handed the yaw axis over. */
float dh_heading(float kp, float kd, float max, double target, double yaw, double rate);

#endif
