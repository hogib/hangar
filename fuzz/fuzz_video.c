// SPDX-License-Identifier: GPL-3.0-or-later
/* The video path's own parsing, both ends, without the codec libraries:
 *
 *  - video_relay's side (the topside): RTP packets from its UDP socket through
 *    depack_push (modules/video_relay/depack.c), and each access unit that
 *    comes out through what the module does with it short of decoding: the
 *    NAL scan, the SPS's picture size (h264_sps_size) and the MP4 writer
 *    (modules/video_relay/mp4.c);
 *  - the encoder's side (the vehicle): the camera's H.264 bytes from the V4L2
 *    device through h264_au_len, h264_nals and the RTP packetizer
 *    (modules/encoder/h264.c), and its packets back through the depacketizer.
 *
 * Decoding itself is libavcodec's (video_relay) and is not fuzzed here.
 *
 * Input: one mode byte. Even: a sequence of RTP packets, each a 16-bit
 * little-endian length and that many bytes. Odd: the next byte chooses the
 * packet size, the rest is an Annex B byte stream as a camera gives it. */
#include "fuzz.h"

#include <fcntl.h>

#include "depack.h"
#include "h264.h"
#include "mp4.h"

static struct depack D, D2;
static char mp4_path[300];
static struct mp4w W;
static int w_open;
static uint64_t n_au;

static void cleanup(void)
{
	unlink(mp4_path);
}

/* What video_relay does with an access unit, short of decoding it. */
static void on_au(const struct depack_au *au)
{
	FZ_CHECK(au->len > 0 && au->len <= DEPACK_MAX_AU);
	struct h264_nal nals[64];
	int n = h264_nals(au->data, au->len, nals, 64);
	for (int i = 0; i < n && i < 64; i++) {
		FZ_CHECK(nals[i].p >= au->data && nals[i].p + nals[i].len <= au->data + au->len);
		if (nals[i].type == H264_NAL_SPS) {
			uint32_t w = 0, h = 0;
			(void)h264_sps_size(nals[i].p, nals[i].len, &w, &h);
		}
	}
	if (w_open)
		(void)mp4_write(&W, au->data, au->len, (uint64_t)au->ts + (n_au << 32), 3000, au->keyframe);
	n_au++;
}

static void rtp_in(struct depack *d, const uint8_t *pkt, size_t len)
{
	struct depack_au out[2];
	int n = depack_push(d, pkt, len, out);
	FZ_CHECK(n >= 0 && n <= 2);
	for (int i = 0; i < n; i++)
		on_au(&out[i]);
	FZ_CHECK(d->len <= DEPACK_MAX_AU);
}

static void loop_back(void *ctx, const uint8_t *pkt, size_t len)
{
	FZ_CHECK(len >= RTP_HEADER + 1 && len <= RTP_MAX_PACKET);
	/* In a block of its own length, as a datagram is. */
	uint8_t *copy = malloc(len);
	if (!copy)
		abort();
	memcpy(copy, pkt, len);
	rtp_in(ctx, copy, len);
	free(copy);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!mp4_path[0]) {
		const char *tmp = getenv("TMPDIR");
		snprintf(mp4_path, sizeof mp4_path, "%s/hg-fuzz-video-%ld.mp4", tmp && *tmp ? tmp : "/tmp", (long)getpid());
		atexit(cleanup);
	}
	if (size < 2)
		return 0;
	unsigned mode = data[0];
	unlink(mp4_path);
	w_open = mp4_open(&W, mp4_path) == 0;
	n_au = 0;
	depack_init(&D, 96);
	if (!(mode & 1)) {
		data++;
		size--;
		while (size >= 2) {
			size_t len = data[0] | (size_t)data[1] << 8;
			data += 2;
			size -= 2;
			if (len > size)
				len = size;
			uint8_t *pkt = malloc(len ? len : 1);
			if (!pkt)
				abort();
			memcpy(pkt, data, len);
			rtp_in(&D, pkt, len);
			free(pkt);
			data += len;
			size -= len;
		}
	} else {
		struct rtp_h264 r;
		rtp_h264_init(&r, 0x48414e47, 96, (size_t)data[1] * 6);
		data += 2;
		size -= 2;
		uint8_t *buf = malloc(size ? size : 1);
		if (!buf)
			abort();
		memcpy(buf, data, size);
		depack_init(&D2, 96);
		size_t off = 0;
		uint32_t ts = 0;
		/* The file source's loop (encoder.c file_tick): one access unit at a time. */
		while (off < size) {
			size_t n = h264_au_len(buf + off, size - off);
			FZ_CHECK(n <= size - off);
			if (n == 0)
				break;
			uint32_t bytes = 0;
			uint64_t before = r.packets;
			int flags = rtp_h264_send_au(&r, buf + off, n, ts, loop_back, &D2, &bytes);
			FZ_CHECK(flags >= -1);
			FZ_CHECK(flags >= 0 || r.packets == before);
			FZ_CHECK(r.sps_len <= H264_PARAM_MAX && r.pps_len <= H264_PARAM_MAX);
			ts += 3000;
			off += n;
		}
		/* Nothing was lost on the way: the depacketizer saw the packets in order. */
		FZ_CHECK(D2.lost == 0 && D2.packets <= r.packets);
		FZ_NOTE("camera: %llu packets sent, %llu frames back, %llu skipped, %llu bad", (unsigned long long)r.packets,
			(unsigned long long)D2.frames, (unsigned long long)D2.skipped, (unsigned long long)D2.bad);
		free(buf);
	}
	if (!(mode & 1))
		FZ_NOTE("rtp: %llu packets, %llu frames (%llu key), %llu skipped, %llu bad, %llu lost; mp4 %llu frames",
			(unsigned long long)D.packets, (unsigned long long)D.frames, (unsigned long long)D.keyframes,
			(unsigned long long)D.skipped, (unsigned long long)D.bad, (unsigned long long)D.lost,
			(unsigned long long)W.frames);
	if (w_open)
		mp4_close(&W);
	return 0;
}
