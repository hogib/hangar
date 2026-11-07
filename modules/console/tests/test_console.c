// SPDX-License-Identifier: GPL-3.0-or-later
/* Dawn Patrol under a manager, as an ordinary user under $HANGAR_ROOT: a
 * hangard with the toy talker and the console as a module of its own (the
 * manifest's [Subscribe *] lets it open any topic; it reports ready through
 * the page), driven over HTTP with lib/net's client: /api/modules names the
 * talker as ready, /api/topics shows its topic with a measured rate,
 * /api/topic/test.depth decodes its message, / is the page, a wrong path is
 * 404, malformed requests get 400 and leave the module running, a POST
 * without the JSON content type is refused, a restart through the control
 * socket raises the console's restart count, stop and start work, the
 * fallback log is served. Under 30 s. */
#include "hd_test.h"
#include "http.h"

#include <netinet/in.h>

static pid_t mgr;
static int port;
static char hostport[64];
static const char *T_console;

/* A port nobody listens on right now (the console binds it a moment later). */
static int free_port(void)
{
	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0)
		return -1;
	socklen_t sl = sizeof sa;
	getsockname(s, (struct sockaddr *)&sa, &sl);
	close(s);
	return ntohs(sa.sin_port);
}

/* One request; the status, the body in out. POSTs carry "{}" as JSON. */
static int api(const char *method, const char *path, char *out, size_t cap)
{
	size_t len = 0;
	char err[200] = "";
	int post = strcmp(method, "POST") == 0;
	int st = http_call(hostport, method, path, post ? "{}" : NULL, post ? 2 : 0, out, cap, &len, 5000, err,
			   sizeof err);
	if (st < 0)
		printf("    %s %s: %s\n", method, path, err);
	return st;
}

/* The number after `key` in the object that starts at the first `anchor`
 * found in body (a JSON fragment search, not a parser). */
static int json_num(const char *body, const char *anchor, const char *key, double *out)
{
	const char *p = anchor ? strstr(body, anchor) : body;
	if (!p)
		return -1;
	const char *end = strchr(p, '}');
	const char *k = strstr(p, key);
	if (!k || (end && k > end))
		return -1;
	k += strlen(key);
	if (strncmp(k, "null", 4) == 0)
		return -2;
	*out = strtod(k, NULL);
	return 0;
}

/* Polls `path` until body holds needle; 1 if it did within the time. */
static int wait_api(const char *path, const char *needle, long timeout_ms, char *out, size_t cap)
{
	uint64_t t0 = now_ms();
	do {
		if (api("GET", path, out, cap) == 200 && strstr(out, needle))
			return 1;
		sleep_ms(100);
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

/* Sends raw bytes and returns the first line of whatever comes back. */
#define RAW(lit) raw(lit, sizeof lit - 1)
static const char *raw(const char *bytes, size_t len)
{
	static char reply[256];
	reply[0] = 0;
	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons((uint16_t)port);
	struct timeval tv = { 3, 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(s);
		return "connect failed";
	}
	if (len)
		(void)!send(s, bytes, len, MSG_NOSIGNAL);
	else
		shutdown(s, SHUT_WR);
	ssize_t n = recv(s, reply, sizeof reply - 1, 0);
	close(s);
	reply[n > 0 ? n : 0] = 0;
	char *nl = strchr(reply, '\r');
	if (nl)
		*nl = 0;
	return reply;
}

static char body[65536];

static void test_start(void)
{
	T_console = getenv("DAWN_PATROLD_BIN");
	if (!T_console) {
		printf("    DAWN_PATROLD_BIN is not set\n");
		exit(2);
	}
	port = free_port();
	CHECK(port > 0);
	tfmt(hostport, sizeof hostport, "127.0.0.1:%d", port);
	manifest("talker", "[Module]\nDescription=toy talker\nExec=%s/talker --topic test.depth --rate 50\n"
			   "[Publish test.depth]\nType=depth\nRate=50\n", T_toys);
	manifest("console", "[Module]\nDescription=Dawn Patrol: the operator's page\n"
			    "Exec=%s --listen 127.0.0.1:%d\n"
			    "[Subscribe hangar.vehicle.system]\n[Subscribe hangar.vehicle.modules]\n[Subscribe *]\n"
			    "[Health]\nWatchdog=10s\n", T_console, port);
	mgr = manager_start();
	CHECK(wait_state("talker", "ready", 5000));
	// The console is a module like any other: ready by hg_ready, its status
	// line says where it listens.
	CHECK(wait_state("console", "ready", 5000));
	struct mstat m;
	CHECK(module_status("console", &m) && m.pid > 0);
	char expect[96];
	tfmt(expect, sizeof expect, "serving http://127.0.0.1:%d (role vehicle)", port);
	CHECK(strcmp(m.status, expect) == 0);
	if (strcmp(m.status, expect) != 0)
		printf("    status line: %s\n", m.status);
}

static void test_modules_and_system(void)
{
	CHECK(wait_api("/api/modules", "\"name\":\"talker\",\"state\":\"ready\"", 5000, body, sizeof body));
	CHECK(strstr(body, "\"name\":\"console\",\"state\":\"ready\"") != NULL);
	CHECK(strstr(body, "\"status\":\"publishing\"") != NULL);
	CHECK(strstr(body, "\"role\":\"vehicle\"") != NULL);
	double v;
	CHECK(json_num(body, "\"name\":\"talker\"", "\"restarts\":", &v) == 0 && v == 0);
	CHECK(json_num(body, "\"name\":\"talker\"", "\"failures\":", &v) == 0 && v == 0);
	struct mstat m;
	module_status("talker", &m);
	CHECK(json_num(body, "\"name\":\"talker\"", "\"pid\":", &v) == 0 && (int)v == m.pid);
	CHECK_EQ_INT(api("GET", "/api/system", body, sizeof body), 200);
	CHECK(strstr(body, "\"present\":true") != NULL);
	CHECK(strstr(body, "\"state\":\"nominal\"") != NULL);
	CHECK(strstr(body, "\"reason\":\"\"") != NULL);
}

static void test_topics(void)
{
	CHECK_EQ_INT(api("GET", "/api/topics", body, sizeof body), 200);
	CHECK(strstr(body, "\"name\":\"test.depth\"") != NULL);
	CHECK(strstr(body, "\"name\":\"hangar.vehicle.modules\"") != NULL);
	CHECK(strstr(body, "\"name\":\"hangar.vehicle.system\"") != NULL);
	double v;
	CHECK(json_num(body, "\"name\":\"test.depth\"", "\"rate_hz\":", &v) == 0 && v == 50.0);
	CHECK(json_num(body, "\"name\":\"test.depth\"", "\"slots\":", &v) == 0 && v == 16);
	CHECK(json_num(body, "\"name\":\"test.depth\"", "\"payload_bytes\":", &v) == 0 && v == 16);
	CHECK(json_num(body, "\"name\":\"test.depth\"", "\"seq\":", &v) == 0 && v > 0);
	const char *t = strstr(body, "\"name\":\"test.depth\"");
	CHECK(t && strstr(t, "\"writer\":\"talker\"") != NULL);
	CHECK(t && strstr(t, "\"flags\":[]") != NULL);
	// The measured rate comes from write_seq deltas between the console's
	// ticks: a second or two after start it is there and near 50 Hz.
	uint64_t t0 = now_ms();
	int rc = -2;
	do {
		if (api("GET", "/api/topics", body, sizeof body) == 200)
			rc = json_num(body, "\"name\":\"test.depth\"", "\"measured_hz\":", &v);
		if (rc == 0)
			break;
		sleep_ms(200);
	} while (now_ms() - t0 < 4000);
	CHECK_EQ_INT(rc, 0);
	CHECK(rc == 0 && v > 20.0 && v < 80.0);
	if (rc == 0)
		printf("    measured %.1f Hz (declared 50)\n", v);
	// The type hash is the header's.
	unsigned char h[4096];
	CHECK_EQ_INT(topic_header("test.depth", h), 0);
	char hash[32];
	tfmt(hash, sizeof hash, "\"type_hash\":\"%016llx\"", (unsigned long long)h64(h, 64));
	CHECK(t && strstr(t, hash) != NULL);
}

static void test_topic_decode(void)
{
	CHECK_EQ_INT(api("GET", "/api/topic/test.depth", body, sizeof body), 200);
	CHECK(strstr(body, "\"topic\":\"test.depth\"") != NULL);
	CHECK(strstr(body, "\"present\":true") != NULL);
	CHECK(strstr(body, "\"writer\":\"talker\"") != NULL);
	CHECK(strstr(body, "\"name\":\"depth_m\",\"type\":\"f32\",\"count\":1,\"comment\":\"m, positive down\"") != NULL);
	CHECK(strstr(body, "\"name\":\"temp_c\"") != NULL);
	CHECK(strstr(body, "\"name\":\"pressure_mbar\"") != NULL);
	double v;
	CHECK(json_num(body, "\"name\":\"temp_c\"", "\"value\":", &v) == 0 && v == 20.0);
	CHECK(json_num(body, "\"name\":\"pressure_mbar\"", "\"value\":", &v) == 0 && v == 1013.0);
	CHECK(json_num(body, NULL, "\"seq\":", &v) == 0 && v > 0);
	CHECK(strstr(body, "\"stale\":false") != NULL);
	// The manager's own topics decode by their schema too.
	CHECK_EQ_INT(api("GET", "/api/topic/hangar.vehicle.system", body, sizeof body), 200);
	CHECK(strstr(body, "\"name\":\"state\",\"type\":\"u8\",\"count\":1,\"comment\":\"0 nominal, 1 degraded, 2 failsafe\",\"value\":0") != NULL);
	CHECK(strstr(body, "\"name\":\"reason\",\"type\":\"char\",\"count\":64") != NULL);
	// A name that is not a topic, and one that is valid but absent.
	CHECK_EQ_INT(api("GET", "/api/topic/no.such.topic", body, sizeof body), 404);
	CHECK_EQ_INT(api("GET", "/api/topic/Not-Valid", body, sizeof body), 404);
	CHECK_EQ_INT(api("GET", "/api/topic/", body, sizeof body), 404);
}

static void test_page_and_errors(void)
{
	CHECK_EQ_INT(api("GET", "/", body, sizeof body), 200);
	CHECK(strstr(body, "<title>Dawn Patrol</title>") != NULL);
	CHECK(strstr(body, "/api/modules") != NULL);
	CHECK(strstr(body, "/api/topic/") != NULL);
	CHECK(strstr(body, "<script") != NULL && strstr(body, "src=") == NULL); /* self-contained */
	CHECK_EQ_INT(api("GET", "/nope", body, sizeof body), 404);
	CHECK(strstr(body, "no such page") != NULL);
	CHECK_EQ_INT(api("GET", "/api/nope", body, sizeof body), 404);
	CHECK_EQ_INT(api("GET", "/api/module/talker/restart", body, sizeof body), 404);
	CHECK_EQ_INT(api("DELETE", "/api/modules", body, sizeof body), 405);
	CHECK_EQ_INT(api("POST", "/api/modules", body, sizeof body), 405);
	// Malformed requests: refused, and the module goes on serving.
	CHECK(strncmp(RAW("GARBAGE\r\n\r\n"), "HTTP/1.1 400", 12) == 0);
	CHECK(strncmp(RAW("GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"), "HTTP/1.1 400", 12) == 0);
	CHECK(strncmp(RAW("GET / HTTP/1.1\nHost: x\n\n"), "HTTP/1.1 400", 12) == 0); /* bare LF */
	CHECK(strncmp(RAW("GET /\0 HTTP/1.1\r\n\r\n"), "HTTP/1.1 400", 12) == 0);
	CHECK(strncmp(RAW("GET / HTTP/1.1\r\nContent-Length: 999999\r\n\r\n"), "HTTP/1.1 413", 12) == 0);
	static char huge[9000];
	memset(huge, 'A', sizeof huge);
	memcpy(huge, "GET / HTTP/1.1\r\nX-Pad: ", 23);
	CHECK(strncmp(raw(huge, sizeof huge), "HTTP/1.1 431", 12) == 0);
	CHECK(strcmp(raw("", 0), "") == 0); /* a client that connects and hangs up */
	// A POST without the JSON content type (a cross-site form) is refused.
	const char *r = RAW("POST /api/module/talker/restart HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
	CHECK(strncmp(r, "HTTP/1.1 403", 12) == 0);
	struct mstat m;
	CHECK(module_status("console", &m) && strcmp(m.state, "ready") == 0);
	CHECK_EQ_INT(api("GET", "/api/system", body, sizeof body), 200);
}

static void test_restart_stop_start(void)
{
	struct mstat before;
	CHECK(module_status("talker", &before) && before.pid > 0);
	CHECK_EQ_INT(api("POST", "/api/module/talker/restart", body, sizeof body), 200);
	CHECK(strstr(body, "\"ok\":true") != NULL);
	CHECK(wait_new_pid("talker", before.pid, "ready", 5000));
	struct mstat after;
	module_status("talker", &after);
	// The console counted the pid change; the manager's failures stay 0
	// (a restart by hand clears them: hangard/README.md).
	char needle[64];
	tfmt(needle, sizeof needle, "\"pid\":%d,\"failures\":0,\"restarts\":1", after.pid);
	CHECK(wait_api("/api/modules", needle, 5000, body, sizeof body));
	// Refusals come back as such.
	CHECK_EQ_INT(api("POST", "/api/module/nobody/restart", body, sizeof body), 409);
	CHECK(strstr(body, "\"ok\":false") != NULL && strstr(body, "nobody") != NULL);
	CHECK_EQ_INT(api("POST", "/api/module/Bad-Name/restart", body, sizeof body), 404);
	CHECK_EQ_INT(api("POST", "/api/module/talker/explode", body, sizeof body), 404);
	CHECK_EQ_INT(api("POST", "/api/reload", body, sizeof body), 200);
	CHECK(strstr(body, "\"ok\":true") != NULL);
	// Stop, then start.
	CHECK_EQ_INT(api("POST", "/api/module/talker/stop", body, sizeof body), 200);
	CHECK(wait_state("talker", "stopped", 5000));
	CHECK(wait_api("/api/modules", "\"name\":\"talker\",\"state\":\"stopped\",\"state_code\":5,\"pid\":0", 5000, body,
		       sizeof body));
	const char *t = strstr(body, "\"name\":\"talker\"");
	CHECK(t && strstr(t, "\"last_exit\":\"exit:0\"") != NULL);
	CHECK_EQ_INT(api("POST", "/api/module/talker/start", body, sizeof body), 200);
	CHECK(wait_state("talker", "ready", 5000));
	CHECK(wait_api("/api/modules", "\"name\":\"talker\",\"state\":\"ready\"", 5000, body, sizeof body));
	double v;
	CHECK(json_num(body, "\"name\":\"talker\"", "\"restarts\":", &v) == 0 && v == 2);
}

static void test_log(void)
{
	// No journald socket under this root: the manager writes the fallback
	// files, and the talker said goodbye into its own at the restart.
	CHECK_EQ_INT(api("GET", "/api/log?lines=50", body, sizeof body), 200);
	CHECK(strstr(body, "\"journald\":false") != NULL);
	CHECK(strstr(body, "\"name\":\"talker\"") != NULL);
	CHECK(strstr(body, "\"name\":\"console\"") != NULL);
	CHECK(strstr(body, "SIGTERM after") != NULL);
	CHECK(strstr(body, "dawn-patrold: listening on") != NULL);
}

static void test_shutdown(void)
{
	CHECK_EQ_INT(manager_stop(mgr), 0);
	// The console went with the manager's shutdown, by its SIGTERM and not
	// by default action (the signal is blocked before the library's helper
	// thread exists): it logged its stop, and nothing listens now.
	CHECK(strcmp(raw("", 0), "connect failed") == 0);
	char log[1024];
	tfmt(log, sizeof log, "%s/run/hangar/log/console.log", T_root);
	CHECK(file_contains(log, "dawn-patrold: stopping"));
	const char *line = strstr(read_file(T_log), "console: pid ");
	CHECK(line != NULL);
	if (line) {
		const char *nl = strchr(line, '\n');
		CHECK(strstr(line, "exited with status 0") != NULL && (!nl || strstr(line, "exited with status 0") < nl));
		if (!strstr(line, "exited with status 0"))
			printf("    manager log: %.*s\n", nl ? (int)(nl - line) : 80, line);
	}
}

int main(void)
{
	hd_setup("console", NULL);
	RUN_TEST(test_start);
	RUN_TEST(test_modules_and_system);
	RUN_TEST(test_topics);
	RUN_TEST(test_topic_decode);
	RUN_TEST(test_page_and_errors);
	RUN_TEST(test_restart_stop_start);
	RUN_TEST(test_log);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
