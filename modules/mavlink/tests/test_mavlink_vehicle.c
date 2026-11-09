// SPDX-License-Identifier: GPL-3.0-or-later
// The vehicle's mavlink module under a manager against the fake ArduSub
// (docs/10 "Tests", the parts that need no SITL): signed MANUAL_CONTROL at
// 50 Hz, neutral while the gate is closed and for a non-finite axis; the
// parameters read, the mismatch named, hangar ardusub-setup (mavlink.setup)
// setting them; the arming sequence through `arming`, a PreArm refusal with
// its text, `disarming` on hangar's own disarm, a refused disarm as a
// disagreement; ArduSub armed behind hangar's back or out of MANUAL disarmed
// at once; telemetry stamped in hangar's time and continuing forward across
// an ArduSub restart, the rates requested again; silence; an unsigned frame
// refused by both ends; the Neutral command. This process plays `safety`
// (safety.state, ctl.wrench, the caller of mavlink.arm). Under 30 s.
#include "hd_test.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>

#include "ardusub_param.h"
#include "ardusub_state.h"
#include "arm_rep.h"
#include "arm_req.h"
#include "attitude.h"
#include "battery.h"
#include "depth.h"
#include "hg_call.h"
#include "manual_control.h"
#include "mav.h"
#include "mavlink_gen.h"
#include "safety_state.h"
#include "thruster_out.h"
#include "wrench.h"

static pid_t mgr, fake;
static const char *T_mavlink, *T_fake;
static int port_module, port_control, port_fake;
static char conf_path[600], key_path[600], params_path[600];
static hg_pub *P_safety, *P_cmd;
static hg_sub *S_ap, *S_mc, *S_att, *S_depth, *S_bat, *S_thr, *S_param;
static struct ardusub_state last_ap;
static struct hg_info last_ap_info;
static long watch_mode = -1; /* test_other_component: the stray component's mode, never to appear */
static int saw_other_mode;
static int have_ap, saw_disarming, saw_armed;
static struct hg_info saw_armed_info;
static struct manual_control last_mc;
static int have_mc;

static struct {
	uint8_t state, enabled;
	struct wrench cmd;
	int publish_cmd;
	uint64_t next_safety_ns, next_cmd_ns;
} T = { .publish_cmd = 1 };

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
	const char *types[] = { "manual_control", "ardusub_state", "ardusub_param", "attitude", "battery", "thruster_out",
				"arm_req", "arm_rep", NULL };
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
	tfmt(key_path, sizeof key_path, "%s/mavlink.key", T_root);
	write_file(key_path, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n");
	tfmt(params_path, sizeof params_path, "%s/ardusub.params", T_root);
	write_file(params_path, "# the suite's parameter file\nSYSID_MYGCS 255\nFS_GCS_ENABLE 1\nJS_GAIN_DEFAULT 1\nBTN0_FUNCTION 0\n");
	tfmt(conf_path, sizeof conf_path, "%s/mavlink.conf", T_root);
	char text[1024];
	tfmt(text, sizeof text,
	     "[Link]\nBind=127.0.0.1:%d\nSigning=yes\nKeyFile=%s\nSysId=255\nCompId=190\nTargetSysId=1\n"
	     "[Params]\nFile=%s\n[Depth]\nFluidDensity=1000\nSurfacePressure=1013.25\n"
	     "[Telemetry ATTITUDE]\nRate=50\n[Telemetry SYS_STATUS]\nRate=5\n",
	     port_module, key_path, params_path);
	write_file(conf_path, text);
}

static void write_manifests(void)
{
	manifest("mavlink", "[Module]\nExec=%s %s\nUser=hg-mavlink\n"
			    "[Subscribe ctl.wrench]\n[Subscribe safety.state]\n"
			    "[Publish ardusub.manual_control]\nType=manual_control\nRate=50\nActuator=yes\nInputs=ctl.wrench\n"
			    "[Publish ardusub.state]\nType=ardusub_state\nRate=10\nInputs=safety.state\n"
			    "[Publish ardusub.param]\nType=ardusub_param\n"
			    "[Publish nav.attitude]\nType=attitude\nRate=50\nStaleAfter=500ms\n[Publish nav.imu]\nType=imu\nRate=50\nStaleAfter=500ms\n"
			    "[Publish nav.depth]\nType=depth\nRate=50\nStaleAfter=500ms\n[Publish power.battery]\nType=battery\nRate=2\nStaleAfter=3s\n"
			    "[Publish thr.out]\nType=thruster_out\nRate=50\nStaleAfter=500ms\n"
			    "[Actuator]\nCommand=ctl.wrench\nNeutral=%s --neutral %s\n"
			    "[Serve arm]\nRequest=arm_req\nReply=arm_rep\nAllow=safety\n"
			    "[Serve setup]\nRequest=empty\nReply=empty\nAllow=admin topside:admin\n"
			    "[Restart]\nBackoff=0.1s..0.2s\n", T_mavlink, conf_path, T_mavlink, conf_path);
	manifest("safety", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe ardusub.state]\n[Subscribe ardusub.manual_control]\n[Subscribe nav.attitude]\n"
			   "[Subscribe nav.depth]\n[Subscribe power.battery]\n[Subscribe thr.out]\n[Subscribe ardusub.param]\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Publish ctl.wrench]\nType=wrench\nRate=50\n"
			   "[Call mavlink.arm]\n");
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

/* ---- The fake ArduSub -------------------------------------------------------- */

static void fake_start(void)
{
	char to[64], ctl[64], at[64];
	tfmt(to, sizeof to, "127.0.0.1:%d", port_module);
	tfmt(ctl, sizeof ctl, "127.0.0.1:%d", port_control);
	tfmt(at, sizeof at, "127.0.0.1:%d", port_fake);
	fake = fork();
	if (fake == 0) {
		char log[600];
		tfmt(log, sizeof log, "%s/fake_ardusub.log", T_root);
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		execl(T_fake, T_fake, "--to", to, "--control", ctl, "--key", key_path, "--gcs-sysid", "255", "--bind", at,
		      (char *)NULL);
		_exit(127);
	}
}

/* One control line to the fake; the reply (one line) in a static buffer. */
static const char *fake_cmd(const char *cmd)
{
	static char reply[600];
	reply[0] = 0;
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons((uint16_t)port_control);
	struct timeval tv = { 2, 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	sendto(s, cmd, strlen(cmd), 0, (struct sockaddr *)&sa, sizeof sa);
	ssize_t n = recv(s, reply, sizeof reply - 1, 0);
	close(s);
	reply[n > 0 ? n : 0] = 0;
	return reply;
}

/* "key=" in the status line, as a number. */
static long long fake_num(const char *status, const char *key)
{
	char needle[48];
	tfmt(needle, sizeof needle, " %s=", key);
	const char *p = strncmp(status, key, strlen(key)) == 0 && status[strlen(key)] == '=' ? status + strlen(key) + 1
											     : strstr(status, needle);
	if (!p)
		return -1;
	if (p != status + strlen(key) + 1)
		p += strlen(needle);
	return atoll(p);
}

/* mc=x,y,z,r,buttons,buttons2,s,t */
static int fake_mc(const char *status, int out[8])
{
	const char *p = strstr(status, " mc=");
	if (!p)
		return -1;
	return sscanf(p + 4, "%d,%d,%d,%d,%d,%d,%d,%d", &out[0], &out[1], &out[2], &out[3], &out[4], &out[5], &out[6],
		      &out[7]) == 8 ? 0 : -1;
}

/* ---- This process as safety -------------------------------------------------- */

static void read_topics(void)
{
	struct ardusub_state ap;
	struct hg_info info;
	while (hg_next(S_ap, &ap, sizeof ap, &info) == HG_MSG) {
		last_ap = ap;
		last_ap_info = info;
		have_ap = 1;
		if (watch_mode >= 0 && ap.mode == (uint32_t)watch_mode && !saw_other_mode) {
			saw_other_mode = 1;
			printf("    ardusub.state seq %llu: mode %u armed %u (the stray's mode %ld), last_rx %llu ms ago\n",
			       (unsigned long long)info.seq, ap.mode, ap.armed, watch_mode,
			       (unsigned long long)((hg_now_ns() - ap.last_rx_ns) / 1000000));
		}
		if (ap.armed == 2)
			saw_disarming = 1;
		if (ap.armed == 1) {
			saw_armed = 1;
			saw_armed_info = info;
		}
	}
	struct manual_control mc;
	while (hg_next(S_mc, &mc, sizeof mc, &info) == HG_MSG) {
		last_mc = mc;
		have_mc = 1;
	}
}

static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	do {
		uint64_t now = hg_now_ns();
		if (now >= T.next_safety_ns) {
			T.next_safety_ns = now + 100000000u;
			struct safety_state s;
			memset(&s, 0, sizeof s);
			s.state = T.state;
			s.outputs_enabled = T.enabled;
			s.since_ns = now;
			hg_publish(P_safety, &s, sizeof s, now);
		}
		if (now >= T.next_cmd_ns) {
			T.next_cmd_ns = now + 20000000u;
			if (T.publish_cmd)
				hg_publish_with(P_cmd, &T.cmd, sizeof T.cmd, now, HG_NO_REFS, NULL, 0);
		}
		hg_sub *subs[2] = { S_ap, S_mc };
		hg_wait(subs, 2, 5);
		read_topics();
	} while (hg_now_ns() < end);
}

/* Publishes a new safety.state at once, then pumps. */
static void set_safety(uint8_t state, uint8_t enabled)
{
	T.state = state;
	T.enabled = enabled;
	T.next_safety_ns = 0;
	pump(30);
}

static int wait_ap(int armed, long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		pump(10);
		if (have_ap && last_ap.armed == armed)
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	printf("    ardusub.state armed=%d mode=%u params=%u result=%u reason '%s', wanted armed=%d\n", last_ap.armed,
	       last_ap.mode, last_ap.params, last_ap.arm_result, last_ap.arm_reason, armed);
	return 0;
}

static int wait_fake(const char *key, long long want, long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		pump(20);
		if (fake_num(fake_cmd("status"), key) == want)
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	printf("    fake: %s (wanted %s=%lld)\n", fake_cmd("status"), key, want);
	return 0;
}

/* The arm call as safety: the result after at most 2 s. */
static hg_call_result call_arm(void)
{
	arm_req req = { .arm = 1 };
	arm_rep rep;
	hg_pending *pc;
	hg_call_result r = { .status = HG_E_NOROUTE };
	int st = hg_call_start("mavlink.arm", &ARM_REQ_TYPE, &req, &ARM_REP_TYPE, &pc, 2000);
	if (st != HG_OK) {
		r.status = st;
		return r;
	}
	do {
		pump(10);
		r = hg_call_poll(pc, &rep);
	} while (r.status == HG_PENDING);
	return r;
}

static const char *module_status_line(void)
{
	static struct mstat m;
	module_status("mavlink", &m);
	return m.status;
}

static char cli_out[8192];

/* ---- Tests -------------------------------------------------------------------- */

static void test_start(void)
{
	port_module = free_udp_port();
	port_control = free_udp_port();
	port_fake = free_udp_port();
	CHECK(port_module > 0 && port_control > 0 && port_fake > 0);
	write_files();
	write_manifests();
	fake_start();
	mgr = manager_start();
	CHECK(wait_state("safety", "stopped", 5000));
	become("safety");
	P_safety = hg_pub_open("safety.state", &SAFETY_STATE_TYPE, NULL);
	P_cmd = hg_pub_open("ctl.wrench", &WRENCH_TYPE, NULL);
	S_ap = hg_sub_open("ardusub.state", &ARDUSUB_STATE_TYPE);
	S_mc = hg_sub_open("ardusub.manual_control", &MANUAL_CONTROL_TYPE);
	S_att = hg_sub_open("nav.attitude", &ATTITUDE_TYPE);
	S_depth = hg_sub_open("nav.depth", &DEPTH_TYPE);
	S_bat = hg_sub_open("power.battery", &BATTERY_TYPE);
	S_thr = hg_sub_open("thr.out", &THRUSTER_OUT_TYPE);
	S_param = hg_sub_open_from("ardusub.param", &ARDUSUB_PARAM_TYPE, HG_OLDEST);
	CHECK(P_safety && P_cmd && S_ap && S_mc && S_att && S_depth && S_bat && S_thr && S_param);
	T.state = 0;
	T.enabled = 0;
	/* Ready: every rated output published once, the telemetry among them,
	 * so the fake answered the rate requests. */
	uint64_t t0 = now_ms();
	while (!wait_state("mavlink", "ready", 100) && now_ms() - t0 < 8000)
		pump(50);
	CHECK(wait_state("mavlink", "ready", 100));
	pump(500);
	CHECK(have_ap);
	CHECK_EQ_INT(last_ap.armed, 0);
	CHECK_EQ_INT(last_ap.mode, 19);
	CHECK(last_ap.last_rx_ns > 0 && hg_now_ns() - last_ap.last_rx_ns < 2000000000ull);
	/* The fake's factory parameters differ from the file (FS_GCS_ENABLE 0 vs 1). */
	CHECK_EQ_INT(last_ap.params, 2);
	CHECK(strstr(module_status_line(), "differs: FS_GCS_ENABLE=0, file 1") != NULL);
	if (!strstr(module_status_line(), "differs"))
		printf("    status: %s\n", module_status_line());
	/* Neutral manual control, signed, from system 255, about 50 Hz. */
	const char *st = fake_cmd("status");
	int mc[8];
	CHECK_EQ_INT(fake_mc(st, mc), 0);
	CHECK(mc[0] == 0 && mc[1] == 0 && mc[2] == 500 && mc[3] == 0 && mc[4] == 0 && mc[5] == 0 && mc[6] == 0 && mc[7] == 0);
	CHECK_EQ_INT(fake_num(st, "mc_from"), 255);
	CHECK_EQ_INT(fake_num(st, "rejected"), 0);
	CHECK(fake_num(st, "setinterval") >= 5);
	long long c0 = fake_num(st, "mc_count");
	pump(1000);
	long long c1 = fake_num(fake_cmd("status"), "mc_count");
	CHECK(c1 - c0 >= 40 && c1 - c0 <= 60);
	printf("    %lld MANUAL_CONTROL in 1 s (neutral, signed)\n", c1 - c0);
	CHECK(have_mc && last_mc.z == 500 && last_mc.x == 0 && last_mc.buttons == 0);
	/* Telemetry: depth 2 m from the fake's pressure, 16 V, 1500 us. */
	struct depth d;
	struct hg_info info;
	CHECK_EQ_INT(hg_latest(S_depth, &d, sizeof d, &info), HG_MSG);
	CHECK(fabsf(d.depth_m - 2.0f) < 0.02f);
	struct battery b;
	CHECK_EQ_INT(hg_latest(S_bat, &b, sizeof b, &info), HG_MSG);
	CHECK(fabsf(b.voltage_v - 16.0f) < 0.01f && fabsf(b.remaining - 0.8f) < 0.01f);
	struct thruster_out th;
	CHECK_EQ_INT(hg_latest(S_thr, &th, sizeof th, &info), HG_MSG);
	CHECK_EQ_INT(th.pwm[0], 1500);
	struct attitude a;
	CHECK_EQ_INT(hg_latest(S_att, &a, sizeof a, &info), HG_MSG);
	CHECK(fabsf(a.roll - 0.1f) < 1e-5f && fabsf(a.yaw - 1.0f) < 1e-5f);
	CHECK(fabsf(a.q[0] * a.q[0] + a.q[1] * a.q[1] + a.q[2] * a.q[2] + a.q[3] * a.q[3] - 1.0f) < 1e-4f);
	/* ATTITUDE at 50 Hz: its stamp is in hangar's clock, near the arrival. */
	CHECK(info.stamp_ns > 0 && info.publish_ns >= info.stamp_ns && info.publish_ns - info.stamp_ns < 50000000ull);
}

static void test_params_and_setup(void)
{
	/* mavlink.arm refuses while the parameters differ (and safety is not arming). */
	set_safety(3, 0);
	hg_call_result r = call_arm();
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "FS_GCS_ENABLE") != NULL);
	set_safety(0, 0);
	/* hangar ardusub-setup is the call mavlink.setup as admin. */
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "call", "mavlink.setup", (char *)NULL), 0);
	CHECK(strstr(cli_out, "HG_OK") != NULL);
	uint64_t t0 = now_ms();
	while ((!have_ap || last_ap.params != 1) && now_ms() - t0 < 3000)
		pump(50);
	CHECK_EQ_INT(last_ap.params, 1);
	CHECK(fake_num(fake_cmd("status"), "params_set") == 4);
	CHECK(strstr(module_status_line(), "params 1") != NULL);
	/* Every PARAM_VALUE went to ardusub.param. */
	struct ardusub_param p;
	struct hg_info info;
	int n = 0, saw_fs = 0;
	while (hg_next(S_param, &p, sizeof p, &info) == HG_MSG) {
		n++;
		if (strcmp(p.id, "FS_GCS_ENABLE") == 0 && p.value == 1.0f)
			saw_fs = 1;
	}
	CHECK(n >= 4 && saw_fs);
	/* setup is refused while safety.state is not disarmed. */
	set_safety(1, 1);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "call", "mavlink.setup", (char *)NULL), 1);
	CHECK(strstr(cli_out, "not disarmed") != NULL);
	set_safety(0, 0);
}

static void arm_ok(void)
{
	set_safety(3, 0);
	hg_call_result r = call_arm();
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(wait_ap(1, 2000));
	set_safety(1, 1);
}

static void test_arming_and_commands(void)
{
	/* mavlink.arm is refused unless safety.state says arming. */
	hg_call_result r = call_arm();
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "says disarmed") != NULL);
	/* The sequence: arming, the call, ArduSub armed, ardusub.state armed. */
	arm_ok();
	CHECK_EQ_INT(last_ap.arm_attempt, 1);
	CHECK_EQ_INT(last_ap.arm_result, 0);
	CHECK(strcmp(last_ap.arm_reason, "MAV_RESULT_ACCEPTED") == 0);
	/* The command reaches ArduSub, within the mapping of docs/10. */
	T.cmd = (struct wrench){ .surge = 0.3f, .sway = 0, .heave = -0.5f, .roll = -0.2f, .pitch = 0.2f, .yaw = 0.1f };
	pump(300);
	int mc[8];
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK(mc[0] == 300 && mc[1] == 0 && mc[2] == 250 && mc[3] == 100 && mc[4] == 0 && mc[5] == 0);
	CHECK(mc[6] == 200 && mc[7] == -200); /* s pitch, t roll */
	CHECK(have_mc && last_mc.x == 300 && last_mc.z == 250 && last_mc.s == 200 && last_mc.t == -200);
	/* Clamped: 1.5 is 1000. */
	T.cmd.surge = 1.5f;
	pump(200);
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK_EQ_INT(mc[0], 1000);
	/* A non-finite axis: the whole message neutral. */
	T.cmd.heave = NAN;
	pump(200);
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK(mc[0] == 0 && mc[2] == 500);
	T.cmd.heave = -0.5f;
	T.cmd.surge = 0.3f;
	pump(200);
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK_EQ_INT(mc[0], 300);
	/* The command stops: stale after CommandTimeout (200 ms), neutral. */
	T.publish_cmd = 0;
	pump(400);
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK(mc[0] == 0 && mc[2] == 500);
	CHECK(strstr(module_status_line(), "command stale") != NULL);
	T.publish_cmd = 1;
	pump(200);
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK_EQ_INT(mc[0], 300);
	/* The 50 Hz output kept its rate throughout. */
	long long c0 = fake_num(fake_cmd("status"), "mc_count");
	pump(1000);
	long long c1 = fake_num(fake_cmd("status"), "mc_count");
	CHECK(c1 - c0 >= 40 && c1 - c0 <= 60);
}

static void test_disarm_and_disagreement(void)
{
	/* hangar disarms: the window closes, ardusub.state says disarming, then
	 * disarmed; the fake saw one disarm; manual control is neutral. */
	saw_disarming = 0;
	long long d0 = fake_num(fake_cmd("status"), "disarms");
	set_safety(0, 0);
	CHECK(wait_ap(0, 1500));
	CHECK(saw_disarming);
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "disarms"), d0 + 1);
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "armed"), 0);
	pump(100);
	int mc[8];
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK(mc[0] == 0 && mc[2] == 500);
	CHECK(strstr(module_status_line(), "gate disarmed") != NULL);
	/* A refused disarm is a disagreement at the answer: reported armed (after
	 * `disarming` while the answer was awaited), disarmed once ArduSub takes it. */
	arm_ok();
	fake_cmd("refuse-disarm on");
	saw_disarming = 0;
	d0 = fake_num(fake_cmd("status"), "disarms");
	uint64_t t0 = now_ms();
	set_safety(0, 0);
	CHECK(wait_ap(1, 2000));
	uint64_t dt = now_ms() - t0;
	CHECK(saw_disarming);
	CHECK(dt < 1500);
	printf("    refused disarm reported as armed after %llu ms\n", (unsigned long long)dt);
	CHECK(fake_num(fake_cmd("status"), "disarms") >= d0 + 1);
	fake_cmd("refuse-disarm off");
	CHECK(wait_ap(0, 3000));
	/* ArduSub armed behind hangar's back: reported armed against a
	 * safety.state that says disarmed, and disarmed at once. */
	d0 = fake_num(fake_cmd("status"), "disarms");
	pump(200);
	saw_armed = 0;
	fake_cmd("arm");
	t0 = now_ms();
	while (!saw_armed && now_ms() - t0 < 1000)
		pump(10);
	CHECK(saw_armed);
	int ref_safety = 0;
	for (uint32_t i = 0; i < saw_armed_info.n_inputs; i++)
		if (saw_armed_info.inputs[i].topic_id == hg_topic_id("safety.state"))
			ref_safety = 1;
	CHECK(ref_safety);
	CHECK(wait_ap(0, 1500));
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "disarms"), d0 + 1);
	/* Out of MANUAL while armed: disarmed at the first message. */
	arm_ok();
	d0 = fake_num(fake_cmd("status"), "disarms");
	fake_cmd("mode 2");
	CHECK(wait_ap(0, 1500));
	CHECK_EQ_INT(last_ap.mode, 2);
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "disarms"), d0 + 1);
	/* mavlink.arm refuses while ArduSub is out of MANUAL. */
	set_safety(3, 0);
	hg_call_result r = call_arm();
	CHECK_EQ_INT(r.status, HG_E_REFUSED);
	CHECK(strstr(r.reason, "not MANUAL") != NULL);
	fake_cmd("mode 19");
	set_safety(0, 0);
	pump(200);
}

static void test_refusal(void)
{
	fake_cmd("refuse PreArm: Compass not calibrated");
	set_safety(3, 0);
	hg_call_result r = call_arm();
	CHECK_EQ_INT(r.status, HG_OK); /* accepted: sent; the outcome is on ardusub.state */
	uint64_t t0 = now_ms();
	while ((last_ap.arm_result == 255 || last_ap.arm_result == 0 || !strstr(last_ap.arm_reason, "Arm: refused")) &&
	       now_ms() - t0 < 2000)
		pump(20);
	CHECK_EQ_INT(last_ap.arm_result, 4);
	CHECK(strstr(last_ap.arm_reason, "PreArm: Compass not calibrated") != NULL);
	CHECK(strstr(last_ap.arm_reason, "Arm: refused by the fake") != NULL);
	if (!strstr(last_ap.arm_reason, "Arm:"))
		printf("    reason: %s\n", last_ap.arm_reason);
	CHECK_EQ_INT(last_ap.armed, 0);
	fake_cmd("accept");
	set_safety(0, 0);
}

static void test_restart_time_silence(void)
{
	/* Stamps rise with arrivals; across an ArduSub restart they keep rising
	 * and the rates and parameters are asked again. */
	struct attitude a;
	struct hg_info i1, i2;
	pump(100);
	CHECK_EQ_INT(hg_latest(S_att, &a, sizeof a, &i1), HG_MSG);
	long long si0 = fake_num(fake_cmd("status"), "setinterval");
	fake_cmd("boot-reset");
	pump(600);
	CHECK_EQ_INT(hg_latest(S_att, &a, sizeof a, &i2), HG_MSG);
	CHECK(i2.stamp_ns > i1.stamp_ns);
	CHECK(i2.publish_ns >= i2.stamp_ns && i2.publish_ns - i2.stamp_ns < 100000000ull);
	printf("    attitude stamp %llu -> %llu across the restart (arrival lag %llu us)\n",
	       (unsigned long long)i1.stamp_ns, (unsigned long long)i2.stamp_ns,
	       (unsigned long long)((i2.publish_ns - i2.stamp_ns) / 1000));
	CHECK(fake_num(fake_cmd("status"), "setinterval") > si0);
	char log[600];
	tfmt(log, sizeof log, "%s/run/hangar/log/mavlink.log", T_root);
	CHECK(wait_file_contains(log, "ArduSub restarted", 2000));
	uint64_t t0 = now_ms();
	while (last_ap.params != 1 && now_ms() - t0 < 3000)
		pump(50);
	CHECK_EQ_INT(last_ap.params, 1);
	/* Silence: last_rx_ns stops advancing; ardusub.state goes on. */
	fake_cmd("silence 1.5");
	pump(300);
	uint64_t rx = last_ap.last_rx_ns, seq = last_ap_info.seq;
	pump(600);
	CHECK_EQ_INT(last_ap.last_rx_ns, rx);
	CHECK(last_ap_info.seq > seq);
	CHECK(hg_now_ns() - last_ap.last_rx_ns > 800000000ull);
	pump(1000);
	CHECK(last_ap.last_rx_ns > rx);
}

static void test_signing(void)
{
	/* An unsigned frame sent to the module's port: refused, counted. An
	 * unsigned MANUAL_CONTROL sent to ArduSub's port: refused by ArduSub. */
	struct mav_manual_control m = { .target = 1, .x = 900, .z = 500 };
	uint8_t payload[MAV_LEN_MANUAL_CONTROL], frame[MAV_MAX_FRAME];
	mav_pack_manual_control(&m, payload);
	size_t n = mav_encode(frame, sizeof frame, 1, 255, 190, MAV_MSG_ID_MANUAL_CONTROL, payload, sizeof payload,
			      MAV_CRC_MANUAL_CONTROL, NULL);
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	long long r0 = fake_num(fake_cmd("status"), "rejected");
	sa.sin_port = htons((uint16_t)port_fake);
	sendto(s, frame, n, 0, (struct sockaddr *)&sa, sizeof sa);
	sendto(s, frame, n, 0, (struct sockaddr *)&sa, sizeof sa);
	CHECK(wait_fake("rejected", r0 + 2, 1000));
	int mc[8];
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK(mc[0] != 900);
	/* A wrongly signed one too. */
	struct mav_signing bad;
	memset(&bad, 0, sizeof bad);
	bad.enabled = 1;
	bad.link_id = 7;
	for (int i = 0; i < 32; i++)
		bad.key[i] = 0xaa;
	n = mav_encode(frame, sizeof frame, 2, 255, 190, MAV_MSG_ID_MANUAL_CONTROL, payload, sizeof payload,
		       MAV_CRC_MANUAL_CONTROL, &bad);
	sendto(s, frame, n, 0, (struct sockaddr *)&sa, sizeof sa);
	CHECK(wait_fake("rejected", r0 + 3, 1000));
	/* And the module's own port. */
	sa.sin_port = htons((uint16_t)port_module);
	sendto(s, frame, n, 0, (struct sockaddr *)&sa, sizeof sa);
	uint64_t t0 = now_ms();
	while (!strstr(module_status_line(), "rejected 1") && now_ms() - t0 < 1000)
		pump(50);
	CHECK(strstr(module_status_line(), "rejected 1") != NULL);
	close(s);
}

static void test_other_component(void)
{
	/* The lab's Pixhawk behind MAVROS (2026-11-09): MAVROS heartbeats as
	 * system 1, component 191, an onboard controller with base_mode bits that
	 * read as armed and custom_mode 0. Signed with the link's key, so only the
	 * component tells it from ArduSub. ardusub.state must keep ArduSub's own
	 * armed flag and mode, and the frames are counted. The stray here says
	 * mode 7, a mode nothing else in the suite uses: 0 is also what
	 * ardusub.state says before ArduSub's first heartbeat (1 Hz), which a
	 * loaded machine can still be waiting for when the test starts. */
	uint64_t t0 = now_ms();
	while ((!have_ap || last_ap.mode != 19) && now_ms() - t0 < 3000)
		pump(50);
	CHECK_EQ_INT(last_ap.mode, 19);
	CHECK_EQ_INT(last_ap.armed, 0);
	watch_mode = 7;
	saw_other_mode = 0;
	saw_armed = 0;
	struct mav_heartbeat h = { .custom_mode = 7, .type = 18 /* MAV_TYPE_ONBOARD_CONTROLLER */,
				   .autopilot = MAV_AUTOPILOT_INVALID, .base_mode = MAV_MODE_FLAG_SAFETY_ARMED | 1,
				   .system_status = 4, .mavlink_version = 3 };
	uint8_t payload[MAV_LEN_HEARTBEAT], frame[MAV_MAX_FRAME];
	mav_pack_heartbeat(&h, payload);
	struct mav_signing sig;
	memset(&sig, 0, sizeof sig);
	sig.enabled = 1;
	sig.link_id = 9;
	for (int i = 0; i < 32; i++)
		sig.key[i] = (uint8_t)i; /* the test's mavlink.key, 000102...1f */
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons((uint16_t)port_module);
	for (int i = 0; i < 10; i++) {
		size_t n = mav_encode(frame, sizeof frame, (uint8_t)i, 1, 191, MAV_MSG_ID_HEARTBEAT, payload, sizeof payload,
				      MAV_CRC_HEARTBEAT, &sig);
		sendto(s, frame, n, 0, (struct sockaddr *)&sa, sizeof sa);
		pump(100);
	}
	close(s);
	pump(1200); /* ArduSub's own 1 Hz heartbeat has come in meanwhile */
	CHECK_EQ_INT(saw_armed, 0);
	CHECK_EQ_INT(saw_other_mode, 0);
	CHECK_EQ_INT(last_ap.armed, 0);
	CHECK_EQ_INT(last_ap.mode, 19);
	CHECK(strstr(module_status_line(), "other-component 10") != NULL);
	if (!strstr(module_status_line(), "other-component 10"))
		printf("    status: %s\n", module_status_line());
	watch_mode = -1;
}

static void test_neutral_command(void)
{
	/* The module stopped: the manager runs its Neutral command, which sends
	 * neutral and a disarm, signed. The fake is armed by hand first. */
	fake_cmd("arm");
	long long d0 = fake_num(fake_cmd("status"), "disarms");
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "stop", "mavlink", (char *)NULL), 0);
	CHECK(wait_state("mavlink", "stopped", 5000));
	/* On SIGTERM the module itself sent neutral and a disarm; then the
	 * manager's Neutral command did too. */
	CHECK(wait_fake("armed", 0, 3000));
	CHECK(fake_num(fake_cmd("status"), "disarms") >= d0 + 1);
	int mc[8];
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK(mc[0] == 0 && mc[2] == 500);
	CHECK(wait_file_contains(T_log, "mavlink: Neutral command ran", 3000));
	/* And by hand, while nothing else holds the port. */
	fake_cmd("arm");
	d0 = fake_num(fake_cmd("status"), "disarms");
	pid_t pid = fork();
	if (pid == 0) {
		execl(T_mavlink, T_mavlink, "--neutral", conf_path, (char *)NULL);
		_exit(127);
	}
	CHECK_EQ_INT(wait_pid_ms(pid, 5000), 0);
	CHECK(wait_fake("armed", 0, 2000));
	CHECK(fake_num(fake_cmd("status"), "disarms") >= d0 + 1);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "start", "mavlink", (char *)NULL), 0);
	uint64_t t0 = now_ms();
	while (!wait_state("mavlink", "ready", 100) && now_ms() - t0 < 8000)
		pump(50);
	CHECK(wait_state("mavlink", "ready", 100));
}

static void test_shutdown(void)
{
	hg_pub_close(P_safety);
	hg_pub_close(P_cmd);
	CHECK_EQ_INT(manager_stop(mgr), 0);
	fake_cmd("quit");
	wait_pid_ms(fake, 2000);
	CHECK(file_contains(T_log, "mavlink: pid"));
}

int main(void)
{
	T_mavlink = getenv("MAVLINK_BIN");
	T_fake = getenv("FAKE_ARDUSUB_BIN");
	if (!T_mavlink || !T_fake) {
		fprintf(stderr, "MAVLINK_BIN and FAKE_ARDUSUB_BIN must be set\n");
		return 2;
	}
	hd_setup("mavlink_vehicle", NULL);
	copy_types();
	RUN_TEST(test_start);
	RUN_TEST(test_params_and_setup);
	RUN_TEST(test_arming_and_commands);
	RUN_TEST(test_disarm_and_disagreement);
	RUN_TEST(test_refusal);
	RUN_TEST(test_restart_time_silence);
	RUN_TEST(test_signing);
	RUN_TEST(test_other_component);
	RUN_TEST(test_neutral_command);
	RUN_TEST(test_shutdown);
	if (fake > 0)
		kill(fake, SIGKILL);
	hd_teardown();
	TEST_MAIN_END();
}
