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
// refused by both ends; the Neutral command; every parameter saved, compared
// and set back through hangar ardusub-params (mavlink.params_dump and
// params_load: a dropped PARAM_VALUE asked again by index, typed values, only
// what differs set and each confirmed, STAT_* left alone, refused while armed,
// MANUAL_CONTROL at 50 Hz all the while). This process plays `safety`
// (safety.state, ctl.wrench, the caller of mavlink.arm). Under 30 s.
//
// With MAVLINK_TEST_SERIAL=1 the same suite runs over a serial line: the
// fake ArduSub at the far end of a pseudo-terminal (fake_ardusub --pty), the
// module's Serial= naming it, as a Pixhawk on USB. The two tests that inject
// UDP datagrams into the module's port are left out; four of the line's own
// take their place: noise and a frame cut across two writes, the line
// vanishing and coming back, a reader that stops reading, and the line held
// exclusively.
#include "hd_test.h"

#include <arpa/inet.h>
#include <fcntl.h>
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
static char conf_path[600], key_path[600], params_path[600], line_path[600];
static int serial_mode;
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
				"arm_req", "arm_rep", "params_job", "params_file_req", NULL };
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
	char text[1400], link[700];
	if (serial_mode) {
		tfmt(line_path, sizeof line_path, "%s/ttyFAKE", T_root);
		tfmt(link, sizeof link, "Serial=%s\nBaud=115200\n", line_path);
	} else {
		tfmt(link, sizeof link, "Bind=127.0.0.1:%d\n", port_module);
	}
	tfmt(text, sizeof text,
	     "[Link]\n%sSigning=yes\nKeyFile=%s\nSysId=255\nCompId=190\nTargetSysId=1\n"
	     "[Params]\nFile=%s\n[Depth]\nFluidDensity=1000\nSurfacePressure=1013.25\n"
	     "[Telemetry ATTITUDE]\nRate=50\n[Telemetry SYS_STATUS]\nRate=5\n",
	     link, key_path, params_path);
	write_file(conf_path, text);
}

static void write_manifests(void)
{
	manifest("mavlink", "[Module]\nExec=%s %s\nUser=hg-mavlink\n"
			    "[Subscribe ctl.wrench]\n[Subscribe safety.state]\n"
			    "[Publish ardusub.manual_control]\nType=manual_control\nRate=50\nActuator=yes\nInputs=ctl.wrench\n"
			    "[Publish ardusub.state]\nType=ardusub_state\nRate=10\nInputs=safety.state\n"
			    "[Publish ardusub.param]\nType=ardusub_param\n[Publish ardusub.params_job]\nType=params_job\n"
			    "[Publish nav.attitude]\nType=attitude\nRate=50\nStaleAfter=500ms\n[Publish nav.imu]\nType=imu\nRate=50\nStaleAfter=500ms\n"
			    "[Publish nav.depth]\nType=depth\nRate=50\nStaleAfter=500ms\n[Publish power.battery]\nType=battery\nRate=2\nStaleAfter=3s\n"
			    "[Publish thr.out]\nType=thruster_out\nRate=50\nStaleAfter=500ms\n"
			    "[Actuator]\nCommand=ctl.wrench\nNeutral=%s --neutral %s\n"
			    "[Serve arm]\nRequest=arm_req\nReply=arm_rep\nAllow=safety\n"
			    "[Serve setup]\nRequest=empty\nReply=empty\nAllow=admin topside:admin\n"
			    "[Serve params_dump]\nRequest=params_file_req\nReply=params_job\nAllow=admin topside:admin\n"
			    "[Serve params_load]\nRequest=params_file_req\nReply=params_job\nAllow=admin topside:admin\n"
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
		if (serial_mode)
			execl(T_fake, T_fake, "--pty", line_path, "--control", ctl, "--key", key_path, "--gcs-sysid", "255",
			      (char *)NULL);
		else
			execl(T_fake, T_fake, "--to", to, "--control", ctl, "--key", key_path, "--gcs-sysid", "255", "--bind",
			      at, (char *)NULL);
		_exit(127);
	}
	if (serial_mode) {
		/* The module opens the line at its start: the link must exist by then
		 * (a line that is not there is retried, but the suite's timings are
		 * shorter than the back-off). */
		uint64_t t0 = now_ms();
		while (access(line_path, F_OK) != 0 && now_ms() - t0 < 2000)
			sleep_ms(10);
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

/* power.battery's voltage as the next message says it, within the time. */
static float battery_voltage_after(long ms)
{
	struct battery b = { 0 };
	struct hg_info info;
	uint64_t t0 = now_ms();
	(void)hg_latest(S_bat, &b, sizeof b, &info); /* what was there before */
	do {
		pump(50);
		if (hg_latest(S_bat, &b, sizeof b, &info) == HG_MSG)
			return b.voltage_v;
	} while (now_ms() - t0 < (uint64_t)ms);
	return -1.0f;
}

/* SYS_STATUS.voltage_battery 65535 is MAVLink's "not measured". Divided by a
 * thousand like any other value it was published as 65.5 V, a healthy pack
 * to the safety module's checklist and battery trigger. It is NaN now, with
 * the topic's rate and the other fields as before. */
static void test_battery_unknown(void)
{
	CHECK(strncmp(fake_cmd("voltage 65535"), "ok", 2) == 0);
	float v = 0;
	uint64_t t0 = now_ms();
	do {
		v = battery_voltage_after(2000);
	} while (!isnan(v) && now_ms() - t0 < 5000);
	CHECK(isnan(v));
	if (!isnan(v))
		printf("    power.battery voltage_v %.3f with voltage_battery 65535\n", (double)v);
	struct battery b;
	struct hg_info info;
	pump(600);
	CHECK_EQ_INT(hg_latest(S_bat, &b, sizeof b, &info), HG_MSG); /* still published */
	CHECK(fabsf(b.remaining - 0.8f) < 0.01f && fabsf(b.current_a - 2.5f) < 0.01f);
	/* 65534 is a voltage like any other; and back to 16 V. */
	CHECK(strncmp(fake_cmd("voltage 65534"), "ok", 2) == 0);
	t0 = now_ms();
	do {
		v = battery_voltage_after(2000);
	} while (isnan(v) && now_ms() - t0 < 5000);
	CHECK(fabsf(v - 65.534f) < 0.01f);
	CHECK(strncmp(fake_cmd("voltage 16000"), "ok", 2) == 0);
	t0 = now_ms();
	do {
		v = battery_voltage_after(2000);
	} while (!(fabsf(v - 16.0f) < 0.01f) && now_ms() - t0 < 5000);
	CHECK(fabsf(v - 16.0f) < 0.01f);
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

/* ---- hangar ardusub-params -------------------------------------------------- */

/* The command run while this process goes on playing safety (safety.state at
 * 10 Hz, ctl.wrench at 50 Hz): its exit status, its output in cli_out, its
 * duration in *ms. */
static int cli_pumping(long timeout_ms, uint64_t *ms, const char *a1, const char *a2)
{
	char out_path[600];
	tfmt(out_path, sizeof out_path, "%s/cli.out", T_root);
	uint64_t t0 = now_ms();
	pid_t pid = fork();
	if (pid == 0) {
		int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
		execl(T_hangar, T_hangar, "ardusub-params", a1, a2, (char *)NULL);
		_exit(127);
	}
	int st = -1;
	for (;;) {
		pump(20);
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == pid)
			break;
		if (now_ms() - t0 > (uint64_t)timeout_ms) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			st = -1;
			break;
		}
	}
	if (ms)
		*ms = now_ms() - t0;
	cli_out[0] = 0;
	FILE *f = fopen(out_path, "r");
	if (f) {
		size_t n = fread(cli_out, 1, sizeof cli_out - 1, f);
		cli_out[n] = 0;
		fclose(f);
	}
	int rc = st >= 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	if (rc != 0)
		printf("    hangar ardusub-params %s: exit %d: %s", a1, rc, cli_out);
	return rc;
}

/* "NAME VALUE TYPE" from the fake. */
static int fake_get(const char *id, double *v, unsigned *type)
{
	char cmd[64], name[32];
	tfmt(cmd, sizeof cmd, "get %s", id);
	return sscanf(fake_cmd(cmd), "%31s %lf %u", name, v, type) == 3 ? 0 : -1;
}

/* The value a saved file gives a name, and its type; -1 if absent. */
static int file_get(const char *path, const char *id, double *v, unsigned *type, int *lines)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[256];
	int found = -1, n = 0;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#')
			continue;
		n++;
		char name[32];
		double val;
		unsigned t;
		if (sscanf(line, "%31s %lf %u", name, &val, &t) == 3 && strcmp(name, id) == 0) {
			*v = val;
			*type = t;
			found = 0;
		}
	}
	fclose(f);
	if (lines)
		*lines = n;
	return found;
}

static void test_params_backup(void)
{
	set_safety(0, 0);
	char saved[600], yaml[600];
	tfmt(saved, sizeof saved, "%s/pixhawk.params", T_root);
	tfmt(yaml, sizeof yaml, "%s/mavros.yaml", T_root);
	const char *st = fake_cmd("status");
	long long n_params = fake_num(st, "n_params"), r0 = fake_num(st, "reads_index");
	CHECK(n_params > 400);
	/* Save: the list streamed with two PARAM_VALUEs lost, asked again by
	 * index; MANUAL_CONTROL at 50 Hz all the while. */
	fake_cmd("drop 17 203");
	long long mc0 = fake_num(fake_cmd("status"), "mc_count");
	uint64_t ms = 0;
	CHECK_EQ_INT(cli_pumping(20000, &ms, "save", saved), 0);
	long long mc1 = fake_num(fake_cmd("status"), "mc_count");
	double hz = (double)(mc1 - mc0) * 1000.0 / (double)ms;
	printf("    %lld parameters saved in %llu ms over %s, two re-read by index; MANUAL_CONTROL %.1f Hz meanwhile\n",
	       n_params, (unsigned long long)ms, serial_mode ? "the serial line" : "UDP", hz);
	CHECK(hz >= 47.0 && hz <= 53.0);
	CHECK(strstr(cli_out, "parameters saved to") != NULL);
	CHECK(fake_num(fake_cmd("status"), "reads_index") >= r0 + 2);
	int lines = 0;
	double v, fv;
	unsigned t, ft;
	CHECK_EQ_INT(file_get(saved, "COMPASS_X03", &v, &t, &lines), 0);
	CHECK_EQ_INT(lines, (int)n_params);
	CHECK_EQ_INT(fake_get("COMPASS_X03", &fv, &ft), 0);
	CHECK(t == MAV_PARAM_TYPE_INT8 && ft == t && v == fv);
	CHECK_EQ_INT(file_get(saved, "EK3_X07", &v, &t, NULL), 0);
	CHECK_EQ_INT(fake_get("EK3_X07", &fv, &ft), 0);
	CHECK(t == MAV_PARAM_TYPE_REAL32 && (float)v == (float)fv);
	double atc5, servo10;
	CHECK_EQ_INT(file_get(saved, "ATC_RAT_X05", &atc5, &t, NULL), 0);
	CHECK_EQ_INT(file_get(saved, "SERVO_X10", &servo10, &t, NULL), 0);
	CHECK_EQ_INT(t, MAV_PARAM_TYPE_INT16);
	/* Nothing differs from the file just saved (STAT_RUNTIME and
	 * BARO1_GND_PRESS moved since, by themselves: not compared). */
	CHECK_EQ_INT(cli_pumping(20000, NULL, "diff", saved), 0);
	CHECK(strstr(cli_out, "every parameter as in") != NULL);
	/* Two changed behind the file's back: named, and nothing else. */
	fake_cmd("set ATC_RAT_X05 9.5");
	fake_cmd("set SERVO_X10 1234");
	CHECK_EQ_INT(cli_pumping(20000, NULL, "diff", saved), 1);
	CHECK(strstr(cli_out, "ATC_RAT_X05") && strstr(cli_out, "SERVO_X10") && strstr(cli_out, "2 differ"));
	CHECK(!strstr(cli_out, "STAT_") && !strstr(cli_out, "GND_PRESS"));
	/* Restore: refused while safety.state is not disarmed, and while ArduSub
	 * is armed (a disarm it refuses keeps it so). */
	set_safety(1, 1);
	CHECK_EQ_INT(cli_pumping(5000, NULL, "restore", saved), 1);
	CHECK(strstr(cli_out, "not disarmed") != NULL);
	set_safety(0, 0);
	fake_cmd("refuse-disarm on");
	fake_cmd("arm");
	uint64_t t0 = now_ms();
	while ((!have_ap || last_ap.armed == 0) && now_ms() - t0 < 2000)
		pump(10);
	CHECK(last_ap.armed != 0);
	CHECK_EQ_INT(cli_pumping(5000, NULL, "restore", saved), 1);
	CHECK(strstr(cli_out, "ArduSub is armed") != NULL);
	fake_cmd("refuse-disarm off");
	CHECK(wait_ap(0, 3000));
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "armed"), 0);
	/* Restore: the two that differ set, each read back, nothing else. */
	long long s0 = fake_num(fake_cmd("status"), "params_set");
	CHECK_EQ_INT(cli_pumping(20000, &ms, "restore", saved), 0);
	CHECK(strstr(cli_out, "2 parameters set and confirmed") != NULL);
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "params_set"), s0 + 2);
	CHECK_EQ_INT(fake_get("ATC_RAT_X05", &fv, &ft), 0);
	CHECK((float)fv == (float)atc5);
	CHECK_EQ_INT(fake_get("SERVO_X10", &fv, &ft), 0);
	CHECK(fv == servo10);
	printf("    restore of 2 in %llu ms (a full read first)\n", (unsigned long long)ms);
	CHECK_EQ_INT(cli_pumping(20000, NULL, "diff", saved), 0);
	CHECK(strstr(cli_out, "every parameter as in") != NULL);
	if (serial_mode)
		return; /* the rest is the same over either link */
	/* MAVROS's YAML (the backups rov.sh made) reads too, its STAT_ ignored; a
	 * write ArduSub does not take fails after its tries, the rest confirmed. */
	write_file(yaml, "/mavros/param:\n  ros__parameters:\n    ATC_RAT_X07: 2.5\n    ATC_RAT_X08: 7.25\n"
			 "    STAT_RUNTIME: 5\n    use_sim_time: false\n");
	fake_cmd("ignore-set ATC_RAT_X08");
	s0 = fake_num(fake_cmd("status"), "params_set");
	CHECK_EQ_INT(cli_pumping(20000, NULL, "restore", yaml), 1);
	CHECK(strstr(cli_out, "1 of 2 parameters not confirmed") != NULL);
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "params_set"), s0 + 1 + 3);
	CHECK_EQ_INT(fake_get("ATC_RAT_X07", &fv, &ft), 0);
	CHECK(fv == 2.5);
	CHECK_EQ_INT(fake_get("STAT_RUNTIME", &fv, &ft), 0);
	CHECK(fv > 100);
	fake_cmd("ignore-set -");
	/* And back as saved. */
	CHECK_EQ_INT(cli_pumping(20000, NULL, "restore", saved), 0);
	CHECK(strstr(cli_out, "1 parameter set and confirmed") != NULL);
	CHECK_EQ_INT(cli_pumping(20000, NULL, "diff", saved), 0);
	CHECK(strstr(cli_out, "every parameter as in") != NULL);
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


/* ---- The serial line (MAVLINK_TEST_SERIAL=1) --------------------------------- */

static const char *module_log(void)
{
	static char path[600];
	tfmt(path, sizeof path, "%s/run/hangar/log/mavlink.log", T_root);
	return path;
}

static void test_serial_noise(void)
{
	/* Line noise with 0xFD bytes in it, then a HEARTBEAT written in two
	 * halves 30 ms apart, marked with mode 77: the module must find it. */
	uint64_t t0 = now_ms();
	while ((!have_ap || last_ap.mode != 19) && now_ms() - t0 < 3000)
		pump(50);
	watch_mode = 77;
	saw_other_mode = 0;
	CHECK(strncmp(fake_cmd("garbage"), "ok", 2) == 0);
	t0 = now_ms();
	while (!saw_other_mode && now_ms() - t0 < 1500)
		pump(20);
	CHECK_EQ_INT(saw_other_mode, 1);
	watch_mode = -1;
	/* And the stream goes on: the next ordinary HEARTBEAT, mode 19. */
	t0 = now_ms();
	while (last_ap.mode != 19 && now_ms() - t0 < 2500)
		pump(50);
	CHECK_EQ_INT(last_ap.mode, 19);
	CHECK(strstr(module_status_line(), "serial open") != NULL);
}

static void test_serial_backpressure(void)
{
	/* The far end stops reading for 1.5 s: the module's writes back up, and
	 * it must not stall. ardusub.state goes on at 10 Hz, the module stays
	 * ready, the same process; when reading resumes the manual control
	 * arriving is the current one, at once. */
	struct mstat m0;
	CHECK(module_status("mavlink", &m0));
	uint64_t seq0 = last_ap_info.seq;
	long long c0 = fake_num(fake_cmd("status"), "mc_count");
	CHECK(strncmp(fake_cmd("stopread on"), "ok", 2) == 0);
	pump(1500);
	CHECK(last_ap_info.seq - seq0 >= 12);
	struct mstat m1;
	CHECK(module_status("mavlink", &m1) && strcmp(m1.state, "ready") == 0 && m1.pid == m0.pid);
	CHECK(strncmp(fake_cmd("stopread off"), "ok", 2) == 0);
	uint64_t t0 = now_ms();
	long long age = 999999;
	while (now_ms() - t0 < 1000) {
		pump(20);
		age = fake_num(fake_cmd("status"), "mc_age_ms");
		if (age >= 0 && age < 100)
			break;
	}
	CHECK(age >= 0 && age < 100);
	long long c1 = fake_num(fake_cmd("status"), "mc_count");
	pump(500);
	long long c2 = fake_num(fake_cmd("status"), "mc_count");
	printf("    reader stopped 1.5 s: %lld MANUAL_CONTROL got through meanwhile and after (kernel buffer), "
	       "then %lld in 0.5 s; status: %s\n", c1 - c0, c2 - c1, module_status_line());
	CHECK(c2 - c1 >= 18 && c2 - c1 <= 32);
}

/* The 1.5 s above never reach the module's own queue: a pseudo-terminal's
 * buffer takes some 20 KB first, ten seconds of what the module sends. This
 * one waits until the queue holds frames (the status line says `tx-blocked`),
 * then has hangar disarm an armed ArduSub: the disarm's three sends are
 * queued behind a MANUAL_CONTROL that is replaced every 20 ms. When the far
 * end reads again, the fake, which checks signatures and wants rising
 * timestamps as ArduSub does, must have refused nothing and must have the
 * disarm. (A frame signed when it was queued was refused behind the newer
 * MANUAL_CONTROL: ArduSub stayed armed until a later repeat found the queue
 * empty.) The wait for the buffer to fill is bounded generously and is not a
 * measurement. */
static void test_serial_queue_under_backpressure(void)
{
	arm_ok();
	T.cmd = (struct wrench){ .surge = 0.4f };
	pump(200);
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "armed"), 1);
	long long r0 = fake_num(fake_cmd("status"), "rejected");
	long long d0 = fake_num(fake_cmd("status"), "disarms");
	struct mstat m0;
	CHECK(module_status("mavlink", &m0));
	CHECK(strstr(m0.status, "tx-blocked") == NULL);
	CHECK(strncmp(fake_cmd("stopread on"), "ok", 2) == 0);
	uint64_t t0 = now_ms();
	while (!strstr(module_status_line(), "tx-blocked") && now_ms() - t0 < 90000)
		pump(100);
	uint64_t filled = now_ms() - t0;
	CHECK(strstr(module_status_line(), "tx-blocked") != NULL);
	/* hangar disarms into the blocked line: `disarming`, the sends queued. */
	set_safety(0, 0);
	pump(600); /* the three sends, 100 ms apart, each behind a newer MANUAL_CONTROL */
	CHECK(last_ap.armed != 0); /* no answer can have come */
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "disarms"), d0);
	struct mstat m1;
	CHECK(module_status("mavlink", &m1) && strcmp(m1.state, "ready") == 0 && m1.pid == m0.pid);
	/* The far end reads again. */
	CHECK(strncmp(fake_cmd("stopread off"), "ok", 2) == 0);
	CHECK(wait_ap(0, 5000));
	long long d1 = fake_num(fake_cmd("status"), "disarms");
	long long r1 = fake_num(fake_cmd("status"), "rejected");
	CHECK(d1 >= d0 + 1);
	CHECK_EQ_INT(r1, r0); /* nothing the module sent was refused: order kept, signatures good */
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "armed"), 0);
	t0 = now_ms();
	while (strstr(module_status_line(), "tx-blocked") && now_ms() - t0 < 5000)
		pump(50);
	CHECK(strstr(module_status_line(), "tx-blocked") == NULL);
	int mc[8];
	CHECK_EQ_INT(fake_mc(fake_cmd("status"), mc), 0);
	CHECK(mc[0] == 0 && mc[2] == 500); /* and the manual control arriving is the current one: neutral */
	printf("    the queue held frames after %llu ms of a blocked line; %lld disarm(s) delivered, %lld frames refused\n",
	       (unsigned long long)filled, d1 - d0, r1 - r0);
	T.cmd = (struct wrench){ 0 };
}

static void test_serial_vanish(void)
{
	/* The line goes away for 0.3 s (a USB glitch) and comes back on a new
	 * pseudo-terminal behind the same path. The module closes it at once,
	 * says so, keeps running, and reopens at its first retry (0.5 s). (A
	 * longer outage, a Pixhawk rebooting, outlasts StaleAfter + StaleGrace of
	 * the telemetry, and the manager restarts the module, as for an ArduSub
	 * gone silent over UDP; the new instance opens the line when it returns.) */
	struct mstat m0;
	CHECK(module_status("mavlink", &m0));
	CHECK(strncmp(fake_cmd("vanish 0.3"), "ok", 2) == 0);
	uint64_t t0 = now_ms();
	while (!strstr(module_status_line(), "serial down") && now_ms() - t0 < 1500)
		pump(20);
	CHECK(strstr(module_status_line(), "serial down") != NULL);
	if (!strstr(module_status_line(), "serial down"))
		printf("    status: %s\n", module_status_line());
	pump(150); /* the last ardusub.state of before the outage */
	uint64_t rx = last_ap.last_rx_ns;
	pump(200);
	CHECK_EQ_INT(last_ap.last_rx_ns, rx); /* silent while it is gone */
	t0 = now_ms();
	while (!strstr(module_status_line(), "serial open") && now_ms() - t0 < 4000)
		pump(50);
	CHECK(strstr(module_status_line(), "serial open") != NULL);
	t0 = now_ms();
	while (last_ap.last_rx_ns == rx && now_ms() - t0 < 2000)
		pump(50);
	CHECK(last_ap.last_rx_ns > rx);
	struct mstat m1;
	CHECK(module_status("mavlink", &m1) && m1.pid == m0.pid && strcmp(m1.state, "ready") == 0);
	CHECK(wait_file_contains(module_log(), "serial", 1000));
	CHECK_EQ_INT(fake_num(fake_cmd("status"), "ptys"), 2);
	/* MANUAL_CONTROL flows again. */
	long long c0 = fake_num(fake_cmd("status"), "mc_count");
	pump(500);
	CHECK(fake_num(fake_cmd("status"), "mc_count") - c0 >= 18);
}

static void test_serial_exclusive(void)
{
	/* While the module has the line, a second program cannot open it
	 * (TIOCEXCL): a MAVROS left running would otherwise read half the
	 * frames. Root is exempt, so the check is skipped as root. */
	if (geteuid() == 0) {
		printf("    (skipped: root opens an exclusive terminal anyway)\n");
		return;
	}
	int fd = open(line_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	int e = errno;
	if (fd >= 0)
		close(fd);
	CHECK(fd < 0 && e == EBUSY);
}

/* The module's CPU while it runs: telemetry at 50 Hz in, manual control at
 * 50 Hz out, the same on UDP and on the serial line. No busy-waiting
 * (hangar#1): well under a tenth of a core. */
static void test_cpu(void)
{
	struct mstat m;
	CHECK(module_status("mavlink", &m) && m.pid > 0);
	long long t0 = th_cpu_ticks(m.pid);
	uint64_t a = now_ms();
	pump(2000);
	double pct = th_cpu_percent(t0, th_cpu_ticks(m.pid), (long)(now_ms() - a));
	printf("    the module over %s: %.2f %% of a core (telemetry and manual control at 50 Hz)\n",
	       serial_mode ? "a serial line" : "UDP", pct);
	CHECK(pct >= 0 && pct < 10);
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
	serial_mode = getenv("MAVLINK_TEST_SERIAL") && strcmp(getenv("MAVLINK_TEST_SERIAL"), "1") == 0;
	hd_setup(serial_mode ? "mavlink_serial" : "mavlink_vehicle", NULL);
	copy_types();
	RUN_TEST(test_start);
	RUN_TEST(test_battery_unknown);
	RUN_TEST(test_params_and_setup);
	RUN_TEST(test_params_backup);
	RUN_TEST(test_arming_and_commands);
	RUN_TEST(test_disarm_and_disagreement);
	RUN_TEST(test_refusal);
	RUN_TEST(test_restart_time_silence);
	if (serial_mode) {
		RUN_TEST(test_serial_noise);
		RUN_TEST(test_serial_backpressure);
		RUN_TEST(test_serial_queue_under_backpressure);
		RUN_TEST(test_serial_vanish);
		RUN_TEST(test_serial_exclusive);
	} else {
		RUN_TEST(test_signing);
		RUN_TEST(test_other_component);
	}
	RUN_TEST(test_cpu);
	RUN_TEST(test_neutral_command);
	RUN_TEST(test_shutdown);
	if (fake > 0)
		kill(fake, SIGKILL);
	hd_teardown();
	TEST_MAIN_END();
}
