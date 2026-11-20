// SPDX-License-Identifier: GPL-3.0-or-later
/* joystick_map: joystick.conf, the events, the shaping (joystick_map.h). */
#include "joystick_map.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hg_ini.h"

const char *const js_axis_names[JS_WRENCH_AXES] = { "surge", "sway", "heave", "roll", "pitch", "yaw" };

void js_conf_defaults(struct js_conf *c)
{
	memset(c, 0, sizeof *c);
	snprintf(c->device, sizeof c->device, "/dev/input/js0");
	c->reopen_ns = 1000000000ull;
	for (int a = 0; a < JS_WRENCH_AXES; a++)
		c->axis[a] = (struct js_axis_conf){ .source = -1, .deadband = 0.05f };
	c->axis[0] = (struct js_axis_conf){ .source = 4, .invert = 1, .deadband = 0.05f }; /* surge: right stick, up forward */
	c->axis[1] = (struct js_axis_conf){ .source = 3, .deadband = 0.05f };              /* sway: right stick, right */
	c->axis[2] = (struct js_axis_conf){ .source = 1, .invert = 1, .deadband = 0.05f }; /* heave: left stick, up up */
	c->axis[5] = (struct js_axis_conf){ .source = 0, .deadband = 0.05f };              /* yaw: left stick, right clockwise */
}

/* ---- joystick.conf --------------------------------------------------------- */

enum { SEC_NONE, SEC_JOYSTICK, SEC_AXIS, SEC_BUTTON };

struct parse {
	struct js_conf *c;
	int section, index;
};

static int to_long(const char *v, long lo, long hi, long *out)
{
	char *end;
	errno = 0;
	long x = strtol(v, &end, 10);
	if (errno || end == v || *end || x < lo || x > hi)
		return -1;
	*out = x;
	return 0;
}

static int to_float(const char *v, float lo, float hi, float *out)
{
	char *end;
	errno = 0;
	float x = strtof(v, &end);
	if (errno || end == v || *end || !(x >= lo && x <= hi))
		return -1;
	*out = x;
	return 0;
}

static int on_section(void *ctx, const char *section, const char *arg, int line, char *err, size_t errlen)
{
	(void)line;
	struct parse *p = ctx;
	if (strcmp(section, "Joystick") == 0 && !*arg) {
		p->section = SEC_JOYSTICK;
		return 0;
	}
	if (strcmp(section, "Axis") == 0) {
		for (int a = 0; a < JS_WRENCH_AXES; a++)
			if (strcmp(arg, js_axis_names[a]) == 0) {
				p->section = SEC_AXIS;
				p->index = a;
				/* A section replaces the axis's mapping: what it does not
				 * say is plain, not the built-in map's (an Invert=yes of the
				 * default would otherwise survive a new Source). */
				p->c->axis[a] = (struct js_axis_conf){ .source = -1, .deadband = 0.05f };
				return 0;
			}
		snprintf(err, errlen, "[Axis %s]: one of surge sway heave roll pitch yaw", arg);
		return -1;
	}
	if (strcmp(section, "Button") == 0) {
		long n;
		if (to_long(arg, 0, JS_MAX_BUTTONS - 1, &n) < 0) {
			snprintf(err, errlen, "[Button %s]: a button number 0 to %d", arg, JS_MAX_BUTTONS - 1);
			return -1;
		}
		p->section = SEC_BUTTON;
		p->index = (int)n;
		return 0;
	}
	snprintf(err, errlen, "unknown section [%s%s%s]", section, *arg ? " " : "", arg);
	return -1;
}

static int on_key(void *ctx, const char *key, const char *value, int line, char *err, size_t errlen)
{
	(void)line;
	struct parse *p = ctx;
	struct js_conf *c = p->c;
	if (p->section == SEC_JOYSTICK) {
		if (strcmp(key, "Device") == 0) {
			if (!*value || strlen(value) >= sizeof c->device) {
				snprintf(err, errlen, "Device: a path (/dev/input/js0) or auto");
				return -1;
			}
			snprintf(c->device, sizeof c->device, "%s", value);
		} else if (strcmp(key, "Name") == 0) {
			if (strlen(value) >= sizeof c->name) {
				snprintf(err, errlen, "Name: at most %zu characters", sizeof c->name - 1);
				return -1;
			}
			snprintf(c->name, sizeof c->name, "%s", value);
		} else if (strcmp(key, "Reopen") == 0) {
			int64_t d = hg_ini_parse_duration(value);
			if (d < 10000000) {
				snprintf(err, errlen, "Reopen: a duration of 10ms or more (1s)");
				return -1;
			}
			c->reopen_ns = (uint64_t)d;
		} else {
			snprintf(err, errlen, "unknown key %s in [Joystick]", key);
			return -1;
		}
		return 0;
	}
	if (p->section == SEC_AXIS) {
		struct js_axis_conf *a = &c->axis[p->index];
		long n;
		if (strcmp(key, "Source") == 0) {
			if (strcmp(value, "none") == 0)
				a->source = -1;
			else if (to_long(value, 0, JS_MAX_AXES - 1, &n) == 0)
				a->source = (int)n;
			else {
				snprintf(err, errlen, "Source: the controller's axis number, 0 to %d, or none", JS_MAX_AXES - 1);
				return -1;
			}
		} else if (strcmp(key, "Invert") == 0) {
			int b = hg_ini_parse_bool(value);
			if (b < 0) {
				snprintf(err, errlen, "Invert: yes or no");
				return -1;
			}
			a->invert = b;
		} else if (strcmp(key, "Deadband") == 0) {
			if (to_float(value, 0, 0.9f, &a->deadband) < 0) {
				snprintf(err, errlen, "Deadband: 0 to 0.9");
				return -1;
			}
		} else if (strcmp(key, "Expo") == 0) {
			if (to_float(value, 0, 1, &a->expo) < 0) {
				snprintf(err, errlen, "Expo: 0 to 1");
				return -1;
			}
		} else {
			snprintf(err, errlen, "unknown key %s in [Axis %s]", key, js_axis_names[p->index]);
			return -1;
		}
		return 0;
	}
	if (p->section == SEC_BUTTON) {
		if (strcmp(key, "Function") != 0) {
			snprintf(err, errlen, "unknown key %s in [Button %d]", key, p->index);
			return -1;
		}
		static const char *const names[] = { "none", "pass", "arm", "disarm" };
		for (int f = 0; f < 4; f++)
			if (strcmp(value, names[f]) == 0) {
				c->button[p->index] = (uint8_t)f;
				return 0;
			}
		snprintf(err, errlen, "Function: none, pass, arm or disarm");
		return -1;
	}
	snprintf(err, errlen, "%s outside a section", key);
	return -1;
}

int js_conf_parse(const char *text, size_t len, struct js_conf *c, char *err, size_t errlen)
{
	struct parse p = { .c = c };
	if (hg_ini_parse(text, len, on_section, on_key, &p, err, errlen) < 0)
		return -1;
	if (strcmp(c->device, "auto") == 0 && !c->name[0]) {
		snprintf(err, errlen, "Device=auto needs Name= (a part of the controller's name)");
		return -1;
	}
	return 0;
}

/* ---- Events and shaping ------------------------------------------------------ */

void js_apply(struct js_state *s, const struct js_ev *e)
{
	uint8_t type = e->type & (uint8_t)~JS_EV_INIT;
	if (type == JS_EV_AXIS && e->number < JS_MAX_AXES) {
		s->axes[e->number] = e->value;
	} else if (type == JS_EV_BUTTON && e->number < JS_MAX_BUTTONS) {
		uint32_t bit = 1u << e->number;
		if (e->value) {
			if (!(s->buttons & bit) && !(e->type & JS_EV_INIT))
				s->pressed |= bit;
			s->buttons |= bit;
		} else {
			s->buttons &= ~bit;
		}
	}
}

float js_shape(const struct js_axis_conf *a, int16_t raw)
{
	float x = raw / 32767.0f;
	if (x > 1)
		x = 1;
	if (x < -1)
		x = -1;
	if (a->invert)
		x = -x;
	float m = fabsf(x);
	if (m <= a->deadband)
		return 0;
	m = (m - a->deadband) / (1 - a->deadband);
	m = (1 - a->expo) * m + a->expo * m * m * m;
	return x < 0 ? -m : m;
}

void js_wrench(const struct js_conf *c, const struct js_state *s, float out[JS_WRENCH_AXES])
{
	for (int a = 0; a < JS_WRENCH_AXES; a++) {
		int src = c->axis[a].source;
		out[a] = src >= 0 && src < JS_MAX_AXES ? js_shape(&c->axis[a], s->axes[src]) : 0;
	}
}

uint32_t js_pass_bits(const struct js_conf *c, const struct js_state *s)
{
	uint32_t bits = 0;
	for (int b = 0; b < JS_MAX_BUTTONS; b++)
		if (c->button[b] == JS_FN_PASS && (s->buttons >> b & 1))
			bits |= 1u << b;
	return bits;
}
