// SPDX-License-Identifier: GPL-3.0-or-later
/* The vehicle's `mavlink` module (docs/10-mavlink.md): ArduSub's only
 * MAVLink peer. It is the ground station ArduSub knows (SYSID_MYGCS, no
 * HEARTBEAT), sends MANUAL_CONTROL at 50 Hz from the actuator gate and
 * neutral when the gate is closed, arms ArduSub on mavlink.arm only while
 * safety.state says arming, keeps the armed agreement both ways (a disarm
 * when the window closes, `disarming` until the answer, a disagreement
 * reported for the safety module's autopilot trigger), turns the telemetry
 * into topics stamped in hangar's time, reads and sets ArduSub's parameters
 * from the checked-in file, and publishes ardusub.state. The --neutral mode,
 * the manager's Neutral command, sends neutral and a disarm and exits.
 * modules/mavlink/README.md lists the deviations and what waits for the
 * bridge and the recorder. */
#include "mod.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "ardusub_param.h"
#include <arpa/inet.h>
#include "ardusub_state.h"
#include "arm_rep.h"
#include "arm_req.h"
#include "attitude.h"
#include "battery.h"
#include "depth.h"
#include "empty.h"
#include "hg_call.h"
#include "hg_gate.h"
#include "imu.h"
#include "manual_control.h"
#include "mavlink.h"
#include "mavlink_gen.h"
#include "thruster_out.h"
#include "wrench.h"

#define WHO "hangar-mavlink"
#define MANUAL_MODE 19u
#define TICK_NS 10000000ull             /* the loop's timer: 10 ms (hg_serve, the glob poll) */
#define ARM_COLLECT_AFTER_ACK_NS 500000000ull
#define DISARM_RETRY_NS 100000000ull
#define DISARM_TRIES 3
#define DISARM_REPEAT_NS 1000000000ull
#define CLOCK_BUCKETS 10               /* a 10 s window, one bucket per second */
#define RATE_REQUEST_RETRY_NS 5000000000ull
#define PARAM_RETRY_NS 300000000ull

static struct mavlink_conf C;
static struct mav_link L;
static hg_gate *G;
static hg_pub *pub_mc, *pub_state, *pub_param, *pub_att, *pub_imu, *pub_depth, *pub_bat, *pub_thr;

/* ArduSub as the module knows it. */
static struct {
	uint8_t sysid;                 /* learned from HEARTBEAT when the conf says 0 */
	int seen;                      /* at least one message */
	uint64_t last_rx_ns;
	uint8_t armed;                 /* 0, 1, 2 disarming */
	uint8_t system_status;
	uint32_t mode;
	uint32_t last_boot_ms;         /* for the restart detection */
	int have_boot_ms;
	/* The clock estimator (docs/10 "Time"): per second of hangar time, the
	 * smallest arrival - stamp offset seen. */
	int64_t bucket_min[CLOCK_BUCKETS];
	uint64_t bucket_second[CLOCK_BUCKETS];
	int have_offset;
	/* Arming. */
	uint32_t arm_attempt;
	uint8_t arm_result;            /* 255 while pending */
	char arm_reason[100];
	int collecting;                /* PreArm:/Arm: texts are being collected */
	uint64_t collect_until_ns;     /* 0: until the ack */
	char failsafe[50];
	/* The disarm hangar asked for. */
	int disarm_pending;
	int disarm_sends;
	uint64_t disarm_next_ns;
	uint64_t disarm_since_ns;
	int disagreement;              /* ArduSub armed outside the window: reported as armed, disarmed again */
	/* Parameters. */
	struct mc_param params[MC_MAX_PARAMS];
	int n_params;
	uint8_t params_state;          /* 0 not read, 1 all as the file, 2 a mismatch */
	int param_read_next;           /* the next index to request; n_params when a pass is done */
	float param_value[MC_MAX_PARAMS];
	unsigned char param_have[MC_MAX_PARAMS];
	uint64_t param_next_ns;
	int setup_pending;             /* a mavlink.setup: PARAM_SETs going out */
	int setup_next;
	char mismatch[40];
	/* Rates. */
	uint64_t rates_next_ns;
	int rates_requested;
	/* Timing of the outputs. */
	uint64_t next_mc_ns, next_state_ns;
	int mc_sent;
	int in_window;                 /* the last judgement of the safety view */
	struct hg_gate_safety safety;
	char gate_why[160];
	char status[128];
	struct ardusub_state last_published;
} A;

/* ---- Clock (docs/10 "Time") ---------------------------------------------------- */

static void clock_reset(void)
{
	for (int i = 0; i < CLOCK_BUCKETS; i++) {
		A.bucket_min[i] = INT64_MAX;
		A.bucket_second[i] = 0;
	}
	A.have_offset = 0;
}

/* Feeds an arrival of a message stamped boot_ns on ArduSub's clock; returns
 * the stamp in hangar's clock. */
static uint64_t clock_stamp(uint64_t boot_ns, uint64_t arrival_ns)
{
	int64_t off = (int64_t)arrival_ns - (int64_t)boot_ns;
	uint64_t sec = arrival_ns / 1000000000ull;
	int k = (int)(sec % CLOCK_BUCKETS);
	if (A.bucket_second[k] != sec) {
		A.bucket_second[k] = sec;
		A.bucket_min[k] = off;
	} else if (off < A.bucket_min[k]) {
		A.bucket_min[k] = off;
	}
	int64_t best = INT64_MAX;
	for (int i = 0; i < CLOCK_BUCKETS; i++)
		if (A.bucket_second[i] + CLOCK_BUCKETS > sec && A.bucket_min[i] < best)
			best = A.bucket_min[i];
	A.have_offset = 1;
	int64_t stamp = (int64_t)boot_ns + best;
	return stamp < 0 ? 0 : (uint64_t)stamp;
}

static void ardusub_restarted(const char *why)
{
	mod_log(WHO, "ArduSub restarted (%s): clock estimate reset, rates requested again, parameters read again", why);
	clock_reset();
	A.rates_requested = 0;
	A.rates_next_ns = 0;
	A.params_state = 0;
	A.param_read_next = 0;
	memset(A.param_have, 0, sizeof A.param_have);
	A.param_next_ns = 0;
}

/* ---- Sending ----------------------------------------------------------------------- */

static void send_command(uint16_t command, float p1, float p2, float p3)
{
	struct mav_command_long c;
	memset(&c, 0, sizeof c);
	c.target_system = A.sysid;
	c.target_component = MAV_COMP_ID_AUTOPILOT1;
	c.command = command;
	c.param1 = p1;
	c.param2 = p2;
	c.param3 = p3;
	MAV_SEND(&L, COMMAND_LONG, command_long, &c);
}

static void send_disarm(uint64_t now)
{
	send_command(MAV_CMD_COMPONENT_ARM_DISARM, 0, 0, 0);
	A.disarm_sends++;
	A.disarm_next_ns = now + DISARM_RETRY_NS;
}

/* A disarm hangar asks for: `disarming` until ArduSub answers (docs/10). A
 * disarm that answers a disagreement keeps reporting armed. */
static void start_disarm(uint64_t now, int disagreement)
{
	A.disarm_pending = 1;
	A.disarm_sends = 0;
	A.disarm_since_ns = now;
	A.disagreement = disagreement;
	if (!disagreement && A.armed == 1)
		A.armed = 2;
	send_disarm(now);
}

/* MANUAL_CONTROL from a wrench (docs/10 "Commands to ArduSub"), or neutral. */
static void send_manual_control(const struct wrench *w, uint64_t now, int from_gate)
{
	struct mav_manual_control m;
	memset(&m, 0, sizeof m);
	m.target = A.sysid;
	m.z = 500;
	m.enabled_extensions = 3; /* s (pitch) and t (roll) carry data */
	int finite = w != NULL;
	if (w)
		for (int a = 0; a < 6; a++)
			if (!isfinite(((const float *)w)[a]))
				finite = 0;
	if (finite) {
		m.x = (int16_t)lrintf(mod_clampf(w->surge, -1, 1) * 1000);
		m.y = (int16_t)lrintf(mod_clampf(w->sway, -1, 1) * 1000);
		m.z = (int16_t)lrintf(500 + mod_clampf(w->heave, -1, 1) * 500);
		m.r = (int16_t)lrintf(mod_clampf(w->yaw, -1, 1) * 1000);
		m.s = (int16_t)lrintf(mod_clampf(w->pitch, -1, 1) * 1000);
		m.t = (int16_t)lrintf(mod_clampf(w->roll, -1, 1) * 1000);
	}
	MAV_SEND(&L, MANUAL_CONTROL, manual_control, &m);
	A.mc_sent++;
	struct manual_control out = { .x = m.x, .y = m.y, .z = m.z, .r = m.r, .buttons = 0, .buttons2 = 0, .s = m.s, .t = m.t };
	if (from_gate && finite)
		hg_publish(pub_mc, &out, sizeof out, now); /* references ctl.wrench, the gate's read */
	else
		hg_publish_with(pub_mc, &out, sizeof out, now, HG_NO_REFS, NULL, 0);
}

static void request_rates(uint64_t now)
{
	for (int i = 0; i < C.n_telemetry; i++) {
		const struct mc_telemetry *t = &C.telemetry[i];
		if (t->rate_hz <= 0)
			continue;
		send_command(MAV_CMD_SET_MESSAGE_INTERVAL, (float)t->msgid, (float)(1e6 / t->rate_hz), 0);
	}
	A.rates_requested = 1;
	A.rates_next_ns = now + RATE_REQUEST_RETRY_NS;
}

static void publish_state(uint64_t now)
{
	struct ardusub_state s;
	memset(&s, 0, sizeof s);
	s.armed = A.armed;
	s.system_status = A.system_status;
	s.params = A.params_state;
	s.mode = A.mode;
	s.last_rx_ns = A.last_rx_ns;
	s.arm_attempt = A.arm_attempt;
	s.arm_result = A.arm_result;
	mod_fmt(s.arm_reason, sizeof s.arm_reason, "%s", A.arm_reason);
	mod_fmt(s.failsafe, sizeof s.failsafe, "%s", A.failsafe);
	if (hg_publish(pub_state, &s, sizeof s, now) < 0) /* references safety.state: the gate's read */
		mod_log(WHO, "hg_publish(ardusub.state): %s", strerror(errno));
	A.last_published = s;
	A.next_state_ns = now + 100000000ull;
	char line[128];
	mod_fmt(line, sizeof line, "ardusub %s mode %u params %u; gate %s%s%s; rx %llu rejected %llu",
		A.armed == 1 ? "armed" : A.armed == 2 ? "disarming" : "disarmed", A.mode, A.params_state, A.gate_why,
		A.mismatch[0] ? "; differs: " : "", A.mismatch, (unsigned long long)L.received, (unsigned long long)L.rejected);
	if (strcmp(line, A.status) != 0) {
		mod_fmt(A.status, sizeof A.status, "%s", line);
		hg_status(line);
	}
}

/* ---- Receiving ---------------------------------------------------------------------- */

static void param_seen(const struct mav_param_value *p, uint64_t now)
{
	char id[17];
	memcpy(id, p->param_id, 16);
	id[16] = 0;
	struct ardusub_param out;
	memset(&out, 0, sizeof out);
	mod_fmt(out.id, sizeof out.id, "%s", id);
	out.type = p->param_type;
	out.value = p->param_value;
	out.index = p->param_index;
	out.count = p->param_count;
	hg_publish_with(pub_param, &out, sizeof out, now, HG_NO_REFS, NULL, 0);
	for (int i = 0; i < A.n_params; i++) {
		if (strcmp(A.params[i].id, id) != 0)
			continue;
		A.param_have[i] = 1;
		A.param_value[i] = p->param_value;
	}
	/* Every parameter of the file read: does each read as the file says? */
	int all = 1, mismatch = -1;
	for (int i = 0; i < A.n_params; i++) {
		if (!A.param_have[i])
			all = 0;
		else if (fabsf(A.param_value[i] - A.params[i].value) > 1e-4f && mismatch < 0)
			mismatch = i;
	}
	if (mismatch >= 0) {
		A.params_state = 2;
		mod_fmt(A.mismatch, sizeof A.mismatch, "%s=%g, file %g", A.params[mismatch].id,
			(double)A.param_value[mismatch], (double)A.params[mismatch].value);
	} else if (all && A.n_params) {
		A.params_state = 1;
		A.mismatch[0] = 0;
	}
}

static void on_frame(void *ctx, const struct mav_frame *f)
{
	uint64_t now = *(uint64_t *)ctx;
	if (C.target_sysid && f->sysid != C.target_sysid)
		return; /* not ArduSub: another system on the link */
	if (!A.seen && !C.target_sysid)
		A.sysid = f->sysid;
	A.seen = 1;
	A.last_rx_ns = now;
	switch (f->msgid) {
	case MAV_MSG_ID_HEARTBEAT: {
		struct mav_heartbeat h;
		mav_unpack_heartbeat(f->payload, f->len, &h);
		if (h.type == MAV_TYPE_GCS)
			break;
		int armed = (h.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
		A.mode = h.custom_mode;
		A.system_status = h.system_status;
		if (!armed) {
			A.armed = 0;
			A.disarm_pending = 0;
			A.disagreement = 0;
		} else if (A.disarm_pending && !A.disagreement && A.disarm_sends <= DISARM_TRIES) {
			A.armed = 2; /* disarming: hangar asked, ArduSub has not answered yet */
		} else {
			A.armed = 1;
		}
		break;
	}
	case MAV_MSG_ID_COMMAND_ACK: {
		struct mav_command_ack a;
		mav_unpack_command_ack(f->payload, f->len, &a);
		if (a.command == MAV_CMD_COMPONENT_ARM_DISARM) {
			if (A.arm_result == 255 && A.collecting && !A.collect_until_ns) {
				A.arm_result = a.result;
				A.collect_until_ns = now + ARM_COLLECT_AFTER_ACK_NS;
				if (!A.arm_reason[0]) {
					static const char *const names[] = { "MAV_RESULT_ACCEPTED", "MAV_RESULT_TEMPORARILY_REJECTED",
									     "MAV_RESULT_DENIED", "MAV_RESULT_UNSUPPORTED",
									     "MAV_RESULT_FAILED", "MAV_RESULT_IN_PROGRESS",
									     "MAV_RESULT_CANCELLED" };
					mod_fmt(A.arm_reason, sizeof A.arm_reason, "%s",
						a.result < 7 ? names[a.result] : "MAV_RESULT unknown");
				}
			}
			if (A.disarm_pending && a.result != MAV_RESULT_ACCEPTED) {
				/* A refused disarm is a disagreement: reported as armed. */
				A.disarm_sends = DISARM_TRIES + 1;
				A.disagreement = 1;
			}
		}
		break;
	}
	case MAV_MSG_ID_STATUSTEXT: {
		struct mav_statustext s;
		mav_unpack_statustext(f->payload, f->len, &s);
		char text[51];
		memcpy(text, s.text, 50);
		text[50] = 0;
		if (A.collecting && (strncmp(text, "PreArm:", 7) == 0 || strncmp(text, "Arm:", 4) == 0)) {
			size_t n = strlen(A.arm_reason);
			if (strncmp(A.arm_reason, "MAV_RESULT", 10) == 0)
				n = 0; /* the texts replace the bare result's name */
			mod_fmt(A.arm_reason + n, sizeof A.arm_reason - n, "%s%s", n ? "; " : "", text);
		}
		if (s.severity <= MAV_SEVERITY_CRITICAL || strstr(text, "ailsafe") || strstr(text, "Leak"))
			mod_fmt(A.failsafe, sizeof A.failsafe, "%s", text);
		break;
	}
	case MAV_MSG_ID_PARAM_VALUE: {
		struct mav_param_value p;
		mav_unpack_param_value(f->payload, f->len, &p);
		param_seen(&p, now);
		break;
	}
	case MAV_MSG_ID_ATTITUDE: {
		struct mav_attitude m;
		mav_unpack_attitude(f->payload, f->len, &m);
		if (A.have_boot_ms && m.time_boot_ms + 1000 < A.last_boot_ms)
			ardusub_restarted("time_boot_ms went backwards");
		A.last_boot_ms = m.time_boot_ms;
		A.have_boot_ms = 1;
		struct attitude a;
		memset(&a, 0, sizeof a);
		a.roll = m.roll;
		a.pitch = m.pitch;
		a.yaw = m.yaw;
		a.rates[0] = m.rollspeed;
		a.rates[1] = m.pitchspeed;
		a.rates[2] = m.yawspeed;
		/* The quaternion from the Euler angles, body to NED (ZYX). */
		double cr = cos(m.roll / 2), sr = sin(m.roll / 2), cp = cos(m.pitch / 2), sp = sin(m.pitch / 2),
		       cy = cos(m.yaw / 2), sy = sin(m.yaw / 2);
		a.q[0] = (float)(cr * cp * cy + sr * sp * sy);
		a.q[1] = (float)(sr * cp * cy - cr * sp * sy);
		a.q[2] = (float)(cr * sp * cy + sr * cp * sy);
		a.q[3] = (float)(cr * cp * sy - sr * sp * cy);
		hg_publish_with(pub_att, &a, sizeof a, clock_stamp((uint64_t)m.time_boot_ms * 1000000u, now), HG_NO_REFS, NULL, 0);
		break;
	}
	case MAV_MSG_ID_SCALED_IMU: {
		struct mav_scaled_imu m;
		mav_unpack_scaled_imu(f->payload, f->len, &m);
		struct imu i;
		memset(&i, 0, sizeof i);
		i.accel[0] = m.xacc * 9.80665f / 1000.0f; /* mG */
		i.accel[1] = m.yacc * 9.80665f / 1000.0f;
		i.accel[2] = m.zacc * 9.80665f / 1000.0f;
		i.gyro[0] = m.xgyro / 1000.0f; /* mrad/s */
		i.gyro[1] = m.ygyro / 1000.0f;
		i.gyro[2] = m.zgyro / 1000.0f;
		i.mag[0] = m.xmag / 1000.0f; /* mgauss */
		i.mag[1] = m.ymag / 1000.0f;
		i.mag[2] = m.zmag / 1000.0f;
		i.temp_c = m.temperature / 100.0f;
		hg_publish_with(pub_imu, &i, sizeof i, clock_stamp((uint64_t)m.time_boot_ms * 1000000u, now), HG_NO_REFS, NULL, 0);
		break;
	}
	case MAV_MSG_ID_SCALED_PRESSURE2:
	case MAV_MSG_ID_SCALED_PRESSURE: {
		int want1 = C.depth_from_pressure1;
		if ((f->msgid == MAV_MSG_ID_SCALED_PRESSURE) != want1)
			break;
		struct mav_scaled_pressure2 m; /* the two messages share one layout */
		mav_unpack_scaled_pressure2(f->payload, f->len, &m);
		struct depth d;
		d.pressure_mbar = m.press_abs;
		d.temp_c = m.temperature / 100.0f;
		d.depth_m = (float)((m.press_abs - C.surface_pressure_hpa) * 100.0 / (C.fluid_density * 9.80665));
		hg_publish_with(pub_depth, &d, sizeof d, clock_stamp((uint64_t)m.time_boot_ms * 1000000u, now), HG_NO_REFS, NULL, 0);
		break;
	}
	case MAV_MSG_ID_SYS_STATUS: {
		struct mav_sys_status m;
		mav_unpack_sys_status(f->payload, f->len, &m);
		struct battery b;
		b.voltage_v = m.voltage_battery / 1000.0f;
		b.current_a = m.current_battery < 0 ? -1.0f : m.current_battery / 100.0f;
		b.remaining = m.battery_remaining < 0 ? -1.0f : m.battery_remaining / 100.0f;
		hg_publish_with(pub_bat, &b, sizeof b, now, HG_NO_REFS, NULL, 0);
		break;
	}
	case MAV_MSG_ID_BATTERY_STATUS:
		/* SYS_STATUS carries the pack voltage ArduSub measures; BATTERY_STATUS
		 * is recorded in the tlog and adds nothing power.battery needs. */
		break;
	case MAV_MSG_ID_SERVO_OUTPUT_RAW: {
		struct mav_servo_output_raw m;
		mav_unpack_servo_output_raw(f->payload, f->len, &m);
		struct thruster_out t = { .pwm = { m.servo1_raw, m.servo2_raw, m.servo3_raw, m.servo4_raw, m.servo5_raw,
						   m.servo6_raw, m.servo7_raw, m.servo8_raw } };
		hg_publish_with(pub_thr, &t, sizeof t, clock_stamp((uint64_t)m.time_usec * 1000u, now), HG_NO_REFS, NULL, 0);
		break;
	}
	default:
		break;
	}
}

/* ---- The methods -------------------------------------------------------------------- */

static int on_arm(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	uint64_t now = hg_now_ns();
	/* docs/10 step 3: the newest safety.state says arming, ArduSub disarmed
	 * and in MANUAL, the parameters as the file says. */
	hg_gate_check(G, NULL);
	hg_gate_safety(G, &A.safety);
	if (!A.safety.present || !A.safety.fresh) {
		mod_fmt(reason, HG_CALL_REASON, "safety.state %s", A.safety.present ? "stale" : "absent");
		return HG_E_REFUSED;
	}
	if (A.safety.state != 3) {
		static const char *const names[] = { "disarmed", "armed", "failsafe", "arming" };
		mod_fmt(reason, HG_CALL_REASON, "safety.state says %s, not arming", A.safety.state < 4 ? names[A.safety.state] : "?");
		return HG_E_REFUSED;
	}
	if (!A.seen || now > A.last_rx_ns + 3000000000ull) {
		mod_fmt(reason, HG_CALL_REASON, "ArduSub silent");
		return HG_E_REFUSED;
	}
	if (A.armed != 0) {
		mod_fmt(reason, HG_CALL_REASON, "ArduSub %s", A.armed == 1 ? "already armed" : "disarming");
		return HG_E_REFUSED;
	}
	if (A.mode != MANUAL_MODE) {
		mod_fmt(reason, HG_CALL_REASON, "ArduSub mode %u is not MANUAL", A.mode);
		return HG_E_REFUSED;
	}
	if (A.params_state != 1) {
		mod_fmt(reason, HG_CALL_REASON, "ArduSub parameters %s", A.params_state == 0 ? "not read yet" : A.mismatch);
		return HG_E_REFUSED;
	}
	A.arm_attempt++;
	A.arm_result = 255;
	A.arm_reason[0] = 0;
	A.collecting = 1;
	A.collect_until_ns = 0;
	send_command(MAV_CMD_COMPONENT_ARM_DISARM, 1, 0, 0);
	((arm_rep *)rep)->accepted = 1;
	publish_state(now);
	return HG_OK;
}

static int on_setup(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	(void)rep;
	hg_gate_check(G, NULL);
	hg_gate_safety(G, &A.safety);
	if (!A.safety.present || A.safety.state != 0) {
		mod_fmt(reason, HG_CALL_REASON, "safety.state is not disarmed");
		return HG_E_REFUSED;
	}
	if (A.armed != 0) {
		mod_fmt(reason, HG_CALL_REASON, "ArduSub is armed");
		return HG_E_REFUSED;
	}
	if (!A.n_params) {
		mod_fmt(reason, HG_CALL_REASON, "no parameter file loaded");
		return HG_E_REFUSED;
	}
	A.setup_pending = 1;
	A.setup_next = 0;
	memset(A.param_have, 0, sizeof A.param_have);
	A.params_state = 0;
	A.param_read_next = A.n_params; /* the PARAM_SETs answer with PARAM_VALUEs */
	return HG_OK;
}

/* ---- The tick ------------------------------------------------------------------------ */

static void send_next_param(uint64_t now)
{
	if (A.setup_pending) {
		if (A.setup_next < A.n_params) {
			struct mav_param_set p;
			memset(&p, 0, sizeof p);
			p.target_system = A.sysid;
			p.target_component = MAV_COMP_ID_AUTOPILOT1;
			memcpy(p.param_id, A.params[A.setup_next].id, strlen(A.params[A.setup_next].id));
			p.param_value = A.params[A.setup_next].value;
			p.param_type = MAV_PARAM_TYPE_REAL32;
			MAV_SEND(&L, PARAM_SET, param_set, &p);
			A.setup_next++;
			A.param_next_ns = now + 20000000ull;
		} else {
			A.setup_pending = 0;
			A.param_read_next = 0; /* then read every one back */
			A.param_next_ns = now + PARAM_RETRY_NS;
		}
		return;
	}
	if (A.param_read_next < A.n_params) {
		/* Skip the ones already answered; request the next missing. */
		while (A.param_read_next < A.n_params && A.param_have[A.param_read_next])
			A.param_read_next++;
		if (A.param_read_next < A.n_params) {
			struct mav_param_request_read r;
			memset(&r, 0, sizeof r);
			r.target_system = A.sysid;
			r.target_component = MAV_COMP_ID_AUTOPILOT1;
			memcpy(r.param_id, A.params[A.param_read_next].id, strlen(A.params[A.param_read_next].id));
			r.param_index = -1;
			MAV_SEND(&L, PARAM_REQUEST_READ, param_request_read, &r);
			A.param_read_next++;
			A.param_next_ns = now + 50000000ull;
			return;
		}
	}
	/* A pass is over: anything unanswered is asked again after a while. */
	int missing = 0;
	for (int i = 0; i < A.n_params; i++)
		if (!A.param_have[i])
			missing = 1;
	if (missing)
		A.param_read_next = 0;
	A.param_next_ns = now + PARAM_RETRY_NS;
}

/* ArduSub's address, once learned, goes to <state dir>/peer so that the Neutral
 * command can send at once instead of waiting for a packet that may never come
 * (the manager allows it 1 s). Written once per address. */
static struct sockaddr_in peer_noted;

static void note_peer(void)
{
	if (!L.have_peer || memcmp(&L.peer, &peer_noted, sizeof L.peer) == 0)
		return;
	char dir[1024], path[1100], text[64], ip[INET_ADDRSTRLEN];
	if (mod_state_dir(dir, sizeof dir) < 0)
		return;
	mod_fmt(path, sizeof path, "%s/peer", dir);
	inet_ntop(AF_INET, &L.peer.sin_addr, ip, sizeof ip);
	mod_fmt(text, sizeof text, "%s:%u\n", ip, (unsigned)ntohs(L.peer.sin_port));
	if (mod_write_file(path, text) < 0)
		mod_log(WHO, "cannot write %s: %s", path, strerror(errno));
	else
		peer_noted = L.peer;
}

static int peer_file_read(struct sockaddr_in *out)
{
	char dir[1024], path[1100], line[64] = "";
	if (mod_state_dir(dir, sizeof dir) < 0)
		return -1;
	mod_fmt(path, sizeof path, "%s/peer", dir);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(line, sizeof line, f))
		line[0] = 0;
	fclose(f);
	char *nl = strchr(line, '\n');
	if (nl)
		*nl = 0;
	return mc_parse_addr(line, out);
}

static void tick(uint64_t now)
{
	note_peer();
	/* The gate: the command at 50 Hz, neutral otherwise; the armed agreement
	 * on every pass from the safety view the gate judged. */
	const char *why = NULL;
	const struct wrench *cmd = hg_gate_check(G, &why);
	hg_gate_safety(G, &A.safety);
	mod_fmt(A.gate_why, sizeof A.gate_why, "%s", why ? why : "");
	int window = A.safety.present && A.safety.fresh && (A.safety.state == 3 || A.safety.outputs_enabled);
	if (A.in_window && !window && A.seen && A.armed != 0 && !A.disarm_pending)
		start_disarm(now, 0); /* the window closed: hangar's own disarm, `disarming` meanwhile */
	A.in_window = window;
	if (!window && A.seen && A.armed == 1 && !A.disarm_pending) {
		/* ArduSub armed outside the window: disarmed, and reported as armed,
		 * the disagreement the safety module's autopilot trigger reads. */
		mod_log(WHO, "ArduSub armed outside hangar's window: disarming it");
		start_disarm(now, 1);
	}
	if (A.seen && A.armed == 1 && A.mode != MANUAL_MODE && !A.disarm_pending) {
		/* Out of MANUAL, neutral input is not stop: disarmed at the first
		 * such message; the safety module's autopilot trigger fires on the mode. */
		mod_log(WHO, "ArduSub in mode %u, not MANUAL: disarming it", A.mode);
		start_disarm(now, 1);
	}
	if (A.disarm_pending && now >= A.disarm_next_ns) {
		if (A.disarm_sends < DISARM_TRIES) {
			send_disarm(now);
		} else if (A.armed != 0) {
			/* Three sends unanswered: a disagreement; try again in a second. */
			if (A.armed == 2)
				mod_log(WHO, "disarm unanswered after %d sends: ArduSub reported armed", A.disarm_sends);
			A.armed = 1;
			A.disagreement = 1;
			if (now >= A.disarm_since_ns + DISARM_REPEAT_NS)
				start_disarm(now, 1);
			else
				A.disarm_next_ns = now + DISARM_RETRY_NS;
		}
	}
	/* ardusub.state on every change, beside its 10 Hz. */
	if (A.armed != A.last_published.armed || A.mode != A.last_published.mode ||
	    A.params_state != A.last_published.params || A.arm_result != A.last_published.arm_result ||
	    A.system_status != A.last_published.system_status)
		publish_state(now);
	if (now >= A.next_mc_ns) {
		A.next_mc_ns += 20000000ull;
		if (A.next_mc_ns < now)
			A.next_mc_ns = now + 20000000ull;
		if (A.seen)
			send_manual_control(cmd, now, cmd != NULL);
	}
	if (A.collecting && A.collect_until_ns && now >= A.collect_until_ns) {
		A.collecting = 0;
		publish_state(now);
	}
	if (A.seen && now >= A.rates_next_ns)
		request_rates(now);
	if (A.seen && A.n_params && now >= A.param_next_ns)
		send_next_param(now);
	if (now >= A.next_state_ns)
		publish_state(now);
}

static uint64_t now_ctx;

/* --neutral has no topics: a frame only names the peer (mav_link_receive). */
static void on_frame_neutral(void *ctx, const struct mav_frame *f)
{
	(void)ctx;
	(void)f;
}

int mavlink_vehicle_main(const char *conf_path)
{
	char err[256];
	if (mavlink_conf_load(&C, conf_path, 0, err, sizeof err) < 0) {
		fprintf(stderr, "%s: %s\n", WHO, err);
		return 78;
	}
	mod_init(WHO);
	A.n_params = mavlink_params_load(C.params_file, A.params, MC_MAX_PARAMS, err, sizeof err);
	if (A.n_params < 0) {
		mod_log(WHO, "%s (no parameter check: mavlink.arm will refuse)", err);
		A.n_params = 0;
	}
	/* SYSID_MYGCS in the file is the id the module presents. */
	for (int i = 0; i < A.n_params; i++)
		if (strcmp(A.params[i].id, "SYSID_MYGCS") == 0 || strcmp(A.params[i].id, "MAV_GCS_SYSID") == 0)
			C.sysid = (uint8_t)A.params[i].value;
	if (mav_link_open(&L, &C, err, sizeof err) < 0) {
		mod_log(WHO, "%s", err);
		return 78;
	}
	A.sysid = C.target_sysid ? C.target_sysid : 1;
	A.arm_result = 255;
	clock_reset();
	G = hg_gate_open();
	if (!G) {
		mod_log(WHO, "hg_gate_open: %s (an [Actuator] section with Command=ctl.wrench is required)", strerror(errno));
		return 4;
	}
	pub_mc = hg_pub_open("ardusub.manual_control", &MANUAL_CONTROL_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_state = hg_pub_open("ardusub.state", &ARDUSUB_STATE_TYPE, &(struct hg_pub_opts){ .rate_mhz = 10000 });
	pub_param = hg_pub_open("ardusub.param", &ARDUSUB_PARAM_TYPE, NULL);
	pub_att = hg_pub_open("nav.attitude", &ATTITUDE_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_imu = hg_pub_open("nav.imu", &IMU_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	pub_bat = hg_pub_open("power.battery", &BATTERY_TYPE, &(struct hg_pub_opts){ .rate_mhz = 2000 });
	pub_thr = hg_pub_open("thr.out", &THRUSTER_OUT_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	if (!pub_mc || !pub_state || !pub_param || !pub_att || !pub_imu || !pub_depth || !pub_bat || !pub_thr) {
		mod_log(WHO, "hg_pub_open: %s", strerror(errno));
		return 4;
	}
	if (hg_serve_method("arm", on_arm, NULL) < 0 || hg_serve_method("setup", on_setup, NULL) < 0) {
		mod_log(WHO, "hg_serve_method: %s", strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT_FD];
	int n = hg_gate_subs(G, subs, 2);
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
	mod_log(WHO, "the vehicle's MAVLink module: as system %u component %u to ArduSub (system %u), signing %s, %d parameters in %s",
		C.sysid, C.compid, A.sysid, C.signing ? "on" : "off", A.n_params, C.params_file);
	uint64_t now = hg_now_ns();
	A.next_mc_ns = now;
	A.next_state_ns = now;
	publish_state(now);
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
	/* On SIGTERM: neutral and a disarm before the exit (docs/07). */
	send_manual_control(NULL, hg_now_ns(), 0);
	if (A.seen)
		send_command(MAV_CMD_COMPONENT_ARM_DISARM, 0, 0, 0);
	mod_log(WHO, "stopping: neutral and disarm sent");
	hg_wait_fd_close();
	mav_link_close(&L);
	return 0;
}

/* The Neutral command (docs/10 "Commands to ArduSub"): neutral manual
 * control and a disarm, signed, to ArduSub's address. ArduSub is a UDP
 * client to the module's port, so its address is learned from the first
 * packet it sends (its 1 Hz HEARTBEAT), unless the configuration names it. */
int mavlink_neutral_main(const char *conf_path)
{
	char err[256];
	if (mavlink_conf_load(&C, conf_path, 0, err, sizeof err) < 0) {
		fprintf(stderr, "%s --neutral: %s\n", WHO, err);
		return 78;
	}
	int n_params = mavlink_params_load(C.params_file, A.params, MC_MAX_PARAMS, err, sizeof err);
	for (int i = 0; i < n_params; i++)
		if (strcmp(A.params[i].id, "SYSID_MYGCS") == 0 || strcmp(A.params[i].id, "MAV_GCS_SYSID") == 0)
			C.sysid = (uint8_t)A.params[i].value;
	if (mav_link_open(&L, &C, err, sizeof err) < 0) {
		fprintf(stderr, "%s --neutral: %s\n", WHO, err);
		return 1;
	}
	A.sysid = C.target_sysid ? C.target_sysid : 1;
	/* The address the module learned, if it ever heard ArduSub; else the next
	 * packet, within the manager's 1 s. */
	int from_file = 0;
	if (!L.have_peer && peer_file_read(&L.peer) == 0) {
		L.have_peer = 1;
		from_file = 1;
	}
	uint64_t deadline = hg_now_ns() + 600000000ull;
	while (!L.have_peer && hg_now_ns() < deadline) {
		struct epoll_event ev;
		int ep = epoll_create1(EPOLL_CLOEXEC);
		ev.events = EPOLLIN;
		ev.data.fd = L.fd;
		epoll_ctl(ep, EPOLL_CTL_ADD, L.fd, &ev);
		epoll_wait(ep, &ev, 1, 200);
		close(ep);
		now_ctx = hg_now_ns();
		mav_link_receive(&L, on_frame_neutral, &now_ctx);
	}
	if (!L.have_peer) {
		/* No Peer=, no address learned by the module in its lifetime, no
		 * packet now: the module never reached ArduSub, so it never
		 * commanded it, and there is nothing to make neutral. */
		fprintf(stderr, "%s --neutral: ArduSub's address is not known (no Peer=, never heard by the module, no packet within 0.6 s): nothing to neutralize\n", WHO);
		mav_link_close(&L);
		return 0;
	}
	A.seen = 1;
	(void)from_file;
	for (int i = 0; i < 3; i++) {
		struct mav_manual_control m;
		memset(&m, 0, sizeof m);
		m.target = A.sysid;
		m.z = 500;
		m.enabled_extensions = 3;
		MAV_SEND(&L, MANUAL_CONTROL, manual_control, &m);
		send_command(MAV_CMD_COMPONENT_ARM_DISARM, 0, 0, 0);
		struct timespec ts = { 0, 20000000 };
		nanosleep(&ts, NULL);
	}
	fprintf(stderr, "%s --neutral: neutral manual control and disarm sent to ArduSub (system %u), signed %s\n", WHO,
		A.sysid, C.signing ? "yes" : "no");
	mav_link_close(&L);
	return 0;
}
