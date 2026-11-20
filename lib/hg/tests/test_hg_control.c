// SPDX-License-Identifier: LGPL-3.0-or-later
// The control socket's client (hg_control.h) against a peer on a
// socketpair: a reply that fits, one that fills the buffer to its last
// byte, and one that does not fit, with and without a descriptor.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hg_control.h"
#include "test_harness.h"

/* A peer that answers one request with `len` bytes ("ok\n" then x's),
 * and a descriptor when with_fd. The client's end of the pair. */
static int peer(size_t len, int with_fd, pid_t *pid)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0)
		return -1;
	*pid = fork();
	if (*pid == 0) {
		char req[64];
		static char rep[4096];
		close(sv[0]);
		if (recv(sv[1], req, sizeof req, 0) <= 0)
			_exit(1);
		memset(rep, 'x', sizeof rep);
		memcpy(rep, "ok\n", 3);
		struct iovec iov = { rep, len };
		struct msghdr mh;
		memset(&mh, 0, sizeof mh);
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		union {
			struct cmsghdr h;
			unsigned char buf[CMSG_SPACE(sizeof(int))];
		} cm;
		if (with_fd) {
			int fd = open("/dev/null", O_RDONLY);
			memset(&cm, 0, sizeof cm);
			mh.msg_control = &cm;
			mh.msg_controllen = sizeof cm;
			cm.h.cmsg_level = SOL_SOCKET;
			cm.h.cmsg_type = SCM_RIGHTS;
			cm.h.cmsg_len = CMSG_LEN(sizeof(int));
			memcpy(CMSG_DATA(&cm.h), &fd, sizeof fd);
		}
		_exit(sendmsg(sv[1], &mh, 0) == (ssize_t)len ? 0 : 1);
	}
	close(sv[1]);
	return sv[0];
}

static void done(int sock, pid_t pid)
{
	int st;
	close(sock);
	waitpid(pid, &st, 0);
}

static int open_fds(void)
{
	int n = 0;
	for (int fd = 0; fd < 256; fd++)
		if (fcntl(fd, F_GETFD) >= 0)
			n++;
	return n;
}

/* A reply longer than the caller's buffer was cut to it and returned as
 * if whole (127, errno 0): the caller parsed the first part of a reply as
 * the reply. It is an error now, EMSGSIZE, and a descriptor that came with
 * it is closed, not handed over with half a reply. */
static void test_truncated_reply_is_an_error(void)
{
	char reply[128];
	pid_t pid;
	int fds0 = open_fds();
	/* A reply that fits. */
	int s = peer(100, 0, &pid);
	ssize_t n = hg_control_request(s, "status", 6, reply, sizeof reply, NULL);
	CHECK_EQ_INT(n, 100);
	CHECK(hg_control_ok(reply) && strlen(reply) == 100);
	done(s, pid);
	/* The longest that fits: the buffer keeps one byte for the NUL. */
	s = peer(127, 0, &pid);
	n = hg_control_request(s, "status", 6, reply, sizeof reply, NULL);
	CHECK_EQ_INT(n, 127);
	CHECK_EQ_INT(strlen(reply), 127);
	done(s, pid);
	/* One byte more, and far more. */
	s = peer(128, 0, &pid);
	errno = 0;
	n = hg_control_request(s, "status", 6, reply, sizeof reply, NULL);
	CHECK_EQ_INT(n, -1);
	CHECK_EQ_INT(errno, EMSGSIZE);
	done(s, pid);
	s = peer(600, 0, &pid);
	errno = 0;
	n = hg_control_request(s, "status", 6, reply, sizeof reply, NULL);
	int e = errno;
	printf("    a 600-byte reply into a 128-byte buffer: returned %zd, errno %d\n", n, e);
	CHECK_EQ_INT(n, -1);
	CHECK_EQ_INT(e, EMSGSIZE);
	CHECK_EQ_INT(strlen(reply), 127); /* what fitted is there to print */
	done(s, pid);
	/* With a descriptor: handed over with a whole reply, closed with a cut one. */
	int fd = -1;
	s = peer(100, 1, &pid);
	n = hg_control_request(s, "page", 4, reply, sizeof reply, &fd);
	CHECK_EQ_INT(n, 100);
	CHECK(fd >= 0);
	if (fd >= 0)
		close(fd);
	done(s, pid);
	fd = 77;
	s = peer(600, 1, &pid);
	errno = 0;
	n = hg_control_request(s, "page", 4, reply, sizeof reply, &fd);
	CHECK_EQ_INT(n, -1);
	CHECK_EQ_INT(errno, EMSGSIZE);
	CHECK_EQ_INT(fd, -1);
	done(s, pid);
	CHECK_EQ_INT(open_fds(), fds0);
}

int main(void)
{
	RUN_TEST(test_truncated_reply_is_an_error);
	TEST_MAIN_END();
}
