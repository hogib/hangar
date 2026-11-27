/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/unix_pair.c: Unix domain sockets as a node pair for bench/pairs
 * (README.md there). The floor: what the kernel's own IPC costs between two
 * processes, with no middleware at all. Whatever a middleware that sends through
 * the kernel reads above this row is its own; one that reads below it does not
 * send through the kernel.
 *
 *   unix_pair --role pub|sub ... [-- --stream] [-- --sndbuf BYTES]
 *
 * The publisher listens on $PAIR_DIR/<topic>.sock; each subscriber connects (and
 * tries again every millisecond until the publisher is there).
 *
 * As is, SOCK_SEQPACKET: one send is one message, and the publisher never waits. A
 * subscriber whose socket buffer is full does not get the message (it is lost, and
 * counted by the subscriber's harness): the nearest a socket comes to hangar's
 * contract, with the difference that a socket drops the newest message and keeps
 * the old ones, where hangar's ring drops the oldest. How many messages the buffer
 * holds is the kernel's accounting (net.core.wmem_default, 212992 B here), not a
 * count: some hundreds of small ones. A message must fit the buffer whole, which
 * without privileges ends near 100 KB: the registry gives this entry a largest size.
 *
 * --stream, SOCK_STREAM: any size, the message a run of bytes that the reader
 * collects until it has `size` of them. The publisher's send blocks while the
 * reader's buffer is full, so here the writer waits and nothing is lost: another
 * contract, said in the registry. It is what a megabyte through a socket honestly
 * is: several send and receive calls, and two copies.
 *
 * The subscriber sleeps in recv and is woken by the kernel when the message is
 * there: no polling. Copies between the two clock readings: user to kernel, kernel
 * to user. */
#include "pair.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>

#define MAX_SUBS 64

static int stream_mode, sndbuf;
static unsigned size;
static struct sockaddr_un addr;

static int lfd = -1, subs[MAX_SUBS], nsubs; /* publisher */
static int fd = -1, cur_timeout = -2;       /* subscriber */
static unsigned char *buf;
static size_t have;

static int setup(const struct pair_opts *o)
{
	for (int i = 0; i < o->argc; i++) {
		if (!strcmp(o->argv[i], "--stream")) {
			stream_mode = 1;
		} else if (!strcmp(o->argv[i], "--sndbuf") && i + 1 < o->argc) {
			sndbuf = atoi(o->argv[++i]);
		} else {
			fprintf(stderr, "unix_pair: unknown option %s\n", o->argv[i]);
			return -1;
		}
	}
	const char *dir = getenv("PAIR_DIR");
	if (!dir || !*dir)
		dir = "/tmp";
	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s.sock", dir, o->topic) >= (int)sizeof addr.sun_path) {
		fprintf(stderr, "unix_pair: %s/%s.sock is longer than a socket's path may be\n", dir, o->topic);
		return -1;
	}
	size = o->size;
	return 0;
}

static int sock_type(void)
{
	return (stream_mode ? SOCK_STREAM : SOCK_SEQPACKET) | SOCK_CLOEXEC;
}

static int pub_open(const struct pair_opts *o)
{
	if (setup(o) < 0)
		return -1;
	unlink(addr.sun_path);
	lfd = socket(AF_UNIX, sock_type() | SOCK_NONBLOCK, 0);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, MAX_SUBS) < 0) {
		fprintf(stderr, "unix_pair: %s: %s\n", addr.sun_path, strerror(errno));
		return -1;
	}
	return 0;
}

/* Called before the harness reads the clock for publish_ns: a subscriber that has
 * connected since the last message is taken in here, outside the sample. */
static void *pub_buffer(void)
{
	while (nsubs < MAX_SUBS) {
		int s = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
		if (s < 0)
			break;
		if (sndbuf)
			setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
		subs[nsubs++] = s;
	}
	return NULL;
}

static void drop_sub(int i)
{
	close(subs[i]);
	subs[i] = subs[--nsubs];
}

static int pub_send(void *msg, size_t n)
{
	int rc = 0;
	for (int i = 0; i < nsubs; i++) {
		if (!stream_mode) {
			/* One message or none: a full buffer is the reader's loss. A message
			 * the buffer can never hold is the send's failure, and counted. */
			if (send(subs[i], msg, n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
				if (errno == EMSGSIZE)
					rc = -1;
				else if (errno != EAGAIN && errno != EINTR)
					drop_sub(i--);
			}
			continue;
		}
		for (size_t off = 0; off < n;) {
			ssize_t k = send(subs[i], (const char *)msg + off, n - off, MSG_NOSIGNAL);
			if (k < 0 && errno == EINTR)
				continue;
			if (k < 0) {
				drop_sub(i--);
				break;
			}
			off += (size_t)k;
		}
	}
	return rc;
}

static void pub_close(void)
{
	for (int i = 0; i < nsubs; i++)
		close(subs[i]); /* the readers see the end of the connection */
	close(lfd);
	unlink(addr.sun_path);
}

static int sub_open(const struct pair_opts *o)
{
	if (setup(o) < 0 || !(buf = malloc(size)))
		return -1;
	return 0; /* the publisher may not be there yet: sub_recv connects */
}

static void set_timeout(int ms)
{
	if (ms == cur_timeout)
		return;
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	cur_timeout = ms;
}

static long sub_recv(const void **msg, int timeout_ms)
{
	if (fd < 0) {
		int s = socket(AF_UNIX, sock_type(), 0);
		if (s < 0)
			return -1;
		if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) {
			close(s);
			if (timeout_ms > 0)
				usleep(1000);
			return 0;
		}
		fd = s;
		have = 0;
	}
	if (timeout_ms > 0)
		set_timeout(timeout_ms);
	int flags = timeout_ms > 0 ? 0 : MSG_DONTWAIT;
	if (!stream_mode) {
		ssize_t k = recv(fd, buf, size, flags | MSG_TRUNC);
		if (k > 0) {
			*msg = buf;
			return (long)k;
		}
		return k == 0 ? -1 : (errno == EAGAIN || errno == EINTR) ? 0 : -1;
	}
	while (have < size) {
		ssize_t k = recv(fd, buf + have, size - have, flags);
		if (k == 0)
			return -1;
		if (k < 0)
			return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
		have += (size_t)k;
	}
	have = 0;
	*msg = buf;
	return (long)size;
}

static void sub_close(void)
{
	if (fd >= 0)
		close(fd);
}

static const char *version(void)
{
	static char v[160];
	struct utsname u;
	if (uname(&u) < 0)
		return "Linux";
	snprintf(v, sizeof v, "%s %s", u.sysname, u.release);
	return v;
}

int main(int argc, char **argv)
{
	static const struct pair_system sys = {
		.name = "unix", .version = version,
		.pub_open = pub_open, .pub_buffer = pub_buffer, .pub_send = pub_send, .pub_close = pub_close,
		.sub_open = sub_open, .sub_recv = sub_recv, .sub_close = sub_close,
	};
	return pair_main(argc, argv, &sys);
}
