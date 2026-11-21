// SPDX-License-Identifier: GPL-3.0-or-later
// The joystick module (hangar#3): joystick.conf and the shaping alone; then the
// module under a manager, its controller a FIFO this process writes struct
// js_event records into, standing in for /dev/input/jsN: the axes mapped,
// inverted, with deadband and expo, into pilot.wrench at 50 Hz; a button
// marked arm or disarm making one call per press and none for a button held
// when the controller appears; one marked pass setting its bit in
// pilot.buttons while held; the controller unplugged (its node gone, the
// writer closed): pilot.wrench neutral and flagged stale, once logged, the
// module still healthy and not restarted; plugged in again: picked up without
// a restart; and its idle CPU. This process also plays `safety`, serving arm
// and disarm. No root, no uinput. Under 30 s.
#include "hd_test.h"

#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>

#include "empty.h"
#include "hg_call.h"
#include "hg_ini.h"
#include "joystick_map.h"
#include "pilot_buttons.h"
#include "wrench.h"

static pid_t mgr;
static const char *T_joy;
static char conf_path[600], fifo_path[600], mod_log[700];
static int wfd = -1;
static hg_sub *S_wrench, *S_buttons;
static struct wrench last_w;
static struct hg_info last_w_info;
static struct pilot_buttons last_b;
static int have_w, arms, disarms;

static int near(float a, float b)
{
	return fabsf(a - b) < 0.002f;
}

/* ---- joystick.conf and the shaping, alone -------------------------------- */

static int parse(const char *text, struct js_conf *c, char *err, size_t len)
{
	js_conf_defaults(c);
	return js_conf_parse(text, strlen(text), c, err, len);
}

static void test_conf(void)
{
	struct js_conf c;
	char err[256];
	CHECK_EQ_INT(parse("", &c, err, sizeof err), 0);
	CHECK(strcmp(c.device, "/dev/input/js0") == 0 && c.reopen_ns == 1000000000ull);
	CHECK(c.axis[0].source == 4 && c.axis[0].invert && c.axis[2].source == 1 && c.axis[2].invert);
	CHECK(c.axis[3].source == -1 && c.axis[4].source == -1 && c.axis[5].source == 0);
	CHECK_EQ_INT(parse("[Joystick]\nDevice=auto\nName=Xbox\nReopen=200ms\n[Axis pitch]\nSource=2\nInvert=yes\n"
			   "Deadband=0.2\nExpo=0.5\n[Button 7]\nFunction=arm\n[Button 6]\nFunction=pass\n",
			   &c, err, sizeof err),
		     0);
	CHECK(strcmp(c.device, "auto") == 0 && strcmp(c.name, "Xbox") == 0 && c.reopen_ns == 200000000ull);
	CHECK(c.axis[4].source == 2 && c.axis[4].invert && near(c.axis[4].deadband, 0.2f) && near(c.axis[4].expo, 0.5f));
	CHECK(c.button[7] == JS_FN_ARM && c.button[6] == JS_FN_PASS && c.button[0] == JS_FN_NONE);
	// A section replaces its axis: unstated keys are plain, not the default map's.
	CHECK_EQ_INT(parse("[Axis heave]\nSource=2\n[Axis surge]\n", &c, err, sizeof err), 0);
	CHECK(c.axis[2].source == 2 && !c.axis[2].invert && near(c.axis[2].deadband, 0.05f) && near(c.axis[2].expo, 0));
	CHECK(c.axis[0].source == -1 && !c.axis[0].invert); /* surge: unmapped by its empty section */
	CHECK(c.axis[5].source == 0); /* yaw: no section, the default stays */
	// The shipped joystick.conf parses and says the built-in defaults, so that
	// the two cannot drift apart (the suite runs from the repository's root).
	static char shipped[16384];
	size_t slen;
	if (hg_ini_read_file("modules/joystick/joystick.conf", shipped, sizeof shipped, &slen) == 0) {
		struct js_conf d;
		js_conf_defaults(&d);
		CHECK_EQ_INT(js_conf_parse(shipped, slen, &c, err, sizeof err), 0);
		js_conf_defaults(&c);
		CHECK_EQ_INT(js_conf_parse(shipped, slen, &c, err, sizeof err), 0);
		CHECK(memcmp(&c, &d, sizeof c) == 0);
	} else {
		printf("    (modules/joystick/joystick.conf not found from here: not compared)\n");
	}
	// Refusals, each with its reason.
	const char *bad[][2] = {
		{ "[Axis spin]\nSource=1\n", "[Axis spin]" },
		{ "[Axis yaw]\nSource=40\n", "Source:" },
		{ "[Axis yaw]\nDeadband=0.95\n", "Deadband:" },
		{ "[Axis yaw]\nExpo=2\n", "Expo:" },
		{ "[Button 40]\nFunction=arm\n", "[Button 40]" },
		{ "[Button 1]\nFunction=fire\n", "Function:" },
		{ "[Joystick]\nDevice=auto\n", "Name=" },
		{ "[Joystick]\nReopen=1ms\n", "Reopen:" },
		{ "[Joystick]\nSpeed=3\n", "unknown key" },
		{ "[Mixer]\n", "unknown section" },
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		int r = parse(bad[i][0], &c, err, sizeof err);
		CHECK(r == -1 && strstr(err, bad[i][1]) != NULL);
		if (r != -1 || !strstr(err, bad[i][1]))
			printf("    %s -> %s\n", bad[i][0], err);
	}
}

static void test_shaping(void)
{
	struct js_axis_conf a = { .source = 0, .deadband = 0 };
	CHECK(near(js_shape(&a, 0), 0) && near(js_shape(&a, 32767), 1) && near(js_shape(&a, -32767), -1));
	CHECK(near(js_shape(&a, -32768), -1)); /* the one value past the range, clamped */
	CHECK(near(js_shape(&a, 16384), 0.5f));
	a.invert = 1;
	CHECK(near(js_shape(&a, 16384), -0.5f));
	// Deadband: inside it 0; outside it rescaled to reach 1 at full travel.
	a = (struct js_axis_conf){ .source = 0, .deadband = 0.1f };
	CHECK(near(js_shape(&a, 3000), 0) && near(js_shape(&a, -3000), 0));
	CHECK(near(js_shape(&a, 32767), 1));
	CHECK(near(js_shape(&a, 16384), (0.5f - 0.1f) / 0.9f));
	// Expo 1: cubic.
	a = (struct js_axis_conf){ .source = 0, .expo = 1 };
	CHECK(near(js_shape(&a, 16384), 0.125f) && near(js_shape(&a, -16384), -0.125f) && near(js_shape(&a, 32767), 1));
	// Events: an INIT press is the state, not a press; a real press is.
	struct js_state s;
	memset(&s, 0, sizeof s);
	js_apply(&s, &(struct js_ev){ .type = JS_EV_BUTTON | JS_EV_INIT, .number = 3, .value = 1 });
	CHECK(s.buttons == 1u << 3 && s.pressed == 0);
	js_apply(&s, &(struct js_ev){ .type = JS_EV_BUTTON, .number = 3, .value = 0 });
	js_apply(&s, &(struct js_ev){ .type = JS_EV_BUTTON, .number = 3, .value = 1 });
	CHECK(s.buttons == 1u << 3 && s.pressed == 1u << 3);
	js_apply(&s, &(struct js_ev){ .type = JS_EV_AXIS, .number = 4, .value = -32767 });
	js_apply(&s, &(struct js_ev){ .type = JS_EV_AXIS, .number = 200, .value = 1 }); /* out of range: ignored */
	struct js_conf c;
	js_conf_defaults(&c);
	float w[JS_WRENCH_AXES];
	js_wrench(&c, &s, w);
	CHECK(near(w[0], 1) && near(w[1], 0) && near(w[2], 0) && near(w[5], 0)); /* stick up, inverted: forward */
}

/* ---- The module under a manager ------------------------------------------ */

static void ev(int16_t value, uint8_t type, uint8_t number)
{
	struct js_ev e = { .time = (uint32_t)now_ms(), .value = value, .type = type, .number = number };
	if (write(wfd, &e, sizeof e) != (ssize_t)sizeof e)
		printf("    write to the FIFO: %s\n", strerror(errno));
}

static void plug_in(void)
{
	if (mkfifo(fifo_path, 0600) < 0 && errno != EEXIST)
		printf("    mkfifo: %s\n", strerror(errno));
	wfd = open(fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC); /* a writer that never blocks */
	if (wfd < 0)
		printf("    open the FIFO: %s\n", strerror(errno));
}

/* The node goes first, then the writer: the module reads the end of its input
 * and finds no device to open again, as when a controller is pulled out. */
static void unplug(void)
{
	unlink(fifo_path);
	close(wfd);
	wfd = -1;
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
	(void)reason;
	if (strcmp(c->caller, "joystick") != 0)
		printf("    arm from %s\n", c->caller);
	arms++;
	return HG_OK;
}

static int on_disarm(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	(void)rep;
	(void)reason;
	disarms++;
	return HG_OK;
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
		hg_wait(subs, n, 5);
		hg_serve();
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
	} while (hg_now_ns() < end);
}

static int stale(void)
{
	return (last_w_info.flags & HG_SLOT_STALE) != 0;
}

static void test_start(void)
{
	tfmt(fifo_path, sizeof fifo_path, "%s/js0", T_root);
	tfmt(conf_path, sizeof conf_path, "%s/joystick.conf", T_root);
	tfmt(mod_log, sizeof mod_log, "%s/run/hangar/log/joystick.log", T_root);
	// The test's own map, unlike the defaults so that each key shows.
	char text[1024];
	tfmt(text, sizeof text,
	     "[Joystick]\nDevice=%s\nReopen=100ms\n"
	     "[Axis surge]\nSource=1\nInvert=yes\nDeadband=0\n"
	     "[Axis sway]\nSource=0\nDeadband=0\n"
	     "[Axis heave]\nSource=2\nDeadband=0.1\n"
	     "[Axis yaw]\nSource=3\nDeadband=0\nExpo=1\n"
	     "[Button 0]\nFunction=arm\n[Button 1]\nFunction=disarm\n[Button 2]\nFunction=pass\n",
	     fifo_path);
	write_file(conf_path, text);
	manifest("joystick", "[Module]\nExec=%s %s\n"
			     "[Publish pilot.wrench]\nType=wrench\nRate=50\n"
			     "[Publish pilot.buttons]\nType=pilot_buttons\nRate=50\n"
			     "[Call safety.arm]\nRequest=empty\nReply=empty\n"
			     "[Call safety.disarm]\nRequest=empty\nReply=empty\n"
			     "[Restart]\nBackoff=0.1s..0.2s\n",
		 T_joy, conf_path);
	manifest("safety", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe pilot.wrench]\n[Subscribe pilot.buttons]\n"
			   "[Serve arm]\nRequest=empty\nReply=empty\nAllow=joystick admin\n"
			   "[Serve disarm]\nRequest=empty\nReply=empty\nAllow=* admin\n");
	plug_in();
	// A button held when the controller appears: the driver's INIT events.
	ev(1, JS_EV_BUTTON | JS_EV_INIT, 0);
	ev(0, JS_EV_AXIS | JS_EV_INIT, 1);
	mgr = manager_start();
	CHECK(wait_state("safety", "stopped", 5000));
	become("safety");
	S_wrench = hg_sub_open("pilot.wrench", &WRENCH_TYPE);
	S_buttons = hg_sub_open("pilot.buttons", &PILOT_BUTTONS_TYPE);
	CHECK(S_wrench && S_buttons);
	CHECK_EQ_INT(hg_serve_method("arm", on_arm, NULL), 0);
	CHECK_EQ_INT(hg_serve_method("disarm", on_disarm, NULL), 0);
	uint64_t t0 = now_ms();
	while (!wait_state("joystick", "ready", 100) && now_ms() - t0 < 8000)
		pump(50);
	CHECK(wait_state("joystick", "ready", 100));
	pump(300);
	CHECK(have_w && !stale() && near(last_w.surge, 0));
	CHECK(wait_file_contains(mod_log, "controller ", 2000));
	CHECK_EQ_INT(arms, 0); /* held at plug-in: not a press */
	// 50 Hz.
	uint64_t s0 = last_w_info.seq;
	pump(1000);
	uint64_t per_s = last_w_info.seq - s0;
	printf("    pilot.wrench: %llu messages in 1 s\n", (unsigned long long)per_s);
	CHECK(per_s >= 45 && per_s <= 55);
}

/* Pumps until cond holds, up to ms: for a value that follows an event, in
 * place of a fixed pump and a look. The CHECK after it is the test. */
#define PUMP_UNTIL(cond, ms)                                               \
	do {                                                               \
		uint64_t t0_ = now_ms();                                   \
		while (!(cond) && now_ms() - t0_ < (uint64_t)(ms))         \
			pump(10);                                          \
	} while (0)

static void test_axes_and_buttons(void)
{
	ev(-32767, JS_EV_AXIS, 1); /* surge, inverted: forward full */
	ev(16384, JS_EV_AXIS, 0);  /* sway: half right */
	ev(3000, JS_EV_AXIS, 2);   /* heave: inside its 0.1 deadband */
	ev(16384, JS_EV_AXIS, 3);  /* yaw, expo 1: half travel is 0.125 */
	PUMP_UNTIL(near(last_w.surge, 1) && near(last_w.sway, 0.5f) && near(last_w.yaw, 0.125f), 5000);
	CHECK(near(last_w.surge, 1) && near(last_w.sway, 0.5f) && near(last_w.heave, 0) && near(last_w.yaw, 0.125f));
	CHECK(near(last_w.roll, 0) && near(last_w.pitch, 0) && !stale());
	ev(32767, JS_EV_AXIS, 2); /* heave past its deadband: full */
	PUMP_UNTIL(near(last_w.heave, 1), 5000);
	CHECK(near(last_w.heave, 1));
	if (!near(last_w.heave, 1))
		printf("    heave %.4f\n", last_w.heave);
	// The held button let go and pressed again: one arm; then disarm.
	ev(0, JS_EV_BUTTON, 0);
	ev(1, JS_EV_BUTTON, 0);
	PUMP_UNTIL(arms >= 1, 5000);
	pump(400); /* and no second one after it */
	CHECK_EQ_INT(arms, 1);
	ev(0, JS_EV_BUTTON, 0);
	ev(1, JS_EV_BUTTON, 1);
	ev(0, JS_EV_BUTTON, 1);
	PUMP_UNTIL(disarms >= 1, 5000);
	pump(400);
	CHECK_EQ_INT(disarms, 1);
	CHECK_EQ_INT(arms, 1);
	// pass: its bit while held, nothing else.
	ev(1, JS_EV_BUTTON, 2);
	PUMP_UNTIL(last_b.pressed != 0, 5000);
	CHECK_EQ_INT(last_b.pressed, 1u << 2);
	ev(0, JS_EV_BUTTON, 2);
	PUMP_UNTIL(last_b.pressed == 0, 5000);
	CHECK_EQ_INT(last_b.pressed, 0);
	CHECK(wait_file_contains(mod_log, "arm (button): accepted", 2000));
}

static void test_unplug_and_back(void)
{
	struct mstat m0;
	CHECK(module_status("joystick", &m0));
	unplug();
	uint64_t t0 = now_ms();
	while ((!stale() || !near(last_w.surge, 0)) && now_ms() - t0 < 2000)
		pump(20);
	uint64_t dt = now_ms() - t0;
	CHECK(stale() && near(last_w.surge, 0) && near(last_w.sway, 0) && near(last_w.yaw, 0));
	printf("    unplugged: neutral and flagged stale after %llu ms\n", (unsigned long long)dt);
	// Still publishing at 50 Hz, healthy, not restarted, and said once.
	uint64_t s0 = last_w_info.seq;
	pump(1500);
	CHECK(last_w_info.seq - s0 >= 65);
	CHECK(stale());
	struct mstat m;
	CHECK(module_status("joystick", &m));
	CHECK(strcmp(m.state, "ready") == 0 && m.pid == m0.pid && m.failures == m0.failures);
	CHECK(strstr(m.status, "no controller") != NULL);
	const char *log = read_file(mod_log);
	int gone = 0;
	for (const char *p = log; p && (p = strstr(p, "gone (")) != NULL; p++)
		gone++;
	CHECK_EQ_INT(gone, 1);
	// Plugged in again: picked up within the Reopen interval, same process.
	plug_in();
	ev(-16384, JS_EV_AXIS | JS_EV_INIT, 1);
	t0 = now_ms();
	while ((stale() || !near(last_w.surge, 0.5f)) && now_ms() - t0 < 2000)
		pump(20);
	dt = now_ms() - t0;
	CHECK(!stale() && near(last_w.surge, 0.5f));
	printf("    plugged in again: read after %llu ms\n", (unsigned long long)dt);
	CHECK(module_status("joystick", &m) && m.pid == m0.pid && strcmp(m.state, "ready") == 0);
}

static void test_idle_cpu(void)
{
	struct mstat m;
	CHECK(module_status("joystick", &m) && m.pid > 0);
	long long t0 = th_cpu_ticks(m.pid);
	uint64_t start = now_ms();
	pump(1000);
	double pct = th_cpu_percent(t0, th_cpu_ticks(m.pid), (long)(now_ms() - start));
	printf("    joystick: %.1f %% of a core at rest\n", pct);
	CHECK(pct >= 0 && pct < 10);
	// And without a controller, which is how it spends most of a bench day.
	unplug();
	pump(300);
	t0 = th_cpu_ticks(m.pid);
	start = now_ms();
	pump(1000);
	pct = th_cpu_percent(t0, th_cpu_ticks(m.pid), (long)(now_ms() - start));
	printf("    joystick, no controller: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 10);
}

static void test_shutdown(void)
{
	CHECK_EQ_INT(manager_stop(mgr), 0);
	CHECK(wait_file_contains(mod_log, "hangar-joystick: stopping", 2000));
}

int main(void)
{
	T_joy = getenv("JOYSTICK_BIN");
	if (!T_joy) {
		fprintf(stderr, "JOYSTICK_BIN must be set\n");
		return 2;
	}
	RUN_TEST(test_conf);
	RUN_TEST(test_shaping);
	hd_setup("joystick", NULL);
	const char *src = getenv("HANGAR_MSG_SRC");
	const char *types[] = { "wrench", "pilot_buttons", "empty", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src && *src ? src : "msg", types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		copy_file(from, to);
	}
	RUN_TEST(test_start);
	RUN_TEST(test_axes_and_buttons);
	RUN_TEST(test_unplug_and_back);
	RUN_TEST(test_idle_cpu);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
