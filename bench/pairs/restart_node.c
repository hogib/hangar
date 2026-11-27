/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/restart_node.c: the process that bench/pairs/restart.py has killed and
 * restarted, by hangard and by the supervisors hangar is compared with (README.md,
 * "Restarting a process"). The same program under each: a publisher or a subscriber
 * of one lib/hg topic of the `depth` type, so that what differs between two rows is
 * who restarts it and nothing else.
 *
 *   restart_node --role pub|sub --topic NAME --rate HZ --log FILE
 *
 * It appends to FILE, one write a line, so that a SIGKILL loses nothing:
 *   S <role> <pid> <ns>              the first statement of main: the process runs
 *   M <reader pid> <writer pid> <ns> a message in the subscriber's hands
 * <ns> is CLOCK_MONOTONIC, the clock restart.py reads when it kills. The publisher
 * puts its pid into every message, so a subscriber's line says which instance of the
 * publisher the message came from. Under hangard it is a module (hg_ready, hg_alive);
 * elsewhere those calls find no manager and do nothing. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "depth.h"
#include "hg_topic.h"

static volatile sig_atomic_t stop;

static void on_term(int s)
{
	(void)s;
	stop = 1;
	hg_wait_interrupt();
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *arg(int argc, char **argv, const char *name, const char *dflt)
{
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], name))
			return argv[i + 1];
	return dflt;
}

int main(int argc, char **argv)
{
	uint64_t t_start = now_ns();
	const char *role = arg(argc, argv, "--role", ""), *topic = arg(argc, argv, "--topic", "restart.depth");
	const char *path = arg(argc, argv, "--log", NULL);
	long rate = atol(arg(argc, argv, "--rate", "100"));
	int pub = !strcmp(role, "pub");
	if ((!pub && strcmp(role, "sub")) || !path || rate < 1 || rate > 10000) {
		fprintf(stderr, "usage: %s --role pub|sub --topic NAME --rate HZ --log FILE\n", argv[0]);
		return 2;
	}
	int log = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (log < 0) {
		fprintf(stderr, "restart_node: %s: %s\n", path, strerror(errno));
		return 3;
	}
	char line[96];
	long pid = (long)getpid();
	int n = snprintf(line, sizeof line, "S %s %ld %llu\n", role, pid, (unsigned long long)t_start);
	if (write(log, line, (size_t)n) != n)
		return 3;
	struct sigaction sa = { .sa_handler = on_term };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	if (pub) {
		struct hg_pub_opts po = { .slots = 16, .rate_mhz = (uint32_t)rate * 1000u };
		hg_pub *p = hg_pub_open(topic, &DEPTH_TYPE, &po);
		if (!p) {
			fprintf(stderr, "restart_node: hg_pub_open(%s): %s\n", topic, strerror(errno));
			return 4;
		}
		/* A pid fits a float exactly up to 2^24; the kernel's default largest is 2^22. */
		struct depth d = { .depth_m = (float)(pid & 0xffffff), .temp_c = 0, .pressure_mbar = 0 };
		struct timespec next;
		clock_gettime(CLOCK_MONOTONIC, &next);
		long period = 1000000000L / rate;
		while (!stop) {
			d.temp_c += 1.0f;
			hg_publish(p, &d, sizeof d, now_ns());
			hg_alive();
			next.tv_nsec += period;
			while (next.tv_nsec >= 1000000000L) {
				next.tv_nsec -= 1000000000L;
				next.tv_sec++;
			}
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		}
		hg_pub_close(p);
		return 0;
	}
	hg_sub *s = hg_sub_open(topic, &DEPTH_TYPE);
	if (!s) {
		fprintf(stderr, "restart_node: hg_sub_open(%s): %s\n", topic, strerror(errno));
		return 4;
	}
	hg_ready();
	while (!stop) {
		struct depth d;
		struct hg_info info;
		hg_wait(&s, 1, 100);
		while (hg_next(s, &d, sizeof d, &info) == HG_MSG) {
			uint64_t t = now_ns();
			n = snprintf(line, sizeof line, "M %ld %ld %llu\n", pid, (long)d.depth_m, (unsigned long long)t);
			if (write(log, line, (size_t)n) != n)
				return 3;
		}
		hg_alive();
	}
	hg_sub_close(s);
	return 0;
}
