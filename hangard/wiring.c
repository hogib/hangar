// SPDX-License-Identifier: GPL-3.0-or-later
/* Wiring text: one line per item, the keys in the order docs/03 shows them,
 * so that `hangar show` prints what the parser read. */
#define _GNU_SOURCE
#include "wiring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "hg_sys.h"

struct out {
	char *p;
	size_t len, used;
	int overflow;
};

static void put(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(struct out *o, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(o->p + o->used, o->len - o->used, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= o->len - o->used) {
		o->overflow = 1;
		o->used = o->len - 1;
		return;
	}
	o->used += (size_t)n;
}

/* Millihertz as the manifest writes a rate: "50", "0.5", "12.345". */
static void put_rate(struct out *o, uint32_t mhz)
{
	if (mhz % 1000 == 0) {
		put(o, "%u", mhz / 1000);
		return;
	}
	char frac[8];
	mf_fmt(frac, sizeof frac, "%03u", mhz % 1000);
	size_t n = strlen(frac);
	while (n > 1 && frac[n - 1] == '0')
		frac[--n] = 0;
	put(o, "%u.%s", mhz / 1000, frac);
}

/* Nanoseconds in the coarsest unit the parser takes that keeps it exact. */
static void put_duration(struct out *o, uint64_t ns)
{
	if (ns % 1000000000u == 0)
		put(o, "%llus", (unsigned long long)(ns / 1000000000u));
	else if (ns % 1000000u == 0)
		put(o, "%llums", (unsigned long long)(ns / 1000000u));
	else if (ns % 1000u == 0)
		put(o, "%lluus", (unsigned long long)(ns / 1000u));
	else
		put(o, "%lluns", (unsigned long long)ns);
}

static void put_type(struct out *o, const struct mf_type *t)
{
	put(o, " type=%s", t->name);
	if (!t->frame)
		put(o, " hash=%016llx", (unsigned long long)t->hash);
}

int wiring_text(const struct mf_set *s, const struct mf_module *m, char *out, size_t len)
{
	struct out o = { out, len, 0, 0 };
	if (len == 0) {
		errno = ENOSPC;
		return -1;
	}
	out[0] = 0;
	put(&o, "hangar-wiring 1\n");
	put(&o, "module %s\n", m->name);
	for (int i = 0; i < m->n_subs; i++) {
		const struct mf_sub *sub = &m->subs[i];
		if (sub->glob) {
			put(&o, "subglob %s%s\n", sub->topic, sub->implicit ? " implicit" : "");
			continue;
		}
		put(&o, "sub %s", sub->topic);
		if (sub->pub_type[0]) {
			put(&o, " type=%s", sub->pub_type);
			if (sub->hash)
				put(&o, " hash=%016llx", (unsigned long long)sub->hash);
		}
		if (sub->rate_mhz) {
			put(&o, " rate=");
			put_rate(&o, sub->rate_mhz);
		}
		if (sub->slots && (sub->implicit || !sub->rate_mhz))
			put(&o, " slots=%u", sub->slots);
		if (sub->implicit)
			put(&o, " implicit");
		put(&o, "\n");
	}
	for (int i = 0; i < m->n_pubs; i++) {
		const struct mf_pub *p = &m->pubs[i];
		const struct mf_type *t = mf_type_at(s, p->type_idx);
		put(&o, "pub %s", p->topic);
		if (t)
			put_type(&o, t);
		else
			put(&o, " type=%s", p->type);
		put(&o, " slots=%u", p->slots);
		if (p->rate_mhz) {
			put(&o, " rate=");
			put_rate(&o, p->rate_mhz);
			put(&o, " stale_after=");
			put_duration(&o, p->stale_after_ns);
		}
		if (p->n_inputs) {
			put(&o, " inputs=");
			for (int k = 0; k < p->n_inputs; k++)
				put(&o, "%s%s", k ? "," : "", p->inputs[k]);
		}
		if (p->n_sources) {
			put(&o, " sources=");
			for (int k = 0; k < p->n_sources; k++)
				put(&o, "%s%s", k ? "," : "", p->sources[k]);
		}
		if (p->implicit)
			put(&o, " implicit");
		put(&o, "\n");
	}
	for (int i = 0; i < m->n_serves; i++) {
		const struct mf_serve *sv = &m->serves[i];
		const struct mf_type *rq = mf_type_at(s, sv->req_idx), *rp = mf_type_at(s, sv->rep_idx);
		if (!rq || !rp)
			continue;
		put(&o, "serve %s req=%s hash=%016llx size=%u rep=%s hash=%016llx size=%u deadline=", sv->method,
		    rq->name, (unsigned long long)rq->hash, rq->payload_bytes, rp->name,
		    (unsigned long long)rp->hash, rp->payload_bytes);
		put_duration(&o, sv->deadline_ns);
		/* The default list, "* admin", unless the module has an actuator
		 * output: then nobody (docs/04). An empty list is no allow= key. */
		if (!sv->allow_set) {
			if (!m->has_actuator_output)
				put(&o, " allow=*,admin");
		} else if (sv->n_allow) {
			put(&o, " allow=");
			for (int k = 0; k < sv->n_allow; k++)
				put(&o, "%s%s", k ? "," : "", sv->allow[k]);
		}
		put(&o, "\n");
	}
	for (int i = 0; i < m->n_calls; i++) {
		const struct mf_call *c = &m->calls[i];
		const struct mf_type *rq = mf_type_at(s, c->req_idx), *rp = mf_type_at(s, c->rep_idx);
		if (!rq || !rp)
			continue;
		put(&o, "call %s.%s req=%s hash=%016llx size=%u rep=%s hash=%016llx size=%u remote=%d\n", c->server,
		    c->method, rq->name, (unsigned long long)rq->hash, rq->payload_bytes, rp->name,
		    (unsigned long long)rp->hash, rp->payload_bytes, c->remote);
	}
	if (o.overflow) {
		errno = ENOSPC;
		return -1;
	}
	return (int)o.used;
}

int wiring_memfd(const char *text, size_t len)
{
	int fd = (int)syscall(SYS_memfd_create, "hangar-wiring", HG_MFD_ALLOW_SEALING);
	if (fd < 0)
		return -1;
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, text + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}
