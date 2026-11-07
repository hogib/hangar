// SPDX-License-Identifier: GPL-3.0-or-later
/* actuator: subscribes to ctl.cmd and safety.state, publishes thr.cmd
 * (Actuator=yes in its manifest) at --rate (50); on SIGTERM it publishes
 * once more and exits 0. --exit-after N exits with --exit-code after N
 * messages, for the Burst/failsafe test. */
#include "toy.h"

int main(int argc, char **argv)
{
	toy_init("actuator");
	long rate = toy_num(argc, argv, "--rate", 50);
	long exit_after = toy_num(argc, argv, "--exit-after", -1);
	long exit_code = toy_num(argc, argv, "--exit-code", 1);
	hg_sub *cmd = hg_sub_open("ctl.cmd", NULL);
	hg_sub *safety = hg_sub_open("safety.state", NULL);
	if (!cmd || !safety) {
		fprintf(stderr, "actuator: hg_sub_open: %s\n", strerror(errno));
		return 4;
	}
	struct hg_pub_opts o = { .rate_mhz = (uint32_t)(rate * 1000) };
	hg_pub *p = hg_pub_open("thr.cmd", &DEPTH_TYPE, &o);
	if (!p) {
		fprintf(stderr, "actuator: hg_pub_open(thr.cmd): %s\n", strerror(errno));
		return 4;
	}
	hg_status("gate closed: no command");
	long n = 0;
	while (!toy_stop) {
		struct depth d = { .depth_m = 0 };
		hg_publish_with(p, &d, sizeof d, hg_now_ns(), HG_NO_REFS, NULL, 0);
		n++;
		if (exit_after >= 0 && n >= exit_after)
			_exit((int)exit_code);
		toy_sleep_ms(rate > 0 ? 1000 / rate : 100);
	}
	struct depth neutral = { 0 };
	hg_publish_with(p, &neutral, sizeof neutral, hg_now_ns(), HG_NO_REFS, NULL, 0);
	hg_pub_close(p);
	return 0;
}
