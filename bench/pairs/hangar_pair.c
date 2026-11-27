/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/hangar_pair.c: hangar's node pair for bench/pairs (README.md there).
 * A topic of lib/hg with a 16-slot ring, the publisher in one process and each
 * subscriber in its own: what two hangar modules are to each other.
 *
 *   hangar_pair --role pub|sub ... [-- --in-place] [-- --slots N]
 *
 * As is: hg_publish copies the message into the ring, hg_next copies it out into the
 * subscriber's buffer, and the harness reads it there. --in-place (docs/01 "In
 * place"): the publisher fills the slot it was loaned and commits it, the subscriber
 * is handed a view of the slot, and hg_view_done says, after the harness has read
 * it, whether the slot held that message all the while.
 *
 * The subscriber sleeps in hg_wait (one futex) and is woken by the publish. The
 * topic's directory is $HANGAR_TOPIC_DIR, which the driver makes anew for each run. */
#include "pair.h"

#include "hg_topic.h"
#include "hg_type.h"

#ifndef HG_COMMIT
#define HG_COMMIT "unknown"
#endif

static struct hg_schema schema;
static hg_pub *pub;
static hg_sub *sub;
static void *buf;
static unsigned size, slots = 16;
static int in_place;
static char topic[96];

static int setup(const struct pair_opts *o)
{
	for (int i = 0; i < o->argc; i++) {
		if (!strcmp(o->argv[i], "--in-place")) {
			in_place = 1;
		} else if (!strcmp(o->argv[i], "--slots") && i + 1 < o->argc) {
			slots = (unsigned)strtoul(o->argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "hangar_pair: unknown option %s\n", o->argv[i]);
			return -1;
		}
	}
	char text[64], err[128];
	size = o->size;
	snprintf(text, sizeof text, "u64 w[%u]\n", size / 8);
	if (hg_schema_parse(text, strlen(text), &schema, err, sizeof err) < 0 || schema.type.payload_bytes != size) {
		fprintf(stderr, "hangar_pair: schema %s: %s\n", text, err);
		return -1;
	}
	snprintf(topic, sizeof topic, "pair.%s", o->topic);
	return 0;
}

static int pub_open(const struct pair_opts *o)
{
	if (setup(o) < 0)
		return -1;
	struct hg_pub_opts po = { .slots = slots, .rate_mhz = o->rate * 1000u };
	pub = hg_pub_open(topic, &schema.type, &po);
	if (!pub)
		fprintf(stderr, "hangar_pair: hg_pub_open %s: %s\n", topic, strerror(errno));
	return pub ? 0 : -1;
}

static void *pub_buffer(void)
{
	return in_place ? hg_loan(pub) : NULL;
}

static int pub_send(void *msg, size_t n)
{
	/* The stamp is the harness's own reading, taken just before this call. */
	uint64_t stamp = ((const uint64_t *)msg)[1];
	return in_place ? hg_commit(pub, n, stamp) : hg_publish(pub, msg, n, stamp);
}

static void pub_close(void)
{
	hg_pub_close(pub); /* wakes the readers: HG_CLOSED */
}

static int sub_open(const struct pair_opts *o)
{
	if (setup(o) < 0 || !(buf = malloc(size)))
		return -1;
	sub = hg_sub_open(topic, &schema.type); /* an absent topic attaches later */
	if (!sub)
		fprintf(stderr, "hangar_pair: hg_sub_open %s: %s\n", topic, strerror(errno));
	return sub ? 0 : -1;
}

static long sub_recv(const void **msg, int timeout_ms)
{
	struct hg_info info;
	for (int waited = 0;; waited = 1) {
		const void *v = buf;
		enum hg_result r = in_place ? hg_next_view(sub, &v, &info) : hg_next(sub, buf, size, &info);
		if (r == HG_MSG) {
			*msg = v;
			return (long)size;
		}
		if (r == HG_CLOSED)
			return -1;
		if (waited || timeout_ms <= 0)
			return 0;
		if (hg_wait(&sub, 1, timeout_ms) == (unsigned)-1)
			return -1;
	}
}

static int sub_done(void)
{
	return in_place ? hg_view_done(sub) == 1 : 1;
}

static void sub_close(void)
{
	hg_sub_close(sub);
}

static const char *version(void)
{
	return "lib/hg " HG_COMMIT;
}

int main(int argc, char **argv)
{
	static const struct pair_system sys = {
		.name = "hangar", .version = version,
		.pub_open = pub_open, .pub_buffer = pub_buffer, .pub_send = pub_send, .pub_close = pub_close,
		.sub_open = sub_open, .sub_recv = sub_recv, .sub_done = sub_done, .sub_close = sub_close,
	};
	return pair_main(argc, argv, &sys);
}
