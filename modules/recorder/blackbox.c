// SPDX-License-Identifier: GPL-3.0-or-later
/* The black box (docs/06 "The black box"): one ring of records per topic in
 * RAM, budgets granted class by class within BlackBoxMemory, handed to the
 * writer whole when a dump is due. */
#include "recorder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct hgr_record *bb_at(const struct bb_ring *r, uint32_t i)
{
	if (i >= r->count)
		return NULL;
	uint32_t slot = (r->head + i) % r->cap;
	return (const struct hgr_record *)(const void *)(r->buf + (size_t)slot * r->rec_bytes);
}

void bb_push(struct bb_ring *r, const struct hgr_record *rec, const void *payload)
{
	r->pushed++;
	if (!r->buf || !r->cap)
		return;
	uint32_t slot;
	if (r->count < r->cap) {
		slot = (r->head + r->count) % r->cap;
		r->count++;
	} else {
		slot = r->head;
		r->head = (r->head + 1) % r->cap;
	}
	unsigned char *dst = r->buf + (size_t)slot * r->rec_bytes;
	memcpy(dst, rec, HGR_RECORD_HEADER);
	if (r->rec_bytes > HGR_RECORD_HEADER)
		memcpy(dst + HGR_RECORD_HEADER, payload, r->rec_bytes - HGR_RECORD_HEADER);
}

uint64_t bb_reach_ns(const struct bb_ring *r)
{
	if (r->count < 2)
		return 0;
	const struct hgr_record *o = bb_at(r, 0), *n = bb_at(r, r->count - 1);
	return n->publish_ns > o->publish_ns ? n->publish_ns - o->publish_ns : 0;
}

/* Resizes a ring to cap entries, keeping the newest that fit. */
static int bb_resize(struct bb_ring *r, uint32_t cap)
{
	if (cap == r->cap)
		return 0;
	unsigned char *nb = NULL;
	if (cap) {
		nb = malloc((size_t)cap * r->rec_bytes);
		if (!nb)
			return -1;
	}
	uint32_t keep = r->count < cap ? r->count : cap;
	for (uint32_t i = 0; i < keep; i++) {
		const struct hgr_record *rec = bb_at(r, r->count - keep + i);
		memcpy(nb + (size_t)i * r->rec_bytes, rec, r->rec_bytes);
	}
	free(r->buf);
	r->buf = nb;
	r->cap = cap;
	r->head = 0;
	r->count = keep;
	return 0;
}

int bb_grant(struct bb_ring *rings, int n, uint64_t mem)
{
	uint64_t left = mem;
	int rc = 0;
	for (int cls = REC_BB_HIGH; cls >= REC_BB_LOW; cls--) {
		uint64_t sum = 0;
		for (int i = 0; i < n; i++)
			if (rings[i].wanted && (int)rings[i].class == cls)
				sum += rings[i].budget * rings[i].rec_bytes;
		for (int i = 0; i < n; i++) {
			if (!rings[i].wanted || (int)rings[i].class != cls)
				continue;
			uint64_t grant;
			if (sum <= left)
				grant = rings[i].budget;
			else if (sum)
				grant = rings[i].budget * left / sum;
			else
				grant = 0;
			if (grant > 0xffffffffu)
				grant = 0xffffffffu;
			if (bb_resize(&rings[i], (uint32_t)grant) < 0)
				rc = -1;
		}
		left = sum <= left ? left - sum : 0;
	}
	for (int i = 0; i < n; i++)
		if (!rings[i].wanted || rings[i].class == REC_BB_OFF)
			bb_resize(&rings[i], 0);
	return rc;
}

struct bb_dump *bb_take(struct bb_ring *rings, const struct hgr_header *hdrs, int n, const char *dir,
			const char *ini)
{
	struct bb_dump *d = calloc(1, sizeof *d + (size_t)n * sizeof d->topics[0]);
	if (!d)
		return NULL;
	snprintf(d->dir, sizeof d->dir, "%s", dir);
	snprintf(d->ini, sizeof d->ini, "%s", ini);
	for (int i = 0; i < n; i++) {
		if (!rings[i].buf || !rings[i].count)
			continue;
		struct bb_dump_topic *t = &d->topics[d->n++];
		t->hdr = hdrs[i];
		t->ring = rings[i];
		unsigned char *fresh = malloc((size_t)rings[i].cap * rings[i].rec_bytes);
		rings[i].buf = fresh;
		rings[i].head = 0;
		rings[i].count = 0;
		if (!fresh)
			rings[i].cap = 0; /* the next grant restores it */
	}
	return d;
}

void bb_dump_free(struct bb_dump *d)
{
	if (!d)
		return;
	for (int i = 0; i < d->n; i++)
		free(d->topics[i].ring.buf);
	free(d);
}
