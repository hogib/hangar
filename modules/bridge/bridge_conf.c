// SPDX-License-Identifier: GPL-3.0-or-later
/* bridge.conf: see bridge_conf.h and docs/08-bridge.md "Configuration". */
#include "bridge_conf.h"

#include "blake2s.h"
#include "hg_ini.h"
#include "hg_wiring.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ferr(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ferr(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (err && errlen)
		vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

int bc_is_glob(const char *p)
{
	return strpbrk(p, "*?") != NULL;
}

enum { SEC_NONE, SEC_BRIDGE, SEC_MIRROR, SEC_CALLS };

struct walk {
	struct bridge_conf *c;
	int sec;
	struct bc_mirror *m;
	int have_dir, have_expect, have_stale;
};

static int copy_word(char *dst, size_t cap, const char *src, size_t n)
{
	if (n == 0 || n >= cap)
		return -1;
	memcpy(dst, src, n);
	dst[n] = 0;
	return 0;
}

static int finish_mirror(struct walk *w, char *err, size_t errlen)
{
	struct bc_mirror *m = w->m;
	if (!m)
		return 0;
	if (!w->have_dir) {
		ferr(err, errlen, "[Mirror] at line %d: Direction= is required", m->line);
		return -1;
	}
	if (m->priority == BC_SAFETY || m->priority == BC_COMMAND) {
		if (!w->have_expect || !w->have_stale) {
			ferr(err, errlen, "[Mirror] at line %d: a safety or command mirror needs ExpectRate= and StaleAfter=",
			     m->line);
			return -1;
		}
		for (int i = 0; i < m->n_patterns; i++)
			if (bc_is_glob(m->patterns[i])) {
				ferr(err, errlen, "[Mirror] at line %d: a safety or command mirror names its topics (%s is a glob)",
				     m->line, m->patterns[i]);
				return -1;
			}
	}
	if (m->max_age_ns && m->priority != BC_COMMAND) {
		ferr(err, errlen, "[Mirror] at line %d: MaxAge= is for command mirrors", m->line);
		return -1;
	}
	if (m->expect_mhz && !m->stale_after_ns)
		m->stale_after_ns = 3 * 1000000000000ull / m->expect_mhz;
	w->m = NULL;
	return 0;
}

static int on_section(void *ctx, const char *section, const char *arg, int line, char *err, size_t errlen)
{
	struct walk *w = ctx;
	if (finish_mirror(w, err, errlen) < 0)
		return -1;
	if (strcmp(section, "Bridge") == 0 && !*arg) {
		w->sec = SEC_BRIDGE;
	} else if (strcmp(section, "Calls") == 0 && !*arg) {
		w->sec = SEC_CALLS;
	} else if (strcmp(section, "Mirror") == 0) {
		if (w->c->n_mirrors == BC_MAX_MIRRORS) {
			ferr(err, errlen, "more than %d [Mirror] sections", BC_MAX_MIRRORS);
			return -1;
		}
		struct bc_mirror *m = &w->c->mirrors[w->c->n_mirrors++];
		memset(m, 0, sizeof *m);
		m->priority = BC_TELEMETRY;
		m->line = line;
		const char *p = arg;
		while (*p) {
			while (*p == ' ' || *p == '\t')
				p++;
			const char *q = p;
			while (*q && *q != ' ' && *q != '\t')
				q++;
			if (q == p)
				break;
			if (m->n_patterns == BC_MAX_PATTERNS ||
			    copy_word(m->patterns[m->n_patterns], BC_NAME, p, (size_t)(q - p)) < 0) {
				ferr(err, errlen, "[Mirror]: too many topics, or one too long");
				return -1;
			}
			m->n_patterns++;
			p = q;
		}
		if (!m->n_patterns) {
			ferr(err, errlen, "[Mirror] names no topic");
			return -1;
		}
		w->sec = SEC_MIRROR;
		w->m = m;
		w->have_dir = w->have_expect = w->have_stale = 0;
	} else {
		ferr(err, errlen, "unknown section [%s%s%s]", section, *arg ? " " : "", arg);
		return -1;
	}
	return 0;
}

static int rate_of(const char *v, uint32_t *out)
{
	int64_t r = hg_parse_rate_mhz(v);
	if (r < 0 || r > 1000000000)
		return -1;
	*out = (uint32_t)r;
	return 0;
}

static int on_key(void *ctx, const char *key, const char *value, int line, char *err, size_t errlen)
{
	(void)line;
	struct walk *w = ctx;
	struct bridge_conf *c = w->c;
	if (w->sec == SEC_BRIDGE) {
		if (strcmp(key, "Pair") == 0) {
			if (copy_word(c->pair, sizeof c->pair, value, strlen(value)) < 0)
				goto bad;
		} else if (strcmp(key, "Key") == 0) {
			if (copy_word(c->key_path, sizeof c->key_path, value, strlen(value)) < 0)
				goto bad;
		} else if (strcmp(key, "Port") == 0 || strcmp(key, "DiscoveryPort") == 0) {
			char *e;
			long p = strtol(value, &e, 10);
			if (*e || p < 1 || p > 65535)
				goto bad;
			if (key[0] == 'P')
				c->port = (uint16_t)p;
			else
				c->discovery_port = (uint16_t)p;
		} else if (strcmp(key, "Peer") == 0) {
			if (copy_word(c->peer, sizeof c->peer, value, strlen(value)) < 0)
				goto bad;
		} else if (strcmp(key, "MaxRate") == 0) {
			char *e;
			double r = strtod(value, &e);
			double mul = strcmp(e, "Mbit") == 0 ? 1e6 : strcmp(e, "kbit") == 0 ? 1e3 : strcmp(e, "bit") == 0 ? 1 : -1;
			if (r <= 0 || mul < 0)
				goto bad;
			c->max_rate_bps = (uint64_t)(r * mul);
		} else if (strcmp(key, "MsgDir") == 0) {
			if (copy_word(c->msg_dir, sizeof c->msg_dir, value, strlen(value)) < 0)
				goto bad;
		} else {
			ferr(err, errlen, "[Bridge] has no key %s", key);
			return -1;
		}
		return 0;
	}
	if (w->sec == SEC_CALLS) {
		if (strcmp(key, "Remote") != 0) {
			ferr(err, errlen, "[Calls] has no key %s", key);
			return -1;
		}
		const char *p = value;
		while (*p) {
			while (*p == ' ')
				p++;
			const char *q = p;
			while (*q && *q != ' ')
				q++;
			if (q == p)
				break;
			if (c->n_remote == BC_MAX_REMOTE ||
			    copy_word(c->remote[c->n_remote], BC_NAME, p, (size_t)(q - p)) < 0 ||
			    !strchr(c->remote[c->n_remote], '.')) {
				ferr(err, errlen, "Remote=: too many entries, or one not <module>.<method>");
				return -1;
			}
			c->n_remote++;
			p = q;
		}
		return 0;
	}
	if (w->sec == SEC_MIRROR) {
		struct bc_mirror *m = w->m;
		if (strcmp(key, "Direction") == 0) {
			if (strcmp(value, "up") == 0)
				m->up = 1;
			else if (strcmp(value, "down") == 0)
				m->up = 0;
			else
				goto bad;
			w->have_dir = 1;
		} else if (strcmp(key, "Delivery") == 0) {
			if (strcmp(value, "reliable") == 0)
				m->reliable = 1;
			else if (strcmp(value, "latest") == 0)
				m->reliable = 0;
			else
				goto bad;
		} else if (strcmp(key, "Priority") == 0) {
			static const char *names[] = { "safety", "command", "call", "telemetry", "dataset", "bulk" };
			int i;
			for (i = 0; i < 6; i++)
				if (strcmp(value, names[i]) == 0)
					break;
			if (i == 6)
				goto bad;
			m->priority = i;
		} else if (strcmp(key, "Rate") == 0) {
			if (rate_of(value, &m->rate_mhz) < 0)
				goto bad;
		} else if (strcmp(key, "ExpectRate") == 0) {
			if (rate_of(value, &m->expect_mhz) < 0)
				goto bad;
			w->have_expect = 1;
		} else if (strcmp(key, "StaleAfter") == 0 || strcmp(key, "MaxAge") == 0) {
			int64_t d = hg_ini_parse_duration(value);
			if (d <= 0)
				goto bad;
			if (key[0] == 'S') {
				m->stale_after_ns = (uint64_t)d;
				w->have_stale = 1;
			} else {
				m->max_age_ns = (uint64_t)d;
			}
		} else {
			ferr(err, errlen, "[Mirror] has no key %s", key);
			return -1;
		}
		return 0;
	}
	ferr(err, errlen, "%s= before any section", key);
	return -1;
bad:
	ferr(err, errlen, "%s=%s: not a valid value", key, value);
	return -1;
}

/* 1 if the line [k, j) sets one of the keys that are each machine's own: Peer
 * (docs/08), and, in this stage, the key file's and MsgDir's paths (the key
 * itself is proved by the handshake, the msg/ tree by its own hash) and the
 * ports (two bridges on one host, in the tests, cannot share one). */
static int local_key(const char *text, size_t k, size_t j)
{
	static const char *keys[] = { "Peer", "Key", "MsgDir", "Port", "DiscoveryPort" };
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		size_t n = strlen(keys[i]);
		if (j - k > n && strncmp(text + k, keys[i], n) == 0 &&
		    (text[k + n] == '=' || text[k + n] == ' ' || text[k + n] == '\t'))
			return 1;
	}
	return 0;
}

/* The configuration hash: of the text, without those lines. */
static void conf_hash(const char *text, size_t len, unsigned char out[32])
{
	struct blake2s s;
	blake2s_init(&s, 32, NULL, 0);
	size_t i = 0;
	while (i < len) {
		size_t j = i;
		while (j < len && text[j] != '\n')
			j++;
		size_t k = i;
		while (k < j && (text[k] == ' ' || text[k] == '\t'))
			k++;
		if (!local_key(text, k, j))
			blake2s_update(&s, text + i, j - i + (j < len));
		i = j + 1;
	}
	blake2s_final(&s, out);
}

int bc_parse(const char *text, size_t len, struct bridge_conf *c, char *err, size_t errlen)
{
	memset(c, 0, sizeof *c);
	snprintf(c->pair, sizeof c->pair, "default");
	snprintf(c->key_path, sizeof c->key_path, "/etc/hangar/bridge.key");
	snprintf(c->peer, sizeof c->peer, "auto");
	snprintf(c->msg_dir, sizeof c->msg_dir, "/usr/share/hangar/msg");
	c->port = 5614;
	c->discovery_port = 5613;
	c->max_rate_bps = 20000000;
	struct walk w = { .c = c };
	if (hg_ini_parse(text, len, on_section, on_key, &w, err, errlen) < 0)
		return -1;
	if (finish_mirror(&w, err, errlen) < 0) {
		errno = EINVAL;
		return -1;
	}
	conf_hash(text, len, c->hash);
	return 0;
}

int bc_load(const char *path, struct bridge_conf *c, char *err, size_t errlen)
{
	static char buf[65536];
	size_t len;
	if (hg_ini_read_file(path, buf, sizeof buf, &len) < 0) {
		ferr(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	char e[200];
	if (bc_parse(buf, len, c, e, sizeof e) < 0) {
		ferr(err, errlen, "%s: %s", path, e);
		return -1;
	}
	return 0;
}

int bc_remote_matches(const struct bridge_conf *c, const char *module, const char *method)
{
	char full[2 * BC_NAME];
	snprintf(full, sizeof full, "%s.%s", module, method);
	for (int i = 0; i < c->n_remote; i++)
		if (hg_glob_match(c->remote[i], full))
			return 1;
	return 0;
}

const struct bc_mirror *bc_find(const struct bridge_conf *c, const char *topic, int up)
{
	const struct bc_mirror *glob = NULL;
	for (int i = 0; i < c->n_mirrors; i++) {
		const struct bc_mirror *m = &c->mirrors[i];
		if (m->up != up)
			continue;
		for (int k = 0; k < m->n_patterns; k++) {
			if (!bc_is_glob(m->patterns[k])) {
				if (strcmp(m->patterns[k], topic) == 0)
					return m;
			} else if (!glob && hg_glob_match(m->patterns[k], topic)) {
				glob = m;
			}
		}
	}
	return glob;
}

static int hexval(int ch)
{
	return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
						 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

int bc_load_key(const char *path, unsigned char key[32], char *err, size_t errlen)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		ferr(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	unsigned char buf[80];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	if (n == 32) {
		memcpy(key, buf, 32);
		return 0;
	}
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	if (n != 64) {
		ferr(err, errlen, "%s: 32 raw bytes or 64 hex digits", path);
		return -1;
	}
	for (int i = 0; i < 32; i++) {
		int hi = hexval(buf[2 * i]), lo = hexval(buf[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			ferr(err, errlen, "%s: not hexadecimal", path);
			return -1;
		}
		key[i] = (unsigned char)(hi << 4 | lo);
	}
	return 0;
}
