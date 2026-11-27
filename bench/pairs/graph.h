/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* bench/pairs/graph.h: one module of a whole graph, the half that is the same for every
 * system (bench/pairs/README.md, "A vehicle's graph"). Where pair.h is one publisher
 * and its subscribers, this is a process with several publications and several
 * subscriptions at once, as a module of a vehicle has: bench/pairs/graphs/ has the
 * graphs, taken from hangar's profiles, and the driver starts one such process for
 * each module of the graph, in every system alike.
 *
 *   node --module NAME --prefix P --from NS --until NS
 *        [--pub TOPIC:BYTES:HZ[:INPUT+INPUT...]]... [--sub TOPIC:BYTES[:HZ]]...
 *        [-- system's own options]
 *   node --version
 *
 * One thread, as a module is written: it waits for any of its inputs, with a timeout
 * at its next publication's instant; a message that has come is counted; a publication
 * that is due is sent. A publication may name the topics its module computes it from
 * (its manifest's Inputs) and a subscription its topic's rate: what a system that
 * records such things needs to know, and the others ignore. The waiting is the system's own (graph_system.wait), and may
 * return late by what the system's waiting call can express: a publication is then
 * late by that, and since every message carries the time it was really sent, the
 * latency is not touched by it.
 *
 * The message: BYTES bytes, a multiple of 8 and 16 at least. Word 0 is the
 * publication's count, word 1 CLOCK_MONOTONIC read immediately before the system's
 * send call. A subscription's sample is the clock at the system's hand-over minus
 * word 1, for the messages sent between --from and --until (both CLOCK_MONOTONIC, the
 * same for every process of the run: the driver sets them a warm-up ahead). The
 * process ends a second after --until and prints one line of JSON.
 *
 * Lost: a gap in a subscription's counts between two messages of the measured part.
 * Out of order: a count not above the one before. Header only, C99 and C++11; include
 * once, in the file that has main(). */
#ifndef HANGAR_BENCH_GRAPH_H
#define HANGAR_BENCH_GRAPH_H

#define PAIR_NO_MAIN
#include "pair.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRAPH_MAX 64
#define GRAPH_MIN_SIZE 16u

struct graph_end {
	char topic[96];      /* prefix and name: unique to the run */
	char name[64];       /* the graph's name for it */
	unsigned size, rate; /* rate: Hz; a subscription's may be 0, not said */
	char inputs[200];    /* a publication's Inputs, '+' between them, or empty */
	/* a publication */
	uint64_t period_ns, next_ns, seq, sent, failed, measured;
	/* a subscription */
	uint64_t received, n, last_seq, lost, disorder;
	struct pair_hist lat;
};

struct graph_opts {
	const char *module, *prefix;
	uint64_t from_ns, until_ns;
	int npub, nsub;
	struct graph_end *pub, *sub;
	int argc; /* the system's own options, after "--" */
	char **argv;
};

struct graph_system {
	const char *name;
	const char *(*version)(void);
	/* Every publication and subscription of o, opened. 0, or -1 with a message. */
	int (*open)(const struct graph_opts *o);
	/* Where publication i's next message is to be built (a slot lent), or NULL for the
	 * harness's buffer. May be NULL altogether. */
	void *(*pub_buffer)(int i);
	int (*pub_send)(int i, void *msg, size_t n);
	/* Waits up to timeout_ns for any subscription, and hands over every message that
	 * has come with graph_deliver(i, bytes, length) before it returns. 0, or -1. With
	 * no subscription it sleeps (graph_sleep). */
	int (*wait)(int64_t timeout_ns);
	void (*close)(void);
};

int graph_main(int argc, char **argv, const struct graph_system *sys);

/* ---- implementation ---------------------------------------------------------------- */

static struct graph_opts graph_o;

static inline void graph_sleep(int64_t ns)
{
	if (ns <= 0)
		return;
	struct timespec ts = { (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
	clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

/* A message of subscription i in the process's hands, at `now`: for a system whose
 * callback runs in a thread of its own, which reads the clock there (pair_now_ns) and
 * hands the message over to the thread that waits. */
static inline void graph_deliver_at(int i, const void *bytes, long len, uint64_t now)
{
	struct graph_end *e = &graph_o.sub[i];
	if (len < (long)GRAPH_MIN_SIZE)
		return;
	const uint64_t *w = (const uint64_t *)bytes;
	uint64_t seq = w[0], pub = w[1];
	e->received++;
	if (pub < graph_o.from_ns || pub > graph_o.until_ns)
		return;
	if (e->n) {
		if (seq <= e->last_seq)
			e->disorder++;
		else
			e->lost += seq - e->last_seq - 1;
	}
	if (!e->n || seq > e->last_seq)
		e->last_seq = seq;
	e->n++;
	pair_hist_add(&e->lat, now > pub ? now - pub : 0);
}

/* A message of subscription i in the process's hands: the clock is read here. */
static inline void graph_deliver(int i, const void *bytes, long len)
{
	graph_deliver_at(i, bytes, len, pair_now_ns());
}

static int graph_end_parse(struct graph_end *e, const char *prefix, const char *spec, int pub)
{
	char name[64];
	unsigned size = 0, rate = 0;
	const char *c = strchr(spec, ':');
	if (!c || (size_t)(c - spec) >= sizeof name)
		return -1;
	memcpy(name, spec, (size_t)(c - spec));
	name[c - spec] = 0;
	if (sscanf(c + 1, "%u:%u", &size, &rate) < (pub ? 2 : 1))
		return -1;
	if (size < GRAPH_MIN_SIZE || size % 8 || (pub && !rate) || rate > 100000)
		return -1;
	memset(e, 0, sizeof *e);
	const char *in = strchr(c + 1, ':');
	in = in ? strchr(in + 1, ':') : NULL;
	if (pub && in)
		snprintf(e->inputs, sizeof e->inputs, "%s", in + 1);
	snprintf(e->name, sizeof e->name, "%s", name);
	snprintf(e->topic, sizeof e->topic, "%s.%s", prefix, name);
	e->size = size;
	e->rate = rate;
	e->period_ns = pub ? 1000000000ull / rate : 0;
	return 0;
}

int graph_main(int argc, char **argv, const struct graph_system *sys)
{
	struct graph_opts *o = &graph_o;
	const char *pubs[GRAPH_MAX], *subs[GRAPH_MAX];
	int np = 0, ns = 0, failed = 0;
	unsigned biggest = GRAPH_MIN_SIZE;
	uint64_t *own = NULL;
	uint64_t end = 0, now = 0;
	o->module = "module";
	o->prefix = "g";
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--")) {
			o->argc = argc - i - 1;
			o->argv = argv + i + 1;
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
		if (i + 1 >= argc)
			goto usage;
		const char *v = argv[++i];
		if (!strcmp(a, "--module"))
			o->module = v;
		else if (!strcmp(a, "--prefix"))
			o->prefix = v;
		else if (!strcmp(a, "--from"))
			o->from_ns = strtoull(v, NULL, 10);
		else if (!strcmp(a, "--until"))
			o->until_ns = strtoull(v, NULL, 10);
		else if (!strcmp(a, "--pub") && np < GRAPH_MAX)
			pubs[np++] = v;
		else if (!strcmp(a, "--sub") && ns < GRAPH_MAX)
			subs[ns++] = v;
		else
			goto usage;
	}
	if (!o->until_ns || o->until_ns <= o->from_ns || (!np && !ns))
		goto usage;
	o->pub = (struct graph_end *)calloc((size_t)np + 1, sizeof *o->pub);
	o->sub = (struct graph_end *)calloc((size_t)ns + 1, sizeof *o->sub);
	if (!o->pub || !o->sub)
		return 3;
	for (int i = 0; i < np; i++) {
		if (graph_end_parse(&o->pub[i], o->prefix, pubs[i], 1) < 0)
			goto usage;
		if (o->pub[i].size > biggest)
			biggest = o->pub[i].size;
	}
	for (int i = 0; i < ns; i++)
		if (graph_end_parse(&o->sub[i], o->prefix, subs[i], 0) < 0)
			goto usage;
	o->npub = np;
	o->nsub = ns;
	own = (uint64_t *)calloc(biggest / 8, 8);
	if (!own)
		return 3;
	if (sys->open(o) < 0) {
		printf("{\"system\": \"%s\", \"module\": \"%s\", \"error\": \"the module could not be opened\"}\n", sys->name, o->module);
		return 3;
	}
	fprintf(stderr, "graph: ready\n");
	fflush(stderr);
	prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);

	end = o->until_ns + 1000000000ull;
	now = pair_now_ns();
	for (int i = 0; i < np; i++)
		o->pub[i].next_ns = now + o->pub[i].period_ns;
	while ((now = pair_now_ns()) < end) {
		int k = -1;
		uint64_t due = now + 100000000ull; /* look at the clock ten times a second at least */
		for (int i = 0; i < np; i++)
			if (o->pub[i].next_ns < due) {
				due = o->pub[i].next_ns;
				k = i;
			}
		if (k >= 0 && now >= due) {
			struct graph_end *e = &o->pub[k];
			e->next_ns += e->period_ns;
			if (now > o->until_ns)
				continue; /* the measured part is over: the readers drain */
			uint64_t *w = sys->pub_buffer ? (uint64_t *)sys->pub_buffer(k) : NULL;
			if (!w)
				w = own;
			w[0] = ++e->seq;
			uint64_t t0 = pair_now_ns();
			w[1] = t0;
			if (sys->pub_send(k, w, e->size) < 0)
				e->failed++;
			else
				e->sent++;
			e->measured += t0 >= o->from_ns && t0 <= o->until_ns;
			continue;
		}
		if (sys->wait((int64_t)(due - now)) < 0) {
			failed = 1;
			break;
		}
	}
	sys->close();

	printf("{\"system\": \"%s\", \"module\": \"%s\", \"failed\": %s, ", sys->name, o->module, failed ? "true" : "false");
	pair_usage_json(stdout);
	fputs(", \"pubs\": [", stdout);
	for (int i = 0; i < np; i++)
		printf("%s{\"topic\": \"%s\", \"size\": %u, \"rate\": %u, \"sent\": %" PRIu64 ", \"send_failed\": %" PRIu64
		       ", \"measured\": %" PRIu64 "}",
		       i ? ", " : "", o->pub[i].name, o->pub[i].size, o->pub[i].rate, o->pub[i].sent, o->pub[i].failed,
		       o->pub[i].measured);
	fputs("], \"subs\": [", stdout);
	for (int i = 0; i < ns; i++) {
		struct graph_end *e = &o->sub[i];
		printf("%s{\"topic\": \"%s\", \"size\": %u, \"received\": %" PRIu64 ", \"measured\": %" PRIu64 ", \"lost\": %" PRIu64
		       ", \"out_of_order\": %" PRIu64 ", ",
		       i ? ", " : "", e->name, e->size, e->received, e->n, e->lost, e->disorder);
		pair_hist_json(stdout, "latency", &e->lat);
		fputs("}", stdout);
	}
	puts("]}");
	fflush(stdout);
	return failed ? 3 : 0;
usage:
	fprintf(stderr,
		"usage: %s --module NAME --prefix P --from NS --until NS [--pub TOPIC:BYTES:HZ]... [--sub TOPIC:BYTES]...\n"
		"       [-- options of %s]\n       %s --version\n"
		"BYTES: a multiple of 8, %u at least. bench/pairs/README.md, \"A vehicle's graph\".\n",
		argv[0], sys->name, argv[0], GRAPH_MIN_SIZE);
	return 2;
}

#ifdef __cplusplus
}
#endif
#endif
