// SPDX-License-Identifier: GPL-3.0-or-later
/* crasher: publishes --topic (test.crash) at --rate (50) and dies after
 * --crash-after N messages (default 5): with --exit-code C it exits C,
 * otherwise it aborts. --crash-after -1 never dies. */
#include "toy.h"

int main(int argc, char **argv)
{
	toy_init("crasher");
	const char *topic = toy_arg(argc, argv, "--topic", "test.crash");
	long rate = toy_num(argc, argv, "--rate", 50);
	long after = toy_num(argc, argv, "--crash-after", 5);
	long code = toy_num(argc, argv, "--exit-code", -1);
	struct hg_pub_opts o = { .rate_mhz = (uint32_t)(rate * 1000) };
	hg_pub *p = hg_pub_open(topic, &DEPTH_TYPE, &o);
	if (!p) {
		fprintf(stderr, "crasher: hg_pub_open(%s): %s\n", topic, strerror(errno));
		return 4;
	}
	hg_status("about to crash");
	long n = 0;
	while (!toy_stop) {
		struct depth d = { .depth_m = (float)n };
		hg_publish(p, &d, sizeof d, hg_now_ns());
		n++;
		if (after >= 0 && n >= after) {
			if (code >= 0)
				_exit((int)code);
			abort();
		}
		toy_sleep_ms(rate > 0 ? 1000 / rate : 100);
	}
	return 0;
}
