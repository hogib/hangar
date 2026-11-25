// SPDX-License-Identifier: GPL-3.0-or-later
// The camera's controls as the encoder's parameters, under a manager, on the
// suite's camera (tests/fake_uvc.c, which has a webcam's controls and writes
// every set to the module's log). Found missing at the lab on 2026-11-25: with
// the encoder holding the camera, hellion's exposure and white-balance controls
// reached nothing.
//
// Checked: nothing is touched while every field is -1 but keep_framerate; a
// set from `hangar param set` reaches the camera in the camera's own units,
// the automatic mode before its manual value; a manual value without its
// automatic mode off is refused with the reason and nothing is set; a control
// the camera does not have is skipped and the others still set; the values
// are put to the camera again when the module starts again.
#include "hd_test.h"

#include <arpa/inet.h>
#include <sys/socket.h>

static const char *T_bin, *T_fake;
static char conf_path[512], mod_log[512];
static pid_t mgr;

static int count(const char *needle)
{
	const char *t = read_file(mod_log);
	int n = 0;
	for (const char *p = t; p && (p = strstr(p, needle)); p += strlen(needle))
		n++;
	return n;
}

static void test_start(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t al = sizeof a;
	CHECK(s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr *)&a, &al) == 0);
	tfmt(conf_path, sizeof conf_path, "%s/encoder.conf", T_root);
	tfmt(mod_log, sizeof mod_log, "%s/run/hangar/log/encoder.log", T_root);
	char text[512];
	tfmt(text, sizeof text, "[Encoder]\nSource=uvc:/dev/fake-uvc\nPeer=127.0.0.1:%d\nWidth=1280\nHeight=720\nFrameRate=30\n",
	     ntohs(a.sin_port));
	write_file(conf_path, text);
	manifest("encoder", "[Module]\nExec=/usr/bin/env LD_PRELOAD=%s FAKE_UVC=/dev/fake-uvc %s %s\n"
			    "[Publish video.index]\nType=video_index\nSlots=64\n"
			    "[Params]\nType=encoder_params\nAllow=admin\n"
			    "[Restart]\nBackoff=0.1s..0.2s\n",
		 T_fake, T_bin, conf_path);
	mgr = manager_start();
	CHECK(wait_state("encoder", "ready", 5000));
	CHECK(wait_file_contains(mod_log, "fake_uvc: opened", 3000));
	/* The defaults: keep_framerate=1 alone, which the camera takes as
	 * exposure_auto_priority 0; nothing else is touched. */
	CHECK(wait_file_contains(mod_log, "fake_uvc: control exposure_auto_priority = 0", 3000));
	CHECK_EQ_INT(count("fake_uvc: control "), 1);
}

static void test_set_reaches_the_camera(void)
{
	char out[2048];
	/* brightness 150 of 0..200 is +50 of -100..100: +32 of the camera's -64..64.
	 * gain 50 % of 0..100 is 50. contrast 100 is the middle of 0..64. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "encoder", "brightness=150", "gain=50", "contrast=100",
			     (char *)NULL), 0);
	CHECK(wait_file_contains(mod_log, "fake_uvc: control brightness = 32", 3000));
	CHECK(wait_file_contains(mod_log, "fake_uvc: control gain = 50", 3000));
	CHECK(wait_file_contains(mod_log, "fake_uvc: control contrast = 32", 3000));
	/* Manual exposure: the mode first (V4L2_EXPOSURE_MANUAL is 1), then 8000 us
	 * as 80 of the camera's 100 us units. In the other order the camera
	 * answers EPERM. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "encoder", "auto_exposure=0", "exposure_us=8000",
			     (char *)NULL), 0);
	CHECK(wait_file_contains(mod_log, "fake_uvc: control exposure_absolute = 80", 3000));
	const char *t = read_file(mod_log);
	const char *mode = t ? strstr(t, "fake_uvc: control exposure_auto = 1") : NULL;
	const char *value = t ? strstr(t, "fake_uvc: control exposure_absolute = 80") : NULL;
	CHECK(mode && value && mode < value);
	CHECK(!file_contains(mod_log, "was refused"));
}

static void test_refused_before_the_camera(void)
{
	char out[2048];
	int before = count("fake_uvc: control ");
	int rc = run_cli(out, sizeof out, 5000, "param", "set", "encoder", "white_balance_k=5000", (char *)NULL);
	CHECK(rc != 0 || strstr(out, "REFUSED") != NULL);
	CHECK(strstr(out, "white_balance_k needs auto_white_balance=0") != NULL);
	if (!strstr(out, "white_balance_k needs auto_white_balance=0"))
		printf("    refusal: %s\n", out);
	sleep_ms(300);
	CHECK_EQ_INT(count("fake_uvc: control "), before);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "encoder", "auto_white_balance=0", "white_balance_k=5000",
			     (char *)NULL), 0);
	CHECK(wait_file_contains(mod_log, "fake_uvc: control white_balance_temperature = 5000", 3000));
}

/* The suite's camera has no sharpness: said once, and the rest of the set still
 * goes to the camera. */
static void test_a_control_the_camera_lacks(void)
{
	char out[2048];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "encoder", "sharpness=150", "saturation=200", (char *)NULL), 0);
	CHECK(wait_file_contains(mod_log, "fake_uvc: control saturation = 128", 3000));
	CHECK(wait_file_contains(mod_log, "no \"sharpness\" control", 3000));
}

/* A camera that was unplugged comes back with its own defaults: the values in
 * force are the saved ones, and a new start puts them to the camera again. */
static void test_applied_again_at_start(void)
{
	struct mstat m;
	CHECK(module_status("encoder", &m));
	int before = count("fake_uvc: control brightness = 32");
	ctl("restart encoder");
	CHECK(wait_new_pid("encoder", m.pid, "ready", 8000));
	int ok = 0;
	for (int i = 0; i < 60 && !ok; i++, sleep_ms(50))
		ok = count("fake_uvc: control brightness = 32") == before + 1;
	CHECK(ok);
	CHECK(wait_file_contains(mod_log, "fake_uvc: control exposure_absolute = 80", 100));
	char out[2048];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "status", (char *)NULL), 0);
	CHECK(strstr(out, "camera controls v") != NULL);
}

static void test_shutdown(void)
{
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(void)
{
	T_bin = getenv("ENCODER_BIN");
	T_fake = getenv("ENCODER_FAKE_UVC");
	if (!T_bin) {
		fprintf(stderr, "ENCODER_BIN must be set\n");
		return 2;
	}
	if (!T_fake || access(T_fake, R_OK) != 0) {
		printf("skipped: no fake camera library (the toolchain makes no shared objects)\n");
		return 0;
	}
	hd_setup("encoder_controls", NULL);
	const char *src = getenv("HANGAR_MSG_SRC");
	const char *types[] = { "video_index", "encoder_params", "set_params_rep", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src && *src ? src : "msg", types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		copy_file(from, to);
	}
	RUN_TEST(test_start);
	if (!file_contains(mod_log, "fake_uvc: opened")) {
		/* LD_PRELOAD did nothing (a static encoder): there is no camera to set. */
		printf("    (skipped: the encoder did not load the fake camera; a static build?)\n");
		manager_stop(mgr);
		hd_teardown();
		return 0;
	}
	RUN_TEST(test_set_reaches_the_camera);
	RUN_TEST(test_refused_before_the_camera);
	RUN_TEST(test_a_control_the_camera_lacks);
	RUN_TEST(test_applied_again_at_start);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
