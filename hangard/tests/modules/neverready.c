// SPDX-License-Identifier: GPL-3.0-or-later
/* neverready: has no rated output and never calls hg_ready(): the manager
 * finds it not ready after StartGrace. */
#include "toy.h"

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	toy_init("neverready");
	hg_status("waiting for nothing");
	while (!toy_stop)
		toy_sleep_ms(50);
	return 0;
}
