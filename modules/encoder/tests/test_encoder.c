// SPDX-License-Identifier: GPL-3.0-or-later
// The encoder (modules/encoder/README.md): the RFC 6184 packetizer and the
// access-unit splitter on synthetic NAL units; the module from an Annex B
// file to a UDP socket, its RTP reassembled and compared byte for byte with
// the input, video.index read back, and its CPU once the file has ended; and,
// where ffmpeg with libx264 is installed, ffmpeg decoding the module's RTP
// from the SDP hellion writes (payload type 96, H264/90000,
// packetization-mode=1, no sprop-parameter-sets), every frame.
#define _GNU_SOURCE
#include "test_harness.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "h264.h"
#include "hg_topic.h"
#include "video_index.h"

static char root[512], topics[600], enc_wiring[600], tester_wiring[600];
static const char *enc_bin;

static uint64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) < 0 && errno == EINTR)
		;
}

static void write_bytes(const char *path, const void *p, size_t n)
{
	FILE *f = fopen(path, "w");
	if (!f || fwrite(p, 1, n, f) != n) {
		perror(path);
		exit(2);
	}
	fclose(f);
}

static void write_text(const char *path, const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	write_bytes(path, buf, (size_t)n);
}

/* ---- Synthetic H.264 ----------------------------------------------------- */

/* A NAL unit of the given type and size, its payload free of 00 00 (so no
 * start code can appear in it), first_mb_in_slice 0 or not for a slice. */
static size_t make_nal(uint8_t *out, uint8_t type, size_t len, int first_mb_zero, unsigned seed)
{
	out[0] = (uint8_t)(0x60 | type); /* nal_ref_idc 3 */
	for (size_t i = 1; i < len; i++) {
		seed = seed * 1103515245u + 12345u;
		out[i] = (uint8_t)(1 + (seed >> 16) % 255);
	}
	if (len > 1 && type >= 1 && type <= 5)
		out[1] = (uint8_t)(first_mb_zero ? (out[1] | 0x80) : (out[1] & 0x7f));
	return len;
}

struct stream {
	uint8_t buf[1 << 20];
	size_t len;
};

static void put_nal(struct stream *s, uint8_t type, size_t len, int first_mb_zero, unsigned seed, int four)
{
	if (four)
		s->buf[s->len++] = 0;
	s->buf[s->len++] = 0;
	s->buf[s->len++] = 0;
	s->buf[s->len++] = 1;
	s->len += make_nal(s->buf + s->len, type, len, first_mb_zero, seed);
}

/* ---- Reassembly of what a receiver gets -------------------------------- */

struct got_nal {
	uint8_t p[16384];
	size_t len;
};

struct got {
	struct got_nal nal[512];
	int n_nal;
	int au_end[256];      /* index of the first NAL after each access unit */
	uint32_t au_ts[256];
	int n_au;
	int markers, packets, too_big, bad_fu;
	uint16_t seqs[4096];
	int n_seqs;
	int open; /* an FU-A under way */
};

static void got_packet(struct got *g, const uint8_t *pkt, size_t len, size_t max_packet)
{
	if (len < RTP_HEADER + 1 || (pkt[0] & 0xc0) != 0x80)
		return;
	g->packets++;
	if (len > max_packet)
		g->too_big++;
	if (g->n_seqs < 4096)
		g->seqs[g->n_seqs++] = (uint16_t)(pkt[2] << 8 | pkt[3]);
	uint32_t ts = (uint32_t)pkt[4] << 24 | (uint32_t)pkt[5] << 16 | (uint32_t)pkt[6] << 8 | pkt[7];
	const uint8_t *p = pkt + RTP_HEADER;
	size_t n = len - RTP_HEADER;
	uint8_t type = p[0] & 0x1f;
	if (type == H264_NAL_FU_A) {
		int s = p[1] & 0x80, e = p[1] & 0x40;
		if (s) {
			if (g->open)
				g->bad_fu++;
			struct got_nal *x = &g->nal[g->n_nal];
			x->p[0] = (uint8_t)((p[0] & 0xe0) | (p[1] & 0x1f));
			x->len = 1;
			g->open = 1;
		} else if (!g->open) {
			g->bad_fu++;
			return;
		}
		struct got_nal *x = &g->nal[g->n_nal];
		if (x->len + n - 2 <= sizeof x->p) {
			memcpy(x->p + x->len, p + 2, n - 2);
			x->len += n - 2;
		}
		if (e) {
			g->open = 0;
			g->n_nal++;
		}
	} else {
		if (g->open)
			g->bad_fu++;
		struct got_nal *x = &g->nal[g->n_nal++];
		memcpy(x->p, p, n);
		x->len = n;
	}
	if (pkt[1] & 0x80) {
		g->markers++;
		if (g->open)
			g->bad_fu++; /* a marker inside a fragmented NAL unit */
		g->au_ts[g->n_au] = ts;
		g->au_end[g->n_au++] = g->n_nal;
	}
}

static struct got G;
static size_t cur_max = 1200;

static void collect(void *ctx, const uint8_t *pkt, size_t len)
{
	got_packet(ctx, pkt, len, cur_max);
}

/* ---- The packetizer and the splitter ------------------------------------ */

static void test_nals_and_access_units(void)
{
	static struct stream s;
	s.len = 0;
	put_nal(&s, H264_NAL_SPS, 12, 0, 1, 1);
	put_nal(&s, H264_NAL_PPS, 5, 0, 2, 0);
	put_nal(&s, H264_NAL_IDR, 300, 1, 3, 1);
	put_nal(&s, H264_NAL_IDR, 200, 0, 4, 0); /* a second slice of the same frame */
	s.buf[s.len++] = 0;                      /* trailing_zero_8bits */
	s.buf[s.len++] = 0;
	size_t first_au = s.len;
	put_nal(&s, H264_NAL_SLICE, 150, 1, 5, 1);
	size_t second_au = s.len;
	put_nal(&s, H264_NAL_AUD, 2, 0, 6, 1);
	put_nal(&s, H264_NAL_SLICE, 90, 1, 7, 0);
	struct h264_nal n[16];
	CHECK_EQ_INT(h264_nals(s.buf, s.len, n, 16), 7);
	CHECK(n[0].type == H264_NAL_SPS && n[0].len == 12 && n[1].type == H264_NAL_PPS && n[1].len == 5);
	CHECK(n[2].type == H264_NAL_IDR && n[2].len == 300 && n[3].len == 200); /* the zeros trimmed */
	CHECK(n[6].type == H264_NAL_SLICE && n[6].len == 90);
	size_t a = h264_au_len(s.buf, s.len);
	CHECK_EQ_INT((int)a, (int)first_au - 0 - 0);
	size_t b = h264_au_len(s.buf + a, s.len - a);
	CHECK_EQ_INT((int)(a + b), (int)second_au);
	size_t c = h264_au_len(s.buf + a + b, s.len - a - b);
	CHECK_EQ_INT((int)(a + b + c), (int)s.len);
	CHECK_EQ_INT((int)h264_au_len(s.buf, 2), 0);
}

static void test_single_fu_a_marker(void)
{
	static struct stream s;
	s.len = 0;
	put_nal(&s, H264_NAL_SPS, 20, 0, 11, 1);
	put_nal(&s, H264_NAL_PPS, 6, 0, 12, 1);
	put_nal(&s, H264_NAL_IDR, 5000, 1, 13, 1);
	struct rtp_h264 r;
	rtp_h264_init(&r, 0x11223344, 96, 1200);
	cur_max = 1200;
	memset(&G, 0, sizeof G);
	uint32_t bytes = 0;
	int f = rtp_h264_send_au(&r, s.buf, s.len, 90000, collect, &G, &bytes);
	CHECK_EQ_INT(f, (int)RTP_AU_KEYFRAME);
	CHECK_EQ_INT((int)bytes, (int)(s.len));
	/* SPS and PPS single; 4999 bytes after the NAL header in fragments of
	 * 1200 - 12 - 2 = 1186: five. */
	CHECK_EQ_INT(G.packets, 2 + 5);
	CHECK_EQ_INT(G.too_big, 0);
	CHECK_EQ_INT(G.bad_fu, 0);
	CHECK_EQ_INT(G.markers, 1);
	CHECK_EQ_INT(G.n_nal, 3);
	struct h264_nal n[4];
	h264_nals(s.buf, s.len, n, 4);
	for (int i = 0; i < 3; i++)
		CHECK(G.nal[i].len == n[i].len && memcmp(G.nal[i].p, n[i].p, n[i].len) == 0);
	CHECK(G.au_ts[0] == 90000);
	/* A NAL unit that fills a packet exactly goes whole; one byte more is two fragments. */
	s.len = 0;
	put_nal(&s, H264_NAL_SLICE, 1200 - RTP_HEADER, 1, 14, 0);
	memset(&G, 0, sizeof G);
	rtp_h264_send_au(&r, s.buf, s.len, 1, collect, &G, NULL);
	CHECK_EQ_INT(G.packets, 1);
	s.len = 0;
	put_nal(&s, H264_NAL_SLICE, 1200 - RTP_HEADER + 1, 1, 15, 0);
	memset(&G, 0, sizeof G);
	rtp_h264_send_au(&r, s.buf, s.len, 2, collect, &G, NULL);
	CHECK_EQ_INT(G.packets, 2);
	CHECK_EQ_INT(G.bad_fu, 0);
	CHECK(G.nal[0].len == 1200 - RTP_HEADER + 1 && memcmp(G.nal[0].p, s.buf + 3, G.nal[0].len) == 0);
}

static void test_sequence_wrap_and_timestamps(void)
{
	static struct stream s;
	s.len = 0;
	put_nal(&s, H264_NAL_SLICE, 3000, 1, 21, 1); /* three packets */
	struct rtp_h264 r;
	rtp_h264_init(&r, 1, 96, 1200);
	r.seq = 65534;
	memset(&G, 0, sizeof G);
	rtp_h264_send_au(&r, s.buf, s.len, 3000, collect, &G, NULL);
	rtp_h264_send_au(&r, s.buf, s.len, 6000, collect, &G, NULL);
	CHECK_EQ_INT(G.n_seqs, 6);
	CHECK(G.seqs[0] == 65534 && G.seqs[1] == 65535 && G.seqs[2] == 0 && G.seqs[5] == 3);
	CHECK(G.n_au == 2 && G.au_ts[0] == 3000 && G.au_ts[1] == 6000);
	CHECK_EQ_INT(G.markers, 2);
}

static void test_parameter_sets_inserted(void)
{
	static struct stream a, p, idr;
	a.len = p.len = idr.len = 0;
	put_nal(&a, H264_NAL_SPS, 20, 0, 31, 1);
	put_nal(&a, H264_NAL_PPS, 6, 0, 32, 1);
	put_nal(&a, H264_NAL_IDR, 800, 1, 33, 1);
	put_nal(&p, H264_NAL_SLICE, 400, 1, 34, 1);
	put_nal(&idr, H264_NAL_IDR, 900, 1, 35, 1);
	struct rtp_h264 r;
	rtp_h264_init(&r, 1, 96, 1200);
	memset(&G, 0, sizeof G);
	CHECK_EQ_INT(rtp_h264_send_au(&r, a.buf, a.len, 1, collect, &G, NULL), (int)RTP_AU_KEYFRAME);
	CHECK_EQ_INT(rtp_h264_send_au(&r, p.buf, p.len, 2, collect, &G, NULL), 0);
	uint32_t bytes;
	CHECK_EQ_INT(rtp_h264_send_au(&r, idr.buf, idr.len, 3, collect, &G, &bytes),
		     (int)(RTP_AU_KEYFRAME | RTP_AU_PARAMS_ADDED));
	CHECK_EQ_INT((int)bytes, (int)(4 + 20 + 4 + 6 + idr.len));
	/* The third access unit: the cached SPS and PPS, then the IDR. */
	CHECK_EQ_INT(G.n_au, 3);
	int i = G.au_end[1];
	CHECK_EQ_INT(G.au_end[2] - i, 3);
	CHECK((G.nal[i].p[0] & 0x1f) == H264_NAL_SPS && G.nal[i].len == 20);
	CHECK((G.nal[i + 1].p[0] & 0x1f) == H264_NAL_PPS && (G.nal[i + 2].p[0] & 0x1f) == H264_NAL_IDR);
	CHECK(r.params_added == 1);
	/* No access unit, nothing sent. */
	CHECK_EQ_INT(rtp_h264_send_au(&r, (const uint8_t *)"\0\0", 2, 4, collect, &G, NULL), -1);
}

/* ---- The module ----------------------------------------------------------- */

static int udp_bound(int *port)
{
	int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	int big = 4 << 20;
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
	if (bind(s, (struct sockaddr *)&a, sizeof a) < 0)
		return -1;
	socklen_t l = sizeof a;
	getsockname(s, (struct sockaddr *)&a, &l);
	*port = ntohs(a.sin_port);
	return s;
}

static int free_port(void)
{
	int port, s = udp_bound(&port);
	close(s);
	return port;
}

/* The suite's camera for the next encoder started: tests/fake_uvc.c as
 * LD_PRELOAD, unplugged after `unplug_s` seconds. NULL: no camera. */
static const char *fake_so;
static const char *fake_unplug_s;

static pid_t spawn_encoder(const char *conf, const char *log)
{
	pid_t pid = fork();
	if (pid == 0) {
		if (fake_so) {
			setenv("LD_PRELOAD", fake_so, 1);
			setenv("FAKE_UVC", "/dev/fake-uvc", 1);
			setenv("FAKE_UVC_UNPLUG_S", fake_unplug_s, 1);
		}
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		setenv("HANGAR_WIRING_FILE", enc_wiring, 1);
		setenv("HANGAR_MODULE", "encoder", 1);
		execl(enc_bin, enc_bin, conf, (char *)NULL);
		_exit(127);
	}
	return pid;
}

static int stop(pid_t pid)
{
	kill(pid, SIGTERM);
	uint64_t t0 = now_ms();
	for (;;) {
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		if (now_ms() - t0 > 5000) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			return -1;
		}
		sleep_ms(10);
	}
}

#define FRAMES 40

static void test_file_to_udp(void)
{
	/* 40 frames: an IDR with its parameter sets, P frames of sizes from a
	 * single packet to several fragments, an IDR without parameter sets every
	 * tenth frame (the encoder must put the cached ones in front). */
	static struct stream s;
	s.len = 0;
	int idr[FRAMES] = { 0 };
	for (int f = 0; f < FRAMES; f++) {
		if (f == 0) {
			put_nal(&s, H264_NAL_SPS, 18, 0, 100, 1);
			put_nal(&s, H264_NAL_PPS, 7, 0, 101, 1);
		}
		idr[f] = f % 10 == 0;
		size_t len = idr[f] ? 6000 : (size_t)(100 + (f * 397) % 3500);
		put_nal(&s, idr[f] ? H264_NAL_IDR : H264_NAL_SLICE, len, 1, 200u + (unsigned)f, f % 2);
		if (f % 3 == 0) /* a second slice in some frames */
			put_nal(&s, idr[f] ? H264_NAL_IDR : H264_NAL_SLICE, 700, 0, 300u + (unsigned)f, 1);
	}
	char path[700], conf[700], log[700];
	snprintf(path, sizeof path, "%s/in.h264", root);
	write_bytes(path, s.buf, s.len);
	int port, rx = udp_bound(&port);
	CHECK(rx >= 0);
	snprintf(conf, sizeof conf, "%s/encoder.conf", root);
	write_text(conf, "[Encoder]\nSource=file:%s\nPeer=127.0.0.1:%d\nFrameRate=100\nMaxPacket=1200\n", path, port);
	snprintf(log, sizeof log, "%s/encoder.log", root);
	pid_t pid = spawn_encoder(conf, log);
	memset(&G, 0, sizeof G);
	cur_max = 1200;
	uint64_t t0 = now_ms();
	uint8_t pkt[2048];
	struct timeval tv = { 0, 100000 };
	setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	while (G.markers < FRAMES && now_ms() - t0 < 10000) {
		ssize_t n = recv(rx, pkt, sizeof pkt, 0);
		if (n > 0)
			got_packet(&G, pkt, (size_t)n, cur_max);
	}
	printf("    %d frames, %d packets received in %llu ms\n", G.markers, G.packets,
	       (unsigned long long)(now_ms() - t0));
	CHECK_EQ_INT(G.markers, FRAMES);
	CHECK_EQ_INT(G.too_big, 0);
	CHECK_EQ_INT(G.bad_fu, 0);
	for (int i = 1; i < G.n_seqs; i++)
		if ((uint16_t)(G.seqs[i - 1] + 1) != G.seqs[i]) {
			CHECK(!"RTP sequence numbers in order");
			break;
		}
	/* Byte for byte: each frame's NAL units as sent, the cached parameter
	 * sets in front of each IDR frame after the first. */
	struct h264_nal in[256];
	int nin = h264_nals(s.buf, s.len, in, 256), k = 0, same = 1, nal = 0;
	for (int f = 0; f < FRAMES && f < G.n_au; f++) {
		CHECK(G.au_ts[f] == (uint32_t)(f + 1) * 900u); /* frame seq x 90000 / 100 */
		if (idr[f] && f > 0) {
			same &= G.nal[nal].len == in[0].len && !memcmp(G.nal[nal].p, in[0].p, in[0].len);
			same &= G.nal[nal + 1].len == in[1].len && !memcmp(G.nal[nal + 1].p, in[1].p, in[1].len);
			nal += 2;
		}
		for (; nal < G.au_end[f]; nal++, k++)
			same &= k < nin && G.nal[nal].len == in[k].len && !memcmp(G.nal[nal].p, in[k].p, in[k].len);
	}
	CHECK_EQ_INT(k, nin);
	CHECK(same);
	/* video.index: one message a frame. */
	setenv("HANGAR_WIRING_FILE", tester_wiring, 1);
	CHECK_EQ_INT(hg_init(), 0);
	hg_sub *vi = hg_sub_open_from("video.index", &VIDEO_INDEX_TYPE, HG_OLDEST);
	CHECK(vi != NULL);
	int nvi = 0, vi_ok = 1;
	struct video_index m;
	struct hg_info info;
	/* The module publishes a frame's index just after sending its packets, so
	 * the last one can still be on its way when the last marker has arrived
	 * here: read until all have come, or 2 s (a loaded machine read 39 of 40
	 * when this read once). */
	uint64_t t_vi = now_ms();
	while (vi && nvi < FRAMES && now_ms() - t_vi < 2000) {
		while (nvi < FRAMES && hg_next(vi, &m, sizeof m, &info) == HG_MSG) {
			nvi++;
			vi_ok &= m.frame_seq == (uint64_t)nvi && m.rtp_timestamp == (uint32_t)nvi * 900u &&
				 m.frame_topic_id == 0;
			vi_ok &= m.keyframe == idr[nvi - 1] && m.params_added == (idr[nvi - 1] && nvi > 1) && m.bytes > 0;
			vi_ok &= info.stamp_ns == m.capture_ns;
		}
		if (nvi < FRAMES)
			hg_wait(&vi, 1, 50);
	}
	CHECK_EQ_INT(nvi, FRAMES);
	CHECK(vi_ok);
	/* The file has ended: the module sleeps on its timer. */
	sleep_ms(200);
	long long c0 = th_cpu_ticks(pid);
	uint64_t m0 = now_ms();
	sleep_ms(1000);
	double pct = th_cpu_percent(c0, th_cpu_ticks(pid), (long)(now_ms() - m0));
	printf("    after the file: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 5);
	CHECK_EQ_INT(stop(pid), 0);
	close(rx);
	if (vi)
		hg_sub_close(vi);
}

static void test_conf_refused(void)
{
	char conf[700], log[700];
	snprintf(conf, sizeof conf, "%s/bad.conf", root);
	snprintf(log, sizeof log, "%s/bad.log", root);
	write_text(conf, "[Encoder]\nSource=rtsp://x\nPeer=127.0.0.1:1\n");
	pid_t pid = spawn_encoder(conf, log);
	int st;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 78);
	/* A node that is there and is no camera: the configuration is wrong, 78. */
	write_text(conf, "[Encoder]\nSource=uvc:/dev/null\nPeer=127.0.0.1:1\n");
	pid = spawn_encoder(conf, log);
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 78);
	/* A node that is not there: the camera is unplugged or not enumerated yet.
	 * Not 78, which the manager gives up on: 75, and it tries again. */
	write_text(conf, "[Encoder]\nSource=uvc:/dev/null-not-a-camera\nPeer=127.0.0.1:1\n");
	pid = spawn_encoder(conf, log);
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 75);
}

/* The camera unplugged while it streams. Every V4L2 request then answers
 * ENODEV and the descriptor reports an error at every poll; the encoder
 * went round its loop on that at 99 % of a core, sending nothing, and never
 * ended: the video was gone until someone restarted the module by hand. It
 * ends now, by itself, with the status the manager restarts on. The camera
 * is tests/fake_uvc.c, inside the encoder's process. */
static void test_camera_unplugged(void)
{
	fake_so = getenv("ENCODER_FAKE_UVC");
	if (!fake_so || access(fake_so, R_OK) != 0) {
		printf("    (skipped: no fake camera library; the toolchain makes no shared objects)\n");
		fake_so = NULL;
		return;
	}
	char conf[700], log[700];
	snprintf(conf, sizeof conf, "%s/cam.conf", root);
	snprintf(log, sizeof log, "%s/cam.log", root);
	int port, s = udp_bound(&port);
	CHECK(s >= 0);
	write_text(conf, "[Encoder]\nSource=uvc:/dev/fake-uvc\nPeer=127.0.0.1:%d\nWidth=1280\nHeight=720\nFrameRate=30\n", port);
	fake_unplug_s = "1.5";
	pid_t pid = spawn_encoder(conf, log);
	fake_so = NULL;
	/* It ends by itself: 1.5 s of frames, the unplug, then at most a loop's
	 * turn. Given 20 s; before the fix it never did. */
	int st = 0, ended = 0;
	uint64_t t0 = now_ms();
	while (now_ms() - t0 < 20000) {
		if (waitpid(pid, &st, WNOHANG) == pid) {
			ended = 1;
			break;
		}
		sleep_ms(20);
	}
	if (!ended) {
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
	}
	char *text = NULL;
	FILE *f = fopen(log, "r");
	static char buf[8192];
	if (f) {
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		buf[n] = 0;
		fclose(f);
		text = buf;
	}
	if (text && !strstr(text, "fake_uvc: opened")) {
		/* LD_PRELOAD did nothing (a static binary): there was no camera to unplug. */
		printf("    (skipped: the encoder did not load the fake camera; a static build?)\n");
		close(s);
		return;
	}
	CHECK(ended);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 75);
	if (ended && !(WIFEXITED(st) && WEXITSTATUS(st) == 75))
		printf("    the encoder ended with %s %d\n", WIFEXITED(st) ? "status" : "signal",
		       WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
	CHECK(text && strstr(text, "fake_uvc: unplugged") != NULL);
	CHECK(text && strstr(text, "/dev/fake-uvc is gone (No such device)") != NULL);
	/* It streamed before that: frames arrived at the peer. */
	unsigned char pkt[2048];
	int packets = 0;
	while (recv(s, pkt, sizeof pkt, MSG_DONTWAIT) > 0)
		packets++;
	CHECK(packets >= 10);
	printf("    %d RTP packets before the unplug; the encoder ended %s\n", packets, ended ? "by itself" : "only when killed");
	close(s);
}

/* ---- ffmpeg decodes it ---------------------------------------------------- */

static int run(const char *cmd)
{
	int rc = system(cmd);
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void test_ffmpeg_decodes(void)
{
	if (run("command -v ffmpeg >/dev/null 2>&1") != 0 ||
	    run("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264") != 0) {
		printf("    (skipped: no ffmpeg with libx264 on PATH)\n");
		return;
	}
	/* A real stream (three slices a frame, x264's sliced threads), its
	 * parameter sets then removed from every IDR frame but the first, as a
	 * camera that sends them once. (repeat-headers=0 would not do: ffmpeg's
	 * libx264 then keeps them out of band, and the raw .h264 file has none.)
	 * ffmpeg joins late, after the first IDR has gone by, as a topside
	 * started or restarted mid-dive: it decodes from the IDR at frame 30 on
	 * only with the parameter sets the encoder puts in front of it. (One that
	 * listens from the start keeps those of the first IDR and needs none:
	 * that case proves nothing about the insertion.) */
	char h264[700], sdp[700], raw[700], conf[700], log[700], cmd[3000];
	snprintf(h264, sizeof h264, "%s/real.h264", root);
	snprintf(sdp, sizeof sdp, "%s/in.sdp", root);
	snprintf(raw, sizeof raw, "%s/out.gray", root);
	snprintf(conf, sizeof conf, "%s/real.conf", root);
	snprintf(log, sizeof log, "%s/ffmpeg.log", root);
	snprintf(cmd, sizeof cmd,
		 "ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=30 -frames:v 60 "
		 "-c:v libx264 -preset ultrafast -tune zerolatency -g 30 -bf 0 -f h264 %s.full 2>>%s",
		 h264, log);
	CHECK_EQ_INT(run(cmd), 0);
	static struct stream full, once;
	char fullpath[720];
	snprintf(fullpath, sizeof fullpath, "%s.full", h264);
	FILE *f = fopen(fullpath, "r");
	full.len = f ? fread(full.buf, 1, sizeof full.buf, f) : 0;
	if (f)
		fclose(f);
	struct h264_nal nals[1024];
	int n = h264_nals(full.buf, full.len, nals, 1024), sps = 0, pps = 0, removed = 0;
	once.len = 0;
	for (int i = 0; i < n && i < 1024; i++) {
		if ((nals[i].type == H264_NAL_SPS && sps++) || (nals[i].type == H264_NAL_PPS && pps++)) {
			removed++;
			continue;
		}
		memcpy(once.buf + once.len, "\0\0\0\1", 4);
		memcpy(once.buf + once.len + 4, nals[i].p, nals[i].len);
		once.len += 4 + nals[i].len;
	}
	CHECK(sps == 2 && pps == 2 && removed == 2); /* the IDR at frame 30 had its own */
	write_bytes(h264, once.buf, once.len);
	int port = free_port();
	write_text(sdp,
		   "v=0\no=- 0 0 IN IP4 127.0.0.1\ns=hellion\nc=IN IP4 127.0.0.1\nt=0 0\n"
		   "m=video %d RTP/AVP 96\na=rtpmap:96 H264/90000\na=fmtp:96 packetization-mode=1\n",
		   port);
	write_text(conf, "[Encoder]\nSource=file:%s\nPeer=127.0.0.1:%d\nFrameRate=15\n", h264, port);
	char elog[700];
	snprintf(elog, sizeof elog, "%s/encoder-ffmpeg.log", root);
	pid_t enc = spawn_encoder(conf, elog);
	sleep_ms(400); /* about frame 6 of 60, at 15 frames/s: the first IDR is gone */
	pid_t ff = fork();
	if (ff == 0) {
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "warning", "-protocol_whitelist", "file,udp,rtp",
		       "-reorder_queue_size", "0", "-i", sdp, "-f", "rawvideo", "-pix_fmt", "gray", "-y", raw,
		       (char *)NULL);
		_exit(127);
	}
	sleep_ms(4000 + 1000); /* the rest of the 60 frames, and room */
	CHECK_EQ_INT(stop(enc), 0);
	/* ffmpeg analyses its input for up to 5 s before its first output; it is
	 * interrupted only after that, then writes what it has and exits. */
	sleep_ms(4500);
	kill(ff, SIGINT);
	int st;
	uint64_t t0 = now_ms();
	while (waitpid(ff, &st, WNOHANG) != ff && now_ms() - t0 < 5000)
		sleep_ms(20);
	if (now_ms() - t0 >= 5000) {
		kill(ff, SIGKILL);
		waitpid(ff, &st, 0);
	}
	struct stat sb;
	long frames = stat(raw, &sb) == 0 ? (long)(sb.st_size / (320 * 240)) : 0;
	printf("    ffmpeg, joining late, decoded %ld of frames 30 to 59 from the module's RTP\n", frames);
	/* From the IDR the encoder put the parameter sets in front of; 0 with
	 * the insertion taken out. How many of frames 30 to 59 depends on the
	 * ffmpeg: 23 or 24 with 9.0 (Arch), 12 with 6.1 (Ubuntu 24.04, CI),
	 * measured 2026-11-20; the rest go while it still probes its input, on a
	 * socket it does not read meanwhile. Half the fewest seen is the bar. */
	CHECK(frames >= 6 && frames <= 30);
	if (frames < 6) {
		snprintf(cmd, sizeof cmd, "tail -5 %s", log);
		run(cmd);
	}
}

int main(void)
{
	enc_bin = getenv("ENCODER_BIN");
	if (!enc_bin) {
		fprintf(stderr, "ENCODER_BIN must be set\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(root, sizeof root, "/dev/shm/hangar-test-%ld/encoder-%ld", (long)getuid(), (long)getpid());
	char mk[700];
	snprintf(mk, sizeof mk, "mkdir -p %s/topics", root);
	if (run(mk) != 0)
		return 2;
	snprintf(topics, sizeof topics, "%s/topics", root);
	setenv("HANGAR_TOPIC_DIR", topics, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	snprintf(enc_wiring, sizeof enc_wiring, "%s/encoder.wiring", root);
	write_text(enc_wiring, "hangar-wiring 1\nmodule encoder\nrole vehicle\npub video.index type=video_index hash=%016llx slots=64\n",
		   (unsigned long long)VIDEO_INDEX_TYPE.hash);
	snprintf(tester_wiring, sizeof tester_wiring, "%s/tester.wiring", root);
	write_text(tester_wiring, "hangar-wiring 1\nmodule tester\nrole vehicle\nsub video.index type=video_index hash=%016llx\n",
		   (unsigned long long)VIDEO_INDEX_TYPE.hash);
	RUN_TEST(test_nals_and_access_units);
	RUN_TEST(test_single_fu_a_marker);
	RUN_TEST(test_sequence_wrap_and_timestamps);
	RUN_TEST(test_parameter_sets_inserted);
	RUN_TEST(test_file_to_udp);
	RUN_TEST(test_conf_refused);
	RUN_TEST(test_camera_unplugged);
	RUN_TEST(test_ffmpeg_decodes);
	if (!getenv("ENCODER_KEEP")) {
		snprintf(mk, sizeof mk, "rm -rf %s", root);
		run(mk);
	}
	TEST_MAIN_END();
}
