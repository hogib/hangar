// SPDX-License-Identifier: GPL-3.0-or-later
// video_relay (modules/video_relay/README.md): the depacketizer on the real
// H.264 of tests/testsrc-320x240-60.h264 (60 frames, a keyframe every 30)
// packetized by the encoder's own packetizer, whole and with a lost packet;
// the SPS's size; the MP4 writer (its boxes, ffprobe where installed, a cut
// file); the module fed that RTP over loopback in lockstep: every frame
// decoded into cam.decoded with the frame's number as its reference, every
// packet relayed byte for byte, a lost packet skipping up to the next
// keyframe, the MP4 written, the CPU at rest, a bad configuration refused.
#define _GNU_SOURCE
#include "test_harness.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "depack.h"
#include "h264.h"
#include "hg_topic.h"
#include "mp4.h"
#include "video_index.h"

#define FRAMES 60
#define PERIOD 3000u /* 90 kHz at 30 frames/s */

static char root[512], topics[600], relay_wiring[600], tester_wiring[600];
static const char *relay_bin, *fixtures;
static uint8_t *stream;
static size_t stream_len;
static struct {
	const uint8_t *p;
	size_t len;
} au[FRAMES];

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&t, &t) < 0 && errno == EINTR)
		;
}

static int run(const char *cmd)
{
	int st = system(cmd);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void write_text(const char *path, const char *fmt, ...)
{
	FILE *f = fopen(path, "w");
	if (!f) {
		perror(path);
		exit(2);
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

static void load_fixture(void)
{
	char path[700];
	snprintf(path, sizeof path, "%s/testsrc-320x240-60.h264", fixtures);
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(2);
	}
	stream = malloc(1 << 20);
	stream_len = fread(stream, 1, 1 << 20, f);
	fclose(f);
	size_t pos = 0;
	int n = 0;
	while (pos < stream_len && n < FRAMES) {
		size_t l = h264_au_len(stream + pos, stream_len - pos);
		if (!l)
			break;
		au[n].p = stream + pos;
		au[n].len = l;
		n++;
		pos += l;
	}
	if (n != FRAMES) {
		printf("    the fixture split into %d access units, not %d\n", n, FRAMES);
		exit(2);
	}
}

/* The NAL units of an access unit, as one comparable string of their bytes. */
static int same_nals(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
	struct h264_nal na[64], nb[64];
	int ka = h264_nals(a, al, na, 64), kb = h264_nals(b, bl, nb, 64);
	if (ka != kb)
		return 0;
	for (int i = 0; i < ka; i++)
		if (na[i].len != nb[i].len || memcmp(na[i].p, nb[i].p, na[i].len))
			return 0;
	return 1;
}

/* ---- Packets made by the encoder's packetizer -------------------------------------- */

#define MAX_PKTS 4096
static struct {
	uint8_t b[1500];
	size_t len;
	int frame; /* the access unit it belongs to */
} pk[MAX_PKTS];
static int npk, cur_frame;

static void collect(void *ctx, const uint8_t *p, size_t len)
{
	(void)ctx;
	if (npk < MAX_PKTS && len <= sizeof pk[0].b) {
		memcpy(pk[npk].b, p, len);
		pk[npk].len = len;
		pk[npk].frame = cur_frame;
		npk++;
	}
}

/* All 60 access units as RTP, frame i with timestamp (first + i) × PERIOD. */
static uint32_t pk_ssrc = 0x1234; /* the encoder draws another at each start */

static void packetize(size_t max_packet, uint64_t first)
{
	static struct rtp_h264 r;
	rtp_h264_init(&r, pk_ssrc, 96, max_packet);
	npk = 0;
	for (int i = 0; i < FRAMES; i++) {
		cur_frame = i;
		rtp_h264_send_au(&r, au[i].p, au[i].len, (uint32_t)((first + (uint64_t)i) * PERIOD), collect, NULL, NULL);
	}
}

/* ---- The depacketizer --------------------------------------------------------------- */

static struct depack D;

static void test_depack_round_trip(void)
{
	size_t sizes[2] = { 1200, 300 }; /* 300: FU-A fragments everywhere */
	for (int s = 0; s < 2; s++) {
		packetize(sizes[s], 1);
		depack_init(&D, 96);
		int got = 0, ok = 1, keys = 0;
		for (int i = 0; i < npk; i++) {
			struct depack_au out[2];
			int k = depack_push(&D, pk[i].b, pk[i].len, out);
			for (int j = 0; j < k; j++) {
				if (got < FRAMES) {
					ok &= same_nals(out[j].data, out[j].len, au[got].p, au[got].len);
					ok &= out[j].ts == (uint32_t)((uint64_t)(got + 1) * PERIOD);
				}
				keys += out[j].keyframe;
				got++;
			}
		}
		printf("    packets of %zu bytes: %d packets, %d frames, %d keyframes\n", sizes[s], npk, got, keys);
		CHECK_EQ_INT(got, FRAMES);
		CHECK(ok);
		CHECK_EQ_INT(keys, 2);
		CHECK_EQ_INT((int)D.lost, 0);
		CHECK_EQ_INT((int)D.skipped, 0);
	}
}

static void test_depack_loss(void)
{
	packetize(300, 1);
	/* The second packet of frame 10 lost: frames 10 to 29 skipped (nothing
	 * decodes from a frame with a hole, nor after it until a keyframe), 30 on
	 * decoded again. */
	int drop = -1;
	for (int i = 0; i < npk; i++)
		if (pk[i].frame == 10 && pk[i + 1].frame == 10) {
			drop = i + 1;
			break;
		}
	CHECK(drop > 0);
	depack_init(&D, 96);
	int got[FRAMES] = { 0 };
	for (int i = 0; i < npk; i++) {
		if (i == drop)
			continue;
		struct depack_au out[2];
		int k = depack_push(&D, pk[i].b, pk[i].len, out);
		for (int j = 0; j < k; j++) {
			int f = (int)(out[j].ts / PERIOD) - 1;
			if (f >= 0 && f < FRAMES)
				got[f] = 1;
		}
	}
	int before = 0, during = 0, after = 0;
	for (int f = 0; f < FRAMES; f++)
		*(f < 10 ? &before : f < 30 ? &during : &after) += got[f];
	printf("    a packet of frame 10 lost: frames 0-9 %d, 10-29 %d, 30-59 %d; %llu lost, %llu skipped\n", before,
	       during, after, (unsigned long long)D.lost, (unsigned long long)D.skipped);
	CHECK_EQ_INT(before, 10);
	CHECK_EQ_INT(during, 0);
	CHECK_EQ_INT(after, 30);
	CHECK_EQ_INT((int)D.lost, 1);
	CHECK_EQ_INT((int)D.skipped, 20);
	/* Bad packets: too short, another payload type, not RTP. */
	uint8_t junk[20] = { 0x80, 97 };
	struct depack_au out[2];
	uint64_t bad = D.bad;
	CHECK_EQ_INT(depack_push(&D, junk, 5, out), 0);
	CHECK_EQ_INT(depack_push(&D, junk, sizeof junk, out), 0);
	junk[0] = 0x40;
	CHECK_EQ_INT(depack_push(&D, junk, sizeof junk, out), 0);
	CHECK_EQ_INT((int)(D.bad - bad), 3);
}

static void test_sps_size(void)
{
	struct h264_nal n[16];
	int k = h264_nals(au[0].p, au[0].len, n, 16);
	int found = 0;
	for (int i = 0; i < k; i++)
		if ((n[i].p[0] & 0x1f) == 7) {
			uint32_t w = 0, h = 0;
			CHECK_EQ_INT(h264_sps_size(n[i].p, n[i].len, &w, &h), 0);
			CHECK(w == 320 && h == 240);
			found = 1;
		}
	CHECK(found);
	uint8_t bad[3] = { 0x67, 0xff, 0xff };
	uint32_t w, h;
	CHECK_EQ_INT(h264_sps_size(bad, sizeof bad, &w, &h), -1);
}

/* ---- The stream's time past the 32 bits (README, deviation 9) --------------------- */

#define WRAP (1ull << 32)
/* The first frame number whose timestamp is past the first wrap, and some:
 * 2^32 / 3000 = 1 431 655.77. */
#define LATE_FRAME 1431700ull

static uint64_t frame_of(uint64_t ts)
{
	return (ts * 30 + 45000) / 90000; /* as video_relay.c takes it, at 30 frames/s */
}

static void test_timestamps(void)
{
	const uint64_t S = 1000000000ull;
	uint64_t now = 100 * S, v;
	int again, ok = 1;

	/* Across the wrap, a frame every 33 ms: counted, the numbers go on. */
	struct ts_unwrap u = { 0 };
	uint64_t t = 1431650ull * PERIOD; /* six frames before it */
	for (uint64_t i = 0; i < 12; i++) {
		v = ts_unwrap(&u, (uint32_t)(t + i * PERIOD), now += S / 30, TS_NO_HINT, &again);
		ok &= v == t + i * PERIOD && !again && frame_of(v) == 1431650 + i;
	}
	CHECK(ok);
	CHECK(v > WRAP);
	/* The encoder restarted without a reboot, 3 s of frames later: its
	 * numbers go on, and so do these. */
	v = ts_unwrap(&u, (uint32_t)(t + 100 * PERIOD), now += 3 * S, TS_NO_HINT, &again);
	CHECK(v == t + 100 * PERIOD && !again);
	/* The vehicle rebooted, 40 s later: a count from zero, and said so.
	 * (The low 32 bits were below 2^31 here: before, the wrap counted
	 * stayed in every number after it.) */
	v = ts_unwrap(&u, 2 * PERIOD, now += 40 * S, TS_NO_HINT, &again);
	CHECK(v == 2 * PERIOD && again && frame_of(v) == 2);
	v = ts_unwrap(&u, 3 * PERIOD, now += S / 30, TS_NO_HINT, &again);
	CHECK(v == 3 * PERIOD && !again);

	/* A reboot while the last timestamp was above 2^31: before, the jump
	 * back was counted as a wrap. A wrap would be 2^30 ticks (3 h 19 min of
	 * frames) in 40 s: a count from zero. */
	struct ts_unwrap r = { 0 };
	ts_unwrap(&r, 0xC0000000u, now, TS_NO_HINT, &again);
	v = ts_unwrap(&r, 5 * PERIOD, now + 40 * S, TS_NO_HINT, &again);
	CHECK(v == 5 * PERIOD && again);
	/* What is left of it: a reboot within TS_SLACK and twice its own time
	 * of the wrap cannot be told from the wrap without the vehicle's numbers. */
	struct ts_unwrap r2 = { 0 };
	ts_unwrap(&r2, (uint32_t)(WRAP - 30 * 90000u), now, TS_NO_HINT, &again);
	v = ts_unwrap(&r2, 5 * PERIOD, now + 40 * S, TS_NO_HINT, &again);
	CHECK(v == WRAP + 5 * PERIOD && !again);
	/* An hour of silence, the count an hour on: the same count. */
	struct ts_unwrap q = { 0 };
	ts_unwrap(&q, 1000 * PERIOD, now, TS_NO_HINT, &again);
	v = ts_unwrap(&q, 1000 * PERIOD + 3600u * 90000u, now + 3600 * S, TS_NO_HINT, &again);
	CHECK(v == 1000ull * PERIOD + 3600ull * 90000 && !again);

	/* A late start: the stream first seen after its first wrap. Without
	 * the vehicle's numbers the 32 bits are all there is: low by 2^32, the
	 * frame number by 1 431 655.77 (the limit the README states). */
	uint64_t full = LATE_FRAME * PERIOD;
	struct ts_unwrap l = { 0 };
	v = ts_unwrap(&l, (uint32_t)full, now, TS_NO_HINT, &again);
	CHECK(v == full - WRAP && !again);
	CHECK(frame_of(v) == 44);
	/* The vehicle's number for the frame before, from video.index: right
	 * from then on, and said to be another count once. */
	v = ts_unwrap(&l, (uint32_t)(full + PERIOD), now + S / 30, full, &again);
	CHECK(v == full + PERIOD && again && frame_of(v) == LATE_FRAME + 1);
	v = ts_unwrap(&l, (uint32_t)(full + 2 * PERIOD), now + 2 * S / 30, full + PERIOD, &again);
	CHECK(v == full + 2 * PERIOD && !again);
	/* With it there at the first frame: right at once; the message may be
	 * of a frame after this one, */
	struct ts_unwrap h = { 0 };
	v = ts_unwrap(&h, (uint32_t)full, now, full + 3 * PERIOD, &again);
	CHECK(v == full && !again && frame_of(v) == LATE_FRAME);
	/* or of one on the other side of a wrap. */
	struct ts_unwrap w = { 0 };
	v = ts_unwrap(&w, (uint32_t)(2 * WRAP + PERIOD), now, 2 * WRAP - PERIOD, &again);
	CHECK(v == 2 * WRAP + PERIOD && !again);
	v = ts_unwrap(&w, (uint32_t)(2 * WRAP - 2 * PERIOD), now, 2 * WRAP + PERIOD, &again);
	CHECK(v == 2 * WRAP - 2 * PERIOD);
	/* A reboot, with the new count's message there: its numbers, said. */
	v = ts_unwrap(&h, 3 * PERIOD, now + 40 * S, 2 * PERIOD, &again);
	CHECK(v == 3 * PERIOD && again && frame_of(v) == 3);
}

/* ---- The MP4 writer ----------------------------------------------------------------- */

static int count_boxes(const char *path, const char *type, int *ftyp_first)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	int n = 0, first = 1;
	*ftyp_first = 0;
	for (;;) {
		uint8_t h[8];
		if (fread(h, 1, 8, f) != 8)
			break;
		uint32_t size = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
		if (first)
			*ftyp_first = memcmp(h + 4, "ftyp", 4) == 0;
		first = 0;
		if (!memcmp(h + 4, type, 4))
			n++;
		if (size < 8 || fseek(f, (long)size - 8, SEEK_CUR) != 0)
			break;
	}
	fclose(f);
	return n;
}

static int have_ffprobe(void)
{
	return run("command -v ffprobe >/dev/null 2>&1") == 0;
}

static long ffprobe_frames(const char *path)
{
	char cmd[1200], out[700];
	snprintf(out, sizeof out, "%s.probe", path);
	snprintf(cmd, sizeof cmd,
		 "ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 %s > %s 2>/dev/null",
		 path, out);
	if (run(cmd) != 0)
		return -1;
	FILE *f = fopen(out, "r");
	long n = -1;
	if (f) {
		if (fscanf(f, "%ld", &n) != 1)
			n = -1;
		fclose(f);
	}
	return n;
}

static void test_mp4(void)
{
	char path[700];
	snprintf(path, sizeof path, "%s/direct.mp4", root);
	struct mp4w w;
	CHECK_EQ_INT(mp4_open(&w, path), 0);
	/* A P frame first: not written (no header yet). */
	CHECK_EQ_INT(mp4_write(&w, au[1].p, au[1].len, 1000, PERIOD, 0), 0);
	int written = 0;
	for (int i = 0; i < FRAMES; i++)
		written += mp4_write(&w, au[i].p, au[i].len, (uint64_t)(i + 1) * PERIOD, PERIOD, i % 30 == 0) == 1;
	CHECK_EQ_INT(written, FRAMES);
	CHECK(w.width == 320 && w.height == 240);
	mp4_close(&w);
	int ftyp_first;
	CHECK_EQ_INT(count_boxes(path, "moof", &ftyp_first), FRAMES);
	CHECK(ftyp_first);
	CHECK_EQ_INT(count_boxes(path, "moov", &ftyp_first), 1);
	if (have_ffprobe()) {
		long n = ffprobe_frames(path);
		printf("    ffprobe: %ld frames in the MP4\n", n);
		CHECK_EQ_INT((int)n, FRAMES);
		/* Cut in the middle of a fragment, as by a power loss: what came
		 * before still plays. */
		char cut[700], cmd[1500];
		snprintf(cut, sizeof cut, "%s/cut.mp4", root);
		struct stat st;
		CHECK_EQ_INT(stat(path, &st), 0);
		snprintf(cmd, sizeof cmd, "head -c %lld %s > %s", (long long)st.st_size * 2 / 3, path, cut);
		CHECK_EQ_INT(run(cmd), 0);
		long m = ffprobe_frames(cut);
		printf("    cut at two thirds: %ld frames play\n", m);
		CHECK(m >= 30 && m < FRAMES);
	} else {
		printf("    (ffprobe skipped: not installed; the boxes were counted)\n");
	}
}

/* A recording that is there is never replaced. mp4_open truncated its path,
 * and the path of a session's recording is the same for the whole session:
 * a relay restarted during a session, or a second `active` message for it,
 * emptied everything the session had recorded. */
static void test_mp4_keeps_what_is_there(void)
{
	char dir[700], path[800];
	snprintf(dir, sizeof dir, "%s/keep", root);
	CHECK_EQ_INT(mkdir(dir, 0755), 0);
	snprintf(path, sizeof path, "%s/relay-s1.mp4", dir);
	struct mp4w w;
	CHECK_EQ_INT(mp4_open(&w, path), 0);
	for (int i = 0; i < 30; i++)
		CHECK_EQ_INT(mp4_write(&w, au[i].p, au[i].len, (uint64_t)(i + 1) * PERIOD, PERIOD, i % 30 == 0), 1);
	mp4_close(&w);
	struct stat a, b;
	CHECK(stat(path, &a) == 0 && a.st_size > 1000);
	/* The same path again, as after a restart: refused, the file untouched. */
	errno = 0;
	int rc = mp4_open(&w, path);
	CHECK(rc < 0 && errno == EEXIST);
	if (rc == 0)
		mp4_close(&w);
	CHECK(stat(path, &b) == 0);
	if (b.st_size != a.st_size)
		printf("    %s: %lld bytes before the second open, %lld after\n", path, (long long)a.st_size, (long long)b.st_size);
	CHECK_EQ_INT((long)b.st_size, (long)a.st_size);
	/* What the module does instead: the next part beside it, -2, then -3. */
	char got[800], want[800];
	for (int part = 2; part <= 3; part++) {
		CHECK_EQ_INT(mp4_open_new(&w, dir, "relay-s1", got, sizeof got), 0);
		snprintf(want, sizeof want, "%s/relay-s1-%d.mp4", dir, part);
		CHECK(strcmp(got, want) == 0);
		for (int i = 0; i < 30; i++)
			CHECK_EQ_INT(mp4_write(&w, au[i].p, au[i].len, (uint64_t)(i + 1) * PERIOD, PERIOD, i % 30 == 0), 1);
		mp4_close(&w);
		int ftyp_first;
		CHECK_EQ_INT(count_boxes(got, "moof", &ftyp_first), 30);
		CHECK(ftyp_first);
	}
	CHECK(stat(path, &b) == 0);
	CHECK_EQ_INT((long)b.st_size, (long)a.st_size);
	int ftyp_first;
	CHECK_EQ_INT(count_boxes(path, "moof", &ftyp_first), 30);
	/* A name with nothing there is taken as it is. */
	CHECK_EQ_INT(mp4_open_new(&w, dir, "relay-s2", got, sizeof got), 0);
	snprintf(want, sizeof want, "%s/relay-s2.mp4", dir);
	CHECK(strcmp(got, want) == 0);
	mp4_close(&w);
	/* A directory that is not there is an error, not a loop over names. */
	errno = 0;
	CHECK(mp4_open_new(&w, "/nonexistent-hangar-dir", "x", got, sizeof got) < 0 && errno == ENOENT);
}

/* ---- The module --------------------------------------------------------------------- */

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

/* Wait until the relay has logged that it listens (its last line before hg_ready): a
 * fixed sleep lost frame 1, the key frame, on a slow runner, and with it 30 frames. */
static int wait_listening(const char *log, int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		char buf[4096];
		FILE *f = fopen(log, "r");
		size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
		if (f)
			fclose(f);
		buf[n] = 0;
		if (strstr(buf, "RTP on "))
			return 1;
		sleep_ms(10);
	}
	return 0;
}

static pid_t spawn_relay(const char *conf, const char *log)
{
	pid_t pid = fork();
	if (pid == 0) {
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		setenv("HANGAR_WIRING_FILE", relay_wiring, 1);
		setenv("HANGAR_MODULE", "video_relay", 1);
		execl(relay_bin, relay_bin, conf, (char *)NULL);
		_exit(127);
	}
	return pid;
}

static int stop(pid_t pid)
{
	kill(pid, SIGTERM);
	uint64_t t0 = now_ns();
	for (;;) {
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		if (now_ns() - t0 > 5000000000ull) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			return -1;
		}
		sleep_ms(10);
	}
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* Sends frame f's packets (pk[] with .frame == f) except `drop`, reads what
 * the relay forwarded and compares it byte for byte. Returns the time the
 * frame's last packet went out. */
static uint64_t send_frame(int out, struct sockaddr_in *to, int in, int f, int drop, int *relay_ok)
{
	uint64_t t = 0;
	int sent = 0;
	uint8_t buf[1600];
	for (int i = 0; i < npk; i++) {
		if (pk[i].frame != f || i == drop)
			continue;
		sendto(out, pk[i].b, pk[i].len, 0, (struct sockaddr *)to, sizeof *to);
		t = now_ns();
		sent++;
		/* The relayed copy, unchanged. */
		struct timeval tv = { 1, 0 };
		setsockopt(in, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		ssize_t n = recv(in, buf, sizeof buf, 0);
		if (n != (ssize_t)pk[i].len || memcmp(buf, pk[i].b, pk[i].len))
			*relay_ok = 0;
	}
	(void)sent;
	return t;
}

static void test_module(void)
{
	int qport, rport;
	int q = udp_bound(&qport); /* the relay target: QGroundControl's place */
	int tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int tmp = udp_bound(&rport);
	close(tmp);
	CHECK(q >= 0 && tx >= 0);
	char conf[700], log[700], rec[700];
	snprintf(rec, sizeof rec, "%s/rec", root);
	snprintf(conf, sizeof conf, "%s/video_relay.conf", root);
	snprintf(log, sizeof log, "%s/video_relay.log", root);
	write_text(conf,
		   "[VideoRelay]\nBind=127.0.0.1:%d\nRelay=127.0.0.1:%d\nFrameRate=30\nFrames=vehicle:video.index\n"
		   "Decode=yes\nThreads=2\nSession=\nRecordDir=%s\nName=test\n",
		   rport, qport, rec);
	pid_t pid = spawn_relay(conf, log);
	CHECK(wait_listening(log, 10000));

	setenv("HANGAR_WIRING_FILE", tester_wiring, 1);
	setenv("HANGAR_MODULE", "tester", 1);
	CHECK_EQ_INT(hg_init(), 0);
	hg_sub *dec = hg_sub_open("cam.decoded", NULL);
	CHECK(dec != NULL);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)rport),
				  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	static uint8_t frame[320 * 240 * 2 + 64];
	struct hg_info info;
	uint32_t vi_id = hg_topic_id("video.index");
	uint64_t lat[2 * FRAMES];
	int nlat = 0, relay_ok = 1, refs_ok = 1, decoded = 0;

	/* Pass 1: frames 1 to 60, all whole, in lockstep. */
	packetize(1200, 1);
	for (int f = 0; f < FRAMES; f++) {
		uint64_t t = send_frame(tx, &to, q, f, -1, &relay_ok);
		hg_sub *w[1] = { dec };
		int got = 0;
		uint64_t t0 = now_ns();
		while (!got && now_ns() - t0 < 2000000000ull) {
			hg_wait(w, 1, 200);
			while (hg_next(dec, frame, sizeof frame, &info) == HG_MSG) {
				got = 1;
				decoded++;
				refs_ok &= info.n_inputs == 1 && info.inputs[0].topic_id == vi_id &&
					   info.inputs[0].seq == (uint64_t)(f + 1);
				if (nlat < 2 * FRAMES)
					lat[nlat++] = info.publish_ns - t;
			}
		}
		if (!got)
			printf("    frame %d: no cam.decoded within 2 s\n", f + 1);
	}
	CHECK_EQ_INT(decoded, FRAMES);
	CHECK(relay_ok);
	CHECK(refs_ok);
	/* The frame itself: I422, 320×240, full range (the test pattern's
	 * white is 255, not 235). */
	uint32_t hdr[9];
	memcpy(hdr, frame, sizeof hdr);
	CHECK(hdr[0] == 320 && hdr[1] == 240 && hdr[2] == 0x32323449u /* I422 */);
	int maxy = 0;
	for (int i = 0; i < 320 * 240; i++)
		if (frame[36 + i] > maxy)
			maxy = frame[36 + i];
	CHECK(maxy >= 250);
	qsort(lat, (size_t)nlat, sizeof lat[0], cmp_u64);
	if (nlat)
		printf("    %d frames decoded; last packet to cam.decoded published: median %.2f ms, worst %.2f ms\n", nlat,
		       (double)lat[nlat / 2] / 1e6, (double)lat[nlat - 1] / 1e6);

	/* Pass 2: frames 61 to 120 from a new packetizer (sequence numbers
	 * starting over, as when the encoder restarts: a new stream, decoded from
	 * its first keyframe), a packet of frame 70 lost: 70 to 90 never decoded,
	 * 91 (the pass's second keyframe) on again. */
	packetize(300, 61);
	int drop = -1;
	for (int i = 0; i < npk; i++)
		if (pk[i].frame == 9 && pk[i + 1].frame == 9) {
			drop = i + 1;
			break;
		}
	int seen[FRAMES] = { 0 };
	for (int f = 0; f < FRAMES; f++) {
		send_frame(tx, &to, q, f, drop, &relay_ok);
		hg_sub *w[1] = { dec };
		uint64_t t0 = now_ns();
		long wait_ns = (f >= 9 && f < 30) ? 60000000L : 2000000000L;
		int got = 0;
		while (!got && now_ns() - t0 < (uint64_t)wait_ns) {
			hg_wait(w, 1, 20);
			while (hg_next(dec, frame, sizeof frame, &info) == HG_MSG) {
				int n = (int)info.inputs[0].seq - 61;
				if (n >= 0 && n < FRAMES)
					seen[n] = 1;
				got = 1;
			}
		}
	}
	int before = 0, during = 0, after = 0;
	for (int f = 0; f < FRAMES; f++)
		*(f < 9 ? &before : f < 30 ? &during : &after) += seen[f];
	printf("    a new stream, a packet of frame 70 lost: frames 61-69 %d, 70-90 %d, 91-120 %d decoded\n", before,
	       during, after);
	CHECK_EQ_INT(before, 9);
	CHECK_EQ_INT(during, 0);
	CHECK_EQ_INT(after, 30);

	/* At rest: nothing arriving, nothing read. */
	long long c0 = th_cpu_ticks(pid);
	uint64_t t0 = now_ns();
	sleep_ms(1000);
	double pct = th_cpu_percent(c0, th_cpu_ticks(pid), (long)((now_ns() - t0) / 1000000));
	printf("    at rest: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 5);

	CHECK_EQ_INT(stop(pid), 0);
	/* The MP4: both passes but the 21 frames skipped. */
	char mp4[800], cmd[1600];
	snprintf(cmd, sizeof cmd, "ls %s/test-*.mp4 > %s/mp4.list 2>/dev/null", rec, root);
	CHECK_EQ_INT(run(cmd), 0);
	snprintf(cmd, sizeof cmd, "%s/mp4.list", root);
	FILE *l = fopen(cmd, "r");
	CHECK(l && fgets(mp4, sizeof mp4, l));
	if (l)
		fclose(l);
	mp4[strcspn(mp4, "\n")] = 0;
	int ftyp_first;
	int moofs = count_boxes(mp4, "moof", &ftyp_first);
	printf("    MP4 %s: %d fragments\n", mp4, moofs);
	CHECK(ftyp_first);
	CHECK_EQ_INT(moofs, 2 * FRAMES - 21);
	struct stat st;
	CHECK(stat(mp4, &st) == 0 && (st.st_mode & 0777) == 0640);
	if (have_ffprobe()) {
		long n = ffprobe_frames(mp4);
		printf("    ffprobe: %ld frames\n", n);
		CHECK_EQ_INT((int)n, 2 * FRAMES - 21);
	}
	hg_sub_close(dec);
	close(q);
	close(tx);
}

/* 1 when the file holds the text. */
static int log_has(const char *log, const char *needle)
{
	static char buf[65536];
	FILE *f = fopen(log, "r");
	size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = 0;
	return strstr(buf, needle) != NULL;
}

/* The MP4 files in dir; the first one's name in `first` when not NULL. */
static int mp4_files(const char *dir, char *first, size_t len)
{
	char pat[800];
	snprintf(pat, sizeof pat, "%s/*.mp4", dir);
	glob_t g;
	int n = glob(pat, 0, NULL, &g) == 0 ? (int)g.gl_pathc : 0;
	if (n && first)
		snprintf(first, len, "%s", g.gl_pathv[0]);
	globfree(&g);
	return n;
}

/* One frame sent, and the frame number cam.decoded names for it; 0 when none
 * came within 2 s. */
static uint64_t frame_number_of(int tx, struct sockaddr_in *to, int q, hg_sub *dec, int f)
{
	static uint8_t frame[320 * 240 * 2 + 64];
	struct hg_info info;
	int relay_ok = 1;
	uint64_t number = 0;
	send_frame(tx, to, q, f, -1, &relay_ok);
	hg_sub *w[1] = { dec };
	for (uint64_t t0 = now_ns(); !number && now_ns() - t0 < 2000000000ull;) {
		hg_wait(w, 1, 200);
		while (hg_next(dec, frame, sizeof frame, &info) == HG_MSG)
			number = info.n_inputs == 1 ? info.inputs[0].seq : 0;
	}
	return number;
}

/* video.index as the bridge mirrors it: one message, the vehicle's seq. */
static int mirror_index(hg_pub *mir, uint64_t seq)
{
	struct video_index vi;
	memset(&vi, 0, sizeof vi);
	vi.rtp_timestamp = (uint32_t)(seq * PERIOD);
	vi.frame_seq = seq;
	return hg_publish_seq(mir, seq, 0, &vi, sizeof vi, now_ns(), NULL, 0);
}

/* A relay started when the vehicle has sent more than 13.25 h of frames, and
 * the vehicle rebooting under it: the numbers cam.decoded names are the
 * vehicle's, from the mirrored video.index. Before, the first were low by
 * 1 431 655.77 frames, and the second high by as much whenever the last
 * timestamp before the reboot was above 2^31. Each count is a file. */
static void test_late_start_and_reboot(void)
{
	int qport, rport;
	int q = udp_bound(&qport);
	int tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int tmp = udp_bound(&rport);
	close(tmp);
	CHECK(q >= 0 && tx >= 0);
	char conf[700], log[700], rec[700];
	snprintf(rec, sizeof rec, "%s/rec-late", root);
	snprintf(conf, sizeof conf, "%s/video_relay_late.conf", root);
	snprintf(log, sizeof log, "%s/video_relay_late.log", root);
	write_text(conf,
		   "[VideoRelay]\nBind=127.0.0.1:%d\nRelay=127.0.0.1:%d\nFrameRate=30\nFrames=vehicle:video.index\n"
		   "Decode=yes\nThreads=2\nSession=\nRecordDir=%s\nName=late\n",
		   rport, qport, rec);
	/* The vehicle's index, mirrored before the relay starts: its newest
	 * message is the frame before the first one sent, and then each frame's
	 * is there before the frame (test_timestamps has it behind). */
	unsigned char boot_a[16] = { 1 }, boot_b[16] = { 2 };
	hg_pub *mir = hg_pub_open_mirror("video.index", &VIDEO_INDEX_TYPE, boot_a, LATE_FRAME - 1, NULL);
	CHECK(mir != NULL);
	if (!mir)
		return;
	CHECK_EQ_INT(mirror_index(mir, LATE_FRAME - 1), 0);
	pid_t pid = spawn_relay(conf, log);
	CHECK(wait_listening(log, 10000));
	hg_sub *dec = hg_sub_open("cam.decoded", NULL);
	CHECK(dec != NULL);
	if (!dec)
		return;
	/* What the relay of the test before left on the topic is not this one's. */
	static uint8_t old[320 * 240 * 2 + 64];
	struct hg_info old_info;
	while (hg_next(dec, old, sizeof old, &old_info) == HG_MSG)
		;
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)rport),
				  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	packetize(1200, LATE_FRAME);
	int ok = 1;
	uint64_t first = 0;
	for (int f = 0; f < 10; f++) {
		mirror_index(mir, LATE_FRAME + (uint64_t)f);
		uint64_t n = frame_number_of(tx, &to, q, dec, f);
		if (!f)
			first = n;
		ok &= n == LATE_FRAME + (uint64_t)f;
	}
	printf("    started after the wrap: frame %llu is named %llu\n", (unsigned long long)LATE_FRAME, (unsigned long long)first);
	CHECK(ok);
	/* The vehicle reboots: its index starts again, under another boot id,
	 * and so does the stream (a new packetizer, frames from 2). The old
	 * count's index is not believed across the new stream, whether or not
	 * the new one's is read yet (a real reboot also leaves it more than
	 * HINT_FRESH_NS old). */
	hg_pub_close(mir);
	mir = hg_pub_open_mirror("video.index", &VIDEO_INDEX_TYPE, boot_b, 1, NULL);
	CHECK(mir != NULL);
	if (mir)
		CHECK_EQ_INT(mirror_index(mir, 1), 0);
	pk_ssrc = 0x5678;
	packetize(1200, 2);
	pk_ssrc = 0x1234;
	ok = 1;
	for (int f = 0; f < 10; f++) {
		uint64_t n = frame_number_of(tx, &to, q, dec, f);
		if (!f)
			first = n;
		ok &= n == 2 + (uint64_t)f;
	}
	printf("    after a reboot of the vehicle: frame 2 is named %llu\n", (unsigned long long)first);
	CHECK(ok);
	CHECK_EQ_INT(stop(pid), 0);
	CHECK(log_has(log, "the frame numbers start again at 2"));
	if (!ok || !log_has(log, "the frame numbers start again at 2")) {
		char cmd[800];
		snprintf(cmd, sizeof cmd, "cat %s", log);
		fflush(stdout);
		run(cmd);
	}
	/* Two counts, two files: a file's times run from its first frame. */
	CHECK_EQ_INT(mp4_files(rec, NULL, 0), 2);
	if (mir)
		hg_pub_close(mir);
	hg_sub_close(dec);
	close(q);
	close(tx);
}

/* With RecordDir= there is no session message to start a recording again:
 * one failure ended it for the life of the module, and nothing looked at the
 * free space. Now a recording that is wanted and not open is tried again at
 * a keyframe (after 1 s, doubling to 60 s), and nothing is recorded under
 * MinFree. */
static void test_recording_goes_on(void)
{
	int qport, rport;
	int q = udp_bound(&qport);
	int tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int tmp = udp_bound(&rport);
	close(tmp);
	CHECK(q >= 0 && tx >= 0);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)rport),
				  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	char conf[700], log[700], rec[700];
	int relay_ok = 1;
	snprintf(conf, sizeof conf, "%s/video_relay_rec.conf", root);

	/* More free space wanted than any disk has: nothing is created, and the
	 * log says why. The relay goes on relaying. */
	snprintf(rec, sizeof rec, "%s/rec-full", root);
	snprintf(log, sizeof log, "%s/video_relay_full.log", root);
	write_text(conf,
		   "[VideoRelay]\nBind=127.0.0.1:%d\nRelay=127.0.0.1:%d\nFrameRate=30\nDecode=no\nSession=\n"
		   "RecordDir=%s\nName=test\nMinFree=8000000000G\n",
		   rport, qport, rec);
	pid_t pid = spawn_relay(conf, log);
	CHECK(wait_listening(log, 10000));
	packetize(1200, 1);
	for (int f = 0; f < FRAMES; f++)
		send_frame(tx, &to, q, f, -1, &relay_ok);
	CHECK_EQ_INT(stop(pid), 0);
	CHECK(relay_ok);
	CHECK(log_has(log, "under MinFree"));
	CHECK_EQ_INT(mp4_files(rec, NULL, 0), 0);

	/* A directory that cannot be written, and then can: the recording
	 * starts at a later keyframe, in the same process. */
	if (geteuid() == 0) {
		printf("    (the unwritable directory skipped: root writes anywhere)\n");
	} else {
		snprintf(rec, sizeof rec, "%s/rec-ro", root);
		snprintf(log, sizeof log, "%s/video_relay_ro.log", root);
		CHECK_EQ_INT(mkdir(rec, 0555), 0);
		write_text(conf,
			   "[VideoRelay]\nBind=127.0.0.1:%d\nRelay=127.0.0.1:%d\nFrameRate=30\nDecode=no\nSession=\n"
			   "RecordDir=%s\nName=test\n",
			   rport, qport, rec);
		pid = spawn_relay(conf, log);
		CHECK(wait_listening(log, 10000));
		uint64_t first = 1;
		packetize(1200, first);
		for (int f = 0; f < FRAMES; f++)
			send_frame(tx, &to, q, f, -1, &relay_ok);
		CHECK(log_has(log, "not recording into"));
		CHECK(log_has(log, strerror(EACCES)));
		CHECK(!log_has(log, ": recording into"));
		CHECK_EQ_INT(mp4_files(rec, NULL, 0), 0);
		CHECK_EQ_INT(chmod(rec, 0755), 0);
		/* The next attempt is at a keyframe no sooner than the wait (1 s
		 * after the first failure, doubling): frames go on until it has
		 * come, each pass of 60 beginning with a keyframe. */
		int passes = 0;
		while (mp4_files(rec, NULL, 0) == 0 && passes < 200) {
			sleep_ms(300);
			first += FRAMES;
			packetize(1200, first);
			for (int f = 0; f < FRAMES; f++)
				send_frame(tx, &to, q, f, -1, &relay_ok);
			passes++;
		}
		printf("    recording again after %d more passes of %d frames\n", passes, FRAMES);
		CHECK_EQ_INT(stop(pid), 0);
		CHECK(relay_ok);
		char mp4[800] = "";
		CHECK_EQ_INT(mp4_files(rec, mp4, sizeof mp4), 1);
		int ftyp_first = 0;
		int moofs = mp4[0] ? count_boxes(mp4, "moof", &ftyp_first) : 0;
		printf("    %s: %d fragments\n", mp4, moofs);
		CHECK(ftyp_first);
		CHECK(moofs == 30 || moofs == 60); /* from the keyframe at which it was opened */
	}
	close(q);
	close(tx);
}

static void test_conf_refused(void)
{
	char conf[700], log[700];
	snprintf(conf, sizeof conf, "%s/bad.conf", root);
	snprintf(log, sizeof log, "%s/bad.log", root);
	write_text(conf, "[VideoRelay]\nBind=127.0.0.1:0\nFrames=cam.pilot\n");
	pid_t pid = spawn_relay(conf, log);
	int st;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 78);
}

int main(void)
{
	relay_bin = getenv("VIDEO_RELAY_BIN");
	fixtures = getenv("VIDEO_RELAY_FIXTURES");
	if (!relay_bin || !fixtures) {
		fprintf(stderr, "VIDEO_RELAY_BIN and VIDEO_RELAY_FIXTURES must be set\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(root, sizeof root, "/dev/shm/hangar-test-%ld/video_relay-%ld", (long)getuid(), (long)getpid());
	char mk[700];
	snprintf(mk, sizeof mk, "mkdir -p %s/topics", root);
	if (run(mk) != 0)
		return 2;
	snprintf(topics, sizeof topics, "%s/topics", root);
	setenv("HANGAR_TOPIC_DIR", topics, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	snprintf(relay_wiring, sizeof relay_wiring, "%s/video_relay.wiring", root);
	write_text(relay_wiring,
		   "hangar-wiring 1\nmodule video_relay\nrole topside\npub cam.decoded type=frame slots=4 inputs=vehicle:video.index\n"
		   "sub video.index type=video_index\n");
	snprintf(tester_wiring, sizeof tester_wiring, "%s/tester.wiring", root);
	write_text(tester_wiring, "hangar-wiring 1\nmodule tester\nrole topside\nsub cam.decoded type=frame\n"
					 "mirror video.index\n");
	load_fixture();
	RUN_TEST(test_depack_round_trip);
	RUN_TEST(test_depack_loss);
	RUN_TEST(test_sps_size);
	RUN_TEST(test_timestamps);
	RUN_TEST(test_mp4);
	RUN_TEST(test_mp4_keeps_what_is_there);
	RUN_TEST(test_module);
	RUN_TEST(test_late_start_and_reboot);
	RUN_TEST(test_recording_goes_on);
	RUN_TEST(test_conf_refused);
	if (!getenv("VIDEO_RELAY_KEEP")) {
		snprintf(mk, sizeof mk, "rm -rf %s", root);
		run(mk);
	}
	TEST_MAIN_END();
}
