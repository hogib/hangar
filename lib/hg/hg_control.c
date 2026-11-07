// SPDX-License-Identifier: LGPL-3.0-or-later
/* Control socket client: one SOCK_SEQPACKET connection per request, a
 * descriptor received with SCM_RIGHTS when the reply carries one. */
#define _GNU_SOURCE
#include "hg_control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int hg_control_connect(const char *path)
{
	if (!path)
		path = getenv("HANGAR_CONTROL");
	if (!path || !*path) {
		errno = EINVAL;
		return -1;
	}
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(sa.sun_path, path);
	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (s < 0)
		return -1;
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
		int e = errno;
		close(s);
		errno = e;
		return -1;
	}
	return s;
}

ssize_t hg_control_request(int sock, const void *req, size_t len, char *reply, size_t replylen,
			   int *fd_out)
{
	if (fd_out)
		*fd_out = -1;
	if (replylen == 0) {
		errno = EINVAL;
		return -1;
	}
	ssize_t w;
	while ((w = send(sock, req, len, MSG_NOSIGNAL)) < 0 && errno == EINTR)
		;
	if (w < 0)
		return -1;
	union {
		struct cmsghdr h;
		unsigned char buf[CMSG_SPACE(sizeof(int))];
	} cm;
	struct iovec iov = { reply, replylen - 1 };
	struct msghdr mh;
	memset(&mh, 0, sizeof mh);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = &cm;
	mh.msg_controllen = sizeof cm;
	ssize_t r;
	while ((r = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR)
		;
	if (r < 0)
		return -1;
	reply[r] = 0;
	int fd = -1;
	/* One control message at most: the manager sends one descriptor. */
	struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
	if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
		int *fds = (int *)(void *)CMSG_DATA(c);
		size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (size_t i = 0; i < n; i++) {
			if (fd < 0)
				fd = fds[i];
			else
				close(fds[i]);
		}
	}
	if (r == 0 && fd < 0) {
		errno = EPROTO;
		return -1;
	}
	if (fd_out)
		*fd_out = fd;
	else if (fd >= 0)
		close(fd);
	return r;
}

int hg_control_ok(const char *reply)
{
	return reply && reply[0] == 'o' && reply[1] == 'k' &&
	       (reply[2] == 0 || reply[2] == '\n' || reply[2] == ' ');
}

ssize_t hg_control_call(const char *path, const void *req, size_t len, char *reply,
			size_t replylen, int *fd_out, int wait)
{
	long delay_ms = 20;
	for (;;) {
		int s = hg_control_connect(path);
		if (s < 0) {
			if (!wait || !(errno == ENOENT || errno == ECONNREFUSED || errno == EAGAIN))
				return -1;
			struct timespec ts = { delay_ms / 1000, (delay_ms % 1000) * 1000000L };
			nanosleep(&ts, NULL);
			if (delay_ms < 200)
				delay_ms *= 2;
			continue;
		}
		ssize_t r = hg_control_request(s, req, len, reply, replylen, fd_out);
		int e = errno;
		close(s);
		if (r < 0 && wait && e == EPROTO) {
			/* The manager went away mid-request (a re-exec): ask again. */
			struct timespec ts = { 0, 50000000L };
			nanosleep(&ts, NULL);
			continue;
		}
		errno = e;
		return r;
	}
}
