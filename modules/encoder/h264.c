// SPDX-License-Identifier: GPL-3.0-or-later
#include "h264.h"

#include <string.h>

/* The offset of the next start code's first byte at or after i, and its
 * length (3 or 4) in *sc; len if none. */
static size_t next_start(const uint8_t *b, size_t len, size_t i, size_t *sc)
{
	for (; i + 3 <= len; i++) {
		if (b[i] != 0 || b[i + 1] != 0)
			continue;
		if (b[i + 2] == 1) {
			*sc = 3;
			return i;
		}
		if (b[i + 2] == 0 && i + 4 <= len && b[i + 3] == 1) {
			*sc = 4;
			return i;
		}
	}
	*sc = 0;
	return len;
}

int h264_nals(const uint8_t *buf, size_t len, struct h264_nal *out, int max)
{
	size_t sc, i = next_start(buf, len, 0, &sc);
	int n = 0;
	while (i < len) {
		size_t start = i + sc, sc2;
		size_t end = next_start(buf, len, start, &sc2);
		size_t e = end;
		while (e > start && buf[e - 1] == 0)
			e--; /* trailing_zero_8bits, or the leading zero of a 4-byte start code */
		if (e > start) {
			if (n < max) {
				out[n].p = buf + start;
				out[n].len = e - start;
				out[n].type = buf[start] & 0x1f;
			}
			n++;
		}
		i = end;
		sc = sc2;
	}
	return n;
}

static int is_slice(uint8_t t)
{
	return t >= 1 && t <= 5;
}

size_t h264_au_len(const uint8_t *buf, size_t len)
{
	size_t sc, i = next_start(buf, len, 0, &sc);
	if (i >= len)
		return 0;
	int seen_slice = 0;
	while (i < len) {
		size_t start = i + sc, sc2;
		if (start >= len)
			break;
		uint8_t t = buf[start] & 0x1f;
		if (seen_slice) {
			int first_mb_zero = is_slice(t) && start + 1 < len && (buf[start + 1] & 0x80);
			if (t == H264_NAL_AUD || t == H264_NAL_SEI || t == H264_NAL_SPS || t == H264_NAL_PPS ||
			    (t >= 14 && t <= 18) || first_mb_zero)
				return i;
		}
		if (is_slice(t))
			seen_slice = 1;
		i = next_start(buf, len, start, &sc2);
		sc = sc2;
	}
	return len;
}

void rtp_h264_init(struct rtp_h264 *r, uint32_t ssrc, uint8_t pt, size_t max_packet)
{
	memset(r, 0, sizeof *r);
	r->ssrc = ssrc;
	r->pt = pt;
	if (max_packet < RTP_HEADER + 3)
		max_packet = RTP_HEADER + 3;
	if (max_packet > RTP_MAX_PACKET)
		max_packet = RTP_MAX_PACKET;
	r->max_packet = max_packet;
}

static void header(struct rtp_h264 *r, uint8_t *pkt, uint32_t ts, int marker)
{
	pkt[0] = 0x80; /* version 2, no padding, no extension, no CSRC */
	pkt[1] = (uint8_t)((marker ? 0x80 : 0) | (r->pt & 0x7f));
	pkt[2] = (uint8_t)(r->seq >> 8);
	pkt[3] = (uint8_t)r->seq;
	pkt[4] = (uint8_t)(ts >> 24);
	pkt[5] = (uint8_t)(ts >> 16);
	pkt[6] = (uint8_t)(ts >> 8);
	pkt[7] = (uint8_t)ts;
	pkt[8] = (uint8_t)(r->ssrc >> 24);
	pkt[9] = (uint8_t)(r->ssrc >> 16);
	pkt[10] = (uint8_t)(r->ssrc >> 8);
	pkt[11] = (uint8_t)r->ssrc;
}

static void emit(struct rtp_h264 *r, uint8_t *pkt, size_t len, rtp_send_fn send, void *ctx)
{
	send(ctx, pkt, len);
	r->seq++; /* wraps at 65535, as RTP's does */
	r->packets++;
	r->bytes += len;
}

/* One NAL unit: a single NAL packet, or FU-A fragments. */
static void send_nal(struct rtp_h264 *r, const uint8_t *nal, size_t len, uint32_t ts, int last, rtp_send_fn send,
		     void *ctx)
{
	uint8_t pkt[RTP_MAX_PACKET];
	if (RTP_HEADER + len <= r->max_packet) {
		header(r, pkt, ts, last);
		memcpy(pkt + RTP_HEADER, nal, len);
		emit(r, pkt, RTP_HEADER + len, send, ctx);
		return;
	}
	size_t room = r->max_packet - RTP_HEADER - 2;
	uint8_t indicator = (uint8_t)((nal[0] & 0xe0) | H264_NAL_FU_A);
	uint8_t type = nal[0] & 0x1f;
	size_t off = 1; /* the NAL header travels in the FU indicator and header */
	while (off < len) {
		size_t chunk = len - off < room ? len - off : room;
		int start = off == 1, end = off + chunk == len;
		header(r, pkt, ts, last && end);
		pkt[RTP_HEADER] = indicator;
		pkt[RTP_HEADER + 1] = (uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | type);
		memcpy(pkt + RTP_HEADER + 2, nal + off, chunk);
		emit(r, pkt, RTP_HEADER + 2 + chunk, send, ctx);
		off += chunk;
	}
}

static void cache(uint8_t *dst, size_t *dst_len, const struct h264_nal *n)
{
	if (n->len <= H264_PARAM_MAX) {
		memcpy(dst, n->p, n->len);
		*dst_len = n->len;
	}
}

int rtp_h264_send_au(struct rtp_h264 *r, const uint8_t *au, size_t len, uint32_t ts, rtp_send_fn send, void *ctx,
		     uint32_t *bytes)
{
	struct h264_nal nals[64];
	int n = h264_nals(au, len, nals, 64);
	if (n > 64)
		n = 64; /* a camera frame of more than 64 NAL units: the rest are not sent (README) */
	if (n <= 0)
		return -1;
	int has_sps = 0, has_pps = 0, idr = 0;
	for (int i = 0; i < n; i++) {
		if (nals[i].type == H264_NAL_SPS) {
			has_sps = 1;
			cache(r->sps, &r->sps_len, &nals[i]);
		} else if (nals[i].type == H264_NAL_PPS) {
			has_pps = 1;
			cache(r->pps, &r->pps_len, &nals[i]);
		} else if (nals[i].type == H264_NAL_IDR) {
			idr = 1;
		}
	}
	int flags = idr ? (int)RTP_AU_KEYFRAME : 0;
	uint32_t total = 0;
	if (idr && (!has_sps || !has_pps) && r->sps_len && r->pps_len) {
		send_nal(r, r->sps, r->sps_len, ts, 0, send, ctx);
		send_nal(r, r->pps, r->pps_len, ts, 0, send, ctx);
		total += (uint32_t)(4 + r->sps_len + 4 + r->pps_len);
		r->params_added++;
		flags |= (int)RTP_AU_PARAMS_ADDED;
	}
	for (int i = 0; i < n; i++) {
		send_nal(r, nals[i].p, nals[i].len, ts, i == n - 1, send, ctx);
		total += (uint32_t)(4 + nals[i].len);
	}
	if (bytes)
		*bytes = total;
	return flags;
}
