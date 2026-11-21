// SPDX-License-Identifier: GPL-3.0-or-later
// The whole chain in one process tree (modules/PHASE1.md, deliverable 7): two
// managers under two roots, the vehicle's (safety, mux, mavlink) and the
// topside's (topside_mavlink), joined by the same-host shim
// (HANGAR_PEER_TOPIC_DIR, until the bridge); the fake ArduSub behind the
// vehicle's module; this process as QGroundControl on UDP and as a plain
// reader of both roots' topics. QGC's MANUAL_CONTROL becomes pilot.wrench on
// the topside, ctl.wrench through the mux on the vehicle, and reaches the
// fake ArduSub as MANUAL_CONTROL once QGC's arm command has gone through
// safety.arm as topside:topside_mavlink; a disarm makes it neutral; safety
// killed makes it neutral within SafetyTimeout + one period, measured; the
// mux killed likewise. With --sitl (HANGAR_SITL=1, make check-sitl) the fake
// is replaced by the real SITL on 127.0.0.1:14560. Under 30 s.
#include "hd_test.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>

#include "ardusub_state.h"
#include "bridge_link.h"
#include "mav.h"
#include "mavlink_gen.h"
#include "safety_state.h"
#include "thruster_out.h"
#include "wrench.h"

static int sitl;
static pid_t mgr_v, mgr_t, fake;
static const char *T_mavlink, *T_fake, *T_safety, *T_mux;
static char root_v[256], root_t[256], ctl_v[512], ctl_t[512], log_v[512], log_t[512], topics_v[512], topics_t[512];
static int port_module, port_control, port_tmod, port_gcs, gcs_fd;
static uint8_t gcs_seq;
static hg_sub *S_safety, *S_ap, *S_cmd, *S_pilot, *S_thr;
static struct safety_state last_safety;
static struct ardusub_state last_ap;
static struct wrench last_cmd, last_pilot;
static struct thruster_out last_thr;
static int have_safety, have_ap, have_cmd, have_pilot, have_thr;
static struct {
	int heartbeats, acks;
	struct mav_heartbeat hb;
	struct mav_command_ack ack;
	char last_text[51];
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

/* ---- Two roots ----------------------------------------------------------------- */

/* hd_test.h's helpers work on one root at a time: this points them at one. */
static void use_root(int topside)
{
	tfmt(T_root, sizeof T_root, "%s", topside ? root_t : root_v);
	tfmt(T_modules, sizeof T_modules, "%s/etc/hangar/modules", T_root);
	tfmt(T_msg, sizeof T_msg, "%s/usr/share/hangar/msg", T_root);
	tfmt(T_control, sizeof T_control, "%s", topside ? ctl_t : ctl_v);
	tfmt(T_topics, sizeof T_topics, "%s", topside ? topics_t : topics_v);
	tfmt(T_log, sizeof T_log, "%s", topside ? log_t : log_v);
}

static void copy_all_types(const char *dst)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	DIR *d = opendir(src);
	if (!d) {
		fprintf(stderr, "cannot read %s\n", src);
		exit(2);
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 5 || strcmp(e->d_name + n - 4, ".msg") != 0)
			continue;
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s", src, e->d_name);
		tfmt(to, sizeof to, "%s/%s", dst, e->d_name);
		copy_file(from, to);
	}
	closedir(d);
}

static void make_root(const char *root, const char *role)
{
	char p[1024];
	rm_tree(root);
	tfmt(p, sizeof p, "%s/etc/hangar/modules", root);
	mkdir_p(p);
	tfmt(p, sizeof p, "%s/usr/share/hangar/msg", root);
	mkdir_p(p);
	copy_all_types(p);
	tfmt(p, sizeof p, "%s/var/lib/hangar/safety", root);
	mkdir_p(p);
	tfmt(p, sizeof p, "%s/etc/hangar/hangard.conf", root);
	char text[256];
	tfmt(text, sizeof text, "# chain\nRole=%s\nHealthTick=50ms\n", role);
	write_file(p, text);
}

static pid_t start_manager(const char *root, const char *peer_topics, int topside)
{
	setenv("HANGAR_ROOT", root, 1);
	setenv("HANGAR_PEER_TOPIC_DIR", peer_topics, 1);
	use_root(topside);
	pid_t pid = manager_start();
	unsetenv("HANGAR_PEER_TOPIC_DIR");
	return pid;
}

/* ---- The vehicle's and the topside's files -------------------------------------- */

static void write_vehicle(void)
{
	char conf[600], key[600], params[600], latch_params[600];
	tfmt(key, sizeof key, "%s/etc/hangar/mavlink.key", root_v);
	write_file(key, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n");
	tfmt(params, sizeof params, "%s/etc/hangar/ardusub.params", root_v);
	write_file(params, "MAV_GCS_SYSID 255\nFS_GCS_ENABLE 1\nJS_GAIN_DEFAULT 1\nJS_GAIN_MAX 1\nJS_GAIN_MIN 1\nBTN0_FUNCTION 0\n");
	tfmt(conf, sizeof conf, "%s/etc/hangar/mavlink.conf", root_v);
	char text[1024];
	if (sitl)
		tfmt(text, sizeof text,
		     "[Link]\nBind=127.0.0.1:14560\nSigning=no\nSysId=255\nCompId=190\nTargetSysId=1\n[Params]\nFile=%s\n"
		     "[Depth]\nFluidDensity=1000\nSurfacePressure=1013.25\n", params);
	else
		tfmt(text, sizeof text,
		     "[Link]\nBind=127.0.0.1:%d\nSigning=yes\nKeyFile=%s\nSysId=255\nCompId=190\nTargetSysId=1\n[Params]\nFile=%s\n"
		     "[Depth]\nFluidDensity=1000\nSurfacePressure=1013.25\n", port_module, key, params);
	write_file(conf, text);
	/* The safety module's saved parameters: no leak module in this chain. */
	tfmt(latch_params, sizeof latch_params, "%s/var/lib/hangar/safety/params", root_v);
	write_file(latch_params, "# hangar parameters: safety_params\n# version 1, set by test, 2026-11-07T00:00:00Z\n"
				 "leak_required = 0\ntether_enabled = 0\nrequired = \"mux mavlink\"\nbattery_min = 10\n");
	use_root(0);
	manifest("safety", "[Module]\nDescription=Vehicle state\nExec=%s\n"
			   "[Subscribe hangar.vehicle.modules]\n[Subscribe hangar.vehicle.system]\n"
			   "[Subscribe vehicle.leak]\n[Subscribe power.battery]\n[Subscribe nav.depth]\n"
			   "[Subscribe ardusub.state]\n[Subscribe mux.params]\n[Subscribe bridge.vehicle.link]\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Params]\nType=safety_params\nAllow=topside:pilot_console admin topside:admin\n"
			   "[Serve arm]\nRequest=empty\nReply=empty\nAllow=topside:pilot_console topside:topside_mavlink admin topside:admin\n"
			   "[Serve disarm]\nRequest=empty\nReply=empty\nAllow=* topside:* admin topside:admin\n"
			   "[Serve estop]\nRequest=empty\nReply=empty\nAllow=* topside:* admin topside:admin\n"
			   "[Serve clear]\nRequest=empty\nReply=empty\nAllow=topside:pilot_console topside:topside_mavlink admin topside:admin\n"
			   "[Serve check]\nRequest=empty\nReply=check_rep\nAllow=* topside:* admin topside:admin\n"
			   "[Call mavlink.arm]\n[Restart]\nPolicy=always\nBackoff=0.1s..0.2s\n", T_safety);
	/* The bridge's place: test_tether publishes the link as this module. */
	manifest("link", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			 "[Publish bridge.vehicle.link]\nType=bridge_link\nRate=10\n");
	manifest("mux", "[Module]\nExec=%s\n"
			"[Subscribe pilot.wrench]\n[Subscribe ctl.depth_hold]\n[Subscribe nav.depth]\n[Subscribe safety.state]\n"
			"[Publish ctl.wrench]\nType=wrench\nRate=50\nSources=pilot.wrench ctl.depth_hold nav.depth\n"
			"[Publish mux.state]\nType=mux_state\nRate=10\n"
			"[Params]\nType=mux_params\nAllow=topside:pilot_console admin topside:admin\n"
			"[Serve set_mode]\nRequest=set_mode_req\nReply=set_mode_rep\nAllow=topside:pilot_console admin topside:admin\n"
			"[Restart]\nOnGiveUp=failsafe\nBackoff=0.1s..0.2s\n", T_mux);
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
			    "[Restart]\nBackoff=0.1s..0.2s\n", T_mavlink, conf, T_mavlink, conf);
}

static void write_topside(void)
{
	char conf[600], text[512];
	tfmt(conf, sizeof conf, "%s/etc/hangar/topside_mavlink.conf", root_t);
	tfmt(text, sizeof text, "[Link]\nBind=127.0.0.1:%d\nGCS=127.0.0.1:%d\nSysId=1\nCompId=1\nLinkTimeout=1s\n"
				"[Button 2]\nFunction=arm\n[Button 3]\nFunction=disarm\n", port_tmod, port_gcs);
	write_file(conf, text);
	use_root(1);
	manifest("topside_mavlink", "[Module]\nExec=%s --topside %s\n"
				    "[Subscribe safety.state]\n[Subscribe ardusub.state]\n[Subscribe nav.attitude]\n"
				    "[Subscribe nav.depth]\n[Subscribe power.battery]\n[Subscribe hangar.vehicle.system]\n"
				    "[Publish pilot.wrench]\nType=wrench\nRate=50\n"
				    "[Publish pilot.buttons]\nType=pilot_buttons\nRate=50\n"
				    "[Call safety.arm]\nRequest=empty\nReply=empty\n"
				    "[Call safety.disarm]\nRequest=empty\nReply=empty\n"
				    "[Restart]\nBackoff=0.1s..0.2s\n", T_mavlink, conf);
}

/* ---- The fake ArduSub and the fake QGC ---------------------------------------------- */

static void fake_start(void)
{
	char to[64], ctl[64], key[600], params[600];
	tfmt(to, sizeof to, "127.0.0.1:%d", port_module);
	tfmt(ctl, sizeof ctl, "127.0.0.1:%d", port_control);
	tfmt(key, sizeof key, "%s/etc/hangar/mavlink.key", root_v);
	tfmt(params, sizeof params, "%s/etc/hangar/ardusub.params", root_v);
	fake = fork();
	if (fake == 0) {
		char log[600];
		tfmt(log, sizeof log, "%s/fake_ardusub.log", root_v);
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		execl(T_fake, T_fake, "--to", to, "--control", ctl, "--key", key, "--params", params, "--gcs-sysid", "255",
		      (char *)NULL);
		_exit(127);
	}
}

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

/* The fake's view of the last MANUAL_CONTROL: x, z. */
static int fake_mc(int *x, int *z)
{
	const char *p = strstr(fake_cmd("status"), " mc=");
	int v[8];
	if (!p || sscanf(p + 4, "%d,%d,%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 8)
		return -1;
	*x = v[0];
	*z = v[2];
	return 0;
}

static void gcs_send(uint32_t msgid, const uint8_t *payload, size_t len, uint8_t crc)
{
	uint8_t frame[MAV_MAX_FRAME];
	size_t n = mav_encode(frame, sizeof frame, gcs_seq++, 255, MAV_COMP_ID_MISSIONPLANNER, msgid, payload, len, crc, NULL);
	struct sockaddr_in to;
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons((uint16_t)port_tmod);
	sendto(gcs_fd, frame, n, 0, (struct sockaddr *)&to, sizeof to);
}

#define GCS_SEND(NAME, lname, msgp)                                                        \
	do {                                                                               \
		uint8_t pl_[MAV_LEN_##NAME];                                               \
		mav_pack_##lname((msgp), pl_);                                             \
		gcs_send(MAV_MSG_ID_##NAME, pl_, sizeof pl_, MAV_CRC_##NAME);              \
	} while (0)

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
			if (f.msgid == MAV_MSG_ID_HEARTBEAT) {
				mav_unpack_heartbeat(f.payload, f.len, &R.hb);
				R.heartbeats++;
			} else if (f.msgid == MAV_MSG_ID_COMMAND_ACK) {
				mav_unpack_command_ack(f.payload, f.len, &R.ack);
				R.acks++;
			} else if (f.msgid == MAV_MSG_ID_STATUSTEXT) {
				struct mav_statustext s;
				mav_unpack_statustext(f.payload, f.len, &s);
				memcpy(R.last_text, s.text, 50);
				R.last_text[50] = 0;
			}
		}
	}
}

/* QGC's input, sent at 50 Hz by pump. */
static struct {
	int16_t x, y, z, r;
	uint16_t buttons;
	int send;
	uint64_t next_ns;
} Q = { .z = 500, .send = 1 };

static void read_topics(void)
{
	struct hg_info info;
	struct safety_state s;
	while (hg_next(S_safety, &s, sizeof s, &info) == HG_MSG) {
		last_safety = s;
		have_safety = 1;
	}
	struct ardusub_state ap;
	while (hg_next(S_ap, &ap, sizeof ap, &info) == HG_MSG) {
		last_ap = ap;
		have_ap = 1;
	}
	struct wrench w;
	while (hg_next(S_cmd, &w, sizeof w, &info) == HG_MSG) {
		last_cmd = w;
		have_cmd = 1;
	}
	while (hg_next(S_pilot, &w, sizeof w, &info) == HG_MSG) {
		last_pilot = w;
		have_pilot = 1;
	}
	struct thruster_out t;
	while (hg_next(S_thr, &t, sizeof t, &info) == HG_MSG) {
		last_thr = t;
		have_thr = 1;
	}
}

static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	hg_sub *subs[5] = { S_safety, S_ap, S_cmd, S_pilot, S_thr };
	do {
		uint64_t now = hg_now_ns();
		if (Q.send && now >= Q.next_ns) {
			Q.next_ns = now + 20000000u;
			struct mav_manual_control m = { .target = 1, .x = Q.x, .y = Q.y, .z = Q.z, .r = Q.r, .buttons = Q.buttons };
			GCS_SEND(MANUAL_CONTROL, manual_control, &m);
		}
		hg_wait(subs, 5, 5);
		gcs_receive();
		read_topics();
	} while (hg_now_ns() < end);
}

static int wait_safety(int st, long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		pump(10);
		if (have_safety && last_safety.state == st)
			return 1;
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	printf("    safety.state %d (reason '%s'), wanted %d\n", last_safety.state, last_safety.reason, st);
	return 0;
}

static void gcs_arm(int arm)
{
	struct mav_command_long c;
	memset(&c, 0, sizeof c);
	c.target_system = 1;
	c.target_component = 1;
	c.command = MAV_CMD_COMPONENT_ARM_DISARM;
	c.param1 = arm ? 1.0f : 0.0f;
	R.acks = 0;
	GCS_SEND(COMMAND_LONG, command_long, &c);
}

static int near(float a, float b)
{
	return fabsf(a - b) < 0.02f;
}

/* Thrust at the fake, or at SITL (thr.out): is the command out of neutral? */
static int thrust_active(void)
{
	if (sitl)
		return have_thr && (last_thr.pwm[0] != 1500 || last_thr.pwm[1] != 1500 || last_thr.pwm[2] != 1500 ||
				    last_thr.pwm[3] != 1500 || last_thr.pwm[4] != 1500 || last_thr.pwm[5] != 1500);
	int x, z;
	return fake_mc(&x, &z) == 0 && (x != 0 || z != 500);
}

static char cli_out[8192];

/* ---- Tests ------------------------------------------------------------------------ */

static void test_start(void)
{
	port_module = free_udp_port();
	port_control = free_udp_port();
	port_tmod = free_udp_port();
	port_gcs = free_udp_port();
	gcs_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons((uint16_t)port_gcs);
	CHECK_EQ_INT(bind(gcs_fd, (struct sockaddr *)&sa, sizeof sa), 0);
	write_vehicle();
	write_topside();
	if (!sitl)
		fake_start();
	/* Two managers, each with the other's topic directory as its peer. */
	mgr_v = start_manager(root_v, topics_t, 0);
	mgr_t = start_manager(root_t, topics_v, 1);
	/* This process reads the vehicle's topics, and the topside's through the shim. */
	setenv("HANGAR_ROOT", root_v, 1);
	setenv("HANGAR_TOPIC_DIR", topics_v, 1);
	setenv("HANGAR_PEER_TOPIC_DIR", topics_t, 1);
	CHECK_EQ_INT(hg_init(), 0);
	S_safety = hg_sub_open("safety.state", &SAFETY_STATE_TYPE);
	S_ap = hg_sub_open("ardusub.state", &ARDUSUB_STATE_TYPE);
	S_cmd = hg_sub_open("ctl.wrench", &WRENCH_TYPE);
	S_pilot = hg_sub_open("pilot.wrench", &WRENCH_TYPE);
	S_thr = hg_sub_open("thr.out", &THRUSTER_OUT_TYPE);
	CHECK(S_safety && S_ap && S_cmd && S_pilot && S_thr);
	use_root(0);
	CHECK(wait_state("safety", "ready", 8000));
	CHECK(wait_state("mux", "ready", 8000));
	uint64_t t0 = now_ms();
	while (!wait_state("mavlink", "ready", 100) && now_ms() - t0 < 15000)
		pump(50);
	CHECK(wait_state("mavlink", "ready", 100));
	use_root(1);
	CHECK(wait_state("topside_mavlink", "ready", 8000));
	/* QGC speaks; the HEARTBEAT comes back (safety.state reaches the topside
	 * through the shim: the link is up). */
	struct mav_heartbeat h = { .type = MAV_TYPE_GCS, .autopilot = MAV_AUTOPILOT_INVALID, .system_status = 4, .mavlink_version = 3 };
	GCS_SEND(HEARTBEAT, heartbeat, &h);
	t0 = now_ms();
	while (R.heartbeats == 0 && now_ms() - t0 < 3000)
		pump(50);
	CHECK(R.heartbeats > 0);
	CHECK_EQ_INT(R.hb.custom_mode, 19);
	CHECK(wait_safety(0, 2000));
	/* In SITL mode the parameters are set first: hangar ardusub-setup. */
	if (sitl) {
		use_root(0);
		CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "call", "mavlink.setup", (char *)NULL), 0);
	}
	t0 = now_ms();
	/* Parameters confirmed and a HEARTBEAT seen (mode 19 comes from it, at 1 Hz). */
	while ((!have_ap || last_ap.params != 1 || last_ap.mode != 19) && now_ms() - t0 < (sitl ? 30000 : 5000))
		pump(100);
	CHECK_EQ_INT(last_ap.params, 1);
	CHECK_EQ_INT(last_ap.armed, 0);
	CHECK_EQ_INT(last_ap.mode, 19);
	/* The checklist passes now (mux and mavlink ready, battery from ArduSub). It is
	 * a live judgement: a 2 Hz battery message or a telemetry stream that SITL has
	 * just been asked for can leave it false for a moment, so it is polled. */
	use_root(0);
	t0 = now_ms();
	for (;;) {
		CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "call", "safety.check", (char *)NULL), 0);
		if (strcmp(cli_field(cli_out, "ok"), "1") == 0 || now_ms() - t0 > (uint64_t)(sitl ? 10000 : 3000))
			break;
		pump(200);
	}
	CHECK(strcmp(cli_field(cli_out, "ok"), "1") == 0);
	if (strcmp(cli_field(cli_out, "ok"), "1") != 0)
		printf("    check: %s\n", cli_out);
}

/* At rest, every module of the chain sleeps between its messages. Before the
 * fix of 2026-11-19, safety (on a reply topic and the *.req glob's copies),
 * the mux (on the inputs it reads only at its tick) and the recorder and the
 * pairer spun at a full core each, on the vehicle and in the simulator: a
 * subscription in the wait set that is never read stays news (hg_wait_unread).
 * The topside's pilot input arrives at 50 Hz meanwhile, as with QGC idle. */
static void test_idle_cpu(void)
{
	const struct {
		int topside;
		const char *name;
	} mods[] = { { 0, "safety" }, { 0, "mux" }, { 0, "mavlink" }, { 1, "topside_mavlink" } };
	enum { N = sizeof mods / sizeof mods[0] };
	pid_t pid[N];
	long long t0[N];
	for (int i = 0; i < N; i++) {
		use_root(mods[i].topside);
		struct mstat m;
		CHECK(module_status(mods[i].name, &m) && m.pid > 0);
		pid[i] = m.pid;
	}
	use_root(0);
	for (int i = 0; i < N; i++)
		t0[i] = th_cpu_ticks(pid[i]);
	uint64_t start = now_ms();
	pump(1000);
	long ms = (long)(now_ms() - start);
	for (int i = 0; i < N; i++) {
		double pct = th_cpu_percent(t0[i], th_cpu_ticks(pid[i]), ms);
		printf("    %-16s %5.1f %% of a core at rest\n", mods[i].name, pct);
		CHECK(pct >= 0 && pct < 10);
	}
}

static void test_pilot_to_vehicle(void)
{
	/* The joystick reaches the mux as pilot.wrench, the gate as ctl.wrench;
	 * disarmed, ArduSub sees neutral. */
	Q.x = 300;
	pump(400);
	CHECK(have_pilot && near(last_pilot.surge, 0.3f));
	CHECK(have_cmd && near(last_cmd.surge, 0.3f));
	CHECK(!thrust_active());
	/* QGC arms: safety.arm as topside:topside_mavlink, ACCEPTED, then armed. */
	gcs_arm(1);
	uint64_t t0 = now_ms();
	while (R.acks == 0 && now_ms() - t0 < 3000)
		pump(20);
	CHECK_EQ_INT(R.acks, 1);
	CHECK_EQ_INT(R.ack.result, MAV_RESULT_ACCEPTED);
	if (R.ack.result != MAV_RESULT_ACCEPTED)
		printf("    text: %s\n", R.last_text);
	CHECK(wait_safety(1, sitl ? 10000 : 3000));
	CHECK(strcmp(last_safety.by, "topside:topside_mavlink") == 0);
	t0 = now_ms();
	while (!thrust_active() && now_ms() - t0 < 3000)
		pump(20);
	CHECK(thrust_active());
	if (!sitl) {
		int x, z;
		CHECK_EQ_INT(fake_mc(&x, &z), 0);
		CHECK(x == 300 && z == 500);
	}
	pump(1200);
	CHECK(R.hb.base_mode & MAV_MODE_FLAG_SAFETY_ARMED);
	/* QGC disarms: neutral. */
	gcs_arm(0);
	t0 = now_ms();
	while (R.acks == 0 && now_ms() - t0 < 3000)
		pump(20);
	CHECK(R.acks == 1 && R.ack.result == MAV_RESULT_ACCEPTED);
	CHECK(wait_safety(0, 3000));
	t0 = now_ms();
	while (thrust_active() && now_ms() - t0 < 3000)
		pump(20);
	CHECK(!thrust_active());
	t0 = now_ms();
	while ((!have_ap || last_ap.armed != 0) && now_ms() - t0 < 3000)
		pump(20);
	CHECK_EQ_INT(last_ap.armed, 0);
}

/* Arms through QGC and measures how long the thrust stays after `name` is
 * killed with SIGKILL. */
static void kill_and_measure(const char *name, long limit_ms)
{
	gcs_arm(1);
	CHECK(wait_safety(1, sitl ? 10000 : 3000));
	uint64_t t0 = now_ms();
	while (!thrust_active() && now_ms() - t0 < 3000)
		pump(20);
	CHECK(thrust_active());
	use_root(0);
	struct mstat m;
	CHECK(module_status(name, &m) && m.pid > 0);
	kill(m.pid, SIGKILL);
	t0 = now_ms();
	while (thrust_active() && now_ms() - t0 < 3000)
		pump(5);
	uint64_t dt = now_ms() - t0;
	CHECK(!thrust_active());
	CHECK(dt <= (uint64_t)limit_ms);
	printf("    %s killed: neutral at ArduSub after %llu ms (limit %ld)\n", name, (unsigned long long)dt, limit_ms);
	/* The manager brings the module back. */
	CHECK(wait_new_pid(name, m.pid, "ready", 8000));
	if (strcmp(name, "safety") == 0) {
		/* A restarted safety module starts disarmed (no failsafe was latched):
		 * the window closed with its death, and ArduSub was disarmed. */
		have_safety = 0;
		CHECK(wait_safety(0, 5000));
		t0 = now_ms();
		while ((!have_ap || last_ap.armed != 0) && now_ms() - t0 < 5000)
			pump(20);
		CHECK_EQ_INT(last_ap.armed, 0);
	} else {
		/* A dead mux leaves the vehicle armed with neutral output; the restarted
		 * mux carries the pilot's command again. QGC then disarms. */
		CHECK_EQ_INT(last_safety.state, 1);
		t0 = now_ms();
		while (!thrust_active() && now_ms() - t0 < 3000)
			pump(20);
		CHECK(thrust_active());
		gcs_arm(0);
		CHECK(wait_safety(0, 3000));
		t0 = now_ms();
		while ((!have_ap || last_ap.armed != 0) && now_ms() - t0 < 5000)
			pump(20);
		CHECK_EQ_INT(last_ap.armed, 0);
	}
	pump(500);
}

/* ---- The tether (docs/07 "Failsafe", docs/10 "Arming and failsafe") -------------- */

/* The vehicle's bridge, as far as the safety module reads it: this suite
 * executed again as the module `link`, publishing bridge.vehicle.link at
 * 10 Hz, state up. SIGUSR1: last_rx_ns stops advancing (no packet from the
 * topside); SIGUSR2: it advances again. It ends on SIGTERM, when the suite
 * is gone, and after 120 s whatever happens. */
static volatile sig_atomic_t link_frozen, link_stop;

static void link_signal(int sig)
{
	if (sig == SIGUSR1)
		link_frozen = 1;
	else if (sig == SIGUSR2)
		link_frozen = 0;
	else
		link_stop = 1;
}

static int link_child(void)
{
	signal(SIGUSR1, link_signal);
	signal(SIGUSR2, link_signal);
	signal(SIGTERM, link_signal);
	if (hg_init() < 0) {
		fprintf(stderr, "link: hg_init: %s\n", hg_init_error());
		return 2;
	}
	hg_pub *p = hg_pub_open("bridge.vehicle.link", &BRIDGE_LINK_TYPE, NULL);
	if (!p)
		return 3;
	pid_t parent = getppid();
	uint64_t last_rx = 0, end = hg_now_ns() + 120000000000ull;
	while (!link_stop && getppid() == parent && hg_now_ns() < end) {
		uint64_t now = hg_now_ns();
		struct bridge_link l;
		memset(&l, 0, sizeof l);
		l.state = 3;
		l.config_match = 1;
		if (!link_frozen)
			last_rx = now;
		l.last_rx_ns = last_rx;
		hg_publish(p, &l, sizeof l, now);
		sleep_ms(100);
	}
	return 0;
}

static pid_t link_start(void)
{
	char wiring[600];
	use_root(0);
	if (wiring_to_file("link", wiring, sizeof wiring) < 0)
		return -1;
	pid_t pid = fork();
	if (pid == 0) {
		setenv("HANGAR_WIRING_FILE", wiring, 1);
		setenv("HANGAR_MODULE", "link", 1);
		setenv("HANGAR_TOPIC_DIR", topics_v, 1);
		unsetenv("HANGAR_CONTROL");
		unsetenv("HANGAR_PEER_TOPIC_DIR");
		execl("/proc/self/exe", "test_phase1_chain", "--link", (char *)NULL);
		_exit(127);
	}
	return pid;
}

/* The fake's count of MANUAL_CONTROL messages received. */
static long long fake_mc_count(void)
{
	const char *p = strstr(fake_cmd("status"), " mc_count=");
	return p ? atoll(p + 10) : -1;
}

/* safety.clear until safety.state says disarmed: it is refused while a
 * trigger is still active (the link has just come back). */
static int clear_failsafe(void)
{
	use_root(0);
	for (uint64_t t0 = now_ms(); now_ms() - t0 < 8000;) {
		run_cli(cli_out, sizeof cli_out, 5000, "call", "safety.clear", (char *)NULL);
		for (uint64_t t1 = now_ms(); last_safety.state != 0 && now_ms() - t1 < 300;)
			pump(10);
		if (last_safety.state == 0)
			return 1;
	}
	return 0;
}

static int arm_with_link(void)
{
	use_root(0);
	/* The checklist wants the link up and fresh: polled, as at the start. */
	for (uint64_t t0 = now_ms();;) {
		run_cli(cli_out, sizeof cli_out, 5000, "call", "safety.check", (char *)NULL);
		if (strcmp(cli_field(cli_out, "ok"), "1") == 0 || now_ms() - t0 > 5000)
			break;
		pump(100);
	}
	gcs_arm(1);
	if (!wait_safety(1, 3000))
		return 0;
	for (uint64_t t0 = now_ms(); (!have_ap || last_ap.armed != 1) && now_ms() - t0 < 3000;)
		pump(20);
	return last_ap.armed == 1;
}

/* What ArduSub is sent when the tether trigger fires, with the real safety
 * module, mux and mavlink module (the fake only: SITL's thrust is read from
 * thr.out, not from what it was sent). tether_timeout is 1 s here.
 *
 * neutral (the default): MANUAL_CONTROL goes on at 50 Hz, neutral, and
 * ArduSub is disarmed; the pilot's stick, still held and still arriving in
 * this chain (the shim is not a tether), does not reach it.
 * surface: MANUAL_CONTROL goes on at 50 Hz with the mux's heave up, ArduSub
 * stays armed. The module does not stop sending in either (README "Until the
 * bridge"), so ArduSub's own ground-station failsafe does not fire. */
static void test_tether(void)
{
	if (sitl) {
		printf("    (skipped with SITL: the fake ArduSub says what it was sent)\n");
		return;
	}
	pid_t link = link_start();
	CHECK(link > 0);
	use_root(0);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "param", "set", "safety", "tether_enabled=1", "tether_timeout=1",
			     (char *)NULL), 0);
	Q.x = 300;
	CHECK(arm_with_link());
	uint64_t t0 = now_ms();
	while (!thrust_active() && now_ms() - t0 < 3000)
		pump(20);
	int x = 0, z = 0;
	CHECK_EQ_INT(fake_mc(&x, &z), 0);
	CHECK(x == 300 && z == 500);

	/* No packet from the topside: failsafe by `tether`, neutral, outputs off. */
	kill(link, SIGUSR1);
	CHECK(wait_safety(2, 5000));
	t0 = now_ms();
	CHECK(last_safety.triggers & (1u << 1));
	CHECK_EQ_INT(last_safety.action, 0);
	CHECK_EQ_INT(last_safety.outputs_enabled, 0);
	CHECK(strcmp(last_safety.by, "tether") == 0);
	while (thrust_active() && now_ms() - t0 < 3000)
		pump(5);
	uint64_t dt = now_ms() - t0;
	CHECK(!thrust_active());
	printf("    tether, neutral: neutral at ArduSub %llu ms after safety.state said failsafe\n", (unsigned long long)dt);
	for (t0 = now_ms(); (!have_ap || last_ap.armed != 0) && now_ms() - t0 < 5000;)
		pump(20);
	CHECK_EQ_INT(last_ap.armed, 0);
	/* Time must pass here: for a second it goes on at 50 Hz, and every
	 * message looked at is neutral with the stick still at 300. */
	CHECK(have_pilot && near(last_pilot.surge, 0.3f));
	long long c0 = fake_mc_count();
	int neutral = 1;
	for (t0 = now_ms(); now_ms() - t0 < 1000;) {
		pump(50);
		neutral &= fake_mc(&x, &z) == 0 && x == 0 && z == 500;
	}
	long long c1 = fake_mc_count();
	uint64_t ms = now_ms() - t0;
	CHECK(neutral);
	CHECK(c0 >= 0 && (double)(c1 - c0) * 1000.0 / (double)ms > 30);
	printf("    then %lld MANUAL_CONTROL in %llu ms, all neutral, ArduSub disarmed\n", c1 - c0, (unsigned long long)ms);

	/* surface: the link back, cleared, armed again with the stick at rest. */
	kill(link, SIGUSR2);
	Q.x = 0;
	CHECK(clear_failsafe());
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "param", "set", "safety", "action_tether=1", (char *)NULL), 0);
	CHECK(arm_with_link());
	kill(link, SIGUSR1);
	CHECK(wait_safety(2, 5000));
	CHECK(last_safety.triggers & (1u << 1));
	CHECK_EQ_INT(last_safety.action, 1);
	CHECK_EQ_INT(last_safety.outputs_enabled, 1);
	/* surface_thrust 0.5 up: z 750, reached through the mux's rate limit. */
	for (t0 = now_ms(); !(fake_mc(&x, &z) == 0 && z == 750) && now_ms() - t0 < 3000;)
		pump(20);
	CHECK(x == 0 && z == 750);
	/* Time must pass here too: a second later it is still sent at 50 Hz,
	 * still heave up, ArduSub still armed. */
	c0 = fake_mc_count();
	t0 = now_ms();
	pump(1000);
	c1 = fake_mc_count();
	ms = now_ms() - t0;
	CHECK_EQ_INT(fake_mc(&x, &z), 0);
	CHECK(x == 0 && z == 750);
	CHECK(c0 >= 0 && (double)(c1 - c0) * 1000.0 / (double)ms > 30);
	CHECK_EQ_INT(last_ap.armed, 1);
	CHECK_EQ_INT(last_safety.state, 2);
	printf("    tether, surface: %lld MANUAL_CONTROL in %llu ms with z %d, ArduSub armed\n", c1 - c0, (unsigned long long)ms, z);

	/* Back as the other tests expect it: disarmed, the tether not watched. */
	kill(link, SIGUSR2);
	use_root(0);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "call", "safety.disarm", (char *)NULL), 0);
	CHECK(clear_failsafe());
	for (t0 = now_ms(); (!have_ap || last_ap.armed != 0) && now_ms() - t0 < 5000;)
		pump(20);
	CHECK_EQ_INT(last_ap.armed, 0);
	CHECK_EQ_INT(run_cli(cli_out, sizeof cli_out, 5000, "param", "set", "safety", "tether_enabled=0", "action_tether=0",
			     "tether_timeout=3", (char *)NULL), 0);
	kill(link, SIGTERM);
	CHECK_EQ_INT(wait_pid_ms(link, 3000), 0);
	Q.x = 300; /* the stick as the tests after this one found it */
	for (t0 = now_ms(); !near(last_cmd.surge, 0.3f) && now_ms() - t0 < 3000;)
		pump(20);
	CHECK(near(last_cmd.surge, 0.3f));
}

static void test_kill_safety(void)
{
	/* SafetyTimeout 300 ms + one 20 ms period, with the fake's 5 ms poll and
	 * this loop's: 400 ms is the bound asserted. */
	kill_and_measure("safety", 400);
}

static void test_kill_mux(void)
{
	/* A dead mux closes ctl.wrench: the gate sees it at once. */
	kill_and_measure("mux", 400);
}

static void test_shutdown(void)
{
	Q.send = 0;
	use_root(1);
	CHECK_EQ_INT(manager_stop(mgr_t), 0);
	use_root(0);
	CHECK_EQ_INT(manager_stop(mgr_v), 0);
	if (!sitl) {
		fake_cmd("quit");
		wait_pid_ms(fake, 2000);
	}
	CHECK(file_contains(log_v, "mavlink: pid"));
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--link") == 0)
		return link_child();
	sitl = argc > 1 && strcmp(argv[1], "--sitl") == 0;
	T_mavlink = getenv("MAVLINK_BIN");
	T_fake = getenv("FAKE_ARDUSUB_BIN");
	T_safety = getenv("SAFETY_BIN");
	T_mux = getenv("MUX_BIN");
	if (!T_mavlink || !T_fake || !T_safety || !T_mux) {
		fprintf(stderr, "MAVLINK_BIN, FAKE_ARDUSUB_BIN, SAFETY_BIN and MUX_BIN must be set\n");
		return 2;
	}
	hd_setup("chain", NULL);
	/* hd_setup made one root; the chain has two of its own beside it. */
	tfmt(root_v, sizeof root_v, "%s-vehicle", T_root);
	tfmt(root_t, sizeof root_t, "%s-topside", T_root);
	make_root(root_v, "vehicle");
	make_root(root_t, "topside");
	tfmt(ctl_v, sizeof ctl_v, "%s/run/hangar/control", root_v);
	tfmt(ctl_t, sizeof ctl_t, "%s/run/hangar/control", root_t);
	tfmt(log_v, sizeof log_v, "%s/hangard.log", root_v);
	tfmt(log_t, sizeof log_t, "%s/hangard.log", root_t);
	tfmt(topics_v, sizeof topics_v, "%s/dev/shm/hangar", root_v);
	tfmt(topics_t, sizeof topics_t, "%s/dev/shm/hangar", root_t);
	RUN_TEST(test_start);
	RUN_TEST(test_idle_cpu);
	RUN_TEST(test_pilot_to_vehicle);
	RUN_TEST(test_tether);
	RUN_TEST(test_kill_safety);
	RUN_TEST(test_kill_mux);
	RUN_TEST(test_shutdown);
	if (fake > 0)
		kill(fake, SIGKILL);
	if (!getenv("HD_KEEP")) {
		rm_tree(root_v);
		rm_tree(root_t);
	}
	hd_teardown();
	TEST_MAIN_END();
}
