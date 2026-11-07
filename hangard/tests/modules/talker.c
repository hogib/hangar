// SPDX-License-Identifier: GPL-3.0-or-later
/* talker: publishes a depth topic at a rate. --topic T (test.depth)
 * --rate R (50) --stop-after N (keep running but stop publishing after N
 * messages) --exit-after N --exit-code C (exit after N messages) --status S.
 * SIGTERM: closes and exits 0. */
#include "toy.h"

int main(int argc, char **argv)
{
	toy_init("talker");
	const char *topic = toy_arg(argc, argv, "--topic", "test.depth");
	long rate = toy_num(argc, argv, "--rate", 50);
	long stop_after = toy_num(argc, argv, "--stop-after", -1);
	long exit_after = toy_num(argc, argv, "--exit-after", -1);
	long exit_code = toy_num(argc, argv, "--exit-code", 0);
	const char *status = toy_arg(argc, argv, "--status", "publishing");
	struct hg_pub_opts o = { .rate_mhz = (uint32_t)(rate * 1000) };
	hg_pub *p = hg_pub_open(topic, &DEPTH_TYPE, &o);
	if (!p) {
		fprintf(stderr, "talker: hg_pub_open(%s): %s\n", topic, strerror(errno));
		return 4;
	}
	hg_status(status);
	long n = 0;
	while (!toy_stop) {
		if (stop_after < 0 || n < stop_after) {
			struct depth d = { .depth_m = (float)n * 0.01f, .temp_c = 20, .pressure_mbar = 1013 };
			hg_publish(p, &d, sizeof d, hg_now_ns());
			n++;
			if (exit_after >= 0 && n >= exit_after) {
				fprintf(stderr, "talker: exiting with %ld after %ld messages\n", exit_code, n);
				hg_pub_close(p);
				return (int)exit_code;
			}
		}
		toy_sleep_ms(rate > 0 ? 1000 / rate : 100);
	}
	fprintf(stderr, "talker: SIGTERM after %ld messages\n", n);
	hg_pub_close(p);
	return 0;
}
