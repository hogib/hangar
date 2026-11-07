// SPDX-License-Identifier: GPL-3.0-or-later
/* eventer: an event-only module: hg_ready() at start, hg_alive() every
 * --alive-every ms (100); --stop-alive-after N stops the heartbeat after N
 * calls, for the watchdog test. --exit-78 exits with EX_CONFIG at once. */
#include "toy.h"

int main(int argc, char **argv)
{
	toy_init("eventer");
	long every = toy_num(argc, argv, "--alive-every", 100);
	long stop_after = toy_num(argc, argv, "--stop-alive-after", -1);
	if (toy_flag(argc, argv, "--exit-78")) {
		hg_status("saved parameters refused");
		return 78;
	}
	hg_ready();
	hg_status("alive");
	long n = 0;
	while (!toy_stop) {
		if (stop_after < 0 || n < stop_after) {
			hg_alive();
			n++;
		}
		toy_sleep_ms(every);
	}
	return 0;
}
