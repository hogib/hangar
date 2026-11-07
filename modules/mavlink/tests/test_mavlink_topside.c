// SPDX-License-Identifier: GPL-3.0-or-later
// The topside's module under a manager against a fake QGroundControl, this
// process speaking MAVLink 2 on QGC's port (docs/10 "Tests", Topside): the
// HEARTBEAT at 1 Hz from safety.state (armed flag, CRITICAL in failsafe), its
// stop when the link is down (safety.state stale) and its return;
// MANUAL_CONTROL becoming pilot.wrench and pilot.buttons at 50 Hz, zero
// after InputTimeout; a button marked arm making one call per press, one
// marked pass setting its bit while held, an unmarked one doing nothing; an
// arm command answered ACCEPTED when safety.arm accepts and DENIED with the
// reason in a STATUSTEXT when it refuses; a mode request DENIED with
// "hangar owns modes"; PARAM_SET denied; MISSION_REQUEST_LIST a count of 0;
// ATTITUDE, depth and battery following the topics; a STATUSTEXT on every
// change of safety.state. This process also plays `safety` (serving arm and
// disarm, publishing safety.state and the telemetry topics). Under 30 s.
#include "hd_test.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>

#include "ardusub_state.h"
#include "attitude.h"
#include "battery.h"
#include "depth.h"
#include "empty.h"
#include "hangar_system.h"
#include "hg_call.h"
#include "mav.h"
#include "mavlink_gen.h"
#include "pilot_buttons.h"
#include "safety_state.h"
#include "wrench.h"

static pid_t mgr;
static const char *T_mavlink;
static int port_module, port_gcs, gcs_fd;
static char conf_path[600];
static hg_pub *P_safety, *P_att, *P_depth, *P_bat, *P_ap, *P_sys;
static hg_sub *S_wrench, *S_buttons;
static struct wrench last_w;
static struct hg_info last_w_info;
static struct pilot_buttons last_b;
static int have_w;
static uint8_t gcs_seq;

static struct {
	uint8_t state, enabled;
	char reason[64];
	int publish_safety;
	float roll, depth_m, volts;
	int arms, disarms, refuse;
	uint64_t next_safety_ns, next_tel_ns;
} T = { .publish_safety = 1, .roll = 0.2f, .depth_m = 3.5f, .volts = 15.5f };

/* What came from the module. */
static struct {
	int heartbeats, statustexts, acks, params, mission_counts, attitudes, pressures, hud, sys_status, battery;
	struct mav_heartbeat hb;
	struct mav_command_ack ack;
	char last_text[51];
	char texts[16][51];
	int n_texts;
	struct mav_attitude att;
	struct mav_scaled_pressure2 press;
	struct mav_sys_status sys;
	uint64_t last_hb_ns;
} R;

static int free_udp_port(void)
{
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0)
		return -1;
	socklen_t sl = sizeof sa;
	getsockname(s, (struct sockaddr *)&sa, &sl);
	close(s);
	return ntohs(sa.sin_port);
}

static void copy_types(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *types[] = { "ardusub_state", "attitude", "battery", "pilot_buttons", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src, types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		if (copy_file(from, to) < 0) {
			fprintf(stderr, "cannot copy %s\n", from);
			exit(2);
		}
	}
}

static void write_files(void)
{
	tfmt(conf_path, sizeof conf_path, "%s/topside_mavlink.conf", T_root);
	char text[1024];
	tfmt(text, sizeof text,
	     "[Link]\nBind=127.0.0.1:%d\nGCS=127.0.0.1:%d\nSysId=1\nCompId=1\nAutopilot=generic\nInputTimeout=500ms\n"
	     "LinkTimeout=1s\n[Button 2]\nFunction=arm\n[Button 3]\nFunction=disarm\n[Button 6]\nFunction=pass\n",
	     port_module, port_gcs);
	write_file(conf_path, text);
}

static void write_manifests(void)
{
	manifest("topside_mavlink", "[Module]\nExec=%s --topside %s\n"
				    "[Subscribe safety.state]\n[Subscribe ardusub.state]\n[Subscribe nav.attitude]\n"
				    "[Subscribe nav.depth]\n[Subscribe power.battery]\n[Subscribe hangar.vehicle.system]\n"
				    "[Publish pilot.wrench]\nType=wrench\nRate=50\n"
				    "[Publish pilot.buttons]\nType=pilot_buttons\nRate=50\n"
				    "[Call safety.arm]\n[Call safety.disarm]\n"
				    "[Restart]\nBackoff=0.1s..0.2s\n", T_mavlink, conf_path);
	/* This process: the vehicle's modules as the topside sees them, local here. */
	manifest("safety", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe pilot.wrench]\n[Subscribe pilot.buttons]\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Publish ardusub.state]\nType=ardusub_state\nRate=10\n"
			   "[Publish nav.attitude]\nType=attitude\nRate=50\n"
			   "[Publish nav.depth]\nType=depth\nRate=50\n"
			   "[Publish power.battery]\nType=battery\nRate=2\n"
			   "[Serve arm]\nRequest=empty\nReply=empty\nAllow=topside_mavlink admin\n"
			   "[Serve disarm]\nRequest=empty\nReply=empty\nAllow=* admin\n");
}

static void become(const char *module)
{
	char path[600];
	if (wiring_to_file(module, path, sizeof path) < 0) {
		printf("    cannot fetch the wiring of %s\n", module);
		exit(2);
	}
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", module, 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		printf("    hg_init as %s: %s\n", module, hg_init_error());
		exit(2);
	}
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
}

static int on_arm(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	T.arms++;
	if (strcmp(c->caller, "topside_mavlink") != 0)
		printf("    arm from %s\n", c->caller);
	if (T.refuse) {
		tfmt(reason, HG_CALL_REASON, "leak probe wet");
		return HG_E_REFUSED;
	}
	T.state = 3;
	return HG_OK;
}

static int on_disarm(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	(void)rep;
	(void)reason;
	T.disarms++;
	T.state = 0;
	T.enabled = 0;
	return HG_OK;
}

/* ---- The fake ground station ---------------------------------------------------- */

static void gcs_send(uint32_t msgid, const uint8_t *payload, size_t len, uint8_t crc)
{
	uint8_t frame[MAV_MAX_FRAME];
	size_t n = mav_encode(frame, sizeof frame, gcs_seq++, 255, MAV_COMP_ID_MISSIONPLANNER, msgid, payload, len, crc, NULL);
	struct sockaddr_in to;
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons((uint16_t)port_module);
	sendto(gcs_fd, frame, n, 0, (struct sockaddr *)&to, sizeof to);
}

#define GCS_SEND(NAME, lname, msgp)                                                        \
	do {                                                                               \
		uint8_t pl_[MAV_LEN_##NAME];                                               \
		mav_pack_##lname((msgp), pl_);                                             \
		gcs_send(MAV_MSG_ID_##NAME, pl_, sizeof pl_, MAV_CRC_##NAME);              \
	} while (0)

static void gcs_heartbeat(void)
{
	struct mav_heartbeat h = { .type = MAV_TYPE_GCS, .autopilot = MAV_AUTOPILOT_INVALID, .system_status = MAV_STATE_ACTIVE,
				   .mavlink_version = 3 };
	GCS_SEND(HEARTBEAT, heartbeat, &h);
}

static void gcs_manual_control(int16_t x, int16_t y, int16_t z, int16_t r, uint16_t buttons)
{
	struct mav_manual_control m = { .target = 1, .x = x, .y = y, .z = z, .r = r, .buttons = buttons };
	GCS_SEND(MANUAL_CONTROL, manual_control, &m);
}

static void gcs_command(uint16_t command, float p1, float p2)
{
	struct mav_command_long c;
	memset(&c, 0, sizeof c);
	c.target_system = 1;
	c.target_component = 1;
	c.command = command;
	c.param1 = p1;
	c.param2 = p2;
	GCS_SEND(COMMAND_LONG, command_long, &c);
}

static void gcs_receive(void)
{
	for (;;) {
		uint8_t buf[2048];
		ssize_t n = recv(gcs_fd, buf, sizeof buf, MSG_DONTWAIT);
		if (n < 0)
			break;
		size_t off = 0;
		while (off < (size_t)n) {
			struct mav_frame f;
			const char *why;
			size_t used = mav_decode(buf + off, (size_t)n - off, &f, NULL, &why);
			if (!used)
				break;
			off += used;
			if (!f.valid)
				continue;
			CHECK_EQ_INT(f.sysid, 1);
			switch (f.msgid) {
			case MAV_MSG_ID_HEARTBEAT:
				mav_unpack_heartbeat(f.payload, f.len, &R.hb);
				R.heartbeats++;
				R.last_hb_ns = hg_now_ns();
				break;
			case MAV_MSG_ID_STATUSTEXT: {
				struct mav_statustext s;
				mav_unpack_statustext(f.payload, f.len, &s);
				memcpy(R.last_text, s.text, 50);
				R.last_text[50] = 0;
				tfmt(R.texts[R.n_texts % 16], 51, "%s", R.last_text); /* the last 16 */
				R.n_texts++;
				R.statustexts++;
				break;
			}
			case MAV_MSG_ID_COMMAND_ACK:
				mav_unpack_command_ack(f.payload, f.len, &R.ack);
				R.acks++;
				break;
			case MAV_MSG_ID_PARAM_VALUE:
				R.params++;
				break;
			case MAV_MSG_ID_MISSION_COUNT:
				R.mission_counts++;
				break;
			case MAV_MSG_ID_ATTITUDE:
				mav_unpack_attitude(f.payload, f.len, &R.att);
				R.attitudes++;
				break;
			case MAV_MSG_ID_SCALED_PRESSURE2:
				mav_unpack_scaled_pressure2(f.payload, f.len, &R.press);
				R.pressures++;
				break;
			case MAV_MSG_ID_VFR_HUD:
				R.hud++;
				break;
			case MAV_MSG_ID_SYS_STATUS:
				mav_unpack_sys_status(f.payload, f.len, &R.sys);
				R.sys_status++;
				break;
			case MAV_MSG_ID_BATTERY_STATUS:
				R.battery++;
				break;
			default:
				break;
			}
		}
	}
}

static void read_topics(void)
{
	struct wrench w;
	struct hg_info info;
	while (hg_next(S_wrench, &w, sizeof w, &info) == HG_MSG) {
		last_w = w;
		last_w_info = info;
		have_w = 1;
	}
	struct pilot_buttons b;
	while (hg_next(S_buttons, &b, sizeof b, &info) == HG_MSG)
		last_b = b;
}

static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	hg_sub *subs[8];
	int n = hg_call_subs(subs, 6);
	if (n < 0)
		n = 0;
	subs[n++] = S_wrench;
	subs[n++] = S_buttons;
	do {
		uint64_t now = hg_now_ns();
		if (now >= T.next_safety_ns) {
			T.next_safety_ns = now + 100000000u;
			if (T.publish_safety) {
				struct safety_state s;
				memset(&s, 0, sizeof s);
				s.state = T.state;
				s.outputs_enabled = T.enabled;
				s.since_ns = now;
				tfmt(s.reason, sizeof s.reason, "%s", T.reason);
				hg_publish(P_safety, &s, sizeof s, now);
				struct ardusub_state ap;
				memset(&ap, 0, sizeof ap);
				ap.armed = T.enabled;
				ap.mode = 19;
				ap.params = 1;
				ap.last_rx_ns = now;
				hg_publish(P_ap, &ap, sizeof ap, now);
			}
		}
		if (now >= T.next_tel_ns) {
			T.next_tel_ns = now + 20000000u;
			struct attitude a;
			memset(&a, 0, sizeof a);
			a.roll = T.roll;
			a.yaw = 1.0f;
			hg_publish(P_att, &a, sizeof a, now);
			struct depth d = { .depth_m = T.depth_m, .temp_c = 12, .pressure_mbar = 1013.25f + T.depth_m * 98.0665f };
			hg_publish(P_depth, &d, sizeof d, now);
			struct battery b = { .voltage_v = T.volts, .current_a = 2.5f, .remaining = 0.7f };
			hg_publish(P_bat, &b, sizeof b, now);
		}
		hg_wait(subs, n, 5);
		hg_serve();
		gcs_receive();
		read_topics();
	} while (hg_now_ns() < end);
}

static int near(float a, float b)
{
	return fabsf(a - b) < 0.002f;
}

/* ---- Tests -------------------------------------------------------------------- */

static void test_start(void)
{
	port_module = free_udp_port();
	port_gcs = free_udp_port();
	CHECK(port_module > 0 && port_gcs > 0);
	gcs_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons((uint16_t)port_gcs);
	CHECK_EQ_INT(bind(gcs_fd, (struct sockaddr *)&sa, sizeof sa), 0);
	write_files();
	write_manifests();
	mgr = manager_start();
	CHECK(wait_state("safety", "stopped", 5000));
	become("safety");
	P_safety = hg_pub_open("safety.state", &SAFETY_STATE_TYPE, NULL);
	P_ap = hg_pub_open("ardusub.state", &ARDUSUB_STATE_TYPE, NULL);
	P_att = hg_pub_open("nav.attitude", &ATTITUDE_TYPE, NULL);
	P_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);
	P_bat = hg_pub_open("power.battery", &BATTERY_TYPE, NULL);
	S_wrench = hg_sub_open("pilot.wrench", &WRENCH_TYPE);
	S_buttons = hg_sub_open("pilot.buttons", &PILOT_BUTTONS_TYPE);
	CHECK(P_safety && P_ap && P_att && P_depth && P_bat && S_wrench && S_buttons);
	CHECK_EQ_INT(hg_serve_method("arm", on_arm, NULL), 0);
	CHECK_EQ_INT(hg_serve_method("disarm", on_disarm, NULL), 0);
	(void)P_sys;
	/* Ready: pilot.wrench and pilot.buttons at 50 Hz from the start, zero. */
	uint64_t t0 = now_ms();
	while (!wait_state("topside_mavlink", "ready", 100) && now_ms() - t0 < 8000)
		pump(50);
	CHECK(wait_state("topside_mavlink", "ready", 100));
	pump(300);
	CHECK(have_w && near(last_w.surge, 0) && last_b.pressed == 0);
	uint64_t s0 = last_w_info.seq;
	pump(1000);
	uint64_t n = last_w_info.seq - s0;
	CHECK(n >= 40 && n <= 60);
	printf("    %llu pilot.wrench in 1 s before any ground station\n", (unsigned long long)n);
	/* QGC's address is configured (its autoconnect port), so the HEARTBEAT
	 * already flows to it; what QGC sends back is read from the first packet. */
	printf("    %d heartbeats before the ground station spoke\n", R.heartbeats);
	R.heartbeats = 0;
}

static void test_heartbeat_and_telemetry(void)
{
	gcs_heartbeat();
	pump(1500);
	CHECK(R.heartbeats >= 1 && R.heartbeats <= 3);
	CHECK_EQ_INT(R.hb.type, MAV_TYPE_SUBMARINE);
	CHECK_EQ_INT(R.hb.autopilot, MAV_AUTOPILOT_GENERIC);
	CHECK_EQ_INT(R.hb.custom_mode, 19);
	CHECK(!(R.hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED));
	CHECK_EQ_INT(R.hb.system_status, MAV_STATE_STANDBY);
	/* Telemetry follows the topics. */
	CHECK(R.attitudes >= 5 && near(R.att.roll, 0.2f));
	CHECK(R.pressures >= 2 && fabsf(R.press.press_abs - (1013.25f + 3.5f * 98.0665f)) < 0.5f);
	CHECK(R.hud >= 2);
	CHECK(R.sys_status >= 1 && R.sys.voltage_battery == 15500 && R.sys.battery_remaining == 70);
	CHECK(R.battery >= 1);
	/* The first STATUSTEXT: the safety state as the module first saw it. */
	CHECK(R.statustexts >= 1);
	CHECK(strstr(R.texts[0], "hangar disarmed") != NULL);
	/* The link down: safety.state stops; the HEARTBEAT stops within a period
	 * of LinkTimeout (1 s here), and comes back with it. */
	T.publish_safety = 0;
	pump(1200);
	int hb = R.heartbeats;
	pump(1500);
	CHECK_EQ_INT(R.heartbeats, hb);
	T.publish_safety = 1;
	pump(1300);
	CHECK(R.heartbeats > hb);
}

static void test_manual_control(void)
{
	/* The axes normalized; z 0..1000 with 500 neutral; at 50 Hz. */
	for (int i = 0; i < 10; i++) {
		gcs_manual_control(300, -200, 750, 100, 0);
		pump(20);
	}
	CHECK(near(last_w.surge, 0.3f) && near(last_w.sway, -0.2f) && near(last_w.heave, 0.5f) && near(last_w.yaw, 0.1f));
	CHECK(near(last_w.roll, 0) && near(last_w.pitch, 0));
	CHECK_EQ_INT(last_b.pressed, 0);
	/* Held with no new message: zero after InputTimeout (500 ms). */
	pump(300);
	CHECK(near(last_w.surge, 0.3f));
	pump(400);
	CHECK(near(last_w.surge, 0));
	/* Buttons: 6 is pass (bit set while held), 2 arm (one call per press),
	 * 3 disarm, 5 nothing. */
	for (int i = 0; i < 5; i++) {
		gcs_manual_control(0, 0, 500, 0, 1u << 6 | 1u << 5);
		pump(20);
	}
	CHECK_EQ_INT(last_b.pressed, 1u << 6);
	CHECK_EQ_INT(T.arms, 0);
	for (int i = 0; i < 5; i++) {
		gcs_manual_control(0, 0, 500, 0, 0);
		pump(20);
	}
	CHECK_EQ_INT(last_b.pressed, 0);
	for (int i = 0; i < 5; i++) {
		gcs_manual_control(0, 0, 500, 0, 1u << 2); /* held: one press */
		pump(20);
	}
	pump(200);
	CHECK_EQ_INT(T.arms, 1);
	CHECK_EQ_INT(T.state, 3);
	T.state = 1;
	T.enabled = 1;
	pump(1200);
	CHECK(R.hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED);
	CHECK_EQ_INT(R.hb.system_status, MAV_STATE_ACTIVE);
	gcs_manual_control(0, 0, 500, 0, 0);
	pump(50);
	gcs_manual_control(0, 0, 500, 0, 1u << 3);
	pump(200);
	CHECK_EQ_INT(T.disarms, 1);
	CHECK_EQ_INT(T.state, 0);
	gcs_manual_control(0, 0, 500, 0, 0);
	pump(50);
}

static void test_commands(void)
{
	/* An arm command: ACCEPTED when safety.arm accepts. */
	R.acks = 0;
	gcs_command(MAV_CMD_COMPONENT_ARM_DISARM, 1, 0);
	pump(300);
	CHECK_EQ_INT(R.acks, 1);
	CHECK_EQ_INT(R.ack.command, MAV_CMD_COMPONENT_ARM_DISARM);
	CHECK_EQ_INT(R.ack.result, MAV_RESULT_ACCEPTED);
	CHECK_EQ_INT(T.arms, 2);
	T.state = 0;
	/* Refused: DENIED, and the reason in a STATUSTEXT. */
	T.refuse = 1;
	R.acks = 0;
	int texts = R.statustexts;
	gcs_command(MAV_CMD_COMPONENT_ARM_DISARM, 1, 0);
	pump(300);
	CHECK_EQ_INT(R.acks, 1);
	CHECK_EQ_INT(R.ack.result, MAV_RESULT_DENIED);
	CHECK(R.statustexts > texts);
	int refused_text = 0;
	for (int i = 0; i < 16 && i < R.n_texts; i++)
		if (strstr(R.texts[i], "arm refused") && strstr(R.texts[i], "leak probe wet"))
			refused_text = 1;
	CHECK(refused_text);
	T.refuse = 0;
	/* Disarm: accepted. */
	R.acks = 0;
	gcs_command(MAV_CMD_COMPONENT_ARM_DISARM, 0, 0);
	pump(300);
	CHECK_EQ_INT(R.acks, 1);
	CHECK_EQ_INT(R.ack.result, MAV_RESULT_ACCEPTED);
	CHECK_EQ_INT(T.disarms, 2);
	/* A mode request: DENIED, "hangar owns modes". */
	R.acks = 0;
	gcs_command(MAV_CMD_DO_SET_MODE, 1, 2);
	pump(300);
	CHECK_EQ_INT(R.acks, 1);
	CHECK_EQ_INT(R.ack.result, MAV_RESULT_DENIED);
	CHECK(strcmp(R.last_text, "hangar owns modes") == 0);
	struct mav_set_mode sm = { .custom_mode = 2, .target_system = 1, .base_mode = 1 };
	texts = R.statustexts;
	GCS_SEND(SET_MODE, set_mode, &sm);
	pump(300);
	CHECK(R.statustexts == texts + 1 && strcmp(R.last_text, "hangar owns modes") == 0);
	/* Any other command: DENIED. */
	R.acks = 0;
	gcs_command(MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES, 1, 0);
	pump(300);
	CHECK(R.acks == 1 && R.ack.result == MAV_RESULT_DENIED);
	/* Parameters: a small fixed list; PARAM_SET denied. */
	struct mav_param_request_list prl = { .target_system = 1, .target_component = 1 };
	GCS_SEND(PARAM_REQUEST_LIST, param_request_list, &prl);
	pump(200);
	CHECK_EQ_INT(R.params, 1);
	struct mav_param_set ps = { .param_value = 2.0f, .target_system = 1, .target_component = 1, .param_id = "HANGAR",
				    .param_type = MAV_PARAM_TYPE_REAL32 };
	GCS_SEND(PARAM_SET, param_set, &ps);
	pump(200);
	CHECK_EQ_INT(R.params, 2);
	/* Missions: none. */
	struct mav_mission_request_list mrl = { .target_system = 1, .target_component = 1 };
	GCS_SEND(MISSION_REQUEST_LIST, mission_request_list, &mrl);
	pump(200);
	CHECK_EQ_INT(R.mission_counts, 1);
	/* A STATUSTEXT for a failsafe, CRITICAL in the HEARTBEAT. */
	T.state = 2;
	tfmt(T.reason, sizeof T.reason, "leak: water at the probe");
	texts = R.statustexts;
	pump(1300);
	CHECK(R.statustexts > texts);
	int found = 0;
	for (int i = 0; i < 16 && i < R.n_texts; i++)
		if (strstr(R.texts[i], "hangar failsafe: leak"))
			found = 1;
	CHECK(found || strstr(R.last_text, "hangar failsafe: leak") != NULL);
	CHECK_EQ_INT(R.hb.system_status, MAV_STATE_CRITICAL);
	T.state = 0;
	T.reason[0] = 0;
	pump(200);
}

static void test_shutdown(void)
{
	struct mstat m;
	CHECK(module_status("topside_mavlink", &m) && strstr(m.status, "QGC connected") != NULL);
	CHECK_EQ_INT(manager_stop(mgr), 0);
	CHECK(file_contains(T_log, "topside_mavlink: pid"));
}

int main(void)
{
	T_mavlink = getenv("MAVLINK_BIN");
	if (!T_mavlink) {
		fprintf(stderr, "MAVLINK_BIN must be set\n");
		return 2;
	}
	hd_setup("mavlink_topside", NULL);
	copy_types();
	RUN_TEST(test_start);
	RUN_TEST(test_heartbeat_and_telemetry);
	RUN_TEST(test_manual_control);
	RUN_TEST(test_commands);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
