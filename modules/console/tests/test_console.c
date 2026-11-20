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
 * fallback log is served, the page has its Hellion tab and /api/links says
 * what --hellion gave it (and its default without it). The Fly view (#2):
 * the page has its Setup/Fly switch and Fly's controls; /api/links says what
 * --preview gave; /api/fly reads safety.state and nav.depth (the real
 * hangar-safety module, and a talker as the depth); /api/call goes through
 * the manager to safety: a disarm answered ok, an arm refused by safety's
 * checklist with its reason verbatim, an e-stop that puts safety in
 * failsafe; a call the console does not make is refused, one to a module
 * that is not there is the manager's refusal; the console idles at under 5 %
 * of a core once its calls are done. The view presets (#2, docs/11 B3): with
 * the real hangar-vision running, /api/fly reads vision.params and
 * vision.set_params {"preset":"edges"} changes it, an unknown preset is
 * vision's refusal verbatim, a body other than one preset is refused by the
 * console. Under 30 s. */
#include "hd_test.h"
#include "http.h"

#include <netinet/in.h>

static pid_t mgr;
static int port;
static char hostport[64];
static const char *T_console, *T_safety, *T_vision;

#define HELLION_TEST_URL "http://127.0.0.1:9/" /* nothing listens there; the console only passes it on */
#define PREVIEW_TEST_URL "http://127.0.0.1:9/stream?src=pilot"

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

/* One request; the status, the body in out. POSTs carry `json` ("{}" when
 * NULL) as JSON. */
static int api_json(const char *method, const char *path, const char *json, char *out, size_t cap)
{
	size_t len = 0;
	char err[200] = "";
	int post = strcmp(method, "POST") == 0;
	if (post && !json)
		json = "{}";
	int st = http_call(hostport, method, path, post ? json : NULL, post ? strlen(json) : 0, out, cap, &len, 5000,
			   err, sizeof err);
	if (st < 0)
		printf("    %s %s: %s\n", method, path, err);
	return st;
}

static int api(const char *method, const char *path, char *out, size_t cap)
{
	return api_json(method, path, NULL, out, cap);
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
static char raw_head[1024]; /* the reply's first kilobyte, whole: its headers */
static const char *raw(const char *bytes, size_t len)
{
	static char reply[1024];
	reply[0] = 0;
	raw_head[0] = 0;
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
	memcpy(raw_head, reply, sizeof raw_head);
	char *nl = strchr(reply, '\r');
	if (nl)
		*nl = 0;
	return reply;
}

static char body[65536];

static void test_start(void)
{
	T_console = getenv("DAWN_PATROLD_BIN");
	T_safety = getenv("SAFETY_BIN");
	T_vision = getenv("VISION_BIN");
	if (!T_console || !T_safety || !T_vision) {
		printf("    DAWN_PATROLD_BIN, SAFETY_BIN and VISION_BIN must be set\n");
		exit(2);
	}
	port = free_port();
	CHECK(port > 0);
	tfmt(hostport, sizeof hostport, "127.0.0.1:%d", port);
	/* The types the real safety module needs beyond the harness's own. */
	const char *src = getenv("HANGAR_MSG_SRC");
	const char *more[] = { "safety_params", "check_rep", "vision_params", NULL };
	for (int i = 0; more[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src ? src : "msg", more[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, more[i]);
		CHECK_EQ_INT(copy_file(from, to), 0);
	}
	manifest("talker", "[Module]\nDescription=toy talker\nExec=%s/talker --topic test.depth --rate 50\n"
			   "[Publish test.depth]\nType=depth\nRate=50\n", T_toys);
	manifest("navdepth", "[Module]\nDescription=toy talker as the depth\nExec=%s/talker --topic nav.depth --rate 10\n"
			     "[Publish nav.depth]\nType=depth\nRate=10\n", T_toys);
	/* The real safety module, without an autopilot (no [Call mavlink.arm]):
	 * its checklist wants mux and mavlink ready, neither of which runs here,
	 * so an arm is refused with its reason. */
	manifest("safety", "[Module]\nDescription=Vehicle state\nExec=%s\n"
			   "[Subscribe hangar.vehicle.modules]\n[Subscribe hangar.vehicle.system]\n[Subscribe vehicle.leak]\n"
			   "[Subscribe power.battery]\n[Subscribe nav.depth]\n[Subscribe ardusub.state]\n[Subscribe mux.params]\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Params]\nType=safety_params\nAllow=admin\n"
			   "[Serve arm]\nRequest=empty\nReply=empty\nAllow=admin\n"
			   "[Serve disarm]\nRequest=empty\nReply=empty\nAllow=* admin\n"
			   "[Serve estop]\nRequest=empty\nReply=empty\nAllow=* admin\n"
			   "[Serve clear]\nRequest=empty\nReply=empty\nAllow=admin\n"
			   "[Serve check]\nRequest=empty\nReply=check_rep\nAllow=* admin\n",
		 T_safety);
	/* The real vision module, with no frames: its parameters are what the
	 * Fly view's presets set (vision.set_params). */
	manifest("vision", "[Module]\nDescription=The pilot's picture\nExec=%s\n[Subscribe cam.decoded]\n"
			   "[Publish cam.pilot]\nType=frame\nSlots=4\nInputs=cam.decoded\n"
			   "[Params]\nType=vision_params\nAllow=admin\n",
		 T_vision);
	manifest("console", "[Module]\nDescription=Dawn Patrol: the operator's page\n"
			    "Exec=%s --listen 127.0.0.1:%d --hellion " HELLION_TEST_URL " --preview " PREVIEW_TEST_URL "\n"
			    "[Subscribe hangar.vehicle.system]\n[Subscribe hangar.vehicle.modules]\n[Subscribe *]\n[Subscribe vision.params]\n"
			    "[Health]\nWatchdog=10s\n", T_console, port);
	mgr = manager_start();
	CHECK(wait_state("talker", "ready", 5000));
	CHECK(wait_state("safety", "ready", 5000));
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
	CHECK(strstr(body, "<script") != NULL && strstr(body, " src=") == NULL); /* self-contained */
	CHECK(strstr(body, "http://") == NULL && strstr(body, "https://") == NULL);  /* nothing external */
	// The tabs, and the Hellion one with its frame; the page asks /api/links
	// for the URL instead of carrying one.
	CHECK(strstr(body, "data-tab=\"modules\"") != NULL && strstr(body, "data-tab=\"topics\"") != NULL);
	CHECK(strstr(body, "data-tab=\"log\"") != NULL && strstr(body, "data-tab=\"hellion\"") != NULL);
	CHECK(strstr(body, "<iframe id=\"hellion-frame\"") != NULL);
	CHECK(strstr(body, "/api/links") != NULL);
	CHECK(strstr(body, "localStorage") != NULL);
	CHECK_EQ_INT(api("GET", "/api/links", body, sizeof body), 200);
	CHECK(strstr(body, "{\"hellion\":\"" HELLION_TEST_URL "\",\"preview\":\"" PREVIEW_TEST_URL "\"}") != NULL);
	CHECK_EQ_INT(api("POST", "/api/links", body, sizeof body), 405);
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
	const char *r = RAW("POST /api/module/talker/restart HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
	CHECK(strncmp(r, "HTTP/1.1 403", 12) == 0);
	struct mstat m;
	CHECK(module_status("console", &m) && strcmp(m.state, "ready") == 0);
	CHECK_EQ_INT(api("GET", "/api/system", body, sizeof body), 200);
}

/* Who the browser thinks it is talking to. A page of evil.example whose name
 * was re-pointed at 127.0.0.1 (DNS rebinding) is same-origin with the console
 * for the browser, JSON content type and all; what gives it away is the Host
 * it still sends. A page of another origin that knows the address gives itself
 * away by its Origin. And no reply may be framed. */
#define GET_AS(path, host) RAW("GET " path " HTTP/1.1\r\n" host "\r\n")
#define JSON_POST(path, more) \
	RAW("POST " path " HTTP/1.1\r\n" more "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}")
#define IS(reply, status) (strncmp(reply, "HTTP/1.1 " status, 12) == 0)
static void test_host_and_origin(void)
{
	struct mstat before, m;
	CHECK(module_status("talker", &before) && before.pid > 0);
	// A name as Host: nothing is served, not the page, not a reading, not a button.
	CHECK(IS(GET_AS("/", "Host: evil.example:8080\r\n"), "403"));
	CHECK(strstr(raw_head, "Host must be an IP address or localhost") != NULL);
	CHECK(IS(GET_AS("/api/fly", "Host: evil.example\r\n"), "403"));
	CHECK(IS(JSON_POST("/api/module/talker/restart", "Host: evil.example:8080\r\nOrigin: http://evil.example:8080\r\n"), "403"));
	CHECK(IS(JSON_POST("/api/call/safety.estop", "Host: evil.example:8080\r\n"), "403"));
	// Names that only look like addresses, and no Host at all.
	CHECK(IS(GET_AS("/api/system", "Host: 127.0.0.1.evil.example\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", "Host: localhost.evil.example:80\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", "Host: 127.0.0.1:80:80\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", "Host: 127.0.0.1@evil.example\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", "Host: 127.0.0.1:\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", "Host: [::1\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", "Host: [evil.example]:80\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", "Host:\r\n"), "403"));
	CHECK(IS(GET_AS("/api/system", ""), "403"));
	CHECK(IS(GET_AS("/api/system", "X-Host: 127.0.0.1\r\n"), "403"));
	// An address or localhost, with any port (an SSH forward's is not the
	// console's own) or none, in any case.
	CHECK(IS(GET_AS("/api/system", "Host: 127.0.0.1:18080\r\n"), "200"));
	CHECK(IS(GET_AS("/api/system", "host:   LocalHost:18080  \r\n"), "200"));
	CHECK(IS(GET_AS("/api/system", "Host: localhost\r\n"), "200"));
	CHECK(IS(GET_AS("/api/system", "Host: 192.168.2.2:8080\r\n"), "200"));
	CHECK(IS(GET_AS("/api/system", "Host: [::1]:8080\r\n"), "200"));
	// A POST from another origin: another site, another port of this machine,
	// a sandboxed frame ("null"). The console's own page passes, and so does
	// a client that sends no Origin (curl, the suite's other requests).
	CHECK(IS(JSON_POST("/api/module/nobody/restart", "Host: 127.0.0.1:8080\r\nOrigin: http://evil.example\r\n"), "403"));
	CHECK(strstr(raw_head, "another origin") != NULL);
	CHECK(IS(JSON_POST("/api/module/nobody/restart", "Host: 127.0.0.1:8080\r\nOrigin: http://127.0.0.1:9000\r\n"), "403"));
	CHECK(IS(JSON_POST("/api/module/nobody/restart", "Host: 127.0.0.1:8080\r\nOrigin: http://127.0.0.1:8080.evil.example\r\n"), "403"));
	CHECK(IS(JSON_POST("/api/module/nobody/restart", "Host: 127.0.0.1:8080\r\nOrigin: 127.0.0.1:8080\r\n"), "403"));
	CHECK(IS(JSON_POST("/api/call/safety.estop", "Host: localhost:8080\r\nOrigin: null\r\n"), "403"));
	CHECK(IS(JSON_POST("/api/module/nobody/restart", "Host: 127.0.0.1:8080\r\nOrigin: http://127.0.0.1:8080\r\n"),
		 "409")); /* through to the manager: no module nobody */
	CHECK(IS(JSON_POST("/api/module/nobody/restart", "Host: LOCALHOST:18080\r\nOrigin: http://localhost:18080\r\n"), "409"));
	CHECK(IS(JSON_POST("/api/module/nobody/restart", "Host: 127.0.0.1:8080\r\n"), "409"));
	// A GET with a foreign Origin changes nothing and is answered (an <img>
	// sends one blind anyway); no reply says another origin may read it.
	CHECK(IS(GET_AS("/api/system", "Host: 127.0.0.1\r\nOrigin: http://evil.example\r\n"), "200"));
	CHECK(strstr(raw_head, "Access-Control-") == NULL);
	// No reply may be framed: the page, a reading, a refusal.
	CHECK(IS(GET_AS("/", "Host: 127.0.0.1\r\n"), "200"));
	CHECK(strstr(raw_head, "\r\nX-Frame-Options: DENY\r\n") != NULL);
	CHECK(strstr(raw_head, "\r\nContent-Security-Policy: frame-ancestors 'none'\r\n") != NULL);
	CHECK(IS(GET_AS("/api/fly", "Host: 127.0.0.1\r\n"), "200"));
	CHECK(strstr(raw_head, "\r\nX-Frame-Options: DENY\r\n") != NULL);
	CHECK(IS(GET_AS("/", "Host: evil.example\r\n"), "403"));
	CHECK(strstr(raw_head, "frame-ancestors 'none'") != NULL);
	// None of the refused requests reached the manager.
	CHECK(module_status("talker", &m) && m.pid == before.pid);
	CHECK(module_status("console", &m) && strcmp(m.state, "ready") == 0);
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

/* dawn-patrold by hand (unwired), as the README's "Running it" does, with
 * the arguments given; its pid, or the exit status (>= 0) when it ends
 * within a second, as for a refused argument. */
static int run_console(pid_t *pid, const char *a1, const char *a2, const char *a3, const char *a4)
{
	*pid = fork();
	if (*pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 2);
		}
		execl(T_console, T_console, a1, a2, a3, a4, (char *)NULL);
		_exit(127);
	}
	return wait_pid_ms(*pid, 1000);
}

static void test_hellion_default(void)
{
	// Without --hellion the URL is the vehicle's hellion on this machine, its
	// "control" port 8091 (legacy/hellion/examples/b-rov.json).
	int port2 = free_port();
	CHECK(port2 > 0);
	char listen[32];
	tfmt(listen, sizeof listen, "127.0.0.1:%d", port2);
	pid_t pid;
	CHECK_EQ_INT(run_console(&pid, "--listen", listen, "--role", "vehicle"), -1); /* still running */
	char saved[64];
	memcpy(saved, hostport, sizeof saved);
	tfmt(hostport, sizeof hostport, "%s", listen);
	CHECK(wait_api("/api/links", "{\"hellion\":\"http://127.0.0.1:8091/\",\"preview\":\"\"}", 5000, body, sizeof body));
	CHECK_EQ_INT(api("GET", "/api/system", body, sizeof body), 200);
	CHECK(strstr(body, "\"role\":\"vehicle\"") != NULL);
	memcpy(hostport, saved, sizeof saved);
	kill(pid, SIGTERM);
	CHECK_EQ_INT(wait_pid_ms(pid, 3000), 0);
	// A value that is not an http(s) URL, or none at all, is refused at start.
	CHECK_EQ_INT(run_console(&pid, "--listen", listen, "--hellion", "ftp://127.0.0.1/"), 2);
	CHECK_EQ_INT(run_console(&pid, "--listen", listen, "--hellion", "http://x/\"><script>"), 2);
	CHECK_EQ_INT(run_console(&pid, "--listen", listen, "--hellion", NULL), 2);
	CHECK_EQ_INT(run_console(&pid, "--listen", listen, "--preview", "file:///etc/passwd"), 2);
	CHECK_EQ_INT(run_console(&pid, "--listen", listen, "--preview", "http://x/\"onerror=\"x"), 2);
	// The module under the manager is untouched by all that.
	CHECK_EQ_INT(api("GET", "/api/links", body, sizeof body), 200);
	CHECK(strstr(body, HELLION_TEST_URL) != NULL);
}

/* A connection closed by one event of an epoll batch and named by a later
 * one of the same batch. An idle connection (a browser's spare one) is swept
 * at its 10 s deadline by the 1 s tick; when the client's bytes arrive in the
 * same wake-up, conn_event ran on the struct the sweep had freed (a read
 * after free, then a second close and free: the plain binary aborted in
 * free()). The two are put into one batch by stopping the console across the
 * deadline: the timer expires while it is stopped, the bytes are sent after
 * that, and on SIGCONT epoll_wait returns both, the timer first. No timing
 * is tight: a slow machine only lengthens the stop. A console run by hand,
 * so no watchdog restarts the stopped process. */
static void test_close_and_event_in_one_batch(void)
{
	int port2 = free_port();
	CHECK(port2 > 0);
	char listen[32];
	tfmt(listen, sizeof listen, "127.0.0.1:%d", port2);
	pid_t pid;
	CHECK_EQ_INT(run_console(&pid, "--listen", listen, "--role", "vehicle"), -1); /* still running */
	char saved[64];
	memcpy(saved, hostport, sizeof saved);
	tfmt(hostport, sizeof hostport, "%s", listen);
	CHECK(wait_api("/api/links", "\"hellion\"", 5000, body, sizeof body));
	int idle[3];
	for (int i = 0; i < 3; i++) {
		idle[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = htons((uint16_t)port2);
		CHECK(idle[i] >= 0 && connect(idle[i], (struct sockaddr *)&sa, sizeof sa) == 0);
	}
	// A request answered after them: the console has accepted all three (it
	// accepts until the queue is empty), so their deadlines are running.
	CHECK_EQ_INT(api("GET", "/api/system", body, sizeof body), 200);
	kill(pid, SIGSTOP);
	sleep_ms(11500); /* past the 10 s deadline, and the tick has fired */
	static const char req[] = "GET /api/system HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
	for (int i = 0; i < 3; i++)
		CHECK(send(idle[i], req, sizeof req - 1, MSG_NOSIGNAL) == (ssize_t)(sizeof req - 1));
	kill(pid, SIGCONT);
	// The swept connections get nothing; the console is alive and serves.
	CHECK_EQ_INT(wait_pid_ms(pid, 1500), -1);
	CHECK_EQ_INT(api("GET", "/api/system", body, sizeof body), 200);
	CHECK(strstr(body, "\"role\":\"vehicle\"") != NULL);
	for (int i = 0; i < 3; i++)
		close(idle[i]);
	CHECK_EQ_INT(api("GET", "/api/system", body, sizeof body), 200);
	memcpy(hostport, saved, sizeof saved);
	kill(pid, SIGTERM);
	CHECK_EQ_INT(wait_pid_ms(pid, 3000), 0);
}

/* The Fly view (#2): the page, the readings, and the calls through the
 * manager to the real safety module. */
static void test_fly(void)
{
	// The page: the Setup/Fly switch (Setup the default), the controls, the
	// presets disabled in the page as served (the script enables them while vision.params is there), the key legend.
	CHECK_EQ_INT(api("GET", "/", body, sizeof body), 200);
	CHECK(strstr(body, "data-view=\"setup\"") != NULL && strstr(body, "data-view=\"fly\"") != NULL);
	CHECK(strstr(body, "storedView() || 'setup'") != NULL);
	CHECK(strstr(body, "id=\"fly-estop\"") != NULL && strstr(body, "id=\"fly-arm\"") != NULL);
	CHECK(strstr(body, "id=\"fly-disarm\"") != NULL && strstr(body, "id=\"fly-record\"") != NULL);
	CHECK(strstr(body, "<button disabled data-preset=\"edges\"><kbd>3</kbd>Edges</button>") != NULL);
	CHECK(strstr(body, "/api/fly") != NULL && strstr(body, "/api/call/") != NULL);
	// The readings: safety.state from the real module, nav.depth from the
	// talker, a topic nobody publishes here as not present.
	CHECK(wait_api("/api/fly", "\"temp_c\":20", 5000, body, sizeof body)); /* nav.depth, attached and read */
	const char *s = strstr(body, "\"safety.state\":{");
	CHECK(s && strstr(s, "\"present\":true") != NULL);
	CHECK(s && strstr(s, "\"v\":{\"state\":0,") != NULL); /* disarmed */
	const char *d = strstr(body, "\"nav.depth\":{");
	CHECK(d && strstr(d, "\"present\":true") != NULL && strstr(d, "\"temp_c\":20") != NULL);
	CHECK(strstr(body, "\"ardusub.state\":{\"present\":false}") != NULL);
	CHECK(strstr(body, "\"video.index\":{\"present\":false}") != NULL);
	// A disarm, answered ok through the manager's call path.
	CHECK_EQ_INT(api("POST", "/api/call/safety.disarm", body, sizeof body), 200);
	CHECK(strstr(body, "\"ok\":true,\"call\":\"safety.disarm\",\"status\":\"HG_OK\"") != NULL);
	// An arm, refused by safety's own checklist, the reason verbatim.
	CHECK_EQ_INT(api("POST", "/api/call/safety.arm", body, sizeof body), 409);
	CHECK(strstr(body, "\"ok\":false,\"call\":\"safety.arm\",\"status\":\"HG_E_REFUSED\"") != NULL);
	CHECK(strstr(body, "\"reason\":\"\"") == NULL);
	printf("    arm refused: %s", body);
	// An e-stop: answered, and safety goes to failsafe.
	CHECK_EQ_INT(api("POST", "/api/call/safety.estop", body, sizeof body), 200);
	CHECK(strstr(body, "\"status\":\"HG_OK\"") != NULL);
	CHECK(wait_api("/api/fly", "\"v\":{\"state\":2,", 3000, body, sizeof body));
	// Calls the console does not make, and one to a module that is not here.
	CHECK_EQ_INT(api("POST", "/api/call/talker.echo", body, sizeof body), 404);
	CHECK(strstr(body, "not a call the console makes") != NULL);
	CHECK_EQ_INT(api("POST", "/api/call/safety.check", body, sizeof body), 404);
	CHECK_EQ_INT(api("GET", "/api/call/safety.estop", body, sizeof body), 404);
	CHECK_EQ_INT(api("POST", "/api/call/recorder.start", body, sizeof body), 409);
	CHECK(strstr(body, "\"ok\":false,\"call\":\"recorder.start\",\"status\":\"refused by the manager\"") != NULL);
	// A call without the JSON content type (a cross-site form) is refused.
	const char *r = RAW("POST /api/call/safety.estop HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
	CHECK(strncmp(r, "HTTP/1.1 403", 12) == 0);
	// Its calls done, the console idles: the call timer stopped with them.
	struct mstat m;
	CHECK(module_status("console", &m) && m.pid > 0);
	long long t0 = th_cpu_ticks(m.pid);
	uint64_t a = now_ms();
	sleep_ms(1000);
	double pct = th_cpu_percent(t0, th_cpu_ticks(m.pid), (long)(now_ms() - a));
	printf("    console at rest: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 5);
	// safety, still running (in the failsafe the e-stop latched).
	CHECK(module_status("safety", &m) && strcmp(m.state, "ready") == 0);
}

/* `"overdue":` of one topic's object in /api/fly's body: 1, 0, or -1 when the
 * topic is not present. */
static int fly_overdue(const char *text, const char *name)
{
	char key[64];
	tfmt(key, sizeof key, "\"%s\":{", name);
	const char *s = strstr(text, key);
	const char *end = s ? strchr(s, '}') : NULL; /* of {"present":false}, or of its "v" */
	const char *o = s ? strstr(s, "\"overdue\":") : NULL;
	if (!o || !end || o > end)
		return -1;
	return strncmp(o + 10, "true", 4) == 0;
}

/* A safety.state older than its staleness bound is not the vehicle's state.
 * A writer that hangs keeps its topic open and unflagged, and the console
 * keeps the last message it read: /api/fly went on saying present, state 1,
 * and the page went on saying ARMED. The API now says `overdue` from the
 * message's age against the topic's stale_after, and the page shows such a
 * state as unknown with Arm disabled (Disarm and E-STOP are never disabled). */
static void test_fly_stale_state(void)
{
	struct mstat m;
	CHECK(module_status("safety", &m) && m.pid > 0);
	CHECK_EQ_INT(api("GET", "/api/fly", body, sizeof body), 200);
	const char *s = strstr(body, "\"safety.state\":{");
	CHECK(s && strstr(s, "\"stale_after_ms\":300,") != NULL); /* Rate=10: three periods */
	// Fresh while safety publishes (a loaded machine may need a second look).
	uint64_t t0 = now_ms();
	int od = -1;
	do {
		if (api("GET", "/api/fly", body, sizeof body) == 200)
			od = fly_overdue(body, "safety.state");
		if (od == 0)
			break;
		sleep_ms(50);
	} while (now_ms() - t0 < 5000);
	CHECK_EQ_INT(od, 0);
	CHECK_EQ_INT(fly_overdue(body, "ardusub.state"), -1);
	// safety hangs: within its bound (plus the poll) the reading is overdue,
	// still present, still carrying the last state for the page to name.
	kill(m.pid, SIGSTOP);
	t0 = now_ms();
	do {
		sleep_ms(100);
		if (api("GET", "/api/fly", body, sizeof body) == 200)
			od = fly_overdue(body, "safety.state");
	} while (od != 1 && now_ms() - t0 < 5000);
	kill(m.pid, SIGCONT);
	CHECK_EQ_INT(od, 1);
	s = strstr(body, "\"safety.state\":{");
	CHECK(s && strstr(s, "\"present\":true") != NULL && strstr(s, "\"v\":{\"state\":2,") != NULL);
	double age = 0;
	CHECK(json_num(body, "\"safety.state\":{", "\"age_ms\":", &age) == 0 && age > 300);
	// The talker beside it was never overdue; a topic published on change has no bound.
	CHECK_EQ_INT(fly_overdue(body, "nav.depth"), 0);
	// Continued (or restarted by the manager, had the stop been long): fresh again.
	t0 = now_ms();
	do {
		sleep_ms(100);
		if (api("GET", "/api/fly", body, sizeof body) == 200)
			od = fly_overdue(body, "safety.state");
	} while (od != 0 && now_ms() - t0 < 15000);
	CHECK_EQ_INT(od, 0);
	CHECK(wait_state("safety", "ready", 15000));
	// The page: the state is shown only when it is not overdue, Arm is
	// disabled as served and whenever the state is unknown, Disarm and
	// E-STOP are not.
	CHECK_EQ_INT(api("GET", "/", body, sizeof body), 200);
	CHECK(strstr(body, "function stateOverdue(s) { return !!s && (s.overdue ||") != NULL);
	CHECK(strstr(body, "if (s && stateOverdue(s)) {") != NULL);
	CHECK(strstr(body, "stateUnknown('STATE UNKNOWN (console unreachable)')") != NULL);
	CHECK(strstr(body, "<button id=\"fly-arm\" disabled ") != NULL);
	CHECK(strstr(body, "$('fly-arm').disabled = true;") != NULL);
	CHECK(strstr(body, "<button id=\"fly-disarm\" title=") != NULL);
	CHECK(strstr(body, "$('fly-disarm').disabled") == NULL && strstr(body, "$('fly-estop').disabled") == NULL);
}

/* The Fly view's presets: vision.set_params with the preset alone. */
static void test_presets(void)
{
	CHECK(wait_state("vision", "ready", 5000));
	CHECK(wait_api("/api/fly", "\"preset\":\"normal\"", 5000, body, sizeof body));
	const char *v = strstr(body, "\"vision.params\":{");
	CHECK(v && strstr(v, "\"present\":true") != NULL);
	CHECK_EQ_INT(api_json("POST", "/api/call/vision.set_params", "{\"preset\":\"edges\"}", body, sizeof body), 200);
	CHECK(strstr(body, "\"ok\":true,\"call\":\"vision.set_params\",\"status\":\"HG_OK\"") != NULL);
	if (!strstr(body, "\"ok\":true"))
		printf("    %s", body);
	CHECK(wait_api("/api/fly", "\"preset\":\"edges\"", 3000, body, sizeof body));
	// vision's own refusal, verbatim; the preset in force unchanged.
	CHECK_EQ_INT(api_json("POST", "/api/call/vision.set_params", "{\"preset\":\"nosuch\"}", body, sizeof body),
		     409);
	CHECK(strstr(body, "no preset nosuch") != NULL);
	CHECK(wait_api("/api/fly", "\"preset\":\"edges\"", 3000, body, sizeof body));
	// The console sets the preset and nothing else.
	CHECK_EQ_INT(api("POST", "/api/call/vision.set_params", body, sizeof body), 400);
	CHECK_EQ_INT(api_json("POST", "/api/call/vision.set_params", "{\"preset\":\"Edges!\"}", body, sizeof body), 400);
	CHECK_EQ_INT(api_json("POST", "/api/call/vision.set_params", "{\"stage_count\":3}", body, sizeof body), 400);
	CHECK_EQ_INT(api_json("POST", "/api/call/vision.set_params", "{\"preset\":\"auto\"}", body, sizeof body), 200);
	CHECK(wait_api("/api/fly", "\"preset\":\"auto\"", 3000, body, sizeof body));
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
	RUN_TEST(test_host_and_origin);
	RUN_TEST(test_restart_stop_start);
	RUN_TEST(test_hellion_default);
	RUN_TEST(test_close_and_event_in_one_batch);
	RUN_TEST(test_fly);
	RUN_TEST(test_fly_stale_state);
	RUN_TEST(test_presets);
	RUN_TEST(test_log);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
