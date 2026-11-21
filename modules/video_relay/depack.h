// SPDX-License-Identifier: GPL-3.0-or-later
/* depack: RTP H.264 (RFC 6184) back into access units, the frames the
 * encoder sent (modules/encoder/h264.c is the other end). One access unit is
 * the NAL units of one RTP timestamp, written in Annex B (00 00 00 01 before
 * each), complete at the packet with the marker bit, or when a packet of
 * another timestamp arrives first.
 *
 * Loss: a gap in the RTP sequence numbers marks the access unit under way as
 * broken; it is not returned. Nor is any later one until an IDR access unit
 * arrives whole: a decoder fed a frame whose reference is missing shows
 * garbage until the next keyframe, so frames are skipped until then instead.
 * What is skipped is counted, and shows downstream as frame numbers with no
 * decoded frame.
 *
 * A new stream (another SSRC, or sequence numbers far behind the last: the
 * encoder restarted) starts over, at its next keyframe; a packet a little
 * behind (within DEPACK_LATE) is late, and dropped. */
#ifndef VIDEO_RELAY_DEPACK_H
#define VIDEO_RELAY_DEPACK_H

#include <stddef.h>
#include <stdint.h>

#define DEPACK_MAX_AU (4u << 20) /* 4 MB: a 1080p IDR is well under 1 MB */
#define DEPACK_LATE 64           /* packets behind the newest: late, not a new stream */

struct depack {
	uint8_t pt;               /* the payload type accepted; others are counted and dropped */
	uint32_t ssrc;            /* the stream's; another one starts over */
	int have_seq;
	uint16_t next_seq;
	int have_ts;
	uint32_t ts;              /* the access unit under way */
	int broken;               /* a packet of it was lost */
	int need_idr;             /* skipping until a whole IDR access unit */
	int has_idr;
	uint8_t fu_type;          /* an FU-A under way: its NAL type, 0 none */
	int cur;                  /* the buffer being filled; the other holds the last returned */
	size_t len;
	uint8_t au[2][DEPACK_MAX_AU];
	/* counts */
	uint64_t packets, lost, bad, frames, skipped, keyframes, resyncs;
};

/* An access unit returned by depack_push: valid until the next call. */
struct depack_au {
	const uint8_t *data;      /* Annex B */
	size_t len;
	uint32_t ts;              /* its RTP timestamp */
	int keyframe;             /* it holds an IDR slice */
};

void depack_init(struct depack *d, uint8_t pt);

/* ---- The stream's time, 64 bits from the 32 that RTP carries --------------------- */

/* The encoder's timestamp is the frame's number times 90000 / FrameRate, cut
 * to 32 bits: it wraps after 2^32 / 90000 s = 13 h 15 min of frames. */
#define TS_NO_HINT UINT64_MAX
#define TS_BACK 90000u    /* 1 s: further back than this is not the same count going on */
#define TS_SLACK 900000u  /* 10 s more than twice the time that passed: no count runs that far ahead */

struct ts_unwrap {
	int seen;
	uint64_t last;    /* the last value returned */
	uint64_t last_ns; /* when its access unit arrived */
};

/* The 64-bit timestamp of an access unit that arrived at now_ns.
 *
 * hint: the full timestamp of a frame of the same count known from elsewhere
 * (the mirrored video.index message), within 2^31 ticks (6 h 37 min) of this
 * one either way; TS_NO_HINT when there is none. With a hint the answer is the
 * value nearest to it. Without one it is the value nearest to the last one
 * returned (a wrap seen is a wrap counted), the 32 bits themselves for the
 * first: a stream first seen after its first wrap is then low by 2^32, which
 * only a hint puts right.
 *
 * *restarted (when not NULL) is 1 when the value does not go on from the last
 * one: it is more than TS_BACK behind it, or further ahead than twice the time
 * that passed between the two arrivals plus TS_SLACK. Frame numbers never do
 * either while one count runs: the count started again (the vehicle rebooted),
 * or a hint has just put a wrong value right. Without a hint the answer is
 * then the 32 bits themselves, a count from zero; that is what tells a reboot
 * from a wrap when the last timestamp was above 2^31. */
uint64_t ts_unwrap(struct ts_unwrap *u, uint32_t ts, uint64_t now_ns, uint64_t hint, int *restarted);

/* Takes one RTP packet; returns how many access units it completed, each
 * whole and decodable from what came before, in out[0] (and out[1]). Two when
 * a packet of a new timestamp completes the previous access unit (its marker
 * was lost or never sent) and is itself a whole access unit with the marker.
 * What out points to is valid until the next call. */
int depack_push(struct depack *d, const uint8_t *pkt, size_t len, struct depack_au out[2]);

#endif
