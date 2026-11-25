// SPDX-License-Identifier: GPL-3.0-or-later
// topside_encoder under a manager (modules/topside_encoder/README.md): this
// process plays `vision`, publishing cam.pilot (1280x720 I422 frames, moving
// noise, so the rate control has work), receives the module's RTP on a local
// socket, puts it back into frames with video_relay's depacketizer and
// decodes them with libavcodec. Every frame comes back, in order, with the
// RTP timestamp its seq gives; a keyframe every 30 frames, SPS and PPS before
// each; one topside_video.index message a frame, referencing its cam.pilot
// frame; a bitrate change applied live and one over the cap refused; the
// latency from publish to the frame's last packet; the idle CPU.
#include "hd_test.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>

#include "depack.h"
#include "h264.h"
#include "hg_type.h"
#include "video_index.h"

#define W 1280
#define H 720
#define FPS 30
#define FRAMES 60

static const char *T_bin;
static pid_t mgr;
static char conf_path[600], mod_log[600];
static struct hg_frame_type FT;
static hg_pub *P_frames;
static hg_sub *S_index;
static uint8_t *frame;
static int rx = -1, rport;
static struct depack D;
static AVCodecContext *dec;
static AVPacket *pkt;
static AVFrame *frm;
static uint32_t rng = 12345;

/* What came back, one entry per access unit received. */
static struct au_seen {
	uint32_t ts;
	int keyframe, sps, pps, idr;
	size_t bytes;
	int decoded, w, h;
	int lmin, lmax; /* the decoded picture's darkest and brightest luma, sampled */
	uint64_t at_ns;
} seen[4 * FRAMES];
static int nseen;

static uint32_t xorshift(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void become(const char *module)
{
	char path[600];
	if (wiring_to_file(module, path, sizeof path) < 0) {
		printf("    cannot fetch the wiring of %s\n", module);
		exit(2);
	}
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", module, 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		printf("    hg_init as %s: %s\n", module, hg_init_error());
		exit(2);
	}
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
}

/* A frame k: a diagonal ramp moving 4 pixels a frame, noise on top (+-24),
 * flat chroma with a moving band: an I422 full-range picture x264 cannot
 * squeeze to nothing. */
static uint64_t publish_frame(int k)
{
	struct hg_frame_header hd = { .width = W, .height = H, .fourcc = HG_I422 };
	memcpy(hd.stride, FT.stride, sizeof hd.stride);
	memcpy(hd.offset, FT.offset, sizeof hd.offset);
	memcpy(frame, &hd, sizeof hd);
	uint8_t *data = frame + sizeof hd;
	for (int y = 0; y < H; y++) {
		uint8_t *row = data + FT.offset[0] + (size_t)y * FT.stride[0];
		for (int x = 0; x < W; x++) {
			int v = ((x + y + 4 * k) & 255) + (int)(xorshift() % 49) - 24;
			row[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
	for (int p = 1; p <= 2; p++)
		for (int y = 0; y < H; y++) {
			uint8_t *row = data + FT.offset[p] + (size_t)y * FT.stride[p];
			for (int x = 0; x < W / 2; x++)
				row[x] = (uint8_t)(((x + k * 3) / 40) & 1 ? 100 + 40 * p : 128);
		}
	hg_publish(P_frames, frame, FT.type.payload_bytes, hg_now_ns());
	return hg_pub_next_seq(P_frames) - 1;
}

static void decode_au(const struct depack_au *au, struct au_seen *s)
{
	pkt->data = (uint8_t *)au->data;
	pkt->size = (int)au->len;
	if (avcodec_send_packet(dec, pkt) < 0)
		return;
	while (avcodec_receive_frame(dec, frm) == 0) {
		s->decoded++;
		s->w = frm->width;
		s->h = frm->height;
		s->lmin = 255;
		s->lmax = 0;
		for (int y = 0; y < frm->height; y += 4)
			for (int x = 0; x < frm->width; x += 4) {
				int v = frm->data[0][(size_t)y * (size_t)frm->linesize[0] + (size_t)x];
				if (v < s->lmin)
					s->lmin = v;
				if (v > s->lmax)
					s->lmax = v;
			}
	}
}

/* Reads every packet that arrives within ms; access units into seen[]. */
static void collect(long ms)
{
	uint8_t buf[2048];
	uint64_t t0 = now_ms();
	for (;;) {
		long left = ms - (long)(now_ms() - t0);
		if (left < 0)
			left = 0;
		struct pollfd pf = { .fd = rx, .events = POLLIN };
		if (poll(&pf, 1, (int)left) <= 0)
			return;
		ssize_t n;
		while ((n = recv(rx, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
			struct depack_au out[2];
			int k = depack_push(&D, buf, (size_t)n, out);
			for (int i = 0; i < k && nseen < (int)(sizeof seen / sizeof seen[0]); i++) {
				struct au_seen *s = &seen[nseen++];
				memset(s, 0, sizeof *s);
				s->ts = out[i].ts;
				s->keyframe = out[i].keyframe;
				s->bytes = out[i].len;
				s->at_ns = hg_now_ns();
				struct h264_nal nals[64];
				int nn = h264_nals(out[i].data, out[i].len, nals, 64);
				for (int j = 0; j < nn && j < 64; j++) {
					if (nals[j].type == H264_NAL_SPS && !s->idr)
						s->sps = 1;
					if (nals[j].type == H264_NAL_PPS && !s->idr)
						s->pps = 1;
					if (nals[j].type == H264_NAL_IDR)
						s->idr = 1;
				}
				decode_au(&out[i], s);
			}
		}
		if (left == 0)
			return;
	}
}

/* Publishes frame k and waits for its access unit; returns publish → last packet, ns. */
static uint64_t frame_round_trip(int k, uint64_t *seq_out)
{
	int before = nseen;
	uint64_t t0 = hg_now_ns();
	uint64_t seq = publish_frame(k);
	*seq_out = seq;
	uint32_t want = (uint32_t)(seq * 90000u / FPS);
	uint64_t start = now_ms();
	while (now_ms() - start < 2000) {
		collect(20);
		for (int i = before; i < nseen; i++)
			if (seen[i].ts == want)
				return seen[i].at_ns - t0;
	}
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static void test_start(void)
{
	rx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	CHECK(rx >= 0 && bind(rx, (struct sockaddr *)&a, sizeof a) == 0);
	socklen_t al = sizeof a;
	getsockname(rx, (struct sockaddr *)&a, &al);
	rport = ntohs(a.sin_port);
	int big = 8 << 20;
	setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
	depack_init(&D, 96);

	av_log_set_level(AV_LOG_ERROR);
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	CHECK(codec != NULL);
	dec = avcodec_alloc_context3(codec);
	dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	pkt = av_packet_alloc();
	frm = av_frame_alloc();
	CHECK(avcodec_open2(dec, codec, NULL) == 0);

	tfmt(conf_path, sizeof conf_path, "%s/topside_encoder.conf", T_root);
	tfmt(mod_log, sizeof mod_log, "%s/run/hangar/log/topside_encoder.log", T_root);
	char text[512];
	tfmt(text, sizeof text,
	     "[TopsideEncoder]\nSource=cam.pilot\nPeer=127.0.0.1:%d\nFrameRate=%d\nKeyInterval=30\nMaxPacket=1200\n"
	     "PayloadType=96\nThreads=2\nIndex=topside_video.index\n",
	     rport, FPS);
	write_file(conf_path, text);
	manifest("topside_encoder", "[Module]\nExec=%s %s\n"
				    "[Subscribe cam.pilot]\nType=frame\n"
				    "[Publish topside_video.index]\nType=video_index\nSlots=128\nInputs=cam.pilot\n"
				    "[Params]\nType=topside_encoder_params\nAllow=admin\n"
				    "[Restart]\nBackoff=0.1s..0.2s\n",
		 T_bin, conf_path);
	manifest("vision", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Publish cam.pilot]\nType=frame\nSlots=4\n"
			   "[Subscribe topside_video.index]\nType=video_index\n");
	mgr = manager_start();
	CHECK(wait_state("vision", "stopped", 5000));
	become("vision");
	CHECK_EQ_INT(hg_frame_type(W, H, HG_I422, &FT), 0);
	FT.type.name = "frame";
	struct hg_pub_opts o = { .slots = 4 };
	P_frames = hg_pub_open("cam.pilot", &FT.type, &o);
	CHECK(P_frames != NULL);
	frame = calloc(1, FT.type.payload_bytes);
	CHECK(wait_state("topside_encoder", "ready", 8000));
	S_index = hg_sub_open("topside_video.index", &VIDEO_INDEX_TYPE);
	CHECK(S_index != NULL);
	if (!S_index) {
		manager_stop(mgr);
		exit(1);
	}
}

static void test_frames_keyframes_index(void)
{
	uint64_t lat[FRAMES], seqs[FRAMES];
	int nlat = 0, missing = 0;
	for (int k = 0; k < FRAMES; k++) {
		uint64_t l = frame_round_trip(k, &seqs[k]);
		if (l)
			lat[nlat++] = l;
		else
			missing++;
	}
	CHECK_EQ_INT(missing, 0);
	// Every frame back, in order, its RTP timestamp from its seq; decoded at 1280x720.
	int order_ok = nseen == FRAMES, decoded = 0, size_ok = 1;
	for (int i = 0; i < nseen && i < FRAMES; i++) {
		order_ok &= seen[i].ts == (uint32_t)(seqs[i] * 90000u / FPS);
		decoded += seen[i].decoded;
		size_ok &= !seen[i].decoded || (seen[i].w == W && seen[i].h == H);
	}
	printf("    %d frames sent, %d access units back, %d decoded\n", FRAMES, nseen, decoded);
	CHECK(order_ok);
	CHECK_EQ_INT(decoded, FRAMES);
	CHECK(size_ok);
	// A keyframe at the first frame and every 30th after; SPS and PPS before each.
	int keys = 0, keys_ok = 1, params_ok = 1;
	for (int i = 0; i < nseen; i++) {
		int want = i % 30 == 0;
		keys += seen[i].keyframe;
		keys_ok &= seen[i].keyframe == want && seen[i].idr == want;
		if (seen[i].keyframe)
			params_ok &= seen[i].sps && seen[i].pps;
	}
	printf("    %d keyframes (frames 1 and 31 expected), SPS and PPS before each: %s\n", keys, params_ok ? "yes" : "no");
	CHECK_EQ_INT(keys, 2);
	CHECK(keys_ok);
	CHECK(params_ok);
	// One index message a frame, its seq, keyframe and RTP timestamp; its
	// provenance the cam.pilot frame.
	struct video_index m;
	struct hg_info info;
	int n = 0, idx_ok = 1;
	uint32_t pilot = hg_topic_id("cam.pilot");
	while (hg_next(S_index, &m, sizeof m, &info) == HG_MSG) {
		if (n < FRAMES)
			idx_ok &= m.frame_seq == seqs[n] && m.rtp_timestamp == (uint32_t)(seqs[n] * 90000u / FPS) &&
				  m.keyframe == (n % 30 == 0) && m.frame_topic_id == pilot && m.bytes > 0 &&
				  info.n_inputs == 1 && info.inputs[0].topic_id == pilot && info.inputs[0].seq == seqs[n];
		n++;
	}
	CHECK_EQ_INT(n, FRAMES);
	CHECK(idx_ok);
	// Latency: publish to the frame's last packet, 1280x720.
	qsort(lat, (size_t)nlat, sizeof lat[0], cmp_u64);
	if (nlat)
		printf("    publish -> last packet at 1280x720: median %.2f ms, p90 %.2f ms, max %.2f ms\n",
		       (double)lat[nlat / 2] / 1e6, (double)lat[nlat * 9 / 10] / 1e6, (double)lat[nlat - 1] / 1e6);
}

static double bytes_per_frame(int from, int to)
{
	size_t sum = 0;
	for (int i = from; i < to; i++)
		sum += seen[i].bytes;
	return to > from ? (double)sum / (double)(to - from) : 0;
}

static void test_bitrate_live(void)
{
	char out[2048];
	nseen = 0;
	uint64_t s;
	for (int k = 0; k < FRAMES; k++)
		frame_round_trip(100 + k, &s);
	double before = bytes_per_frame(15, FRAMES);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "topside_encoder", "bitrate_kbps=1000", (char *)NULL), 0);
	CHECK(wait_file_contains(mod_log, "bitrate 4000 -> 1000 kbit/s", 3000));
	int at = nseen;
	for (int k = 0; k < FRAMES; k++)
		frame_round_trip(200 + k, &s);
	double after = bytes_per_frame(at + 15, nseen);
	printf("    bytes a frame at 4000 kbit/s: %.0f (%.0f kbit/s at %d frames/s); after bitrate_kbps=1000: %.0f (%.0f kbit/s)\n",
	       before, before * 8 * FPS / 1000, FPS, after, after * 8 * FPS / 1000);
	CHECK(after > 0 && after < before * 0.5);
	// Over the cap: refused, with the reason; the bitrate stays.
	int rc = run_cli(out, sizeof out, 5000, "param", "set", "topside_encoder", "bitrate_kbps=9000", (char *)NULL);
	CHECK(rc != 0 || strstr(out, "over max_bitrate_kbps") != NULL);
	if (!strstr(out, "over max_bitrate_kbps"))
		printf("    refusal: %s\n", out);
	CHECK(strstr(out, "over max_bitrate_kbps") != NULL);
	CHECK(!file_contains(mod_log, "-> 9000 kbit/s"));
}

/* The module goes on: frames at the usual pace come back as pictures. Not the
 * first one necessarily: where the burst before it overran the receiving
 * socket, the depacketizer has lost its place in the stream and takes it up
 * again at the next keyframe, at most 30 frames on (seen on CI's two-CPU
 * machines; never on the development laptop). */
static int pictures_come_back(int k0)
{
	collect(100);
	nseen = 0;
	for (int k = 0; k < 2 * FPS + 5; k++) {
		publish_frame(k0 + k);
		collect(1000 / FPS);
		for (int i = 0; i < nseen; i++)
			if (seen[i].decoded)
				return 1;
	}
	return 0;
}

/* One grey level over the whole picture: a picture put together from two
 * frames, the top of one and the bottom of the next, has two. */
static void publish_flat(uint8_t level, const struct hg_frame_header *lie)
{
	struct hg_frame_header hd = { .width = W, .height = H, .fourcc = HG_I422 };
	memcpy(hd.stride, FT.stride, sizeof hd.stride);
	memcpy(hd.offset, FT.offset, sizeof hd.offset);
	memcpy(frame, lie ? lie : &hd, sizeof hd);
	uint8_t *data = frame + sizeof hd;
	memset(data + FT.offset[0], level, (size_t)FT.stride[0] * H);
	memset(data + FT.offset[1], 128, (size_t)FT.stride[1] * H);
	memset(data + FT.offset[2], 128, (size_t)FT.stride[2] * H);
	hg_publish(P_frames, frame, FT.type.payload_bytes, hg_now_ns());
}

/* The module converts a frame where it lies in the ring and gives it to x264
 * only when the slot held that frame throughout. Here the writer comes round
 * the four slots several times while one picture is converted, flat frames of
 * two grey levels in turn: a picture made of two frames would show both
 * levels. Every picture that comes back shows one, and pictures do come back:
 * a writer this fast does not starve the encoder (after three frames lost in
 * a row it copies the newest, as it did before reading in place). */
static void test_overwritten_while_read(void)
{
	collect(200);
	nseen = 0; /* seen[] holds 4 * FRAMES access units: this test's alone */
	int at = 0;
	uint64_t t0 = now_ms();
	int published = 0;
	while (now_ms() - t0 < 1200) {
		publish_flat(published & 1 ? 190 : 60, NULL);
		published++;
		if (published % 8 == 0)
			sleep_ms(1);
	}
	collect(700);
	int pictures = 0, mixed = 0, widest = 0;
	for (int i = at; i < nseen; i++) {
		if (!seen[i].decoded)
			continue;
		pictures++;
		int span = seen[i].lmax - seen[i].lmin;
		if (span > widest)
			widest = span;
		mixed += span > 40;
	}
	printf("    %d frames published in 1.2 s, %d pictures looked at%s, the widest span of luma in one %d (two frames "
	       "in one would be about 110)\n",
	       published, pictures, nseen == (int)(sizeof seen / sizeof seen[0]) ? " (as many as are kept)" : "", widest);
	CHECK(pictures >= 10);
	CHECK_EQ_INT(mixed, 0);
	CHECK(pictures_come_back(900));
}

/* A header is the publisher's word, and through a view it is not even known
 * to be one frame's. One whose planes reach past the payload is not
 * converted, in place or copied, and the module goes on. */
static void test_header_past_the_payload(void)
{
	struct mstat m;
	CHECK(module_status("topside_encoder", &m));
	int pid = m.pid;
	struct hg_frame_header lie = { .width = W, .height = H, .fourcc = HG_I422 };
	memcpy(lie.stride, FT.stride, sizeof lie.stride);
	memcpy(lie.offset, FT.offset, sizeof lie.offset);
	lie.offset[2] = FT.data_bytes - 16; /* the last plane starts 16 bytes before the end */
	collect(100);
	nseen = 0;
	int at = 0;
	publish_flat(100, &lie);
	collect(300);
	CHECK_EQ_INT(nseen, at);
	CHECK(wait_file_contains(mod_log, "planes do not lie inside", 2000));
	lie.offset[2] = FT.offset[2];
	lie.stride[0] = 0x7fffffff; /* rows that far apart */
	publish_flat(100, &lie);
	collect(300);
	CHECK_EQ_INT(nseen, at);
	CHECK(module_status("topside_encoder", &m) && m.pid == pid && strcmp(m.state, "ready") == 0);
	CHECK(pictures_come_back(1000));
}

static void test_idle_cpu(void)
{
	struct mstat m;
	CHECK(module_status("topside_encoder", &m) && m.pid > 0);
	long long t0 = th_cpu_ticks(m.pid);
	uint64_t a = now_ms();
	sleep_ms(1000);
	double pct = th_cpu_percent(t0, th_cpu_ticks(m.pid), (long)(now_ms() - a));
	printf("    topside_encoder with no frames: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 2);
}

static void test_shutdown(void)
{
	hg_pub_close(P_frames);
	CHECK_EQ_INT(manager_stop(mgr), 0);
	CHECK(file_contains(mod_log, "stopping:"));
	const char *log = read_file(mod_log);
	const char *p = log ? strstr(log, "stopping:") : NULL;
	if (p)
		printf("    %.*s\n", (int)strcspn(p, "\n"), p);
	avcodec_free_context(&dec);
	av_packet_free(&pkt);
	av_frame_free(&frm);
	free(frame);
}

int main(void)
{
	T_bin = getenv("TOPSIDE_ENCODER_BIN");
	if (!T_bin) {
		fprintf(stderr, "TOPSIDE_ENCODER_BIN must be set\n");
		return 2;
	}
	hd_setup("topside_encoder", NULL);
	const char *src = getenv("HANGAR_MSG_SRC");
	const char *types[] = { "video_index", "topside_encoder_params", "set_params_rep", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src && *src ? src : "msg", types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		copy_file(from, to);
	}
	RUN_TEST(test_start);
	RUN_TEST(test_frames_keyframes_index);
	RUN_TEST(test_bitrate_live);
	RUN_TEST(test_overwritten_while_read);
	RUN_TEST(test_header_past_the_payload);
	RUN_TEST(test_idle_cpu);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
