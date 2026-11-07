// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared by hangar's phase 1 modules (safety, mux, mavlink): the SIGTERM
 * flag that ends the loop through hg_wait_interrupt, hg_init with its error
 * on stderr, time helpers, and the state directory the latch and the
 * parameters live in (lib/hg's rule: $HANGAR_STATE_DIR, else
 * $HANGAR_ROOT/var/lib/hangar/<module>). Header only. */
#ifndef HANGAR_MOD_H
#define HANGAR_MOD_H

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hg_topic.h"

static volatile sig_atomic_t mod_stop;

static void mod_on_term(int sig)
{
	(void)sig;
	mod_stop = 1;
	hg_wait_interrupt();
}

static inline void mod_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = mod_on_term;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* hg_init, or exit 3 with the reason: the manager shows the exit. */
static inline void mod_init(const char *who)
{
	mod_signals();
	if (hg_init() < 0) {
		fprintf(stderr, "%s: hg_init: %s (%s)\n", who, hg_init_error(), strerror(errno));
		exit(3);
	}
}

/* One line on stderr, prefixed with the program's name; the manager's log
 * (journald or the fallback file) keeps it. */
static inline void mod_log(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void mod_log(const char *who, const char *fmt, ...)
{
	va_list ap;
	fprintf(stderr, "%s: ", who);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* A bounded snprintf whose truncation is intended. */
static inline int mod_fmt(char *dst, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static inline int mod_fmt(char *dst, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, len, fmt, ap);
	va_end(ap);
	return n;
}

static inline uint64_t mod_ms_to_ns(double ms)
{
	return ms <= 0 ? 0 : (uint64_t)(ms * 1e6);
}

static inline uint64_t mod_s_to_ns(double s)
{
	return s <= 0 ? 0 : (uint64_t)(s * 1e9);
}

/* The module's state directory, created if missing. */
static inline int mod_state_dir(char *out, size_t len)
{
	const char *dir = getenv("HANGAR_STATE_DIR");
	if (dir && *dir) {
		mod_fmt(out, len, "%s", dir);
	} else {
		const char *root = getenv("HANGAR_ROOT");
		mod_fmt(out, len, "%s/var/lib/hangar/%s", root ? root : "", hg_module_name());
	}
	/* mkdir -p over the tail: the root's var/lib/hangar exists under a manager. */
	char buf[1024];
	mod_fmt(buf, sizeof buf, "%s", out);
	for (char *p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0750) < 0 && errno != EEXIST && errno != EACCES)
				return -1;
			*p = '/';
		}
	if (mkdir(buf, 0750) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* Writes text to path through a temporary name and a rename. */
static inline int mod_write_file(const char *path, const char *text)
{
	char tmp[1100];
	mod_fmt(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	int rc = fputs(text, f) < 0 ? -1 : 0;
	if (fclose(f) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) < 0)
		rc = -1;
	if (rc < 0)
		unlink(tmp);
	return rc;
}

/* The newest message of s into buf, keeping the last copy when nothing is
 * newer. Returns 1 while a message is held and the topic is present and its
 * writer live; 0 when absent, closed, dead or never published. info is the
 * newest message's. */
static inline int mod_latest(hg_sub *s, void *buf, size_t len, struct hg_info *info, int *have)
{
	struct hg_info i;
	enum hg_result r = hg_latest(s, buf, len, &i);
	if (r == HG_MSG) {
		*info = i;
		*have = 1;
	} else if (r == HG_ABSENT || r == HG_ERROR) {
		*have = 0;
	} else if (r == HG_CLOSED) {
		return 0;
	}
	return *have;
}

/* Whether a message read at info is fresh now: within the topic's own
 * stale_after (three periods by default), or present when the topic has no
 * rate. */
static inline int mod_fresh(hg_sub *s, const struct hg_info *info, uint64_t now)
{
	struct hg_topic_meta m;
	if (hg_sub_meta(s, &m) < 0)
		return 0;
	if (!m.rate_mhz || !m.stale_after_ns)
		return 1;
	return now <= info->publish_ns + m.stale_after_ns;
}

static inline float mod_clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

#endif
