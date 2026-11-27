/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* bench/pairs/pair.h: the half of a benchmark node pair that is the same for every
 * system (bench/pairs/README.md, "The node pair"). A system's node is one source
 * file that fills a struct pair_system with how it opens, sends and receives, and
 * calls pair_main; everything a number depends on is here and so is identical
 * across systems: the publisher's schedule, the three stamps in the message, the
 * warm-up, the counting of lost, duplicated and out-of-order messages, the
 * histograms, and the line of JSON the driver reads.
 *
 * Header only, C99 and C++11, no dependency but libc: include it once, in the file
 * that has main().
 *
 *   node --role pub|sub|both --size BYTES --rate HZ --seconds S [--warmup S]
 *        [--id K] [--first-only] [--check-all] [--topic NAME] [-- system's own options]
 *   node --version      one line of JSON: the system's version, the compiler, the flags
 *
 * The message: three 64-bit words and a fill.
 *   seq          1, 2, 3, ...
 *   publish_ns   CLOCK_MONOTONIC read immediately before the system's send call
 *   intended_ns  the instant of the publisher's schedule this message belongs to
 * Every word of the fill holds seq. The subscriber reads the clock as soon as the
 * system has handed it the message and takes two samples from that one reading:
 * now - publish_ns (publish call to the reader's hands) and now - intended_ns (the
 * same from the time the message was due, which a publisher that fell behind cannot
 * hide). Then it checks the fill: its first and last word, or every word with
 * --check-all.
 *
 * pub publishes rate x (warmup + seconds) messages on an absolute schedule
 * (clock_nanosleep, TIMER_ABSTIME, timer slack 1 ns). sub counts and samples the
 * messages whose seq is past the warm-up, and ends at the last seq, or 5 s after the
 * run should have been over, or when the system says the publisher is gone. `both`
 * is the two in one thread, send then receive, for a system that hands over within a
 * process (the null system: what the harness itself costs).
 *
 * A system that delivers in a callback calls pair_deliver(bytes, length) as the
 * callback's first statement: the clock is read there, and the message read and
 * counted before the callback returns.
 *
 * Histograms are log-spaced, 32 bins to the octave (a bin is at most 3.1 % wide), in
 * nanoseconds, written sparse as [[index, count], ...]. Index i < 32 is the value i;
 * above, with e = 5 + (i - 32) / 32 and s = (i - 32) % 32, a bin starts at
 * (32 + s) << (e - 5). bench/pairs/driver.py has the same two lines.
 */
#ifndef HANGAR_BENCH_PAIR_H
#define HANGAR_BENCH_PAIR_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_MIN_SIZE 32u

struct pair_opts {
	const char *role;
	unsigned size, rate, seconds, warmup;
	int id;            /* which subscriber of several, from 0 */
	int first_only;    /* sub: report the first message and end (start-up, rejoin) */
	int check_all;     /* sub: check every word of the fill */
	const char *topic; /* a name for the run's channel, unique to the run */
	int argc;          /* the system's own options, after "--" */
	char **argv;
};

struct pair_system {
	const char *name;
	const char *(*version)(void); /* the system's own version, or NULL */

	int (*pub_open)(const struct pair_opts *o); /* 0, or -1 with a message on stderr */
	/* Where the next message is to be built: a slot the system lends (zero copy),
	 * or NULL for the harness's own buffer. May be NULL altogether. */
	void *(*pub_buffer)(void);
	int (*pub_send)(void *msg, size_t n); /* 0, or -1 */
	void (*pub_close)(void);

	int (*sub_open)(const struct pair_opts *o);
	/* The next message: *msg points at its bytes, good until sub_done or the next
	 * call; returns its length, 0 if none came within timeout_ms, -1 when the
	 * publisher is gone for good. */
	long (*sub_recv)(const void **msg, int timeout_ms);
	/* After the harness has read the bytes: 1 they were one whole message, 0 they
	 * were not (a view overwritten meanwhile) and the sample is dropped. May be
	 * NULL: always whole. */
	int (*sub_done)(void);
	void (*sub_close)(void);
};

int pair_main(int argc, char **argv, const struct pair_system *sys);

/* ---- implementation ---------------------------------------------------------------- */

#define PAIR_HIST_BINS (32 + 59 * 32)

struct pair_hist {
	uint64_t n, min, max;
	double sum;
	uint64_t bin[PAIR_HIST_BINS];
};

static inline unsigned pair_hist_index(uint64_t ns)
{
	if (ns < 32)
		return (unsigned)ns;
	unsigned e = 63u - (unsigned)__builtin_clzll(ns);
	return 32u + (e - 5u) * 32u + (unsigned)((ns >> (e - 5u)) & 31u);
}

static inline void pair_hist_add(struct pair_hist *h, uint64_t ns)
{
	if (!h->n || ns < h->min)
		h->min = ns;
	if (ns > h->max)
		h->max = ns;
	h->n++;
	h->sum += (double)ns;
	h->bin[pair_hist_index(ns)]++;
}

static void pair_hist_json(FILE *f, const char *key, const struct pair_hist *h)
{
	fprintf(f, "\"%s\": {\"n\": %" PRIu64 ", \"min_ns\": %" PRIu64 ", \"max_ns\": %" PRIu64 ", \"sum_ns\": %.0f, \"bins\": [",
		key, h->n, h->n ? h->min : 0, h->max, h->sum);
	int first = 1;
	for (unsigned i = 0; i < PAIR_HIST_BINS; i++)
		if (h->bin[i]) {
			fprintf(f, "%s[%u, %" PRIu64 "]", first ? "" : ", ", i, h->bin[i]);
			first = 0;
		}
	fputs("]}", f);
}

static inline uint64_t pair_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pair_usage_json(FILE *f)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	fprintf(f, "\"cpu_s\": %.6f, \"ctxsw\": [%ld, %ld]",
		(double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6,
		ru.ru_nvcsw, ru.ru_nivcsw);
}

#ifndef PAIR_CFLAGS
#define PAIR_CFLAGS "not recorded"
#endif

/* graph.h, the harness of a whole graph's module, takes the histogram, the clock and
 * the usage line from here and nothing below. */
#ifndef PAIR_NO_MAIN

static void pair_head_json(FILE *f, const struct pair_system *sys, const struct pair_opts *o)
{
	fprintf(f, "{\"system\": \"%s\", \"role\": \"%s\", \"id\": %d, \"size\": %u, \"rate\": %u, \"seconds\": %u, \"warmup\": %u, ",
		sys->name, o->role, o->id, o->size, o->rate, o->seconds, o->warmup);
}

static int pair_fail(const struct pair_system *sys, const struct pair_opts *o, const char *what)
{
	pair_head_json(stdout, sys, o);
	printf("\"error\": \"%s\"}\n", what);
	fflush(stdout);
	return 3;
}

static inline void pair_fill(uint64_t *w, size_t words, uint64_t seq, uint64_t intended)
{
	for (size_t i = 3; i < words; i++)
		w[i] = seq;
	w[0] = seq;
	w[2] = intended;
}

/* The publisher's side of one period: sleeps to the next instant of the schedule and
 * returns it. */
static inline uint64_t pair_tick(struct timespec *next, uint64_t period)
{
	next->tv_nsec += (long)(period % 1000000000ull);
	next->tv_sec += (time_t)(period / 1000000000ull);
	while (next->tv_nsec >= 1000000000L) {
		next->tv_nsec -= 1000000000L;
		next->tv_sec++;
	}
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL) == EINTR)
		;
	return (uint64_t)next->tv_sec * 1000000000ull + (uint64_t)next->tv_nsec;
}

/* What the subscriber keeps of a run. */
struct pair_sub_state {
	uint64_t total, warm, expected; /* messages of the whole run, of the warm-up, measured */
	uint64_t received, unique, dup, ooo, corrupt, discarded, max_seq, first_msg_ns, first_seq;
	unsigned char *seen; /* a bit a measured seq */
	struct pair_hist call, intended;
};

/* One message in the subscriber's hands, at `now`: its three words and its fill read
 * where the system put them, then the system asked whether those bytes were one whole
 * message, then the count. Returns 1 when it was the last of the run. */
static inline int pair_sub_take(const struct pair_system *sys, struct pair_sub_state *st, const struct pair_opts *o,
				const void *m, long len, uint64_t now)
{
	const uint64_t *w = (const uint64_t *)m;
	size_t words = (size_t)len / 8;
	uint64_t seq = w[0], pub = w[1], due = w[2];
	int bad = (unsigned long)len != o->size || (words > 3 && w[words - 1] != seq);
	if (!bad && o->check_all)
		for (size_t i = 3; i < words; i++)
			if (w[i] != seq) {
				bad = 1;
				break;
			}
	int whole = sys->sub_done ? sys->sub_done() : 1;
	if (!st->first_msg_ns) {
		st->first_msg_ns = now;
		st->first_seq = seq;
	}
	if (!whole) {
		st->discarded++;
		return 0;
	}
	if (seq > st->warm && seq <= st->total) {
		st->received++;
		st->corrupt += (uint64_t)bad;
		uint64_t k = seq - st->warm - 1;
		if (st->seen[k >> 3] & (1u << (k & 7))) {
			st->dup++;
		} else {
			st->seen[k >> 3] |= (unsigned char)(1u << (k & 7));
			st->unique++;
			if (seq < st->max_seq)
				st->ooo++;
			pair_hist_add(&st->call, now > pub ? now - pub : 0);
			pair_hist_add(&st->intended, now > due ? now - due : 0);
		}
	}
	if (seq > st->max_seq)
		st->max_seq = seq;
	return seq >= st->total;
}

/* For a system that delivers in a callback (README, "Adding a system"): the callback
 * calls pair_deliver as its first statement, and the clock is read here, at once.
 * The bytes need be good only for the length of the call, so a message the system
 * lent for the callback alone is read where it lies. The node's sub_recv, inside which
 * the system's executor ran the callback, then returns 0. */
static struct {
	const struct pair_system *sys;
	struct pair_sub_state *st;
	const struct pair_opts *o;
	unsigned n;         /* delivered during the current sub_recv */
	int last;           /* the run's last message was among them */
	uint64_t first_now; /* the reading at the very first message, or 0 */
} pair_cb;

static inline void pair_deliver(const void *m, long len)
{
	uint64_t now = pair_now_ns();
	if (!pair_cb.st || (unsigned long)len < PAIR_MIN_SIZE)
		return;
	if (!pair_cb.st->first_msg_ns)
		pair_cb.first_now = now;
	pair_cb.last |= pair_sub_take(pair_cb.sys, pair_cb.st, pair_cb.o, m, len, now);
	pair_cb.n++;
}

static void pair_sub_json(const struct pair_system *sys, const struct pair_opts *o, const struct pair_sub_state *st,
			  uint64_t t_open, int gone, int timed_out)
{
	pair_head_json(stdout, sys, o);
	printf("\"expected\": %" PRIu64 ", \"received\": %" PRIu64 ", \"unique\": %" PRIu64 ", \"lost\": %" PRIu64
	       ", \"duplicated\": %" PRIu64 ", \"out_of_order\": %" PRIu64 ", \"corrupt\": %" PRIu64 ", \"discarded\": %" PRIu64
	       ", \"open_ns\": %" PRIu64 ", \"first_msg_ns\": %" PRIu64 ", \"first_seq\": %" PRIu64
	       ", \"publisher_gone\": %s, \"timed_out\": %s, \"check_all\": %s, ",
	       st->expected, st->received, st->unique, st->expected - st->unique, st->dup, st->ooo, st->corrupt,
	       st->discarded, t_open, st->first_msg_ns, st->first_seq, gone ? "true" : "false",
	       timed_out ? "true" : "false", o->check_all ? "true" : "false");
	pair_usage_json(stdout);
	fputs(", ", stdout);
	pair_hist_json(stdout, "call", &st->call);
	fputs(", ", stdout);
	pair_hist_json(stdout, "intended", &st->intended);
	puts("}");
	fflush(stdout);
}

static int pair_sub_init(struct pair_sub_state *st, const struct pair_opts *o)
{
	memset(st, 0, sizeof *st);
	st->warm = (uint64_t)o->rate * o->warmup;
	st->expected = (uint64_t)o->rate * o->seconds;
	st->total = st->warm + st->expected;
	st->seen = (unsigned char *)calloc((size_t)(st->expected / 8 + 1), 1);
	return st->seen ? 0 : -1;
}

static int pair_run_sub(const struct pair_system *sys, const struct pair_opts *o)
{
	static struct pair_sub_state st;
	if (pair_sub_init(&st, o) < 0)
		return pair_fail(sys, o, "out of memory");
	pair_cb.sys = sys;
	pair_cb.st = &st;
	pair_cb.o = o;
	if (sys->sub_open(o) < 0)
		return pair_fail(sys, o, "the subscriber could not be opened");
	uint64_t t_open = pair_now_ns();
	/* Ready: the driver starts the publisher once every subscriber has said so. */
	fprintf(stderr, "pair: ready\n");
	fflush(stderr);
	/* The run is over when the last seq has come; if it never does, a while after
	 * it should have: counted from the first message, or from the start if none
	 * comes at all (60 s for discovery, as a ROS 2 node is given). */
	uint64_t span = (uint64_t)(o->warmup + o->seconds + 5) * 1000000000ull;
	uint64_t deadline = t_open + span + 60000000000ull;
	int gone = 0, timed_out = 0;
	for (;;) {
		const void *m = NULL;
		pair_cb.n = 0;
		long len = sys->sub_recv(&m, 100);
		uint64_t now = pair_now_ns();
		if (pair_cb.n) { /* taken in the system's callback, with its own reading */
			if (pair_cb.first_now) {
				deadline = pair_cb.first_now + span;
				pair_cb.first_now = 0;
			}
			if (pair_cb.last || (o->first_only && st.first_msg_ns))
				break;
			if (len <= 0)
				continue;
		}
		if (len < 0) {
			gone = 1;
			break;
		}
		if (len == 0) {
			if (now > deadline) {
				timed_out = 1;
				break;
			}
			continue;
		}
		if ((unsigned long)len < PAIR_MIN_SIZE)
			continue;
		int first = !st.first_msg_ns;
		int last = pair_sub_take(sys, &st, o, m, len, now);
		if (first)
			deadline = now + span;
		if (last || (o->first_only && st.first_msg_ns))
			break;
	}
	sys->sub_close();
	pair_sub_json(sys, o, &st, t_open, gone, timed_out);
	return 0;
}

static int pair_run_pub(const struct pair_system *sys, const struct pair_opts *o, int both)
{
	static struct pair_sub_state st;
	static struct pair_hist send;
	size_t words = o->size / 8;
	uint64_t *own = (uint64_t *)calloc(words, 8);
	if (!own || (both && pair_sub_init(&st, o) < 0)) {
		free(own);
		return pair_fail(sys, o, "out of memory");
	}
	if (both) {
		pair_cb.sys = sys;
		pair_cb.st = &st;
		pair_cb.o = o;
	}
	if (both && sys->sub_open(o) < 0) {
		free(own);
		return pair_fail(sys, o, "the subscriber could not be opened");
	}
	if (sys->pub_open(o) < 0) {
		free(own);
		return pair_fail(sys, o, "the publisher could not be opened");
	}
	prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0); /* the default slack is 50 us */
	uint64_t total = (uint64_t)o->rate * (o->warmup + o->seconds);
	uint64_t warm = (uint64_t)o->rate * o->warmup;
	uint64_t period = 1000000000ull / o->rate;
	uint64_t late = 0, failed = 0, t_start = pair_now_ns(), t_measured = 0;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (uint64_t seq = 1; seq <= total; seq++) {
		uint64_t due = pair_tick(&next, period);
		uint64_t *w = sys->pub_buffer ? (uint64_t *)sys->pub_buffer() : NULL;
		if (!w)
			w = own;
		pair_fill(w, words, seq, due);
		uint64_t t0 = pair_now_ns();
		w[1] = t0;
		if (sys->pub_send(w, o->size) < 0)
			failed++;
		uint64_t t1 = pair_now_ns();
		if (seq == warm + 1)
			t_measured = t0;
		if (seq > warm) {
			pair_hist_add(&send, t1 - t0);
			late += t0 > due + period;
		}
		if (both) {
			const void *m = NULL;
			long len = sys->sub_recv(&m, 0);
			uint64_t now = pair_now_ns();
			if (len >= (long)PAIR_MIN_SIZE)
				pair_sub_take(sys, &st, o, m, len, now);
		}
	}
	uint64_t t_end = pair_now_ns();
	if (both)
		sys->sub_close();
	sys->pub_close();
	free(own);
	if (both) {
		pair_sub_json(sys, o, &st, t_start, 0, 0);
		return 0;
	}
	pair_head_json(stdout, sys, o);
	printf("\"published\": %" PRIu64 ", \"send_failed\": %" PRIu64 ", \"late\": %" PRIu64 ", \"start_ns\": %" PRIu64
	       ", \"measured_from_ns\": %" PRIu64 ", \"end_ns\": %" PRIu64 ", ",
	       total, failed, late, t_start, t_measured, t_end);
	pair_usage_json(stdout);
	fputs(", ", stdout);
	pair_hist_json(stdout, "send", &send);
	puts("}");
	fflush(stdout);
	return 0;
}

int pair_main(int argc, char **argv, const struct pair_system *sys)
{
	struct pair_opts o;
	memset(&o, 0, sizeof o);
	o.role = "";
	o.topic = "pair";
	o.warmup = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--")) {
			o.argc = argc - i - 1;
			o.argv = argv + i + 1;
			break;
		}
		if (!strcmp(a, "--version")) {
			const char *v = sys->version ? sys->version() : NULL;
			printf("{\"system\": \"%s\", \"version\": \"%s\", \"compiler\": \"%s\", \"flags\": \"%s\", \"harness\": 1}\n",
			       sys->name, v ? v : "unknown",
#if defined(__clang__)
			       __VERSION__,
#elif defined(__GNUC__)
			       "gcc " __VERSION__,
#else
			       "unknown",
#endif
			       PAIR_CFLAGS);
			return 0;
		}
		if (!strcmp(a, "--first-only")) {
			o.first_only = 1;
			continue;
		}
		if (!strcmp(a, "--check-all")) {
			o.check_all = 1;
			continue;
		}
		if (i + 1 >= argc)
			goto usage;
		const char *v = argv[++i];
		if (!strcmp(a, "--role"))
			o.role = v;
		else if (!strcmp(a, "--topic"))
			o.topic = v;
		else if (!strcmp(a, "--size"))
			o.size = (unsigned)strtoul(v, NULL, 10);
		else if (!strcmp(a, "--rate"))
			o.rate = (unsigned)strtoul(v, NULL, 10);
		else if (!strcmp(a, "--seconds"))
			o.seconds = (unsigned)strtoul(v, NULL, 10);
		else if (!strcmp(a, "--warmup"))
			o.warmup = (unsigned)strtoul(v, NULL, 10);
		else if (!strcmp(a, "--id"))
			o.id = atoi(v);
		else
			goto usage;
	}
	if (o.size < PAIR_MIN_SIZE || o.size % 8 || !o.rate || o.rate > 1000000 || !o.seconds)
		goto usage;
	if (!strcmp(o.role, "pub"))
		return pair_run_pub(sys, &o, 0);
	if (!strcmp(o.role, "sub"))
		return pair_run_sub(sys, &o);
	if (!strcmp(o.role, "both"))
		return pair_run_pub(sys, &o, 1);
usage:
	fprintf(stderr,
		"usage: %s --role pub|sub|both --size BYTES --rate HZ --seconds S [--warmup S] [--id K]\n"
		"       [--first-only] [--check-all] [--topic NAME] [-- options of %s]\n"
		"       %s --version\n"
		"BYTES: a multiple of 8, %u at least. bench/pairs/README.md has the rest.\n",
		argv[0], sys->name, argv[0], PAIR_MIN_SIZE);
	return 2;
}

#endif /* PAIR_NO_MAIN */

#ifdef __cplusplus
}
#endif
#endif
