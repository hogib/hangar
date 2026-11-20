// SPDX-License-Identifier: GPL-3.0-or-later
/* encoder: the vehicle's video to the topside as H.264 over RTP
 * (docs/11-electric-eye.md "encoder"). This stage has the camera's own H.264
 * as its source (Source=uvc:/dev/videoN): nothing is encoded on the vehicle,
 * the access units the camera makes are packetized (h264.c, RFC 6184) and
 * sent to the topside's video port; and an Annex B file (Source=file:), paced
 * at the frame rate, for the tests and the bench. Each frame sent is one
 * video.index message. modules/encoder/README.md has the rest.
 *
 *   hangar-encoder /etc/hangar/encoder.conf
 */
#include "mod.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include "h264.h"
#include "hg_call.h"
#include "hg_ini.h"
#include "video_index.h"

#define WHO "hangar-encoder"
#define UVC_BUFFERS 4
#define FILE_MAX (64u << 20)

static struct {
	char source[256];      /* "uvc:/dev/video2" or "file:/path/x.h264" */
	char peer[128];        /* host:port */
	unsigned width, height, fps;
	size_t max_packet;
	uint8_t pt;
	uint64_t keyframe_ns;  /* ask the camera for a keyframe when none came for this long; 0 never */
	int loop;              /* file: start again at its end */
} C = { .width = 1280, .height = 720, .fps = 30, .max_packet = 1200, .pt = 96, .keyframe_ns = 1000000000ull };

/* ---- encoder.conf ---------------------------------------------------------- */

struct conf_state {
	int in_encoder;
};

static int on_section(void *ctx, const char *section, const char *arg, int line, char *err, size_t errlen)
{
	(void)line;
	struct conf_state *st = ctx;
	st->in_encoder = strcmp(section, "Encoder") == 0 && !*arg;
	if (!st->in_encoder) {
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

static int on_key(void *ctx, const char *key, const char *value, int line, char *err, size_t errlen)
{
	(void)ctx;
	(void)line;
	unsigned u;
	if (strcmp(key, "Source") == 0) {
		if ((strncmp(value, "uvc:", 4) && strncmp(value, "file:", 5)) || strlen(value) >= sizeof C.source) {
			snprintf(err, errlen, "Source: uvc:/dev/videoN or file:<path>");
			return -1;
		}
		snprintf(C.source, sizeof C.source, "%s", value);
	} else if (strcmp(key, "Peer") == 0) {
		if (!strchr(value, ':') || strlen(value) >= sizeof C.peer) {
			snprintf(err, errlen, "Peer: host:port");
			return -1;
		}
		snprintf(C.peer, sizeof C.peer, "%s", value);
	} else if (strcmp(key, "Width") == 0 || strcmp(key, "Height") == 0) {
		if (parse_uint(value, 16, 8192, &u) < 0) {
			snprintf(err, errlen, "%s: 16 to 8192", key);
			return -1;
		}
		*(key[0] == 'W' ? &C.width : &C.height) = u;
	} else if (strcmp(key, "FrameRate") == 0) {
		if (parse_uint(value, 1, 240, &u) < 0) {
			snprintf(err, errlen, "FrameRate: 1 to 240");
			return -1;
		}
		C.fps = u;
	} else if (strcmp(key, "MaxPacket") == 0) {
		if (parse_uint(value, 64, RTP_MAX_PACKET, &u) < 0) {
			snprintf(err, errlen, "MaxPacket: 64 to %d bytes, the RTP header included", RTP_MAX_PACKET);
			return -1;
		}
		C.max_packet = u;
	} else if (strcmp(key, "PayloadType") == 0) {
		if (parse_uint(value, 96, 127, &u) < 0) {
			snprintf(err, errlen, "PayloadType: 96 to 127");
			return -1;
		}
		C.pt = (uint8_t)u;
	} else if (strcmp(key, "KeyframeInterval") == 0) {
		int64_t d = hg_ini_parse_duration(value);
		if (d < 0) {
			snprintf(err, errlen, "KeyframeInterval: a duration (1s, 500ms), 0 for never");
			return -1;
		}
		C.keyframe_ns = (uint64_t)d;
	} else if (strcmp(key, "Loop") == 0) {
		int b = hg_ini_parse_bool(value);
		if (b < 0) {
			snprintf(err, errlen, "Loop: yes or no");
			return -1;
		}
		C.loop = b;
	} else {
		snprintf(err, errlen, "unknown key %s", key);
		return -1;
	}
	return 0;
}

static int load_conf(const char *path)
{
	static char text[16384];
	size_t len;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		mod_log(WHO, "%s: %s", path, strerror(errno));
		return -1;
	}
	struct conf_state st = { 0 };
	char why[256];
	if (hg_ini_parse(text, len, on_section, on_key, &st, why, sizeof why) < 0) {
		mod_log(WHO, "%s: %s", path, why);
		hg_status(why);
		return -1;
	}
	if (!C.source[0] || !C.peer[0]) {
		mod_log(WHO, "%s: Source= and Peer= are required in [Encoder]", path);
		hg_status("Source= and Peer= are required");
		return -1;
	}
	return 0;
}

/* ---- Sending --------------------------------------------------------------- */

static int sock = -1;
static struct rtp_h264 R;
static hg_pub *pub_index;
static uint64_t frames, keyframes, send_errors;
static uint64_t last_key_ns;

static void send_packet(void *ctx, const uint8_t *pkt, size_t len)
{
	(void)ctx;
	/* A topside not listening yet answers with ICMP, which a connected
	 * socket reports on a later send: counted, never fatal. */
	if (send(sock, pkt, len, MSG_DONTWAIT) < 0)
		send_errors++;
}

static int open_peer(void)
{
	char host[128];
	snprintf(host, sizeof host, "%s", C.peer);
	char *colon = strrchr(host, ':');
	*colon = 0;
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM }, *ai;
	int rc = getaddrinfo(host, colon + 1, &hints, &ai);
	if (rc) {
		mod_log(WHO, "Peer %s: %s", C.peer, gai_strerror(rc));
		return -1;
	}
	sock = socket(ai->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0 || connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
		mod_log(WHO, "Peer %s: %s", C.peer, strerror(errno));
		freeaddrinfo(ai);
		return -1;
	}
	freeaddrinfo(ai);
	return 0;
}

/* One access unit: packetized, sent, and one video.index message. The RTP
 * timestamp is the frame's number times the nominal period, so the frame's
 * identity crosses the tether (docs/11). The number is the seq of the
 * video.index message that names the frame, not the camera's own count
 * (frame_seq), which skips when the camera drops a frame: the topside's
 * video_relay recovers it from the timestamp and references
 * (vehicle:video.index, seq) exactly. */
static void send_frame(const uint8_t *au, size_t len, uint64_t frame_seq, uint64_t capture_ns)
{
	uint32_t ts = (uint32_t)(hg_pub_next_seq(pub_index) * 90000u / C.fps);
	uint32_t bytes = 0;
	int f = rtp_h264_send_au(&R, au, len, ts, send_packet, NULL, &bytes);
	if (f < 0)
		return;
	frames++;
	if (f & RTP_AU_KEYFRAME) {
		keyframes++;
		last_key_ns = hg_now_ns();
	}
	struct video_index vi;
	memset(&vi, 0, sizeof vi);
	vi.frame_topic_id = 0;
	vi.rtp_timestamp = ts;
	vi.frame_seq = frame_seq;
	vi.capture_ns = capture_ns;
	vi.bytes = bytes;
	vi.keyframe = (f & RTP_AU_KEYFRAME) != 0;
	vi.params_added = (f & RTP_AU_PARAMS_ADDED) != 0;
	hg_publish_with(pub_index, &vi, sizeof vi, capture_ns, HG_NO_REFS, NULL, 0);
}

/* ---- Source=file: ------------------------------------------------------------ */

static uint8_t *file_buf;
static size_t file_len, file_pos;
static uint64_t file_seq;
static int file_done;

static int file_open(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0 || st.st_size <= 0 || (uint64_t)st.st_size > FILE_MAX) {
		mod_log(WHO, "%s: %s", path, fd < 0 ? strerror(errno) : "empty, or larger than 64 MB");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	file_buf = malloc((size_t)st.st_size);
	ssize_t n = file_buf ? read(fd, file_buf, (size_t)st.st_size) : -1;
	close(fd);
	if (n != st.st_size) {
		mod_log(WHO, "%s: short read", path);
		return -1;
	}
	file_len = (size_t)n;
	return 0;
}

/* The next access unit of the file, one per frame period. */
static void file_tick(void)
{
	if (file_done)
		return;
	if (file_pos >= file_len) {
		if (!C.loop) {
			file_done = 1;
			mod_log(WHO, "end of %s: %llu frames sent", C.source + 5, (unsigned long long)frames);
			return;
		}
		file_pos = 0;
	}
	size_t n = h264_au_len(file_buf + file_pos, file_len - file_pos);
	if (!n) {
		file_pos = file_len;
		return;
	}
	send_frame(file_buf + file_pos, n, ++file_seq, hg_now_ns());
	file_pos += n;
}

/* ---- Source=uvc: --------------------------------------------------------------- */

static int vfd = -1;
static struct {
	void *p;
	size_t len;
} vbuf[UVC_BUFFERS];
static int force_key_ok = 1;

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static void force_keyframe(void)
{
	if (!force_key_ok)
		return;
	struct v4l2_control c = { .id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, .value = 1 };
	if (xioctl(vfd, VIDIOC_S_CTRL, &c) < 0) {
		force_key_ok = 0;
		mod_log(WHO, "%s cannot be asked for a keyframe (V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME: %s): "
			     "a receiver waits for the camera's own next one",
			C.source + 4, strerror(errno));
	}
}

static int uvc_open(const char *dev)
{
	vfd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (vfd < 0) {
		mod_log(WHO, "%s: %s", dev, strerror(errno));
		return -1;
	}
	struct v4l2_format f;
	memset(&f, 0, sizeof f);
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = C.width;
	f.fmt.pix.height = C.height;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	f.fmt.pix.field = V4L2_FIELD_ANY;
	if (xioctl(vfd, VIDIOC_S_FMT, &f) < 0 || f.fmt.pix.pixelformat != V4L2_PIX_FMT_H264) {
		mod_log(WHO, "%s: no H.264 (%s): this node does not offer it; `v4l2-ctl -d %s --list-formats-ext` "
			     "lists what each of the camera's nodes does",
			dev, errno ? strerror(errno) : "another format given", dev);
		return -1;
	}
	if (f.fmt.pix.width != C.width || f.fmt.pix.height != C.height)
		mod_log(WHO, "%s: %ux%u asked, %ux%u given", dev, C.width, C.height, f.fmt.pix.width,
			f.fmt.pix.height);
	struct v4l2_streamparm sp;
	memset(&sp, 0, sizeof sp);
	sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	sp.parm.capture.timeperframe.numerator = 1;
	sp.parm.capture.timeperframe.denominator = C.fps;
	(void)xioctl(vfd, VIDIOC_S_PARM, &sp); /* best effort: the timestamps follow C.fps either way */
	struct v4l2_requestbuffers rb = { .count = UVC_BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					  .memory = V4L2_MEMORY_MMAP };
	if (xioctl(vfd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		mod_log(WHO, "%s: VIDIOC_REQBUFS: %s", dev, strerror(errno));
		return -1;
	}
	for (unsigned i = 0; i < rb.count && i < UVC_BUFFERS; i++) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (xioctl(vfd, VIDIOC_QUERYBUF, &b) < 0)
			return -1;
		vbuf[i].len = b.length;
		vbuf[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
		if (vbuf[i].p == MAP_FAILED || xioctl(vfd, VIDIOC_QBUF, &b) < 0) {
			mod_log(WHO, "%s: buffers: %s", dev, strerror(errno));
			return -1;
		}
	}
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(vfd, VIDIOC_STREAMON, &t) < 0) {
		mod_log(WHO, "%s: VIDIOC_STREAMON: %s", dev, strerror(errno));
		return -1;
	}
	force_keyframe(); /* a receiver may be there already */
	return 0;
}

/* Every frame the camera has ready: each V4L2 buffer is one access unit. */
static void uvc_read(void)
{
	for (;;) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0)
			return; /* EAGAIN: none left */
		uint64_t cap = hg_now_ns();
		if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
			cap = (uint64_t)b.timestamp.tv_sec * 1000000000u + (uint64_t)b.timestamp.tv_usec * 1000u;
		if (b.index < UVC_BUFFERS && b.bytesused)
			send_frame(vbuf[b.index].p, b.bytesused, (uint64_t)b.sequence + 1, cap);
		(void)xioctl(vfd, VIDIOC_QBUF, &b);
	}
}

/* ---- The module ------------------------------------------------------------- */

static void status_line(uint64_t now)
{
	static uint64_t last_ns, last_bytes;
	if (now - last_ns < 1000000000u)
		return;
	double kbps = last_ns ? (double)(R.bytes - last_bytes) * 8.0 / 1000.0 / ((double)(now - last_ns) / 1e9) : 0;
	last_ns = now;
	last_bytes = R.bytes;
	char line[256];
	size_t u = (size_t)mod_fmt(line, sizeof line, "%s to %s: %llu frames, %llu keyframes, %llu packets, %.0f kbit/s",
				   C.source, C.peer, (unsigned long long)frames, (unsigned long long)keyframes,
				   (unsigned long long)R.packets, kbps);
	if (send_errors && u < sizeof line)
		u += (size_t)mod_fmt(line + u, sizeof line - u, "; %llu send errors", (unsigned long long)send_errors);
	if (file_done && u < sizeof line)
		mod_fmt(line + u, sizeof line - u, "; file ended");
	hg_status(line);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s encoder.conf\n", argv[0]);
		return 2;
	}
	mod_init(WHO);
	if (load_conf(argv[1]) < 0)
		return 78;
	pub_index = hg_pub_open("video.index", &VIDEO_INDEX_TYPE, NULL);
	if (!pub_index) {
		mod_log(WHO, "hg_pub_open video.index: %s", strerror(errno));
		return 4;
	}
	if (open_peer() < 0)
		return 78;
	uint32_t ssrc = (uint32_t)(hg_now_ns() ^ ((uint64_t)getpid() << 16));
	rtp_h264_init(&R, ssrc, C.pt, C.max_packet);

	int uvc = strncmp(C.source, "uvc:", 4) == 0;
	if (uvc ? uvc_open(C.source + 4) < 0 : file_open(C.source + 5) < 0) {
		hg_status("the source cannot be opened (the log says why)");
		return 78;
	}
	/* The timer: the file's frame period, or 100 ms for the keyframe check
	 * and the status line. The camera's frames wake the loop themselves. */
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	long period = uvc ? 100000000L : (long)(1000000000L / C.fps);
	struct itimerspec its = { .it_interval = { period / 1000000000L, period % 1000000000L },
				  .it_value = { period / 1000000000L, period % 1000000000L } };
	timerfd_settime(tfd, 0, &its, NULL);
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
	epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
	if (uvc) {
		ev.data.fd = vfd;
		epoll_ctl(ep, EPOLL_CTL_ADD, vfd, &ev);
	}
	/* Calls, when the wiring has any (none in this stage): only what is read
	 * goes into the wait set (hangar#1). */
	hg_sub *subs[HG_MAX_WAIT_FD];
	int n = hg_call_subs(subs, HG_MAX_WAIT_FD);
	int efd = n > 0 ? hg_wait_fd(subs, n) : -1;
	if (efd >= 0) {
		ev.data.fd = efd;
		epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
	}
	mod_log(WHO, "%s to %s, %u frames/s, packets of at most %zu bytes, payload type %u%s", C.source, C.peer, C.fps,
		C.max_packet, C.pt, uvc ? "" : C.loop ? ", looped" : "");
	hg_ready();
	last_key_ns = hg_now_ns();
	while (!mod_stop) {
		struct epoll_event evs[4];
		int k = epoll_wait(ep, evs, 4, 1000);
		if (k < 0 && errno != EINTR)
			break;
		for (int i = 0; i < k; i++) {
			int fd = evs[i].data.fd;
			if (fd == vfd) {
				uvc_read();
			} else if (fd == tfd) {
				uint64_t ticks;
				if (read(tfd, &ticks, sizeof ticks) == sizeof ticks && !uvc)
					file_tick();
			} else if (fd == efd) {
				uint64_t v;
				(void)!read(efd, &v, sizeof v);
			}
		}
		if (n > 0)
			hg_serve();
		uint64_t now = hg_now_ns();
		if (uvc && C.keyframe_ns && now - last_key_ns > C.keyframe_ns) {
			force_keyframe();
			last_key_ns = now; /* asked: the next ask after another interval */
		}
		status_line(now);
	}
	mod_log(WHO, "stopping: %llu frames, %llu keyframes, %llu packets sent", (unsigned long long)frames,
		(unsigned long long)keyframes, (unsigned long long)R.packets);
	if (efd >= 0)
		hg_wait_fd_close();
	if (uvc) {
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		(void)xioctl(vfd, VIDIOC_STREAMOFF, &t);
	}
	return 0;
}
