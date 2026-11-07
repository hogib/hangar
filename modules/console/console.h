// SPDX-License-Identifier: GPL-3.0-or-later
/* Dawn Patrol (module `console`, binary dawn-patrold): the operator's page.
 * Shared between dawn_patrold.c (the process: arguments, the HTTP loop,
 * routing) and console_api.c (what the manager's topics and the topic
 * directory say, as JSON). docs/names.md, lib/hg/PLAN.md "Stage 3
 * addition", docs/12-tools.md (`hangar top`, `topics`, `echo`). */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#include "hangar_modules.h"
#include "hangar_system.h"
#include "hg_topic.h"

#define CON_PATH 512
#define CON_MAX_MODULES 128   /* HD_MAX_MODULES */
#define CON_MAX_TOPICS 1024

/* A growable text buffer: every response is built in one. */
struct cbuf {
	char *p;
	size_t len, cap;
};
int cbuf_printf(struct cbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* s as a JSON string literal, quotes included; at most maxlen bytes of it. */
void cbuf_json_str(struct cbuf *b, const char *s, size_t maxlen);
void cbuf_free(struct cbuf *b);

/* One module as the manager last described it, plus what the console
 * counted itself: the pid changes it saw (the manager clears `failures` on
 * a restart by hand, so they are not the restarts an operator counts). */
struct modrec {
	struct hangar_modules m;
	uint64_t seen_ns;
	int32_t last_pid;
	uint32_t restarts;
};

/* write_seq of a topic at the last tick, for the measured rate. */
struct raterec {
	char name[HG_NAME_MAX + 1];
	uint64_t seq, t_ns;
	double hz;
	int seen;
};

struct console {
	char role[16];
	char topic_dir[CON_PATH];
	char control_path[CON_PATH];
	char log_dir[CON_PATH];
	char journal_path[CON_PATH];
	hg_sub *sub_system, *sub_modules;
	struct hangar_system sys;
	struct hg_info sys_info;
	int have_sys;
	struct modrec mods[CON_MAX_MODULES];
	int n_mods;
	struct raterec rates[CON_MAX_TOPICS];
	int n_rates;
	uint64_t last_tick_ns;
};

/* Reads whatever the two manager topics hold (called when hg_wait_fd's
 * descriptor fires, and on each tick). */
void api_drain(struct console *c);
/* The 1 s tick: drain, measure every topic's rate from its write_seq, drop
 * modules the manager stopped describing. */
void api_tick(struct console *c);

/* The JSON bodies. Each returns the HTTP status to send. */
int api_system_json(struct console *c, struct cbuf *b);
int api_modules_json(struct console *c, struct cbuf *b);
int api_topics_json(struct console *c, struct cbuf *b);
int api_topic_json(struct console *c, const char *name, struct cbuf *b);
int api_log_json(struct console *c, int lines, struct cbuf *b);

#endif
