/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/hangar_graph.c: a module of a graph, in hangar (graph.h; README.md, "A
 * vehicle's graph"). What a hangar module is: its publications are topics of lib/hg,
 * its subscriptions are read in order, and it sleeps in one hg_wait over all of them
 * with a timeout at its next publication.
 *
 *   hangar_graph --module NAME ... [-- --in-place]
 *
 * hg_wait's timeout is in milliseconds, so a publication can be up to a millisecond
 * late: as a real module's is. --in-place: publications built in a loaned slot,
 * subscriptions read as views (docs/01 "In place"). */
#include "graph.h"

#include "hg_topic.h"
#include "hg_type.h"

#ifndef HG_COMMIT
#define HG_COMMIT "unknown"
#endif

static const struct graph_opts *O;
static hg_pub *pubs[GRAPH_MAX];
static hg_sub *subs[GRAPH_MAX];
static struct hg_schema schemas[2 * GRAPH_MAX];
static void *bufs[GRAPH_MAX];
static int in_place;

static const struct hg_type *type_of(int slot, unsigned size)
{
	char text[64], err[128];
	snprintf(text, sizeof text, "u64 w[%u]\n", size / 8);
	if (hg_schema_parse(text, strlen(text), &schemas[slot], err, sizeof err) < 0) {
		fprintf(stderr, "hangar_graph: schema %s: %s\n", text, err);
		return NULL;
	}
	return &schemas[slot].type;
}

static int open_all(const struct graph_opts *o)
{
	O = o;
	for (int i = 0; i < o->argc; i++) {
		if (!strcmp(o->argv[i], "--in-place")) {
			in_place = 1;
		} else {
			fprintf(stderr, "hangar_graph: unknown option %s\n", o->argv[i]);
			return -1;
		}
	}
	for (int i = 0; i < o->npub; i++) {
		const struct hg_type *t = type_of(i, o->pub[i].size);
		struct hg_pub_opts po = { .slots = 16, .rate_mhz = o->pub[i].rate * 1000u };
		if (!t || !(pubs[i] = hg_pub_open(o->pub[i].topic, t, &po))) {
			fprintf(stderr, "hangar_graph: hg_pub_open %s: %s\n", o->pub[i].topic, strerror(errno));
			return -1;
		}
	}
	for (int i = 0; i < o->nsub; i++) {
		const struct hg_type *t = type_of(GRAPH_MAX + i, o->sub[i].size);
		if (!t || !(bufs[i] = malloc(o->sub[i].size)) || !(subs[i] = hg_sub_open(o->sub[i].topic, t))) {
			fprintf(stderr, "hangar_graph: hg_sub_open %s: %s\n", o->sub[i].topic, strerror(errno));
			return -1;
		}
	}
	return 0;
}

static void *pub_buffer(int i)
{
	return in_place ? hg_loan(pubs[i]) : NULL;
}

static int pub_send(int i, void *msg, size_t n)
{
	uint64_t stamp = ((const uint64_t *)msg)[1];
	return in_place ? hg_commit(pubs[i], n, stamp) : hg_publish(pubs[i], msg, n, stamp);
}

static int wait_any(int64_t timeout_ns)
{
	if (!O->nsub) {
		graph_sleep(timeout_ns);
		return 0;
	}
	int ms = (int)((timeout_ns + 999999) / 1000000);
	if (hg_wait(subs, O->nsub, ms) == (unsigned)-1)
		return -1;
	for (int i = 0; i < O->nsub; i++) {
		struct hg_info info;
		for (;;) {
			if (in_place) {
				const void *v = NULL;
				if (hg_next_view(subs[i], &v, &info) != HG_MSG)
					break;
				/* The two words the harness reads, taken first; the view then
				 * says whether they were one message's. */
				uint64_t w[2];
				memcpy(w, v, sizeof w);
				if (hg_view_done(subs[i]) == 1)
					graph_deliver(i, w, (long)O->sub[i].size);
			} else {
				if (hg_next(subs[i], bufs[i], O->sub[i].size, &info) != HG_MSG)
					break;
				graph_deliver(i, bufs[i], (long)O->sub[i].size);
			}
		}
	}
	return 0;
}

static void close_all(void)
{
	for (int i = 0; i < O->npub; i++)
		hg_pub_close(pubs[i]);
	for (int i = 0; i < O->nsub; i++)
		hg_sub_close(subs[i]);
}

static const char *version(void)
{
	return "lib/hg " HG_COMMIT;
}

int main(int argc, char **argv)
{
	static const struct graph_system sys = {
		.name = "hangar", .version = version, .open = open_all, .pub_buffer = pub_buffer, .pub_send = pub_send,
		.wait = wait_any, .close = close_all,
	};
	return graph_main(argc, argv, &sys);
}
