// SPDX-License-Identifier: LGPL-3.0-or-later
/* The actuator gate of docs/07-safety.md: see hg_gate.h. */
#define _GNU_SOURCE
#include "hg_gate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATE_DEFAULT_COMMAND_NS 200000000ull
#define GATE_DEFAULT_SAFETY_NS 300000000ull
#define SAFETY_BYTES 128

struct hg_gate {
	hg_sub *cmd, *safety;
	uint64_t cmd_timeout_ns, safety_timeout_ns;
	unsigned char cmdbuf[HG_GATE_COMMAND_MAX];
	struct hg_info cmd_info;
	int have_cmd;
	unsigned char sbuf[SAFETY_BYTES];
	struct hg_info s_info;
	int have_s;
	struct hg_gate_safety judged;
	char why[160];
};

static hg_gate *gate_new(const char *command, uint64_t ct, uint64_t st)
{
	hg_gate *g = calloc(1, sizeof *g);
	if (!g)
		return NULL;
	g->cmd_timeout_ns = ct ? ct : GATE_DEFAULT_COMMAND_NS;
	g->safety_timeout_ns = st ? st : GATE_DEFAULT_SAFETY_NS;
	g->cmd = hg_sub_open(command, NULL);
	if (g->cmd)
		g->safety = hg_sub_open("safety.state", NULL);
	if (!g->cmd || !g->safety) {
		int e = errno;
		hg_gate_close(g);
		errno = e;
		return NULL;
	}
	snprintf(g->why, sizeof g->why, "not checked yet");
	return g;
}

hg_gate *hg_gate_open(void)
{
	if (hg_init() < 0)
		return NULL;
	const struct hg_wiring *w = hg_wiring();
	if (!w || !w->actuator.present) {
		errno = EACCES;
		return NULL;
	}
	return gate_new(w->actuator.command, w->actuator.command_timeout_ns, w->actuator.safety_timeout_ns);
}

hg_gate *hg_gate_open_with(const char *command, uint64_t command_timeout_ns, uint64_t safety_timeout_ns)
{
	if (!command || hg_init() < 0) {
		errno = EINVAL;
		return NULL;
	}
	return gate_new(command, command_timeout_ns, safety_timeout_ns);
}

void hg_gate_close(hg_gate *g)
{
	if (!g)
		return;
	hg_sub_close(g->cmd);
	hg_sub_close(g->safety);
	free(g);
}

/* Reads the newest message of s into buf, keeping the previous one when
 * nothing newer is there. Returns the result of the read for a fresh
 * message, HG_NONE when the old copy stands, or the condition reported. */
static enum hg_result newest(hg_sub *s, void *buf, size_t len, struct hg_info *info, int *have)
{
	struct hg_info i;
	enum hg_result r = hg_latest(s, buf, len, &i);
	if (r == HG_MSG) {
		*info = i;
		*have = 1;
	} else if (r == HG_ABSENT || r == HG_CLOSED || r == HG_ERROR) {
		/* A closed or dead writer's last message is no command; an absent
		 * topic has none. HG_NONE and HG_BUSY leave the last copy. */
		if (r == HG_ABSENT)
			*have = 0;
	}
	return r;
}

static const char *state_word(uint8_t st)
{
	switch (st) {
	case 0: return "disarmed";
	case 1: return "armed";
	case 2: return "failsafe";
	case 3: return "arming";
	default: return "unknown state";
	}
}

const void *hg_gate_check(hg_gate *g, const char **why)
{
	static const char *const nowhy = "";
	if (why)
		*why = nowhy;
	if (!g) {
		errno = EINVAL;
		return NULL;
	}
	uint64_t now = hg_now_ns();

	/* 1. safety.state: fresh and outputs_enabled. */
	enum hg_result rs = newest(g->safety, g->sbuf, sizeof g->sbuf, &g->s_info, &g->have_s);
	struct hg_gate_safety *j = &g->judged;
	memset(j, 0, sizeof *j);
	if (g->have_s) {
		j->present = 1;
		j->state = g->sbuf[0];
		j->outputs_enabled = g->sbuf[1];
		j->action = g->sbuf[2];
		memcpy(&j->triggers, g->sbuf + 4, 4);
		j->seq = g->s_info.seq;
		j->age_ns = now > g->s_info.publish_ns ? now - g->s_info.publish_ns : 0;
		j->fresh = j->age_ns <= g->safety_timeout_ns && rs != HG_CLOSED && rs != HG_ERROR;
	}
	if (!j->present) {
		snprintf(g->why, sizeof g->why, rs == HG_ERROR ? "safety state unreadable" : "safety state absent");
		goto closed;
	}
	if (rs == HG_CLOSED) {
		snprintf(g->why, sizeof g->why, "safety state closed: its writer is gone");
		goto closed;
	}
	if (!j->fresh) {
		snprintf(g->why, sizeof g->why, "safety state stale %llu ms",
			 (unsigned long long)(j->age_ns / 1000000u));
		goto closed;
	}
	if (!j->outputs_enabled) {
		snprintf(g->why, sizeof g->why, "%s", state_word(j->state));
		goto closed;
	}

	/* 2. The command: present, within CommandTimeout, not flagged. */
	enum hg_result rc = newest(g->cmd, g->cmdbuf, sizeof g->cmdbuf, &g->cmd_info, &g->have_cmd);
	if (rc == HG_ERROR) {
		snprintf(g->why, sizeof g->why, "command unreadable: %s",
			 errno == EMSGSIZE ? "payload over HG_GATE_COMMAND_MAX" : strerror(errno));
		goto closed;
	}
	if (!g->have_cmd) {
		snprintf(g->why, sizeof g->why, "command absent");
		goto closed;
	}
	if (rc == HG_CLOSED) {
		snprintf(g->why, sizeof g->why, "command closed: its writer is gone");
		goto closed;
	}
	uint64_t age = now > g->cmd_info.publish_ns ? now - g->cmd_info.publish_ns : 0;
	if (age > g->cmd_timeout_ns) {
		snprintf(g->why, sizeof g->why, "command stale %llu ms", (unsigned long long)(age / 1000000u));
		goto closed;
	}
	if (g->cmd_info.flags & HG_SLOT_STALE) {
		/* The reference that carries the stale or absent flag names the
		 * input by topic id; the name is the publisher's wiring's, not
		 * ours (docs/07 shows "computed from stale nav.depth"). */
		uint32_t id = 0;
		for (uint32_t i = 0; i < g->cmd_info.n_inputs && i < HG_MAX_REFS; i++)
			if (g->cmd_info.inputs[i].flags & (HG_REF_STALE | HG_REF_ABSENT)) {
				id = g->cmd_info.inputs[i].topic_id;
				break;
			}
		if (id)
			snprintf(g->why, sizeof g->why, "command computed from stale inputs (topic %08x)", id);
		else
			snprintf(g->why, sizeof g->why, "command computed from stale inputs");
		goto closed;
	}
	snprintf(g->why, sizeof g->why, "open");
	if (why)
		*why = g->why;
	return g->cmdbuf;
closed:
	if (why)
		*why = g->why;
	return NULL;
}

int hg_gate_subs(const hg_gate *g, hg_sub **out, int max)
{
	if (!g || max < 2) {
		errno = E2BIG;
		return -1;
	}
	out[0] = g->cmd;
	out[1] = g->safety;
	return 2;
}

hg_sub *hg_gate_command_sub(const hg_gate *g)
{
	return g ? g->cmd : NULL;
}

hg_sub *hg_gate_safety_sub(const hg_gate *g)
{
	return g ? g->safety : NULL;
}

const struct hg_info *hg_gate_command_info(const hg_gate *g)
{
	return &g->cmd_info;
}

uint32_t hg_gate_command_bytes(const hg_gate *g)
{
	return g ? hg_sub_payload_bytes(g->cmd) : 0;
}

uint64_t hg_gate_command_timeout_ns(const hg_gate *g)
{
	return g ? g->cmd_timeout_ns : 0;
}

uint64_t hg_gate_safety_timeout_ns(const hg_gate *g)
{
	return g ? g->safety_timeout_ns : 0;
}

void hg_gate_safety(const hg_gate *g, struct hg_gate_safety *out)
{
	if (!g || !out)
		return;
	*out = g->judged;
}
