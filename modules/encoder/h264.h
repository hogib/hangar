// SPDX-License-Identifier: GPL-3.0-or-later
/* H.264 for the encoder module (docs/11-electric-eye.md "encoder"): Annex B
 * split into NAL units and access units, and RFC 6184 packetization over RTP
 * (single NAL units and FU-A fragments, the marker bit on each access unit's
 * last packet). No dependency; nothing is decoded: only NAL headers and the
 * first bit of a slice header are read. */
#ifndef HANGAR_ENCODER_H264_H
#define HANGAR_ENCODER_H264_H

#include <stddef.h>
#include <stdint.h>

enum {
	H264_NAL_SLICE = 1,
	H264_NAL_IDR = 5,
	H264_NAL_SEI = 6,
	H264_NAL_SPS = 7,
	H264_NAL_PPS = 8,
	H264_NAL_AUD = 9,
	H264_NAL_FU_A = 28,
};

#define RTP_HEADER 12
#define RTP_MAX_PACKET 1500       /* the largest MaxPacket accepted */
#define H264_PARAM_MAX 512        /* a cached SPS or PPS */

struct h264_nal {
	const uint8_t *p; /* the NAL unit from its header byte, without its start code */
	size_t len;
	uint8_t type;
};

/* The NAL units of an Annex B buffer, in order: start codes (00 00 01 or
 * 00 00 00 01) dropped, and the zero bytes a NAL unit cannot end with
 * (trailing_zero_8bits before the next start code) trimmed. At most max are
 * stored; the count of all of them is returned. */
int h264_nals(const uint8_t *buf, size_t len, struct h264_nal *out, int max);

/* The length of the access unit that starts at buf (start codes included),
 * by H.264 7.4.1.2.3: a new one begins at an AUD, SEI, SPS or PPS after a
 * slice, or at a slice whose first_mb_in_slice is 0 after a slice. 0 when
 * buf holds no NAL unit. */
size_t h264_au_len(const uint8_t *buf, size_t len);

/* A sender's RTP state and what it has seen. */
struct rtp_h264 {
	uint32_t ssrc;
	uint16_t seq;          /* the next packet's sequence number */
	uint8_t pt;            /* payload type, 96 by the SDP hellion and ffmpeg read */
	size_t max_packet;     /* RTP header included; at least RTP_HEADER + 3 */
	uint8_t sps[H264_PARAM_MAX], pps[H264_PARAM_MAX];
	size_t sps_len, pps_len;
	uint64_t packets, bytes, params_added;
};

typedef void (*rtp_send_fn)(void *ctx, const uint8_t *pkt, size_t len);

void rtp_h264_init(struct rtp_h264 *r, uint32_t ssrc, uint8_t pt, size_t max_packet);

#define RTP_AU_KEYFRAME 1u      /* the access unit has an IDR slice */
#define RTP_AU_PARAMS_ADDED 2u  /* cached SPS and PPS were sent in front of it */

/* Sends one access unit (Annex B) with the RTP timestamp ts: every NAL unit
 * in order, a single NAL packet when it fits max_packet, else FU-A
 * fragments; the marker bit on the last packet. The SPS and PPS it carries
 * are cached; an IDR access unit without them gets the cached ones first,
 * so a receiver that starts at any keyframe can decode (hellion's SDP has no
 * sprop-parameter-sets). *bytes, if given, is the Annex B size of what was
 * sent, inserted parameter sets included. Returns RTP_AU_* flags, or -1 if
 * the access unit holds no NAL unit (nothing is sent). */
int rtp_h264_send_au(struct rtp_h264 *r, const uint8_t *au, size_t len, uint32_t ts, rtp_send_fn send, void *ctx,
		     uint32_t *bytes);

#endif
