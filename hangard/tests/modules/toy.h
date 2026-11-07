// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared by the toy modules of hangard's suites: argument parsing, a
 * SIGTERM flag, sleeping, the depth type. */
#ifndef TOY_H
#define TOY_H

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "depth.h"
#include "hg_topic.h"

static volatile sig_atomic_t toy_stop;

static void toy_on_term(int sig)
{
	(void)sig;
	toy_stop = 1;
	hg_wait_interrupt();
}

static inline void toy_signals(void)
{
	struct sigaction sa = { .sa_handler = toy_on_term };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
}

/* "--name value" lookups over argv. */
static inline const char *toy_arg(int argc, char **argv, const char *name, const char *dflt)
{
	for (int i = 1; i + 1 < argc; i++)
		if (strcmp(argv[i], name) == 0)
			return argv[i + 1];
	return dflt;
}

static inline int toy_flag(int argc, char **argv, const char *name)
{
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], name) == 0)
			return 1;
	return 0;
}

static inline long toy_num(int argc, char **argv, const char *name, long dflt)
{
	const char *v = toy_arg(argc, argv, name, NULL);
	return v ? atol(v) : dflt;
}

static inline void toy_sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !toy_stop)
		;
}

static inline void toy_init(const char *who)
{
	toy_signals();
	if (hg_init() < 0) {
		fprintf(stderr, "%s: hg_init: %s (%s)\n", who, hg_init_error(), strerror(errno));
		exit(3);
	}
}

#endif
