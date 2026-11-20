// SPDX-License-Identifier: GPL-3.0-or-later
/* dh_ctl: see dh_ctl.h. */
#include "dh_ctl.h"

#include <string.h>

static double clampd(double v, double lim)
{
	return v > lim ? lim : v < -lim ? -lim : v;
}

void dh_init(struct dh_ctl *c)
{
	memset(c, 0, sizeof *c);
	c->follow = 1;
}

void dh_set(struct dh_ctl *c, double target)
{
	c->target = target;
	c->follow = 0;
	c->started = 1;
	c->was_driving = 0; /* not a takeover at the next step */
}

void dh_hold_current(struct dh_ctl *c)
{
	c->follow = 1; /* the next step takes the depth... */
	c->was_driving = 0; /* ...and, driving, keeps it */
}

struct dh_out dh_step(struct dh_ctl *c, const struct dh_gains *g, double depth, double vel_z, int driving,
		      int outputs_enabled, double dt)
{
	if (!c->started) {
		c->target = depth;
		c->started = 1;
	}
	/* The pilot (or nobody) took the axis: follow the depth from now. */
	if (!driving && c->was_driving)
		c->follow = 1;
	if (c->follow)
		c->target = depth;
	/* The axis handed back: hold the depth it was handed back at. */
	if (driving && c->follow)
		c->follow = 0;
	c->was_driving = driving;

	double err = c->target - depth;
	double raw = g->kp * err - g->kd * vel_z;
	double accel = clampd(raw, g->accel_max);
	/* Conditional integration: near the target and unsaturated, else the
	 * integrator winds up on the approach and overshoots. */
	if (driving && outputs_enabled && dt > 0 && err < g->i_band && err > -g->i_band &&
	    raw <= g->accel_max && raw >= -g->accel_max)
		c->integral = clampd(c->integral - g->ki * err * dt, g->i_max);
	double heave = clampd(-g->ff * accel + c->integral, g->max_thrust);
	return (struct dh_out){ .heave = (float)heave, .accel_z = (float)accel };
}
