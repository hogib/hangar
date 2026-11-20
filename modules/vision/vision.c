// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vision (docs/11-electric-eye.md "vision"; modules/vision/README.md): eeye's
 * effects thread as a hangar module. Reads a frame topic (cam.decoded on the
 * topside, cam.raw on the vehicle in placement B), runs the visibility
 * correction and the operator chain of lib/vision on its own thread pool,
 * and publishes the pilot's picture (cam.pilot), each frame referencing the
 * one it was made from. The chain is a parameter: a preset by name (the
 * Fly view's buttons) or eight stages as given.
 *
 *   hangar-vision [vision.conf]
 *
 * One thread reads and publishes; the pool's threads run the operators'
 * loops and touch no topic. The wait set is the input frame topic, read at
 * every wake (the newest frame: older ones are skipped, never queued), and
 * the call topics, served at every wake.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effect_chain.h"
#include "hg_call.h"
#include "hg_ini.h"
#include "hg_params.h"
#include "hg_pool.h"
#include "hg_topic.h"
#include "hg_type.h"
#include "img_stats.h"
#include "mod.h"
#include "presets.h"
#include "video_frame.h"
#include "vision_params.h"
#include "vision_pool.h"

#define WHO "hangar-vision"

static struct vision_conf C;
static vision_params P;
static EffectChainConfig chain;

static int validate(const void *candidate, const void *current, char reason[64])
{
	(void)current;
	EffectChainConfig c;
	return vision_effective_chain(&C, candidate, &c, reason, 64) < 0 ? HG_E_REFUSED : HG_OK;
}

static int load_conf(const char *path)
{
	vision_conf_defaults(&C);
	if (!path)
		return 0;
	static char text[16384];
	size_t len;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		mod_log(WHO, "%s: %s", path, strerror(errno));
		return -1;
	}
	char err[160];
	if (vision_conf_parse(&C, text, len, err, sizeof err) < 0) {
		mod_log(WHO, "%s: %s", path, err);
		return -1;
	}
	return 0;
}

static void chain_text(char *out, size_t len)
{
	size_t n = (size_t)mod_fmt(out, len, "%s", chain.auto_visibility ? "auto" : "");
	for (size_t i = 0; i < chain.stage_count && n < len; i++)
		n += (size_t)mod_fmt(out + n, len - n, "%s%s", n ? "+" : "", vision_effect_name(chain.stages[i].effect));
	if (!n)
		mod_fmt(out, len, "pass-through");
}

/* ---- Frames ---------------------------------------------------------------------- */

static hg_sub *sub_in;
static hg_pub *pub_out;
static struct hg_frame_type otype;
static unsigned char *inbuf, *outbuf;
static size_t in_cap;
static VideoFrame *frame;
static AutoVis *av;
static uint32_t fw, fh;
static uint64_t last_seq, frames_in, frames_out, skipped, wrong, size_said;
static uint64_t cost_ns_sum, cost_ns_max, cost_n;

/* The frame size is fixed by the first frame, as cam.decoded's is: the
 * buffers, the output type and the topic are made once. */
static int setup(uint32_t w, uint32_t h)
{
	if (hg_frame_type(w, h, HG_I422, &otype) < 0)
		return -1;
	otype.type.name = "frame";
	struct hg_pub_opts o = { .slots = 4 };
	pub_out = hg_pub_open(C.output, &otype.type, &o);
	if (!pub_out) {
		mod_log(WHO, "hg_pub_open %s (%ux%u): %s", C.output, w, h, strerror(errno));
		return -1;
	}
	frame = vf_create(w, h, 0);
	outbuf = calloc(1, otype.type.payload_bytes);
	if (!frame || !outbuf)
		return -1;
	fw = w;
	fh = h;
	mod_log(WHO, "%s %ux%u I422 to %s", C.input, w, h, C.output);
	return 0;
}

static void process(const struct hg_info *info)
{
	struct hg_frame_header hd;
	memcpy(&hd, inbuf, sizeof hd);
	if (hd.fourcc != HG_I422 || !hd.width || !hd.height || (hd.width & 1)) {
		if (!wrong++)
			mod_log(WHO, "%s: a frame that is not I422 (fourcc %08x): dropped", C.input, (unsigned)hd.fourcc);
		return;
	}
	if (!pub_out && setup(hd.width, hd.height) < 0)
		return;
	if (hd.width != fw || hd.height != fh) {
		if (!size_said++)
			mod_log(WHO, "a %ux%u frame on a %ux%u topic: dropped (the size is fixed at the first frame)",
				(unsigned)hd.width, (unsigned)hd.height, (unsigned)fw, (unsigned)fh);
		return;
	}
	uint64_t t0 = hg_now_ns();
	const unsigned char *data = inbuf + sizeof hd;
	for (int p = 0; p < 3; p++) {
		uint32_t pw = p ? fw / 2 : fw;
		for (uint32_t y = 0; y < fh; y++)
			memcpy(frame->raw_planes[p] + (size_t)y * frame->stride[p],
			       data + hd.offset[p] + (size_t)y * hd.stride[p], pw);
	}
	frame->pts = (int64_t)info->seq;
	frame->capture_ns = info->stamp_ns;
	ImgStats stats;
	if (chain.auto_visibility) {
		const uint8_t *raw[3] = { frame->raw_planes[0], frame->raw_planes[1], frame->raw_planes[2] };
		img_stats_compute(raw, fw, fh, frame->stride, &stats);
		img_stats_summarize(&stats, &frame->stats);
	} else {
		memset(&stats, 0, sizeof stats);
		memset(&frame->stats, 0, sizeof frame->stats);
	}
	apply_effect_chain_auto(frame, &chain, av, &stats);
	struct hg_frame_header oh = { .width = fw, .height = fh, .fourcc = HG_I422 };
	memcpy(oh.stride, otype.stride, sizeof oh.stride);
	memcpy(oh.offset, otype.offset, sizeof oh.offset);
	memcpy(outbuf, &oh, sizeof oh);
	unsigned char *od = outbuf + sizeof oh;
	for (int p = 0; p < 3; p++) {
		uint32_t pw = p ? fw / 2 : fw;
		if (p && frame->chroma_neutral) {
			/* The chain ended in an edge map or grayscale: the chroma is all
			 * 128 and need not be read (the frame bus's shortcut). */
			memset(od + otype.offset[p], 128, (size_t)pw * fh);
			continue;
		}
		for (uint32_t y = 0; y < fh; y++)
			memcpy(od + otype.offset[p] + (size_t)y * otype.stride[p],
			       frame->planes[p] + (size_t)y * frame->stride[p], pw);
	}
	/* The reference is the frame read: cam.pilot's Inputs, filled by the
	 * library from that read (hg_latest). The capture stamp goes through. */
	hg_publish(pub_out, outbuf, otype.type.payload_bytes, info->stamp_ns);
	frames_out++;
	uint64_t dt = hg_now_ns() - t0;
	cost_ns_sum += dt;
	cost_n++;
	if (dt > cost_ns_max)
		cost_ns_max = dt;
}

/* The newest frame, if there is one not yet processed: one read, which also
 * takes the topic's news (nothing in the wait set stays unread: hangar#1). */
static void read_input(void)
{
	struct hg_info info;
	if (!in_cap) {
		unsigned char probe[8];
		enum hg_result r = hg_latest(sub_in, probe, sizeof probe, &info); /* attaches; too short to read */
		(void)r;
		uint32_t pl = hg_sub_payload_bytes(sub_in);
		if (!pl)
			return;
		inbuf = malloc(pl);
		if (!inbuf)
			return;
		in_cap = pl;
	}
	enum hg_result r = hg_latest(sub_in, inbuf, in_cap, &info);
	if (r == HG_ERROR && errno == EMSGSIZE) {
		/* The writer was replaced by one with another frame size. */
		free(inbuf);
		inbuf = NULL;
		in_cap = 0;
		return;
	}
	if (r != HG_MSG || info.seq == last_seq)
		return;
	if (last_seq && info.seq > last_seq + 1)
		skipped += info.seq - last_seq - 1;
	last_seq = info.seq;
	frames_in++;
	process(&info);
}

/* Once a second: the cost of the frames of the last second, or, after a
 * second without frames, of the last second that had some. */
static void status(void)
{
	static double avg, max;
	if (cost_n) {
		avg = (double)cost_ns_sum / (double)cost_n / 1e6;
		max = (double)cost_ns_max / 1e6;
	}
	char ct[96], line[256];
	chain_text(ct, sizeof ct);
	mod_fmt(line, sizeof line, "%.15s (%s): %llu frames, %llu skipped, %.2f ms a frame (max %.2f), %u threads",
		P.preset, ct, (unsigned long long)frames_out, (unsigned long long)skipped, avg, max,
		hg_pool_threads(vision_pool()));
	hg_status(line);
	cost_ns_sum = 0;
	cost_n = 0;
	cost_ns_max = 0;
}

int main(int argc, char **argv)
{
	mod_init(WHO);
	if (load_conf(argc > 1 ? argv[1] : NULL) < 0)
		return 78;
	if (hg_params_open(&VISION_PARAMS_TYPE, &P, validate, NULL) < 0) {
		mod_log(WHO, "hg_params_open: %s", strerror(errno));
		return 4;
	}
	char err[160];
	if (vision_effective_chain(&C, &P, &chain, err, sizeof err) < 0) {
		/* A saved preset vision.conf no longer defines: the picture passes
		 * through until a set_params names one that exists. */
		mod_log(WHO, "the saved parameters: %s; the picture passes through", err);
		memset(&chain, 0, sizeof chain);
	}
	struct hg_pool *pool = hg_pool_create(P.threads ? P.threads : hg_pool_default_threads());
	if (!pool) {
		mod_log(WHO, "hg_pool_create: %s", strerror(errno));
		return 4;
	}
	vision_set_pool(pool);
	av = auto_vis_create();
	sub_in = hg_sub_open(C.input, NULL);
	if (!sub_in || !av) {
		mod_log(WHO, "%s: %s (the manifest subscribes to it?)", C.input, sub_in ? "no memory" : strerror(errno));
		return 4;
	}
	hg_sub *subs[HG_MAX_WAIT];
	int n = 0;
	subs[n++] = sub_in;
	int nc = hg_call_subs(subs + n, HG_MAX_WAIT - n);
	if (nc < 0) {
		mod_log(WHO, "hg_call_subs: %s", strerror(errno));
		return 4;
	}
	n += nc;
	char ct[96];
	chain_text(ct, sizeof ct);
	mod_log(WHO, "%s to %s; preset %.15s (%s); %u threads", C.input, C.output, P.preset, ct,
		hg_pool_threads(pool));
	/* cam.pilot is an event topic: no frames, no message, and ready all the same. */
	hg_ready();
	uint64_t next_status = hg_now_ns();
	while (!mod_stop) {
		unsigned r = hg_wait(subs, n, 1000);
		if (r & HG_READY_INTERRUPT)
			break;
		hg_serve();
		if (hg_params_changed()) {
			EffectChainConfig c;
			if (vision_effective_chain(&C, &P, &c, err, sizeof err) == 0)
				chain = c;
			chain_text(ct, sizeof ct);
			mod_log(WHO, "parameters v%llu by %s: preset %.15s (%s)", (unsigned long long)hg_params_version(),
				hg_params_changed_by(), P.preset, ct);
		}
		read_input();
		uint64_t now = hg_now_ns();
		if (now >= next_status) {
			status();
			hg_alive();
			next_status = now + 1000000000u;
		}
	}
	mod_log(WHO, "stopping: %llu frames in, %llu out, %llu skipped", (unsigned long long)frames_in,
		(unsigned long long)frames_out, (unsigned long long)skipped);
	vision_set_pool(NULL);
	hg_pool_destroy(pool);
	return 0;
}
