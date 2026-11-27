/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/lcm_pair.c: LCM as a node pair for bench/pairs (README.md there). LCM is
 * what a number of underwater and ground vehicles pass messages with: no broker, no
 * discovery, every message a UDP multicast datagram to a group that every process of
 * the system has joined.
 *
 *   lcm_pair --role pub|sub ... [-- --url URL] [-- --recv-buf BYTES] [-- --queue N]
 *
 * The URL is LCM's (default udpm://239.255.76.67:7667?ttl=0: LCM's own group and port,
 * and ttl 0, which keeps the datagrams on this host). --recv-buf asks for a kernel
 * receive buffer of that size, as LCM's documentation tells a user of large messages to
 * ("Kernel UDP receive buffer sizing"); the kernel grants at most net.core.rmem_max.
 *
 * The message is published as it is (lcm_publish takes bytes; the types of lcm-gen are
 * not used: they would add a marshalling step that no other row has). Above 65499
 * bytes LCM splits a message into datagrams of 65487 and the receiver puts them
 * together; one datagram lost is the message lost. A megabyte is 17.
 *
 * The subscriber is LCM as it is used: lcm_handle_timeout, which waits for a message
 * and runs the subscription's handler, whose first statement is the harness's
 * pair_deliver. Under it LCM has a thread of its own that reads the socket into a
 * buffer and wakes the handling thread through a pipe: two wake-ups a message, and they
 * are in the sample. Its queue holds --queue messages (16, as hangar's ring; LCM's
 * default is 30) and drops the newest when full.
 *
 * LCM has no notion of a publisher that has gone. The publisher says so on a channel
 * of its own when it closes, three times; that is the harness's, not LCM's. */
#include "pair.h"

#include <lcm/lcm.h>

static lcm_t *lcm;
static char channel[96], gone_channel[104], url[256] = "udpm://239.255.76.67:7667?ttl=0";
static int queue = 16, gone;
static long recv_buf;

static int setup(const struct pair_opts *o)
{
	for (int i = 0; i < o->argc; i++) {
		if (!strcmp(o->argv[i], "--url") && i + 1 < o->argc) {
			snprintf(url, sizeof url, "%s", o->argv[++i]);
		} else if (!strcmp(o->argv[i], "--recv-buf") && i + 1 < o->argc) {
			recv_buf = atol(o->argv[++i]);
		} else if (!strcmp(o->argv[i], "--queue") && i + 1 < o->argc) {
			queue = atoi(o->argv[++i]);
		} else {
			fprintf(stderr, "lcm_pair: unknown option %s\n", o->argv[i]);
			return -1;
		}
	}
	if (recv_buf > 0) {
		size_t n = strlen(url);
		snprintf(url + n, sizeof url - n, "%srecv_buf_size=%ld", strchr(url, '?') ? "&" : "?", recv_buf);
	}
	snprintf(channel, sizeof channel, "PAIR_%s", o->topic);
	snprintf(gone_channel, sizeof gone_channel, "PAIR_%s_GONE", o->topic);
	lcm = lcm_create(url);
	if (!lcm) {
		fprintf(stderr, "lcm_pair: lcm_create(%s) failed\n", url);
		return -1;
	}
	return 0;
}

static int pub_open(const struct pair_opts *o)
{
	return setup(o);
}

static int pub_send(void *msg, size_t n)
{
	return lcm_publish(lcm, channel, msg, (unsigned)n) == 0 ? 0 : -1;
}

static void pub_close(void)
{
	static const char bye[8] = "gone";
	for (int i = 0; i < 3; i++) {
		lcm_publish(lcm, gone_channel, bye, sizeof bye);
		usleep(20000);
	}
	lcm_destroy(lcm);
}

static void on_message(const lcm_recv_buf_t *rbuf, const char *ch, void *user)
{
	pair_deliver(rbuf->data, (long)rbuf->data_size);
	(void)ch;
	(void)user;
}

static void on_gone(const lcm_recv_buf_t *rbuf, const char *ch, void *user)
{
	(void)rbuf;
	(void)ch;
	(void)user;
	gone = 1;
}

static int sub_open(const struct pair_opts *o)
{
	if (setup(o) < 0)
		return -1;
	lcm_subscription_t *s = lcm_subscribe(lcm, channel, on_message, NULL);
	if (!s || !lcm_subscribe(lcm, gone_channel, on_gone, NULL)) {
		fprintf(stderr, "lcm_pair: lcm_subscribe failed\n");
		return -1;
	}
	lcm_subscription_set_queue_capacity(s, queue);
	return 0;
}

static long sub_recv(const void **msg, int timeout_ms)
{
	(void)msg;
	int rc = lcm_handle_timeout(lcm, timeout_ms > 0 ? timeout_ms : 0);
	if (rc < 0)
		return -1;
	return gone && rc == 0 ? -1 : 0; /* gone, and nothing left to handle */
}

static void sub_close(void)
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
	static const struct pair_system sys = {
		.name = "lcm", .version = version,
		.pub_open = pub_open, .pub_send = pub_send, .pub_close = pub_close,
		.sub_open = sub_open, .sub_recv = sub_recv, .sub_close = sub_close,
	};
	return pair_main(argc, argv, &sys);
}
