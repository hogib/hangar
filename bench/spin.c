/* bench/spin.c -- a module that never sleeps, on purpose.
 *
 * It does nothing but run: no topic, no call, no sleep. Under a manifest with
 * [Schedule] Policy=fifo it is what the manager's one-second limit is for
 * (docs/13 "Real-time scheduling without root"): the kernel ends a real-time
 * process that has run a second without sleeping (RLIMIT_RTTIME, set by the
 * manager), so one spinning module cannot keep a core from everything
 * ordinary. `rov.sh spin` runs the check; nothing else should start this.
 *
 * At an ordinary priority it only uses a core until it is stopped. */
#include <signal.h>

static volatile sig_atomic_t stop;

static void on_term(int s)
{
	(void)s;
	stop = 1;
}

int main(void)
{
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	while (!stop)
		;
	return 0;
}
