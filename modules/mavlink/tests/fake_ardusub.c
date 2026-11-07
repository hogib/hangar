// SPDX-License-Identifier: GPL-3.0-or-later
/* fake_ardusub: a stand-in for ArduSub SITL in the suites (modules/PHASE1.md:
 * "a small C program speaking just enough MAVLink"). A UDP client to the
 * vehicle module's port, as MAVProxy's --out is: it sends HEARTBEAT at 1 Hz
 * and the telemetry at the intervals MAV_CMD_SET_MESSAGE_INTERVAL asks for,
 * takes MANUAL_CONTROL from the ground station's system id only, arms and
 * disarms on MAV_CMD_COMPONENT_ARM_DISARM with COMMAND_ACK (a scripted
 * refusal sends PreArm: STATUSTEXTs first), changes mode on SET_MODE and
 * MAV_CMD_DO_SET_MODE, answers PARAM_REQUEST_READ and PARAM_SET from a small
 * table, and, given a key, requires and produces MAVLink 2 signatures. A
 * second UDP socket takes one-line commands from the suite and answers
 * "status" with everything it has seen.
 *
 *   fake_ardusub --to 127.0.0.1:14660 --control 127.0.0.1:14670 [--key FILE]
 *                [--params FILE] [--gcs-sysid 255] [--depth 2.0] [--bind host:port]
 *
 * Control lines: status | arm | disarm | mode N | refuse <text> | accept |
 * refuse-disarm on|off | silence SECONDS | boot-reset | set NAME VALUE | quit */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "mav.h"
#include "mavlink_conf.h"
#include "mavlink_gen.h"

#define MAX_PARAMS 64
#define MANUAL 19u

static struct {
	int fd, cfd;
	struct sockaddr_in to;
	struct mav_signing signing;
	uint8_t seq;
	uint8_t gcs_sysid;
	int armed;
	uint32_t mode;
	uint64_t boot_ns;
	uint64_t silent_until;
	char refuse[64];
	int refuse_disarm;
	float depth;
	struct { char id[17]; float value; } params[MAX_PARAMS];
	int n_params;
	uint64_t interval_ns[8];     /* ATTITUDE SCALED_IMU SCALED_PRESSURE2 SYS_STATUS SERVO_OUTPUT_RAW BATTERY_STATUS SCALED_PRESSURE - */
	uint64_t next_ns[8];
	uint64_t next_hb_ns;
	/* What was seen. */
	struct mav_manual_control mc;
	uint64_t mc_count, mc_ns;
	uint8_t mc_from;
	uint64_t rx, rejected, rejected_gcs, arms, disarms, params_set, setinterval, mode_sets;
	int quit;
} S;

static const uint32_t TELEMETRY[7] = { MAV_MSG_ID_ATTITUDE, MAV_MSG_ID_SCALED_IMU, MAV_MSG_ID_SCALED_PRESSURE2,
				       MAV_MSG_ID_SYS_STATUS, MAV_MSG_ID_SERVO_OUTPUT_RAW, MAV_MSG_ID_BATTERY_STATUS,
				       MAV_MSG_ID_SCALED_PRESSURE };

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t boot_ms(void)
{
	return (uint32_t)((now_ns() - S.boot_ns) / 1000000u);
}

static void send_msg(uint32_t msgid, const uint8_t *payload, size_t len, uint8_t crc_extra)
{
	if (now_ns() < S.silent_until)
		return;
	uint8_t frame[MAV_MAX_FRAME];
	size_t n = mav_encode(frame, sizeof frame, S.seq++, 1, MAV_COMP_ID_AUTOPILOT1, msgid, payload, len, crc_extra,
			      &S.signing);
	sendto(S.fd, frame, n, 0, (const struct sockaddr *)&S.to, sizeof S.to);
}

#define SEND(NAME, lname, msgp)                                                     \
	do {                                                                        \
		uint8_t pl_[MAV_LEN_##NAME];                                        \
		mav_pack_##lname((msgp), pl_);                                      \
		send_msg(MAV_MSG_ID_##NAME, pl_, sizeof pl_, MAV_CRC_##NAME);       \
	} while (0)

static void send_heartbeat(void)
{
	struct mav_heartbeat h = { .custom_mode = S.mode, .type = MAV_TYPE_SUBMARINE,
				   .autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA,
				   .base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | (S.armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0),
				   .system_status = S.armed ? MAV_STATE_ACTIVE : MAV_STATE_STANDBY, .mavlink_version = 3 };
	SEND(HEARTBEAT, heartbeat, &h);
}

static void send_statustext(uint8_t sev, const char *text)
{
	struct mav_statustext s;
	memset(&s, 0, sizeof s);
	s.severity = sev;
	snprintf(s.text, sizeof s.text, "%.49s", text);
	SEND(STATUSTEXT, statustext, &s);
}

static void send_ack(uint16_t cmd, uint8_t result)
{
	struct mav_command_ack a;
	memset(&a, 0, sizeof a);
	a.command = cmd;
	a.result = result;
	a.target_system = S.gcs_sysid;
	a.target_component = MAV_COMP_ID_MISSIONPLANNER;
	SEND(COMMAND_ACK, command_ack, &a);
}

static int param_index(const char *id)
{
	for (int i = 0; i < S.n_params; i++)
		if (strcmp(S.params[i].id, id) == 0)
			return i;
	return -1;
}

static void send_param(int i)
{
	struct mav_param_value p;
	memset(&p, 0, sizeof p);
	memcpy(p.param_id, S.params[i].id, strlen(S.params[i].id));
	p.param_value = S.params[i].value;
	p.param_type = MAV_PARAM_TYPE_REAL32;
	p.param_count = (uint16_t)S.n_params;
	p.param_index = (uint16_t)i;
	SEND(PARAM_VALUE, param_value, &p);
}

static void send_telemetry(int k)
{
	uint32_t ms = boot_ms();
	switch (TELEMETRY[k]) {
	case MAV_MSG_ID_ATTITUDE: {
		struct mav_attitude a = { .time_boot_ms = ms, .roll = 0.1f, .pitch = -0.05f, .yaw = 1.0f, .rollspeed = 0.01f,
					  .pitchspeed = 0.02f, .yawspeed = 0.03f };
		SEND(ATTITUDE, attitude, &a);
		break;
	}
	case MAV_MSG_ID_SCALED_IMU: {
		struct mav_scaled_imu m = { .time_boot_ms = ms, .zacc = -1000, .xgyro = 10, .temperature = 2500 };
		SEND(SCALED_IMU, scaled_imu, &m);
		break;
	}
	case MAV_MSG_ID_SCALED_PRESSURE2:
	case MAV_MSG_ID_SCALED_PRESSURE: {
		float press = (float)(1013.25 + S.depth * 1000 * 9.80665 / 100);
		if (TELEMETRY[k] == MAV_MSG_ID_SCALED_PRESSURE2) {
			struct mav_scaled_pressure2 p = { .time_boot_ms = ms, .press_abs = press, .temperature = 1500 };
			SEND(SCALED_PRESSURE2, scaled_pressure2, &p);
		} else {
			struct mav_scaled_pressure p = { .time_boot_ms = ms, .press_abs = press, .temperature = 1500 };
			SEND(SCALED_PRESSURE, scaled_pressure, &p);
		}
		break;
	}
	case MAV_MSG_ID_SYS_STATUS: {
		struct mav_sys_status s;
		memset(&s, 0, sizeof s);
		s.voltage_battery = 16000;
		s.current_battery = 250;
		s.battery_remaining = 80;
		SEND(SYS_STATUS, sys_status, &s);
		break;
	}
	case MAV_MSG_ID_BATTERY_STATUS: {
		struct mav_battery_status b;
		memset(&b, 0, sizeof b);
		b.voltages[0] = 16000;
		for (int i = 1; i < 10; i++)
			b.voltages[i] = UINT16_MAX;
		b.current_battery = 250;
		b.battery_remaining = 80;
		SEND(BATTERY_STATUS, battery_status, &b);
		break;
	}
	case MAV_MSG_ID_SERVO_OUTPUT_RAW: {
		/* The last manual control as pulse widths: what the thrusters got. */
		struct mav_servo_output_raw s;
		memset(&s, 0, sizeof s);
		s.time_usec = (uint32_t)ms * 1000u;
		int armed = S.armed;
		s.servo1_raw = (uint16_t)(1500 + (armed ? S.mc.x / 2 : 0));
		s.servo2_raw = (uint16_t)(1500 + (armed ? S.mc.y / 2 : 0));
		s.servo3_raw = (uint16_t)(1500 + (armed ? (S.mc.z - 500) : 0));
		s.servo4_raw = (uint16_t)(1500 + (armed ? S.mc.r / 2 : 0));
		s.servo5_raw = s.servo6_raw = s.servo7_raw = s.servo8_raw = 1500;
		SEND(SERVO_OUTPUT_RAW, servo_output_raw, &s);
		break;
	}
	default:
		break;
	}
}

static void on_frame(const struct mav_frame *f)
{
	S.rx++;
	switch (f->msgid) {
	case MAV_MSG_ID_MANUAL_CONTROL: {
		struct mav_manual_control m;
		mav_unpack_manual_control(f->payload, f->len, &m);
		if (f->sysid != S.gcs_sysid) {
			S.rejected_gcs++;
			break;
		}
		S.mc = m;
		S.mc_count++;
		S.mc_ns = now_ns();
		S.mc_from = f->sysid;
		break;
	}
	case MAV_MSG_ID_COMMAND_LONG: {
		struct mav_command_long c;
		mav_unpack_command_long(f->payload, f->len, &c);
		if (c.command == MAV_CMD_COMPONENT_ARM_DISARM) {
			if (c.param1 >= 0.5f) {
				S.arms++;
				if (S.refuse[0]) {
					send_statustext(MAV_SEVERITY_CRITICAL, S.refuse);
					send_ack(c.command, MAV_RESULT_FAILED);
					send_statustext(MAV_SEVERITY_CRITICAL, "Arm: refused by the fake");
				} else {
					S.armed = 1;
					send_ack(c.command, MAV_RESULT_ACCEPTED);
					send_heartbeat();
				}
			} else {
				S.disarms++;
				if (S.refuse_disarm) {
					send_ack(c.command, MAV_RESULT_DENIED);
				} else {
					S.armed = 0;
					send_ack(c.command, MAV_RESULT_ACCEPTED);
					send_heartbeat();
				}
			}
		} else if (c.command == MAV_CMD_SET_MESSAGE_INTERVAL) {
			S.setinterval++;
			for (int k = 0; k < 7; k++)
				if (TELEMETRY[k] == (uint32_t)c.param1)
					S.interval_ns[k] = c.param2 > 0 ? (uint64_t)(c.param2 * 1000.0f) : 0;
			send_ack(c.command, MAV_RESULT_ACCEPTED);
		} else if (c.command == MAV_CMD_DO_SET_MODE) {
			S.mode = (uint32_t)c.param2;
			S.mode_sets++;
			send_ack(c.command, MAV_RESULT_ACCEPTED);
			send_heartbeat();
		} else {
			send_ack(c.command, MAV_RESULT_UNSUPPORTED);
		}
		break;
	}
	case MAV_MSG_ID_SET_MODE: {
		struct mav_set_mode m;
		mav_unpack_set_mode(f->payload, f->len, &m);
		S.mode = m.custom_mode;
		S.mode_sets++;
		send_heartbeat();
		break;
	}
	case MAV_MSG_ID_PARAM_REQUEST_READ: {
		struct mav_param_request_read r;
		mav_unpack_param_request_read(f->payload, f->len, &r);
		char id[17];
		memcpy(id, r.param_id, 16);
		id[16] = 0;
		int i = r.param_index >= 0 ? r.param_index : param_index(id);
		if (i >= 0 && i < S.n_params)
			send_param(i);
		break;
	}
	case MAV_MSG_ID_PARAM_REQUEST_LIST:
		for (int i = 0; i < S.n_params; i++)
			send_param(i);
		break;
	case MAV_MSG_ID_PARAM_SET: {
		struct mav_param_set p;
		mav_unpack_param_set(f->payload, f->len, &p);
		char id[17];
		memcpy(id, p.param_id, 16);
		id[16] = 0;
		int i = param_index(id);
		if (i >= 0) {
			S.params[i].value = p.param_value;
			S.params_set++;
			send_param(i);
		}
		break;
	}
	default:
		break;
	}
}

static void receive(void)
{
	for (;;) {
		uint8_t buf[2048];
		ssize_t n = recv(S.fd, buf, sizeof buf, MSG_DONTWAIT);
		if (n < 0)
			break;
		size_t off = 0;
		while (off < (size_t)n) {
			struct mav_frame f;
			const char *why;
			size_t used = mav_decode(buf + off, (size_t)n - off, &f, &S.signing, &why);
			if (!used)
				break;
			off += used;
			if (!f.valid) {
				if (used > 1)
					S.rejected++;
				continue;
			}
			on_frame(&f);
		}
	}
}

static void control(void)
{
	for (;;) {
		char buf[256];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(S.cfd, buf, sizeof buf - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0)
			break;
		buf[n] = 0;
		char *nl = strchr(buf, '\n');
		if (nl)
			*nl = 0;
		char reply[512] = "ok\n";
		if (strcmp(buf, "status") == 0) {
			snprintf(reply, sizeof reply,
				 "armed=%d mode=%u mc=%d,%d,%d,%d,%u,%u,%d,%d mc_count=%llu mc_from=%u mc_age_ms=%llu "
				 "rx=%llu rejected=%llu rejected_gcs=%llu params_set=%llu setinterval=%llu arms=%llu disarms=%llu "
				 "mode_sets=%llu\n",
				 S.armed, S.mode, S.mc.x, S.mc.y, S.mc.z, S.mc.r, S.mc.buttons, S.mc.buttons2, S.mc.s, S.mc.t,
				 (unsigned long long)S.mc_count, S.mc_from,
				 (unsigned long long)(S.mc_ns ? (now_ns() - S.mc_ns) / 1000000u : 999999),
				 (unsigned long long)S.rx, (unsigned long long)S.rejected, (unsigned long long)S.rejected_gcs,
				 (unsigned long long)S.params_set, (unsigned long long)S.setinterval, (unsigned long long)S.arms,
				 (unsigned long long)S.disarms, (unsigned long long)S.mode_sets);
		} else if (strcmp(buf, "arm") == 0) {
			S.armed = 1;
			send_heartbeat();
		} else if (strcmp(buf, "disarm") == 0) {
			S.armed = 0;
			send_heartbeat();
		} else if (strncmp(buf, "mode ", 5) == 0) {
			S.mode = (uint32_t)atoi(buf + 5);
			send_heartbeat();
		} else if (strncmp(buf, "refuse ", 7) == 0) {
			snprintf(S.refuse, sizeof S.refuse, "%.63s", buf + 7);
		} else if (strcmp(buf, "accept") == 0) {
			S.refuse[0] = 0;
		} else if (strncmp(buf, "refuse-disarm ", 14) == 0) {
			S.refuse_disarm = strcmp(buf + 14, "on") == 0;
		} else if (strncmp(buf, "silence ", 8) == 0) {
			S.silent_until = now_ns() + (uint64_t)(atof(buf + 8) * 1e9);
		} else if (strcmp(buf, "boot-reset") == 0) {
			S.boot_ns = now_ns();
		} else if (strncmp(buf, "set ", 4) == 0) {
			char id[32];
			float v;
			if (sscanf(buf + 4, "%31s %f", id, &v) == 2) {
				int i = param_index(id);
				if (i >= 0)
					S.params[i].value = v;
				else if (S.n_params < MAX_PARAMS) {
					snprintf(S.params[S.n_params].id, sizeof S.params[0].id, "%.16s", id);
					S.params[S.n_params++].value = v;
				}
			}
		} else if (strcmp(buf, "quit") == 0) {
			S.quit = 1;
		} else {
			snprintf(reply, sizeof reply, "err unknown command\n");
		}
		sendto(S.cfd, reply, strlen(reply), 0, (const struct sockaddr *)&from, fl);
	}
}

static void add_param(const char *id, float v)
{
	int i = param_index(id);
	if (i < 0 && S.n_params < MAX_PARAMS) {
		i = S.n_params++;
		snprintf(S.params[i].id, sizeof S.params[i].id, "%.16s", id);
	}
	if (i >= 0)
		S.params[i].value = v;
}

int main(int argc, char **argv)
{
	const char *to = "127.0.0.1:14660", *ctl = "127.0.0.1:14670", *key = NULL, *params = NULL, *bind_at = NULL;
	S.gcs_sysid = 255;
	S.mode = MANUAL;
	S.depth = 2.0f;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--to") == 0)
			to = argv[i + 1];
		else if (strcmp(argv[i], "--control") == 0)
			ctl = argv[i + 1];
		else if (strcmp(argv[i], "--key") == 0)
			key = argv[i + 1];
		else if (strcmp(argv[i], "--params") == 0)
			params = argv[i + 1];
		else if (strcmp(argv[i], "--gcs-sysid") == 0)
			S.gcs_sysid = (uint8_t)atoi(argv[i + 1]);
		else if (strcmp(argv[i], "--depth") == 0)
			S.depth = (float)atof(argv[i + 1]);
		else if (strcmp(argv[i], "--bind") == 0)
			bind_at = argv[i + 1]; /* the MAVLink socket's own address, for a suite that sends to it */
		else {
			fprintf(stderr, "fake_ardusub: unknown option %s\n", argv[i]);
			return 2;
		}
	}
	struct sockaddr_in caddr;
	if (mc_parse_addr(to, &S.to) < 0 || mc_parse_addr(ctl, &caddr) < 0) {
		fprintf(stderr, "fake_ardusub: addresses are host:port\n");
		return 2;
	}
	/* The factory table: what SITL has before hangar ardusub-setup. */
	add_param("SYSID_MYGCS", 255);
	add_param("FS_GCS_ENABLE", 0);
	add_param("FS_LEAK_ENABLE", 0);
	add_param("FS_BATT_ENABLE", 0);
	add_param("JS_GAIN_DEFAULT", 0.5f);
	add_param("JS_GAIN_MAX", 1.0f);
	add_param("JS_GAIN_MIN", 0.25f);
	add_param("JS_GAIN_STEPS", 4);
	for (int i = 0; i < 16; i++) {
		char id[20];
		snprintf(id, sizeof id, "BTN%d_FUNCTION", i);
		add_param(id, i < 8 ? (float)(i + 1) : 0);
		snprintf(id, sizeof id, "BTN%d_SFUNCTION", i);
		add_param(id, 0);
	}
	if (params) {
		struct mc_param file[MC_MAX_PARAMS];
		char err[200];
		int n = mavlink_params_load(params, file, MC_MAX_PARAMS, err, sizeof err);
		if (n < 0) {
			fprintf(stderr, "fake_ardusub: %s\n", err);
			return 2;
		}
		for (int i = 0; i < n; i++)
			add_param(file[i].id, file[i].value);
	}
	if (key) {
		char err[200];
		S.signing.enabled = 1;
		S.signing.link_id = 2;
		if (mav_signing_load_key(&S.signing, key, err, sizeof err) < 0) {
			fprintf(stderr, "fake_ardusub: %s\n", err);
			return 2;
		}
	}
	S.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	S.cfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int one = 1;
	setsockopt(S.cfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (S.fd < 0 || S.cfd < 0 || bind(S.cfd, (const struct sockaddr *)&caddr, sizeof caddr) < 0) {
		fprintf(stderr, "fake_ardusub: sockets: %s\n", strerror(errno));
		return 1;
	}
	if (bind_at) {
		struct sockaddr_in a;
		setsockopt(S.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		if (mc_parse_addr(bind_at, &a) < 0 || bind(S.fd, (const struct sockaddr *)&a, sizeof a) < 0) {
			fprintf(stderr, "fake_ardusub: bind %s: %s\n", bind_at, strerror(errno));
			return 1;
		}
	}
	S.boot_ns = now_ns();
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	struct itimerspec its = { .it_interval = { 0, 5000000 }, .it_value = { 0, 5000000 } };
	timerfd_settime(tfd, 0, &its, NULL);
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = S.fd };
	epoll_ctl(ep, EPOLL_CTL_ADD, S.fd, &ev);
	ev.data.fd = S.cfd;
	epoll_ctl(ep, EPOLL_CTL_ADD, S.cfd, &ev);
	ev.data.fd = tfd;
	epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
	fprintf(stderr, "fake_ardusub: to %s, control %s, signing %s, %d parameters\n", to, ctl, key ? "on" : "off",
		S.n_params);
	while (!S.quit) {
		struct epoll_event evs[4];
		int k = epoll_wait(ep, evs, 4, 50);
		for (int i = 0; i < k; i++) {
			if (evs[i].data.fd == S.fd)
				receive();
			else if (evs[i].data.fd == S.cfd)
				control();
			else {
				uint64_t v;
				(void)!read(tfd, &v, sizeof v);
			}
		}
		uint64_t now = now_ns();
		if (now >= S.next_hb_ns) {
			S.next_hb_ns = now + 1000000000ull;
			send_heartbeat();
		}
		for (int t = 0; t < 7; t++) {
			if (!S.interval_ns[t])
				continue;
			if (now >= S.next_ns[t]) {
				S.next_ns[t] = now + S.interval_ns[t];
				send_telemetry(t);
			}
		}
	}
	return 0;
}
