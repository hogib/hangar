// SPDX-License-Identifier: GPL-3.0-or-later
/* The console's HTTP side (modules/console/dawn_patrold.c: conn_read, route and
 * every handler behind it, with console_api.c's JSON bodies; the request is
 * parsed by lib/net/http.c). This is network input: the console listens on TCP
 * for the operator's browser, and whatever else on that network connects.
 *
 * dawn_patrold.c is included whole. The harness makes a connection as
 * conn_accept does, over a socket pair, writes the input to the far end in
 * pieces and hands the connection its events. There is no manager: a POST that
 * reaches the control socket or a call is refused where it starts; the topic
 * directory is this process's own and empty.
 *
 * Input: one byte, the size of the pieces the bytes arrive in (0: all at
 * once; else that many * 8 + 1), then the bytes of the connection. */
#include "fuzz.h"

#include <sys/stat.h>

#define main console_main
#include "dawn_patrold.c"
#undef main

/* The page the binary embeds (web/index.html through the embed tool). */
const unsigned char console_page_html[] = "<!doctype html><title>fuzz</title>";
const size_t console_page_html_len = sizeof console_page_html - 1;

static char dir[256];

static void cleanup(void)
{
	char p[300];
	static const char *const names[] = { "topics/.dir", "topics", "log", "" };
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		snprintf(p, sizeof p, "%s/%s", dir, names[i]);
		if (unlink(p) < 0)
			rmdir(p);
	}
}

static void setup(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/hg-fuzz-console-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir))
		abort();
	atexit(cleanup);
	snprintf(G.role, sizeof G.role, "vehicle");
	snprintf(G.topic_dir, sizeof G.topic_dir, "%s/topics", dir);
	snprintf(G.control_path, sizeof G.control_path, "%s/no-control", dir);
	snprintf(G.log_dir, sizeof G.log_dir, "%s/log", dir);
	snprintf(G.journal_path, sizeof G.journal_path, "%s/no-journal", dir);
	snprintf(G.hellion_url, sizeof G.hellion_url, "%s", CON_HELLION_DEFAULT);
	mkdir(G.topic_dir, 0700);
	mkdir(G.log_dir, 0700);
	setenv("HANGAR_TOPIC_DIR", G.topic_dir, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_PEER_TOPIC_DIR");
	G.sub_system = hg_sub_open("hangar.vehicle.system", &HANGAR_SYSTEM_TYPE);
	G.sub_modules = hg_sub_open_from("hangar.vehicle.modules", &HANGAR_MODULES_TYPE, HG_OLDEST);
	if (!G.sub_system || !G.sub_modules)
		abort();
	call_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (call_tfd < 0 || epfd < 0)
		abort();
	api_tick(&G);
}

/* What the console sent back, checked for what every answer has. */
static void take_answer(int sock)
{
	static char buf[1 << 16];
	size_t got = 0;
	for (;;) {
		ssize_t n = recv(sock, buf + got, sizeof buf - 1 - got, MSG_DONTWAIT);
		if (n <= 0)
			break;
		got += (size_t)n;
		if (got == sizeof buf - 1) {
			got = sizeof buf - 2; /* a long body: its head is what is looked at */
			char sink[4096];
			while (recv(sock, sink, sizeof sink, MSG_DONTWAIT) > 0)
				;
			break;
		}
	}
	if (!got)
		return;
	buf[got] = 0;
	FZ_CHECK(strncmp(buf, "HTTP/1.1 ", 9) == 0);
	FZ_CHECK(strstr(buf, "\r\n\r\n") != NULL);
	FZ_NOTE("answer: %.*s", (int)strcspn(buf, "\r"), buf);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!dir[0])
		setup();
	if (size < 1)
		return 0;
	size_t piece = data[0] ? (size_t)data[0] * 8 + 1 : size;
	data++;
	size--;
	int sp[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sp) < 0)
		abort();
	/* As conn_accept makes one. */
	struct conn *c = calloc(1, sizeof *c);
	if (!c || !(c->in = malloc(CON_IN_MAX + 1)))
		abort();
	c->fd = sp[0];
	c->deadline_ns = hg_now_ns() + CON_CONN_TIMEOUT_NS;
	struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = c };
	epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
	conns[n_conns++] = c;

	for (size_t off = 0; off < size && !c->closed;) {
		size_t k = size - off < piece ? size - off : piece;
		if (send(sp[1], data + off, k, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)k)
			break;
		off += k;
		conn_event(c, EPOLLIN);
		FZ_CHECK(c->closed || c->in_len <= CON_IN_MAX);
		take_answer(sp[1]);
	}
	/* Calls in flight get one look; then the client goes away. */
	calls_poll();
	take_answer(sp[1]);
	shutdown(sp[1], SHUT_WR);
	conn_event(c, EPOLLIN | EPOLLRDHUP);
	if (!c->closed)
		conn_close(c);
	take_answer(sp[1]);
	close(sp[1]);
	conns_reap();
	FZ_CHECK(n_conns == 0 && n_calls == 0);
	return 0;
}
