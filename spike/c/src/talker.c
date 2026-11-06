/* talker <topic> <rate_hz> [--payload 256] [--slots 16] [--crash-after N] */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

#include "hg_topic.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int usage(void)
{
	fprintf(stderr, "usage: talker <topic> <rate_hz> [--payload 256] [--slots 16] "
			"[--crash-after N]\n");
	return 2;
}

static int parse_u64(const char *s, unsigned long long *out)
{
	char *end;
	errno = 0;
	*out = strtoull(s, &end, 10);
	return errno || end == s || *end ? -1 : 0;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return usage();
	const char *topic = argv[1];
	char *end;
	double rate = strtod(argv[2], &end);
	if (end == argv[2] || *end || !(rate >= 1e-3 && rate <= 1e9))
		return usage();
	unsigned long long payload = 256, slots = 16, crash_after = 0;
	for (int i = 3; i < argc; i++) {
		unsigned long long *dst = NULL;
		if (!strcmp(argv[i], "--payload"))
			dst = &payload;
		else if (!strcmp(argv[i], "--slots"))
			dst = &slots;
		else if (!strcmp(argv[i], "--crash-after"))
			dst = &crash_after;
		if (!dst || i + 1 >= argc || parse_u64(argv[++i], dst) < 0)
			return usage();
	}
	if (payload == 0 || payload > (1u << 24) || slots > UINT32_MAX)
		return usage();

	/* As hg_init would: no core dumps (and no ptrace by same-user modules). */
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	unsigned char *buf = malloc(payload);
	hg_pub *p = buf ? hg_pub_open(topic, (uint32_t)payload, (uint32_t)slots) : NULL;
	if (!p) {
		fprintf(stderr, "talker: cannot open topic %s: %s\n", topic, strerror(errno));
		return 1;
	}

	uint64_t period = (uint64_t)(1e9 / rate);
	if (period == 0)
		period = 1;
	uint64_t next = hg_now_ns();
	unsigned long long sent = 0;
	while (!stop) {
		memset(buf, (int)(hg_pub_next_seq(p) & 0xff), payload);
		hg_publish(p, buf, payload, hg_now_ns());
		if (crash_after && ++sent >= crash_after)
			abort();
		next += period;
		struct timespec ts = { .tv_sec = (time_t)(next / 1000000000u),
				       .tv_nsec = (long)(next % 1000000000u) };
		while (!stop && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
	}
	hg_pub_close(p);
	free(buf);
	return 0;
}
