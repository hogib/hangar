// SPDX-License-Identifier: LGPL-3.0-or-later
/* Datasets: the spec parser, the row type and schema, Where, the join and
 * the reset rule of hg_dataset.h. */
#define _GNU_SOURCE
#include "hg_dataset.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hg_ini.h"
#include "hg_record.h"
#include "hg_wiring.h"

const char *hg_ds_role_name(enum hg_ds_role r)
{
	switch (r) {
	case HG_DS_INPUT: return "input";
	case HG_DS_LABEL: return "label";
	case HG_DS_FILTER: return "filter";
	}
	return "?";
}

static void seterr(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
	if (!err || !errlen)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

/* ---- Where: syntax ---------------------------------------------------------- */

enum tok_kind { T_END, T_IDENT, T_NUM, T_CMP, T_AND, T_OR, T_LP, T_RP, T_BAD };

struct tok {
	enum tok_kind kind;
	const char *s;
	size_t n;
	enum hg_ds_cmp cmp;
	double num;
};

static int ident_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

static void next_tok(const char **pp, struct tok *t)
{
	const char *p = *pp;
	while (*p == ' ' || *p == '\t')
		p++;
	t->s = p;
	t->n = 0;
	if (*p == 0) {
		t->kind = T_END;
	} else if (*p == '(') {
		t->kind = T_LP;
		p++;
	} else if (*p == ')') {
		t->kind = T_RP;
		p++;
	} else if (*p == '=' && p[1] == '=') {
		t->kind = T_CMP, t->cmp = HG_DS_EQ, p += 2;
	} else if (*p == '!' && p[1] == '=') {
		t->kind = T_CMP, t->cmp = HG_DS_NE, p += 2;
	} else if (*p == '<' && p[1] == '=') {
		t->kind = T_CMP, t->cmp = HG_DS_LE, p += 2;
	} else if (*p == '>' && p[1] == '=') {
		t->kind = T_CMP, t->cmp = HG_DS_GE, p += 2;
	} else if (*p == '<') {
		t->kind = T_CMP, t->cmp = HG_DS_LT, p++;
	} else if (*p == '>') {
		t->kind = T_CMP, t->cmp = HG_DS_GT, p++;
	} else if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+') {
		char *e;
		t->num = strtod(p, &e);
		if (e == p || (*e && *e != ' ' && *e != '\t' && *e != ')')) {
			t->kind = T_BAD;
			return;
		}
		t->kind = T_NUM;
		p = e;
	} else if (*p >= 'a' && *p <= 'z') {
		const char *s = p;
		while (ident_char(*p))
			p++;
		t->n = (size_t)(p - s);
		if (t->n == 3 && memcmp(s, "and", 3) == 0)
			t->kind = T_AND;
		else if (t->n == 2 && memcmp(s, "or", 2) == 0)
			t->kind = T_OR;
		else
			t->kind = T_IDENT;
	} else {
		t->kind = T_BAD;
		return;
	}
	*pp = p;
}

/* Resolves an identifier of Where to a row field: 0, or -1 with the reason. */
typedef int (*resolve_fn)(void *ctx, const char *name, size_t n, struct hg_ds_where_node *node, char *why,
			  size_t whylen);

struct wcomp {
	const char *p;
	struct tok t;
	struct hg_ds_where_node *out;
	int n, cap;
	resolve_fn resolve;
	void *ctx;
	char why[160];
	int failed;
};

static void advance(struct wcomp *c)
{
	next_tok(&c->p, &c->t);
}

static int emit(struct wcomp *c, const struct hg_ds_where_node *n)
{
	if (c->n == c->cap) {
		snprintf(c->why, sizeof c->why, "Where: more than %d terms", c->cap);
		return -1;
	}
	if (c->out)
		c->out[c->n] = *n;
	c->n++;
	return 0;
}

static int parse_or(struct wcomp *c);

static int parse_cmp(struct wcomp *c)
{
	if (c->t.kind == T_LP) {
		advance(c);
		if (parse_or(c) < 0)
			return -1;
		if (c->t.kind != T_RP) {
			snprintf(c->why, sizeof c->why, "Where: expected ')'");
			return -1;
		}
		advance(c);
		return 0;
	}
	if (c->t.kind != T_IDENT) {
		snprintf(c->why, sizeof c->why, "Where: expected <part>.<field>");
		return -1;
	}
	struct hg_ds_where_node node = { .op = HG_DS_OP_CMP, .part = -1 };
	if (c->resolve && c->resolve(c->ctx, c->t.s, c->t.n, &node, c->why, sizeof c->why) < 0)
		return -1;
	advance(c);
	if (c->t.kind != T_CMP) {
		snprintf(c->why, sizeof c->why, "Where: expected a comparison (== != < <= > >=)");
		return -1;
	}
	node.cmp = c->t.cmp;
	advance(c);
	if (c->t.kind != T_NUM) {
		snprintf(c->why, sizeof c->why, "Where: expected a number");
		return -1;
	}
	node.value = c->t.num;
	advance(c);
	return emit(c, &node);
}

static int parse_and(struct wcomp *c)
{
	if (parse_cmp(c) < 0)
		return -1;
	while (c->t.kind == T_AND) {
		advance(c);
		if (parse_cmp(c) < 0)
			return -1;
		struct hg_ds_where_node n = { .op = HG_DS_OP_AND };
		if (emit(c, &n) < 0)
			return -1;
	}
	return 0;
}

static int parse_or(struct wcomp *c)
{
	if (parse_and(c) < 0)
		return -1;
	while (c->t.kind == T_OR) {
		advance(c);
		if (parse_and(c) < 0)
			return -1;
		struct hg_ds_where_node n = { .op = HG_DS_OP_OR };
		if (emit(c, &n) < 0)
			return -1;
	}
	return 0;
}

/* Compiles text into out (postfix); resolve NULL checks the syntax alone.
 * The node count, or -1 with the reason. */
static int where_compile(const char *text, struct hg_ds_where_node *out, int cap, resolve_fn resolve, void *ctx,
			 char *why, size_t whylen)
{
	struct wcomp c = { .p = text, .out = out, .cap = cap, .resolve = resolve, .ctx = ctx };
	advance(&c);
	if (c.t.kind == T_END) {
		snprintf(why, whylen, "Where: empty");
		return -1;
	}
	if (parse_or(&c) < 0 || c.t.kind != T_END) {
		if (!c.why[0])
			snprintf(c.why, sizeof c.why, c.t.kind == T_BAD ? "Where: bad token" : "Where: unexpected text");
		snprintf(why, whylen, "%s", c.why);
		return -1;
	}
	return c.n;
}

/* ---- The spec --------------------------------------------------------------- */

struct pstate {
	struct hg_ds_spec *s;
	int section;        /* 0 none, 1 Dataset, 2 Input */
	int dataset_seen;
	uint32_t seen;      /* keys of the current section */
	char *err;
	size_t errlen;
};

static int split_fields(const char *v, char (*out)[HG_FIELD_NAME_MAX + 1], int cap, char *why, size_t whylen)
{
	int n = 0;
	const char *p = v;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *s = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		size_t len = (size_t)(p - s);
		if (len > HG_FIELD_NAME_MAX) {
			snprintf(why, whylen, "field name too long");
			return -1;
		}
		if (n == cap) {
			snprintf(why, whylen, "more than %d fields", cap);
			return -1;
		}
		for (size_t i = 0; i < len; i++)
			if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
				snprintf(why, whylen, "bad field name '%.*s'", (int)len, s);
				return -1;
			}
		for (int i = 0; i < n; i++)
			if (strlen(out[i]) == len && memcmp(out[i], s, len) == 0) {
				snprintf(why, whylen, "field '%.*s' listed twice", (int)len, s);
				return -1;
			}
		memcpy(out[n], s, len);
		out[n][len] = 0;
		n++;
	}
	if (n == 0) {
		snprintf(why, whylen, "Fields= is empty");
		return -1;
	}
	return n;
}

static int parse_role(const char *v, enum hg_ds_role *out)
{
	if (strcmp(v, "input") == 0)
		*out = HG_DS_INPUT;
	else if (strcmp(v, "label") == 0)
		*out = HG_DS_LABEL;
	else if (strcmp(v, "filter") == 0)
		*out = HG_DS_FILTER;
	else
		return -1;
	return 0;
}

/* A signed duration: "-50ms", "0", "100ms". */
static int parse_signed_duration(const char *v, int64_t *out)
{
	int neg = 0;
	if (*v == '-') {
		neg = 1;
		v++;
	} else if (*v == '+') {
		v++;
	}
	int64_t d = hg_ini_parse_duration(v);
	if (d < 0)
		return -1;
	*out = neg ? -d : d;
	return 0;
}

static int on_section(void *ctx, const char *section, const char *arg, int line, char *why, size_t whylen)
{
	struct pstate *p = ctx;
	struct hg_ds_spec *s = p->s;
	p->seen = 0;
	if (strcmp(section, "Dataset") == 0) {
		if (arg[0]) {
			snprintf(why, whylen, "[Dataset] takes no argument");
			return -1;
		}
		if (p->dataset_seen) {
			snprintf(why, whylen, "[Dataset] given twice");
			return -1;
		}
		p->dataset_seen = 1;
		p->section = 1;
		return 0;
	}
	if (strcmp(section, "Input") == 0) {
		if (!arg[0] || !hg_valid_name(arg)) {
			snprintf(why, whylen, "[Input <part>] needs a part name (a topic name)");
			return -1;
		}
		for (int i = 0; i < s->n_parts; i++)
			if (strcmp(s->parts[i].name, arg) == 0) {
				snprintf(why, whylen, "part %s given twice", arg);
				return -1;
			}
		if (s->n_parts == HG_DS_MAX_PARTS) {
			snprintf(why, whylen, "more than %d parts", HG_DS_MAX_PARTS);
			return -1;
		}
		struct hg_ds_part_spec *part = &s->parts[s->n_parts++];
		memset(part, 0, sizeof *part);
		snprintf(part->name, sizeof part->name, "%s", arg);
		snprintf(part->topic, sizeof part->topic, "%s", arg);
		part->window = 1;
		part->role = HG_DS_INPUT;
		part->join = -1;
		part->line = line;
		p->section = 2;
		return 0;
	}
	snprintf(why, whylen, "unknown section [%s]", section);
	return -1;
}

enum { K_ANCHOR, K_ANCHORROLE, K_FIELDS, K_RATE, K_WHERE, K_FILTER, K_MISSING, K_RESETAFTER, K_LATENESS,
       K_TOPIC, K_JOIN, K_WINDOW, K_ROLE };

static int seen_once(struct pstate *p, int k, const char *key, char *why, size_t whylen)
{
	if (p->seen & (1u << k)) {
		snprintf(why, whylen, "%s given twice", key);
		return -1;
	}
	p->seen |= 1u << k;
	return 0;
}

static int on_key(void *ctx, const char *key, const char *value, int line, char *why, size_t whylen)
{
	struct pstate *p = ctx;
	struct hg_ds_spec *s = p->s;
	(void)line;
	if (p->section == 1) {
		if (strcmp(key, "Anchor") == 0) {
			if (seen_once(p, K_ANCHOR, key, why, whylen) < 0)
				return -1;
			if (!hg_valid_name(value)) {
				snprintf(why, whylen, "Anchor: '%s' is not a topic name", value);
				return -1;
			}
			snprintf(s->anchor, sizeof s->anchor, "%s", value);
			return 0;
		}
		if (strcmp(key, "AnchorRole") == 0) {
			if (seen_once(p, K_ANCHORROLE, key, why, whylen) < 0)
				return -1;
			if (parse_role(value, &s->anchor_role) < 0 || s->anchor_role == HG_DS_FILTER) {
				snprintf(why, whylen, "AnchorRole: label or input, not '%s'", value);
				return -1;
			}
			return 0;
		}
		if (strcmp(key, "Fields") == 0) {
			if (seen_once(p, K_FIELDS, key, why, whylen) < 0)
				return -1;
			int n = split_fields(value, s->anchor_fields, HG_DS_MAX_SELECT, why, whylen);
			if (n < 0)
				return -1;
			s->n_anchor_fields = n;
			return 0;
		}
		if (strcmp(key, "Rate") == 0) {
			if (seen_once(p, K_RATE, key, why, whylen) < 0)
				return -1;
			if (strncmp(value, "every:", 6) == 0) {
				char *e;
				unsigned long long n = strtoull(value + 6, &e, 10);
				if (e == value + 6 || *e || n == 0) {
					snprintf(why, whylen, "Rate: every:N needs N from 1");
					return -1;
				}
				s->rate_kind = HG_DS_EVERY;
				s->rate_every = n;
				return 0;
			}
			if (strncmp(value, "rate:", 5) == 0) {
				int64_t mhz = hg_parse_rate_mhz(value + 5);
				if (mhz <= 0 || mhz > 0xffffffffll) {
					snprintf(why, whylen, "Rate: rate:<Hz> needs a rate above 0");
					return -1;
				}
				s->rate_kind = HG_DS_RATE_HZ;
				s->rate_mhz = (uint32_t)mhz;
				return 0;
			}
			snprintf(why, whylen, "Rate: every:N or rate:<Hz>, not '%s'", value);
			return -1;
		}
		if (strcmp(key, "Where") == 0) {
			if (seen_once(p, K_WHERE, key, why, whylen) < 0)
				return -1;
			if (strlen(value) >= sizeof s->where) {
				snprintf(why, whylen, "Where: longer than %d bytes", HG_DS_WHERE_TEXT - 1);
				return -1;
			}
			char w[160];
			if (where_compile(value, NULL, HG_DS_WHERE_MAX, NULL, NULL, w, sizeof w) < 0) {
				snprintf(why, whylen, "%s", w);
				return -1;
			}
			snprintf(s->where, sizeof s->where, "%s", value);
			return 0;
		}
		if (strcmp(key, "Filter") == 0) {
			if (seen_once(p, K_FILTER, key, why, whylen) < 0)
				return -1;
			if (strcmp(value, "drop") == 0)
				s->filter = HG_DS_DROP;
			else if (strcmp(value, "mask") == 0)
				s->filter = HG_DS_MASK;
			else {
				snprintf(why, whylen, "Filter: drop or mask, not '%s'", value);
				return -1;
			}
			return 0;
		}
		if (strcmp(key, "Missing") == 0) {
			if (seen_once(p, K_MISSING, key, why, whylen) < 0)
				return -1;
			if (strcmp(value, "flag") == 0)
				s->missing_drop = 0;
			else if (strcmp(value, "drop") == 0)
				s->missing_drop = 1;
			else {
				snprintf(why, whylen, "Missing: flag or drop, not '%s'", value);
				return -1;
			}
			return 0;
		}
		if (strcmp(key, "ResetAfter") == 0) {
			if (seen_once(p, K_RESETAFTER, key, why, whylen) < 0)
				return -1;
			int64_t d = hg_ini_parse_duration(value);
			if (d < 0) {
				snprintf(why, whylen, "ResetAfter: '%s' is not a duration", value);
				return -1;
			}
			s->reset_after_ns = d;
			return 0;
		}
		if (strcmp(key, "Lateness") == 0) {
			if (seen_once(p, K_LATENESS, key, why, whylen) < 0)
				return -1;
			int64_t d = hg_ini_parse_duration(value);
			if (d < 0) {
				snprintf(why, whylen, "Lateness: '%s' is not a duration", value);
				return -1;
			}
			s->lateness_ns = d;
			return 0;
		}
		snprintf(why, whylen, "unknown key %s in [Dataset]", key);
		return -1;
	}
	if (p->section == 2) {
		struct hg_ds_part_spec *part = &s->parts[s->n_parts - 1];
		if (strcmp(key, "Topic") == 0) {
			if (seen_once(p, K_TOPIC, key, why, whylen) < 0)
				return -1;
			if (!hg_valid_name(value)) {
				snprintf(why, whylen, "Topic: '%s' is not a topic name", value);
				return -1;
			}
			snprintf(part->topic, sizeof part->topic, "%s", value);
			return 0;
		}
		if (strcmp(key, "Join") == 0) {
			if (seen_once(p, K_JOIN, key, why, whylen) < 0)
				return -1;
			if (strcmp(value, "provenance") == 0) {
				part->join = HG_DS_PROVENANCE;
				return 0;
			}
			if (strncmp(value, "asof:", 5) == 0 && parse_signed_duration(value + 5, &part->offset_ns) == 0) {
				part->join = HG_DS_ASOF;
				return 0;
			}
			snprintf(why, whylen, "Join: provenance or asof:<offset>, not '%s'", value);
			return -1;
		}
		if (strcmp(key, "Window") == 0) {
			if (seen_once(p, K_WINDOW, key, why, whylen) < 0)
				return -1;
			char *e;
			unsigned long k = strtoul(value, &e, 10);
			if (e == value || *e || k < 1 || k > HG_DS_MAX_WINDOW) {
				snprintf(why, whylen, "Window: 1 to %d, not '%s'", HG_DS_MAX_WINDOW, value);
				return -1;
			}
			part->window = (uint32_t)k;
			return 0;
		}
		if (strcmp(key, "Role") == 0) {
			if (seen_once(p, K_ROLE, key, why, whylen) < 0)
				return -1;
			if (parse_role(value, &part->role) < 0) {
				snprintf(why, whylen, "Role: input, label or filter, not '%s'", value);
				return -1;
			}
			return 0;
		}
		if (strcmp(key, "Fields") == 0) {
			if (seen_once(p, K_FIELDS, key, why, whylen) < 0)
				return -1;
			int n = split_fields(value, part->fields, HG_DS_MAX_SELECT, why, whylen);
			if (n < 0)
				return -1;
			part->n_fields = n;
			return 0;
		}
		snprintf(why, whylen, "unknown key %s in [Input %s]", key, part->name);
		return -1;
	}
	snprintf(why, whylen, "a key before any section");
	return -1;
}

int hg_ds_parse(const char *text, size_t len, const char *name, struct hg_ds_spec *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof *out);
	if (!name || !*name || strlen(name) >= HG_DS_NAME) {
		seterr(err, errlen, "dataset name missing or over %d bytes", HG_DS_NAME - 1);
		errno = EINVAL;
		return -1;
	}
	for (const char *p = name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
			seterr(err, errlen, "dataset name '%s': [a-z0-9_] only", name);
			errno = EINVAL;
			return -1;
		}
	snprintf(out->name, sizeof out->name, "%s", name);
	out->anchor_role = HG_DS_LABEL;
	out->rate_kind = HG_DS_EVERY;
	out->rate_every = 1;
	out->filter = HG_DS_DROP;
	out->reset_after_ns = 200000000;
	out->lateness_ns = 100000000;
	struct pstate p = { .s = out, .err = err, .errlen = errlen };
	if (hg_ini_parse(text, len, on_section, on_key, &p, err, errlen) < 0)
		return -1;
	if (!p.dataset_seen || !out->anchor[0]) {
		seterr(err, errlen, "[Dataset] with Anchor= is required");
		errno = EINVAL;
		return -1;
	}
	for (int i = 0; i < out->n_parts; i++) {
		if ((int)out->parts[i].join < 0) {
			seterr(err, errlen, "line %d: [Input %s] needs Join=", out->parts[i].line, out->parts[i].name);
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

int hg_ds_read_file(const char *path, struct hg_ds_spec *out, char *err, size_t errlen)
{
	static char text[65536];
	size_t len;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		seterr(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char name[HG_DS_NAME];
	const char *dot = strrchr(base, '.');
	size_t n = dot ? (size_t)(dot - base) : strlen(base);
	if (n == 0 || n >= sizeof name || !dot || strcmp(dot, ".dataset") != 0) {
		seterr(err, errlen, "%s: a spec is <name>.dataset", path);
		errno = EINVAL;
		return -1;
	}
	memcpy(name, base, n);
	name[n] = 0;
	char why[256];
	if (hg_ds_parse(text, len, name, out, why, sizeof why) < 0) {
		seterr(err, errlen, "%s: %s", path, why);
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int64_t hg_ds_wait_ns(const struct hg_ds_spec *s)
{
	int64_t w = 0;
	for (int i = 0; i < s->n_parts; i++)
		if (s->parts[i].join == HG_DS_ASOF && s->parts[i].offset_ns + s->lateness_ns > w)
			w = s->parts[i].offset_ns + s->lateness_ns;
	return w;
}

int hg_ds_policy_usable(const struct hg_ds_spec *s, char *out, size_t outlen)
{
	int ok = 1;
	if (out && outlen)
		out[0] = 0;
	for (int i = 0; i < s->n_parts; i++) {
		const struct hg_ds_part_spec *p = &s->parts[i];
		if (p->role == HG_DS_INPUT && p->join == HG_DS_ASOF && p->offset_ns + s->lateness_ns > 0) {
			ok = 0;
			if (out && outlen) {
				size_t l = strlen(out);
				snprintf(out + l, outlen - l, "%s%s", l ? " " : "", p->name);
			}
		}
	}
	return ok;
}

/* ---- Manifests --------------------------------------------------------------- */

struct mscan {
	const char *anchor;
	int in_anchor;
	int found;
	char links[2048];   /* "topic topic ..." from Inputs and Sources */
};

static int m_section(void *ctx, const char *section, const char *arg, int line, char *why, size_t whylen)
{
	struct mscan *m = ctx;
	(void)line;
	(void)why;
	(void)whylen;
	m->in_anchor = strcmp(section, "Publish") == 0 && strcmp(arg, m->anchor) == 0;
	if (m->in_anchor)
		m->found = 1;
	return 0;
}

static int m_key(void *ctx, const char *key, const char *value, int line, char *why, size_t whylen)
{
	struct mscan *m = ctx;
	(void)line;
	(void)why;
	(void)whylen;
	if (m->in_anchor && (strcmp(key, "Inputs") == 0 || strcmp(key, "Sources") == 0)) {
		size_t l = strlen(m->links);
		snprintf(m->links + l, sizeof m->links - l, " %s ", value);
	}
	return 0;
}

int hg_ds_check_manifests(const struct hg_ds_spec *s, const char *dir, char *err, size_t errlen)
{
	DIR *d = opendir(dir);
	if (!d) {
		seterr(err, errlen, "%s: %s", dir, strerror(errno));
		return -1;
	}
	struct mscan m = { .anchor = s->anchor };
	static char text[65536];
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 8 || strcmp(e->d_name + n - 7, ".module") != 0)
			continue;
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		size_t len;
		if (hg_ini_read_file(path, text, sizeof text, &len) < 0)
			continue;
		m.in_anchor = 0;
		/* Manifests the manager refused are not this check's concern:
		 * a parse failure here is ignored, the walker stops at it. */
		(void)hg_ini_parse(text, len, m_section, m_key, &m, NULL, 0);
	}
	closedir(d);
	if (!m.found) {
		seterr(err, errlen, "no manifest in %s publishes the anchor %s", dir, s->anchor);
		errno = ENOENT;
		return -1;
	}
	for (int i = 0; i < s->n_parts; i++) {
		const struct hg_ds_part_spec *p = &s->parts[i];
		if (p->join != HG_DS_PROVENANCE)
			continue;
		char needle[HG_NAME_MAX + 3];
		snprintf(needle, sizeof needle, " %s ", p->topic);
		if (!strstr(m.links, needle)) {
			seterr(err, errlen, "part %s: a provenance join to %s, which is neither in the Inputs nor in the "
					    "Sources of %s", p->name, p->topic, s->anchor);
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

/* ---- The row type ---------------------------------------------------------- */

static const char *const kind_text[] = { "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64", "char" };

struct sbuf {
	char *p;
	size_t cap, len;
	int overflow;
};

static void sput(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sput(struct sbuf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= b->cap - b->len) {
		b->overflow = 1;
		b->len = b->cap - 1;
	} else {
		b->len += (size_t)n;
	}
}

/* One field line of the row schema. */
static void field_line(struct sbuf *b, const struct hg_field *f, const char *prefix, int k, const char *role,
		       int comments)
{
	char name[HG_FIELD_NAME_MAX + 32];
	if (k >= 0)
		snprintf(name, sizeof name, "%s.%d.%s", prefix, k, f->name);
	else
		snprintf(name, sizeof name, "%s.%s", prefix, f->name);
	if (f->count > 1 || f->kind == HG_CHAR)
		sput(b, "%s %s[%u]", kind_text[f->kind], name, f->count);
	else
		sput(b, "%s %s", kind_text[f->kind], name);
	if (comments && f->comment && f->comment[0])
		sput(b, " # role=%s %s\n", role, f->comment);
	else
		sput(b, " # role=%s\n", role);
}

/* The fields a part (or the anchor) keeps, in order: the selection, or all.
 * Returns the count, -1 if a name does not exist. */
static int select_fields(const struct hg_type *t, const char (*names)[HG_FIELD_NAME_MAX + 1], int n_names,
			 const struct hg_field **out, int cap, const char **missing)
{
	if (n_names == 0) {
		if ((int)t->n_fields > cap)
			return -2;
		for (uint32_t i = 0; i < t->n_fields; i++)
			out[i] = &t->fields[i];
		return (int)t->n_fields;
	}
	for (int i = 0; i < n_names; i++) {
		out[i] = hg_type_field(t, names[i]);
		if (!out[i]) {
			*missing = names[i];
			return -1;
		}
	}
	return n_names;
}

/* Writes the schema text; comments 0 drops the source comments. */
static int gen_schema(const struct hg_ds_spec *s, const struct hg_ds_topic_info *anchor,
		      const struct hg_ds_topic_info *parts, int comments, char *out, size_t cap, char *err,
		      size_t errlen)
{
	struct sbuf b = { .p = out, .cap = cap };
	out[0] = 0;
	sput(&b, "# dataset %s: one row per %s message (docs/06-recorder.md, The row)\n", s->name, s->anchor);
	sput(&b, "u64 anchor_seq # role=meta\n"
		 "u64 anchor_stamp_ns # role=meta\n"
		 "u8 boot_id[16] # role=meta the boot that numbered the anchor\n"
		 "u32 missing # role=meta bit i: part i missing\n"
		 "u32 flags # role=meta bit 0 reset, bit 1 masked, bit 2 late\n"
		 "u64 emitted_ns # role=meta when the live row's joins closed\n");
	const struct hg_field *sel[HG_DS_MAX_SELECT];
	const char *miss = NULL;
	int n = select_fields(anchor->type, s->anchor_fields, s->n_anchor_fields, sel, HG_DS_MAX_SELECT, &miss);
	if (n == -1) {
		seterr(err, errlen, "anchor %s has no field %s", s->anchor, miss);
		return -1;
	}
	if (n == -2) {
		seterr(err, errlen, "anchor %s: more than %d fields; name them with Fields=", s->anchor, HG_DS_MAX_SELECT);
		return -1;
	}
	for (int i = 0; i < n; i++)
		field_line(&b, sel[i], s->anchor, -1, hg_ds_role_name(s->anchor_role), comments);
	for (int i = 0; i < s->n_parts; i++) {
		const struct hg_ds_part_spec *p = &s->parts[i];
		const char *role = hg_ds_role_name(p->role);
		sput(&b, "u64 %s._seq # role=%s\nu64 %s._stamp_ns # role=%s\nu32 %s._count # role=%s\nu32 %s._flags # role=%s\n",
		     p->name, role, p->name, role, p->name, role, p->name, role);
		n = select_fields(parts[i].type, p->fields, p->n_fields, sel, HG_DS_MAX_SELECT, &miss);
		if (n == -1) {
			seterr(err, errlen, "part %s: %s has no field %s", p->name, p->topic, miss);
			return -1;
		}
		if (n == -2) {
			seterr(err, errlen, "part %s: more than %d fields; name them with Fields=", p->name, HG_DS_MAX_SELECT);
			return -1;
		}
		for (uint32_t k = 0; k < p->window; k++)
			for (int j = 0; j < n; j++)
				field_line(&b, sel[j], p->name, p->window > 1 ? (int)k : -1, role, comments);
	}
	return b.overflow ? 1 : 0;
}

struct resolve_ctx {
	struct hg_ds_row_type *rt;
};

/* `<part>.<field>`, `<part>.<k>.<field>`, or `<anchor>.<field>`: the
 * longest part name that is a prefix wins. */
static int resolve_where(void *ctx, const char *name, size_t n, struct hg_ds_where_node *node, char *why,
			 size_t whylen)
{
	struct hg_ds_row_type *rt = ((struct resolve_ctx *)ctx)->rt;
	char full[256];
	if (n >= sizeof full) {
		snprintf(why, whylen, "Where: name too long");
		return -1;
	}
	memcpy(full, name, n);
	full[n] = 0;
	int best = -2;
	size_t best_len = 0;
	size_t al = strlen(rt->spec.anchor);
	if (n > al + 1 && strncmp(full, rt->spec.anchor, al) == 0 && full[al] == '.') {
		best = -1;
		best_len = al;
	}
	for (int i = 0; i < rt->spec.n_parts; i++) {
		size_t pl = strlen(rt->spec.parts[i].name);
		if (n > pl + 1 && strncmp(full, rt->spec.parts[i].name, pl) == 0 && full[pl] == '.' && pl > best_len) {
			best = i;
			best_len = pl;
		}
	}
	if (best == -2) {
		snprintf(why, whylen, "Where: '%s' names no part of the spec", full);
		return -1;
	}
	const char *rest = full + best_len + 1;
	char rowname[HG_FIELD_NAME_MAX + 1];
	if (best >= 0 && rt->spec.parts[best].window > 1) {
		/* With no index the last message is meant. */
		int has_index = rest[0] >= '0' && rest[0] <= '9';
		if (has_index)
			snprintf(rowname, sizeof rowname, "%s.%s", rt->spec.parts[best].name, rest);
		else
			snprintf(rowname, sizeof rowname, "%s.%u.%s", rt->spec.parts[best].name,
				 rt->spec.parts[best].window - 1, rest);
	} else {
		if (n > HG_FIELD_NAME_MAX) {
			snprintf(why, whylen, "Where: '%s' is longer than a field name", full);
			return -1;
		}
		memcpy(rowname, full, n + 1);
	}
	const struct hg_field *f = hg_type_field(&rt->schema.type, rowname);
	if (!f) {
		snprintf(why, whylen, "Where: no row field '%s' (is it kept by Fields=?)", full);
		return -1;
	}
	if (f->count != 1 || f->kind == HG_CHAR) {
		snprintf(why, whylen, "Where: '%s' is an array or text; a comparison needs a scalar", full);
		return -1;
	}
	node->part = best;
	node->row_off = f->offset;
	node->kind = f->kind;
	return 0;
}

int hg_ds_bind(const struct hg_ds_spec *s, const struct hg_ds_topic_info *anchor,
	       const struct hg_ds_topic_info *parts, struct hg_ds_row_type *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof *out);
	out->spec = *s;
	if (!anchor->type) {
		seterr(err, errlen, "anchor %s: no type", s->anchor);
		goto einval;
	}
	for (int i = 0; i < s->n_parts; i++)
		if (!parts[i].type) {
			seterr(err, errlen, "part %s: no type", s->parts[i].name);
			goto einval;
		}
	if (s->rate_kind == HG_DS_RATE_HZ) {
		if (anchor->rate_mhz == 0) {
			seterr(err, errlen, "Rate=rate:<Hz> on the event topic %s; use every:N", s->anchor);
			goto einval;
		}
		uint64_t n = ((uint64_t)anchor->rate_mhz + s->rate_mhz / 2) / s->rate_mhz;
		out->spec.rate_every = n ? n : 1;
	}
	char text[HG_SCHEMA_MAX + 1024];
	int r = gen_schema(s, anchor, parts, 1, text, sizeof text, err, errlen);
	if (r < 0)
		goto einval;
	if (r == 1 || strlen(text) > HG_SCHEMA_MAX)
		r = gen_schema(s, anchor, parts, 0, text, sizeof text, err, errlen);
	if (r < 0)
		goto einval;
	if (r == 1 || strlen(text) > HG_SCHEMA_MAX) {
		seterr(err, errlen, "the row schema is longer than %d bytes", HG_SCHEMA_MAX);
		goto einval;
	}
	char why[200];
	if (hg_schema_parse(text, strlen(text), &out->schema, why, sizeof why) < 0) {
		seterr(err, errlen, "the row schema does not parse: %s (over %d fields?)", why, HG_SCHEMA_MAX_FIELDS);
		goto einval;
	}
	out->row_bytes = out->schema.type.payload_bytes;
	out->anchor_topic_id = anchor->topic_id;
	out->anchor_payload_bytes = anchor->type->payload_bytes;
	out->anchor_rate_mhz = anchor->rate_mhz;
	out->anchor_stale_after_ns = anchor->stale_after_ns;
	out->wait_ns = hg_ds_wait_ns(s);
	/* Map the row's fields back, in the order they were emitted: 6 header
	 * fields, the anchor's, then per part 4 header fields and K x n. */
	uint32_t fi = 6;
	const struct hg_field *rowf = out->schema.type.fields;
	const struct hg_field *sel[HG_DS_MAX_SELECT];
	const char *miss;
	int n = select_fields(anchor->type, s->anchor_fields, s->n_anchor_fields, sel, HG_DS_MAX_SELECT, &miss);
	out->n_anchor_map = n;
	for (int j = 0; j < n; j++, fi++) {
		out->anchor_map[j].src_off = sel[j]->offset;
		out->anchor_map[j].row_off = rowf[fi].offset;
		out->anchor_map[j].bytes = hg_field_elem_size(sel[j]->kind) * sel[j]->count;
		out->anchor_map[j].kind = sel[j]->kind;
	}
	out->scratch_bytes = 8;
	for (int i = 0; i < s->n_parts; i++) {
		const struct hg_ds_part_spec *p = &s->parts[i];
		struct hg_ds_part *bp = &out->parts[i];
		bp->topic_id = parts[i].topic_id;
		bp->payload_bytes = parts[i].type->payload_bytes;
		bp->rate_mhz = parts[i].rate_mhz;
		bp->stale_after_ns = parts[i].stale_after_ns;
		bp->immediate = p->join == HG_DS_PROVENANCE || p->offset_ns + s->lateness_ns <= 0;
		if (bp->payload_bytes > out->scratch_bytes)
			out->scratch_bytes = bp->payload_bytes;
		bp->hdr_off = rowf[fi].offset;
		fi += 4;
		n = select_fields(parts[i].type, p->fields, p->n_fields, sel, HG_DS_MAX_SELECT, &miss);
		bp->first_map = out->n_map;
		for (uint32_t k = 0; k < p->window; k++)
			for (int j = 0; j < n; j++, fi++) {
				struct hg_ds_field_map *m = &out->map[out->n_map++];
				m->src_off = sel[j]->offset;
				m->row_off = rowf[fi].offset;
				m->bytes = hg_field_elem_size(sel[j]->kind) * sel[j]->count;
				m->k = k;
				m->kind = sel[j]->kind;
			}
		bp->n_map = out->n_map - bp->first_map;
	}
	if (s->where[0]) {
		struct resolve_ctx rc = { out };
		int nw = where_compile(s->where, out->where, HG_DS_WHERE_MAX, resolve_where, &rc, why, sizeof why);
		if (nw < 0) {
			seterr(err, errlen, "%s", why);
			goto einval;
		}
		out->n_where = nw;
		for (int i = 0; i < nw; i++)
			if (out->where[i].op == HG_DS_OP_CMP && out->where[i].part >= 0)
				out->where_parts |= 1u << out->where[i].part;
	}
	return 0;
einval:
	errno = EINVAL;
	return -1;
}

/* ---- Joining ------------------------------------------------------------------ */

static enum hg_result src_sub_read_seq(void *ctx, uint64_t seq, void *buf, size_t len, struct hg_info *info)
{
	return hg_read_seq(ctx, seq, buf, len, info);
}

static enum hg_result src_sub_find_stamp(void *ctx, uint64_t t, void *buf, size_t len, struct hg_info *info)
{
	return hg_find_stamp(ctx, t, buf, len, info);
}

struct hg_ds_source hg_ds_source_sub(hg_sub *s)
{
	struct hg_ds_source src = { s, src_sub_read_seq, src_sub_find_stamp };
	return src;
}

static enum hg_result file_copy(const struct hgr_reader *r, int64_t i, void *buf, size_t len, struct hg_info *info)
{
	if (i < 0 || r->meta) {
		memset(info, 0, sizeof *info);
		info->result = HG_GONE;
		return HG_GONE;
	}
	hgr_info(r, (uint64_t)i, info);
	size_t n = len < r->payload_bytes ? len : r->payload_bytes;
	memcpy(buf, hgr_payload_at(r, (uint64_t)i), n);
	return HG_MSG;
}

static enum hg_result src_file_read_seq(void *ctx, uint64_t seq, void *buf, size_t len, struct hg_info *info)
{
	return file_copy(ctx, hgr_find_seq(ctx, seq), buf, len, info);
}

static enum hg_result src_file_find_stamp(void *ctx, uint64_t t, void *buf, size_t len, struct hg_info *info)
{
	return file_copy(ctx, hgr_find_stamp(ctx, t), buf, len, info);
}

struct hg_ds_source hg_ds_source_file(const struct hgr_reader *r)
{
	struct hg_ds_source src = { (void *)r, src_file_read_seq, src_file_find_stamp };
	return src;
}

uint64_t hg_ds_anchor_time(const struct hg_info *anchor)
{
	return anchor->stamp_ns ? anchor->stamp_ns : anchor->publish_ns;
}

static void put64(unsigned char *row, uint32_t off, uint64_t v)
{
	memcpy(row + off, &v, 8);
}

static void put32(unsigned char *row, uint32_t off, uint32_t v)
{
	memcpy(row + off, &v, 4);
}

static uint64_t get64(const unsigned char *row, uint32_t off)
{
	uint64_t v;
	memcpy(&v, row + off, 8);
	return v;
}

static uint32_t get32(const unsigned char *row, uint32_t off)
{
	uint32_t v;
	memcpy(&v, row + off, 4);
	return v;
}

void hg_ds_row_begin(const struct hg_ds_row_type *rt, unsigned char *row, const struct hg_info *anchor,
		     const void *anchor_payload, const unsigned char boot[16])
{
	memset(row, 0, rt->row_bytes);
	put64(row, HG_DS_OFF_ANCHOR_SEQ, anchor->seq);
	put64(row, HG_DS_OFF_ANCHOR_STAMP, anchor->stamp_ns);
	memcpy(row + HG_DS_OFF_BOOT, boot, 16);
	put64(row, HG_DS_OFF_EMITTED, anchor->publish_ns + (uint64_t)rt->wait_ns);
	const unsigned char *src = anchor_payload;
	for (int j = 0; j < rt->n_anchor_map; j++)
		memcpy(row + rt->anchor_map[j].row_off, src + rt->anchor_map[j].src_off, rt->anchor_map[j].bytes);
}

/* Copies message k's selected fields. */
static void copy_part(const struct hg_ds_row_type *rt, const struct hg_ds_part *bp, unsigned char *row, uint32_t k,
		      const unsigned char *payload)
{
	for (int j = 0; j < bp->n_map; j++) {
		const struct hg_ds_field_map *m = &rt->map[bp->first_map + j];
		if (m->k == k)
			memcpy(row + m->row_off, payload + m->src_off, m->bytes);
	}
}

void hg_ds_join(const struct hg_ds_row_type *rt, int i, unsigned char *row, const struct hg_info *anchor,
		const struct hg_ds_source *src, void *scratch, struct hg_ds_joined *out)
{
	const struct hg_ds_part_spec *p = &rt->spec.parts[i];
	const struct hg_ds_part *bp = &rt->parts[i];
	struct hg_info info;
	uint32_t ref_flags = 0;
	enum hg_result r;
	memset(out, 0, sizeof *out);
	uint64_t t = hg_ds_anchor_time(anchor) + (uint64_t)p->offset_ns;
	if (p->join == HG_DS_PROVENANCE) {
		int found = -1;
		for (uint32_t k = 0; k < anchor->n_inputs && k < HG_MAX_REFS; k++)
			if (anchor->inputs[k].topic_id == bp->topic_id) {
				found = (int)k;
				break;
			}
		if (found < 0 || (anchor->inputs[found].flags & HG_REF_ABSENT) || anchor->inputs[found].seq == 0) {
			ref_flags = found >= 0 ? (anchor->inputs[found].flags & 7u) << 8 : 0;
			goto missing;
		}
		ref_flags = (anchor->inputs[found].flags & 7u) << 8;
		r = src->read_seq(src->ctx, anchor->inputs[found].seq, scratch, bp->payload_bytes, &info);
	} else {
		r = src->find_stamp(src->ctx, t, scratch, bp->payload_bytes, &info);
	}
	if (r != HG_MSG)
		goto missing;
	/* The joined message is k = K-1; the K-1 before it, by seq, fill the
	 * rest; count stops at the first that is not there. */
	uint32_t K = p->window;
	copy_part(rt, bp, row, K - 1, scratch);
	uint32_t count = 1;
	uint64_t last_seq = info.seq, last_stamp = info.stamp_ns, last_pub = info.publish_ns;
	uint32_t last_flags = info.flags;
	for (uint32_t k = 1; k < K; k++) {
		struct hg_info wi;
		if (info.seq < k)
			break;
		if (src->read_seq(src->ctx, info.seq - k, scratch, bp->payload_bytes, &wi) != HG_MSG)
			break;
		copy_part(rt, bp, row, K - 1 - k, scratch);
		count++;
	}
	put64(row, bp->hdr_off, last_seq);
	put64(row, bp->hdr_off + 8, last_stamp);
	put32(row, bp->hdr_off + 16, count);
	put32(row, bp->hdr_off + 20, (last_flags & 0xffu) | ref_flags);
	out->publish_ns = last_pub;
	uint64_t now = hg_now_ns();
	out->latency_ns = now > last_pub ? now - last_pub : 0;
	out->stale = (last_flags & HG_SLOT_STALE) != 0 || (ref_flags & HG_DS_PART_STALE) != 0;
	if (p->join == HG_DS_ASOF && bp->rate_mhz && bp->stale_after_ns) {
		uint64_t ms = last_stamp ? last_stamp : last_pub;
		if (t > ms && t - ms > bp->stale_after_ns)
			out->stale = 1;
	}
	return;
missing:
	out->missing = 1;
	put32(row, HG_DS_OFF_MISSING, get32(row, HG_DS_OFF_MISSING) | (1u << i));
	put32(row, bp->hdr_off + 20, ref_flags);
}

int hg_ds_rate_keeps(const struct hg_ds_row_type *rt, uint64_t anchor_seq)
{
	return rt->spec.rate_every <= 1 || anchor_seq % rt->spec.rate_every == 0;
}

static double field_value(const unsigned char *row, uint32_t off, enum hg_field_kind kind)
{
	switch (kind) {
	case HG_U8: return (double)row[off];
	case HG_I8: return (double)(int8_t)row[off];
	case HG_U16: { uint16_t v; memcpy(&v, row + off, 2); return (double)v; }
	case HG_I16: { int16_t v; memcpy(&v, row + off, 2); return (double)v; }
	case HG_U32: return (double)get32(row, off);
	case HG_I32: { int32_t v; memcpy(&v, row + off, 4); return (double)v; }
	case HG_U64: return (double)get64(row, off);
	case HG_I64: { int64_t v; memcpy(&v, row + off, 8); return (double)v; }
	case HG_F32: { float v; memcpy(&v, row + off, 4); return (double)v; }
	case HG_F64: { double v; memcpy(&v, row + off, 8); return v; }
	case HG_CHAR: return 0;
	}
	return 0;
}

int hg_ds_where(const struct hg_ds_row_type *rt, const unsigned char *row)
{
	if (!rt->n_where)
		return 1;
	if (get32(row, HG_DS_OFF_MISSING) & rt->where_parts)
		return 0;
	int stack[HG_DS_WHERE_MAX];
	int sp = 0;
	for (int i = 0; i < rt->n_where; i++) {
		const struct hg_ds_where_node *n = &rt->where[i];
		if (n->op == HG_DS_OP_CMP) {
			double v = field_value(row, n->row_off, n->kind);
			int r = 0;
			switch (n->cmp) {
			case HG_DS_EQ: r = v == n->value; break;
			case HG_DS_NE: r = v != n->value; break;
			case HG_DS_LT: r = v < n->value; break;
			case HG_DS_LE: r = v <= n->value; break;
			case HG_DS_GT: r = v > n->value; break;
			case HG_DS_GE: r = v >= n->value; break;
			}
			stack[sp++] = r;
		} else {
			if (sp < 2)
				return 0;
			int b = stack[--sp], a = stack[--sp];
			stack[sp++] = n->op == HG_DS_OP_AND ? (a && b) : (a || b);
		}
	}
	return sp == 1 ? stack[0] : 0;
}

int hg_ds_row_finish(const struct hg_ds_row_type *rt, struct hg_ds_state *st, unsigned char *row,
		     const struct hg_ds_joined *joined, int anchor_gap, uint32_t epoch)
{
	const struct hg_ds_spec *s = &rt->spec;
	uint32_t flags = get32(row, HG_DS_OFF_FLAGS);
	uint64_t stamp = get64(row, HG_DS_OFF_ANCHOR_STAMP);
	if (!stamp)
		stamp = get64(row, HG_DS_OFF_EMITTED) - (uint64_t)rt->wait_ns;
	/* The reset rule (docs/06 "The reset flag"). */
	int reset = 0;
	if (!st->started || anchor_gap || (st->started && epoch != st->epoch))
		reset = 1;
	for (int i = 0; i < s->n_parts; i++) {
		if (s->parts[i].role != HG_DS_INPUT)
			continue;
		int bad = joined[i].missing || joined[i].stale;
		if (bad) {
			if (!st->bad[i]) {
				st->bad[i] = 1;
				st->bad_since[i] = stamp;
			}
			if (stamp >= st->bad_since[i] && stamp - st->bad_since[i] >= (uint64_t)s->reset_after_ns)
				reset = 1;
		} else {
			st->bad[i] = 0;
		}
	}
	st->started = 1;
	st->epoch = epoch;
	if (reset)
		flags |= HG_DS_ROW_RESET;
	int keep = 1;
	if (s->missing_drop && get32(row, HG_DS_OFF_MISSING))
		keep = 0;
	if (keep && s->where[0] && !hg_ds_where(rt, row)) {
		if (s->filter == HG_DS_MASK)
			flags |= HG_DS_ROW_MASKED;
		else
			keep = 0;
	}
	put32(row, HG_DS_OFF_FLAGS, flags);
	return keep;
}

void hg_ds_boot_id(unsigned char out[16])
{
	memset(out, 0, 16);
	int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	char text[64];
	ssize_t n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return;
	text[n] = 0;
	int k = 0;
	for (const char *p = text; *p && k < 32; p++) {
		int v;
		if (*p >= '0' && *p <= '9')
			v = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			v = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')
			v = *p - 'A' + 10;
		else
			continue;
		out[k / 2] = (unsigned char)((out[k / 2] << 4) | v);
		k++;
	}
}
