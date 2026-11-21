// SPDX-License-Identifier: GPL-3.0-or-later
// sim_vehicle (modules/sim_vehicle/README.md): docs/14's tests where they can
// run here. The physics through the module's core: neutral buoyancy holds
// depth, a known thrust reaches the terminal velocity the damping gives, the
// restoring moment rights the vehicle, energy decays with no thrust, the
// thrust table gives the same force here as in the depth-hold label's
// conversion, the same seed gives the same readings, a buoyancy change takes
// effect, the battery drains with thrust, SITL's frames. Then the module
// under a manager: standalone (thr.cmd in, the truth and sensor topics out at
// their rates, a live parameter change, its CPU), and as SITL's physics
// against a fake SITL in lockstep. HANGAR_SITL=1 adds the real container.
#include "hd_test.h"

#include "hg_params.h"
#include "hg_topic.h"
#include "sim_vehicle_params.h"
#include "sv.h"
#include "thruster_cmd.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>

static const char *T_sv;

static void defaults(struct sim_vehicle_params *P)
{
	struct hg_param_attr attrs[64];
	char err[200];
	memset(P, 0, sizeof *P);
	if (hg_params_attrs(&SIM_VEHICLE_PARAMS_TYPE, attrs, err, sizeof err) < 0 ||
	    hg_params_defaults(&SIM_VEHICLE_PARAMS_TYPE, attrs, (unsigned char *)P, err, sizeof err) < 0) {
		printf("    defaults: %s\n", err);
		exit(2);
	}
}

/* Noise-free sensors and neutral buoyancy: the physics alone. */
static void quiet(struct sim_vehicle_params *P)
{
	defaults(P);
	P->buoyancy = P->mass * 9.81f;
	P->gyro_noise = P->gyro_bias = P->accel_noise = P->accel_bias = 0;
	P->depth_noise = P->depth_bias = P->attitude_noise = P->attitude_bias = 0;
}

static const uint16_t NEUTRAL[8] = { 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500 };

static void run(struct sv *v, const uint16_t pwm[8], double seconds)
{
	int n = (int)lround(seconds / v->dt);
	for (int i = 0; i < n; i++)
		sv_step(v, pwm);
}

/* ---- The physics, through the core --------------------------------------------- */

static void test_defaults_are_libsims(void)
{
	struct sim_vehicle_params P;
	defaults(&P);
	struct sim_params h = sim_params_bluerov2_heavy();
	CHECK(fabs(P.mass - h.mass) < 1e-6 && fabs(P.buoyancy - h.buoyancy) < 1e-4);
	for (int i = 0; i < 6; i++)
		CHECK(fabs(P.d_lin[i] - h.d_lin[i]) < 1e-5 && fabs(P.d_quad[i] - h.d_quad[i]) < 1e-5);
	struct sim_imu_model m = sim_imu_typical();
	CHECK(fabs(P.gyro_noise - m.gyro.noise_sd) < 1e-9 && fabs(P.accel_bias - m.accel.bias) < 1e-7);
}

static void test_neutral_holds_depth(void)
{
	struct sim_vehicle_params P;
	quiet(&P);
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	v.s.pos[2] = 5;
	run(&v, NEUTRAL, 10);
	printf("    neutral, 10 s at 5 m: depth %.4f m\n", v.s.pos[2]);
	CHECK(fabs(v.s.pos[2] - 5) < 0.01);
}

static void test_terminal_velocity(void)
{
	/* The four vertical thrusters at equal thrust, through the table: a pure
	 * heave force (their roll and pitch moments cancel), and the speed the
	 * damping coefficients give, F = d_lin w + d_quad w^2. Heave, not surge:
	 * moving forward the model rightly pitches (the Munk moment of unequal
	 * added masses), so surge damping alone does not set the forward speed. */
	struct sim_vehicle_params P;
	quiet(&P);
	P.thruster_lag = 0;
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	v.s.pos[2] = 5;
	P.floor_depth = 0;
	uint16_t pwm[8];
	memcpy(pwm, NEUTRAL, sizeof pwm);
	for (int i = 4; i < 8; i++) {
		double best = 1e9;
		for (int w = SIM_T200_PWM_MIN; w <= SIM_T200_PWM_MAX; w++) {
			double d = fabs(sim_t200_thrust(w, v.voltage) - 6.0);
			if (d < best) {
				best = d;
				pwm[i] = (uint16_t)w;
			}
		}
	}
	run(&v, pwm, 20);
	double F = v.tau[2], dl = P.d_lin[2], dq = P.d_quad[2];
	double w = (-dl + sqrt(dl * dl + 4 * dq * F)) / (2 * dq);
	double r, p, y;
	sim_quat_to_euler(v.s.q, &r, &p, &y);
	printf("    heave %.2f N, 20 s: w %.4f m/s, the damping says %.4f (roll %.2f, pitch %.2f deg)\n", F, v.s.nu[2], w,
	       r * 180 / M_PI, p * 180 / M_PI);
	CHECK(F > 20);
	CHECK(fabs(v.tau[3]) < 1e-6 && fabs(v.tau[4]) < 1e-6);
	CHECK(fabs(v.s.nu[2] - w) < 0.01 * w);
}

static void test_restoring_moment(void)
{
	struct sim_vehicle_params P;
	quiet(&P);
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	v.s.pos[2] = 5;
	sim_quat_from_euler(20 * M_PI / 180, 0, 0, v.s.q);
	run(&v, NEUTRAL, 30);
	double r, p, y;
	sim_quat_to_euler(v.s.q, &r, &p, &y);
	printf("    rolled 20 deg, 30 s later: %.2f deg\n", r * 180 / M_PI);
	CHECK(fabs(r) < 2 * M_PI / 180);
}

static void test_energy_decays(void)
{
	struct sim_vehicle_params P;
	quiet(&P);
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	v.s.pos[2] = 5;
	const double nu0[6] = { 0.5, 0.2, 0.1, 0.2, 0.1, 0.3 };
	memcpy(v.s.nu, nu0, sizeof nu0);
	double e = sim_energy(&v.p, &v.s), e0 = e;
	int rises = 0;
	for (int i = 0; i < 2000; i++) {
		sv_step(&v, NEUTRAL);
		double e1 = sim_energy(&v.p, &v.s);
		rises += e1 > e + 1e-9;
		e = e1;
	}
	printf("    energy %.4f J to %.6f J in 10 s, %d rises\n", e0, e, rises);
	CHECK_EQ_INT(rises, 0);
	CHECK(e < e0 * 0.05);
}

static void test_thrust_table_agrees(void)
{
	/* With no lag, one step applies the commanded thrust: the wrench must be
	 * the depth-hold label's conversion of the same pulse widths. */
	struct sim_vehicle_params P;
	quiet(&P);
	P.thruster_lag = 0;
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	struct sim_rng r;
	sim_rng_seed(&r, 99);
	double worst = 0;
	for (int k = 0; k < 500; k++) {
		uint16_t pwm[8];
		for (int i = 0; i < 8; i++)
			pwm[i] = (uint16_t)(1100 + (sim_rng_u64(&r) % 801));
		double volt = v.voltage;
		sv_step(&v, pwm);
		float w[6];
		sim_applied_wrench(pwm, (float)volt, w);
		for (int i = 0; i < 6; i++) {
			double d = fabs(v.tau[i] - w[i]) / (1 + fabs(w[i]));
			if (d > worst)
				worst = d;
		}
	}
	printf("    500 random commands: largest difference %.2e (relative)\n", worst);
	CHECK(worst < 1e-5);
}

static void test_same_seed_same_readings(void)
{
	struct sim_vehicle_params P;
	defaults(&P); /* with the typical noise */
	struct sv a, b, c;
	sv_init(&a, &P, 7, 0.005);
	sv_init(&b, &P, 7, 0.005);
	sv_init(&c, &P, 8, 0.005);
	uint16_t pwm[8] = { 1550, 1450, 1550, 1450, 1600, 1600, 1600, 1600 };
	int same = 1, differs = 0;
	for (int i = 0; i < 1000; i++) {
		sv_step(&a, pwm);
		sv_step(&b, pwm);
		sv_step(&c, pwm);
		double ga[3], aa[3], gb[3], ab[3], gc[3], ac[3];
		sv_imu(&a, ga, aa);
		sv_imu(&b, gb, ab);
		sv_imu(&c, gc, ac);
		double da = sv_depth(&a), db = sv_depth(&b), dc = sv_depth(&c);
		same &= memcmp(ga, gb, sizeof ga) == 0 && memcmp(aa, ab, sizeof aa) == 0 && da == db &&
			memcmp(&a.s, &b.s, sizeof a.s) == 0;
		differs |= memcmp(ga, gc, sizeof ga) != 0 || dc != da;
	}
	CHECK(same);
	CHECK(differs);
}

static void test_buoyancy_change(void)
{
	struct sim_vehicle_params P;
	quiet(&P);
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	v.s.pos[2] = 5;
	run(&v, NEUTRAL, 2);
	double d0 = v.s.pos[2];
	P.buoyancy -= 5; /* 5 N heavier: it sinks */
	sv_set_params(&v, &P);
	run(&v, NEUTRAL, 5);
	printf("    5 N heavier: %.3f m to %.3f m in 5 s\n", d0, v.s.pos[2]);
	CHECK(v.s.pos[2] - d0 > 0.5);
	/* A payload does the same through sim_params_add_payload. */
	quiet(&P);
	struct sv w;
	sv_init(&w, &P, 1, 0.005);
	w.s.pos[2] = 5;
	P.payload_mass = 0.5f;
	sv_set_params(&w, &P);
	run(&w, NEUTRAL, 5);
	CHECK(w.s.pos[2] - 5 > 0.3);
}

static void test_battery_drains(void)
{
	struct sim_vehicle_params P;
	quiet(&P);
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	double v0, a0, r0;
	sv_battery(&v, &v0, &a0, &r0);
	uint16_t pwm[8] = { 1800, 1200, 1800, 1200, 1800, 1800, 1800, 1800 };
	run(&v, pwm, 60);
	double v1, a1, r1;
	sv_battery(&v, &v1, &a1, &r1);
	printf("    60 s of thrust: %.0f A, %.3f to %.3f V, %.4f left\n", a1, v0, v1, r1);
	CHECK(a1 > a0 + 10 && r1 < r0 && v1 < v0);
}

/* What a step costs: physics, thrusters, battery and the delay lines, plus
 * the sensor reads a standalone step makes (the IMU every step, depth and
 * attitude every fourth). Reported, and held to a generous bound. */
static void test_step_cost(void)
{
	struct sim_vehicle_params P;
	defaults(&P);
	struct sv v;
	sv_init(&v, &P, 1, 0.005);
	uint16_t pwm[8] = { 1550, 1450, 1550, 1450, 1600, 1600, 1600, 1600 };
	const int n = 200000; /* 1000 s of simulated time */
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int i = 0; i < n; i++) {
		sv_step(&v, pwm);
		double g[3], ac[3], rpy[3];
		sv_imu(&v, g, ac);
		if (i % 4 == 0) {
			(void)sv_depth(&v);
			sv_attitude(&v, rpy);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	double s = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
	printf("    %d steps in %.3f s: %.2f us a step, %.0fx real time at 200 Hz\n", n, s, s / n * 1e6, n * 0.005 / s);
	CHECK(s / n < 100e-6);
}

static void put16(uint8_t *b, uint16_t x)
{
	b[0] = (uint8_t)x;
	b[1] = (uint8_t)(x >> 8);
}

static size_t servo_packet(uint8_t *b, uint16_t magic, uint16_t rate, uint32_t frame, const uint16_t *pwm, int n)
{
	put16(b, magic);
	put16(b + 2, rate);
	b[4] = (uint8_t)frame;
	b[5] = (uint8_t)(frame >> 8);
	b[6] = (uint8_t)(frame >> 16);
	b[7] = (uint8_t)(frame >> 24);
	for (int i = 0; i < n; i++)
		put16(b + 8 + 2 * i, pwm[i]);
	return 8 + 2 * (size_t)n;
}

static void test_sitl_frames(void)
{
	uint8_t b[128];
	uint16_t pwm[32], out[16], rate;
	uint32_t frame;
	for (int i = 0; i < 32; i++)
		pwm[i] = (uint16_t)(1100 + i);
	size_t n = servo_packet(b, SV_SERVO16_MAGIC, 400, 70000, pwm, 16);
	CHECK_EQ_INT(n, SV_SERVO16_SIZE);
	CHECK_EQ_INT(sv_parse_servo(b, n, &rate, &frame, out), 16);
	CHECK(rate == 400 && frame == 70000 && out[0] == 1100 && out[15] == 1115);
	n = servo_packet(b, SV_SERVO32_MAGIC, 1200, 5, pwm, 32);
	CHECK_EQ_INT(sv_parse_servo(b, n, &rate, &frame, out), 32);
	CHECK(rate == 1200 && out[7] == 1107);
	CHECK_EQ_INT(sv_parse_servo(b, n - 2, &rate, &frame, out), -1); /* short */
	put16(b, 1234);
	CHECK_EQ_INT(sv_parse_servo(b, n, &rate, &frame, out), -1); /* not a servo packet */
	struct sim_vehicle_params P;
	defaults(&P);
	struct sv v;
	sv_init(&v, &P, 1, 0.0025);
	sv_step(&v, NEUTRAL);
	char j[1024];
	int len = sv_json(&v, j, sizeof j);
	CHECK(len > 0 && j[0] == '\n' && j[len - 1] == '\n');
	const char *keys[] = { "\"timestamp\":", "\"imu\":{\"gyro\":[", "\"accel_body\":[", "\"position\":[",
			       "\"quaternion\":[", "\"velocity\":[", "\"battery\":{\"voltage\":" };
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
		CHECK(strstr(j, keys[i]) != NULL);
	double ts = 0;
	CHECK(sscanf(strstr(j, "\"timestamp\":") + 12, "%lf", &ts) == 1 && fabs(ts - 0.0025) < 1e-9);
	CHECK_EQ_INT(sv_json(&v, j, 40), -1);
}

/* ---- The module under a manager ------------------------------------------------- */

static void copy_types(void)
{
	const char *types[] = { "sim_pose", "sim_velocity", "sim_thrust_truth", "thruster_cmd", "sim_vehicle_params",
				"imu", "attitude", "depth", "battery", NULL };
	const char *src = getenv("HANGAR_MSG_SRC");
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src && *src ? src : "msg", types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		if (copy_file(from, to) < 0) {
			printf("    cannot copy %s\n", from);
			exit(2);
		}
	}
}

/* A shipped manifest with the binary's path (and the SITL address) of this
 * build: the suite runs what the release installs. */
static void shipped(const char *name, const char *path, const char *sitl)
{
	const char *t = read_file(path);
	if (!t) {
		printf("    cannot read %s (the suite runs from the repository root)\n", path);
		exit(2);
	}
	static char text[8192], tmp[8192];
	tfmt(text, sizeof text, "%s", t);
	const char *subs[][2] = { { "/usr/lib/hangar/hangar-sim-vehicle", T_sv }, { "127.0.0.1:9002", sitl } };
	for (int k = 0; k < 2; k++) {
		if (!subs[k][1])
			continue;
		char *at = strstr(text, subs[k][0]);
		if (!at)
			continue;
		*at = 0;
		tfmt(tmp, sizeof tmp, "%s%s%s", text, subs[k][1], at + strlen(subs[k][0]));
		tfmt(text, sizeof text, "%s", tmp);
	}
	manifest(name, "%s", text);
}

/* A field of `hangar echo <topic> --count 1`: "name value ..." lines. */
static double echo_field(const char *topic, const char *field, int index)
{
	char out[8192];
	if (run_cli(out, sizeof out, 3000, "echo", topic, "--count", "1", (char *)NULL) != 0)
		return NAN;
	const char *v = cli_field(out, field);
	if (!v)
		return NAN;
	while (*v == '[' || *v == ' ')
		v++;
	double x = NAN;
	for (int i = 0; i <= index; i++) {
		char *end;
		x = strtod(v, &end);
		if (end == v)
			return NAN;
		v = end;
		while (*v == ' ' || *v == ',')
			v++;
	}
	return x;
}

static double hz_of(const char *topic)
{
	char out[2048];
	run_cli(out, sizeof out, 6000, "hz", topic, "--seconds", "2", (char *)NULL);
	const char *p = strstr(out, " Hz");
	if (!p)
		return 0;
	while (p > out && (p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9')))
		p--;
	return strtod(p, NULL);
}

/* Time must pass here: CPU use is measured over ms. */
static double cpu_of(pid_t pid, long ms)
{
	long long t0 = th_cpu_ticks(pid);
	uint64_t a = now_ms();
	sleep_ms(ms);
	return th_cpu_percent(t0, th_cpu_ticks(pid), (long)(now_ms() - a));
}

static pid_t mgr;
static hg_pub *cmd_pub;
static struct thruster_cmd cmd;
static atomic_int feeding;

/* thr.cmd at 50 Hz while `feeding`, as phase 2's thruster module would. */
static void *feed_cmd(void *arg)
{
	(void)arg;
	while (atomic_load(&feeding)) {
		hg_publish(cmd_pub, &cmd, sizeof cmd, hg_now_ns());
		sleep_ms(20);
	}
	return NULL;
}

static void test_standalone(void)
{
	shipped("sim_vehicle", "profiles/sim-vehicle/sim_vehicle.module.off", NULL);
	mgr = manager_start();
	CHECK(mgr > 0);
	CHECK(wait_state("sim_vehicle", "ready", 8000));
	struct mstat m;
	CHECK(module_status("sim_vehicle", &m) && m.pid > 0);
	/* The rates. */
	double r_pose = hz_of("sim.pose"), r_imu = hz_of("nav.imu"), r_depth = hz_of("nav.depth");
	printf("    sim.pose %.1f Hz, nav.imu %.1f Hz, nav.depth %.1f Hz\n", r_pose, r_imu, r_depth);
	CHECK(r_pose > 180 && r_pose < 220);
	CHECK(r_imu > 180 && r_imu < 220);
	CHECK(r_depth > 45 && r_depth < 55);
	CHECK(!isnan(echo_field("power.battery", "voltage_v", 0)));
	/* Floating, at the surface, with nothing commanding it. */
	CHECK(echo_field("sim.pose", "pos", 2) < 0.01);
	/* thr.cmd, from a writer standing in for phase 2's thruster module: the
	 * verticals pushing down take it under. */
	char wiring[700];
	tfmt(wiring, sizeof wiring, "%s/tester.wiring", T_root);
	char text[400];
	tfmt(text, sizeof text, "hangar-wiring 1\nmodule tester\nrole vehicle\npub thr.cmd type=thruster_cmd hash=%016llx slots=16 rate=50\n",
	     (unsigned long long)THRUSTER_CMD_TYPE.hash);
	write_file(wiring, text);
	setenv("HANGAR_WIRING_FILE", wiring, 1);
	setenv("HANGAR_TOPIC_DIR", T_topics, 1);
	unsetenv("HANGAR_CONTROL");
	CHECK_EQ_INT(hg_init(), 0);
	cmd_pub = hg_pub_open("thr.cmd", &THRUSTER_CMD_TYPE, &(struct hg_pub_opts){ .rate_mhz = 50000 });
	/* The wiring is read: the hangar commands this test runs must not take it
	 * for theirs. */
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_TOPIC_DIR");
	CHECK(cmd_pub != NULL);
	double T[6][SIM_NTHRUSTERS];
	sim_alloc_matrix(sim_geometry_bluerov2_heavy(), T);
	for (int i = 0; i < 8; i++)
		cmd.pwm[i] = i < 4 ? 1500 : (T[2][i] > 0 ? 1700 : 1300); /* z is down: push down */
	atomic_store(&feeding, 1);
	pthread_t feeder;
	CHECK(pthread_create(&feeder, NULL, feed_cmd, NULL) == 0);
	/* Until it is half a metre down with that thrust: waited for, up to 20 s. */
	double depth = 0, pwm5 = 0;
	uint64_t t0 = now_ms();
	do {
		sleep_ms(100);
		depth = echo_field("sim.pose", "pos", 2);
		pwm5 = echo_field("sim.thrust", "pwm", 4);
	} while (!(depth > 0.5 && pwm5 == cmd.pwm[4]) && now_ms() - t0 < 20000);
	printf("    thr.cmd down for %.1f s: depth %.2f m, sim.thrust pwm[4] %.0f\n", (double)(now_ms() - t0) / 1e3, depth, pwm5);
	CHECK(depth > 0.5);
	CHECK(pwm5 == cmd.pwm[4]);
	if (pwm5 != cmd.pwm[4]) {
		char out[8192];
		run_cli(out, sizeof out, 3000, "echo", "sim.thrust", "--count", "1", (char *)NULL);
		printf("    echo sim.thrust:\n%s\n", out);
	}
	/* Its CPU while it runs at 200 Hz, with thr.cmd coming in. */
	double pct = cpu_of(m.pid, 2000);
	printf("    running at 200 Hz: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 10);
	atomic_store(&feeding, 0);
	pthread_join(feeder, NULL);
	/* thr.cmd stops: stale after its StaleAfter (3 periods), the thrusters
	 * stop, and it floats back up. */
	for (t0 = now_ms(); echo_field("sim.thrust", "pwm", 4) != 1500 && now_ms() - t0 < 5000;)
		sleep_ms(20);
	CHECK(echo_field("sim.thrust", "pwm", 4) == 1500);
	/* It is still going down from the thrust: waited for until it rises, so
	 * that what follows is the parameter's doing and not what is left of
	 * the dive. */
	double prev = echo_field("sim.pose", "pos", 2), d0 = prev;
	int rising = 0;
	for (t0 = now_ms(); !rising && now_ms() - t0 < 20000; prev = d0) {
		sleep_ms(100);
		d0 = echo_field("sim.pose", "pos", 2);
		rising = d0 < prev;
	}
	CHECK(rising);
	/* A live parameter change: 15 N heavier, it sinks without a restart. */
	char out[2048];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "sim_vehicle", "buoyancy=97.8", (char *)NULL), 0);
	/* Until it has sunk a metre from there: waited for, up to 20 s. */
	double d1 = d0;
	t0 = now_ms();
	do {
		sleep_ms(100);
		d1 = echo_field("sim.pose", "pos", 2);
	} while (!(d1 - d0 > 1.0) && now_ms() - t0 < 20000);
	struct mstat m2;
	CHECK(module_status("sim_vehicle", &m2) && m2.pid == m.pid);
	printf("    buoyancy 114.8 to 97.8 N, live: %.2f m to %.2f m in %.1f s, same pid\n", d0, d1,
	       (double)(now_ms() - t0) / 1e3);
	CHECK(d1 - d0 > 1.0);
}

/* A fake SITL in lockstep: servo packets out, one JSON reply each. */
static int free_udp_port(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	bind(s, (struct sockaddr *)&a, sizeof a);
	socklen_t l = sizeof a;
	getsockname(s, (struct sockaddr *)&a, &l);
	close(s);
	return ntohs(a.sin_port);
}

static int exchange(int s, const struct sockaddr_in *to, uint32_t frame, const uint16_t pwm[16], char *reply, size_t cap)
{
	uint8_t b[64];
	size_t n = servo_packet(b, SV_SERVO16_MAGIC, 400, frame, pwm, 16);
	for (int tries = 0; tries < 50; tries++) {
		sendto(s, b, n, 0, (const struct sockaddr *)to, sizeof *to);
		struct timeval tv = { 0, 100000 };
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		ssize_t r = recv(s, reply, cap - 1, 0);
		if (r > 0) {
			reply[r] = 0;
			return (int)r;
		}
	}
	return -1;
}

static double json_num(const char *j, const char *key, int index)
{
	const char *p = strstr(j, key);
	if (!p)
		return NAN;
	p += strlen(key);
	while (*p == '[' || *p == ' ')
		p++;
	double x = NAN;
	for (int i = 0; i <= index; i++) {
		char *end;
		x = strtod(p, &end);
		p = end;
		while (*p == ',' || *p == ' ')
			p++;
	}
	return x;
}

static void test_sitl_physics(void)
{
	int port = free_udp_port();
	char addr[64];
	tfmt(addr, sizeof addr, "127.0.0.1:%d", port);
	manifest_remove("sim_vehicle");
	shipped("sim_sitl", "modules/sim_vehicle/sim_vehicle.module", addr);
	char out[2048];
	run_cli(out, sizeof out, 5000, "reload", (char *)NULL);
	/* The module is started (it is ready only once frames come; the first
	 * exchange below sends its frame again for 5 s until the socket answers). */
	struct mstat ms;
	memset(&ms, 0, sizeof ms);
	for (uint64_t t0 = now_ms(); !(module_status("sim_sitl", &ms) && ms.pid > 0) && now_ms() - t0 < 5000;)
		sleep_ms(20);
	CHECK(ms.pid > 0);
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
				  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	uint16_t pwm[16];
	for (int i = 0; i < 16; i++)
		pwm[i] = 1500;
	char reply[2048], again[2048];
	/* 400 frames, a second of SITL time at 400 Hz. */
	int ok = 1;
	double last_t = -1;
	for (uint32_t f = 1; f <= 400 && ok; f++) {
		ok = exchange(s, &to, f, pwm, reply, sizeof reply) > 0;
		double t = json_num(reply, "\"timestamp\":", 0);
		ok &= fabs(t - last_t - 0.0025) < 1e-6 || last_t < 0;
		last_t = t;
	}
	CHECK(ok);
	printf("    400 frames at 400 Hz: timestamp %.4f s\n", last_t);
	CHECK(fabs(last_t - 1.0) < 1e-6);
	CHECK(reply[0] == '\n' && strstr(reply, "\"quaternion\":[") && strstr(reply, "\"accel_body\":["));
	/* At rest and level the accelerometer reads -g in z (NED body). */
	CHECK(fabs(json_num(reply, "\"accel_body\":", 2) + 9.81) < 0.3);
	/* SITL resends a frame: the same answer, no step. */
	CHECK(exchange(s, &to, 400, pwm, again, sizeof again) > 0);
	CHECK(strcmp(reply, again) == 0);
	/* Thrust down through SITL's servos 5 to 8: it dives. */
	double T[6][SIM_NTHRUSTERS];
	sim_alloc_matrix(sim_geometry_bluerov2_heavy(), T);
	for (int i = 4; i < 8; i++)
		pwm[i] = T[2][i] > 0 ? 1700 : 1300;
	for (uint32_t f = 401; f <= 1600; f++)
		exchange(s, &to, f, pwm, reply, sizeof reply);
	double d = json_num(reply, "\"position\":", 2);
	printf("    3 s of SITL thrust down: depth %.2f m\n", d);
	CHECK(d > 0.5);
	CHECK(echo_field("sim.pose", "pos", 2) > 0.5);
	/* A restarted SITL counts frames from 1 again: the vehicle starts over. */
	for (int i = 0; i < 16; i++)
		pwm[i] = 1500;
	CHECK(exchange(s, &to, 1, pwm, reply, sizeof reply) > 0);
	CHECK(json_num(reply, "\"timestamp\":", 0) < 0.01 && fabs(json_num(reply, "\"position\":", 2)) < 0.01);
	struct mstat m;
	CHECK(module_status("sim_sitl", &m) && m.pid > 0);
	double pct = cpu_of(m.pid, 1000);
	printf("    idle (no SITL frames): %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 2);
	close(s);
}

/* HANGAR_SITL=1: the real ArduSub SITL, started by hand as the README says
 * (sim_vehicle.py -v ArduSub -f JSON:127.0.0.1 ...), sends its servos to UDP
 * 9002. Waits for its frames and checks ArduSub accepted the physics. */
static void test_real_sitl(void)
{
	const char *on = getenv("HANGAR_SITL");
	if (!on || strcmp(on, "1") != 0) {
		printf("    (skipped: HANGAR_SITL=1 runs it against the real container; README)\n");
		return;
	}
	manifest_remove("sim_sitl");
	char text[1024];
	tfmt(text, sizeof text,
	     "[Module]\nDescription=SITL's physics\nExec=%s --sitl 0.0.0.0:9002\n\n"
	     "[Publish sim.pose]\nType=sim_pose\nRate=200\n\n[Publish sim.velocity]\nType=sim_velocity\nRate=200\n\n"
	     "[Publish sim.thrust]\nType=sim_thrust_truth\nRate=200\n\n[Params]\nType=sim_vehicle_params\nAllow=admin\n\n"
	     "[Health]\nStartGrace=60s\n",
	     T_sv);
	manifest("sim_real", "%s", text);
	char out[2048];
	run_cli(out, sizeof out, 5000, "reload", (char *)NULL);
	CHECK(wait_state("sim_real", "ready", 60000));
	double hz = hz_of("sim.pose");
	printf("    real SITL: sim.pose %.1f Hz\n", hz);
	CHECK(hz > 50);
}

int main(void)
{
	T_sv = getenv("SV_BIN");
	if (!T_sv) {
		fprintf(stderr, "SV_BIN must be set\n");
		return 2;
	}
	RUN_TEST(test_defaults_are_libsims);
	RUN_TEST(test_neutral_holds_depth);
	RUN_TEST(test_terminal_velocity);
	RUN_TEST(test_restoring_moment);
	RUN_TEST(test_energy_decays);
	RUN_TEST(test_thrust_table_agrees);
	RUN_TEST(test_same_seed_same_readings);
	RUN_TEST(test_buoyancy_change);
	RUN_TEST(test_battery_drains);
	RUN_TEST(test_step_cost);
	RUN_TEST(test_sitl_frames);
	hd_setup("sim_vehicle", NULL);
	copy_types();
	RUN_TEST(test_standalone);
	RUN_TEST(test_sitl_physics);
	RUN_TEST(test_real_sitl);
	if (mgr > 0)
		manager_stop(mgr);
	hd_teardown();
	TEST_MAIN_END();
}
