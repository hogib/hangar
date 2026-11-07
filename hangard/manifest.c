// SPDX-License-Identifier: GPL-3.0-or-later
/* Manifests: parsing to the letter of docs/02 "Syntax" and "Sections and
 * keys", the single-manifest rules, the implicit topics of docs/03 "Wiring",
 * and the graph checks of docs/02 "How the manager uses manifests". Every
 * refusal names the file and the line. */
#define _GNU_SOURCE
#include "manifest.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hg_topic.h"
#include "hg_wiring.h"

/* ---- Values ------------------------------------------------------------ */

int64_t mf_parse_duration(const char *s)
{
	/* digits[.digits] then ms, s or min (docs/02), exact in nanoseconds:
	 * the number is read as value * 1e9, then scaled by the unit. */
	const char *p = s;
	int64_t v = 0;
	int digits = 0, frac = 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		if (v > ((int64_t)1 << 50))
			return -1;
		p++;
		digits++;
	}
	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9') {
			if (frac < 9) {
				v = v * 10 + (*p - '0');
				frac++;
			} else if (*p != '0') {
				return -1;
			}
			p++;
			digits++;
		}
	}
	if (digits == 0)
		return -1;
	for (; frac < 9; frac++)
		v *= 10;
	if (strcmp(p, "ms") == 0) {
		if (v % 1000 != 0)
			return -1; /* finer than a nanosecond */
		return v / 1000;
	}
	if (strcmp(p, "s") == 0)
		return v;
	if (strcmp(p, "min") == 0)
		return v * 60;
	return -1;
}

int mf_fmt(char *dst, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, len, fmt, ap);
	va_end(ap);
	return n;
}

int mf_parse_bool(const char *s)
{
	if (strcmp(s, "yes") == 0)
		return 1;
	if (strcmp(s, "no") == 0)
		return 0;
	return -1;
}

static int parse_range(const char *s, uint64_t *lo, uint64_t *hi)
{
	const char *dots = strstr(s, "..");
	if (!dots)
		return -1;
	char a[64];
	size_t n = (size_t)(dots - s);
	if (n == 0 || n >= sizeof a)
		return -1;
	memcpy(a, s, n);
	a[n] = 0;
	int64_t l = mf_parse_duration(a), h = mf_parse_duration(dots + 2);
	if (l < 0 || h < 0 || l > h)
		return -1;
	*lo = (uint64_t)l;
	*hi = (uint64_t)h;
	return 0;
}

static int parse_burst(const char *s, uint32_t *count, uint64_t *period)
{
	const char *sl = strchr(s, '/');
	if (!sl || sl == s)
		return -1;
	char *e;
	unsigned long c = strtoul(s, &e, 10);
	if (e != sl || c == 0 || c > 100000)
		return -1;
	int64_t p = mf_parse_duration(sl + 1);
	if (p <= 0)
		return -1;
	*count = (uint32_t)c;
	*period = (uint64_t)p;
	return 0;
}

static int parse_u32(const char *s, uint32_t *out)
{
	if (!*s)
		return -1;
	char *e;
	unsigned long v = strtoul(s, &e, 10);
	if (*e || v > 0xffffffffu)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static int parse_int(const char *s, int *out)
{
	if (!*s)
		return -1;
	char *e;
	long v = strtol(s, &e, 10);
	if (*e || v < -1000000 || v > 1000000)
		return -1;
	*out = (int)v;
	return 0;
}

int mf_valid_module_name(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > 31)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_'))
			return 0;
	return 1;
}

/* User and group names: POSIX portable, 1..31 bytes. */
static int valid_account_name(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > 31)
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!(islower((unsigned char)c) || isupper((unsigned char)c) || isdigit((unsigned char)c) ||
		      c == '_' || c == '-' || (c == '$' && i == n - 1)))
			return 0;
	}
	return 1;
}

static int is_role(const char *s)
{
	return strcmp(s, "vehicle") == 0 || strcmp(s, "topside") == 0;
}

int mf_valid_allow_token(const char *t, const char *role)
{
	if (strcmp(t, "*") == 0 || strcmp(t, "admin") == 0)
		return 1;
	const char *colon = strchr(t, ':');
	if (!colon) {
		return mf_valid_module_name(t);
	}
	char r[16];
	size_t n = (size_t)(colon - t);
	if (n >= sizeof r)
		return 0;
	memcpy(r, t, n);
	r[n] = 0;
	/* A module or operator of the other side: this machine's role would
	 * name a local caller, which is written without a role. */
	if (!is_role(r) || strcmp(r, role) == 0)
		return 0;
	const char *rest = colon + 1;
	return strcmp(rest, "*") == 0 || strcmp(rest, "admin") == 0 || mf_valid_module_name(rest);
}

int mf_parse_exec(const char *s, struct mf_exec *out)
{
	memset(out, 0, sizeof *out);
	if (strlen(s) >= sizeof out->buf)
		return -1;
	char *w = out->buf;
	const char *p = s;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (out->argc == MF_MAX_ARGS)
			return -1;
		out->argv[out->argc++] = w;
		while (*p && *p != ' ' && *p != '\t') {
			if (*p == '"') {
				p++;
				while (*p && *p != '"')
					*w++ = *p++;
				if (*p != '"')
					return -1; /* unterminated */
				p++;
			} else {
				*w++ = *p++;
			}
		}
		*w++ = 0;
	}
	if (out->argc == 0 || out->argv[0][0] != '/')
		return -1;
	return 0;
}

/* Splits a blank-separated list into fixed-size entries. The count, or -1
 * when one does not fit or there are too many. */
static int split_words(const char *s, char *out, size_t width, int max)
{
	int n = 0;
	while (*s) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		const char *b = s;
		while (*s && *s != ' ' && *s != '\t')
			s++;
		size_t len = (size_t)(s - b);
		if (len >= width || n == max)
			return -1;
		memcpy(out + (size_t)n * width, b, len);
		out[(size_t)n * width + len] = 0;
		n++;
	}
	return n;
}

static int parse_cpus(const char *s, uint64_t mask[4])
{
	memset(mask, 0, 4 * sizeof *mask);
	if (!*s)
		return -1;
	const char *p = s;
	int any = 0;
	while (*p) {
		char *e;
		long a = strtol(p, &e, 10);
		if (e == p || a < 0 || a > 255)
			return -1;
		long b = a;
		p = e;
		if (*p == '-') {
			b = strtol(p + 1, &e, 10);
			if (e == p + 1 || b < a || b > 255)
				return -1;
			p = e;
		}
		for (long c = a; c <= b; c++)
			mask[c / 64] |= (uint64_t)1 << (c % 64);
		any = 1;
		if (*p == ',')
			p++;
		else if (*p)
			return -1;
	}
	return any ? 0 : -1;
}

static const char *const manager_caps[] = { "CAP_SETUID",  "CAP_SETGID", "CAP_SETPCAP",
					    "CAP_SYS_NICE", "CAP_IPC_LOCK", "CAP_KILL",
					    "CAP_CHOWN",   "CAP_FOWNER", NULL };

static int manager_cap(const char *s)
{
	for (int i = 0; manager_caps[i]; i++)
		if (strcmp(manager_caps[i], s) == 0)
			return 1;
	return 0;
}

/* ---- The parser -------------------------------------------------------- */

enum section {
	SEC_NONE,
	SEC_MODULE,
	SEC_PUBLISH,
	SEC_SUBSCRIBE,
	SEC_SERVE,
	SEC_CALL,
	SEC_BRIDGE,
	SEC_ACTUATOR,
	SEC_PARAMS,
	SEC_RESTART,
	SEC_HEALTH,
	SEC_SCHEDULE,
	SEC_COUNT
};

struct parser {
	struct mf_module *m;
	const char *path;
	int line;
	enum section sec;
	int idx;                  /* the current pub/sub/serve/call */
	unsigned seen[SEC_COUNT]; /* bit per key, for duplicates */
	int stale_line;           /* lines kept for checks after the section ends */
	char *err;
	size_t errlen;
};

static int fail(struct parser *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(struct parser *p, const char *fmt, ...)
{
	char what[MF_ERR];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(what, sizeof what, fmt, ap);
	va_end(ap);
	if (p->err && p->errlen)
		mf_fmt(p->err, p->errlen, "%s:%d: %s", p->path, p->line, what);
	errno = EINVAL;
	return -1;
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = 0;
	return s;
}

static int seen(struct parser *p, int key)
{
	if (p->seen[p->sec] & (1u << key))
		return 1;
	p->seen[p->sec] |= 1u << key;
	return 0;
}

static int dup_key(struct parser *p, const char *key, int bit)
{
	if (seen(p, bit))
		return fail(p, "%s given twice", key);
	return 0;
}

static int set_name(struct parser *p, char *dst, size_t len, const char *key, const char *v,
		    int (*valid)(const char *), const char *rule)
{
	if (!valid(v))
		return fail(p, "%s: '%s' is not %s", key, v, rule);
	mf_fmt(dst, len, "%s", v);
	return 0;
}

static int valid_topic(const char *s)
{
	return hg_valid_name(s);
}

static int valid_topic_or_glob(const char *s)
{
	if (!strchr(s, '*') && !strchr(s, '?'))
		return hg_valid_name(s);
	size_t n = strlen(s);
	if (n == 0 || n > HG_NAME_MAX)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' ||
		      *s == '*' || *s == '?'))
			return 0;
	return 1;
}

static int valid_type_name(const char *s)
{
	/* A .msg stem, "frame", or "dataset:<name>". */
	if (strcmp(s, "frame") == 0)
		return 1;
	if (strncmp(s, "dataset:", 8) == 0)
		return mf_valid_module_name(s + 8);
	size_t n = strlen(s);
	if (n == 0 || n > 63)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_'))
			return 0;
	return 1;
}

static int set_duration(struct parser *p, const char *key, const char *v, uint64_t *out, int allow_zero)
{
	int64_t d = mf_parse_duration(v);
	if (d < 0 || (!allow_zero && d == 0))
		return fail(p, "%s: '%s' is not a duration (100ms, 2.5s, 1min)", key, v);
	*out = (uint64_t)d;
	return 0;
}

static int set_bool(struct parser *p, const char *key, const char *v, int *out)
{
	int b = mf_parse_bool(v);
	if (b < 0)
		return fail(p, "%s: '%s' is not yes or no", key, v);
	*out = b;
	return 0;
}

static int set_allow(struct parser *p, const char *v, char (*allow)[MF_REF], int *n, int *set)
{
	char words[MF_MAX_ALLOW + 1][MF_REF];
	int k = split_words(v, words[0], MF_REF, MF_MAX_ALLOW + 1);
	if (k < 0 || k > MF_MAX_ALLOW)
		return fail(p, "Allow: at most %d tokens", MF_MAX_ALLOW);
	for (int i = 0; i < k; i++) {
		/* The role is checked at the set check, where it is known; here
		 * only the form. */
		if (!mf_valid_allow_token(words[i], "") )
			return fail(p, "Allow: '%s' is not a caller token", words[i]);
		memcpy(allow[i], words[i], MF_REF);
	}
	*n = k;
	*set = 1;
	return 0;
}

/* ---- Sections ---------------------------------------------------------- */

static int key_module(struct parser *p, const char *k, const char *v)
{
	struct mf_module *m = p->m;
	if (strcmp(k, "Description") == 0) {
		if (dup_key(p, k, 0))
			return -1;
		mf_fmt(m->description, sizeof m->description, "%s", v);
		return 0;
	}
	if (strcmp(k, "Legacy") == 0)
		return dup_key(p, k, 1) ? -1 : set_bool(p, k, v, &m->legacy);
	if (strcmp(k, "Neutral") == 0) {
		if (dup_key(p, k, 2))
			return -1;
		if (mf_parse_exec(v, &m->module_neutral) < 0)
			return fail(p, "Neutral: not a command (an absolute path and its arguments)");
		m->has_module_neutral = 1;
		return 0;
	}
	if (strcmp(k, "ExclusiveGroups") == 0) {
		if (dup_key(p, k, 3))
			return -1;
		m->n_excl = split_words(v, m->excl[0], MF_NAME, MF_MAX_GROUPS);
		if (m->n_excl < 0)
			return fail(p, "ExclusiveGroups: too many or too long");
		for (int i = 0; i < m->n_excl; i++)
			if (!valid_account_name(m->excl[i]))
				return fail(p, "ExclusiveGroups: '%s' is not a group name", m->excl[i]);
		return 0;
	}
	if (strcmp(k, "Exec") == 0) {
		if (dup_key(p, k, 4))
			return -1;
		if (mf_parse_exec(v, &m->exec) < 0)
			return fail(p, "Exec: the first word must be an absolute path; \"...\" keeps blanks; no shell");
		return 0;
	}
	if (strcmp(k, "Environment") == 0) {
		/* Repeatable. */
		if (m->n_env == MF_MAX_ENV)
			return fail(p, "Environment: more than %d entries", MF_MAX_ENV);
		const char *eq = strchr(v, '=');
		if (!eq || eq == v || strlen(v) >= sizeof m->env[0])
			return fail(p, "Environment: want NAME=value");
		for (const char *c = v; c < eq; c++)
			if (!(isalnum((unsigned char)*c) || *c == '_'))
				return fail(p, "Environment: '%.*s' is not a variable name", (int)(eq - v), v);
		mf_fmt(m->env[m->n_env++], sizeof m->env[0], "%s", v);
		return 0;
	}
	if (strcmp(k, "User") == 0) {
		if (dup_key(p, k, 5))
			return -1;
		m->user_set = 1;
		return set_name(p, m->user, sizeof m->user, k, v, valid_account_name, "a user name");
	}
	if (strcmp(k, "Group") == 0) {
		if (dup_key(p, k, 6))
			return -1;
		m->group_set = 1;
		return set_name(p, m->group, sizeof m->group, k, v, valid_account_name, "a group name");
	}
	if (strcmp(k, "SupplementaryGroups") == 0) {
		if (dup_key(p, k, 7))
			return -1;
		m->n_groups = split_words(v, m->groups[0], MF_NAME, MF_MAX_GROUPS);
		if (m->n_groups < 0)
			return fail(p, "SupplementaryGroups: too many or too long");
		for (int i = 0; i < m->n_groups; i++)
			if (!valid_account_name(m->groups[i]))
				return fail(p, "SupplementaryGroups: '%s' is not a group name", m->groups[i]);
		return 0;
	}
	if (strcmp(k, "Capabilities") == 0) {
		if (dup_key(p, k, 8))
			return -1;
		m->n_caps = split_words(v, m->caps[0], sizeof m->caps[0], MF_MAX_CAPS);
		if (m->n_caps < 0)
			return fail(p, "Capabilities: too many or too long");
		for (int i = 0; i < m->n_caps; i++)
			if (!manager_cap(m->caps[i]))
				return fail(p, "Capabilities: %s is not in the manager's set", m->caps[i]);
		return 0;
	}
	if (strcmp(k, "Privileged") == 0)
		return dup_key(p, k, 9) ? -1 : set_bool(p, k, v, &m->privileged);
	return fail(p, "unknown key %s in [Module]", k);
}

static int key_publish(struct parser *p, const char *k, const char *v)
{
	struct mf_pub *pub = &p->m->pubs[p->idx];
	if (strcmp(k, "Type") == 0) {
		if (dup_key(p, k, 0))
			return -1;
		return set_name(p, pub->type, sizeof pub->type, k, v, valid_type_name,
				"a type name (a .msg stem, frame, or dataset:<name>)");
	}
	if (strcmp(k, "Rate") == 0) {
		if (dup_key(p, k, 1))
			return -1;
		int64_t r = hg_parse_rate_mhz(v);
		if (r < 0)
			return fail(p, "Rate: '%s' is not a rate in Hz", v);
		pub->rate_mhz = (uint32_t)r;
		return 0;
	}
	if (strcmp(k, "Slots") == 0) {
		if (dup_key(p, k, 2))
			return -1;
		uint32_t s;
		if (parse_u32(v, &s) < 0 || s < 2 || s > 65536 || (s & (s - 1)))
			return fail(p, "Slots: '%s' is not a power of two from 2 to 65536", v);
		pub->slots = s;
		return 0;
	}
	if (strcmp(k, "Inputs") == 0) {
		if (dup_key(p, k, 3))
			return -1;
		char words[MF_MAX_INPUTS + 1][MF_REF];
		int n = split_words(v, words[0], MF_REF, MF_MAX_INPUTS + 1);
		if (n < 0 || n > MF_MAX_INPUTS)
			return fail(p, "Inputs: more than %d topics", MF_MAX_INPUTS);
		for (int i = 0; i < n; i++) {
			const char *colon = strchr(words[i], ':');
			const char *t = colon ? colon + 1 : words[i];
			char role[16] = "";
			if (colon && (size_t)(colon - words[i]) < sizeof role)
				memcpy(role, words[i], (size_t)(colon - words[i]));
			if ((colon && !is_role(role)) || !valid_topic(t))
				return fail(p, "Inputs: '%s' is not a topic or <role>:<topic>", words[i]);
			memcpy(pub->inputs[i], words[i], MF_REF);
		}
		pub->n_inputs = n;
		return 0;
	}
	if (strcmp(k, "Sources") == 0) {
		if (dup_key(p, k, 4))
			return -1;
		char words[MF_MAX_SOURCES + 1][MF_TOPIC];
		int n = split_words(v, words[0], MF_TOPIC, MF_MAX_SOURCES + 1);
		if (n < 0 || n > MF_MAX_SOURCES)
			return fail(p, "Sources: more than %d topics", MF_MAX_SOURCES);
		for (int i = 0; i < n; i++) {
			if (!valid_topic(words[i]))
				return fail(p, "Sources: '%s' is not a topic name", words[i]);
			memcpy(pub->sources[i], words[i], MF_TOPIC);
		}
		pub->n_sources = n;
		return 0;
	}
	if (strcmp(k, "StaleAfter") == 0) {
		if (dup_key(p, k, 5))
			return -1;
		pub->stale_set = p->line;
		return set_duration(p, k, v, &pub->stale_after_ns, 0);
	}
	if (strcmp(k, "Actuator") == 0)
		return dup_key(p, k, 6) ? -1 : set_bool(p, k, v, &pub->actuator);
	return fail(p, "unknown key %s in [Publish]", k);
}

static int key_subscribe(struct parser *p, const char *k, const char *v)
{
	struct mf_sub *sub = &p->m->subs[p->idx];
	if (strcmp(k, "Type") == 0) {
		if (dup_key(p, k, 0))
			return -1;
		if (sub->glob)
			return fail(p, "Type: not allowed with a glob subscription");
		return set_name(p, sub->type, sizeof sub->type, k, v, valid_type_name, "a type name");
	}
	return fail(p, "unknown key %s in [Subscribe]", k);
}

static int key_serve(struct parser *p, const char *k, const char *v)
{
	struct mf_serve *s = &p->m->serves[p->idx];
	if (strcmp(k, "Request") == 0)
		return dup_key(p, k, 0) ? -1 :
		       set_name(p, s->req, sizeof s->req, k, v, valid_type_name, "a type name");
	if (strcmp(k, "Reply") == 0)
		return dup_key(p, k, 1) ? -1 :
		       set_name(p, s->rep, sizeof s->rep, k, v, valid_type_name, "a type name");
	if (strcmp(k, "Allow") == 0)
		return dup_key(p, k, 2) ? -1 : set_allow(p, v, s->allow, &s->n_allow, &s->allow_set);
	if (strcmp(k, "Deadline") == 0)
		return dup_key(p, k, 3) ? -1 : set_duration(p, k, v, &s->deadline_ns, 0);
	return fail(p, "unknown key %s in [Serve]", k);
}

static int key_call(struct parser *p, const char *k, const char *v)
{
	struct mf_call *c = &p->m->calls[p->idx];
	if (strcmp(k, "Request") == 0)
		return dup_key(p, k, 0) ? -1 :
		       set_name(p, c->req, sizeof c->req, k, v, valid_type_name, "a type name");
	if (strcmp(k, "Reply") == 0)
		return dup_key(p, k, 1) ? -1 :
		       set_name(p, c->rep, sizeof c->rep, k, v, valid_type_name, "a type name");
	return fail(p, "unknown key %s in [Call]", k);
}

static int key_actuator(struct parser *p, const char *k, const char *v)
{
	struct mf_module *m = p->m;
	if (strcmp(k, "Command") == 0)
		return dup_key(p, k, 0) ? -1 :
		       set_name(p, m->act_command, sizeof m->act_command, k, v, valid_topic, "a topic name");
	if (strcmp(k, "Neutral") == 0) {
		if (dup_key(p, k, 1))
			return -1;
		if (mf_parse_exec(v, &m->act_neutral) < 0)
			return fail(p, "Neutral: not a command (an absolute path and its arguments)");
		return 0;
	}
	if (strcmp(k, "Groups") == 0) {
		if (dup_key(p, k, 2))
			return -1;
		m->n_act_groups = split_words(v, m->act_groups[0], MF_NAME, MF_MAX_GROUPS);
		if (m->n_act_groups < 0)
			return fail(p, "Groups: too many or too long");
		for (int i = 0; i < m->n_act_groups; i++)
			if (!valid_account_name(m->act_groups[i]))
				return fail(p, "Groups: '%s' is not a group name", m->act_groups[i]);
		return 0;
	}
	if (strcmp(k, "CommandTimeout") == 0)
		return dup_key(p, k, 3) ? -1 : set_duration(p, k, v, &m->command_timeout_ns, 0);
	if (strcmp(k, "SafetyTimeout") == 0)
		return dup_key(p, k, 4) ? -1 : set_duration(p, k, v, &m->safety_timeout_ns, 0);
	return fail(p, "unknown key %s in [Actuator]", k);
}

static int key_params(struct parser *p, const char *k, const char *v)
{
	struct mf_module *m = p->m;
	if (strcmp(k, "Type") == 0)
		return dup_key(p, k, 0) ? -1 :
		       set_name(p, m->params_type, sizeof m->params_type, k, v, valid_type_name, "a type name");
	if (strcmp(k, "Allow") == 0)
		return dup_key(p, k, 1) ? -1 :
		       set_allow(p, v, m->params_allow, &m->n_params_allow, &m->params_allow_set);
	return fail(p, "unknown key %s in [Params]", k);
}

static int key_restart(struct parser *p, const char *k, const char *v)
{
	struct mf_module *m = p->m;
	if (strcmp(k, "Policy") == 0) {
		if (dup_key(p, k, 0))
			return -1;
		if (strcmp(v, "always") == 0)
			m->policy = MF_RESTART_ALWAYS;
		else if (strcmp(v, "on-failure") == 0)
			m->policy = MF_RESTART_ON_FAILURE;
		else if (strcmp(v, "never") == 0)
			m->policy = MF_RESTART_NEVER;
		else
			return fail(p, "Policy: '%s' is not always, on-failure or never", v);
		return 0;
	}
	if (strcmp(k, "Backoff") == 0) {
		if (dup_key(p, k, 1))
			return -1;
		if (parse_range(v, &m->backoff_first_ns, &m->backoff_max_ns) < 0 || m->backoff_first_ns == 0)
			return fail(p, "Backoff: '%s' is not a duration range (0.1s..10s)", v);
		return 0;
	}
	if (strcmp(k, "ResetAfter") == 0)
		return dup_key(p, k, 2) ? -1 : set_duration(p, k, v, &m->reset_after_ns, 0);
	if (strcmp(k, "Burst") == 0) {
		if (dup_key(p, k, 3))
			return -1;
		if (parse_burst(v, &m->burst_count, &m->burst_period_ns) < 0)
			return fail(p, "Burst: '%s' is not <count>/<duration> (5/60s)", v);
		return 0;
	}
	if (strcmp(k, "OnGiveUp") == 0) {
		if (dup_key(p, k, 4))
			return -1;
		if (strcmp(v, "degrade") == 0)
			m->giveup = MF_GIVEUP_DEGRADE;
		else if (strcmp(v, "failsafe") == 0)
			m->giveup = MF_GIVEUP_FAILSAFE;
		else
			return fail(p, "OnGiveUp: '%s' is not degrade or failsafe", v);
		m->giveup_set = 1;
		return 0;
	}
	if (strcmp(k, "StopTimeout") == 0) {
		if (dup_key(p, k, 5))
			return -1;
		m->stop_timeout_set = 1;
		return set_duration(p, k, v, &m->stop_timeout_ns, 0);
	}
	return fail(p, "unknown key %s in [Restart]", k);
}

static int key_health(struct parser *p, const char *k, const char *v)
{
	struct mf_module *m = p->m;
	if (strcmp(k, "StartGrace") == 0)
		return dup_key(p, k, 0) ? -1 : set_duration(p, k, v, &m->start_grace_ns, 1);
	if (strcmp(k, "StaleGrace") == 0)
		return dup_key(p, k, 1) ? -1 : set_duration(p, k, v, &m->stale_grace_ns, 1);
	if (strcmp(k, "Watchdog") == 0)
		return dup_key(p, k, 2) ? -1 : set_duration(p, k, v, &m->watchdog_ns, 0);
	return fail(p, "unknown key %s in [Health]", k);
}

static int key_schedule(struct parser *p, const char *k, const char *v)
{
	struct mf_module *m = p->m;
	if (strcmp(k, "Policy") == 0) {
		if (dup_key(p, k, 0))
			return -1;
		if (strcmp(v, "other") == 0)
			m->sched = MF_SCHED_OTHER;
		else if (strcmp(v, "fifo") == 0)
			m->sched = MF_SCHED_FIFO;
		else if (strcmp(v, "rr") == 0)
			m->sched = MF_SCHED_RR;
		else
			return fail(p, "Policy: '%s' is not other, fifo or rr", v);
		return 0;
	}
	if (strcmp(k, "Priority") == 0) {
		if (dup_key(p, k, 1))
			return -1;
		int pr;
		if (parse_int(v, &pr) < 0 || pr < 1 || pr > 99)
			return fail(p, "Priority: '%s' is not 1 to 99", v);
		m->priority = pr;
		return 0;
	}
	if (strcmp(k, "Nice") == 0) {
		if (dup_key(p, k, 2))
			return -1;
		int n;
		if (parse_int(v, &n) < 0 || n < -20 || n > 19)
			return fail(p, "Nice: '%s' is not -20 to 19", v);
		m->nice = n;
		return 0;
	}
	if (strcmp(k, "CPUs") == 0) {
		if (dup_key(p, k, 3))
			return -1;
		if (strlen(v) >= sizeof m->cpus || parse_cpus(v, m->cpu_mask) < 0)
			return fail(p, "CPUs: '%s' is not a list or ranges (3, 2-3, 0,2)", v);
		mf_fmt(m->cpus, sizeof m->cpus, "%s", v);
		m->cpus_set = 1;
		return 0;
	}
	if (strcmp(k, "LockMemory") == 0)
		return dup_key(p, k, 4) ? -1 : set_bool(p, k, v, &m->lock_memory);
	return fail(p, "unknown key %s in [Schedule]", k);
}

/* Rules checked when a section ends: they need its whole key set. */
static int end_section(struct parser *p)
{
	struct mf_module *m = p->m;
	switch (p->sec) {
	case SEC_PUBLISH: {
		struct mf_pub *pub = &m->pubs[p->idx];
		if (!pub->type[0])
			return fail(p, "[Publish %s] needs Type=", pub->topic);
		if (pub->stale_set && !pub->rate_mhz) {
			p->line = pub->stale_set;
			return fail(p, "StaleAfter: only with a Rate");
		}
		if (!pub->slots)
			pub->slots = strcmp(pub->type, "frame") == 0 ? 4 : 16;
		if (pub->rate_mhz && !pub->stale_after_ns)
			pub->stale_after_ns = 3 * 1000000000000ull / pub->rate_mhz;
		if (pub->actuator)
			m->has_actuator_output = 1;
		break;
	}
	case SEC_SERVE: {
		struct mf_serve *s = &m->serves[p->idx];
		if (!s->req[0] || !s->rep[0])
			return fail(p, "[Serve %s] needs Request= and Reply=", s->method);
		if (!s->deadline_ns)
			s->deadline_ns = 100000000;
		break;
	}
	case SEC_CALL: {
		struct mf_call *c = &m->calls[p->idx];
		if ((c->req[0] != 0) != (c->rep[0] != 0))
			return fail(p, "[Call %s.%s]: Request= and Reply= go together", c->server, c->method);
		break;
	}
	case SEC_ACTUATOR:
		if (!m->act_command[0] || !m->act_neutral.argc)
			return fail(p, "[Actuator] needs Command= and Neutral=");
		break;
	case SEC_PARAMS:
		if (!m->params_type[0])
			return fail(p, "[Params] needs Type=");
		break;
	case SEC_SCHEDULE:
		if (m->sched != MF_SCHED_OTHER && !m->priority)
			return fail(p, "[Schedule] Priority is required with Policy=fifo or rr");
		if (m->sched == MF_SCHED_OTHER && m->priority)
			return fail(p, "[Schedule] Priority is only for Policy=fifo or rr");
		if (m->sched != MF_SCHED_OTHER && m->nice)
			return fail(p, "[Schedule] Nice is only for Policy=other");
		break;
	default:
		break;
	}
	return 0;
}

static int begin_section(struct parser *p, const char *name, const char *arg)
{
	struct mf_module *m = p->m;
	int once = 0;
	enum section sec;
	if (strcmp(name, "Module") == 0) {
		sec = SEC_MODULE;
		once = 1;
	} else if (strcmp(name, "Publish") == 0) {
		sec = SEC_PUBLISH;
	} else if (strcmp(name, "Subscribe") == 0) {
		sec = SEC_SUBSCRIBE;
	} else if (strcmp(name, "Serve") == 0) {
		sec = SEC_SERVE;
	} else if (strcmp(name, "Call") == 0) {
		sec = SEC_CALL;
	} else if (strcmp(name, "Bridge") == 0) {
		sec = SEC_BRIDGE;
		once = 1;
	} else if (strcmp(name, "Actuator") == 0) {
		sec = SEC_ACTUATOR;
		once = 1;
	} else if (strcmp(name, "Params") == 0) {
		sec = SEC_PARAMS;
		once = 1;
	} else if (strcmp(name, "Restart") == 0) {
		sec = SEC_RESTART;
		once = 1;
	} else if (strcmp(name, "Health") == 0) {
		sec = SEC_HEALTH;
		once = 1;
	} else if (strcmp(name, "Schedule") == 0) {
		sec = SEC_SCHEDULE;
		once = 1;
	} else {
		return fail(p, "unknown section [%s]", name);
	}
	if (once) {
		if (*arg)
			return fail(p, "[%s] takes no argument", name);
		if (m->lines[sec])
			return fail(p, "[%s] given twice (first at line %d)", name, m->lines[sec]);
		m->lines[sec] = p->line;
	} else if (!*arg) {
		return fail(p, "[%s] needs an argument", name);
	}
	p->sec = sec;
	p->seen[sec] = 0;
	switch (sec) {
	case SEC_PUBLISH:
		if (!valid_topic(arg))
			return fail(p, "[Publish %s]: not a topic name", arg);
		for (int i = 0; i < m->n_pubs; i++)
			if (strcmp(m->pubs[i].topic, arg) == 0)
				return fail(p, "[Publish %s] given twice (first at line %d)", arg,
					    m->pubs[i].line);
		if (m->n_pubs == MF_MAX_PUBS)
			return fail(p, "more than %d [Publish] sections", MF_MAX_PUBS);
		p->idx = m->n_pubs++;
		memset(&m->pubs[p->idx], 0, sizeof m->pubs[0]);
		mf_fmt(m->pubs[p->idx].topic, MF_TOPIC, "%s", arg);
		m->pubs[p->idx].line = p->line;
		m->pubs[p->idx].type_idx = -1;
		break;
	case SEC_SUBSCRIBE:
		if (!valid_topic_or_glob(arg))
			return fail(p, "[Subscribe %s]: not a topic name or glob", arg);
		for (int i = 0; i < m->n_subs; i++)
			if (strcmp(m->subs[i].topic, arg) == 0)
				return fail(p, "[Subscribe %s] given twice (first at line %d)", arg,
					    m->subs[i].line);
		if (m->n_subs == MF_MAX_SUBS)
			return fail(p, "more than %d [Subscribe] sections", MF_MAX_SUBS);
		p->idx = m->n_subs++;
		memset(&m->subs[p->idx], 0, sizeof m->subs[0]);
		mf_fmt(m->subs[p->idx].topic, MF_TOPIC, "%s", arg);
		m->subs[p->idx].glob = strchr(arg, '*') || strchr(arg, '?');
		m->subs[p->idx].line = p->line;
		break;
	case SEC_SERVE:
		if (!mf_valid_module_name(arg))
			return fail(p, "[Serve %s]: not a method name ([a-z0-9_], 1 to 31 bytes)", arg);
		for (int i = 0; i < m->n_serves; i++)
			if (strcmp(m->serves[i].method, arg) == 0)
				return fail(p, "[Serve %s] given twice (first at line %d)", arg,
					    m->serves[i].line);
		if (m->n_serves == MF_MAX_SERVES)
			return fail(p, "more than %d [Serve] sections", MF_MAX_SERVES);
		p->idx = m->n_serves++;
		memset(&m->serves[p->idx], 0, sizeof m->serves[0]);
		mf_fmt(m->serves[p->idx].method, MF_NAME, "%s", arg);
		m->serves[p->idx].line = p->line;
		m->serves[p->idx].req_idx = m->serves[p->idx].rep_idx = -1;
		break;
	case SEC_CALL: {
		const char *dot = strchr(arg, '.');
		char server[MF_NAME];
		if (!dot || (size_t)(dot - arg) >= sizeof server)
			return fail(p, "[Call %s]: want <module>.<method>", arg);
		memcpy(server, arg, (size_t)(dot - arg));
		server[dot - arg] = 0;
		if (!mf_valid_module_name(server) || !mf_valid_module_name(dot + 1))
			return fail(p, "[Call %s]: want <module>.<method>, each [a-z0-9_] 1 to 31 bytes", arg);
		for (int i = 0; i < m->n_calls; i++)
			if (strcmp(m->calls[i].server, server) == 0 && strcmp(m->calls[i].method, dot + 1) == 0)
				return fail(p, "[Call %s] given twice (first at line %d)", arg, m->calls[i].line);
		if (m->n_calls == MF_MAX_CALLS)
			return fail(p, "more than %d [Call] sections", MF_MAX_CALLS);
		p->idx = m->n_calls++;
		memset(&m->calls[p->idx], 0, sizeof m->calls[0]);
		mf_fmt(m->calls[p->idx].server, MF_NAME, "%s", server);
		mf_fmt(m->calls[p->idx].method, MF_NAME, "%s", dot + 1);
		m->calls[p->idx].line = p->line;
		m->calls[p->idx].req_idx = m->calls[p->idx].rep_idx = -1;
		break;
	}
	case SEC_BRIDGE:
		m->bridge = 1;
		break;
	case SEC_ACTUATOR:
		m->has_actuator = 1;
		break;
	case SEC_PARAMS:
		m->has_params = 1;
		break;
	default:
		break;
	}
	return 0;
}

static int key_value(struct parser *p, const char *k, const char *v)
{
	switch (p->sec) {
	case SEC_NONE:
		return fail(p, "%s: a key before any section", k);
	case SEC_MODULE:
		return key_module(p, k, v);
	case SEC_PUBLISH:
		return key_publish(p, k, v);
	case SEC_SUBSCRIBE:
		return key_subscribe(p, k, v);
	case SEC_SERVE:
		return key_serve(p, k, v);
	case SEC_CALL:
		return key_call(p, k, v);
	case SEC_BRIDGE:
		return fail(p, "[Bridge] has no keys (%s given)", k);
	case SEC_ACTUATOR:
		return key_actuator(p, k, v);
	case SEC_PARAMS:
		return key_params(p, k, v);
	case SEC_RESTART:
		return key_restart(p, k, v);
	case SEC_HEALTH:
		return key_health(p, k, v);
	case SEC_SCHEDULE:
		return key_schedule(p, k, v);
	default:
		return fail(p, "internal: section");
	}
}

static void defaults(struct mf_module *m)
{
	mf_fmt(m->user, sizeof m->user, "hangar");
	mf_fmt(m->group, sizeof m->group, "hangar");
	m->policy = MF_RESTART_ALWAYS;
	m->backoff_first_ns = 100000000;
	m->backoff_max_ns = 10000000000ull;
	m->reset_after_ns = 30000000000ull;
	m->burst_count = 5;
	m->burst_period_ns = 60000000000ull;
	m->giveup = MF_GIVEUP_DEGRADE;
	m->stop_timeout_ns = 5000000000ull;
	m->start_grace_ns = 5000000000ull;
	m->stale_grace_ns = 1000000000ull;
	m->command_timeout_ns = 200000000;
	m->safety_timeout_ns = 300000000;
	m->params_idx = -1;
}

int mf_parse_text(const char *text, size_t len, const char *name, const char *path,
		  struct mf_module *m, char *err, size_t errlen)
{
	memset(m, 0, sizeof *m);
	defaults(m);
	mf_fmt(m->name, sizeof m->name, "%s", name);
	mf_fmt(m->path, sizeof m->path, "%s", path);
	m->text_hash = hg_fnv1a64(text, len);
	struct parser p = { .m = m, .path = path, .err = err, .errlen = errlen };
	if (!mf_valid_module_name(name)) {
		p.line = 0;
		return fail(&p, "'%s' is not a module name ([a-z0-9_], 1 to 31 bytes)", name);
	}
	const char *s = text, *end = text + len;
	char line[2048];
	while (s < end) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		size_t n = nl ? (size_t)(nl - s) : (size_t)(end - s);
		p.line++;
		if (n >= sizeof line)
			return fail(&p, "line longer than %zu bytes", sizeof line - 1);
		memcpy(line, s, n);
		line[n] = 0;
		s = nl ? nl + 1 : end;
		char *t = trim(line);
		if (!*t || *t == '#' || *t == ';')
			continue;
		if (*t == '[') {
			char *close = strchr(t, ']');
			if (!close || close[1] != 0)
				return fail(&p, "malformed section header");
			*close = 0;
			char *hdr = trim(t + 1);
			char *sp = strchr(hdr, ' ');
			char *arg = (char *)"";
			if (sp) {
				*sp = 0;
				arg = trim(sp + 1);
			}
			if (!*hdr)
				return fail(&p, "empty section name");
			if (end_section(&p) < 0)
				return -1;
			if (begin_section(&p, hdr, arg) < 0)
				return -1;
			continue;
		}
		char *eq = strchr(t, '=');
		if (!eq)
			return fail(&p, "not a Key=Value line");
		*eq = 0;
		char *k = trim(t), *v = trim(eq + 1);
		if (!*k)
			return fail(&p, "empty key");
		if (key_value(&p, k, v) < 0)
			return -1;
	}
	p.line++;
	if (end_section(&p) < 0)
		return -1;
	if (!m->lines[SEC_MODULE])
		return fail(&p, "no [Module] section"); /* reported at the end of the file */
	p.line = m->lines[SEC_MODULE];
	if (!m->exec.argc)
		return fail(&p, "[Module] needs Exec=");
	/* Defaults that depend on the whole manifest (docs/02). */
	m->has_neutral = m->has_actuator || m->has_module_neutral;
	if (!m->giveup_set && m->has_actuator_output)
		m->giveup = MF_GIVEUP_FAILSAFE;
	if (!m->stop_timeout_set && (m->has_actuator_output || m->has_module_neutral))
		m->stop_timeout_ns = 500000000;
	return 0;
}

int mf_parse_file(const char *path, struct mf_module *m, char *err, size_t errlen)
{
	memset(m, 0, sizeof *m);
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char name[MF_NAME + 8];
	mf_fmt(name, sizeof name, "%s", base);
	char *dot = strrchr(name, '.');
	if (!dot || strcmp(dot, ".module") != 0 || dot == name) {
		if (err && errlen)
			mf_fmt(err, errlen, "%s:0: not a <name>.module file", path);
		errno = EINVAL;
		return -1;
	}
	*dot = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		if (err && errlen)
			mf_fmt(err, errlen, "%s:0: %s", path, strerror(errno));
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size > (1 << 20)) {
		close(fd);
		if (err && errlen)
			mf_fmt(err, errlen, "%s:0: not a readable file under 1 MB", path);
		errno = EFBIG;
		return -1;
	}
	size_t len = (size_t)st.st_size;
	char *text = malloc(len + 1);
	if (!text) {
		close(fd);
		return -1;
	}
	size_t off = 0;
	while (off < len) {
		ssize_t r = read(fd, text + off, len - off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		off += (size_t)r;
	}
	close(fd);
	int rc = mf_parse_text(text, off, name, path, m, err, errlen);
	int e = errno;
	free(text);
	errno = e;
	return rc;
}

/* ---- The set ------------------------------------------------------------ */

void mf_set_init(struct mf_set *s, const char *role, const char *privileged, const char *msg_dir)
{
	memset(s, 0, sizeof *s);
	mf_fmt(s->role, sizeof s->role, "%s", role);
	mf_fmt(s->other_role, sizeof s->other_role, "%s",
		 strcmp(role, "vehicle") == 0 ? "topside" : "vehicle");
	mf_fmt(s->privileged_module, sizeof s->privileged_module, "%s", privileged ? privileged : "");
	mf_fmt(s->msg_dir, sizeof s->msg_dir, "%s", msg_dir ? msg_dir : "");
}

struct mf_module *mf_find(const struct mf_set *s, const char *name)
{
	for (int i = 0; i < s->n_modules; i++)
		if (strcmp(s->modules[i]->name, name) == 0)
			return s->modules[i];
	return NULL;
}

int mf_set_add(struct mf_set *s, struct mf_module *m)
{
	if (mf_find(s, m->name)) {
		errno = EEXIST;
		return -1;
	}
	if (s->n_modules == MF_MAX_MODULES) {
		errno = ENOSPC;
		return -1;
	}
	s->modules[s->n_modules++] = m;
	return 0;
}

static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int mf_set_load_dir(struct mf_set *s, const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return -1;
	char *names[MF_MAX_MODULES * 2];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t len = strlen(e->d_name);
		if (len < 8 || strcmp(e->d_name + len - 7, ".module") != 0 || e->d_name[0] == '.')
			continue;
		if (n == (int)(sizeof names / sizeof names[0]))
			break;
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof names[0], cmp_names); /* a stable order for the logs */
	int read_ = 0;
	for (int i = 0; i < n; i++) {
		char path[MF_PATH];
		mf_fmt(path, sizeof path, "%s/%s", dir, names[i]);
		struct mf_module *m = calloc(1, sizeof *m);
		if (!m)
			break;
		char err[MF_ERR];
		if (mf_parse_file(path, m, err, sizeof err) < 0) {
			/* Kept, refused, so that the module is listed as such. */
			char stem[MF_NAME + 8];
			mf_fmt(stem, sizeof stem, "%s", names[i]);
			char *dot = strrchr(stem, '.');
			if (dot)
				*dot = 0;
			memset(m, 0, sizeof *m);
			defaults(m);
			mf_fmt(m->name, sizeof m->name, "%.*s", MF_NAME - 1, stem);
			mf_fmt(m->path, sizeof m->path, "%s", path);
			m->refused = 1;
			mf_fmt(m->reason, sizeof m->reason, "%s", err);
		}
		if (mf_set_add(s, m) < 0)
			free(m);
		else
			read_++;
	}
	for (int i = 0; i < n; i++)
		free(names[i]);
	return read_;
}

void mf_set_free(struct mf_set *s)
{
	for (int i = 0; i < s->n_modules; i++)
		free(s->modules[i]);
	for (int i = 0; i < s->n_types; i++)
		free(s->types[i].schema);
	free(s->types);
	memset(s, 0, sizeof *s);
}

const struct mf_type *mf_type_at(const struct mf_set *s, int idx)
{
	return idx >= 0 && idx < s->n_types ? &s->types[idx] : NULL;
}

const struct mf_type *mf_type_find(const struct mf_set *s, const char *name)
{
	for (int i = 0; i < s->n_types; i++)
		if (strcmp(s->types[i].name, name) == 0)
			return &s->types[i];
	return NULL;
}

static int type_index(const struct mf_set *s, const char *name)
{
	for (int i = 0; i < s->n_types; i++)
		if (strcmp(s->types[i].name, name) == 0)
			return i;
	return -1;
}

/* Adds a type from schema text; -1 with err if the text does not parse. */
static int add_type(struct mf_set *s, const char *name, const char *text, size_t len, char *err,
		    size_t errlen)
{
	int i = type_index(s, name);
	if (i >= 0)
		return i;
	struct hg_schema *parsed = malloc(sizeof *parsed);
	if (!parsed)
		return -1;
	if (hg_schema_parse(text, len, parsed, err, errlen) < 0) {
		free(parsed);
		return -1;
	}
	if (s->n_types == s->cap_types) {
		int cap = s->cap_types ? s->cap_types * 2 : 64;
		struct mf_type *nt = realloc(s->types, (size_t)cap * sizeof *nt);
		if (!nt) {
			free(parsed);
			return -1;
		}
		s->types = nt;
		s->cap_types = cap;
	}
	struct mf_type *t = &s->types[s->n_types];
	memset(t, 0, sizeof *t);
	mf_fmt(t->name, sizeof t->name, "%s", name);
	t->hash = parsed->type.hash;
	t->payload_bytes = parsed->type.payload_bytes;
	t->schema = malloc(len + 1);
	if (!t->schema) {
		free(parsed);
		return -1;
	}
	memcpy(t->schema, text, len);
	t->schema[len] = 0;
	t->schema_bytes = (uint32_t)len;
	free(parsed);
	return s->n_types++;
}

/* Resolves a declared type name: MsgDir/<name>.msg, "frame" (any frame
 * type), or a generated one already in the table. -1 with a reason. */
static int resolve_type(struct mf_set *s, const char *name, char *why, size_t whylen)
{
	int i = type_index(s, name);
	if (i >= 0)
		return i;
	if (strcmp(name, "frame") == 0) {
		if (s->n_types == s->cap_types) {
			int cap = s->cap_types ? s->cap_types * 2 : 64;
			struct mf_type *nt = realloc(s->types, (size_t)cap * sizeof *nt);
			if (!nt)
				return -1;
			s->types = nt;
			s->cap_types = cap;
		}
		struct mf_type *t = &s->types[s->n_types];
		memset(t, 0, sizeof *t);
		mf_fmt(t->name, sizeof t->name, "frame");
		t->frame = 1;
		t->schema = strdup("");
		return s->n_types++;
	}
	if (strncmp(name, "dataset:", 8) == 0) {
		mf_fmt(why, whylen, "type %s: dataset specs (docs/06) are not implemented yet", name);
		return -1;
	}
	char path[MF_PATH];
	mf_fmt(path, sizeof path, "%s/%s.msg", s->msg_dir, name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		mf_fmt(why, whylen, "type %s: cannot read %s: %s", name, path, strerror(errno));
		return -1;
	}
	char text[HG_SCHEMA_MAX + 2];
	ssize_t n = read(fd, text, sizeof text);
	close(fd);
	if (n < 0 || n > HG_SCHEMA_MAX) {
		mf_fmt(why, whylen, "type %s: %s is over %d bytes", name, path, HG_SCHEMA_MAX);
		return -1;
	}
	char err[160];
	int idx = add_type(s, name, text, (size_t)n, err, sizeof err);
	if (idx < 0)
		mf_fmt(why, whylen, "type %s: %s: %s", name, path, err);
	return idx;
}

static const char *const kind_names[] = { "u8",  "i8",  "u16", "i16", "u32", "i32",
					  "u64", "i64", "f32", "f64", "char" };

/* The generated types of docs/03 "Implicit outputs": the request envelope
 * (docs/04) then u8 request[N]; the reply envelope then u8 reply[N]; the
 * parameter topic's fields (docs/05) then the parameter struct's. */
static int gen_req_type(struct mf_set *s, const char *module, uint32_t largest)
{
	char name[MF_TYPE], text[HG_SCHEMA_MAX];
	mf_fmt(name, sizeof name, "req:%s", module);
	if (largest < 8)
		largest = 8;
	int n = mf_fmt(text, sizeof text,
			 "# req:%s: the request envelope (docs/04), then the largest request\n"
			 "# among the methods %s calls\n"
			 "u64  call_id        # per caller, counting from 1\n"
			 "u64  deadline_ns    # CLOCK_MONOTONIC: when the caller stops waiting\n"
			 "u64  request_type   # type hash\n"
			 "u32  target_id      # topic id of the server's <server>.rep\n"
			 "u32  reserved       # 0\n"
			 "char method[32]\n"
			 "char caller[48]     # empty; set by the manager on hangar.<role>.admin.req only\n"
			 "u8   request[%u]\n",
			 module, module, largest);
	char err[160];
	return add_type(s, name, text, (size_t)n, err, sizeof err);
}

/* hangar.<role>.admin.req: the request envelope and 16 KB, the largest
 * request a method may take (docs/03), published by the manager. */
static int gen_admin_type(struct mf_set *s)
{
	static const char text[] =
		"# req:hangar: operators' call requests, published by the manager (docs/04 Admin calls)\n"
		"u64  call_id\n"
		"u64  deadline_ns\n"
		"u64  request_type\n"
		"u32  target_id\n"
		"u32  reserved\n"
		"char method[32]\n"
		"char caller[48]     # admin:<user>, written by the manager\n"
		"u8   request[16384]\n";
	char err[160];
	return add_type(s, "req:hangar", text, sizeof text - 1, err, sizeof err);
}

static int gen_rep_type(struct mf_set *s, const char *module, uint32_t largest)
{
	char name[MF_TYPE], text[HG_SCHEMA_MAX];
	mf_fmt(name, sizeof name, "rep:%s", module);
	if (largest < 8)
		largest = 8;
	int n = mf_fmt(text, sizeof text,
			 "# rep:%s: the reply envelope (docs/04), then the largest reply among\n"
			 "# the methods %s serves\n"
			 "u64  call_id\n"
			 "u32  caller_id      # topic id of the request topic the request was read from\n"
			 "i32  status         # HG_OK or an error (docs/04 Status codes)\n"
			 "u64  reply_type     # type hash\n"
			 "u64  received_ns    # CLOCK_MONOTONIC: when the server read the request\n"
			 "char reason[64]     # one line, when not HG_OK\n"
			 "u8   reply[%u]\n",
			 module, module, largest);
	char err[160];
	return add_type(s, name, text, (size_t)n, err, sizeof err);
}

static int gen_params_type(struct mf_set *s, const char *module, const struct mf_type *pt, char *why,
			   size_t whylen)
{
	char name[MF_TYPE];
	mf_fmt(name, sizeof name, "params:%s", module);
	struct hg_schema *parsed = malloc(sizeof *parsed);
	if (!parsed)
		return -1;
	if (hg_schema_parse(pt->schema, pt->schema_bytes, parsed, NULL, 0) < 0) {
		free(parsed);
		mf_fmt(why, whylen, "type %s does not parse", pt->name);
		return -1;
	}
	char text[HG_SCHEMA_MAX + 512];
	int n = mf_fmt(text, sizeof text,
			 "# params:%s: the parameter topic (docs/05), then the fields of %s\n"
			 "u64  version\n"
			 "char changed_by[48]  # the caller, or \"saved\" at start\n"
			 "u8   restart_pending # a change to a @restart field is saved, not in force\n"
			 "u8   reserved[7]\n",
			 module, pt->name);
	for (uint32_t i = 0; i < parsed->type.n_fields; i++) {
		const struct hg_field *f = &parsed->fields[i];
		int w;
		if (f->kind == HG_CHAR || f->count != 1)
			w = mf_fmt(text + n, sizeof text - (size_t)n, "%s %s[%u]\n", kind_names[f->kind],
				     f->name, f->count);
		else
			w = mf_fmt(text + n, sizeof text - (size_t)n, "%s %s\n", kind_names[f->kind], f->name);
		if (w < 0 || n + w >= (int)sizeof text || n + w > HG_SCHEMA_MAX) {
			free(parsed);
			mf_fmt(why, whylen, "params:%s: the generated schema exceeds %d bytes", module,
				 HG_SCHEMA_MAX);
			return -1;
		}
		n += w;
	}
	free(parsed);
	char err[160];
	int idx = add_type(s, name, text, (size_t)n, err, sizeof err);
	if (idx < 0)
		mf_fmt(why, whylen, "params:%s: %s", module, err);
	return idx;
}

/* The set_params request of a module with [Params] (docs/05 "Setting
 * parameters"): u64 base_version, then the parameter struct's fields. */
static int gen_set_params_type(struct mf_set *s, const struct mf_type *pt, char *why, size_t whylen)
{
	char name[MF_TYPE];
	mf_fmt(name, sizeof name, "set_params:%s", pt->name);
	int have = type_index(s, name);
	if (have >= 0)
		return have;
	struct hg_schema *parsed = malloc(sizeof *parsed);
	if (!parsed)
		return -1;
	if (hg_schema_parse(pt->schema, pt->schema_bytes, parsed, NULL, 0) < 0) {
		free(parsed);
		mf_fmt(why, whylen, "type %s does not parse", pt->name);
		return -1;
	}
	char text[HG_SCHEMA_MAX + 512];
	int n = mf_fmt(text, sizeof text,
			 "# set_params:%s: the set_params request (docs/05): the version the change\n"
			 "# was based on, then the fields of %s\n"
			 "u64  base_version\n",
			 pt->name, pt->name);
	for (uint32_t i = 0; i < parsed->type.n_fields; i++) {
		const struct hg_field *f = &parsed->fields[i];
		int w;
		if (f->kind == HG_CHAR || f->count != 1)
			w = mf_fmt(text + n, sizeof text - (size_t)n, "%s %s[%u]\n", kind_names[f->kind],
				     f->name, f->count);
		else
			w = mf_fmt(text + n, sizeof text - (size_t)n, "%s %s\n", kind_names[f->kind], f->name);
		if (w < 0 || n + w >= (int)sizeof text || n + w > HG_SCHEMA_MAX) {
			free(parsed);
			mf_fmt(why, whylen, "set_params:%s: the generated schema exceeds %d bytes", pt->name,
				 HG_SCHEMA_MAX);
			return -1;
		}
		n += w;
	}
	free(parsed);
	char err[160];
	int idx = add_type(s, name, text, (size_t)n, err, sizeof err);
	if (idx < 0)
		mf_fmt(why, whylen, "set_params:%s: %s", pt->name, err);
	return idx;
}

/* set_params_rep (msg/set_params_rep.msg): the same canonical form, so the
 * same hash, whether a module links the generated file or not. */
static int gen_set_params_rep(struct mf_set *s)
{
	static const char text[] =
		"# set_params_rep: the reply of every module's set_params method (docs/05)\n"
		"u64  version      # the version now in force, or, with restarting, the one the new instance starts with\n"
		"u8   restarting   # 1: a @restart field changed; the module restarts to apply it\n";
	char err[160];
	return add_type(s, "set_params_rep", text, sizeof text - 1, err, sizeof err);
}

/* ---- Checks ------------------------------------------------------------- */

static void refuse(struct mf_module *m, int line, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
static void refuse(struct mf_module *m, int line, const char *fmt, ...)
{
	if (m->refused)
		return; /* the first reason stands */
	char what[MF_ERR];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(what, sizeof what, fmt, ap);
	va_end(ap);
	m->refused = 1;
	mf_fmt(m->reason, sizeof m->reason, "%s:%d: %s", m->path, line, what);
}

static void warn(struct mf_set *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void warn(struct mf_set *s, const char *fmt, ...)
{
	if (s->n_warnings == (int)(sizeof s->warnings / sizeof s->warnings[0]))
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->warnings[s->n_warnings++], MF_ERR, fmt, ap);
	va_end(ap);
}

static int in_list(const char *name, char (*list)[MF_NAME], int n)
{
	for (int i = 0; i < n; i++)
		if (strcmp(list[i], name) == 0)
			return 1;
	return 0;
}

static const struct mf_sub *find_sub(const struct mf_module *m, const char *topic, int *by_glob)
{
	*by_glob = 0;
	for (int i = 0; i < m->n_subs; i++)
		if (!m->subs[i].glob && strcmp(m->subs[i].topic, topic) == 0)
			return &m->subs[i];
	for (int i = 0; i < m->n_subs; i++)
		if (m->subs[i].glob && hg_glob_match(m->subs[i].topic, topic)) {
			*by_glob = 1;
			return &m->subs[i];
		}
	return NULL;
}

static const struct mf_pub *find_pub(const struct mf_module *m, const char *topic)
{
	for (int i = 0; i < m->n_pubs; i++)
		if (strcmp(m->pubs[i].topic, topic) == 0)
			return &m->pubs[i];
	return NULL;
}

static int reserved_topic(const char *t)
{
	size_t n = strlen(t);
	return strncmp(t, "hangar.", 7) == 0 || (n > 4 && strcmp(t + n - 4, ".req") == 0) ||
	       (n > 4 && strcmp(t + n - 4, ".rep") == 0) || (n > 7 && strcmp(t + n - 7, ".params") == 0);
}

static int module_line(const struct mf_module *m)
{
	return m->lines[SEC_MODULE] ? m->lines[SEC_MODULE] : 1;
}

/* The rules one manifest can break by itself. */
static void check_module(struct mf_set *s, struct mf_module *m)
{
	if (strcmp(m->name, "hangar") == 0)
		refuse(m, module_line(m), "a module may not be named hangar");
	if (m->bridge)
		refuse(m, m->lines[SEC_BRIDGE],
		       "[Bridge]: the bridge (bridge.conf, mirror lines, its own topics) is not implemented in this stage");
	if (m->legacy) {
		if (m->n_pubs || m->n_subs || m->n_serves || m->n_calls || m->has_params)
			refuse(m, module_line(m), "Legacy=yes: a legacy module declares no topics, calls or parameters");
		if (m->n_excl && !m->has_module_neutral)
			refuse(m, module_line(m), "ExclusiveGroups needs a [Module] Neutral");
		if (m->has_actuator)
			refuse(m, m->lines[SEC_ACTUATOR], "[Actuator]: not allowed with Legacy=yes");
	} else {
		if (m->has_module_neutral)
			refuse(m, module_line(m), "[Module] Neutral is only for Legacy=yes");
		if (m->n_excl)
			refuse(m, module_line(m), "[Module] ExclusiveGroups is only for Legacy=yes");
	}
	for (int i = 0; i < m->n_excl; i++)
		if (!in_list(m->excl[i], m->groups, m->n_groups))
			refuse(m, module_line(m), "ExclusiveGroups: %s is not one of the module's SupplementaryGroups",
			       m->excl[i]);
	if (m->privileged) {
		if (strcmp(m->name, s->privileged_module) != 0)
			refuse(m, module_line(m), "Privileged=yes: only the module PrivilegedModule= names in hangard.conf (%s) may be privileged",
			       s->privileged_module[0] ? s->privileged_module : "none");
		if (m->user_set || m->group_set || m->n_groups || m->n_caps)
			refuse(m, module_line(m), "Privileged=yes: no User, Group, SupplementaryGroups or Capabilities");
		if (m->n_pubs)
			refuse(m, m->pubs[0].line, "Privileged=yes: no [Publish]");
		if (m->n_subs)
			refuse(m, m->subs[0].line, "Privileged=yes: no [Subscribe]");
		if (m->n_calls)
			refuse(m, m->calls[0].line, "Privileged=yes: no [Call]");
		if (m->has_params)
			refuse(m, m->lines[SEC_PARAMS], "Privileged=yes: no [Params]");
		for (int i = 0; i < m->n_serves; i++)
			if (strcmp(m->serves[i].method, "apply") != 0 && strcmp(m->serves[i].method, "rollback") != 0)
				refuse(m, m->serves[i].line, "Privileged=yes: serves only apply and rollback, not %s",
				       m->serves[i].method);
	}
	/* Outputs. */
	for (int i = 0; i < m->n_pubs; i++) {
		struct mf_pub *p = &m->pubs[i];
		if (reserved_topic(p->topic))
			refuse(m, p->line, "[Publish %s]: names starting with hangar. or ending in .req, .rep or .params are reserved",
			       p->topic);
		for (int k = 0; k < p->n_inputs; k++) {
			const char *in = p->inputs[k];
			const char *colon = strchr(in, ':');
			if (colon) {
				if (strncmp(in, s->other_role, (size_t)(colon - in)) != 0 ||
				    strlen(s->other_role) != (size_t)(colon - in))
					refuse(m, p->line, "Inputs: %s names this machine's own role; a topic of the other side is %s:<topic>",
					       in, s->other_role);
				continue;
			}
			int by_glob;
			if (find_sub(m, in, &by_glob)) {
				if (by_glob)
					refuse(m, p->line, "Inputs: %s is subscribed to only by glob", in);
			} else if (!find_pub(m, in)) {
				refuse(m, p->line, "Inputs: %s is neither a subscription nor an output of this module", in);
			}
		}
		for (int k = 0; k < p->n_sources; k++) {
			int by_glob;
			if (!find_sub(m, p->sources[k], &by_glob))
				refuse(m, p->line, "Sources: %s is not a subscription of this module", p->sources[k]);
			else if (by_glob)
				refuse(m, p->line, "Sources: %s is subscribed to only by glob", p->sources[k]);
		}
		if (p->actuator) {
			if (!m->user_set || strcmp(m->user, "hangar") == 0)
				refuse(m, p->line, "Actuator=yes: a dedicated User (not hangar) is required");
			if (!m->has_actuator)
				refuse(m, p->line, "Actuator=yes: an [Actuator] section is required");
		}
	}
	if (m->has_actuator && !m->has_actuator_output)
		refuse(m, m->lines[SEC_ACTUATOR], "[Actuator]: only for a module with an actuator output (Actuator=yes)");
	if (m->has_actuator) {
		int g;
		if (!find_sub(m, m->act_command, &g) || g)
			refuse(m, m->lines[SEC_ACTUATOR], "[Actuator] Command=%s is not one of the module's subscriptions by name",
			       m->act_command);
		if (!find_sub(m, "safety.state", &g) || g)
			refuse(m, m->lines[SEC_ACTUATOR], "an actuator module must subscribe to safety.state");
	}
	for (int i = 0; i < m->n_serves; i++) {
		if (strcmp(m->serves[i].method, "set_params") == 0)
			refuse(m, m->serves[i].line, "[Serve set_params]: reserved; declare [Params] instead");
		for (int k = 0; k < m->serves[i].n_allow; k++)
			if (!mf_valid_allow_token(m->serves[i].allow[k], s->role))
				refuse(m, m->serves[i].line, "Allow: '%s' is not a caller token for a %s",
				       m->serves[i].allow[k], s->role);
	}
	for (int k = 0; k < m->n_params_allow; k++)
		if (!mf_valid_allow_token(m->params_allow[k], s->role))
			refuse(m, m->lines[SEC_PARAMS], "Allow: '%s' is not a caller token for a %s",
			       m->params_allow[k], s->role);
	/* Types. */
	char why[MF_ERR];
	for (int i = 0; i < m->n_pubs && !m->refused; i++) {
		struct mf_pub *p = &m->pubs[i];
		p->type_idx = resolve_type(s, p->type, why, sizeof why);
		if (p->type_idx < 0)
			refuse(m, p->line, "%s", why);
	}
	for (int i = 0; i < m->n_serves && !m->refused; i++) {
		struct mf_serve *sv = &m->serves[i];
		if ((sv->req_idx = resolve_type(s, sv->req, why, sizeof why)) < 0 ||
		    (sv->rep_idx = resolve_type(s, sv->rep, why, sizeof why)) < 0)
			refuse(m, sv->line, "%s", why);
		else if (s->types[sv->req_idx].frame || s->types[sv->rep_idx].frame)
			refuse(m, sv->line, "a request or reply cannot be a frame");
		else if (s->types[sv->req_idx].payload_bytes > 16384 || s->types[sv->rep_idx].payload_bytes > 16384)
			refuse(m, sv->line, "requests and replies are at most 16 KB");
	}
	for (int i = 0; i < m->n_calls && !m->refused; i++) {
		struct mf_call *c = &m->calls[i];
		if (c->req[0]) {
			if ((c->req_idx = resolve_type(s, c->req, why, sizeof why)) < 0 ||
			    (c->rep_idx = resolve_type(s, c->rep, why, sizeof why)) < 0)
				refuse(m, c->line, "%s", why);
		}
	}
	if (m->has_params && !m->refused) {
		m->params_idx = resolve_type(s, m->params_type, why, sizeof why);
		if (m->params_idx < 0)
			refuse(m, m->lines[SEC_PARAMS], "%s", why);
	}
	for (int i = 0; i < m->n_subs && !m->refused; i++) {
		struct mf_sub *sub = &m->subs[i];
		if (sub->type[0] && resolve_type(s, sub->type, why, sizeof why) < 0)
			refuse(m, sub->line, "%s", why);
	}
	/* [Params] serves set_params like any other method (docs/05): the
	 * implicit serve line, its types generated, its Allow the section's, or
	 * the default "* admin", none for safety, mux and an actuator module. */
	if (m->has_params && !m->refused) {
		if (m->n_serves == MF_MAX_SERVES) {
			refuse(m, m->lines[SEC_PARAMS], "more than %d methods with set_params", MF_MAX_SERVES);
			return;
		}
		struct mf_serve *sv = &m->serves[m->n_serves];
		memset(sv, 0, sizeof *sv);
		mf_fmt(sv->method, sizeof sv->method, "set_params");
		mf_fmt(sv->req, sizeof sv->req, "set_params:%s", m->params_type);
		mf_fmt(sv->rep, sizeof sv->rep, "set_params_rep");
		sv->deadline_ns = 100000000;
		sv->implicit = 1;
		sv->line = m->lines[SEC_PARAMS];
		sv->allow_set = 1;
		if (m->params_allow_set) {
			sv->n_allow = m->n_params_allow;
			memcpy(sv->allow, m->params_allow, sizeof sv->allow);
		} else if (!m->has_actuator_output && strcmp(m->name, "safety") != 0 && strcmp(m->name, "mux") != 0) {
			sv->n_allow = 2;
			mf_fmt(sv->allow[0], MF_REF, "*");
			mf_fmt(sv->allow[1], MF_REF, "admin");
		}
		sv->req_idx = gen_set_params_type(s, &s->types[m->params_idx], why, sizeof why);
		sv->rep_idx = gen_set_params_rep(s);
		if (sv->req_idx < 0 || sv->rep_idx < 0) {
			refuse(m, m->lines[SEC_PARAMS], "%s", sv->req_idx < 0 ? why : "cannot generate set_params_rep");
			return;
		}
		m->n_serves++;
	}
}

/* Adds an implicit pub/sub; refuses when the module is full. */
static struct mf_pub *add_pub(struct mf_module *m, const char *topic, int type_idx, uint32_t slots)
{
	if (m->n_pubs == MF_MAX_PUBS) {
		refuse(m, module_line(m), "more than %d outputs with the implicit ones", MF_MAX_PUBS);
		return NULL;
	}
	struct mf_pub *p = &m->pubs[m->n_pubs++];
	memset(p, 0, sizeof *p);
	mf_fmt(p->topic, sizeof p->topic, "%s", topic);
	p->slots = slots;
	p->implicit = 1;
	p->type_idx = type_idx;
	p->line = module_line(m);
	return p;
}

static struct mf_sub *add_sub(struct mf_module *m, const char *topic, int glob)
{
	for (int i = 0; i < m->n_subs; i++)
		if (strcmp(m->subs[i].topic, topic) == 0)
			return &m->subs[i];
	if (m->n_subs == MF_MAX_SUBS) {
		refuse(m, module_line(m), "more than %d subscriptions with the implicit ones", MF_MAX_SUBS);
		return NULL;
	}
	struct mf_sub *sub = &m->subs[m->n_subs++];
	memset(sub, 0, sizeof *sub);
	mf_fmt(sub->topic, sizeof sub->topic, "%s", topic);
	sub->glob = glob;
	sub->implicit = 1;
	sub->line = module_line(m);
	return sub;
}

static struct mf_module *server_of(const struct mf_set *s, const char *module, const char *method,
				   const struct mf_serve **serve)
{
	struct mf_module *sv = mf_find(s, module);
	if (!sv || sv->refused)
		return NULL;
	for (int i = 0; i < sv->n_serves; i++)
		if (strcmp(sv->serves[i].method, method) == 0) {
			*serve = &sv->serves[i];
			return sv;
		}
	return NULL;
}

static int allow_names(const struct mf_serve *sv, const struct mf_module *server, const char *caller)
{
	if (!sv->allow_set) {
		/* "* admin" unless the server has an actuator output: then none. */
		return !server->has_actuator_output;
	}
	for (int i = 0; i < sv->n_allow; i++)
		if (strcmp(sv->allow[i], "*") == 0 || strcmp(sv->allow[i], caller) == 0)
			return 1;
	return 0;
}

/* Pass 2: calls, the implicit topics and subscriptions. Needs every
 * module's declared types resolved. */
static void derive_implicit(struct mf_set *s, struct mf_module *m)
{
	char admin_req[MF_TOPIC];
	mf_fmt(admin_req, sizeof admin_req, "hangar.%s.admin.req", s->role);
	/* Calls: the server's types, or the [Call]'s own for a remote method. */
	uint32_t largest_req = 0;
	for (int i = 0; i < m->n_calls; i++) {
		struct mf_call *c = &m->calls[i];
		const struct mf_serve *sv;
		struct mf_module *server = server_of(s, c->server, c->method, &sv);
		if (server) {
			if (c->req[0] && (c->req_idx != sv->req_idx || c->rep_idx != sv->rep_idx))
				refuse(m, c->line, "[Call %s.%s]: Request=/Reply= differ from the server's (%s, %s)",
				       c->server, c->method, sv->req, sv->rep);
			c->req_idx = sv->req_idx;
			c->rep_idx = sv->rep_idx;
			mf_fmt(c->req, sizeof c->req, "%s", sv->req);
			mf_fmt(c->rep, sizeof c->rep, "%s", sv->rep);
			c->remote = 0;
			if (!allow_names(sv, server, m->name))
				refuse(m, c->line, "[Call %s.%s]: %s is not in the method's Allow list", c->server,
				       c->method, m->name);
		} else {
			if (!c->req[0]) {
				refuse(m, c->line, "[Call %s.%s]: no module enabled here serves it; a method of the other side needs Request= and Reply=",
				       c->server, c->method);
				continue;
			}
			/* Without the bridge (not in this stage) nothing routes it: the
			 * call line says so, and the library answers HG_E_NOROUTE. */
			c->remote = 0;
			c->noroute = 1;
			warn(s, "%s: [Call %s.%s]: no module enabled here serves it, and no bridge routes it: HG_E_NOROUTE",
			     m->name, c->server, c->method);
		}
		if (c->req_idx >= 0 && s->types[c->req_idx].payload_bytes > largest_req)
			largest_req = s->types[c->req_idx].payload_bytes;
	}
	if (m->refused)
		return;
	char topic[MF_TOPIC];
	if (m->n_calls) {
		int t = gen_req_type(s, m->name, largest_req);
		mf_fmt(topic, sizeof topic, "%s.req", m->name);
		if (t < 0 || !add_pub(m, topic, t, 16))
			refuse(m, module_line(m), "cannot generate req:%s", m->name);
		for (int i = 0; i < m->n_calls; i++) {
			mf_fmt(topic, sizeof topic, "%s.rep", m->calls[i].server);
			add_sub(m, topic, 0);
		}
	}
	if (m->n_serves || m->has_params) {
		uint32_t largest_rep = 8; /* set_params_rep, 16 bytes, is among the serves when [Params] */
		for (int i = 0; i < m->n_serves; i++)
			if (s->types[m->serves[i].rep_idx].payload_bytes > largest_rep)
				largest_rep = s->types[m->serves[i].rep_idx].payload_bytes;
		int t = gen_rep_type(s, m->name, largest_rep);
		mf_fmt(topic, sizeof topic, "%s.rep", m->name);
		if (t < 0 || !add_pub(m, topic, t, 16))
			refuse(m, module_line(m), "cannot generate rep:%s", m->name);
		/* Every local caller's request topic, and the operators'. */
		for (int k = 0; k < s->n_modules; k++) {
			struct mf_module *o = s->modules[k];
			if (o->refused)
				continue;
			for (int i = 0; i < o->n_calls; i++)
				if (strcmp(o->calls[i].server, m->name) == 0) {
					mf_fmt(topic, sizeof topic, "%s.req", o->name);
					add_sub(m, topic, 0);
					break;
				}
		}
		add_sub(m, admin_req, 0);
	}
	if (m->has_params) {
		char why[MF_ERR];
		int t = gen_params_type(s, m->name, &s->types[m->params_idx], why, sizeof why);
		mf_fmt(topic, sizeof topic, "%s.params", m->name);
		if (t < 0)
			refuse(m, m->lines[SEC_PARAMS], "%s", why);
		else
			add_pub(m, topic, t, 16);
	}
	if (m->privileged)
		add_sub(m, "safety.state", 0);
	/* The request topics a server reads are Sources of every output it has
	 * (docs/01 "Calls"), the .rep topic's among them. */
	if (m->n_serves || m->has_params) {
		for (int i = 0; i < m->n_pubs && !m->refused; i++) {
			struct mf_pub *p = &m->pubs[i];
			for (int k = 0; k < m->n_subs; k++) {
				const struct mf_sub *sub = &m->subs[k];
				size_t n = strlen(sub->topic);
				if (!sub->implicit || n < 5 || strcmp(sub->topic + n - 4, ".req") != 0)
					continue;
				int have = 0;
				for (int j = 0; j < p->n_sources; j++)
					if (strcmp(p->sources[j], sub->topic) == 0)
						have = 1;
				if (have)
					continue;
				if (p->n_sources == MF_MAX_SOURCES) {
					refuse(m, p->line, "[Publish %s]: more than %d Sources with the implicit request topics",
					       p->topic, MF_MAX_SOURCES);
					break;
				}
				mf_fmt(p->sources[p->n_sources++], MF_TOPIC, "%s", sub->topic);
			}
		}
	}
	/* The 127 limit, implicit subscriptions counted, globs not. */
	int named = 0;
	for (int i = 0; i < m->n_subs; i++)
		if (!m->subs[i].glob)
			named++;
	if (named > HG_WIRING_MAX_SUBS)
		refuse(m, module_line(m), "%d subscriptions by name, the implicit .req and .rep ones counted; one wait holds %d",
		       named, HG_WIRING_MAX_SUBS);
}

/* Pass 3: between modules. */
static void check_set(struct mf_set *s)
{
	static struct {
		char topic[MF_TOPIC];
		uint32_t id;
		struct mf_module *m;
		const struct mf_pub *p;
	} all[MF_MAX_MODULES * MF_MAX_PUBS + 4];
	int n = 0;
	/* The manager's own topics are in the namespace too. */
	const char *mine[] = { "system", "modules", "admin.req" };
	for (int i = 0; i < 3; i++) {
		mf_fmt(all[n].topic, MF_TOPIC, "hangar.%s.%s", s->role, mine[i]);
		all[n].id = hg_topic_id(all[n].topic);
		all[n].m = NULL;
		all[n].p = NULL;
		n++;
	}
	for (int k = 0; k < s->n_modules; k++) {
		struct mf_module *m = s->modules[k];
		if (m->refused)
			continue;
		for (int i = 0; i < m->n_pubs; i++) {
			const struct mf_pub *p = &m->pubs[i];
			for (int j = 0; j < n; j++) {
				if (strcmp(all[j].topic, p->topic) == 0) {
					if (all[j].m) {
						refuse(all[j].m, all[j].p->line, "%s is also published by %s (%s:%d); neither is started",
						       p->topic, m->name, m->path, p->line);
						refuse(m, p->line, "%s is also published by %s (%s:%d); neither is started",
						       p->topic, all[j].m->name, all[j].m->path, all[j].p->line);
					} else {
						refuse(m, p->line, "%s is the manager's own topic", p->topic);
					}
				} else if (all[j].id == hg_topic_id(p->topic)) {
					refuse(m, p->line, "%s has the same topic id (%08x) as %s%s%s", p->topic,
					       all[j].id, all[j].topic, all[j].m ? " of " : "",
					       all[j].m ? all[j].m->name : "");
				}
			}
			mf_fmt(all[n].topic, MF_TOPIC, "%s", p->topic);
			all[n].id = hg_topic_id(p->topic);
			all[n].m = m;
			all[n].p = p;
			n++;
		}
	}
	/* Subscriptions: the publisher's type, carried into the wiring. */
	for (int k = 0; k < s->n_modules; k++) {
		struct mf_module *m = s->modules[k];
		for (int i = 0; i < m->n_subs; i++) {
			struct mf_sub *sub = &m->subs[i];
			if (sub->glob)
				continue;
			struct mf_module *pm = NULL;
			const struct mf_pub *pp = NULL;
			for (int j = 3; j < n; j++)
				if (strcmp(all[j].topic, sub->topic) == 0 && !all[j].m->refused) {
					pm = all[j].m;
					pp = all[j].p;
				}
			if (pp) {
				const struct mf_type *t = &s->types[pp->type_idx];
				mf_fmt(sub->pub_type, sizeof sub->pub_type, "%s", t->name);
				sub->hash = t->hash;
				sub->rate_mhz = pp->rate_mhz;
				sub->slots = pp->slots;
				if (sub->type[0] && strcmp(sub->type, t->name) != 0 && !m->refused)
					refuse(m, sub->line, "[Subscribe %s] Type=%s, but %s publishes it as %s",
					       sub->topic, sub->type, pm->name, t->name);
			} else if (sub->type[0]) {
				mf_fmt(sub->pub_type, sizeof sub->pub_type, "%s", sub->type);
				const struct mf_type *t = mf_type_find(s, sub->type);
				sub->hash = t ? t->hash : 0;
			} else if (strcmp(sub->topic, all[2].topic) == 0) {
				/* The manager's own request topic. */
				const struct mf_type *t = mf_type_find(s, "req:hangar");
				mf_fmt(sub->pub_type, sizeof sub->pub_type, "req:hangar");
				sub->hash = t ? t->hash : 0;
				sub->slots = 16;
			} else if (!m->refused && !sub->implicit) {
				warn(s, "%s: [Subscribe %s]: no enabled module publishes it", m->name, sub->topic);
			}
		}
	}
	/* Users and groups. */
	int n_priv = 0;
	for (int k = 0; k < s->n_modules; k++)
		if (!s->modules[k]->refused && s->modules[k]->privileged)
			n_priv++;
	for (int k = 0; k < s->n_modules; k++) {
		struct mf_module *m = s->modules[k];
		if (m->refused)
			continue;
		if (m->privileged && n_priv > 1)
			refuse(m, module_line(m), "Privileged=yes: two manifests have it");
		for (int j = 0; j < s->n_modules; j++) {
			struct mf_module *o = s->modules[j];
			if (o == m || o->refused)
				continue;
			if (m->has_actuator_output && strcmp(m->user, o->user) == 0)
				refuse(m, module_line(m), "an actuator module's user %s is shared with %s", m->user, o->name);
			for (int g = 0; g < m->n_act_groups; g++)
				if (in_list(m->act_groups[g], o->groups, o->n_groups) ||
				    strcmp(o->group, m->act_groups[g]) == 0)
					refuse(o, module_line(o), "%s is in group %s, which [Actuator] Groups of %s reserves",
					       o->name, m->act_groups[g], m->name);
			for (int g = 0; g < m->n_excl; g++)
				if (in_list(m->excl[g], o->groups, o->n_groups) || strcmp(o->group, m->excl[g]) == 0)
					refuse(o, module_line(o), "%s is in group %s, which ExclusiveGroups of %s reserves",
					       o->name, m->excl[g], m->name);
		}
	}
}

int mf_set_check(struct mf_set *s)
{
	s->n_warnings = 0;
	gen_admin_type(s);
	for (int k = 0; k < s->n_modules; k++)
		if (!s->modules[k]->refused)
			check_module(s, s->modules[k]);
	for (int k = 0; k < s->n_modules; k++)
		if (!s->modules[k]->refused)
			derive_implicit(s, s->modules[k]);
	check_set(s);
	int ok = 0;
	for (int k = 0; k < s->n_modules; k++)
		if (!s->modules[k]->refused)
			ok++;
	return ok;
}
