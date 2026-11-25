// SPDX-License-Identifier: GPL-3.0-or-later
/* topside_encoder: the pilot's picture to QGroundControl (docs/11-electric-eye.md
 * "encoder", placement A's `topside_encoder`). It reads a frame topic (cam.pilot,
 * from vision; cam.decoded until vision runs), converts the frame from full-range
 * I422 to limited-range I420 as hellion did, encodes it in process with libx264
 * (preset ultrafast, tune zerolatency, a keyframe every KeyInterval frames, SPS
 * and PPS before each), and sends it over RTP with the RFC 6184 packetizer of
 * the vehicle's encoder (modules/encoder/h264.c). The RTP timestamp is the
 * input frame's seq times the nominal period, and each frame sent is a message
 * on the index topic (video_index), referencing the frame it was made from.
 * The bitrate is a live parameter. No ffmpeg, no thread of its own (x264's
 * sliced threads aside): one hg_wait over the frame topic and the call topics,
 * each read on every wake. modules/topside_encoder/README.md has the rest.
 *
 *   hangar-topside-encoder /etc/hangar/topside_encoder.conf
 */
#include "mod.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <x264.h>

#include "h264.h"
#include "hg_call.h"
#include "hg_ini.h"
#include "hg_params.h"
#include "hg_type.h"
#include "topside_encoder_params.h"
#include "video_index.h"

#define WHO "hangar-topside-encoder"

static struct {
	char source[64];      /* the frame topic encoded */
	char peer[128];       /* host:port, QGroundControl's video in */
	unsigned fps;         /* the nominal rate: RTP timestamps and the rate control's */
	unsigned keyint;      /* frames between keyframes */
	unsigned max_packet;  /* RTP header included */
	unsigned pt;
	unsigned threads;     /* x264's sliced threads; 0 lets it choose */
	char index[64];       /* the index topic */
} C = { .source = "cam.pilot", .peer = "127.0.0.1:5600", .fps = 30, .keyint = 30, .max_packet = 1200, .pt = 96,
	.threads = 0, .index = "topside_video.index" };

static struct topside_encoder_params P = { .bitrate_kbps = 4000, .max_bitrate_kbps = 8000 };
static int have_params; /* hg_params_open succeeded: the bitrate is live */

/* ---- topside_encoder.conf --------------------------------------------------------- */

static int on_section(void *ctx, const char *section, const char *arg, int line, char *err, size_t errlen)
{
	(void)ctx;
	(void)line;
	if (strcmp(section, "TopsideEncoder") || *arg) {
		snprintf(err, errlen, "unknown section [%s%s%s]", section, *arg ? " " : "", arg);
		return -1;
	}
	return 0;
}

static int parse_uint(const char *v, unsigned lo, unsigned hi, unsigned *out)
{
	char *end;
	errno = 0;
	unsigned long x = strtoul(v, &end, 10);
	if (errno || end == v || *end || x < lo || x > hi)
		return -1;
	*out = (unsigned)x;
	return 0;
}

static int set_name(char *dst, size_t cap, const char *v, const char *key, char *err, size_t errlen)
{
	if (!*v || strlen(v) >= cap || strchr(v, ' ') || strchr(v, '*')) {
		snprintf(err, errlen, "%s: a topic name", key);
		return -1;
	}
	snprintf(dst, cap, "%s", v);
	return 0;
}

static int on_key(void *ctx, const char *key, const char *value, int line, char *err, size_t errlen)
{
	(void)ctx;
	(void)line;
	unsigned u;
	if (strcmp(key, "Source") == 0)
		return set_name(C.source, sizeof C.source, value, key, err, errlen);
	if (strcmp(key, "Index") == 0)
		return set_name(C.index, sizeof C.index, value, key, err, errlen);
	if (strcmp(key, "Peer") == 0) {
		if (!strchr(value, ':') || strlen(value) >= sizeof C.peer) {
			snprintf(err, errlen, "Peer: host:port, QGroundControl's video in");
			return -1;
		}
		snprintf(C.peer, sizeof C.peer, "%s", value);
		return 0;
	}
	struct {
		const char *key;
		unsigned lo, hi, *out;
		const char *what;
	} nums[] = {
		{ "FrameRate", 1, 240, &C.fps, "1 to 240, the frame topic's nominal rate" },
		{ "KeyInterval", 1, 600, &C.keyint, "1 to 600 frames" },
		{ "MaxPacket", RTP_HEADER + 3, RTP_MAX_PACKET, &C.max_packet, "15 to 1500 bytes, the RTP header included" },
		{ "PayloadType", 96, 127, &C.pt, "96 to 127" },
		{ "Threads", 0, 16, &C.threads, "0 (x264 chooses) to 16" },
	};
	for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
		if (strcmp(key, nums[i].key))
			continue;
		if (parse_uint(value, nums[i].lo, nums[i].hi, &u) < 0) {
			snprintf(err, errlen, "%s: %s", key, nums[i].what);
			return -1;
		}
		*nums[i].out = u;
		return 0;
	}
	snprintf(err, errlen, "unknown key %s", key);
	return -1;
}

static int load_conf(const char *path)
{
	static char text[8192];
	size_t len;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		mod_log(WHO, "%s: %s", path, strerror(errno));
		return -1;
	}
	char why[256];
	if (hg_ini_parse(text, len, on_section, on_key, NULL, why, sizeof why) < 0) {
		mod_log(WHO, "%s: %s", path, why);
		hg_status(why);
		return -1;
	}
	return 0;
}

/* ---- The socket ---------------------------------------------------------------- */

static int sock = -1;
static uint64_t sent_packets, send_errors;

static int open_socket(void)
{
	char host[128], port[16];
	const char *c = strrchr(C.peer, ':');
	size_t h = (size_t)(c - C.peer);
	if (h >= sizeof host)
		return -1;
	memcpy(host, C.peer, h);
	host[h] = 0;
	snprintf(port, sizeof port, "%s", c + 1);
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
	struct addrinfo *ai;
	if (getaddrinfo(host, port, &hints, &ai) != 0) {
		mod_log(WHO, "Peer %s: cannot be resolved", C.peer);
		return -1;
	}
	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int big = 4 << 20; /* a keyframe's packets at once */
	if (sock >= 0)
		setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
	if (sock < 0 || connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
		mod_log(WHO, "Peer %s: %s", C.peer, strerror(errno));
		freeaddrinfo(ai);
		return -1;
	}
	freeaddrinfo(ai);
	return 0;
}

/* Nobody listening (QGroundControl not open) is an error on a connected UDP
 * socket on loopback: counted, never a reason to stop. */
static void send_packet(void *ctx, const uint8_t *pkt, size_t len)
{
	(void)ctx;
	if (send(sock, pkt, len, 0) == (ssize_t)len)
		sent_packets++;
	else
		send_errors++;
}

/* ---- I422 full range to I420 limited range (hellion's to_i420) ------------------ */

/* Y' = 16 + Y*219/255 and C' = 128 + (C-128)*224/255, rounded, in 16-bit fixed
 * point with every term positive; 4:2:2 to 4:2:0 averages each pair of chroma
 * rows (legacy/hellion/sinks.c, where it replaced ffmpeg's scale filter). */
static inline uint8_t y_to_tv(uint32_t y)
{
	return (uint8_t)((y * 56284u + 16u * 65536u + 32768u) >> 16);
}

static inline uint8_t c_to_tv(uint32_t c)
{
	return (uint8_t)((c * 57570u + 1019648u + 32768u) >> 16);
}

static void to_i420(const struct hg_frame_header *hd, const uint8_t *data, x264_image_t *img)
{
	uint32_t w = hd->width, h = hd->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
	for (uint32_t r = 0; r < h; r++) {
		const uint8_t *s = data + hd->offset[0] + (size_t)r * hd->stride[0];
		uint8_t *d = img->plane[0] + (size_t)r * (size_t)img->i_stride[0];
		for (uint32_t x = 0; x < w; x++)
			d[x] = y_to_tv(s[x]);
	}
	for (int p = 1; p <= 2; p++) {
		const uint8_t *src = data + hd->offset[p];
		for (uint32_t r = 0; r < ch; r++) {
			const uint8_t *a = src + (size_t)(2 * r) * hd->stride[p];
			const uint8_t *b = 2 * r + 1 < h ? a + hd->stride[p] : a;
			uint8_t *d = img->plane[p] + (size_t)r * (size_t)img->i_stride[p];
			for (uint32_t x = 0; x < cw; x++)
				d[x] = c_to_tv(((uint32_t)a[x] + b[x] + 1) >> 1);
		}
	}
}

/* ---- The encoder ----------------------------------------------------------------- */

static x264_t *enc;
static x264_picture_t pic;
static int pic_ok;
static uint32_t enc_w, enc_h;
static struct rtp_h264 R;
static uint64_t frames, skipped, keyframes, bad_frames, encode_ns_sum, encode_ns_max, bytes_window;
static unsigned bitrate_now;

static unsigned effective_kbps(void)
{
	return P.bitrate_kbps < P.max_bitrate_kbps ? P.bitrate_kbps : P.max_bitrate_kbps;
}

/* The rate control: an average bitrate held by a VBV of half a second, as
 * hellion's -maxrate/-bufsize gave ffmpeg's libx264. */
static void set_rate(x264_param_t *p, unsigned kbps)
{
	p->rc.i_rc_method = X264_RC_ABR;
	p->rc.i_bitrate = (int)kbps;
	p->rc.i_vbv_max_bitrate = (int)kbps;
	p->rc.i_vbv_buffer_size = (int)(kbps / 2 ? kbps / 2 : 1);
}

static void encoder_close(void)
{
	if (enc)
		x264_encoder_close(enc);
	enc = NULL;
	if (pic_ok)
		x264_picture_clean(&pic);
	pic_ok = 0;
}

static int encoder_open(uint32_t w, uint32_t h)
{
	encoder_close();
	x264_param_t p;
	if (x264_param_default_preset(&p, "ultrafast", "zerolatency") < 0)
		return -1;
	p.i_width = (int)w;
	p.i_height = (int)h;
	p.i_csp = X264_CSP_I420;
	p.i_fps_num = C.fps;
	p.i_fps_den = 1;
	p.i_timebase_num = 1;
	p.i_timebase_den = C.fps;
	p.b_vfr_input = 0;
	p.i_keyint_max = (int)C.keyint;
	p.i_keyint_min = (int)C.keyint;
	p.i_scenecut_threshold = 0; /* a keyframe every KeyInterval frames, no others */
	p.b_repeat_headers = 1;     /* SPS and PPS before each keyframe */
	p.b_annexb = 1;
	p.vui.b_fullrange = 0;
	p.i_log_level = X264_LOG_WARNING;
	if (C.threads)
		p.i_threads = (int)C.threads;
	bitrate_now = effective_kbps();
	set_rate(&p, bitrate_now);
	enc = x264_encoder_open(&p);
	if (!enc) {
		mod_log(WHO, "x264_encoder_open %ux%u failed", w, h);
		return -1;
	}
	if (x264_picture_alloc(&pic, X264_CSP_I420, (int)w, (int)h) < 0) {
		encoder_close();
		return -1;
	}
	pic_ok = 1;
	enc_w = w;
	enc_h = h;
	mod_log(WHO, "encoding %ux%u at %u frames/s, %u kbit/s, a keyframe every %u frames", w, h, C.fps, bitrate_now,
		C.keyint);
	return 0;
}

static void apply_bitrate(void)
{
	unsigned kbps = effective_kbps();
	if (!enc || kbps == bitrate_now) {
		bitrate_now = kbps;
		return;
	}
	x264_param_t p;
	x264_encoder_parameters(enc, &p);
	set_rate(&p, kbps);
	if (x264_encoder_reconfig(enc, &p) < 0) {
		mod_log(WHO, "bitrate %u kbit/s: x264 refused the change", kbps);
		return;
	}
	mod_log(WHO, "bitrate %u -> %u kbit/s", bitrate_now, kbps);
	bitrate_now = kbps;
}

/* ---- Frames in, frames out --------------------------------------------------------- */

static hg_sub *sub_frames;
static hg_pub *pub_index;
static uint32_t source_id;
static uint8_t *fbuf;
static size_t fcap;

static int frame_encodable(const struct hg_frame_header *hd)
{
	return hd->fourcc == HG_I422 && hd->width && hd->height && !(hd->width & 1) && !(hd->height & 1);
}

/* 1 if every byte to_i420 reads for this header lies inside `bytes` of plane
 * data. A header read in place may be half of one frame and half of the
 * next (hg_topic.h, hg_latest_view): nothing in it sizes or indexes a read
 * until this has said the reads stay inside the payload. */
static int planes_inside(const struct hg_frame_header *hd, size_t bytes)
{
	uint64_t w = hd->width, h = hd->height;
	if (w > 16384 || h > 16384)
		return 0;
	for (int p = 0; p < 3; p++) {
		uint64_t pw = p ? (w + 1) / 2 : w;
		if (hd->stride[p] < pw || (uint64_t)hd->offset[p] + (h - 1) * hd->stride[p] + pw > bytes)
			return 0;
	}
	return 1;
}

/* The picture in pic.img, already converted, to x264 and out as RTP; t0 is
 * when the conversion started. */
static void encode_converted(const struct hg_info *info, uint64_t t0)
{
	pic.i_pts = (int64_t)info->seq;
	pic.i_type = X264_TYPE_AUTO;
	x264_nal_t *nals;
	int nn;
	x264_picture_t out;
	int size = x264_encoder_encode(enc, &nals, &nn, &pic, &out);
	if (size <= 0)
		return; /* zerolatency: nothing is held back, so nothing comes later */
	/* The NAL units of one encode are contiguous: one access unit. */
	uint64_t seq = (uint64_t)out.i_pts;
	uint32_t ts = (uint32_t)(seq * 90000u / C.fps);
	uint32_t bytes = 0;
	int fl = rtp_h264_send_au(&R, nals[0].p_payload, (size_t)size, ts, send_packet, NULL, &bytes);
	uint64_t dt = hg_now_ns() - t0;
	encode_ns_sum += dt;
	if (dt > encode_ns_max)
		encode_ns_max = dt;
	frames++;
	bytes_window += bytes;
	int key = out.b_keyframe || (fl >= 0 && (fl & RTP_AU_KEYFRAME));
	keyframes += key != 0;
	if (pub_index) {
		struct video_index m = { 0 };
		m.frame_topic_id = source_id;
		m.rtp_timestamp = ts;
		m.frame_seq = seq;
		m.capture_ns = info->stamp_ns;
		m.bytes = bytes;
		m.keyframe = key ? 1 : 0;
		m.params_added = fl >= 0 && (fl & RTP_AU_PARAMS_ADDED) ? 1 : 0;
		/* The reference is to the frame just read (Inputs=<source>): the newest. */
		hg_publish(pub_index, &m, sizeof m, info->stamp_ns ? info->stamp_ns : hg_now_ns());
	}
}

/* A frame that was copied out of the ring: a whole message, so its header is
 * believed. This is the path that opens the encoder, at the first frame and
 * when the picture's size changes. */
static void encode(const uint8_t *payload, size_t len, const struct hg_info *info)
{
	struct hg_frame_header hd;
	if (!payload || len < sizeof hd)
		return;
	memcpy(&hd, payload, sizeof hd);
	if (!frame_encodable(&hd)) {
		if (!bad_frames++)
			mod_log(WHO, "%s: a frame not I422 of even size (fourcc %08x, %ux%u): not encoded", C.source,
				hd.fourcc, hd.width, hd.height);
		return;
	}
	/* The header is the publisher's word. One whose strides and offsets reach
	 * past the payload would have the conversion read past the buffer. */
	if (!planes_inside(&hd, len - sizeof hd)) {
		if (!bad_frames++)
			mod_log(WHO, "%s: a %ux%u frame whose planes do not lie inside its %zu bytes: not encoded",
				C.source, hd.width, hd.height, len - sizeof hd);
		return;
	}
	if ((!enc || hd.width != enc_w || hd.height != enc_h) && encoder_open(hd.width, hd.height) < 0)
		return;
	uint64_t t0 = hg_now_ns();
	to_i420(&hd, payload + sizeof hd, &pic.img);
	encode_converted(info, t0);
}

static uint64_t last_seq, views_lost;
static uint32_t last_epoch;

/* The frames between the last one taken and this one were never encoded (a
 * jump in the RTP timestamps, as docs/11 has it for a frame dropped before
 * encoding). */
static void count_skipped(const struct hg_info *info)
{
	if (last_seq && info->epoch == last_epoch && info->seq > last_seq + 1)
		skipped += info->seq - last_seq - 1;
	last_seq = info->seq;
	last_epoch = info->epoch;
}

/* A frame copied out of the ring. By its seq, the frame a view was of: the one
 * that opens the encoder, or one whose header says something the encoder is
 * not open for; HG_GONE (the writer has come round to it) is a frame not
 * encoded, and the next one takes the same way. Or the newest, for a writer
 * that keeps overwriting the frame while it is converted in place. */
static void read_copy(uint64_t seq, int newest)
{
	struct hg_info info;
	for (int grown = 0;;) {
		uint32_t need = hg_sub_payload_bytes(sub_frames);
		if (need > fcap) {
			uint8_t *n = realloc(fbuf, need);
			if (!n)
				return;
			fbuf = n;
			fcap = need;
		}
		enum hg_result r = newest ? hg_latest(sub_frames, fbuf, fcap, &info)
					  : hg_read_seq(sub_frames, seq, fbuf, fcap, &info);
		if (r == HG_MSG) {
			count_skipped(&info);
			encode(fbuf, need, &info);
		} else if (r == HG_ERROR && errno == EMSGSIZE && !grown++) {
			continue; /* replaced by a larger frame: the buffer grows above, once */
		}
		return;
	}
}

/* The newest frame is encoded and the ones behind it skipped, as before; what
 * changed is that the frame is not copied out of the ring first. A 1280x720
 * I422 frame is 1.8 MB, and the only thing done with it is to_i420, which
 * writes x264's own picture: so the conversion reads the ring's slot in place
 * (hg_latest_view), and hg_view_done says afterwards whether the slot held
 * that frame throughout. Only then is the picture given to x264: a frame the
 * writer overwrote during the conversion is dropped before the encoder, whose
 * references it would have spoiled, has seen any of it.
 *
 * The header is read through the same pointer, so until hg_view_done it is
 * not believed: it is copied to a local, it must describe the picture the
 * encoder is already open for, and its offsets must stay inside the payload.
 * A header that says anything else (the first frame, a new size, a torn one)
 * sends the frame down the copying path, which reads a whole message and
 * checks the same things on it. */
static void read_frames(void)
{
	for (int tries = 0; tries < 3; tries++) {
		const void *view;
		struct hg_info info;
		if (hg_latest_view(sub_frames, &view, &info) != HG_MSG)
			return;
		uint32_t payload_bytes = hg_sub_payload_bytes(sub_frames); /* a field read: the view stays open */
		struct hg_frame_header hd;
		if (payload_bytes < sizeof hd) {
			hg_view_done(sub_frames);
			return;
		}
		memcpy(&hd, view, sizeof hd);
		if (!enc || !frame_encodable(&hd) || hd.width != enc_w || hd.height != enc_h ||
		    !planes_inside(&hd, payload_bytes - sizeof hd)) {
			hg_view_done(sub_frames);
			read_copy(info.seq, 0);
			return;
		}
		uint64_t t0 = hg_now_ns();
		to_i420(&hd, (const uint8_t *)view + sizeof hd, &pic.img);
		if (hg_view_done(sub_frames) == 1) {
			count_skipped(&info);
			encode_converted(&info, t0);
			return;
		}
		/* Overwritten while it was converted: a newer frame is there. */
		views_lost++;
	}
	/* Three in a row: the writer comes round the ring faster than a picture
	 * is converted (a camera does not; a ring of few slots under a burst
	 * does). A copy is shorter than a conversion and is what was done before:
	 * the newest frame that way, so that a fast writer cannot keep the encoder
	 * from ever sending one. */
	read_copy(0, 1);
}

/* ---- Parameters, status ------------------------------------------------------------ */

static int validate(const void *candidate, const void *current, char reason[64])
{
	(void)current;
	const struct topside_encoder_params *c = candidate;
	if (c->bitrate_kbps > c->max_bitrate_kbps) {
		snprintf(reason, 64, "bitrate_kbps %u over max_bitrate_kbps %u", c->bitrate_kbps, c->max_bitrate_kbps);
		return -1;
	}
	return 0;
}

static char status[200];
static uint64_t last_status_ns, last_frames;

static void status_line(uint64_t now)
{
	if (now - last_status_ns < 1000000000ull)
		return;
	double dt = last_status_ns ? (double)(now - last_status_ns) / 1e9 : 1.0;
	char line[200];
	mod_fmt(line, sizeof line, "%s to %s: %.1f frames/s, %.0f kbit/s (target %u), %llu keyframes, %llu skipped, %llu send errors",
		C.source, C.peer, (double)(frames - last_frames) / dt, (double)bytes_window * 8.0 / 1000.0 / dt, bitrate_now,
		(unsigned long long)keyframes, (unsigned long long)skipped, (unsigned long long)send_errors);
	last_status_ns = now;
	last_frames = frames;
	bytes_window = 0;
	if (strcmp(line, status)) {
		snprintf(status, sizeof status, "%s", line);
		hg_status(line);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s topside_encoder.conf\n", argv[0]);
		return 2;
	}
	mod_init(WHO);
	if (load_conf(argv[1]) < 0)
		return 78;
	if (open_socket() < 0)
		return 78;
	if (hg_params_open(&TOPSIDE_ENCODER_PARAMS_TYPE, &P, validate, NULL) == 0)
		have_params = 1;
	else
		mod_log(WHO, "no parameters (%s): bitrate %u kbit/s, fixed", strerror(errno), effective_kbps());
	sub_frames = hg_sub_open(C.source, NULL);
	if (!sub_frames) {
		mod_log(WHO, "Source %s: %s", C.source, strerror(errno));
		return 78;
	}
	source_id = hg_topic_id(C.source);
	struct hg_pub_opts io = { .slots = 64 };
	pub_index = hg_pub_open(C.index, &VIDEO_INDEX_TYPE, &io);
	if (!pub_index)
		mod_log(WHO, "Index %s: %s; no index", C.index, strerror(errno));
	uint32_t ssrc = (uint32_t)(hg_now_ns() ^ ((uint64_t)getpid() << 16));
	rtp_h264_init(&R, ssrc, (uint8_t)C.pt, C.max_packet);

	/* The wait set: the frame topic and the call topics, each read on every
	 * wake (hangar#1: nothing waited on is left unread). */
	hg_sub *subs[HG_MAX_WAIT];
	int n = 0;
	subs[n++] = sub_frames;
	int nc = hg_call_subs(subs + n, HG_MAX_WAIT - n);
	if (nc > 0)
		n += nc;
	else
		nc = 0;
	mod_log(WHO, "%s to %s over RTP (payload type %u, packets of at most %u bytes), %u frames/s, index %s", C.source,
		C.peer, C.pt, C.max_packet, C.fps, C.index);
	hg_ready();
	while (!mod_stop) {
		unsigned r = hg_wait(subs, n, 1000);
		if (r & HG_READY_INTERRUPT)
			break;
		read_frames();
		if (nc)
			hg_serve();
		if (have_params && hg_params_changed()) {
			mod_log(WHO, "parameters v%llu by %s", (unsigned long long)hg_params_version(), hg_params_changed_by());
			apply_bitrate();
		}
		status_line(hg_now_ns());
	}
	mod_log(WHO, "stopping: %llu frames sent (%llu keyframes, %llu skipped, %llu overwritten while read, %llu not encodable), %llu packets, %llu send errors; encode %.2f ms a frame on average, %.2f ms at most",
		(unsigned long long)frames, (unsigned long long)keyframes, (unsigned long long)skipped,
		(unsigned long long)views_lost, (unsigned long long)bad_frames, (unsigned long long)sent_packets,
		(unsigned long long)send_errors,
		frames ? (double)encode_ns_sum / (double)frames / 1e6 : 0.0, (double)encode_ns_max / 1e6);
	encoder_close();
	return 0;
}
