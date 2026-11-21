// SPDX-License-Identifier: GPL-3.0-or-later
/* mp4: the H.264 stream as it arrives, into a fragmented MP4, in process (no
 * ffmpeg child, docs/11). The header (ftyp, moov with an empty sample table
 * and mvex) is written at the first keyframe, from its SPS and PPS; then each
 * frame is one fragment (moof + mdat), its samples' NAL units length-prefixed.
 * A file cut by a crash or a power loss plays up to its last whole fragment.
 * Frames before the first keyframe are not written. */
#ifndef VIDEO_RELAY_MP4_H
#define VIDEO_RELAY_MP4_H

#include <stddef.h>
#include <stdint.h>

struct mp4w {
	int fd;                  /* -1: closed */
	int started;             /* the header is written */
	uint32_t timescale;      /* 90000, the RTP clock */
	uint32_t width, height;  /* from the SPS */
	uint32_t seq;            /* the next fragment's sequence number */
	uint64_t base_ts;        /* the first frame's 64-bit RTP timestamp */
	uint64_t frames, bytes;
	uint8_t *buf;            /* one fragment, built before it is written */
	size_t cap;
};

/* Creates path (mode 0640). 0, or -1 with errno. A path that exists is
 * refused, EEXIST: a recording that is there is never replaced or cut. */
int mp4_open(struct mp4w *w, const char *path);

/* Creates the first of <dir>/<stem>.mp4, <dir>/<stem>-2.mp4, <dir>/<stem>-3.mp4
 * and so on that does not exist yet, and writes its name to path. A second
 * recording under one name (a module restarted during a session, a session
 * announced twice) is a further part beside the first: appending would put a
 * second header and timestamps from zero into one file, and what is there may
 * end in a fragment cut short. 0, or -1 with errno. */
int mp4_open_new(struct mp4w *w, const char *dir, const char *stem, char *path, size_t len);

/* One frame: Annex B, its 64-bit RTP timestamp (unwrapped), its duration in
 * RTP ticks, whether it is a keyframe. 1 written, 0 skipped (before the first
 * keyframe, or a keyframe without SPS and PPS), -1 with errno on a write
 * error. */
int mp4_write(struct mp4w *w, const uint8_t *au, size_t len, uint64_t ts, uint32_t duration, int keyframe);

void mp4_close(struct mp4w *w);

/* The SPS's picture size, after cropping. 0, or -1 for an SPS it cannot read. */
int h264_sps_size(const uint8_t *sps, size_t len, uint32_t *width, uint32_t *height);

#endif
