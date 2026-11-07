// SPDX-License-Identifier: GPL-3.0-or-later
/* listener: subscribes to --topic (test.depth), calls hg_ready() once it
 * runs (it has no rated output) and hg_alive() on every pass unless
 * --no-alive; --no-ready never calls hg_ready(). Counts messages into its
 * status line. */
#include "toy.h"

int main(int argc, char **argv)
{
	toy_init("listener");
	const char *topic = toy_arg(argc, argv, "--topic", "test.depth");
	int no_alive = toy_flag(argc, argv, "--no-alive");
	int no_ready = toy_flag(argc, argv, "--no-ready");
	hg_sub *s = hg_sub_open(topic, &DEPTH_TYPE);
	if (!s) {
		fprintf(stderr, "listener: hg_sub_open(%s): %s\n", topic, strerror(errno));
		return 4;
	}
	if (!no_ready)
		hg_ready();
	long n = 0;
	struct depth d;
	struct hg_info info;
	while (!toy_stop) {
		hg_wait(&s, 1, 100);
		while (hg_next(s, &d, sizeof d, &info) == HG_MSG)
			n++;
		if (!no_alive)
			hg_alive();
		char line[64];
		snprintf(line, sizeof line, "%ld messages", n);
		hg_status(line);
	}
	fprintf(stderr, "listener: %ld messages\n", n);
	return 0;
}
