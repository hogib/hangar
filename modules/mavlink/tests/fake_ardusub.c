// SPDX-License-Identifier: GPL-3.0-or-later
/* fake_ardusub: a stand-in for ArduSub SITL in the suites (modules/PHASE1.md:
 * "a small C program speaking just enough MAVLink"). A UDP client to the
 * vehicle module's port, as MAVProxy's --out is: it sends HEARTBEAT at 1 Hz
 * and the telemetry at the intervals MAV_CMD_SET_MESSAGE_INTERVAL asks for,
 * takes MANUAL_CONTROL from the ground station's system id only, arms and
 * disarms on MAV_CMD_COMPONENT_ARM_DISARM with COMMAND_ACK (a scripted
 * refusal sends PreArm: STATUSTEXTs first), changes mode on SET_MODE and
 * MAV_CMD_DO_SET_MODE, answers PARAM_REQUEST_READ and PARAM_SET from a typed
 * table of about 430 parameters (as ArduSub's 1 000, smaller), streams the
 * whole table on PARAM_REQUEST_LIST at 800 a second (four every 5 ms, as a
 * real link paces it) with the indices `drop` names left out once, keeps
 * STAT_RUNTIME and BARO1_GND_PRESS changing by themselves, and, given a key, requires and produces MAVLink 2 signatures. A
 * second UDP socket takes one-line commands from the suite and answers
 * "status" with everything it has seen.
 *
 *   fake_ardusub --to 127.0.0.1:14660 --control 127.0.0.1:14670 [--key FILE]
 *                [--params FILE] [--gcs-sysid 255] [--depth 2.0] [--bind host:port]
 *   fake_ardusub --pty LINK --control ... (a serial autopilot, a Pixhawk on USB)
 *
 * With --pty the fake is the far end of a pseudo-terminal, LINK a symbolic
 * link to its slave side (what the module's Serial= names, as a udev by-id
 * link would), and MAVLink a byte stream on it. The line is raw from its
 * creation, so nothing echoes before the module opens it. The master is read
 * on the 5 ms timer, not from the epoll: with no slave open a master reports
 * a hang-up for ever.
 *
 * Control lines: status | arm | disarm | mode N | refuse <text> | accept |
 * refuse-disarm on|off | silence SECONDS | boot-reset | set NAME VALUE | quit |
 * voltage MILLIVOLTS (SYS_STATUS.voltage_battery; 65535 is "not measured") |
 * get NAME ("NAME VALUE TYPE", or "none") | drop I [J ...] (those indices left
 * out of the next list stream) | ignore-set NAME|- (PARAM_SETs of NAME answered
 * with the old value, as a refused write);
 * with --pty also: vanish SECONDS (the line goes away, as a USB unplug, and
 * comes back on a new pseudo-terminal behind the same LINK) | stopread on|off
 * (stop reading: the module's writes back up) | garbage (line noise with a
 * 0xFD in it, then a HEARTBEAT written in two halves 30 ms apart). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <termios.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "mav.h"
#include "mavlink_conf.h"
#include "mavlink_gen.h"

#define MAX_PARAMS 640
#define MAX_DROP 8
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
	struct { char id[17]; float value; uint8_t type; } params[MAX_PARAMS];
	int n_params;
	int listing, list_next;      /* a list stream under way, and its next index */
	int drop[MAX_DROP], n_drop;
	char ignore_set[17];
	uint64_t lists, reads_index, reads_name, values_sent;
	uint64_t interval_ns[8];     /* ATTITUDE SCALED_IMU SCALED_PRESSURE2 SYS_STATUS SERVO_OUTPUT_RAW BATTERY_STATUS SCALED_PRESSURE - */
	uint64_t next_ns[8];
	uint64_t next_hb_ns;
	/* What was seen. */
	struct mav_manual_control mc;
	uint64_t mc_count, mc_ns;
	uint8_t mc_from;
	uint64_t rx, rejected, rejected_gcs, arms, disarms, params_set, setinterval, mode_sets;
	int quit;
	uint16_t voltage_mv;         /* SYS_STATUS.voltage_battery */
	/* --pty */
	int serial, pty, noread;
	const char *link;
	uint64_t reopen_ns;
	uint8_t rxbuf[4 * MAV_MAX_FRAME];
	size_t rx_len;
	uint8_t half[MAV_MAX_FRAME];
	size_t half_len;
	uint64_t half_ns;
	uint64_t ptys;
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

/* A new pseudo-terminal, raw, behind S.link. */
static int open_pty(void)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0)
		return -1;
	const char *name = ptsname(m);
	int sl = name ? open(name, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
	if (sl < 0) {
		close(m);
		return -1;
	}
	struct termios t;
	tcgetattr(sl, &t);
	cfmakeraw(&t);
	tcsetattr(sl, TCSANOW, &t);
	close(sl);
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.new", S.link);
	unlink(tmp);
	if (symlink(name, tmp) < 0 || rename(tmp, S.link) < 0) {
		close(m);
		return -1;
	}
	S.pty = m;
	S.rx_len = 0;
	S.ptys++;
	return 0;
}

static void write_line(const uint8_t *p, size_t n)
{
	if (S.pty >= 0)
		(void)!write(S.pty, p, n);
}

static void send_msg(uint32_t msgid, const uint8_t *payload, size_t len, uint8_t crc_extra)
{
	if (now_ns() < S.silent_until)
		return;
	if (S.half_len)
		return; /* a frame is half written (garbage): nothing may come between its halves */
	uint8_t frame[MAV_MAX_FRAME];
	size_t n = mav_encode(frame, sizeof frame, S.seq++, 1, MAV_COMP_ID_AUTOPILOT1, msgid, payload, len, crc_extra,
			      &S.signing);
	if (S.serial)
		write_line(frame, n);
	else
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

static int int_type(uint8_t t)
{
	return t >= MAV_PARAM_TYPE_UINT8 && t <= MAV_PARAM_TYPE_INT64;
}

static void send_param(int i)
{
	struct mav_param_value p;
	memset(&p, 0, sizeof p);
	memcpy(p.param_id, S.params[i].id, strlen(S.params[i].id));
	p.param_value = S.params[i].value;
	p.param_type = S.params[i].type;
	S.values_sent++;
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
		s.voltage_battery = S.voltage_mv;
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
		if (r.param_index >= 0)
			S.reads_index++;
		else
			S.reads_name++;
		if (i >= 0 && i < S.n_params)
			send_param(i);
		break;
	}
	case MAV_MSG_ID_PARAM_REQUEST_LIST:
		S.lists++;
		S.listing = 1;
		S.list_next = 0; /* streamed from the main loop */
		break;
	case MAV_MSG_ID_PARAM_SET: {
		struct mav_param_set p;
		mav_unpack_param_set(f->payload, f->len, &p);
		char id[17];
		memcpy(id, p.param_id, 16);
		id[16] = 0;
		int i = param_index(id);
		if (i >= 0) {
			if (strcmp(id, S.ignore_set) != 0)
				S.params[i].value = int_type(S.params[i].type) ? roundf(p.param_value) : p.param_value;
			S.params_set++;
			send_param(i);
		}
		break;
	}
	default:
		break;
	}
}

/* The serial line: a byte stream, frames reassembled across reads. */
static void receive_line(void)
{
	while (S.pty >= 0 && !S.noread) {
		ssize_t r = read(S.pty, S.rxbuf + S.rx_len, sizeof S.rxbuf - S.rx_len);
		if (r <= 0)
			break; /* EAGAIN; or EIO while no slave is open */
		S.rx_len += (size_t)r;
		size_t off = 0;
		while (off < S.rx_len) {
			if (S.rxbuf[off] != MAV_STX) {
				off++;
				continue;
			}
			struct mav_frame f;
			const char *why;
			size_t used = mav_decode(S.rxbuf + off, S.rx_len - off, &f, &S.signing, &why);
			if (!used)
				break;
			if (!f.valid) {
				if (used > 1)
					S.rejected++;
				off += used > 1 && strncmp(why, "unknown", 7) == 0 ? used : 1;
				continue;
			}
			off += used;
			on_frame(&f);
		}
		memmove(S.rxbuf, S.rxbuf + off, S.rx_len - off);
		S.rx_len -= off;
	}
}

static void receive(void)
{
	if (S.serial) {
		receive_line();
		return;
	}
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
				 "mode_sets=%llu ptys=%llu n_params=%d lists=%llu reads_index=%llu reads_name=%llu values_sent=%llu\n",
				 S.armed, S.mode, S.mc.x, S.mc.y, S.mc.z, S.mc.r, S.mc.buttons, S.mc.buttons2, S.mc.s, S.mc.t,
				 (unsigned long long)S.mc_count, S.mc_from,
				 (unsigned long long)(S.mc_ns ? (now_ns() - S.mc_ns) / 1000000u : 999999),
				 (unsigned long long)S.rx, (unsigned long long)S.rejected, (unsigned long long)S.rejected_gcs,
				 (unsigned long long)S.params_set, (unsigned long long)S.setinterval, (unsigned long long)S.arms,
				 (unsigned long long)S.disarms, (unsigned long long)S.mode_sets, (unsigned long long)S.ptys,
				 S.n_params, (unsigned long long)S.lists, (unsigned long long)S.reads_index,
				 (unsigned long long)S.reads_name, (unsigned long long)S.values_sent);
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
		} else if (strncmp(buf, "voltage ", 8) == 0) {
			S.voltage_mv = (uint16_t)atoi(buf + 8);
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
		} else if (strncmp(buf, "get ", 4) == 0) {
			int i = param_index(buf + 4);
			if (i < 0)
				snprintf(reply, sizeof reply, "none\n");
			else
				snprintf(reply, sizeof reply, "%s %.9g %u\n", S.params[i].id, (double)S.params[i].value,
					 (unsigned)S.params[i].type);
		} else if (strncmp(buf, "drop ", 5) == 0) {
			S.n_drop = 0;
			for (char *t = strtok(buf + 5, " "); t && S.n_drop < MAX_DROP; t = strtok(NULL, " "))
				S.drop[S.n_drop++] = atoi(t);
		} else if (strncmp(buf, "ignore-set ", 11) == 0) {
			snprintf(S.ignore_set, sizeof S.ignore_set, "%.16s", strcmp(buf + 11, "-") == 0 ? "" : buf + 11);
		} else if (strcmp(buf, "quit") == 0) {
			S.quit = 1;
		} else if (S.serial && strncmp(buf, "vanish ", 7) == 0) {
			if (S.pty >= 0)
				close(S.pty);
			S.pty = -1;
			unlink(S.link);
			S.reopen_ns = now_ns() + (uint64_t)(atof(buf + 7) * 1e9);
		} else if (S.serial && strncmp(buf, "stopread ", 9) == 0) {
			S.noread = strcmp(buf + 9, "on") == 0;
		} else if (S.serial && strcmp(buf, "garbage") == 0) {
			static const uint8_t junk[] = { 0x00, 0x13, 0xFD, 0x07, 0x00, 0x00, 0x42, 0x01, 0x01, 0xFF, 0xFD, 0xFD,
							0x55, 0xAA, 0x0A, 0x0D };
			write_line(junk, sizeof junk);
			struct mav_heartbeat h = { .custom_mode = 77u /* a mark the suite looks for */, .type = MAV_TYPE_SUBMARINE,
						   .autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA,
						   .base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED |
								(S.armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0),
						   .system_status = MAV_STATE_STANDBY, .mavlink_version = 3 };
			uint8_t pl[MAV_LEN_HEARTBEAT], frame[MAV_MAX_FRAME];
			mav_pack_heartbeat(&h, pl);
			size_t n = mav_encode(frame, sizeof frame, S.seq++, 1, MAV_COMP_ID_AUTOPILOT1, MAV_MSG_ID_HEARTBEAT,
					      pl, sizeof pl, MAV_CRC_HEARTBEAT, &S.signing);
			write_line(frame, n / 2);
			memcpy(S.half, frame + n / 2, n - n / 2);
			S.half_len = n - n / 2;
			S.half_ns = now_ns() + 30000000ull;
		} else {
			snprintf(reply, sizeof reply, "err unknown command\n");
		}
		sendto(S.cfd, reply, strlen(reply), 0, (const struct sockaddr *)&from, fl);
	}
}

static void add_typed(const char *id, float v, uint8_t type)
{
	int i = param_index(id);
	if (i < 0 && S.n_params < MAX_PARAMS) {
		i = S.n_params++;
		snprintf(S.params[i].id, sizeof S.params[i].id, "%.16s", id);
		S.params[i].type = type;
	}
	if (i >= 0)
		S.params[i].value = v;
}

static void add_param(const char *id, float v)
{
	add_typed(id, v, MAV_PARAM_TYPE_REAL32);
}

/* The bulk of the table: groups of typed parameters, as ArduSub's are, with
 * values that print exactly and inexactly alike. */
static void add_bulk(void)
{
	static const struct { const char *prefix; int n; uint8_t type; } G[] = {
		{ "ATC_RAT", 40, MAV_PARAM_TYPE_REAL32 }, { "PSC_POS", 30, MAV_PARAM_TYPE_REAL32 },
		{ "INS_ACC", 36, MAV_PARAM_TYPE_REAL32 }, { "COMPASS", 30, MAV_PARAM_TYPE_INT8 },
		{ "SERVO", 64, MAV_PARAM_TYPE_INT16 },   { "RC", 64, MAV_PARAM_TYPE_INT16 },
		{ "EK3", 50, MAV_PARAM_TYPE_REAL32 },     { "LOG", 20, MAV_PARAM_TYPE_INT32 },
		{ "SR0", 20, MAV_PARAM_TYPE_INT16 },      { "BATT", 20, MAV_PARAM_TYPE_REAL32 },
	};
	for (size_t g = 0; g < sizeof G / sizeof G[0]; g++)
		for (int i = 0; i < G[g].n; i++) {
			char id[20];
			snprintf(id, sizeof id, "%s_X%02d", G[g].prefix, i);
			float v = int_type(G[g].type) ? (float)((i * 7 + (int)g) % 100) : 0.1f * (float)i + 0.0123456f * (float)g;
			add_typed(id, v, G[g].type);
		}
	add_typed("STAT_BOOTCNT", 12, MAV_PARAM_TYPE_INT16);
	add_typed("STAT_RUNTIME", 1000, MAV_PARAM_TYPE_INT32);
	add_typed("BARO1_GND_PRESS", 101325.0f, MAV_PARAM_TYPE_REAL32);
}

/* The list stream: four parameters every 5 ms tick. */
static void stream_list(void)
{
	for (int k = 0; k < 4 && S.list_next < S.n_params; k++) {
		int i = S.list_next++, dropped = 0;
		for (int d = 0; d < S.n_drop; d++)
			if (S.drop[d] == i) {
				dropped = 1;
				S.drop[d] = -1; /* once */
			}
		if (!dropped)
			send_param(i);
	}	if (S.list_next >= S.n_params)
		S.listing = 0;
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
		else if (strcmp(argv[i], "--pty") == 0)
			S.link = argv[i + 1];
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
	add_param("MAV_GCS_SYSID", 255); /* the 4.6+ name of the same parameter */
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
	add_bulk();
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
	S.pty = -1;
	S.voltage_mv = 16000;
	if (S.link) {
		S.serial = 1;
		if (open_pty() < 0) {
			fprintf(stderr, "fake_ardusub: a pseudo-terminal at %s: %s\n", S.link, strerror(errno));
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
	fprintf(stderr, "fake_ardusub: to %s, control %s, signing %s, %d parameters\n", S.serial ? S.link : to, ctl,
		key ? "on" : "off", S.n_params);
	while (!S.quit) {
		struct epoll_event evs[4];
		int k = epoll_wait(ep, evs, 4, 50);
		for (int i = 0; i < k; i++) {
			if (evs[i].data.fd == S.fd && !S.serial)
				receive();
			else if (evs[i].data.fd == S.cfd)
				control();
			else {
				uint64_t v;
				(void)!read(tfd, &v, sizeof v);
			}
		}
		uint64_t now = now_ns();
		if (S.serial) {
			if (S.pty < 0 && S.reopen_ns && now >= S.reopen_ns) {
				S.reopen_ns = 0;
				if (open_pty() < 0)
					fprintf(stderr, "fake_ardusub: reopen: %s\n", strerror(errno));
			}
			if (S.half_len && now >= S.half_ns) {
				write_line(S.half, S.half_len);
				S.half_len = 0;
			}
			receive_line();
		}
		if (now >= S.next_hb_ns) {
			S.next_hb_ns = now + 1000000000ull;
			send_heartbeat();
			/* ArduSub's own bookkeeping moves by itself. */
			int r = param_index("STAT_RUNTIME"), b = param_index("BARO1_GND_PRESS");
			if (r >= 0)
				S.params[r].value += 1;
			if (b >= 0)
				S.params[b].value = 101325.0f + (float)(boot_ms() % 7000) / 1000.0f;
		}
		if (S.listing && now >= S.silent_until)
			stream_list();
		for (int t = 0; t < 7; t++) {
			if (!S.interval_ns[t])
				continue;
			if (now >= S.next_ns[t]) {
				S.next_ns[t] = now + S.interval_ns[t];
				send_telemetry(t);
			}
		}
	}
	if (S.link)
		unlink(S.link);
	return 0;
}
