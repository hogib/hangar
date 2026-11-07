// SPDX-License-Identifier: GPL-3.0-or-later
/* recorder.conf (docs/06-recorder.md "What it records", "Sessions"): the
 * manifest syntax, parsed as strictly. [Recorder] once, [Record <glob>]
 * any number of times, matched in order. */
#include "recorder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hg_ini.h"
#include "hg_wiring.h"

struct cstate {
	struct rec_conf *c;
	int section;              /* 0 none, 1 Recorder, 2 Record */
	int recorder_seen;
	uint32_t seen;
};

enum { K_AUTOSTART, K_DIR, K_MINFREE, K_BLACKBOX, K_BLACKBOXMEM, K_WRITEBUFFER, K_POLICY, K_CLASS };

static int once(struct cstate *p, int k, const char *key, char *why, size_t whylen)
{
	if (p->seen & (1u << k)) {
		snprintf(why, whylen, "%s given twice", key);
		return -1;
	}
	p->seen |= 1u << k;
	return 0;
}

static int on_section(void *ctx, const char *section, const char *arg, int line, char *why, size_t whylen)
{
	struct cstate *p = ctx;
	p->seen = 0;
	if (strcmp(section, "Recorder") == 0) {
		if (arg[0]) {
			snprintf(why, whylen, "[Recorder] takes no argument");
			return -1;
		}
		if (p->recorder_seen) {
			snprintf(why, whylen, "[Recorder] given twice");
			return -1;
		}
		p->recorder_seen = 1;
		p->section = 1;
		return 0;
	}
	if (strcmp(section, "Record") == 0) {
		if (!arg[0] || strlen(arg) > HG_NAME_MAX) {
			snprintf(why, whylen, "[Record <glob>] needs a topic name or glob");
			return -1;
		}
		for (const char *s = arg; *s; s++)
			if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' ||
			      *s == '*' || *s == '?')) {
				snprintf(why, whylen, "[Record %s]: not a topic name or glob", arg);
				return -1;
			}
		if (p->c->n_rules == REC_MAX_RULES) {
			snprintf(why, whylen, "more than %d [Record] sections", REC_MAX_RULES);
			return -1;
		}
		struct rec_rule *r = &p->c->rules[p->c->n_rules++];
		memset(r, 0, sizeof *r);
		snprintf(r->glob, sizeof r->glob, "%s", arg);
		r->line = line;
		p->section = 2;
		return 0;
	}
	snprintf(why, whylen, "unknown section [%s]", section);
	return -1;
}

static int parse_policy(const char *v, struct rec_rule *r, char *why, size_t whylen)
{
	if (strlen(v) >= sizeof r->text) {
		snprintf(why, whylen, "Policy: too long");
		return -1;
	}
	snprintf(r->text, sizeof r->text, "%s", v);
	r->every_n = 1;
	if (strcmp(v, "full") == 0)
		r->policy = HGR_FULL;
	else if (strcmp(v, "meta") == 0)
		r->policy = HGR_META;
	else if (strcmp(v, "off") == 0)
		r->policy = HGR_OFF;
	else if (strncmp(v, "every:", 6) == 0) {
		char *e;
		unsigned long long n = strtoull(v + 6, &e, 10);
		if (e == v + 6 || *e || n == 0) {
			snprintf(why, whylen, "Policy: every:N needs N from 1");
			return -1;
		}
		r->policy = HGR_EVERY;
		r->every_n = n;
	} else if (strncmp(v, "rate:", 5) == 0) {
		int64_t mhz = hg_parse_rate_mhz(v + 5);
		if (mhz <= 0 || mhz > 0xffffffffll) {
			snprintf(why, whylen, "Policy: rate:<Hz> needs a rate above 0");
			return -1;
		}
		r->policy = HGR_RATE;
		r->rate_mhz = (uint32_t)mhz;
	} else {
		snprintf(why, whylen, "Policy: full, every:N, rate:<Hz>, meta or off, not '%s'", v);
		return -1;
	}
	r->have_policy = 1;
	return 0;
}

static int parse_class(const char *v, enum rec_class *out)
{
	if (strcmp(v, "off") == 0)
		*out = REC_BB_OFF;
	else if (strcmp(v, "low") == 0)
		*out = REC_BB_LOW;
	else if (strcmp(v, "normal") == 0)
		*out = REC_BB_NORMAL;
	else if (strcmp(v, "high") == 0)
		*out = REC_BB_HIGH;
	else
		return -1;
	return 0;
}

static int on_key(void *ctx, const char *key, const char *value, int line, char *why, size_t whylen)
{
	struct cstate *p = ctx;
	struct rec_conf *c = p->c;
	(void)line;
	if (p->section == 1) {
		if (strcmp(key, "AutoStart") == 0) {
			if (once(p, K_AUTOSTART, key, why, whylen) < 0)
				return -1;
			if (strcmp(value, "boot") == 0)
				c->autostart = REC_BOOT;
			else if (strcmp(value, "armed") == 0)
				c->autostart = REC_ARMED;
			else if (strcmp(value, "manual") == 0)
				c->autostart = REC_MANUAL;
			else {
				snprintf(why, whylen, "AutoStart: boot, armed or manual, not '%s'", value);
				return -1;
			}
			return 0;
		}
		if (strcmp(key, "Dir") == 0) {
			if (once(p, K_DIR, key, why, whylen) < 0)
				return -1;
			if (value[0] != '/' || strlen(value) >= sizeof c->dir - 100) {
				snprintf(why, whylen, "Dir: an absolute path under %zu bytes", sizeof c->dir - 100);
				return -1;
			}
			snprintf(c->dir, sizeof c->dir, "%s", value);
			return 0;
		}
		if (strcmp(key, "MinFree") == 0 || strcmp(key, "BlackBoxMemory") == 0 || strcmp(key, "WriteBuffer") == 0) {
			int k = key[0] == 'M' ? K_MINFREE : key[0] == 'B' ? K_BLACKBOXMEM : K_WRITEBUFFER;
			if (once(p, k, key, why, whylen) < 0)
				return -1;
			int64_t v = hg_ini_parse_size(value);
			if (v < 0) {
				snprintf(why, whylen, "%s: a size such as 32MB, not '%s'", key, value);
				return -1;
			}
			if (k == K_MINFREE)
				c->min_free = (uint64_t)v;
			else if (k == K_BLACKBOXMEM)
				c->blackbox_mem = (uint64_t)v;
			else {
				if (v < 65536) {
					snprintf(why, whylen, "WriteBuffer: at least 64KB");
					return -1;
				}
				c->write_buffer = (uint64_t)v;
			}
			return 0;
		}
		if (strcmp(key, "BlackBox") == 0) {
			if (once(p, K_BLACKBOX, key, why, whylen) < 0)
				return -1;
			int64_t d = hg_ini_parse_duration(value);
			if (d < 0) {
				snprintf(why, whylen, "BlackBox: a duration such as 60s, or 0, not '%s'", value);
				return -1;
			}
			c->blackbox_ns = d;
			return 0;
		}
		snprintf(why, whylen, "unknown key %s in [Recorder]", key);
		return -1;
	}
	if (p->section == 2) {
		struct rec_rule *r = &c->rules[c->n_rules - 1];
		if (strcmp(key, "Policy") == 0) {
			if (once(p, K_POLICY, key, why, whylen) < 0)
				return -1;
			return parse_policy(value, r, why, whylen);
		}
		if (strcmp(key, "BlackBox") == 0) {
			if (once(p, K_CLASS, key, why, whylen) < 0)
				return -1;
			if (parse_class(value, &r->class) < 0) {
				snprintf(why, whylen, "BlackBox: off, low, normal or high, not '%s'", value);
				return -1;
			}
			r->have_class = 1;
			return 0;
		}
		snprintf(why, whylen, "unknown key %s in [Record %s]", key, r->glob);
		return -1;
	}
	snprintf(why, whylen, "a key before any section");
	return -1;
}

int rec_conf_load(const char *path, struct rec_conf *c, char *err, size_t errlen)
{
	memset(c, 0, sizeof *c);
	c->autostart = REC_ARMED;
	const char *root = getenv("HANGAR_ROOT");
	snprintf(c->dir, sizeof c->dir, "%s/var/lib/hangar/recordings", root && *root ? root : "");
	c->min_free = (uint64_t)1 << 30;
	c->blackbox_ns = 60000000000ll;
	c->blackbox_mem = 32u << 20;
	c->write_buffer = 16u << 20;
	if (!path)
		return 0;
	static char text[65536];
	size_t len;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	struct cstate st = { .c = c };
	char why[300];
	if (hg_ini_parse(text, len, on_section, on_key, &st, why, sizeof why) < 0) {
		snprintf(err, errlen, "%s: %s", path, why);
		return -1;
	}
	return 0;
}

void rec_conf_resolve(const struct rec_conf *c, const char *topic, int is_frame, uint32_t rate_mhz,
		      struct rec_policy *out)
{
	memset(out, 0, sizeof *out);
	int is_event = rate_mhz == 0;
	const struct rec_rule *r = NULL;
	for (int i = 0; i < c->n_rules; i++)
		if (hg_glob_match(c->rules[i].glob, topic)) {
			r = &c->rules[i];
			break;
		}
	if (r && r->have_policy) {
		out->policy = r->policy;
		out->every_n = r->every_n;
		snprintf(out->text, sizeof out->text, "%s", r->text);
		if (r->policy == HGR_RATE) {
			/* every:N with N = declared rate / wanted rate, rounded, at
			 * least 1; an event topic is recorded full. */
			if (is_event) {
				out->policy = HGR_FULL;
				out->every_n = 1;
			} else {
				uint64_t n = ((uint64_t)rate_mhz + r->rate_mhz / 2) / r->rate_mhz;
				out->every_n = n ? n : 1;
			}
		}
	} else {
		out->policy = is_frame ? HGR_META : HGR_FULL;
		out->every_n = 1;
		snprintf(out->text, sizeof out->text, "%s", is_frame ? "meta" : "full");
	}
	if (r && r->have_class)
		out->class = r->class;
	else if (hg_glob_match("dataset.*", topic))
		out->class = REC_BB_OFF;
	else if (is_frame)
		out->class = REC_BB_LOW;
	else if (is_event)
		out->class = REC_BB_HIGH;
	else
		out->class = REC_BB_NORMAL;
}
