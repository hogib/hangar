// SPDX-License-Identifier: GPL-3.0-or-later
/* The configuration of the two MAVLink modules (docs/02: a program's own
 * settings live in its own file, named on its Exec line): mavlink.conf for
 * the vehicle's module, topside_mavlink.conf for the topside's. INI like the
 * manifests, strict: an unknown section or key, or a value that does not
 * parse, refuses the file with its line. modules/mavlink/README.md lists
 * the keys. */
#ifndef HANGAR_MAVLINK_CONF_H
#define HANGAR_MAVLINK_CONF_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#define MC_PATH 512
#define MC_TELEMETRY 8
#define MC_BUTTONS 32

/* The telemetry table of docs/10: a MAVLink message, the topic it becomes,
 * the rate asked of ArduSub. The messages are fixed (their topics have fixed
 * types); the configuration changes the rates. */
struct mc_telemetry {
	const char *message;
	const char *topic;
	uint32_t msgid;
	double rate_hz;       /* 0: not requested */
};

enum mc_button { MC_BTN_NONE = 0, MC_BTN_ARM, MC_BTN_DISARM, MC_BTN_PASS };

struct mavlink_conf {
	/* [Link] */
	struct sockaddr_in bind;       /* the module's own socket */
	struct sockaddr_in peer;       /* vehicle: ArduSub, fixed; topside: QGroundControl */
	int have_peer;
	int signing;
	char key_file[MC_PATH];
	uint8_t sysid, compid;         /* the module's own ids */
	uint8_t target_sysid;          /* vehicle: ArduSub's; 0 learns it from its HEARTBEAT */
	uint8_t target_compid;         /* vehicle: ArduSub's component (1, the autopilot); 0 takes any */
	char tlog[MC_PATH];            /* vehicle: a .tlog path, "" for none (until the recorder) */
	int autopilot_ardupilot;       /* topside: present as ArduPilot rather than a generic autopilot */
	uint64_t input_timeout_ns;     /* topside: pilot input older than this counts as zero */
	uint64_t link_timeout_ns;      /* topside: safety.state older than this is the link down (until the bridge) */
	/* [Params] */
	char params_file[MC_PATH];
	/* [Depth] */
	double fluid_density;          /* kg/m^3 */
	double surface_pressure_hpa;
	int depth_from_pressure1;      /* SCALED_PRESSURE instead of SCALED_PRESSURE2 */
	/* [Telemetry <MESSAGE>] */
	struct mc_telemetry telemetry[MC_TELEMETRY];
	int n_telemetry;
	/* [Button n] */
	enum mc_button buttons[MC_BUTTONS];
};

/* The defaults of docs/10 for a role, then the file. 0, or -1 with one line
 * in err ("<file>:<line>: <reason>"). path may be NULL: the defaults alone. */
int mavlink_conf_load(struct mavlink_conf *c, const char *path, int topside, char *err, size_t errlen);

/* The ArduSub parameter file (docs/12 "hangar ardusub-setup"): one
 * "NAME value" per line, '#' comments. */
struct mc_param {
	char id[17];
	float value;
};
#define MC_MAX_PARAMS 64
/* The number read, or -1 with err. */
int mavlink_params_load(const char *path, struct mc_param *out, int max, char *err, size_t errlen);

/* "host:port" into an IPv4 socket address; -1 if malformed. */
int mc_parse_addr(const char *s, struct sockaddr_in *out);

#endif
