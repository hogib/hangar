// SPDX-License-Identifier: GPL-3.0-or-later
/* The MAVLink modules' configuration files: see mavlink_conf.h. */
#define _GNU_SOURCE
#include "mavlink_conf.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hg_wiring.h"
#include "mavlink_gen.h"

/* docs/10 "Telemetry": the defaults. */
static const struct mc_telemetry DEFAULT_TELEMETRY[] = {
	{ "ATTITUDE", "nav.attitude", MAV_MSG_ID_ATTITUDE, 50 },
	{ "SCALED_IMU", "nav.imu", MAV_MSG_ID_SCALED_IMU, 50 },
	{ "SCALED_PRESSURE2", "nav.depth", MAV_MSG_ID_SCALED_PRESSURE2, 50 },
	{ "SCALED_PRESSURE", "nav.depth", MAV_MSG_ID_SCALED_PRESSURE, 0 },
	{ "SYS_STATUS", "power.battery", MAV_MSG_ID_SYS_STATUS, 2 },
	{ "BATTERY_STATUS", "power.battery", MAV_MSG_ID_BATTERY_STATUS, 2 },
	{ "SERVO_OUTPUT_RAW", "thr.out", MAV_MSG_ID_SERVO_OUTPUT_RAW, 50 },
};

int mc_parse_addr(const char *s, struct sockaddr_in *out)
{
	char host[64];
	const char *colon = strrchr(s, ':');
	if (!colon || (size_t)(colon - s) >= sizeof host)
		return -1;
	memcpy(host, s, (size_t)(colon - s));
	host[colon - s] = 0;
	char *end;
	long port = strtol(colon + 1, &end, 10);
	if (*end || port < 1 || port > 65535)
		return -1;
	memset(out, 0, sizeof *out);
	out->sin_family = AF_INET;
	out->sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &out->sin_addr) != 1)
		return -1;
	return 0;
}

static void defaults(struct mavlink_conf *c, int topside)
{
	memset(c, 0, sizeof *c);
	if (topside) {
		mc_parse_addr("127.0.0.1:14650", &c->bind);
		mc_parse_addr("127.0.0.1:14550", &c->peer);
		c->have_peer = 1;
		c->sysid = 1;
		c->compid = MAV_COMP_ID_AUTOPILOT1;
		c->input_timeout_ns = 500000000ull;
		c->link_timeout_ns = 2000000000ull;
	} else {
		mc_parse_addr("127.0.0.1:14660", &c->bind);
		c->signing = 1;
		snprintf(c->key_file, sizeof c->key_file, "/etc/hangar/mavlink.key");
		c->sysid = 255;
		c->compid = MAV_COMP_ID_MISSIONPLANNER;
		c->target_sysid = 1;
		c->target_compid = MAV_COMP_ID_AUTOPILOT1;
		snprintf(c->params_file, sizeof c->params_file, "/etc/hangar/ardusub.params");
		c->fluid_density = 1000;
		c->surface_pressure_hpa = 1013.25;
		c->n_telemetry = (int)(sizeof DEFAULT_TELEMETRY / sizeof DEFAULT_TELEMETRY[0]);
		memcpy(c->telemetry, DEFAULT_TELEMETRY, sizeof DEFAULT_TELEMETRY);
	}
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static int parse_bool(const char *v, int *out)
{
	if (strcmp(v, "yes") == 0)
		*out = 1;
	else if (strcmp(v, "no") == 0)
		*out = 0;
	else
		return -1;
	return 0;
}

static int parse_u8(const char *v, uint8_t *out)
{
	char *end;
	long n = strtol(v, &end, 10);
	if (*end || n < 0 || n > 255)
		return -1;
	*out = (uint8_t)n;
	return 0;
}

static int parse_double(const char *v, double *out)
{
	char *end;
	double d = strtod(v, &end);
	if (*end || end == v)
		return -1;
	*out = d;
	return 0;
}

#define FAIL(...)                                                                                     \
	do {                                                                                          \
		int n_ = snprintf(err, errlen, "%s:%d: ", path, line);                                \
		if (n_ > 0 && (size_t)n_ < errlen)                                                    \
			snprintf(err + n_, errlen - (size_t)n_, __VA_ARGS__);                         \
		fclose(f);                                                                            \
		return -1;                                                                            \
	} while (0)

int mavlink_conf_load(struct mavlink_conf *c, const char *path, int topside, char *err, size_t errlen)
{
	defaults(c, topside);
	if (!path)
		return 0;
	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	char buf[512], section[64] = "", arg[64] = "";
	int line = 0;
	while (fgets(buf, sizeof buf, f)) {
		line++;
		char *s = trim(buf);
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (!e || e[1])
				FAIL("malformed section header");
			*e = 0;
			char *sp = strchr(s + 1, ' ');
			arg[0] = 0;
			if (sp) {
				*sp = 0;
				snprintf(arg, sizeof arg, "%s", trim(sp + 1));
			}
			snprintf(section, sizeof section, "%s", s + 1);
			if (strcmp(section, "Link") == 0 || strcmp(section, "Params") == 0 || strcmp(section, "Depth") == 0) {
				if (arg[0])
					FAIL("[%s] takes no argument", section);
			} else if (strcmp(section, "Telemetry") == 0) {
				int k = -1;
				for (int i = 0; i < c->n_telemetry; i++)
					if (strcmp(c->telemetry[i].message, arg) == 0)
						k = i;
				if (k < 0)
					FAIL("[Telemetry %s]: not a message the module turns into a topic", arg);
			} else if (strcmp(section, "Button") == 0) {
				char *end;
				long n = strtol(arg, &end, 10);
				if (*end || n < 0 || n >= MC_BUTTONS)
					FAIL("[Button %s]: a button number 0 to %d", arg, MC_BUTTONS - 1);
			} else {
				FAIL("unknown section [%s]", section);
			}
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq)
			FAIL("expected Key=Value");
		*eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		if (strcmp(section, "Link") == 0) {
			if (strcmp(k, "Bind") == 0) {
				if (mc_parse_addr(v, &c->bind) < 0)
					FAIL("Bind: host:port");
			} else if (strcmp(k, "Peer") == 0 || strcmp(k, "GCS") == 0) {
				if (!*v) {
					c->have_peer = 0;
				} else if (mc_parse_addr(v, &c->peer) < 0) {
					FAIL("%s: host:port", k);
				} else {
					c->have_peer = 1;
				}
			} else if (strcmp(k, "Signing") == 0) {
				if (parse_bool(v, &c->signing) < 0)
					FAIL("Signing: yes or no");
			} else if (strcmp(k, "KeyFile") == 0) {
				snprintf(c->key_file, sizeof c->key_file, "%s", v);
			} else if (strcmp(k, "SysId") == 0) {
				if (parse_u8(v, &c->sysid) < 0 || c->sysid == 0)
					FAIL("SysId: 1 to 255");
			} else if (strcmp(k, "CompId") == 0) {
				if (parse_u8(v, &c->compid) < 0)
					FAIL("CompId: 0 to 255");
			} else if (strcmp(k, "TargetSysId") == 0) {
				if (parse_u8(v, &c->target_sysid) < 0)
					FAIL("TargetSysId: 0 to 255");
			} else if (strcmp(k, "TargetCompId") == 0) {
				if (parse_u8(v, &c->target_compid) < 0)
					FAIL("TargetCompId: 0 to 255");
			} else if (strcmp(k, "Tlog") == 0) {
				snprintf(c->tlog, sizeof c->tlog, "%s", v);
			} else if (strcmp(k, "Autopilot") == 0) {
				if (strcmp(v, "generic") == 0)
					c->autopilot_ardupilot = 0;
				else if (strcmp(v, "ardupilot") == 0)
					c->autopilot_ardupilot = 1;
				else
					FAIL("Autopilot: generic or ardupilot");
			} else if (strcmp(k, "InputTimeout") == 0 || strcmp(k, "LinkTimeout") == 0) {
				int64_t d = hg_parse_duration(v);
				if (d <= 0)
					FAIL("%s: a duration", k);
				*(k[0] == 'I' ? &c->input_timeout_ns : &c->link_timeout_ns) = (uint64_t)d;
			} else {
				FAIL("unknown key %s in [Link]", k);
			}
		} else if (strcmp(section, "Params") == 0) {
			if (strcmp(k, "File") == 0)
				snprintf(c->params_file, sizeof c->params_file, "%s", v);
			else
				FAIL("unknown key %s in [Params]", k);
		} else if (strcmp(section, "Depth") == 0) {
			if (strcmp(k, "FluidDensity") == 0) {
				if (parse_double(v, &c->fluid_density) < 0 || c->fluid_density <= 0)
					FAIL("FluidDensity: kg/m^3");
			} else if (strcmp(k, "SurfacePressure") == 0) {
				if (parse_double(v, &c->surface_pressure_hpa) < 0)
					FAIL("SurfacePressure: hPa");
			} else if (strcmp(k, "From") == 0) {
				if (strcmp(v, "SCALED_PRESSURE2") == 0)
					c->depth_from_pressure1 = 0;
				else if (strcmp(v, "SCALED_PRESSURE") == 0)
					c->depth_from_pressure1 = 1;
				else
					FAIL("From: SCALED_PRESSURE2 or SCALED_PRESSURE");
			} else {
				FAIL("unknown key %s in [Depth]", k);
			}
		} else if (strcmp(section, "Telemetry") == 0) {
			if (strcmp(k, "Rate") != 0)
				FAIL("unknown key %s in [Telemetry]", k);
			double r;
			if (parse_double(v, &r) < 0 || r < 0 || r > 200)
				FAIL("Rate: 0 to 200 Hz");
			for (int i = 0; i < c->n_telemetry; i++)
				if (strcmp(c->telemetry[i].message, arg) == 0)
					c->telemetry[i].rate_hz = r;
		} else if (strcmp(section, "Button") == 0) {
			if (strcmp(k, "Function") != 0)
				FAIL("unknown key %s in [Button]", k);
			enum mc_button b;
			if (strcmp(v, "arm") == 0)
				b = MC_BTN_ARM;
			else if (strcmp(v, "disarm") == 0)
				b = MC_BTN_DISARM;
			else if (strcmp(v, "pass") == 0)
				b = MC_BTN_PASS;
			else
				FAIL("Function: arm, disarm or pass");
			c->buttons[atoi(arg)] = b;
		} else {
			FAIL("a key before any section");
		}
	}
	fclose(f);
	return 0;
}
#undef FAIL

int mavlink_params_load(const char *path, struct mc_param *out, int max, char *err, size_t errlen)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	char buf[256];
	int n = 0, line = 0;
	while (fgets(buf, sizeof buf, f)) {
		line++;
		char *s = trim(buf);
		if (!*s || *s == '#')
			continue;
		char id[32];
		char val[64];
		if (sscanf(s, "%31s %63s", id, val) != 2 || strlen(id) > 16) {
			snprintf(err, errlen, "%s:%d: expected NAME value", path, line);
			fclose(f);
			return -1;
		}
		char *end;
		double v = strtod(val, &end);
		if (*end) {
			snprintf(err, errlen, "%s:%d: %s: not a number", path, line, val);
			fclose(f);
			return -1;
		}
		if (n == max) {
			snprintf(err, errlen, "%s:%d: more than %d parameters", path, line, max);
			fclose(f);
			return -1;
		}
		snprintf(out[n].id, sizeof out[n].id, "%.16s", id);
		out[n].value = (float)v;
		n++;
	}
	fclose(f);
	return n;
}
