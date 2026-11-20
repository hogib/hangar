// SPDX-License-Identifier: GPL-3.0-or-later
/* joystick_map: what the joystick module does with a controller's events,
 * without the device or the topics, so that the suite can test it alone:
 * joystick.conf, the Linux joystick events (struct js_event), the state they
 * build, and the shaping of an axis into a wrench axis (invert, deadband,
 * expo). modules/joystick/README.md. */
#ifndef JOYSTICK_MAP_H
#define JOYSTICK_MAP_H
#include <stddef.h>
#include <stdint.h>

#define JS_MAX_AXES 32
#define JS_MAX_BUTTONS 32
#define JS_WRENCH_AXES 6 /* surge sway heave roll pitch yaw: struct wrench's order */

/* The kernel's struct js_event (linux/joystick.h), 8 bytes, stable ABI. */
struct js_ev {
	uint32_t time;  /* ms, the driver's clock */
	int16_t value;  /* an axis: -32767..32767; a button: 0 or 1 */
	uint8_t type;   /* JS_EV_BUTTON, JS_EV_AXIS, | JS_EV_INIT for the state at open */
	uint8_t number; /* which axis or button */
};
#define JS_EV_BUTTON 0x01
#define JS_EV_AXIS 0x02
#define JS_EV_INIT 0x80

enum js_func { JS_FN_NONE = 0, JS_FN_PASS, JS_FN_ARM, JS_FN_DISARM };

struct js_axis_conf {
	int source;      /* the controller's axis number; -1: none, the wrench axis stays 0 */
	int invert;
	float deadband;  /* 0..0.9: |x| below it is 0, the rest rescaled to reach 1 */
	float expo;      /* 0..1: (1 - e) x + e x^3, finer near the centre */
};

struct js_conf {
	char device[256];       /* /dev/input/jsN; "auto": the first whose name holds name */
	char name[128];         /* with device "auto": a part of the controller's name ("Xbox") */
	uint64_t reopen_ns;     /* while no controller: how often to look for one */
	struct js_axis_conf axis[JS_WRENCH_AXES];
	uint8_t button[JS_MAX_BUTTONS]; /* enum js_func per button number */
};

extern const char *const js_axis_names[JS_WRENCH_AXES];

/* The defaults: /dev/input/js0; the left stick heave (up positive) and yaw,
 * the right stick surge (forward positive) and sway, as QGroundControl's
 * ArduSub default and the Xbox layout on Linux (axes 0 1 left, 3 4 right);
 * roll and pitch none; deadband 0.05; no buttons. */
void js_conf_defaults(struct js_conf *c);
/* joystick.conf's text over the defaults. 0, or -1 with "line N: reason" in err. */
int js_conf_parse(const char *text, size_t len, struct js_conf *c, char *err, size_t errlen);

struct js_state {
	int16_t axes[JS_MAX_AXES];
	uint32_t buttons;  /* bit n: button n held */
	uint32_t pressed;  /* bits newly pressed by the events applied since the last take: not by INIT events */
};

/* One event into the state. An INIT event sets the state without being a
 * press: a button held while the controller is plugged in calls nothing. */
void js_apply(struct js_state *s, const struct js_ev *e);
/* One axis, shaped: -1..1. */
float js_shape(const struct js_axis_conf *a, int16_t raw);
/* The six wrench axes in struct wrench's order, from the state. */
void js_wrench(const struct js_conf *c, const struct js_state *s, float out[JS_WRENCH_AXES]);
/* pilot.buttons' bits: the held buttons marked pass. */
uint32_t js_pass_bits(const struct js_conf *c, const struct js_state *s);
#endif
