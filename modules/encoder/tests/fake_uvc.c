// SPDX-License-Identifier: GPL-3.0-or-later
// A camera for the encoder's suite: an LD_PRELOAD library that gives one
// process a V4L2 H.264 capture node with no camera and no kernel module.
// open($FAKE_UVC) returns one end of a socketpair; a thread makes it readable
// 30 times a second (a frame is ready); the VIDIOC_* requests the encoder
// makes are answered here, each buffer one IDR slice. After
// $FAKE_UVC_UNPLUG_S seconds the camera is "unplugged": the other end is
// closed, so the descriptor reports a hang-up at every poll, and every ioctl
// fails with ENODEV. That is what the kernel does for a video node whose
// device is gone (v4l2_poll answers EPOLLERR | EPOLLHUP once the device is
// unregistered, v4l2_ioctl -ENODEV); a real unplug was not observed. With
// $FAKE_UVC_STALL_S instead, the frames stop after that long and nothing else
// happens: a camera that hangs without an error. (After the reproduction of
// the 2026-11-20 review, repro-modules/fake_uvc_preload.c.)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

static int fake_fd = -1, cam_fd = -1;
static volatile int unplugged;
static unsigned seq;

/* The camera's controls (the encoder's parameters reach them through
 * lib/camera's camera_ctrl_apply): ranges of a UVC webcam's kind, the value
 * kept, each set written to stderr. As a camera does, a manual exposure or
 * white balance is refused with EPERM while its automatic mode is on, and
 * there is no sharpness control at all. */
static struct fake_ctl {
	unsigned id;
	const char *name;
	int min, max, value;
} ctls[] = {
	{ V4L2_CID_BRIGHTNESS, "brightness", -64, 64, 0 },
	{ V4L2_CID_CONTRAST, "contrast", 0, 64, 32 },
	{ V4L2_CID_SATURATION, "saturation", 0, 128, 64 },
	{ V4L2_CID_GAIN, "gain", 0, 100, 0 },
	{ V4L2_CID_EXPOSURE_AUTO, "exposure_auto", 0, 3, V4L2_EXPOSURE_APERTURE_PRIORITY },
	{ V4L2_CID_EXPOSURE_ABSOLUTE, "exposure_absolute", 1, 5000, 157 },
	{ V4L2_CID_EXPOSURE_AUTO_PRIORITY, "exposure_auto_priority", 0, 1, 1 },
	{ V4L2_CID_AUTO_WHITE_BALANCE, "white_balance_auto", 0, 1, 1 },
	{ V4L2_CID_WHITE_BALANCE_TEMPERATURE, "white_balance_temperature", 2800, 6500, 4600 },
};

static struct fake_ctl *ctl_find(unsigned id)
{
	for (size_t i = 0; i < sizeof ctls / sizeof ctls[0]; i++)
		if (ctls[i].id == id)
			return &ctls[i];
	return NULL;
}

static int ctl_query(struct v4l2_queryctrl *q)
{
	struct fake_ctl *c = ctl_find(q->id);
	if (!c) {
		errno = EINVAL;
		return -1;
	}
	q->type = c->id == V4L2_CID_EXPOSURE_AUTO ? V4L2_CTRL_TYPE_MENU : V4L2_CTRL_TYPE_INTEGER;
	snprintf((char *)q->name, sizeof q->name, "%s", c->name);
	q->minimum = c->min;
	q->maximum = c->max;
	q->step = 1;
	q->default_value = c->value;
	q->flags = 0;
	return 0;
}

static int ctl_set(const struct v4l2_control *v)
{
	if (v->id == V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME)
		return 0;
	struct fake_ctl *c = ctl_find(v->id);
	if (!c || v->value < c->min || v->value > c->max) {
		errno = c ? ERANGE : EINVAL;
		return -1;
	}
	if ((v->id == V4L2_CID_EXPOSURE_ABSOLUTE && ctl_find(V4L2_CID_EXPOSURE_AUTO)->value != V4L2_EXPOSURE_MANUAL) ||
	    (v->id == V4L2_CID_WHITE_BALANCE_TEMPERATURE && ctl_find(V4L2_CID_AUTO_WHITE_BALANCE)->value != 0)) {
		errno = EPERM;
		return -1;
	}
	c->value = v->value;
	fprintf(stderr, "fake_uvc: control %s = %d\n", c->name, v->value);
	return 0;
}

static void *camera(void *arg)
{
	(void)arg;
	const char *unplug = getenv("FAKE_UVC_UNPLUG_S"), *stall = getenv("FAKE_UVC_STALL_S");
	double life = unplug ? atof(unplug) : stall ? atof(stall) : 1e9;
	for (double t = 0; t < life; t += 1.0 / 30) {
		(void)!write(cam_fd, "f", 1);
		usleep(33333);
	}
	if (unplug) {
		unplugged = 1;
		close(cam_fd);
		fprintf(stderr, "fake_uvc: unplugged\n");
	} else {
		fprintf(stderr, "fake_uvc: stalled\n");
	}
	return NULL;
}

static int fake_open(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0)
		return -1;
	fake_fd = sv[0];
	cam_fd = sv[1];
	pthread_t t;
	pthread_create(&t, NULL, camera, NULL);
	pthread_detach(t);
	fprintf(stderr, "fake_uvc: opened\n");
	return fake_fd;
}

static int is_fake(const char *path)
{
	const char *f = getenv("FAKE_UVC");
	return f && path && strcmp(path, f) == 0;
}

#define OPEN_WRAPPER(name)                                              \
	int name(const char *path, int flags, ...)                      \
	{                                                               \
		static int (*real)(const char *, int, ...);             \
		if (!real)                                              \
			real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, #name); \
		if (is_fake(path))                                      \
			return fake_open();                             \
		mode_t mode = 0;                                        \
		if (flags & (O_CREAT | O_TMPFILE)) {                    \
			va_list ap;                                     \
			va_start(ap, flags);                            \
			mode = va_arg(ap, mode_t);                      \
			va_end(ap);                                     \
		}                                                       \
		return real(path, flags, mode);                         \
	}
OPEN_WRAPPER(open)
OPEN_WRAPPER(open64)

int ioctl(int fd, unsigned long req, ...)
{
	static int (*real)(int, unsigned long, ...);
	if (!real)
		real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (fd != fake_fd || fake_fd < 0)
		return real(fd, req, arg);
	if (unplugged) {
		errno = ENODEV;
		return -1;
	}
	switch (req) {
	case VIDIOC_S_FMT:
	case VIDIOC_S_PARM:
	case VIDIOC_REQBUFS:
	case VIDIOC_QBUF:
	case VIDIOC_STREAMON:
	case VIDIOC_STREAMOFF:
		return 0;
	case VIDIOC_S_CTRL:
		return ctl_set(arg);
	case VIDIOC_QUERYCTRL:
		return ctl_query(arg);
	case VIDIOC_G_CTRL: {
		struct v4l2_control *v = arg;
		struct fake_ctl *c = ctl_find(v->id);
		if (!c) {
			errno = EINVAL;
			return -1;
		}
		v->value = c->value;
		return 0;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b = arg;
		b->length = 65536;
		b->m.offset = b->index * 65536;
		return 0;
	}
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b = arg;
		char c;
		if (recv(fake_fd, &c, 1, MSG_DONTWAIT) != 1) {
			errno = EAGAIN;
			return -1;
		}
		b->index = 0;
		b->bytesused = 64;
		b->sequence = seq++;
		b->flags = 0;
		return 0;
	}
	default:
		break;
	}
	errno = ENOTTY;
	return -1;
}

static void *fake_map(size_t len)
{
	/* One IDR slice NAL after a start code: enough to be an access unit. */
	unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p != MAP_FAILED) {
		static const unsigned char au[] = { 0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33, 0xff };
		memset(p, 0x55, 64);
		memcpy(p, au, sizeof au);
	}
	return p;
}

#define MMAP_WRAPPER(name, off_type)                                                  \
	void *name(void *addr, size_t len, int prot, int flags, int fd, off_type off) \
	{                                                                             \
		static void *(*real)(void *, size_t, int, int, int, off_type);        \
		if (!real)                                                            \
			real = (void *(*)(void *, size_t, int, int, int, off_type))dlsym(RTLD_NEXT, #name); \
		if (fd >= 0 && fd == fake_fd)                                         \
			return fake_map(len);                                         \
		return real(addr, len, prot, flags, fd, off);                         \
	}
MMAP_WRAPPER(mmap, off_t)
MMAP_WRAPPER(mmap64, off64_t)
