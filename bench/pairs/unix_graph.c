/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/unix_graph.c: a module of a graph, on Unix domain sockets (graph.h;
 * README.md, "A vehicle's graph"). The floor again: what the same graph costs with
 * the kernel's IPC and nothing else. Each publication listens on
 * $PAIR_DIR/<topic>.sock (SOCK_SEQPACKET); each subscription connects to its
 * topic's, trying again every 20 ms until the publisher is there; the module sleeps
 * in one ppoll over its subscriptions and its listening sockets. A publication never
 * waits: a subscriber whose buffer is full does not get the message. */
#include "graph.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>

#define MAX_READERS 32

static const struct graph_opts *O;
static struct {
	int lfd, n, fd[MAX_READERS];
} pubs[GRAPH_MAX];
static int subs[GRAPH_MAX]; /* -1: not connected yet */
static struct sockaddr_un sub_addr[GRAPH_MAX];
static unsigned char *buf;
static uint64_t next_connect_ns;

static int path_of(struct sockaddr_un *a, const char *topic)
{
	const char *dir = getenv("PAIR_DIR");
	a->sun_family = AF_UNIX;
	return snprintf(a->sun_path, sizeof a->sun_path, "%s/%s.sock", dir && *dir ? dir : "/tmp", topic) <
			       (int)sizeof a->sun_path
		       ? 0
		       : -1;
}

static int open_all(const struct graph_opts *o)
{
	O = o;
	unsigned biggest = GRAPH_MIN_SIZE;
	for (int i = 0; i < o->npub; i++) {
		struct sockaddr_un a;
		if (path_of(&a, o->pub[i].topic) < 0)
			return -1;
		unlink(a.sun_path);
		pubs[i].lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (pubs[i].lfd < 0 || bind(pubs[i].lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(pubs[i].lfd, MAX_READERS) < 0) {
			fprintf(stderr, "unix_graph: %s: %s\n", a.sun_path, strerror(errno));
			return -1;
		}
	}
	for (int i = 0; i < o->nsub; i++) {
		subs[i] = -1;
		if (path_of(&sub_addr[i], o->sub[i].topic) < 0)
			return -1;
		if (o->sub[i].size > biggest)
			biggest = o->sub[i].size;
	}
	return (buf = malloc(biggest)) ? 0 : -1;
}

static int pub_send(int i, void *msg, size_t n)
{
	for (int k = 0; k < pubs[i].n; k++)
		if (send(pubs[i].fd[k], msg, n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN && errno != EINTR) {
			close(pubs[i].fd[k]);
			pubs[i].fd[k--] = pubs[i].fd[--pubs[i].n];
		}
	return 0;
}

static int wait_any(int64_t timeout_ns)
{
	static struct pollfd p[2 * GRAPH_MAX];
	int n = 0, missing = 0;
	uint64_t now = pair_now_ns();
	for (int i = 0; i < O->nsub; i++) {
		if (subs[i] < 0 && now >= next_connect_ns) {
			int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
			if (s >= 0 && connect(s, (struct sockaddr *)&sub_addr[i], sizeof sub_addr[i]) == 0)
				subs[i] = s;
			else if (s >= 0)
				close(s);
		}
		missing += subs[i] < 0;
	}
	if (now >= next_connect_ns)
		next_connect_ns = now + 20000000ull;
	if (missing && timeout_ns > 20000000)
		timeout_ns = 20000000;
	for (int i = 0; i < O->nsub; i++)
		p[n++] = (struct pollfd){ .fd = subs[i], .events = POLLIN }; /* a negative fd is skipped */
	for (int i = 0; i < O->npub; i++)
		p[n++] = (struct pollfd){ .fd = pubs[i].lfd, .events = POLLIN };
	struct timespec ts = { (time_t)(timeout_ns / 1000000000), (long)(timeout_ns % 1000000000) };
	if (ppoll(p, (nfds_t)n, &ts, NULL) <= 0)
		return 0;
	for (int i = 0; i < O->nsub; i++) {
		if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		for (;;) {
			ssize_t k = recv(subs[i], buf, O->sub[i].size, MSG_DONTWAIT);
			if (k > 0) {
				graph_deliver(i, buf, (long)k);
				continue;
			}
			if (k == 0 || (errno != EAGAIN && errno != EINTR)) { /* the publisher went: look for it again */
				close(subs[i]);
				subs[i] = -1;
			}
			break;
		}
	}
	for (int i = 0; i < O->npub; i++)
		while ((p[O->nsub + i].revents & POLLIN) && pubs[i].n < MAX_READERS) {
			int s = accept4(pubs[i].lfd, NULL, NULL, SOCK_CLOEXEC);
			if (s < 0)
				break;
			pubs[i].fd[pubs[i].n++] = s;
		}
	return 0;
}

static void close_all(void)
{
	for (int i = 0; i < O->npub; i++) {
		struct sockaddr_un a;
		for (int k = 0; k < pubs[i].n; k++)
			close(pubs[i].fd[k]);
		close(pubs[i].lfd);
		if (path_of(&a, O->pub[i].topic) == 0)
			unlink(a.sun_path);
	}
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
	static const struct graph_system sys = {
		.name = "unix", .version = version, .open = open_all, .pub_send = pub_send, .wait = wait_any, .close = close_all,
	};
	return graph_main(argc, argv, &sys);
}
