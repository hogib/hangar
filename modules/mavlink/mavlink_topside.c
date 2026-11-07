// SPDX-License-Identifier: GPL-3.0-or-later
/* The topside's `topside_mavlink` module (docs/10 "The topside's MAVLink
 * module"): what QGroundControl talks to. To QGroundControl it is the
 * vehicle: system 1, a submarine with a generic autopilot. Its MANUAL_CONTROL
 * becomes pilot.wrench (normalized, the gain not applied: the envelope is the
 * gain) and pilot.buttons at 50 Hz; its arm and disarm requests, commands and
 * buttons alike, become safety.arm and safety.disarm through hg_call_start,
 * answered with the call's outcome; a mode request is answered with the
 * STATUSTEXT "hangar owns modes"; nothing else is forwarded. Back go a
 * HEARTBEAT from safety.state, the telemetry from the mirrored topics, and a
 * STATUSTEXT for every change of safety.state and every refused call. */
#include "mod.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "ardusub_state.h"
#include "attitude.h"
#include "battery.h"
#include "depth.h"
#include "empty.h"
#include "hangar_system.h"
#include "hg_call.h"
#include "mavlink.h"
#include "mavlink_gen.h"
#include "pilot_buttons.h"
#include "safety_state.h"
#include "wrench.h"

#define WHO "hangar-topside-mavlink"
#define TICK_NS 10000000ull
#define MANUAL_MODE 19u
#define MAX_PENDING 8

static struct mavlink_conf C;
static struct mav_link L;
static hg_pub *pub_wrench, *pub_buttons;
static hg_sub *sub_safety, *sub_ardusub, *sub_att, *sub_depth, *sub_bat, *sub_system;

/* QGroundControl's input and our view of the vehicle. */
static struct {
	struct mav_manual_control mc;
	uint64_t mc_ns;                  /* 0: none yet */
	uint32_t buttons_last;           /* every button's last state, for the edges */
	int gcs_seen;
	uint8_t gcs_sysid, gcs_compid;
	uint64_t next_wrench_ns, next_hb_ns, next_att_ns, next_depth_ns, next_bat_ns;
	uint8_t hb_seq;
	struct {
		int in_use;
		hg_pending *pc;
		uint16_t command;
		int is_arm;
		const char *from;            /* "command" or "button" */
	} pending[MAX_PENDING];
	struct safety_state safety, safety_last;
	struct hg_info safety_info;
	int have_safety, had_safety;
	struct ardusub_state ap;
	struct hg_info ap_info;
	int have_ap;
	char ap_failsafe_last[50];
	struct attitude att;
	struct hg_info att_info;
	int have_att;
	struct depth depth;
	struct hg_info depth_info;
	int have_depth;
	struct battery bat;
	struct hg_info bat_info;
	int have_bat;
	struct hangar_system sys;
	struct hg_info sys_info;
	int have_sys;
	uint8_t sys_last;
	int link_up;
	uint64_t statustexts, calls, mc_count;
	char status[128];
} T;

static void send_statustext(uint8_t severity, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void send_statustext(uint8_t severity, const char *fmt, ...)
{
	struct mav_statustext s;
	memset(&s, 0, sizeof s);
	s.severity = severity;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s.text, sizeof s.text + 1 > 51 ? 50 : sizeof s.text, fmt, ap);
	va_end(ap);
	MAV_SEND(&L, STATUSTEXT, statustext, &s);
	T.statustexts++;
}

static void send_ack(uint16_t command, uint8_t result)
{
	struct mav_command_ack a;
	memset(&a, 0, sizeof a);
	a.command = command;
	a.result = result;
	a.target_system = T.gcs_sysid;
	a.target_component = T.gcs_compid;
	MAV_SEND(&L, COMMAND_ACK, command_ack, &a);
}

static void send_heartbeat(void)
{
	struct mav_heartbeat h;
	memset(&h, 0, sizeof h);
	h.type = MAV_TYPE_SUBMARINE;
	h.autopilot = C.autopilot_ardupilot ? MAV_AUTOPILOT_ARDUPILOTMEGA : MAV_AUTOPILOT_GENERIC;
	h.base_mode = MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | MAV_MODE_FLAG_MANUAL_INPUT_ENABLED;
	if (T.have_safety && T.safety.outputs_enabled)
		h.base_mode |= MAV_MODE_FLAG_SAFETY_ARMED;
	h.custom_mode = MANUAL_MODE;
	h.system_status = !T.have_safety ? MAV_STATE_UNINIT : T.safety.state == 2 ? MAV_STATE_CRITICAL
			  : T.safety.outputs_enabled ? MAV_STATE_ACTIVE : MAV_STATE_STANDBY;
	h.mavlink_version = 3;
	MAV_SEND(&L, HEARTBEAT, heartbeat, &h);
}

/* The one parameter QGroundControl is given, so that its connection completes. */
static void send_param(const char *id, float value, uint16_t index)
{
	struct mav_param_value p;
	memset(&p, 0, sizeof p);
	memcpy(p.param_id, id, strlen(id) > 16 ? 16 : strlen(id));
	p.param_value = value;
	p.param_type = MAV_PARAM_TYPE_REAL32;
	p.param_count = 1;
	p.param_index = index;
	MAV_SEND(&L, PARAM_VALUE, param_value, &p);
}

/* ---- Calls to the vehicle's safety module ------------------------------------------- */

static void start_call(int is_arm, uint16_t command, const char *from)
{
	int k = -1;
	for (int i = 0; i < MAX_PENDING; i++)
		if (!T.pending[i].in_use) {
			k = i;
			break;
		}
	if (k < 0) {
		send_statustext(MAV_SEVERITY_WARNING, "hangar: too many requests in flight");
		if (command)
			send_ack(command, MAV_RESULT_TEMPORARILY_REJECTED);
		return;
	}
	empty req = { 0 };
	hg_pending *pc;
	int st = hg_call_start(is_arm ? "safety.arm" : "safety.disarm", &EMPTY_TYPE, &req, &EMPTY_TYPE, &pc, 3000);
	T.calls++;
	if (st != HG_OK) {
		send_statustext(MAV_SEVERITY_WARNING, "hangar: %s: %s", is_arm ? "arm" : "disarm", hg_status_name(st));
		if (command)
			send_ack(command, MAV_RESULT_DENIED);
		return;
	}
	T.pending[k].in_use = 1;
	T.pending[k].pc = pc;
	T.pending[k].command = command;
	T.pending[k].is_arm = is_arm;
	T.pending[k].from = from;
}

static void poll_calls(void)
{
	for (int i = 0; i < MAX_PENDING; i++) {
		if (!T.pending[i].in_use)
			continue;
		hg_call_result r = hg_call_poll(T.pending[i].pc, NULL);
		if (r.status == HG_PENDING)
			continue;
		T.pending[i].in_use = 0;
		const char *what = T.pending[i].is_arm ? "arm" : "disarm";
		if (r.status == HG_OK) {
			if (T.pending[i].command)
				send_ack(T.pending[i].command, MAV_RESULT_ACCEPTED);
			mod_log(WHO, "%s (%s): accepted", what, T.pending[i].from);
		} else {
			if (T.pending[i].command)
				send_ack(T.pending[i].command, MAV_RESULT_DENIED);
			send_statustext(MAV_SEVERITY_WARNING, "hangar %s refused: %.30s", what, r.reason[0] ? r.reason : hg_status_name(r.status));
			mod_log(WHO, "%s (%s): %s %s", what, T.pending[i].from, hg_status_name(r.status), r.reason);
		}
	}
}

/* ---- From QGroundControl --------------------------------------------------------------- */

static void on_frame(void *ctx, const struct mav_frame *f)
{
	uint64_t now = *(uint64_t *)ctx;
	if (!T.gcs_seen) {
		T.gcs_seen = 1;
		T.gcs_sysid = f->sysid;
		T.gcs_compid = f->compid;
		mod_log(WHO, "ground station: system %u component %u", f->sysid, f->compid);
	}
	switch (f->msgid) {
	case MAV_MSG_ID_MANUAL_CONTROL: {
		struct mav_manual_control m;
		mav_unpack_manual_control(f->payload, f->len, &m);
		if (m.target != C.sysid && m.target != 0)
			break;
		T.mc = m;
		T.mc_ns = now;
		T.mc_count++;
		/* Buttons: an arm or a disarm once per press, pass bits in pilot.buttons. */
		uint32_t b = (uint32_t)m.buttons | (uint32_t)m.buttons2 << 16;
		uint32_t pressed = b & ~T.buttons_last;
		for (int i = 0; i < MC_BUTTONS; i++)
			if ((pressed >> i) & 1) {
				if (C.buttons[i] == MC_BTN_ARM)
					start_call(1, 0, "button");
				else if (C.buttons[i] == MC_BTN_DISARM)
					start_call(0, 0, "button");
			}
		T.buttons_last = b;
		break;
	}
	case MAV_MSG_ID_COMMAND_LONG:
	case MAV_MSG_ID_COMMAND_INT: {
		uint16_t command;
		float p1;
		if (f->msgid == MAV_MSG_ID_COMMAND_LONG) {
			struct mav_command_long c;
			mav_unpack_command_long(f->payload, f->len, &c);
			command = c.command;
			p1 = c.param1;
		} else {
			struct mav_command_int c;
			mav_unpack_command_int(f->payload, f->len, &c);
			command = c.command;
			p1 = c.param1;
		}
		if (command == MAV_CMD_COMPONENT_ARM_DISARM) {
			start_call(p1 >= 0.5f, command, "command");
		} else if (command == MAV_CMD_DO_SET_MODE) {
			send_ack(command, MAV_RESULT_DENIED);
			send_statustext(MAV_SEVERITY_NOTICE, "hangar owns modes");
		} else {
			send_ack(command, MAV_RESULT_DENIED); /* anything else: denied */
		}
		break;
	}
	case MAV_MSG_ID_SET_MODE:
		/* No COMMAND_ACK for a SET_MODE message; the STATUSTEXT says it. */
		send_statustext(MAV_SEVERITY_NOTICE, "hangar owns modes");
		break;
	case MAV_MSG_ID_PARAM_REQUEST_LIST:
	case MAV_MSG_ID_PARAM_REQUEST_READ:
		send_param("HANGAR", 1.0f, 0);
		break;
	case MAV_MSG_ID_PARAM_SET: {
		struct mav_param_set p;
		mav_unpack_param_set(f->payload, f->len, &p);
		send_param("HANGAR", 1.0f, 0); /* denied: the value stands */
		send_statustext(MAV_SEVERITY_NOTICE, "hangar: parameters are not set here");
		break;
	}
	case MAV_MSG_ID_MISSION_REQUEST_LIST: {
		struct mav_mission_count mc;
		memset(&mc, 0, sizeof mc);
		mc.target_system = T.gcs_sysid;
		mc.target_component = T.gcs_compid;
		MAV_SEND(&L, MISSION_COUNT, mission_count, &mc);
		break;
	}
	default:
		break; /* HEARTBEAT, REQUEST_DATA_STREAM and the rest: dropped */
	}
}

/* ---- To QGroundControl and the topics --------------------------------------------------- */

static void publish_pilot(uint64_t now)
{
	struct wrench w = { 0 };
	struct pilot_buttons b = { 0 };
	if (T.mc_ns && now <= T.mc_ns + C.input_timeout_ns) {
		w.surge = mod_clampf(T.mc.x / 1000.0f, -1, 1);
		w.sway = mod_clampf(T.mc.y / 1000.0f, -1, 1);
		/* QGroundControl sends z as 0..1000 with 500 neutral for ArduSub. */
		w.heave = mod_clampf((T.mc.z - 500) / 500.0f, -1, 1);
		w.yaw = mod_clampf(T.mc.r / 1000.0f, -1, 1);
		w.pitch = (T.mc.enabled_extensions & 1) ? mod_clampf(T.mc.s / 1000.0f, -1, 1) : 0;
		w.roll = (T.mc.enabled_extensions & 2) ? mod_clampf(T.mc.t / 1000.0f, -1, 1) : 0;
		uint32_t bits = (uint32_t)T.mc.buttons | (uint32_t)T.mc.buttons2 << 16;
		for (int i = 0; i < MC_BUTTONS; i++)
			if (C.buttons[i] == MC_BTN_PASS && ((bits >> i) & 1))
				b.pressed |= 1u << i;
	}
	hg_publish_with(pub_wrench, &w, sizeof w, now, HG_NO_REFS, NULL, 0);
	hg_publish_with(pub_buttons, &b, sizeof b, now, HG_NO_REFS, NULL, 0);
}

static void read_topics(uint64_t now)
{
	mod_latest(sub_safety, &T.safety, sizeof T.safety, &T.safety_info, &T.have_safety);
	mod_latest(sub_ardusub, &T.ap, sizeof T.ap, &T.ap_info, &T.have_ap);
	mod_latest(sub_att, &T.att, sizeof T.att, &T.att_info, &T.have_att);
	mod_latest(sub_depth, &T.depth, sizeof T.depth, &T.depth_info, &T.have_depth);
	mod_latest(sub_bat, &T.bat, sizeof T.bat, &T.bat_info, &T.have_bat);
	mod_latest(sub_system, &T.sys, sizeof T.sys, &T.sys_info, &T.have_sys);
	/* until the bridge: the link is up while safety.state keeps arriving. */
	T.link_up = T.have_safety && now <= T.safety_info.publish_ns + C.link_timeout_ns;
	/* STATUSTEXT on every change of safety.state, with its reason. */
	if (T.have_safety && (!T.had_safety || T.safety.state != T.safety_last.state ||
			      T.safety.triggers != T.safety_last.triggers ||
			      strncmp(T.safety.reason, T.safety_last.reason, sizeof T.safety.reason) != 0)) {
		static const char *const names[] = { "disarmed", "armed", "failsafe", "arming" };
		if (T.link_up && L.have_peer)
			send_statustext(T.safety.state == 2 ? MAV_SEVERITY_CRITICAL : MAV_SEVERITY_INFO, "hangar %s%s%.30s",
					T.safety.state < 4 ? names[T.safety.state] : "?", T.safety.reason[0] ? ": " : "",
					T.safety.reason);
		T.safety_last = T.safety;
		T.had_safety = 1;
	}
	if (T.have_sys && T.sys.state != T.sys_last) {
		if (T.sys.state != 0 && T.link_up && L.have_peer)
			send_statustext(MAV_SEVERITY_WARNING, "hangar vehicle %s: %.30s", T.sys.state == 1 ? "degraded" : "failsafe",
					T.sys.reason);
		T.sys_last = T.sys.state;
	}
	if (T.have_ap && strncmp(T.ap.failsafe, T.ap_failsafe_last, sizeof T.ap.failsafe) != 0) {
		if (T.ap.failsafe[0] && T.link_up && L.have_peer)
			send_statustext(MAV_SEVERITY_CRITICAL, "ArduSub: %.40s", T.ap.failsafe);
		memcpy(T.ap_failsafe_last, T.ap.failsafe, sizeof T.ap_failsafe_last);
	}
}

static void tick(uint64_t now)
{
	poll_calls();
	read_topics(now);
	if (now >= T.next_wrench_ns) {
		T.next_wrench_ns += 20000000ull;
		if (T.next_wrench_ns < now)
			T.next_wrench_ns = now + 20000000ull;
		publish_pilot(now);
	}
	if (!L.have_peer)
		goto status;
	if (T.link_up && now >= T.next_hb_ns) {
		T.next_hb_ns = now + 1000000000ull;
		send_heartbeat();
	}
	if (T.link_up && T.have_att && now >= T.next_att_ns) {
		T.next_att_ns = now + 100000000ull; /* 10 Hz to the HUD */
		struct mav_attitude a;
		memset(&a, 0, sizeof a);
		a.time_boot_ms = (uint32_t)(now / 1000000u);
		a.roll = T.att.roll;
		a.pitch = T.att.pitch;
		a.yaw = T.att.yaw;
		a.rollspeed = T.att.rates[0];
		a.pitchspeed = T.att.rates[1];
		a.yawspeed = T.att.rates[2];
		MAV_SEND(&L, ATTITUDE, attitude, &a);
	}
	if (T.link_up && T.have_depth && now >= T.next_depth_ns) {
		T.next_depth_ns = now + 200000000ull;
		struct mav_scaled_pressure2 p;
		memset(&p, 0, sizeof p);
		p.time_boot_ms = (uint32_t)(now / 1000000u);
		p.press_abs = T.depth.pressure_mbar;
		p.temperature = (int16_t)(T.depth.temp_c * 100);
		MAV_SEND(&L, SCALED_PRESSURE2, scaled_pressure2, &p);
		struct mav_vfr_hud h;
		memset(&h, 0, sizeof h);
		h.alt = -T.depth.depth_m;
		h.heading = T.have_att ? (int16_t)fmodf(T.att.yaw * 57.29578f + 360.0f, 360.0f) : 0;
		MAV_SEND(&L, VFR_HUD, vfr_hud, &h);
	}
	if (T.link_up && T.have_bat && now >= T.next_bat_ns) {
		T.next_bat_ns = now + 500000000ull;
		struct mav_sys_status s;
		memset(&s, 0, sizeof s);
		s.voltage_battery = (uint16_t)(T.bat.voltage_v * 1000);
		s.current_battery = T.bat.current_a < 0 ? -1 : (int16_t)(T.bat.current_a * 100);
		s.battery_remaining = T.bat.remaining < 0 ? -1 : (int8_t)(T.bat.remaining * 100);
		s.onboard_control_sensors_present = s.onboard_control_sensors_enabled = s.onboard_control_sensors_health =
			MAV_SYS_STATUS_SENSOR_BATTERY;
		MAV_SEND(&L, SYS_STATUS, sys_status, &s);
		struct mav_battery_status b;
		memset(&b, 0, sizeof b);
		b.battery_function = MAV_BATTERY_FUNCTION_ALL;
		b.type = MAV_BATTERY_TYPE_LIPO;
		for (int i = 0; i < 10; i++)
			b.voltages[i] = UINT16_MAX;
		b.voltages[0] = (uint16_t)(T.bat.voltage_v * 1000);
		b.current_battery = s.current_battery;
		b.battery_remaining = s.battery_remaining;
		b.temperature = INT16_MAX;
		b.current_consumed = -1;
		b.energy_consumed = -1;
		MAV_SEND(&L, BATTERY_STATUS, battery_status, &b);
	}
status:;
	char line[128];
	mod_fmt(line, sizeof line, "%s; link %s; %llu manual_control, %llu calls, %llu texts", T.gcs_seen ? "QGC connected" : "no QGC yet",
		T.link_up ? "up" : "down", (unsigned long long)T.mc_count, (unsigned long long)T.calls,
		(unsigned long long)T.statustexts);
	if (strcmp(line, T.status) != 0) {
		mod_fmt(T.status, sizeof T.status, "%s", line);
		hg_status(line);
	}
}

static uint64_t now_ctx;

int mavlink_topside_main(const char *conf_path)
{
	char err[256];
	if (mavlink_conf_load(&C, conf_path, 1, err, sizeof err) < 0) {
		fprintf(stderr, "%s: %s\n", WHO, err);
		return 78;
	}
	mod_init(WHO);
	if (mav_link_open(&L, &C, err, sizeof err) < 0) {
		mod_log(WHO, "%s", err);
		return 78;
	}
	sub_safety = hg_sub_open("safety.state", &SAFETY_STATE_TYPE);
	sub_ardusub = hg_sub_open("ardusub.state", &ARDUSUB_STATE_TYPE);
	sub_att = hg_sub_open("nav.attitude", &ATTITUDE_TYPE);
	sub_depth = hg_sub_open("nav.depth", &DEPTH_TYPE);
	sub_bat = hg_sub_open("power.battery", &BATTERY_TYPE);
	sub_system = hg_sub_open("hangar.vehicle.system", &HANGAR_SYSTEM_TYPE);
	if (!sub_safety || !sub_ardusub || !sub_att || !sub_depth || !sub_bat || !sub_system) {
		mod_log(WHO, "hg_sub_open: %s", strerror(errno));
		return 4;
	}
	pub_wrench = hg_pub_open("pilot.wrench", &WRENCH_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_buttons = hg_pub_open("pilot.buttons", &PILOT_BUTTONS_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	if (!pub_wrench || !pub_buttons) {
		mod_log(WHO, "hg_pub_open: %s", strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT_FD];
	int n = 0;
	subs[n++] = sub_safety;
	subs[n++] = sub_ardusub;
	subs[n++] = sub_att;
	subs[n++] = sub_depth;
	subs[n++] = sub_bat;
	subs[n++] = sub_system;
	int nc = hg_call_subs(subs + n, HG_MAX_WAIT_FD - n);
	if (nc < 0) {
		mod_log(WHO, "hg_call_subs: %s", strerror(errno));
		return 4;
	}
	n += nc;
	int efd = hg_wait_fd(subs, n);
	if (efd < 0) {
		mod_log(WHO, "hg_wait_fd: %s", strerror(errno));
		return 4;
	}
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	struct itimerspec its = { .it_interval = { 0, (long)TICK_NS }, .it_value = { 0, (long)TICK_NS } };
	timerfd_settime(tfd, 0, &its, NULL);
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = L.fd };
	epoll_ctl(ep, EPOLL_CTL_ADD, L.fd, &ev);
	ev.data.fd = efd;
	epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
	ev.data.fd = tfd;
	epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
	mod_log(WHO, "the topside's MAVLink module: system %u to QGroundControl, %s autopilot", C.sysid,
		C.autopilot_ardupilot ? "ArduPilot" : "generic");
	uint64_t now = hg_now_ns();
	T.next_wrench_ns = now;
	while (!mod_stop) {
		struct epoll_event evs[4];
		int k = epoll_wait(ep, evs, 4, 20);
		if (k < 0 && errno != EINTR)
			break;
		now = hg_now_ns();
		for (int i = 0; i < k; i++) {
			if (evs[i].data.fd == L.fd) {
				now_ctx = now;
				mav_link_receive(&L, on_frame, &now_ctx);
			} else {
				uint64_t v;
				(void)!read(evs[i].data.fd, &v, sizeof v);
			}
		}
		hg_serve();
		tick(now);
	}
	mod_log(WHO, "stopping");
	hg_wait_fd_close();
	mav_link_close(&L);
	return 0;
}
