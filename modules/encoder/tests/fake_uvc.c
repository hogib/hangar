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
	case VIDIOC_S_CTRL:
		return 0;
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
