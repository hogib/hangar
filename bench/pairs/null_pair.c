/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/null_pair.c: the null system (bench/pairs/README.md, "The harness's own
 * floor"). Nothing is sent: the publisher's send keeps the pointer, the subscriber's
 * receive returns it, in the same thread (--role both). What its row shows is what
 * the harness itself costs between its two clock readings, and the schedule's own
 * lateness in the "intended" column: no system's figure can be below it. */
#include "pair.h"

static void *held;
static size_t held_n;

static int open_(const struct pair_opts *o) { (void)o; return 0; }
static void close_(void) {}
static int send_(void *msg, size_t n) { held = msg; held_n = n; return 0; }
static long recv_(const void **msg, int timeout_ms)
{
	(void)timeout_ms;
	if (!held)
		return 0;
	*msg = held;
	held = NULL;
	return (long)held_n;
}
static const char *version(void) { return "none: a pointer handed over in one thread"; }

int main(int argc, char **argv)
{
	static const struct pair_system sys = {
		.name = "null", .version = version,
		.pub_open = open_, .pub_send = send_, .pub_close = close_,
		.sub_open = open_, .sub_recv = recv_, .sub_close = close_,
	};
	return pair_main(argc, argv, &sys);
}
