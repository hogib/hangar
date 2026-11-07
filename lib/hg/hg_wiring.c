// SPDX-License-Identifier: LGPL-3.0-or-later
/* Wiring parser. Line-oriented, one pass, fixed-size output: a module reads
 * its wiring once at start, so there is nothing to allocate or free. */
#define _GNU_SOURCE
#include "hg_wiring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_WIRING_BYTES (1u << 20)

int hg_glob_match(const char *p, const char *n)
{
	/* Iterative matching with one backtrack point: on a '*' remember where it
	 * was and where in the name it started; on a mismatch, retry with the
	 * star eating one more character. Linear in practice for these names. */
	const char *star = NULL, *mark = NULL;
	while (*n) {
		if (*p == '*') {
			star = p++;
			mark = n;
		} else if (*p == '?' || *p == *n) {
			p++;
			n++;
		} else if (star) {
			p = star + 1;
			n = ++mark;
		} else {
			return 0;
		}
	}
	while (*p == '*')
		p++;
	return *p == 0;
}

/* Reads an unsigned decimal with up to `scale_digits` fractional digits as an
 * integer of that scale (e.g. 3 digits: "2.5" -> 2500). -1 if malformed; the
 * caller handles the unit suffix that follows. */
static int64_t scaled(const char **sp, int scale_digits)
{
	const char *s = *sp;
	int64_t v = 0;
	int digits = 0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s - '0');
		if (v > (int64_t)1 << 50)
			return -1;
		s++;
		digits++;
	}
	int frac = 0;
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9') {
			if (frac < scale_digits) {
				v = v * 10 + (*s - '0');
				frac++;
			} else if (*s != '0') {
				return -1; /* more precision than the unit holds */
			}
			s++;
			digits++;
		}
	}
	if (digits == 0)
		return -1;
	for (; frac < scale_digits; frac++)
		v *= 10;
	*sp = s;
	return v;
}

int64_t hg_parse_duration(const char *s)
{
	/* Parsed as nanoseconds with 9 fractional digits, then scaled by unit. */
	const char *p = s;
	int64_t v = scaled(&p, 9); /* value * 1e9 */
	if (v < 0)
		return -1;
	int64_t unit_ns;
	if (strcmp(p, "ns") == 0)
		unit_ns = 1;
	else if (strcmp(p, "us") == 0)
		unit_ns = 1000;
	else if (strcmp(p, "ms") == 0)
		unit_ns = 1000000;
	else if (strcmp(p, "s") == 0)
		unit_ns = 1000000000;
	else if (*p == 0 && v == 0)
		return 0; /* a bare 0: no unit needed */
	else
		return -1;
	/* v is value*1e9; result = value*unit_ns = v*unit_ns/1e9, exact when the
	 * fractional part fits the unit. */
	if (v % (1000000000 / unit_ns) != 0)
		return -1;
	return v / (1000000000 / unit_ns);
}

int64_t hg_parse_rate_mhz(const char *s)
{
	const char *p = s;
	int64_t v = scaled(&p, 3);
	if (v < 0 || *p != 0 || v > 0xffffffffll)
		return -1;
	return v;
}

/* ---- Tokens ------------------------------------------------------------ */

struct tok {
	const char *s;
	size_t len;
};

/* Splits a line into whitespace-separated tokens; returns their number, at
 * most max. */
static int split(const char *s, const char *end, struct tok *t, int max)
{
	int n = 0;
	while (s < end) {
		while (s < end && (*s == ' ' || *s == '\t' || *s == '\r'))
			s++;
		if (s == end)
			break;
		const char *b = s;
		while (s < end && !(*s == ' ' || *s == '\t' || *s == '\r'))
			s++;
		if (n < max) {
			t[n].s = b;
			t[n].len = (size_t)(s - b);
		}
		n++;
	}
	return n;
}

static int tok_is(const struct tok *t, const char *word)
{
	return strlen(word) == t->len && memcmp(t->s, word, t->len) == 0;
}

/* Copies a token into a fixed field; 0 if it does not fit or is empty. */
static int tok_copy(const struct tok *t, char *dst, size_t len)
{
	if (t->len == 0 || t->len >= len)
		return 0;
	memcpy(dst, t->s, t->len);
	dst[t->len] = 0;
	return 1;
}

/* "key=value": the key's length if the token has one, with *val set past '='. */
static size_t tok_key(const struct tok *t, const char **val)
{
	const char *eq = memchr(t->s, '=', t->len);
	if (!eq)
		return 0;
	*val = eq + 1;
	return (size_t)(eq - t->s);
}

static int key_is(const struct tok *t, size_t klen, const char *key)
{
	return strlen(key) == klen && memcmp(t->s, key, klen) == 0;
}

/* A NUL-terminated copy of the value part of a token, in buf. */
static const char *tok_val(const struct tok *t, const char *val, char *buf, size_t len)
{
	size_t n = t->len - (size_t)(val - t->s);
	if (n >= len)
		n = len - 1;
	memcpy(buf, val, n);
	buf[n] = 0;
	return buf;
}

static int parse_hash(const char *s, uint64_t *out)
{
	if (strlen(s) != 16)
		return -1;
	uint64_t v = 0;
	for (int i = 0; i < 16; i++) {
		char c = s[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
			: c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0)
			return -1;
		v = v << 4 | (uint64_t)d;
	}
	*out = v;
	return 0;
}

static int parse_u32(const char *s, uint32_t *out)
{
	char *e;
	if (!*s)
		return -1;
	unsigned long v = strtoul(s, &e, 10);
	if (*e || v > 0xffffffffu)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

/* Splits "a,b,c" into fixed names; the count or -1 if one does not fit. */
static int split_list(const char *s, char (*out)[HG_WIRING_NAME], int max)
{
	int n = 0;
	while (*s) {
		const char *c = strchr(s, ',');
		size_t len = c ? (size_t)(c - s) : strlen(s);
		if (len == 0 || len >= HG_WIRING_NAME || n == max)
			return -1;
		memcpy(out[n], s, len);
		out[n][len] = 0;
		n++;
		s = c ? c + 1 : s + len;
	}
	return n;
}

/* ---- Lines ------------------------------------------------------------- */

static void set_err(char *err, size_t errlen, int line, const char *what)
{
	if (err && errlen)
		snprintf(err, errlen, "wiring line %d: %s", line, what);
}

/* Fills the req/rep part of a serve or call line from tokens t[from..n):
 * "req=T hash=H size=S rep=T hash=H size=S", the hash and size belonging to
 * whichever of req/rep came last. Other keys go to `other`, called with the
 * token; it returns 0 to accept, -1 to refuse. */
static int parse_method(const struct tok *t, int from, int n, struct hg_wiring_method *m,
			int (*other)(const struct tok *, size_t, const char *, void *), void *ctx,
			const char **what)
{
	int side = 0; /* 0: none yet, 1: req, 2: rep */
	char buf[128];
	for (int i = from; i < n; i++) {
		const char *val;
		size_t k = tok_key(&t[i], &val);
		if (!k) {
			*what = "unexpected word";
			return -1;
		}
		tok_val(&t[i], val, buf, sizeof buf);
		if (key_is(&t[i], k, "req")) {
			side = 1;
			if (!tok_copy(&(struct tok){ buf, strlen(buf) }, m->req_type, sizeof m->req_type))
				goto bad;
		} else if (key_is(&t[i], k, "rep")) {
			side = 2;
			if (!tok_copy(&(struct tok){ buf, strlen(buf) }, m->rep_type, sizeof m->rep_type))
				goto bad;
		} else if (key_is(&t[i], k, "hash")) {
			if (!side || parse_hash(buf, side == 1 ? &m->req_hash : &m->rep_hash) < 0)
				goto bad;
		} else if (key_is(&t[i], k, "size")) {
			if (!side || parse_u32(buf, side == 1 ? &m->req_size : &m->rep_size) < 0)
				goto bad;
		} else if (other(&t[i], k, buf, ctx) < 0) {
			goto bad;
		}
	}
	if (!m->req_type[0] || !m->rep_type[0]) {
		*what = "req= and rep= are required";
		return -1;
	}
	return 0;
bad:
	*what = "bad key or value";
	return -1;
}

static int serve_other(const struct tok *t, size_t k, const char *val, void *ctx)
{
	struct hg_wiring_serve *s = ctx;
	if (key_is(t, k, "deadline")) {
		int64_t d = hg_parse_duration(val);
		if (d <= 0)
			return -1;
		s->deadline_ns = (uint64_t)d;
		return 0;
	}
	if (key_is(t, k, "allow")) {
		char names[HG_WIRING_MAX_ALLOW][HG_WIRING_NAME];
		int n = split_list(val, names, HG_WIRING_MAX_ALLOW);
		if (n < 0)
			return -1;
		for (int i = 0; i < n; i++)
			memcpy(s->allow[i], names[i], HG_WIRING_NAME);
		s->n_allow = n;
		return 0;
	}
	return -1;
}

static int call_other(const struct tok *t, size_t k, const char *val, void *ctx)
{
	struct hg_wiring_call *c = ctx;
	if (key_is(t, k, "remote")) {
		if (strcmp(val, "0") == 0)
			c->remote = 0;
		else if (strcmp(val, "1") == 0)
			c->remote = 1;
		else
			return -1;
		return 0;
	}
	return -1;
}

static int parse_sub(const struct tok *t, int n, struct hg_wiring_sub *s, const char **what)
{
	char buf[128];
	if (n < 2 || !tok_copy(&t[1], s->topic, sizeof s->topic)) {
		*what = "sub needs a topic";
		return -1;
	}
	for (int i = 2; i < n; i++) {
		const char *val;
		size_t k = tok_key(&t[i], &val);
		if (!k) {
			if (tok_is(&t[i], "implicit")) {
				s->implicit = 1;
				continue;
			}
			goto bad;
		}
		tok_val(&t[i], val, buf, sizeof buf);
		if (key_is(&t[i], k, "type")) {
			if (!tok_copy(&(struct tok){ buf, strlen(buf) }, s->type, sizeof s->type))
				goto bad;
		} else if (key_is(&t[i], k, "hash")) {
			if (parse_hash(buf, &s->hash) < 0)
				goto bad;
		} else if (key_is(&t[i], k, "rate")) {
			int64_t r = hg_parse_rate_mhz(buf);
			if (r < 0)
				goto bad;
			s->rate_mhz = (uint32_t)r;
		} else if (key_is(&t[i], k, "slots")) {
			if (parse_u32(buf, &s->slots) < 0)
				goto bad;
		} else {
			goto bad;
		}
	}
	return 0;
bad:
	*what = "bad key or value on sub line";
	return -1;
}

static int parse_pub(const struct tok *t, int n, struct hg_wiring_pub *p, const char **what)
{
	char buf[1024];
	if (n < 2 || !tok_copy(&t[1], p->topic, sizeof p->topic)) {
		*what = "pub needs a topic";
		return -1;
	}
	p->slots = 16;
	for (int i = 2; i < n; i++) {
		const char *val;
		size_t k = tok_key(&t[i], &val);
		if (!k) {
			if (tok_is(&t[i], "implicit")) {
				p->implicit = 1;
				continue;
			}
			goto bad;
		}
		tok_val(&t[i], val, buf, sizeof buf);
		if (key_is(&t[i], k, "type")) {
			if (!tok_copy(&(struct tok){ buf, strlen(buf) }, p->type, sizeof p->type))
				goto bad;
		} else if (key_is(&t[i], k, "hash")) {
			if (parse_hash(buf, &p->hash) < 0)
				goto bad;
		} else if (key_is(&t[i], k, "slots")) {
			if (parse_u32(buf, &p->slots) < 0 || p->slots < 2 || p->slots > 65536 ||
			    (p->slots & (p->slots - 1)))
				goto bad;
		} else if (key_is(&t[i], k, "rate")) {
			int64_t r = hg_parse_rate_mhz(buf);
			if (r < 0)
				goto bad;
			p->rate_mhz = (uint32_t)r;
		} else if (key_is(&t[i], k, "stale_after")) {
			int64_t d = hg_parse_duration(buf);
			if (d < 0)
				goto bad;
			p->stale_after_ns = (uint64_t)d;
		} else if (key_is(&t[i], k, "inputs")) {
			p->n_inputs = split_list(buf, p->inputs, 4);
			if (p->n_inputs < 0) {
				*what = "inputs: at most 4 topics";
				return -1;
			}
		} else if (key_is(&t[i], k, "sources")) {
			p->n_sources = split_list(buf, p->sources, HG_WIRING_MAX_SOURCES);
			if (p->n_sources < 0) {
				*what = "sources: too many topics";
				return -1;
			}
		} else {
			goto bad;
		}
	}
	if (!p->type[0]) {
		*what = "pub needs type=";
		return -1;
	}
	/* The manifest's default: three periods. The manager writes it out, but a
	 * hand-written wiring may leave it to us. */
	if (p->rate_mhz && !p->stale_after_ns)
		p->stale_after_ns = 3 * 1000000000000ull / p->rate_mhz;
	return 0;
bad:
	*what = "bad key or value on pub line";
	return -1;
}

static int parse_mirror(const struct tok *t, int n, struct hg_wiring_mirror *m, const char **what)
{
	char buf[128];
	if (n < 2 || !tok_copy(&t[1], m->pattern, sizeof m->pattern)) {
		*what = "mirror needs a topic or glob";
		return -1;
	}
	for (int i = 2; i < n; i++) {
		const char *val;
		size_t k = tok_key(&t[i], &val);
		if (!k)
			goto bad;
		tok_val(&t[i], val, buf, sizeof buf);
		if (key_is(&t[i], k, "type")) {
			if (strcmp(buf, "any") != 0)
				goto bad;
		} else if (key_is(&t[i], k, "rate")) {
			int64_t r = hg_parse_rate_mhz(buf);
			if (r < 0)
				goto bad;
			m->rate_mhz = (uint32_t)r;
		} else if (key_is(&t[i], k, "stale_after")) {
			int64_t d = hg_parse_duration(buf);
			if (d < 0)
				goto bad;
			m->stale_after_ns = (uint64_t)d;
		} else {
			goto bad;
		}
	}
	return 0;
bad:
	*what = "bad key or value on mirror line";
	return -1;
}

int hg_wiring_parse(const char *text, size_t len, struct hg_wiring *w, char *err, size_t errlen)
{
	memset(w, 0, sizeof *w);
	const char *s = text, *end = text + len;
	int line = 0;
	int seen_header = 0;
	while (s < end) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		const char *le = nl ? nl : end;
		line++;
		struct tok t[64];
		int n = split(s, le, t, 64);
		s = nl ? nl + 1 : end;
		if (n == 0 || t[0].s[0] == '#')
			continue;
		if (n > 64) {
			set_err(err, errlen, line, "too many words");
			goto bad;
		}
		const char *what = "";
		if (!seen_header) {
			if (n != 2 || !tok_is(&t[0], "hangar-wiring") || !tok_is(&t[1], "1")) {
				set_err(err, errlen, line, "expected 'hangar-wiring 1'");
				goto bad;
			}
			seen_header = 1;
			continue;
		}
		if (tok_is(&t[0], "module")) {
			if (n != 2 || !tok_copy(&t[1], w->module, sizeof w->module)) {
				set_err(err, errlen, line, "module needs one name");
				goto bad;
			}
		} else if (tok_is(&t[0], "sub")) {
			if (w->n_subs == HG_WIRING_MAX_SUBS) {
				set_err(err, errlen, line, "more than 127 subscriptions by name");
				goto bad;
			}
			if (parse_sub(t, n, &w->subs[w->n_subs], &what) < 0) {
				set_err(err, errlen, line, what);
				goto bad;
			}
			w->n_subs++;
		} else if (tok_is(&t[0], "subglob")) {
			if (w->n_globs == HG_WIRING_MAX_GLOBS) {
				set_err(err, errlen, line, "too many subglob lines");
				goto bad;
			}
			struct hg_wiring_glob *g = &w->globs[w->n_globs];
			if (n < 2 || n > 3 || !tok_copy(&t[1], g->pattern, sizeof g->pattern) ||
			    (n == 3 && !tok_is(&t[2], "implicit"))) {
				set_err(err, errlen, line, "subglob needs a pattern");
				goto bad;
			}
			g->implicit = n == 3;
			w->n_globs++;
		} else if (tok_is(&t[0], "pub")) {
			if (w->n_pubs == HG_WIRING_MAX_PUBS) {
				set_err(err, errlen, line, "too many pub lines");
				goto bad;
			}
			if (parse_pub(t, n, &w->pubs[w->n_pubs], &what) < 0) {
				set_err(err, errlen, line, what);
				goto bad;
			}
			w->n_pubs++;
		} else if (tok_is(&t[0], "serve")) {
			if (w->n_serves == HG_WIRING_MAX_SERVES) {
				set_err(err, errlen, line, "too many serve lines");
				goto bad;
			}
			struct hg_wiring_serve *sv = &w->serves[w->n_serves];
			sv->deadline_ns = 100000000; /* 100 ms, the manifest default */
			if (n < 2 || !tok_copy(&t[1], sv->method, sizeof sv->method) ||
			    parse_method(t, 2, n, &sv->m, serve_other, sv, &what) < 0) {
				set_err(err, errlen, line, n < 2 ? "serve needs a method" : what);
				goto bad;
			}
			w->n_serves++;
		} else if (tok_is(&t[0], "call")) {
			if (w->n_calls == HG_WIRING_MAX_CALLS) {
				set_err(err, errlen, line, "too many call lines");
				goto bad;
			}
			struct hg_wiring_call *c = &w->calls[w->n_calls];
			if (n < 2 || !tok_copy(&t[1], c->target, sizeof c->target) ||
			    !strchr(c->target, '.')) {
				set_err(err, errlen, line, "call needs <module>.<method>");
				goto bad;
			}
			if (parse_method(t, 2, n, &c->m, call_other, c, &what) < 0) {
				set_err(err, errlen, line, what);
				goto bad;
			}
			w->n_calls++;
		} else if (tok_is(&t[0], "mirror")) {
			if (w->n_mirrors == HG_WIRING_MAX_MIRRORS) {
				set_err(err, errlen, line, "too many mirror lines");
				goto bad;
			}
			if (parse_mirror(t, n, &w->mirrors[w->n_mirrors], &what) < 0) {
				set_err(err, errlen, line, what);
				goto bad;
			}
			w->n_mirrors++;
		} else {
			set_err(err, errlen, line, "unknown line kind");
			goto bad;
		}
	}
	if (!seen_header) {
		set_err(err, errlen, line, "empty wiring");
		goto bad;
	}
	if (!w->module[0]) {
		set_err(err, errlen, line, "no module line");
		goto bad;
	}
	return 0;
bad:
	errno = EINVAL;
	return -1;
}

int hg_wiring_read_fd(int fd, struct hg_wiring *w, char *err, size_t errlen)
{
	/* One read of the whole thing into a heap buffer that is freed before
	 * returning: the wiring is read once at hg_init, before any hot path. */
	size_t cap = 65536, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return -1;
	for (;;) {
		if (len == cap) {
			if (cap >= MAX_WIRING_BYTES) {
				free(buf);
				errno = EFBIG;
				return -1;
			}
			char *nb = realloc(buf, cap * 2);
			if (!nb) {
				free(buf);
				return -1;
			}
			buf = nb;
			cap *= 2;
		}
		ssize_t r = pread(fd, buf + len, cap - len, (off_t)len);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == ESPIPE)
				r = read(fd, buf + len, cap - len); /* a pipe: read in order */
			if (r < 0) {
				int e = errno;
				free(buf);
				errno = e;
				return -1;
			}
		}
		if (r == 0)
			break;
		len += (size_t)r;
	}
	int rc = hg_wiring_parse(buf, len, w, err, errlen);
	int e = errno;
	free(buf);
	errno = e;
	return rc;
}

int hg_wiring_read_file(const char *path, struct hg_wiring *w, char *err, size_t errlen)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int rc = hg_wiring_read_fd(fd, w, err, errlen);
	int e = errno;
	close(fd);
	errno = e;
	return rc;
}

const struct hg_wiring_sub *hg_wiring_find_sub(const struct hg_wiring *w, const char *topic)
{
	for (int i = 0; i < w->n_subs; i++)
		if (strcmp(w->subs[i].topic, topic) == 0)
			return &w->subs[i];
	return NULL;
}

const struct hg_wiring_glob *hg_wiring_find_glob(const struct hg_wiring *w, const char *pattern)
{
	for (int i = 0; i < w->n_globs; i++)
		if (strcmp(w->globs[i].pattern, pattern) == 0)
			return &w->globs[i];
	return NULL;
}

int hg_wiring_glob_matches(const struct hg_wiring *w, const char *topic)
{
	for (int i = 0; i < w->n_globs; i++)
		if (hg_glob_match(w->globs[i].pattern, topic))
			return 1;
	return 0;
}

const struct hg_wiring_pub *hg_wiring_find_pub(const struct hg_wiring *w, const char *topic)
{
	for (int i = 0; i < w->n_pubs; i++)
		if (strcmp(w->pubs[i].topic, topic) == 0)
			return &w->pubs[i];
	return NULL;
}

const struct hg_wiring_mirror *hg_wiring_match_mirror(const struct hg_wiring *w, const char *topic)
{
	for (int i = 0; i < w->n_mirrors; i++)
		if (hg_glob_match(w->mirrors[i].pattern, topic))
			return &w->mirrors[i];
	return NULL;
}
