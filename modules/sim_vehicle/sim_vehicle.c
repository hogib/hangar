// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * hangar-sim-vehicle: the vehicle simulator (docs/14-simulation.md,
 * "sim_vehicle"; modules/sim_vehicle/README.md). lib/sim's 6-DOF model at
 * 200 Hz, publishing the truth topics sim.pose, sim.velocity and sim.thrust,
 * in one of two modes:
 *
 *   hangar-sim-vehicle [--seed N]                      without ArduSub: reads
 *       thr.cmd, publishes nav.imu, nav.attitude, nav.depth and power.battery
 *   hangar-sim-vehicle [--seed N] --sitl [host:]port   ArduSub SITL's external
 *       physics over its JSON interface: SITL's servo outputs in, sensors out,
 *       stepped in lockstep with SITL; nav.* and power.battery are then the
 *       mavlink module's, from SITL's telemetry, never this module's
 *
 * Parameters are live (msg/sim_vehicle_params.msg). The loop is one epoll
 * over a timer, the SITL socket and the call topics' eventfd; thr.cmd is
 * read at each tick and is not in the wait set (hangar#1).
 */
#define _GNU_SOURCE
#include "mod.h"

#include "attitude.h"
#include "battery.h"
#include "depth.h"
#include "hg_call.h"
#include "hg_params.h"
#include "hg_topic.h"
#include "imu.h"
#include "sim_pose.h"
#include "sim_thrust_truth.h"
#include "sim_velocity.h"
#include "sim_vehicle_params.h"
#include "sv.h"
#include "thruster_cmd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define WHO "hangar-sim-vehicle"
#define RATE_HZ 200
#define SURFACE_MBAR 1013.25
#define WATER_RHO 1000.0

static struct sim_vehicle_params P;
static struct sv V;
static int sitl;
static hg_pub *pub_pose, *pub_vel, *pub_thrust, *pub_imu, *pub_att, *pub_depth, *pub_bat;
static hg_sub *sub_cmd;
static uint64_t steps, truth_published;
static double next_truth_t;

static int validate(const void *cand, const void *cur, char reason[64])
{
	(void)cur;
	const struct sim_vehicle_params *c = cand;
	if (c->battery_full_v <= c->battery_empty_v) {
		snprintf(reason, 64, "battery_full_v must be above battery_empty_v");
		return -1;
	}
	return 0;
}

static void publish_truth(uint64_t now)
{
	struct sim_pose pose;
	memset(&pose, 0, sizeof pose);
	pose.t = V.s.t;
	double roll, pitch, yaw;
	sim_quat_to_euler(V.s.q, &roll, &pitch, &yaw);
	for (int i = 0; i < 3; i++)
		pose.pos[i] = V.s.pos[i];
	for (int i = 0; i < 4; i++)
		pose.q[i] = (float)V.s.q[i];
	pose.roll = (float)roll;
	pose.pitch = (float)pitch;
	pose.yaw = (float)yaw;
	hg_publish(pub_pose, &pose, sizeof pose, now);

	struct sim_velocity vel;
	memset(&vel, 0, sizeof vel);
	double w[3];
	sim_quat_rotate(V.s.q, V.s.nu, w);
	for (int i = 0; i < 6; i++) {
		vel.nu[i] = (float)V.s.nu[i];
		vel.nu_dot[i] = (float)V.s.nu_dot[i];
	}
	for (int i = 0; i < 3; i++)
		vel.vel_world[i] = (float)w[i];
	hg_publish(pub_vel, &vel, sizeof vel, now);

	struct sim_thrust_truth th;
	memset(&th, 0, sizeof th);
	for (int i = 0; i < SIM_NTHRUSTERS; i++) {
		th.pwm[i] = V.pwm[i];
		th.thrust[i] = (float)V.th.thrust[i];
	}
	for (int i = 0; i < 6; i++)
		th.wrench[i] = (float)V.tau[i];
	th.voltage = (float)V.voltage;
	hg_publish(pub_thrust, &th, sizeof th, now);
	truth_published++;
}

/* Without ArduSub: the sensors at their rates, from the step count. */
static void publish_sensors(uint64_t now)
{
	struct imu imu;
	memset(&imu, 0, sizeof imu);
	double g[3], a[3];
	sv_imu(&V, g, a);
	for (int i = 0; i < 3; i++) {
		imu.gyro[i] = (float)g[i];
		imu.accel[i] = (float)a[i];
	}
	imu.temp_c = 20;
	hg_publish(pub_imu, &imu, sizeof imu, now);
	if (steps % 4 == 0) { /* 50 Hz */
		struct attitude att;
		memset(&att, 0, sizeof att);
		double rpy[3];
		sv_attitude(&V, rpy);
		double q[4];
		sim_quat_from_euler(rpy[0], rpy[1], rpy[2], q);
		for (int i = 0; i < 4; i++)
			att.q[i] = (float)q[i];
		att.roll = (float)rpy[0];
		att.pitch = (float)rpy[1];
		att.yaw = (float)rpy[2];
		for (int i = 0; i < 3; i++)
			att.rates[i] = (float)g[i];
		hg_publish(pub_att, &att, sizeof att, now);
		struct depth d;
		memset(&d, 0, sizeof d);
		double depth = sv_depth(&V);
		d.depth_m = (float)depth;
		d.temp_c = 15;
		d.pressure_mbar = (float)(SURFACE_MBAR + WATER_RHO * SIM_G_N * depth / 100.0);
		hg_publish(pub_depth, &d, sizeof d, now);
	}
	if (steps % RATE_HZ == 0) { /* 1 Hz */
		struct battery b;
		double volt, amps, rem;
		sv_battery(&V, &volt, &amps, &rem);
		b.voltage_v = (float)volt;
		b.current_a = (float)amps;
		b.remaining = (float)rem;
		hg_publish(pub_bat, &b, sizeof b, now);
	}
}

static void standalone_tick(uint64_t now)
{
	uint16_t pwm[SIM_NTHRUSTERS];
	for (int i = 0; i < SIM_NTHRUSTERS; i++)
		pwm[i] = SIM_T200_PWM_NEUTRAL;
	/* The last command is held between its messages (thr.cmd comes at the
	 * thruster module's rate, below this tick's) while it is fresh; stale,
	 * absent or computed from stale inputs, the thrusters stop. */
	static struct thruster_cmd cmd;
	static struct hg_info info;
	static int have;
	if (mod_latest(sub_cmd, &cmd, sizeof cmd, &info, &have) && mod_fresh(sub_cmd, &info, now) &&
	    !(info.flags & HG_STALE))
		memcpy(pwm, cmd.pwm, sizeof pwm);
	sv_step(&V, pwm);
	steps++;
	publish_truth(now);
	publish_sensors(now);
}

/* ---- SITL's JSON interface ------------------------------------------------- */

static int sock = -1;
static uint32_t last_frame;
static int have_frame;
static char reply[1024];
static int reply_len;
static uint64_t sitl_frames, sitl_resets;

static void sitl_packet(uint64_t now)
{
	unsigned char buf[256];
	struct sockaddr_storage from;
	socklen_t fl = sizeof from;
	for (;;) {
		ssize_t n = recvfrom(sock, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
		if (n < 0)
			return;
		uint16_t rate, pwm16[16];
		uint32_t frame;
		if (sv_parse_servo(buf, (size_t)n, &rate, &frame, pwm16) < 0)
			continue;
		if (have_frame && frame == last_frame) {
			/* SITL resent its servos: the same answer, no step. */
		} else {
			if (have_frame && frame < last_frame) {
				/* A restarted SITL counts from 0 again: so does the vehicle. */
				sv_init(&V, &P, V.rng.s, V.dt);
				next_truth_t = 0;
				sitl_resets++;
				mod_log(WHO, "SITL restarted (frame %u after %u): the vehicle starts again at rest",
					(unsigned)frame, (unsigned)last_frame);
			}
			if (rate > 0)
				V.dt = 1.0 / rate;
			sv_step(&V, pwm16);
			steps++;
			sitl_frames++;
			last_frame = frame;
			have_frame = 1;
			if (V.s.t + 1e-9 >= next_truth_t) {
				publish_truth(now);
				next_truth_t = V.s.t + 1.0 / RATE_HZ;
			}
			reply_len = sv_json(&V, reply, sizeof reply);
		}
		if (reply_len > 0)
			(void)!sendto(sock, reply, (size_t)reply_len, 0, (struct sockaddr *)&from, fl);
	}
}

static int sitl_open(const char *spec)
{
	char host[128] = "0.0.0.0";
	const char *port = spec;
	const char *colon = strrchr(spec, ':');
	if (colon) {
		size_t l = (size_t)(colon - spec);
		if (l >= sizeof host)
			return -1;
		memcpy(host, spec, l);
		host[l] = 0;
		port = colon + 1;
	}
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM, .ai_flags = AI_PASSIVE }, *ai;
	if (getaddrinfo(host, port, &hints, &ai) != 0)
		return -1;
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	int r = bind(sock, ai->ai_addr, ai->ai_addrlen);
	freeaddrinfo(ai);
	return r;
}

/* ---- The loop ---------------------------------------------------------------- */

static void status_line(void)
{
	char line[200];
	double volt, amps, rem;
	sv_battery(&V, &volt, &amps, &rem);
	if (sitl)
		mod_fmt(line, sizeof line, "SITL physics: %llu frames at %.0f Hz, %llu resets; t %.1f s, depth %.2f m, battery %.1f V",
			(unsigned long long)sitl_frames, V.dt > 0 ? 1.0 / V.dt : 0.0, (unsigned long long)sitl_resets,
			V.s.t, V.s.pos[2], volt);
	else
		mod_fmt(line, sizeof line, "standalone %d Hz: t %.1f s, depth %.2f m, %.1f A, battery %.1f V (%.0f %%)",
			RATE_HZ, V.s.t, V.s.pos[2], amps, volt, rem * 100);
	hg_status(line);
}

int main(int argc, char **argv)
{
	uint64_t seed = 1;
	const char *sitl_spec = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
			seed = strtoull(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--sitl") == 0 && i + 1 < argc)
			sitl_spec = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--seed N] [--sitl [host:]port]\n", argv[0]);
			return 64;
		}
	}
	sitl = sitl_spec != NULL;
	mod_init(WHO);
	if (hg_params_open(&SIM_VEHICLE_PARAMS_TYPE, &P, validate, NULL) < 0) {
		mod_log(WHO, "hg_params_open: %s", strerror(errno));
		return 4;
	}
	sv_init(&V, &P, seed, 1.0 / RATE_HZ);
	struct hg_pub_opts r200 = { .rate_mhz = RATE_HZ * 1000u };
	pub_pose = hg_pub_open("sim.pose", &SIM_POSE_TYPE, &r200);
	pub_vel = hg_pub_open("sim.velocity", &SIM_VELOCITY_TYPE, &r200);
	pub_thrust = hg_pub_open("sim.thrust", &SIM_THRUST_TRUTH_TYPE, &r200);
	if (!pub_pose || !pub_vel || !pub_thrust) {
		mod_log(WHO, "hg_pub_open: %s", strerror(errno));
		return 4;
	}
	if (!sitl) {
		sub_cmd = hg_sub_open("thr.cmd", &THRUSTER_CMD_TYPE);
		pub_imu = hg_pub_open("nav.imu", &IMU_TYPE, &r200);
		pub_att = hg_pub_open("nav.attitude", &ATTITUDE_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
		pub_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
		pub_bat = hg_pub_open("power.battery", &BATTERY_TYPE, &(struct hg_pub_opts){ .rate_mhz = 1000 });
		if (!sub_cmd || !pub_imu || !pub_att || !pub_depth || !pub_bat) {
			mod_log(WHO, "opening thr.cmd and the sensor topics: %s", strerror(errno));
			return 4;
		}
	} else if (sitl_open(sitl_spec) < 0) {
		mod_log(WHO, "cannot listen for SITL on %s: %s", sitl_spec, strerror(errno));
		hg_status("cannot listen for SITL (the log says why)");
		return 78;
	}

	int ep = epoll_create1(EPOLL_CLOEXEC);
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	long period = sitl ? 1000000000L : 1000000000L / RATE_HZ;
	struct itimerspec its = { .it_interval = { period / 1000000000L, period % 1000000000L },
				  .it_value = { period / 1000000000L, period % 1000000000L } };
	timerfd_settime(tfd, 0, &its, NULL);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
	epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
	if (sitl) {
		ev.data.fd = sock;
		epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);
	}
	/* The call topics (set_params): only what hg_serve reads (hangar#1). */
	hg_sub *subs[HG_MAX_WAIT_FD];
	int n = hg_call_subs(subs, HG_MAX_WAIT_FD);
	int efd = n > 0 ? hg_wait_fd(subs, n) : -1;
	if (efd >= 0) {
		ev.data.fd = efd;
		epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
	}
	mod_log(WHO, "%s, seed %llu, mass %.2f kg, buoyancy %.1f N",
		sitl ? "SITL's external physics" : "standalone, without ArduSub", (unsigned long long)seed,
		(double)P.mass, (double)P.buoyancy);
	if (!sitl)
		hg_ready(); /* also ready by its first publishes; said at once so the status shows it */
	uint64_t last_status = 0;
	while (!mod_stop) {
		struct epoll_event evs[4];
		int k = epoll_wait(ep, evs, 4, 1000);
		if (k < 0 && errno != EINTR)
			break;
		uint64_t now = hg_now_ns();
		for (int i = 0; i < k; i++) {
			int fd = evs[i].data.fd;
			if (fd == tfd) {
				uint64_t ticks = 0;
				if (read(tfd, &ticks, sizeof ticks) == sizeof ticks && !sitl) {
					/* Late ticks are stepped too: simulated time keeps pace
					 * with the clock, up to a second behind. */
					if (ticks > RATE_HZ)
						ticks = RATE_HZ;
					for (uint64_t t = 0; t < ticks; t++)
						standalone_tick(now);
				}
			} else if (fd == sock) {
				sitl_packet(now);
			} else if (fd == efd) {
				uint64_t v;
				(void)!read(efd, &v, sizeof v);
			}
		}
		if (n > 0)
			hg_serve();
		if (hg_params_changed()) {
			sv_set_params(&V, &P);
			mod_log(WHO, "parameters v%llu by %s: mass %.2f kg, buoyancy %.1f N, payload %.2f kg",
				(unsigned long long)hg_params_version(), hg_params_changed_by(), (double)P.mass,
				(double)P.buoyancy, (double)P.payload_mass);
		}
		if (now - last_status >= 1000000000ull) {
			last_status = now;
			status_line();
			hg_alive();
		}
	}
	mod_log(WHO, "stopping: %llu steps, t %.1f s", (unsigned long long)steps, V.s.t);
	if (efd >= 0)
		hg_wait_fd_close();
	return 0;
}
