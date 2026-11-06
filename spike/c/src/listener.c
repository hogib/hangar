/* listener <topic> <seconds>: waits, reads in order, checks payloads, and
 * prints one JSON line of counts and wake-up latencies. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hg_topic.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
	hg_wait_interrupt(); /* no window between the check of stop and the sleep */
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

static double pct_us(const uint64_t *v, size_t n, double q)
{
	if (n == 0)
		return 0;
	size_t i = (size_t)(q * (double)(n - 1) + 0.5);
	return (double)v[i] / 1000.0;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: listener <topic> <seconds>\n");
		return 2;
	}
	char *end;
	double seconds = strtod(argv[2], &end);
	if (end == argv[2] || *end || !(seconds > 0 && seconds <= 1e7)) {
		fprintf(stderr, "listener: seconds must be in (0, 1e7]\n");
		return 2;
	}
	hg_sub *s = hg_sub_open(argv[1]);
	if (!s) {
		fprintf(stderr, "listener: cannot open topic %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	size_t cap = 4096, nlat = 0, latcap = 1 << 16;
	unsigned char *buf = malloc(cap);
	uint64_t *lat = malloc(latcap * sizeof *lat);
	if (!buf || !lat)
		return 1;
	uint64_t msgs = 0, gaps = 0, torn = 0;
	uint64_t start = hg_now_ns();
	uint64_t deadline = start + (uint64_t)(seconds * 1e9);

	while (!stop) {
		uint64_t now = hg_now_ns();
		if (now >= deadline)
			break;
		unsigned char ready;
		uint64_t ms = (deadline - now + 999999) / 1000000;
		int r = hg_wait(&s, 1, ms > INT_MAX ? INT_MAX : (int)ms, &ready);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			fprintf(stderr, "listener: wait: %s\n", strerror(errno));
			return 1;
		}
		uint64_t woke = hg_now_ns();
		for (;;) {
			struct hg_info info;
			enum hg_result res = hg_next(s, buf, cap, &info);
			gaps += info.gap;
			if (res == HG_ERROR && hg_sub_payload_bytes(s) > cap) {
				cap = hg_sub_payload_bytes(s);
				free(buf);
				if (!(buf = malloc(cap)))
					return 1;
				continue;
			}
			if (res == HG_ERROR) {
				fprintf(stderr, "listener: read %s: %s\n", argv[1], strerror(errno));
				return 1;
			}
			if (res != HG_MSG)
				break;
			msgs++;
			if (info.publish_ns > woke)
				woke = hg_now_ns();
			unsigned char want = (unsigned char)(info.seq & 0xff);
			uint32_t len = hg_sub_payload_bytes(s);
			for (uint32_t i = 0; i < len; i++) {
				if (buf[i] != want) {
					torn++;
					break;
				}
			}
			/* Backlog found in the ring at attach was not a wake-up. */
			if (info.publish_ns < start)
				continue;
			if (nlat == latcap) {
				uint64_t *n = realloc(lat, 2 * latcap * sizeof *lat);
				if (!n)
					return 1;
				lat = n;
				latcap *= 2;
			}
			lat[nlat++] = woke - info.publish_ns;
		}
	}

	qsort(lat, nlat, sizeof *lat, cmp_u64);
	printf("{\"msgs\":%llu,\"gaps\":%llu,\"torn\":%llu,\"p50_us\":%.1f,\"p99_us\":%.1f,"
	       "\"max_us\":%.1f,\"reopens\":%llu}\n",
	       (unsigned long long)msgs, (unsigned long long)gaps, (unsigned long long)torn,
	       pct_us(lat, nlat, 0.5), pct_us(lat, nlat, 0.99),
	       nlat ? (double)lat[nlat - 1] / 1000.0 : 0.0,
	       (unsigned long long)hg_sub_reopens(s));
	hg_sub_close(s);
	free(buf);
	free(lat);
	return 0;
}
