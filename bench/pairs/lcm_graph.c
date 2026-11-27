/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/lcm_graph.c: a module of a graph, in LCM (graph.h; README.md, "A
 * vehicle's graph"). One lcm_t a process, a channel a topic, a subscription with its
 * handler for each of the module's inputs, and lcm_handle_timeout to wait for any of
 * them with a timeout at the next publication. lcm_handle_timeout takes milliseconds:
 * a publication can be up to one late.
 *
 * Every process of the graph has joined the one multicast group, so the kernel hands
 * every datagram of every topic to every process, and LCM's reading thread in each
 * takes it in and drops what its process has not subscribed to. That is how LCM
 * works, and what it costs is in the row.
 *
 *   lcm_graph --module NAME ... [-- --url URL] [-- --recv-buf BYTES] */
#include "graph.h"

#include <lcm/lcm.h>

static const struct graph_opts *O;
static lcm_t *lcm;
static char url[256] = "udpm://239.255.76.67:7667?ttl=0";
static char channels[2 * GRAPH_MAX][112];
static int index_of[GRAPH_MAX];

static void channel_of(char *out, size_t n, const char *topic)
{
	snprintf(out, n, "G_%s", topic);
}

static void on_message(const lcm_recv_buf_t *rbuf, const char *ch, void *user)
{
	graph_deliver(*(const int *)user, rbuf->data, (long)rbuf->data_size);
	(void)ch;
}

static int open_all(const struct graph_opts *o)
{
	long recv_buf = 0;
	O = o;
	for (int i = 0; i < o->argc; i++) {
		if (!strcmp(o->argv[i], "--url") && i + 1 < o->argc) {
			snprintf(url, sizeof url, "%s", o->argv[++i]);
		} else if (!strcmp(o->argv[i], "--recv-buf") && i + 1 < o->argc) {
			recv_buf = atol(o->argv[++i]);
		} else if (!strcmp(o->argv[i], "--queue") && i + 1 < o->argc) {
			i++; /* the pair's option: here every subscription has 16 */
		} else {
			fprintf(stderr, "lcm_graph: unknown option %s\n", o->argv[i]);
			return -1;
		}
	}
	if (recv_buf > 0) {
		size_t n = strlen(url);
		snprintf(url + n, sizeof url - n, "%srecv_buf_size=%ld", strchr(url, '?') ? "&" : "?", recv_buf);
	}
	lcm = lcm_create(url);
	if (!lcm) {
		fprintf(stderr, "lcm_graph: lcm_create(%s) failed\n", url);
		return -1;
	}
	for (int i = 0; i < o->npub; i++)
		channel_of(channels[i], sizeof channels[i], o->pub[i].topic);
	for (int i = 0; i < o->nsub; i++) {
		index_of[i] = i;
		channel_of(channels[GRAPH_MAX + i], sizeof channels[0], o->sub[i].topic);
		lcm_subscription_t *s = lcm_subscribe(lcm, channels[GRAPH_MAX + i], on_message, &index_of[i]);
		if (!s) {
			fprintf(stderr, "lcm_graph: lcm_subscribe(%s) failed\n", channels[GRAPH_MAX + i]);
			return -1;
		}
		lcm_subscription_set_queue_capacity(s, 16);
	}
	return 0;
}

static int pub_send(int i, void *msg, size_t n)
{
	return lcm_publish(lcm, channels[i], msg, (unsigned)n) == 0 ? 0 : -1;
}

static int wait_any(int64_t timeout_ns)
{
	if (!O->nsub) {
		graph_sleep(timeout_ns);
		return 0;
	}
	int rc = lcm_handle_timeout(lcm, (int)((timeout_ns + 999999) / 1000000));
	while (rc > 0)
		rc = lcm_handle_timeout(lcm, 0); /* whatever else has come meanwhile */
	return rc < 0 ? -1 : 0;
}

static void close_all(void)
{
	lcm_destroy(lcm);
}

static const char *version(void)
{
	static char v[64];
	snprintf(v, sizeof v, "LCM %d.%d.%d", LCM_MAJOR_VERSION, LCM_MINOR_VERSION, LCM_MICRO_VERSION);
	return v;
}

int main(int argc, char **argv)
{
	static const struct graph_system sys = {
		.name = "lcm", .version = version, .open = open_all, .pub_send = pub_send, .wait = wait_any, .close = close_all,
	};
	return graph_main(argc, argv, &sys);
}
