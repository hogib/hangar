// SPDX-License-Identifier: GPL-3.0-or-later
/* dawn-patrold: Dawn Patrol, hangar's web console (module `console`;
 * docs/names.md, lib/hg/PLAN.md "Stage 3 addition: the web console"). The
 * monitoring half of docs/11's pilot_console: one page showing every process
 * the manager keeps alive, every topic, the recent log, with start, stop and
 * restart buttons that go through the control socket.
 *
 *   dawn-patrold [--listen ADDR] [--role vehicle|topside] [--hellion URL]
 *
 * ADDR is "host:port", "[v6]:port" or "port" (default 127.0.0.1:8080; port 0
 * picks one and the log line says which). The role names the manager's
 * topics, hangar.<role>.system and .modules: taken from the wiring when the
 * manager started this module, else --role, else hangard.conf's Role under
 * $HANGAR_ROOT, else vehicle. URL is where hellion's page is (its "control"
 * port; default http://127.0.0.1:8091/): the page's Hellion tab frames it,
 * reading the URL from /api/links.
 *
 * One thread, one epoll loop: the listening socket, every client, the
 * eventfd hg_wait_fd() signals when a manager topic has news, a 1 s timer
 * (rates from write_seq deltas, hg_alive, connection deadlines) and a
 * signalfd. lib/net/http.c, electric-eye's fuzzed server, parses each
 * request (http_parse_request); its thread-per-connection driver is not
 * used, so the sockets and responses are handled here, in its format.
 *
 *   GET  /                         the page (web/index.html, embedded at build)
 *   GET  /api/system               hangar.<role>.system, newest
 *   GET  /api/modules              one entry per module, from hangar.<role>.modules
 *   GET  /api/topics               every file of the topic directory
 *   GET  /api/topic/<name>         the newest message, decoded by its schema
 *   GET  /api/log[?lines=N]        the fallback log tails, or a journald note
 *   GET  /api/links                {"hellion": URL}, what the Hellion tab frames
 *   POST /api/module/<name>/start|stop|restart
 *   POST /api/reload               through the control socket, as this process's user
 *
 * GPL-3.0-or-later; C11; libc, pthreads (the library's helper thread),
 * lib/hg and lib/net only; builds static for musl. */
#define _GNU_SOURCE
#include "console.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#include "hg_control.h"
#include "hg_wiring.h"
#include "http.h"

/* web/index.html, as $(OBJDIR)/gen/console_page.c (legacy/hellion/embed.c). */
extern const unsigned char console_page_html[];
extern const size_t console_page_html_len;

#define CON_MAX_CONNS 32
#define CON_BODY_MAX 4096                       /* a POST carries "{}" at most */
#define CON_IN_MAX (http_max_head + CON_BODY_MAX)
#define CON_CONN_TIMEOUT_NS 10000000000ull      /* a client has 10 s to send and take its answer */

static struct console G;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("dawn-patrold: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- Paths and the role ----------------------------------------------------- */

static const char *root(void)
{
	const char *r = getenv("HANGAR_ROOT");
	return r && *r ? r : "";
}

/* hangard.conf's Role, if the file is there (as hangar/hangar.c reads it). */
static int conf_role(char *role, size_t len)
{
	char path[CON_PATH];
	snprintf(path, sizeof path, "%s/etc/hangar/hangard.conf", root());
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	char line[512];
	int found = -1;
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';')
			continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		while (*k == ' ' || *k == '\t')
			k++;
		size_t n = strlen(k);
		while (n && (k[n - 1] == ' ' || k[n - 1] == '\t'))
			k[--n] = 0;
		while (*v == ' ' || *v == '\t')
			v++;
		n = strlen(v);
		while (n && (v[n - 1] == '\n' || v[n - 1] == ' ' || v[n - 1] == '\t'))
			v[--n] = 0;
		if (strcmp(k, "Role") == 0) {
			snprintf(role, len, "%s", v);
			found = 0;
		}
	}
	fclose(f);
	return found;
}

/* The role from the wiring's `sub hangar.<role>.system` line, if wired. */
static int wiring_role(char *role, size_t len)
{
	const struct hg_wiring *w = hg_wiring();
	if (!w)
		return -1;
	for (int i = 0; i < w->n_subs; i++) {
		const char *t = w->subs[i].topic;
		if (strncmp(t, "hangar.", 7) != 0)
			continue;
		const char *dot = strchr(t + 7, '.');
		if (!dot || strcmp(dot, ".system") != 0 || (size_t)(dot - (t + 7)) >= len)
			continue;
		snprintf(role, len, "%.*s", (int)(dot - (t + 7)), t + 7);
		return 0;
	}
	return -1;
}

static void setup_paths(const char *role_arg)
{
	const char *d = getenv("HANGAR_TOPIC_DIR");
	if (d && *d)
		snprintf(G.topic_dir, sizeof G.topic_dir, "%s", d);
	else
		snprintf(G.topic_dir, sizeof G.topic_dir, "%s/dev/shm/hangar", root());
	setenv("HANGAR_TOPIC_DIR", G.topic_dir, 1);
	const char *c = getenv("HANGAR_CONTROL");
	if (c && *c)
		snprintf(G.control_path, sizeof G.control_path, "%s", c);
	else
		snprintf(G.control_path, sizeof G.control_path, "%s/run/hangar/control", root());
	snprintf(G.log_dir, sizeof G.log_dir, "%s/run/hangar/log", root());
	snprintf(G.journal_path, sizeof G.journal_path, "%s/run/systemd/journal/stdout", root());
	if (wiring_role(G.role, sizeof G.role) == 0)
		return;
	if (role_arg && *role_arg) {
		snprintf(G.role, sizeof G.role, "%s", role_arg);
		return;
	}
	if (conf_role(G.role, sizeof G.role) < 0 || !G.role[0])
		snprintf(G.role, sizeof G.role, "vehicle");
}

/* ---- Connections -------------------------------------------------------------- */

struct conn {
	int fd;
	char *in;
	size_t in_len;
	char *out;
	size_t out_len, out_off;
	uint64_t deadline_ns;
	int want_write;
};

static int epfd = -1;
static struct conn *conns[CON_MAX_CONNS];
static int n_conns;

static void conn_close(struct conn *c)
{
	epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	free(c->in);
	free(c->out);
	for (int i = 0; i < n_conns; i++)
		if (conns[i] == c) {
			conns[i] = conns[--n_conns];
			break;
		}
	free(c);
}

static const char *reason(int status)
{
	switch (status) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 413: return "Content Too Large";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 502: return "Bad Gateway";
	}
	return "Status";
}

/* Queues a complete response, in lib/net/http.c's format (Content-Length,
 * Connection: close, Cache-Control: no-store; a text type gets the charset). */
static void respond(struct conn *c, int status, const char *ctype, const void *body, size_t len)
{
	char head[512];
	int text = strncmp(ctype, "text/", 5) == 0;
	int n = snprintf(head, sizeof head,
			 "HTTP/1.1 %d %s\r\nContent-Type: %s%s\r\nContent-Length: %zu\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
			 status, reason(status), ctype, text ? "; charset=utf-8" : "", len);
	free(c->out);
	c->out = malloc((size_t)n + len);
	if (!c->out) {
		c->out_len = 0;
		return;
	}
	memcpy(c->out, head, (size_t)n);
	memcpy(c->out + n, body, len);
	c->out_len = (size_t)n + len;
	c->out_off = 0;
	c->want_write = 1;
}

static void respond_json(struct conn *c, int status, struct cbuf *b)
{
	respond(c, status, "application/json", b->p ? b->p : "", b->len);
	cbuf_free(b);
}

static void respond_error(struct conn *c, int status, const char *msg)
{
	struct cbuf b = { 0 };
	cbuf_printf(&b, "{\"error\":");
	cbuf_json_str(&b, msg, 512);
	cbuf_printf(&b, "}\n");
	respond_json(c, status, &b);
}

/* Writes what is queued; 1 when all of it went, 0 to wait for EPOLLOUT,
 * -1 when the client is gone. */
static int conn_flush(struct conn *c)
{
	while (c->out_off < c->out_len) {
		ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			c->out_off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct epoll_event ev = { .events = EPOLLOUT | EPOLLRDHUP, .data.ptr = c };
			epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
			return 0;
		}
		return -1;
	}
	return 1;
}

/* ---- Routing ------------------------------------------------------------------- */

static int valid_module_name(const char *s)
{
	size_t n = strlen(s);
	if (n < 1 || n > 31)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_'))
			return 0;
	return 1;
}

/* A POST must carry Content-Type: application/json. A cross-origin page
 * cannot send that header without a preflight this server never grants,
 * so a form on some other site cannot restart a module through the
 * operator's browser. http_parse_request keeps Content-Length alone, so
 * the raw head is scanned. */
static int head_has_json_type(const char *head, size_t len)
{
	static const char key[] = "\r\ncontent-type:";
	for (size_t i = 0; i + sizeof key - 1 < len; i++) {
		size_t k = 0;
		while (k < sizeof key - 1 && i + k < len) {
			char ch = head[i + k];
			if (ch >= 'A' && ch <= 'Z')
				ch = (char)(ch - 'A' + 'a');
			if (ch != key[k])
				break;
			k++;
		}
		if (k != sizeof key - 1)
			continue;
		const char *v = head + i + k;
		while (v < head + len && (*v == ' ' || *v == '\t'))
			v++;
		return (size_t)(head + len - v) >= 16 && strncasecmp(v, "application/json", 16) == 0;
	}
	return 0;
}

/* One control request; the reply as JSON with the status it deserves. */
static void control_post(struct conn *c, const char *req)
{
	char reply[HG_CONTROL_MAX];
	ssize_t n = hg_control_call(G.control_path, req, strlen(req), reply, sizeof reply, NULL, 0);
	struct cbuf b = { 0 };
	if (n < 0) {
		cbuf_printf(&b, "{\"ok\":false,\"request\":\"%s\",\"error\":", req);
		char msg[CON_PATH + 128];
		snprintf(msg, sizeof msg, "cannot reach the manager at %s: %s", G.control_path, strerror(errno));
		cbuf_json_str(&b, msg, sizeof msg);
		cbuf_printf(&b, "}\n");
		respond_json(c, 502, &b);
		return;
	}
	char *nl = strchr(reply, '\n');
	const char *rest = nl ? nl + 1 : "";
	if (nl)
		*nl = 0;
	if (hg_control_ok(reply)) {
		cbuf_printf(&b, "{\"ok\":true,\"request\":\"%s\",\"reply\":", req);
		cbuf_json_str(&b, rest, 4096);
		cbuf_printf(&b, "}\n");
		respond_json(c, 200, &b);
		return;
	}
	const char *why = strncmp(reply, "err ", 4) == 0 ? reply + 4 : reply;
	cbuf_printf(&b, "{\"ok\":false,\"request\":\"%s\",\"error\":", req);
	cbuf_json_str(&b, why, 4096);
	cbuf_printf(&b, "}\n");
	/* The manager's refusals: "permission denied: hangar-admin required
	 * for restart" is the one the operator must read. */
	respond_json(c, strncmp(why, "permission denied", 17) == 0 ? 403 : 409, &b);
}

static void route(struct conn *c, const HttpRequest *req)
{
	int get = strcmp(req->method, "GET") == 0;
	int post = strcmp(req->method, "POST") == 0;
	const char *p = req->path;
	struct cbuf b = { 0 };
	if (!get && !post) {
		respond_error(c, 405, "method not allowed");
		return;
	}
	if (get && strcmp(p, "/") == 0) {
		respond(c, 200, "text/html", console_page_html, console_page_html_len);
		return;
	}
	if (get && strcmp(p, "/api/system") == 0) {
		respond_json(c, api_system_json(&G, &b), &b);
		return;
	}
	if (get && strcmp(p, "/api/modules") == 0) {
		respond_json(c, api_modules_json(&G, &b), &b);
		return;
	}
	if (get && strcmp(p, "/api/topics") == 0) {
		respond_json(c, api_topics_json(&G, &b), &b);
		return;
	}
	if (get && strncmp(p, "/api/topic/", 11) == 0) {
		respond_json(c, api_topic_json(&G, p + 11, &b), &b);
		return;
	}
	if (get && strcmp(p, "/api/links") == 0) {
		respond_json(c, api_links_json(&G, &b), &b);
		return;
	}
	if (get && strcmp(p, "/api/log") == 0) {
		int lines = 100;
		const char *q = strstr(req->query, "lines=");
		if (q && (q == req->query || q[-1] == '&'))
			lines = atoi(q + 6);
		respond_json(c, api_log_json(&G, lines, &b), &b);
		return;
	}
	if (post && !head_has_json_type(c->in, req->head_len)) {
		respond_error(c, 403, "a POST needs Content-Type: application/json");
		return;
	}
	if (post && strcmp(p, "/api/reload") == 0) {
		control_post(c, "reload");
		return;
	}
	if (post && strncmp(p, "/api/module/", 12) == 0) {
		char name[32];
		const char *rest = p + 12;
		const char *slash = strchr(rest, '/');
		if (!slash || (size_t)(slash - rest) >= sizeof name) {
			respond_error(c, 404, "use /api/module/<name>/start|stop|restart");
			return;
		}
		snprintf(name, sizeof name, "%.*s", (int)(slash - rest), rest);
		const char *verb = slash + 1;
		if (!valid_module_name(name)) {
			respond_error(c, 404, "not a module name");
			return;
		}
		if (strcmp(verb, "start") != 0 && strcmp(verb, "stop") != 0 && strcmp(verb, "restart") != 0) {
			respond_error(c, 404, "use /api/module/<name>/start|stop|restart");
			return;
		}
		char req_text[80];
		snprintf(req_text, sizeof req_text, "%s %s", verb, name);
		control_post(c, req_text);
		return;
	}
	if (post && (strncmp(p, "/api/", 5) == 0 || strcmp(p, "/") == 0)) {
		respond_error(c, 405, "method not allowed");
		return;
	}
	respond_error(c, 404, "no such page");
}

/* Reads what the client sent; parses and routes once the request is whole.
 * -1 when the connection was closed here. */
static int conn_read(struct conn *c)
{
	for (;;) {
		if (c->in_len >= CON_IN_MAX) {
			respond(c, 431, "text/plain", "request too large\n", 18);
			return 0;
		}
		ssize_t n = recv(c->fd, c->in + c->in_len, CON_IN_MAX - c->in_len, MSG_DONTWAIT);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (n <= 0) {
			conn_close(c);
			return -1;
		}
		c->in_len += (size_t)n;
		c->in[c->in_len] = 0;
		HttpRequest req;
		HttpParse pr = http_parse_request(c->in, c->in_len, &req);
		if (pr == HTTP_PARSE_INCOMPLETE)
			continue;
		if (pr == HTTP_PARSE_TOO_LARGE) {
			respond(c, 431, "text/plain", "request head too large\n", 23);
			return 0;
		}
		if (pr != HTTP_PARSE_OK) {
			respond(c, 400, "text/plain", "malformed request\n", 18);
			return 0;
		}
		if (req.content_length > CON_BODY_MAX) {
			respond(c, 413, "text/plain", "request body too large\n", 23);
			return 0;
		}
		if (c->in_len < req.head_len + req.content_length)
			continue; /* the body is still coming */
		route(c, &req);
		return 0;
	}
}

static void conn_accept(int lfd)
{
	for (;;) {
		int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			return; /* EAGAIN, or a transient failure: the next wake-up tries again */
		}
		if (n_conns == CON_MAX_CONNS) {
			static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			(void)!send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
			close(fd);
			continue;
		}
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		struct conn *c = calloc(1, sizeof *c);
		if (c)
			c->in = malloc(CON_IN_MAX + 1);
		if (!c || !c->in) {
			if (c)
				free(c->in);
			free(c);
			close(fd);
			continue;
		}
		c->fd = fd;
		c->deadline_ns = hg_now_ns() + CON_CONN_TIMEOUT_NS;
		struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = c };
		if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
			free(c->in);
			free(c);
			close(fd);
			continue;
		}
		conns[n_conns++] = c;
	}
}

static void conn_event(struct conn *c, uint32_t events)
{
	if (events & (EPOLLERR | EPOLLHUP)) {
		conn_close(c);
		return;
	}
	if (!c->want_write && (events & (EPOLLIN | EPOLLRDHUP)) && conn_read(c) < 0)
		return;
	if (c->want_write) {
		int r = conn_flush(c);
		if (r != 0)
			conn_close(c); /* sent, or the client is gone */
	}
}

/* ---- Listening ------------------------------------------------------------------ */

/* Binds ADDR; the bound address into shown[]. -1 with a message on failure. */
static int listen_on(const char *spec, char *shown, size_t shown_len)
{
	char host[128] = "", port[16] = "";
	if (spec[0] == '[') {
		const char *close_b = strchr(spec, ']');
		if (!close_b || close_b[1] != ':')
			return logf_("bad address \"%s\"", spec), -1;
		snprintf(host, sizeof host, "%.*s", (int)(close_b - spec - 1), spec + 1);
		snprintf(port, sizeof port, "%s", close_b + 2);
	} else {
		const char *colon = strrchr(spec, ':');
		if (colon) {
			snprintf(host, sizeof host, "%.*s", (int)(colon - spec), spec);
			snprintf(port, sizeof port, "%s", colon + 1);
		} else {
			snprintf(port, sizeof port, "%s", spec);
		}
	}
	if (!port[0])
		return logf_("bad address \"%s\": no port", spec), -1;
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
	hints.ai_family = host[0] ? AF_UNSPEC : AF_INET6;
	int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
	if (rc != 0 && !host[0]) {
		hints.ai_family = AF_INET;
		rc = getaddrinfo(NULL, port, &hints, &res);
	}
	if (rc != 0 || !res)
		return logf_("bad address \"%s\": %s", spec, gai_strerror(rc)), -1;
	int fd = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	int one = 1, off = 0;
	if (fd >= 0) {
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		if (res->ai_family == AF_INET6 && !host[0])
			setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
	}
	if (fd < 0 || bind(fd, res->ai_addr, res->ai_addrlen) < 0 || listen(fd, 16) < 0) {
		logf_("cannot listen on %s: %s", spec, strerror(errno));
		if (fd >= 0)
			close(fd);
		freeaddrinfo(res);
		return -1;
	}
	freeaddrinfo(res);
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;
	char hbuf[INET6_ADDRSTRLEN] = "?";
	int bound_port = 0;
	if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0) {
		if (ss.ss_family == AF_INET6) {
			inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, hbuf, sizeof hbuf);
			bound_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
		} else {
			inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, hbuf, sizeof hbuf);
			bound_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
		}
	}
	snprintf(shown, shown_len, ss.ss_family == AF_INET6 ? "[%s]:%d" : "%s:%d", hbuf, bound_port);
	return fd;
}

/* ---- main ----------------------------------------------------------------------- */

static void usage(void)
{
	fputs("usage: dawn-patrold [--listen ADDR] [--role vehicle|topside] [--hellion URL]\n"
	      "  ADDR: host:port, [v6]:port or port (default 127.0.0.1:8080; port 0 picks one)\n"
	      "  URL:  hellion's page, framed by the Hellion tab (default " CON_HELLION_DEFAULT ")\n"
	      "Paths follow $HANGAR_ROOT; $HANGAR_CONTROL and $HANGAR_TOPIC_DIR override.\n",
	      stderr);
}

int main(int argc, char **argv)
{
	const char *listen_spec = "127.0.0.1:8080", *role_arg = NULL, *hellion = CON_HELLION_DEFAULT;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc)
			listen_spec = argv[++i];
		else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc)
			role_arg = argv[++i];
		else if (strcmp(argv[i], "--hellion") == 0 && i + 1 < argc)
			hellion = argv[++i];
		else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
			return usage(), 0;
		else
			return usage(), 2;
	}
	/* The signals go to the signalfd below. Blocked before any thread
	 * exists (hg_wait_fd's helper inherits the mask), or a SIGTERM landing
	 * on the helper would end the process by default action, unlogged. */
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	if (hg_init() < 0) {
		logf_("hg_init: %s (%s)", hg_init_error(), strerror(errno));
		return 3;
	}
	setup_paths(role_arg);
	/* The URL becomes a frame's src and a link's href in the page: an
	 * http(s) URL that fits, without spaces, quotes or angle brackets. */
	if ((strncmp(hellion, "http://", 7) != 0 && strncmp(hellion, "https://", 8) != 0) ||
	    strlen(hellion) >= sizeof G.hellion_url || strpbrk(hellion, " \t\r\n\"'\\<>") != NULL) {
		logf_("--hellion: not an http(s) URL: \"%s\"", hellion);
		return 2;
	}
	snprintf(G.hellion_url, sizeof G.hellion_url, "%s", hellion);

	char name[64];
	snprintf(name, sizeof name, "hangar.%s.system", G.role);
	G.sub_system = hg_sub_open(name, &HANGAR_SYSTEM_TYPE);
	if (!G.sub_system)
		logf_("cannot subscribe to %s: %s", name, strerror(errno));
	snprintf(name, sizeof name, "hangar.%s.modules", G.role);
	G.sub_modules = hg_sub_open_from(name, &HANGAR_MODULES_TYPE, HG_OLDEST);
	if (!G.sub_modules)
		logf_("cannot subscribe to %s: %s", name, strerror(errno));
	if (!G.sub_system || !G.sub_modules)
		return 4;

	hg_sub *subs[2] = { G.sub_system, G.sub_modules };
	int efd = hg_wait_fd(subs, 2);
	if (efd < 0) {
		logf_("hg_wait_fd: %s", strerror(errno));
		return 4;
	}
	char shown[80];
	int lfd = listen_on(listen_spec, shown, sizeof shown);
	if (lfd < 0)
		return 5;

	int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	struct itimerspec its = { { 1, 0 }, { 1, 0 } };
	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (sfd < 0 || tfd < 0 || epfd < 0 || timerfd_settime(tfd, 0, &its, NULL) < 0) {
		logf_("cannot set up the event loop: %s", strerror(errno));
		return 5;
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = &lfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	ev.data.ptr = &efd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);
	ev.data.ptr = &tfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);
	ev.data.ptr = &sfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

	api_tick(&G);
	char status[128];
	snprintf(status, sizeof status, "serving http://%s (role %s)", shown, G.role);
	hg_status(status);
	hg_ready();
	logf_("listening on %s, role %s, topics %s, control %s, hellion %s", shown, G.role, G.topic_dir,
	      G.control_path, G.hellion_url);

	int stop = 0;
	while (!stop) {
		struct epoll_event evs[64];
		int n = epoll_wait(epfd, evs, 64, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			logf_("epoll_wait: %s", strerror(errno));
			break;
		}
		for (int i = 0; i < n; i++) {
			void *p = evs[i].data.ptr;
			if (p == &lfd) {
				conn_accept(lfd);
			} else if (p == &efd) {
				uint64_t v;
				(void)!read(efd, &v, sizeof v);
				api_drain(&G);
			} else if (p == &tfd) {
				uint64_t v;
				(void)!read(tfd, &v, sizeof v);
				api_tick(&G);
				hg_alive();
				uint64_t now = hg_now_ns();
				for (int k = 0; k < n_conns;) {
					if (now > conns[k]->deadline_ns)
						conn_close(conns[k]); /* conns[k] is now another connection */
					else
						k++;
				}
			} else if (p == &sfd) {
				struct signalfd_siginfo si;
				(void)!read(sfd, &si, sizeof si);
				stop = 1;
			} else {
				conn_event(p, evs[i].events);
			}
		}
	}
	logf_("stopping");
	while (n_conns)
		conn_close(conns[0]);
	close(lfd);
	hg_wait_fd_close();
	hg_sub_close(G.sub_system);
	hg_sub_close(G.sub_modules);
	return 0;
}
