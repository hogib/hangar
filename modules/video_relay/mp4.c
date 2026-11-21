// SPDX-License-Identifier: GPL-3.0-or-later
/* mp4: a fragmented MP4 written in process (mp4.h). ISO/IEC 14496-12 boxes,
 * 14496-15 for the avc1 sample entry and its avcC. */
#define _POSIX_C_SOURCE 200809L
#include "mp4.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- The SPS's size (ITU-T H.264 7.3.2.1.1) --------------------------------- */

struct bits {
	uint8_t b[256];
	size_t n, pos; /* in bits */
	int bad;
};

static unsigned u1(struct bits *r)
{
	if (r->pos >= r->n) {
		r->bad = 1;
		return 0;
	}
	unsigned v = (r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
	r->pos++;
	return v;
}

static unsigned un(struct bits *r, int n)
{
	unsigned v = 0;
	while (n--)
		v = v << 1 | u1(r);
	return v;
}

static unsigned ue(struct bits *r)
{
	int zeros = 0;
	while (!u1(r) && !r->bad)
		if (++zeros > 31) {
			r->bad = 1;
			return 0;
		}
	return zeros ? (1u << zeros) - 1 + un(r, zeros) : 0;
}

static int se(struct bits *r)
{
	unsigned k = ue(r);
	return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2);
}

int h264_sps_size(const uint8_t *sps, size_t len, uint32_t *width, uint32_t *height)
{
	struct bits r = { .n = 0 };
	/* The RBSP: the NAL header dropped, emulation prevention bytes removed. */
	size_t o = 0;
	for (size_t i = 1; i < len && o < sizeof r.b; i++) {
		if (i >= 3 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0)
			continue;
		r.b[o++] = sps[i];
	}
	r.n = o * 8;
	unsigned profile = un(&r, 8);
	un(&r, 16); /* constraint flags, level */
	ue(&r);     /* seq_parameter_set_id */
	unsigned chroma = 1;
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
	    profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
	    profile == 135) {
		chroma = ue(&r);
		if (chroma == 3)
			u1(&r);
		ue(&r);
		ue(&r);
		u1(&r);
		if (u1(&r)) { /* seq_scaling_matrix_present_flag */
			for (int i = 0; i < (chroma != 3 ? 8 : 12); i++) {
				if (!u1(&r))
					continue;
				int size = i < 6 ? 16 : 64, last = 8, next = 8;
				for (int j = 0; j < size; j++) {
					if (next)
						next = (last + se(&r) + 256) % 256;
					last = next ? next : last;
				}
			}
		}
	}
	ue(&r); /* log2_max_frame_num_minus4 */
	unsigned poc = ue(&r);
	if (poc == 0) {
		ue(&r);
	} else if (poc == 1) {
		u1(&r);
		se(&r);
		se(&r);
		unsigned k = ue(&r);
		for (unsigned i = 0; i < k && !r.bad; i++)
			se(&r);
	}
	ue(&r); /* max_num_ref_frames */
	u1(&r);
	unsigned w_mbs = ue(&r) + 1, h_units = ue(&r) + 1;
	unsigned frame_mbs_only = u1(&r);
	if (!frame_mbs_only)
		u1(&r);
	u1(&r);
	unsigned cl = 0, cr = 0, ct = 0, cb = 0;
	if (u1(&r)) {
		cl = ue(&r);
		cr = ue(&r);
		ct = ue(&r);
		cb = ue(&r);
	}
	if (r.bad || chroma > 3)
		return -1;
	unsigned cx = chroma == 1 || chroma == 2 ? 2 : 1;
	unsigned cy = (chroma == 1 ? 2 : 1) * (2 - frame_mbs_only);
	uint64_t w = (uint64_t)w_mbs * 16, h = (uint64_t)(2 - frame_mbs_only) * h_units * 16;
	if ((uint64_t)cx * (cl + cr) >= w || (uint64_t)cy * (ct + cb) >= h)
		return -1;
	*width = (uint32_t)(w - (uint64_t)cx * (cl + cr));
	*height = (uint32_t)(h - (uint64_t)cy * (ct + cb));
	return 0;
}

/* ---- Annex B ------------------------------------------------------------------- */

/* The next NAL unit at or after *pos: 1, *nal and *nlen filled, *pos past it. */
static int next_nal(const uint8_t *au, size_t len, size_t *pos, const uint8_t **nal, size_t *nlen)
{
	size_t i = *pos;
	while (i + 3 <= len && !(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1))
		i++;
	if (i + 3 > len)
		return 0;
	size_t s = i + 3, e = s;
	while (e + 3 <= len && !(au[e] == 0 && au[e + 1] == 0 && (au[e + 2] == 1 || (au[e + 2] == 0 && e + 3 < len && au[e + 3] == 1))))
		e++;
	if (e + 3 > len)
		e = len;
	*nal = au + s;
	*nlen = e - s;
	*pos = e;
	return *nlen > 0;
}

/* ---- Boxes --------------------------------------------------------------------- */

struct out {
	uint8_t *p;
	size_t len, cap;
	int oom;
};

static void put(struct out *o, const void *p, size_t n)
{
	if (o->len + n > o->cap) {
		size_t c = o->cap ? o->cap : 4096;
		while (c < o->len + n)
			c *= 2;
		uint8_t *q = realloc(o->p, c);
		if (!q) {
			o->oom = 1;
			return;
		}
		o->p = q;
		o->cap = c;
	}
	memcpy(o->p + o->len, p, n);
	o->len += n;
}

static void p8(struct out *o, uint8_t v) { put(o, &v, 1); }
static void p16(struct out *o, uint16_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; put(o, b, 2); }
static void p32(struct out *o, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
	put(o, b, 4);
}
static void p64(struct out *o, uint64_t v) { p32(o, (uint32_t)(v >> 32)); p32(o, (uint32_t)v); }
static void pz(struct out *o, size_t n) { while (n--) p8(o, 0); }

/* A box: its size patched when it is closed. */
static size_t box(struct out *o, const char *type)
{
	size_t at = o->len;
	p32(o, 0);
	put(o, type, 4);
	return at;
}

static size_t fullbox(struct out *o, const char *type, uint8_t version, uint32_t flags)
{
	size_t at = box(o, type);
	p32(o, (uint32_t)version << 24 | (flags & 0xffffff));
	return at;
}

static void close_box(struct out *o, size_t at)
{
	if (o->oom)
		return;
	uint32_t n = (uint32_t)(o->len - at);
	o->p[at] = (uint8_t)(n >> 24);
	o->p[at + 1] = (uint8_t)(n >> 16);
	o->p[at + 2] = (uint8_t)(n >> 8);
	o->p[at + 3] = (uint8_t)n;
}

static const uint32_t MATRIX[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };

static void header(struct out *o, const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len,
		   uint32_t w, uint32_t h, uint32_t timescale)
{
	size_t b = box(o, "ftyp");
	put(o, "isom", 4);
	p32(o, 512);
	put(o, "isomiso2iso6avc1mp41", 20);
	close_box(o, b);

	size_t moov = box(o, "moov");
	b = fullbox(o, "mvhd", 0, 0);
	p32(o, 0);
	p32(o, 0);
	p32(o, timescale);
	p32(o, 0); /* duration: fragments carry it */
	p32(o, 0x10000);
	p16(o, 0x100);
	pz(o, 10);
	for (int i = 0; i < 9; i++)
		p32(o, MATRIX[i]);
	pz(o, 24);
	p32(o, 2); /* next_track_ID */
	close_box(o, b);

	size_t trak = box(o, "trak");
	b = fullbox(o, "tkhd", 0, 3);
	p32(o, 0);
	p32(o, 0);
	p32(o, 1); /* track_ID */
	p32(o, 0);
	p32(o, 0);
	pz(o, 8);
	p16(o, 0);
	p16(o, 0);
	p16(o, 0);
	p16(o, 0);
	for (int i = 0; i < 9; i++)
		p32(o, MATRIX[i]);
	p32(o, w << 16);
	p32(o, h << 16);
	close_box(o, b);

	size_t mdia = box(o, "mdia");
	b = fullbox(o, "mdhd", 0, 0);
	p32(o, 0);
	p32(o, 0);
	p32(o, timescale);
	p32(o, 0);
	p16(o, 0x55c4); /* "und" */
	p16(o, 0);
	close_box(o, b);
	b = fullbox(o, "hdlr", 0, 0);
	p32(o, 0);
	put(o, "vide", 4);
	pz(o, 12);
	put(o, "hangar video_relay", 19);
	close_box(o, b);

	size_t minf = box(o, "minf");
	b = fullbox(o, "vmhd", 0, 1);
	pz(o, 8);
	close_box(o, b);
	size_t dinf = box(o, "dinf");
	size_t dref = fullbox(o, "dref", 0, 0);
	p32(o, 1);
	b = fullbox(o, "url ", 0, 1); /* the data is in this file */
	close_box(o, b);
	close_box(o, dref);
	close_box(o, dinf);

	size_t stbl = box(o, "stbl");
	size_t stsd = fullbox(o, "stsd", 0, 0);
	p32(o, 1);
	size_t avc1 = box(o, "avc1");
	pz(o, 6);
	p16(o, 1); /* data_reference_index */
	pz(o, 16);
	p16(o, (uint16_t)w);
	p16(o, (uint16_t)h);
	p32(o, 0x00480000);
	p32(o, 0x00480000);
	p32(o, 0);
	p16(o, 1);
	pz(o, 32); /* compressorname */
	p16(o, 0x18);
	p16(o, 0xffff);
	size_t avcc = box(o, "avcC");
	p8(o, 1);
	p8(o, sps[1]); /* profile, compatibility, level */
	p8(o, sps[2]);
	p8(o, sps[3]);
	p8(o, 0xff); /* 4-byte NAL lengths */
	p8(o, 0xe1); /* one SPS */
	p16(o, (uint16_t)sps_len);
	put(o, sps, sps_len);
	p8(o, 1);
	p16(o, (uint16_t)pps_len);
	put(o, pps, pps_len);
	close_box(o, avcc);
	close_box(o, avc1);
	close_box(o, stsd);
	b = fullbox(o, "stts", 0, 0);
	p32(o, 0);
	close_box(o, b);
	b = fullbox(o, "stsc", 0, 0);
	p32(o, 0);
	close_box(o, b);
	b = fullbox(o, "stsz", 0, 0);
	p32(o, 0);
	p32(o, 0);
	close_box(o, b);
	b = fullbox(o, "stco", 0, 0);
	p32(o, 0);
	close_box(o, b);
	close_box(o, stbl);
	close_box(o, minf);
	close_box(o, mdia);
	close_box(o, trak);

	size_t mvex = box(o, "mvex");
	b = fullbox(o, "trex", 0, 0);
	p32(o, 1);
	p32(o, 1);
	p32(o, 0);
	p32(o, 0);
	p32(o, 0);
	close_box(o, b);
	close_box(o, mvex);
	close_box(o, moov);
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += k;
		n -= (size_t)k;
	}
	return 0;
}

int mp4_open(struct mp4w *w, const char *path)
{
	memset(w, 0, sizeof *w);
	w->fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	w->timescale = 90000;
	w->seq = 1;
	return w->fd < 0 ? -1 : 0;
}

int mp4_open_new(struct mp4w *w, const char *dir, const char *stem, char *path, size_t len)
{
	for (int k = 1; k <= 9999; k++) {
		int n = k == 1 ? snprintf(path, len, "%s/%s.mp4", dir, stem) : snprintf(path, len, "%s/%s-%d.mp4", dir, stem, k);
		if (n < 0 || (size_t)n >= len) {
			w->fd = -1;
			errno = ENAMETOOLONG;
			return -1;
		}
		if (mp4_open(w, path) == 0)
			return 0;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

int mp4_write(struct mp4w *w, const uint8_t *au, size_t len, uint64_t ts, uint32_t duration, int keyframe)
{
	if (w->fd < 0) {
		errno = EBADF;
		return -1;
	}
	struct out o = { .p = w->buf, .cap = w->cap };
	if (!w->started) {
		if (!keyframe)
			return 0;
		const uint8_t *sps = NULL, *pps = NULL, *nal;
		size_t sl = 0, pl = 0, nl, pos = 0;
		while (next_nal(au, len, &pos, &nal, &nl)) {
			if ((nal[0] & 0x1f) == 7 && !sps) {
				sps = nal;
				sl = nl;
			} else if ((nal[0] & 0x1f) == 8 && !pps) {
				pps = nal;
				pl = nl;
			}
		}
		if (!sps || !pps || sl < 4 || h264_sps_size(sps, sl, &w->width, &w->height) < 0)
			return 0;
		header(&o, sps, sl, pps, pl, w->width, w->height, w->timescale);
		w->started = 1;
		w->base_ts = ts;
	}
	/* The sample: each NAL unit with a 4-byte length, access unit delimiters
	 * left out (the sample is the access unit). */
	size_t moof = box(&o, "moof");
	size_t b = fullbox(&o, "mfhd", 0, 0);
	p32(&o, w->seq++);
	close_box(&o, b);
	size_t traf = box(&o, "traf");
	b = fullbox(&o, "tfhd", 0, 0x020000); /* default-base-is-moof */
	p32(&o, 1);
	close_box(&o, b);
	b = fullbox(&o, "tfdt", 1, 0);
	p64(&o, ts - w->base_ts);
	close_box(&o, b);
	size_t trun = fullbox(&o, "trun", 0, 0x000701); /* data offset, duration, size, flags */
	p32(&o, 1);
	size_t data_offset_at = o.len;
	p32(&o, 0);
	p32(&o, duration);
	size_t size_at = o.len;
	p32(&o, 0);
	p32(&o, keyframe ? 0x02000000u : 0x01010000u); /* sync / depends on others, non-sync */
	close_box(&o, trun);
	close_box(&o, traf);
	close_box(&o, moof);
	size_t mdat = box(&o, "mdat");
	size_t sample = o.len, pos = 0, nl;
	const uint8_t *nal;
	while (next_nal(au, len, &pos, &nal, &nl)) {
		if ((nal[0] & 0x1f) == 9)
			continue;
		p32(&o, (uint32_t)nl);
		put(&o, nal, nl);
	}
	close_box(&o, mdat);
	if (o.oom) {
		w->buf = o.p;
		w->cap = o.cap;
		errno = ENOMEM;
		return -1;
	}
	uint32_t ss = (uint32_t)(o.len - sample), off = (uint32_t)(sample - moof);
	uint8_t *p = o.p + data_offset_at;
	p[0] = (uint8_t)(off >> 24), p[1] = (uint8_t)(off >> 16), p[2] = (uint8_t)(off >> 8), p[3] = (uint8_t)off;
	p = o.p + size_at;
	p[0] = (uint8_t)(ss >> 24), p[1] = (uint8_t)(ss >> 16), p[2] = (uint8_t)(ss >> 8), p[3] = (uint8_t)ss;
	w->buf = o.p;
	w->cap = o.cap;
	if (write_all(w->fd, o.p, o.len) < 0)
		return -1;
	w->frames++;
	w->bytes += o.len;
	return 1;
}

void mp4_close(struct mp4w *w)
{
	if (w->fd >= 0)
		close(w->fd);
	w->fd = -1;
	free(w->buf);
	w->buf = NULL;
	w->cap = 0;
}
