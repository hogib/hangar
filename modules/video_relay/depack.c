// SPDX-License-Identifier: GPL-3.0-or-later
/* depack: RTP H.264 (RFC 6184) back into access units (depack.h). */
#include "depack.h"

#include <string.h>

void depack_init(struct depack *d, uint8_t pt)
{
	memset(d, 0, sizeof *d);
	d->pt = pt;
	d->need_idr = 1; /* the first frame decoded is a keyframe */
}

static void append(struct depack *d, const uint8_t *p, size_t n)
{
	if (d->len + n > DEPACK_MAX_AU) {
		d->broken = 1; /* larger than any frame we expect: not decoded */
		return;
	}
	memcpy(d->au[d->cur] + d->len, p, n);
	d->len += n;
}

static void nal_start(struct depack *d, uint8_t header)
{
	static const uint8_t sc[4] = { 0, 0, 0, 1 };
	append(d, sc, sizeof sc);
	append(d, &header, 1);
	if ((header & 0x1f) == 5)
		d->has_idr = 1;
}

/* Ends the access unit under way: 1 and *out filled if it is whole and
 * decodable; either way the next one starts empty in the other buffer. */
static int finish(struct depack *d, struct depack_au *out)
{
	int ok = 0;
	if (d->len) {
		if (d->fu_type)
			d->broken = 1; /* an FU-A without its end */
		if (!d->broken && d->has_idr)
			d->need_idr = 0;
		if (!d->broken && !d->need_idr) {
			out->data = d->au[d->cur];
			out->len = d->len;
			out->ts = d->ts;
			out->keyframe = d->has_idr;
			d->frames++;
			d->keyframes += d->has_idr != 0;
			d->cur ^= 1;
			ok = 1;
		} else {
			d->skipped++;
			if (d->broken)
				d->need_idr = 1;
		}
	}
	d->len = 0;
	d->broken = 0;
	d->has_idr = 0;
	d->fu_type = 0;
	d->have_ts = 0;
	return ok;
}

int depack_push(struct depack *d, const uint8_t *pkt, size_t len, struct depack_au out[2])
{
	int n = 0;
	if (len < 12 || (pkt[0] >> 6) != 2) {
		d->bad++;
		return 0;
	}
	size_t off = 12 + 4u * (pkt[0] & 0x0f);
	if (pkt[0] & 0x10) { /* a header extension: its length in 32-bit words */
		if (len < off + 4) {
			d->bad++;
			return 0;
		}
		off += 4 + 4u * (((size_t)pkt[off + 2] << 8) | pkt[off + 3]);
	}
	if (off >= len) { /* the header's own lengths run past the packet */
		d->bad++;
		return 0;
	}
	size_t end = len;
	if (pkt[0] & 0x20) { /* padding: its count is the last byte */
		if (pkt[len - 1] > len - off) {
			d->bad++;
			return 0;
		}
		end -= pkt[len - 1];
	}
	if (off >= end) {
		d->bad++;
		return 0;
	}
	if ((pkt[1] & 0x7f) != d->pt) {
		d->bad++;
		return 0;
	}
	int marker = (pkt[1] & 0x80) != 0;
	uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
	uint32_t ts = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
	uint32_t ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) | ((uint32_t)pkt[10] << 8) | pkt[11];
	d->packets++;

	uint16_t gap = (uint16_t)(seq - d->next_seq);
	if (d->have_seq && (ssrc != d->ssrc || (gap >= 0x8000 && (uint16_t)(0x10000u - gap) > DEPACK_LATE))) {
		/* A new stream: the encoder restarted. What was under way is
		 * dropped, and this one is decoded from its first keyframe. */
		d->resyncs++;
		d->len = 0;
		d->broken = 0;
		d->has_idr = 0;
		d->fu_type = 0;
		d->have_ts = 0;
		d->have_seq = 0;
		d->need_idr = 1;
	}
	d->ssrc = ssrc;
	if (d->have_seq) {
		if (gap >= 0x8000) { /* a little older than the newest: late, or a duplicate */
			d->bad++;
			return 0;
		}
		if (gap) {
			d->lost += gap;
			/* Which frame the lost packets belonged to is not known: the
			 * one under way is broken, and so is this one if it starts a
			 * new frame (its first packets may be what was lost). */
			d->broken = 1;
			d->need_idr = 1;
		}
	}
	d->have_seq = 1;
	d->next_seq = (uint16_t)(seq + 1);

	if (d->have_ts && ts != d->ts) {
		int lost_here = d->broken;
		if (finish(d, &out[n]))
			n++;
		if (lost_here)
			d->broken = 1; /* the gap may have been at this frame's start */
	}
	if (!d->have_ts) {
		d->have_ts = 1;
		d->ts = ts;
	}

	const uint8_t *p = pkt + off;
	size_t plen = end - off;
	uint8_t type = p[0] & 0x1f;
	if (type >= 1 && type <= 23) {
		nal_start(d, p[0]);
		append(d, p + 1, plen - 1);
	} else if (type == 24) { /* STAP-A: 16-bit sizes, then NAL units */
		size_t i = 1;
		while (i + 2 <= plen) {
			size_t s = ((size_t)p[i] << 8) | p[i + 1];
			i += 2;
			if (s == 0 || i + s > plen) {
				d->broken = 1;
				break;
			}
			nal_start(d, p[i]);
			append(d, p + i + 1, s - 1);
			i += s;
		}
	} else if (type == 28 && plen >= 2) { /* FU-A */
		uint8_t fh = p[1];
		uint8_t nal_type = fh & 0x1f;
		if (fh & 0x80) {
			if (d->fu_type)
				d->broken = 1; /* the last one never ended */
			nal_start(d, (uint8_t)((p[0] & 0xe0) | nal_type));
			d->fu_type = nal_type;
		} else if (d->fu_type != nal_type) {
			d->broken = 1; /* a middle or end without its start */
		}
		append(d, p + 2, plen - 2);
		if (fh & 0x40)
			d->fu_type = 0;
	} else {
		d->bad++;
		d->broken = 1;
	}
	if (marker && finish(d, &out[n]))
		n++;
	return n;
}

/* The value nearest to ref whose low 32 bits are ts; ts itself where that
 * would be before zero. */
static uint64_t ts_nearest(uint64_t ref, uint32_t ts)
{
	int64_t d = (int32_t)(ts - (uint32_t)ref);
	if (d < 0 && (uint64_t)-d > ref)
		return ts;
	return d < 0 ? ref - (uint64_t)-d : ref + (uint64_t)d;
}

uint64_t ts_unwrap(struct ts_unwrap *u, uint32_t ts, uint64_t now_ns, uint64_t hint, int *restarted)
{
	uint64_t v = hint != TS_NO_HINT ? ts_nearest(hint, ts) : u->seen ? ts_nearest(u->last, ts) : ts;
	int again = 0;
	if (u->seen) {
		/* 90 kHz ticks in the time between the two arrivals. */
		uint64_t passed = now_ns > u->last_ns ? (now_ns - u->last_ns) / 1000u * 9u / 100u : 0;
		if (v + TS_BACK < u->last || (v > u->last && v - u->last > 2 * passed + TS_SLACK)) {
			again = 1;
			if (hint == TS_NO_HINT)
				v = ts;
		}
	}
	u->seen = 1;
	u->last = v;
	u->last_ns = now_ns;
	if (restarted)
		*restarted = again;
	return v;
}
