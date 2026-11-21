// SPDX-License-Identifier: GPL-3.0-or-later
/* The bridge, end to end on loopback (docs/08-bridge.md): two managers, a
 * vehicle and a topside, each with its own root and topic directory, each
 * with a bridge; between the two bridges a UDP forwarder in this process,
 * which drops, replays, modifies and forges datagrams when told to, and
 * records what the topside sends so that a run can play it back later. No
 * HANGAR_PEER_TOPIC_DIR: only the bridge joins the two.
 *
 * test_bridge [--only <run>]: every run, or the one named (main, mismatch,
 * clocks, hold, replay, auto, writer, limits, beacon, peer, files, filestall,
 * filerekey).
 *
 * Takes HANGARD_BIN, HANGAR_BIN, HANGARD_TOYS, HANGAR_MSG_SRC and BRIDGE_BIN
 * from the environment (modules/bridge/bridge.mk sets them). */
#include "../../../hangard/tests/hd_test.h"

#include "blake2s.h"
#include "bridge_file.h"
#include "bridge_link.h"
#include "depth.h"
#include "hg_call.h"

/* hd_test.h brings the shared harness; this suite prints its own lines. */
static void unused_harness(void) __attribute__((unused));
static void unused_harness(void)
{
	(void)tests_run;
	(void)checks_failed;
	(void)current_test_failures;
	(void)current_test_name;
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

struct side {
	const char *role;
	char root[256], modules[512], msg[512], control[512], topics[512], log[512], conf[512];
	int port;
	pid_t mgr;
	const char *peer; /* Peer= of its bridge.conf; NULL: the forwarder */
	int dport;        /* DiscoveryPort=, for Peer=auto; 0: the default */
};
static struct side V = { .role = "vehicle" }, T = { .role = "topside" };
static const char *bridge_bin;
static char self_path[1024];

/* The hd_test.h helpers work on one root: point them at a side. */
static void use(const struct side *s)
{
	tfmt(T_root, sizeof T_root, "%s", s->root);
	tfmt(T_modules, sizeof T_modules, "%s", s->modules);
	tfmt(T_msg, sizeof T_msg, "%s", s->msg);
	tfmt(T_control, sizeof T_control, "%s", s->control);
	tfmt(T_topics, sizeof T_topics, "%s", s->topics);
	tfmt(T_log, sizeof T_log, "%s", s->log);
	setenv("HANGAR_ROOT", s->root, 1);
	setenv("HANGAR_TOPIC_DIR", s->topics, 1);
}

/* ---- The forwarder ------------------------------------------------------------ */

static int fwd_sock, fwd_port;
static struct sockaddr_in fwd_v, fwd_t;
static atomic_int fwd_drop_pct, fwd_stop, fwd_tamper; /* tamper: 1 replay, 2 modify, 3 forge, 4 and 5 the tag */
static atomic_long fwd_count;
static atomic_int fwd_drop_kind_v; /* a datagram kind (4 pong) not passed on to the vehicle; 0 none */
static atomic_long fwd_hello_v;    /* hellos the vehicle sent */

/* What the topside sent, in order, as someone on the tether would record it. */
#define REC_MAX 16384
struct rec {
	uint16_t len;
	unsigned char d[1300];
};
static struct rec *recs;
static atomic_int fwd_rec_on, fwd_nrec;

static void *forwarder(void *arg)
{
	(void)arg;
	unsigned rng = 12345;
	while (!atomic_load(&fwd_stop)) {
		unsigned char d[1500];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(fwd_sock, d, sizeof d, 0, (struct sockaddr *)&from, &fl);
		if (n <= 0)
			continue;
		const struct sockaddr_in *to = ntohs(from.sin_port) == V.port ? &fwd_t : &fwd_v;
		atomic_fetch_add(&fwd_count, 1);
		if (to == &fwd_t && n > 4 && d[4] == 1)
			atomic_fetch_add(&fwd_hello_v, 1);
		if (to == &fwd_v && atomic_load(&fwd_rec_on) && n <= (ssize_t)sizeof recs[0].d) {
			int k = atomic_load(&fwd_nrec);
			if (k < REC_MAX) {
				recs[k].len = (uint16_t)n;
				memcpy(recs[k].d, d, (size_t)n);
				atomic_store(&fwd_nrec, k + 1);
			}
		}
		if (to == &fwd_v && n > 4 && d[4] == atomic_load(&fwd_drop_kind_v))
			continue;
		rng = rng * 1103515245u + 12345u;
		if ((int)((rng >> 16) % 100) < atomic_load(&fwd_drop_pct))
			continue;
		int t = atomic_load(&fwd_tamper);
		if ((t == 4 || t == 5) && to == &fwd_v) {
			/* Passed on in place of the datagram itself, so that only its
			 * tag can refuse it: right in every byte but the last (4) or
			 * the first (5). */
			d[t == 4 ? n - 1 : n - 16] ^= 0x80;
			atomic_store(&fwd_tamper, 0);
		}
		sendto(fwd_sock, d, (size_t)n, 0, (const struct sockaddr *)to, sizeof *to);
		if (t >= 1 && t <= 3)
			atomic_store(&fwd_tamper, 0);
		if (t == 1) {
			sendto(fwd_sock, d, (size_t)n, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
		} else if (t == 2) {
			d[n - 20] ^= 1; /* the body's last byte */
			sendto(fwd_sock, d, (size_t)n, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
		} else if (t == 3) {
			/* A hello for pair test, tagged with the wrong key. */
			unsigned char h[24 + 72 + 16] = { 0 }, key[32];
			memset(key, 0x5a, sizeof key);
			memcpy(h, "HGB1", 4);
			h[4] = 1;
			uint32_t bl = 72;
			memcpy(h + 12, &bl, 4);
			h[16] = 1;
			h[24] = 2;
			memcpy(h + 24 + 8, "test", 4);
			memset(h + 24 + 40, 0x77, 16);
			blake2s(h + 24 + 72, 16, key, 32, h, 24 + 72);
			sendto(fwd_sock, h, sizeof h, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
		}
	}
	return NULL;
}

/* Plays recorded datagrams [from, to) to the vehicle, from the forwarder's
 * address (the peer the vehicle's bridge knows), gap_us apart. */
static void replay_to_vehicle(int from, int to, int gap_us)
{
	for (int i = from; i < to; i++) {
		sendto(fwd_sock, recs[i].d, recs[i].len, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
		if (gap_us)
			usleep((useconds_t)gap_us);
	}
}

static int udp_bound(int *port)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (struct sockaddr *)&a, sizeof a) < 0)
		return -1;
	socklen_t l = sizeof a;
	getsockname(s, (struct sockaddr *)&a, &l);
	*port = ntohs(a.sin_port);
	return s;
}

/* A free UDP port for a bridge. */
static int free_port(void)
{
	int p;
	int s = udp_bound(&p);
	close(s);
	return p;
}

/* ---- The two roots ------------------------------------------------------------ */

static void copy_msgs(const char *to)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	DIR *d = opendir(src && *src ? src : "msg");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 5 || strcmp(e->d_name + l - 4, ".msg") != 0)
			continue;
		char a[1024], b[1024];
		tfmt(a, sizeof a, "%s/%s", src && *src ? src : "msg", e->d_name);
		tfmt(b, sizeof b, "%s/%s", to, e->d_name);
		copy_file(a, b);
	}
	if (d)
		closedir(d);
}

static const char *MIRRORS = "[Calls]\nRemote=server.*\n"
			     "[Mirror test.depth]\nDirection=up\nExpectRate=100\nStaleAfter=100ms\n"
			     "[Mirror test.burst]\nDirection=up\nExpectRate=100\nStaleAfter=50ms\n"
			     "[Mirror test.log]\nDirection=up\nDelivery=reliable\n"
			     "[Mirror pilot.depth]\nDirection=down\nPriority=command\nExpectRate=50\n"
			     "StaleAfter=100ms\nMaxAge=200ms\n";

static void setup_side(struct side *s, const char *tag, const char *extra_conf)
{
	char base[256];
	const char *b = getenv("HANGAR_TEST_BASE");
	if (b && *b)
		tfmt(base, sizeof base, "%s", b);
	else
		tfmt(base, sizeof base, "/dev/shm/hangar-test-%ld", (long)getuid());
	tfmt(s->root, sizeof s->root, "%s/bridge-%s-%s-%ld", base, tag, s->role, (long)getpid());
	rm_tree(s->root);
	tfmt(s->modules, sizeof s->modules, "%s/etc/hangar/modules", s->root);
	tfmt(s->msg, sizeof s->msg, "%s/usr/share/hangar/msg", s->root);
	tfmt(s->control, sizeof s->control, "%s/run/hangar/control", s->root);
	tfmt(s->topics, sizeof s->topics, "%s/dev/shm/hangar", s->root);
	tfmt(s->log, sizeof s->log, "%s/hangard.log", s->root);
	tfmt(s->conf, sizeof s->conf, "%s/etc/hangar/bridge.conf", s->root);
	mkdir_p(s->modules);
	mkdir_p(s->msg);
	copy_msgs(s->msg);
	char p[1024], text[4096];
	tfmt(p, sizeof p, "%s/xdg", s->root);
	mkdir_p(p);
	tfmt(p, sizeof p, "%s/etc/hangar/hangard.conf", s->root);
	tfmt(text, sizeof text, "Role=%s\nHealthTick=50ms\n", s->role);
	write_file(p, text);
	tfmt(p, sizeof p, "%s/etc/hangar/bridge.key", s->root);
	write_file(p, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n");
	char peer[64], dport[64] = "";
	tfmt(peer, sizeof peer, "127.0.0.1:%d", fwd_port);
	if (s->dport)
		tfmt(dport, sizeof dport, "DiscoveryPort=%d\n", s->dport);
	tfmt(text, sizeof text, "[Bridge]\nPair=test\nKey=%s/etc/hangar/bridge.key\nPort=%d\nPeer=%s\n%sMsgDir=%s\n%s%s",
	     s->root, s->port, s->peer ? s->peer : peer, dport, s->msg, MIRRORS, extra_conf ? extra_conf : "");
	write_file(s->conf, text);
	use(s);
	manifest("bridge", "[Module]\nExec=%s %s\n[Bridge]\n", bridge_bin, s->conf);
}

static void start(struct side *s)
{
	use(s);
	setenv("XDG_RUNTIME_DIR", s->root, 1);
	s->mgr = manager_start();
}

static void stop_both(void)
{
	if (V.mgr > 0) {
		use(&V);
		manager_stop(V.mgr);
		V.mgr = 0;
	}
	if (T.mgr > 0) {
		use(&T);
		manager_stop(T.mgr);
		T.mgr = 0;
	}
	if (!getenv("HD_KEEP")) {
		rm_tree(V.root);
		rm_tree(T.root);
	}
}

/* ---- Reading the topics of a side ------------------------------------------- */

static int read_link(const struct side *s, struct bridge_link *out)
{
	use(s);
	char name[48];
	tfmt(name, sizeof name, "bridge.%s.link", s->role);
	hg_sub *sub = hg_sub_open(name, &BRIDGE_LINK_TYPE);
	if (!sub)
		return -1;
	struct hg_info in;
	int r = hg_latest(sub, out, sizeof *out, &in) == HG_MSG ? 0 : -1;
	hg_sub_close(sub);
	return r;
}

static int wait_link(const struct side *s, int want_match, long ms, struct bridge_link *out)
{
	uint64_t t0 = now_ms();
	do {
		/* config_match is false too while the versions are still on their
		 * way ("exchanging versions"): not the verdict waited for. */
		if (read_link(s, out) == 0 && out->state == 3 && out->config_match == want_match &&
		    (want_match || (out->detail[0] && strcmp(out->detail, "exchanging versions") != 0)))
			return 1;
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)ms);
	return 0;
}

static pid_t pid_of(const struct side *s, const char *module)
{
	use(s);
	struct mstat m;
	return module_status(module, &m) ? m.pid : -1;
}

/* ---- Another clock ------------------------------------------------------------ */

/* Two machines never share a CLOCK_MONOTONIC; two roots on one host do,
 * and a mistake in translating a time between them goes unseen. A time
 * namespace gives one side its own: a process started with
 * HANGAR_TEST_TIMENS=<seconds> in its environment (this binary: the suite
 * sets it for the topside's manager and for its tester) enters a user and a
 * time namespace whose CLOCK_MONOTONIC is that much ahead, and then runs
 * HANGAR_TEST_EXEC, or itself again. The offset takes effect at the exec. */
#define TIMENS_S 259200 /* three days: a laptop up since Monday, a Pi just powered */

static void put_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text))
		_exit(125);
	close(fd);
}

static void timens_enter(char **argv)
{
	char text[64], secs[32];
	tfmt(secs, sizeof secs, "%s", getenv("HANGAR_TEST_TIMENS"));
	unsetenv("HANGAR_TEST_TIMENS");
	uid_t uid = getuid();
	gid_t gid = getgid();
	if (unshare(CLONE_NEWUSER | CLONE_NEWTIME) < 0)
		_exit(125);
	put_file("/proc/self/setgroups", "deny");
	tfmt(text, sizeof text, "%u %u 1", (unsigned)uid, (unsigned)uid);
	put_file("/proc/self/uid_map", text);
	tfmt(text, sizeof text, "%u %u 1", (unsigned)gid, (unsigned)gid);
	put_file("/proc/self/gid_map", text);
	tfmt(text, sizeof text, "monotonic %s 0", secs);
	put_file("/proc/self/timens_offsets", text);
	const char *x = getenv("HANGAR_TEST_EXEC");
	if (x && *x) {
		char path[1024];
		tfmt(path, sizeof path, "%s", x);
		unsetenv("HANGAR_TEST_EXEC");
		execl(path, path, (char *)NULL);
	} else {
		execv("/proc/self/exe", argv);
	}
	_exit(125);
}

/* 1 if a child started that way reads a clock that much ahead of ours. */
static int timens_works(void)
{
	char now[32], secs[32];
	tfmt(now, sizeof now, "%llu", (unsigned long long)(hg_now_ns() / 1000000000u));
	tfmt(secs, sizeof secs, "%d", TIMENS_S);
	pid_t c = fork();
	if (c == 0) {
		setenv("HANGAR_TEST_TIMENS", secs, 1);
		unsetenv("HANGAR_TEST_EXEC");
		execl(self_path, self_path, "--clock-ahead-of", now, (char *)NULL);
		_exit(127);
	}
	return wait_pid_ms(c, 5000) == 0;
}

/* Starts a side's manager with its clock TIMENS_S ahead. */
static void start_ahead(struct side *s)
{
	char secs[32];
	tfmt(secs, sizeof secs, "%d", TIMENS_S);
	setenv("HANGAR_TEST_TIMENS", secs, 1);
	setenv("HANGAR_TEST_EXEC", T_hangard, 1);
	const char *real = T_hangard;
	T_hangard = self_path;
	start(s);
	T_hangard = real;
	unsetenv("HANGAR_TEST_TIMENS");
	unsetenv("HANGAR_TEST_EXEC");
}

/* ---- The tester: calls from the topside ---------------------------------------- */

/* Run as a fresh process (this binary again, --tester <topside root> <out>):
 * the suite has used lib/hg unwired already, and a module's wiring is read
 * once per process. */
static void tester_init(const char *root)
{
	tfmt(T.root, sizeof T.root, "%s", root);
	tfmt(T.control, sizeof T.control, "%s/run/hangar/control", root);
	tfmt(T.topics, sizeof T.topics, "%s/dev/shm/hangar", root);
	use(&T);
	char path[600];
	if (wiring_to_file("tester", path, sizeof path) < 0)
		_exit(2);
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", "tester", 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0)
		_exit(3);
}

static void tester(const char *root, const char *out_path)
{
	tester_init(root);
	FILE *f = fopen(out_path, "w");
	struct depth d = { .depth_m = 4.5f }, r;
	/* The first call waits for the request and reply topics to exist on
	 * both sides; the rest are timed. */
	hg_call_result res;
	uint64_t t0 = now_ms();
	do {
		res = hg_call("server.secret", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	} while (res.status != HG_OK && now_ms() - t0 < 8000);
	fprintf(f, "first %s %s\n", hg_status_name(res.status), res.reason);
	uint64_t rt[20];
	int ok = 0;
	for (int i = 0; i < 20; i++) {
		uint64_t a = hg_now_ns();
		res = hg_call("server.secret", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
		if (res.status == HG_OK && r.depth_m == 4.5f)
			rt[ok++] = hg_now_ns() - a;
	}
	for (int i = 0; i < ok; i++)
		for (int j = i + 1; j < ok; j++)
			if (rt[j] < rt[i]) {
				uint64_t x = rt[i];
				rt[i] = rt[j];
				rt[j] = x;
			}
	fprintf(f, "ok %d median_us %llu max_us %llu\n", ok, ok ? (unsigned long long)rt[ok / 2] / 1000 : 0,
		ok ? (unsigned long long)rt[ok - 1] / 1000 : 0);
	res = hg_call("server.adminonly", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	fprintf(f, "adminonly %s %s\n", hg_status_name(res.status), res.reason);
	fclose(f);
	_exit(0);
}

/* --tester-zero: server.secret echoes its request, so a request of zeros
 * gets an OK reply whose body is all zeros, as every method with Reply=empty
 * gives (safety.arm, disarm, clear): on the wire that reply ends inside its
 * envelope. Five of those, then five with a body. */
static void tester_zero(const char *root, const char *out_path)
{
	tester_init(root);
	FILE *f = fopen(out_path, "w");
	struct depth d = { .depth_m = 4.5f }, z = { 0 }, r;
	hg_call_result res;
	uint64_t t0 = now_ms();
	do {
		res = hg_call("server.secret", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 1000);
	} while (res.status != HG_OK && now_ms() - t0 < 8000);
	fprintf(f, "first %s %s\n", hg_status_name(res.status), res.reason);
	int zero_ok = 0, body_ok = 0;
	char last[160] = "";
	for (int i = 0; i < 5; i++) {
		res = hg_call("server.secret", &DEPTH_TYPE, &z, &DEPTH_TYPE, &r, 500);
		zero_ok += res.status == HG_OK;
		if (res.status != HG_OK)
			tfmt(last, sizeof last, "%s %s", hg_status_name(res.status), res.reason);
		res = hg_call("server.secret", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 500);
		body_ok += res.status == HG_OK && r.depth_m == 4.5f;
	}
	fprintf(f, "zero %d body %d last %s\n", zero_ok, body_ok, last);
	fclose(f);
	_exit(0);
}

/* --tester-one: one call, with 4 s to be answered in. */
static void tester_one(const char *root, const char *out_path)
{
	tester_init(root);
	FILE *f = fopen(out_path, "w");
	struct depth d = { .depth_m = 4.5f }, r;
	uint64_t t0 = now_ms();
	hg_call_result res = hg_call("server.secret", &DEPTH_TYPE, &d, &DEPTH_TYPE, &r, 4000);
	fprintf(f, "one %s after %llu ms %s\n", hg_status_name(res.status), (unsigned long long)(now_ms() - t0),
		res.reason);
	fclose(f);
	_exit(0);
}

/* Runs this binary again as the topside's tester; its output, or NULL. */
static const char *run_tester(const char *mode, int ahead, long timeout_ms)
{
	char out[600], secs[32];
	tfmt(out, sizeof out, "%s/tester.out", T.root);
	unlink(out);
	tfmt(secs, sizeof secs, "%d", TIMENS_S);
	pid_t c = fork();
	if (c == 0) {
		if (ahead)
			setenv("HANGAR_TEST_TIMENS", secs, 1);
		execl(self_path, self_path, mode, T.root, out, (char *)NULL);
		_exit(127);
	}
	return wait_pid_ms(c, timeout_ms) == 0 ? read_file(out) : NULL;
}

/* ---- The suite ---------------------------------------------------------------- */

static int failed;
#define EXPECT(c, ...)                                                                  \
	do {                                                                           \
		if (c) {                                                               \
			printf("ok    ");                                              \
		} else {                                                               \
			printf("FAIL  ");                                              \
			failed = 1;                                                    \
		}                                                                      \
		printf(__VA_ARGS__);                                                   \
		printf("\n");                                                          \
		fflush(stdout);                                                        \
	} while (0)

static void main_run(void)
{
	char text[2048];
	setup_side(&V, "main", NULL);
	manifest("talker", "[Module]\nExec=%s/talker --topic test.depth --rate 100\n"
			   "[Publish test.depth]\nType=depth\nRate=100\n", T_toys);
	manifest("burst", "[Module]\nExec=%s/talker --topic test.burst --rate 100 --stop-after 30\n"
			  "[Publish test.burst]\nType=depth\nRate=100\n", T_toys);
	manifest("logger", "[Module]\nExec=%s/talker --topic test.log --rate 200\n"
			   "[Publish test.log]\nType=depth\nRate=200\nSlots=256\n", T_toys);
	manifest("server", "[Module]\nExec=%s/server\n"
			   "[Serve secret]\nRequest=depth\nReply=depth\nAllow=topside:tester\n"
			   "[Serve adminonly]\nRequest=depth\nReply=depth\nAllow=admin topside:admin\n", T_toys);
	manifest("pilotread", "[Module]\nExec=%s/listener --topic pilot.depth\n[Subscribe pilot.depth]\n", T_toys);
	setup_side(&T, "main", NULL);
	/* The topside's clock runs 5 s ahead on the wire: the offset the vehicle
	 * measures, and every stamp and deadline it translates. */
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_OFFSET_NS=5000000000 %s %s\n[Bridge]\n",
		 bridge_bin, T.conf);
	manifest("pilot", "[Module]\nExec=%s/talker --topic pilot.depth --rate 50\n"
			  "[Publish pilot.depth]\nType=depth\nRate=50\n", T_toys);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Call server.secret]\nRequest=depth\nReply=depth\n"
			   "[Call server.adminonly]\nRequest=depth\nReply=depth\n");
	start(&V);
	start(&T);

	struct bridge_link lv, lt;
	uint64_t t0 = now_ms();
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	EXPECT(up, "link up, config_match on both sides (%llu ms from the managers' start)",
	      (unsigned long long)(now_ms() - t0));
	if (!up) {
		printf("    vehicle: state %u detail '%s'; topside: state %u detail '%s'\n", lv.state, lv.detail,
		       lt.state, lt.detail);
		return;
	}
	sleep_ms(1500); /* pings for the clock, the first messages through */

	/* Mirrors up: the sender's seqs and writer, its boot id. */
	use(&T);
	hg_sub *td = hg_sub_open("test.depth", &DEPTH_TYPE);
	struct hg_info in;
	struct depth d;
	int got = td && hg_latest(td, &d, sizeof d, &in) == HG_MSG;
	struct hg_topic_meta mt;
	int metaok = got && hg_sub_meta(td, &mt) == 0;
	uint64_t seq_t = in.seq;
	use(&V);
	hg_sub *vd = hg_sub_open("test.depth", &DEPTH_TYPE);
	struct hg_info inv;
	int gotv = vd && hg_latest(vd, &d, sizeof d, &inv) == HG_MSG;
	int boot_set = 0;
	for (int i = 0; metaok && i < 16; i++)
		boot_set |= mt.origin_boot[i];
	EXPECT(got && gotv && metaok && strcmp(mt.writer, "talker") == 0 && boot_set && seq_t <= inv.seq &&
		      inv.seq - seq_t < 10,
	      "test.depth mirrored up: writer %s, the sender's seq (%llu there, %llu here), its boot id",
	      metaok ? mt.writer : "?", (unsigned long long)inv.seq, (unsigned long long)seq_t);

	/* Down, with the 5 s offset taken out: pilot.depth's stamps in this clock. */
	use(&V);
	hg_sub *pd = hg_sub_open("pilot.depth", &DEPTH_TYPE);
	uint64_t lat[100];
	int nl = 0;
	t0 = now_ms();
	while (pd && nl < 100 && now_ms() - t0 < 4000) {
		hg_wait(&pd, 1, 100);
		while (nl < 100 && hg_next(pd, &d, sizeof d, &in) == HG_MSG) {
			uint64_t now = hg_now_ns();
			if (in.stamp_ns && now >= in.stamp_ns)
				lat[nl++] = now - in.stamp_ns;
			else if (in.stamp_ns)
				lat[nl++] = 0;
		}
	}
	for (int i = 0; i < nl; i++)
		for (int j = i + 1; j < nl; j++)
			if (lat[j] < lat[i]) {
				uint64_t x = lat[i];
				lat[i] = lat[j];
				lat[j] = x;
			}
	read_link(&V, &lv);
	read_link(&T, &lt);
	EXPECT(nl >= 50 && lat[nl / 2] < 20000000ull,
	      "pilot.depth mirrored down at 50 Hz: %d messages, stamp to the vehicle's ring median %.3f ms, max %.3f ms",
	      nl, nl ? lat[nl / 2] / 1e6 : 0, nl ? lat[nl - 1] / 1e6 : 0);
	EXPECT(lv.clock_offset_ns > 4990000000ll && lv.clock_offset_ns < 5010000000ll &&
		      lt.clock_offset_ns < -4990000000ll && lt.clock_offset_ns > -5010000000ll,
	      "clock offset: the vehicle measures %+.3f ms, the topside %+.3f ms (5000 ms faked), uncertainty %.3f ms; rtt %.3f ms",
	      lv.clock_offset_ns / 1e6, lt.clock_offset_ns / 1e6, lv.clock_uncertainty_ns / 1e6, lv.rtt_ns / 1e6);
	if (pd) {
		struct hg_topic_meta mp;
		hg_sub_meta(pd, &mp);
		EXPECT(strcmp(mp.writer, "pilot") == 0 && mp.stale_after_ns == 100000000ull && mp.rate_mhz == 50000,
		      "pilot.depth here: writer %s, ExpectRate and StaleAfter from bridge.conf (%u mHz, %llu ns)",
		      mp.writer, mp.rate_mhz, (unsigned long long)mp.stale_after_ns);
	}

	/* Silence: test.burst stopped after 30; stale here, the link still up. */
	use(&T);
	hg_sub *tb = hg_sub_open("test.burst", &DEPTH_TYPE);
	int stale = tb && hg_latest(tb, &d, sizeof d, &in) == HG_MSG && in.age_ns > 50000000ull;
	read_link(&T, &lt);
	EXPECT(stale && lt.state == 3, "test.burst silent: stale here (age %.0f ms > 50 ms), the link stays up",
	      tb ? in.age_ns / 1e6 : -1.0);

	/* A call across: tester on the topside, server on the vehicle. */
	char out[600];
	tfmt(out, sizeof out, "%s/tester.out", T.root);
	pid_t c = fork();
	if (c == 0) {
		execl(self_path, self_path, "--tester", T.root, out, (char *)NULL);
		_exit(127);
	}
	int rc = wait_pid_ms(c, 20000);
	const char *res = read_file(out);
	unsigned ok_n = 0;
	unsigned long long med = 0, mx = 0;
	const char *l = res ? strstr(res, "\nok ") : NULL;
	if (l)
		sscanf(l, "\nok %u median_us %llu max_us %llu", &ok_n, &med, &mx);
	EXPECT(rc == 0 && res && strstr(res, "first HG_OK") && ok_n == 20,
	      "server.secret called from the topside: Allow=topside:tester lets it in; 20 calls, round trip median %.3f ms, max %.3f ms",
	      med / 1e3, mx / 1e3);
	const char *ao = res ? strstr(res, "adminonly ") : NULL;
	EXPECT(ao && strstr(ao, "HG_E_DENIED") && strstr(ao, "topside:tester"),
	       "server.adminonly (Allow=admin topside:admin) refuses the topside's tester: %.*s", ao ? (int)strcspn(ao, "\n") : 6,
	       ao ? ao : "(none)");

	/* An operator on the topside: hangar call, through the topside's
	 * manager and its admin topic, the types from tester's [Call]. */
	char cmd[1600];
	tfmt(cmd, sizeof cmd, "HANGAR_ROOT='%s' '%s' call server.adminonly depth_m=2.5 --timeout 2000 2>&1", T.root,
	     T_hangar);
	FILE *pc = popen(cmd, "r");
	char cout[512] = "";
	size_t cn = pc ? fread(cout, 1, sizeof cout - 1, pc) : 0;
	cout[cn] = 0;
	int crc = pc ? pclose(pc) : -1;
	EXPECT(crc == 0 && strstr(cout, "HG_OK") && strstr(cout, "2.5"),
	       "hangar call server.adminonly on the topside: answered across as topside:admin:<user> (%.*s)",
	       (int)strcspn(cout, "\n"), cout);

	/* Authentication: replayed, modified, forged under the wrong key. */
	read_link(&V, &lv);
	uint64_t r0 = lv.rejected;
	const char *names[] = { "", "a replayed datagram", "a datagram with one byte changed",
				"a hello under the wrong key", "a new datagram, its tag wrong in the last byte only",
				"a new datagram, its tag wrong in the first byte only" };
	for (int t = 1; t <= 5; t++) {
		atomic_store(&fwd_tamper, t);
		sleep_ms(300);
		read_link(&V, &lv);
		EXPECT(lv.rejected > r0 && lv.state == 3, "%s: rejected and counted (rejected %llu), the link up",
		      names[t], (unsigned long long)lv.rejected);
		r0 = lv.rejected;
	}

	/* Loss: 20% of datagrams each way. test.log (reliable) arrives whole;
	 * test.depth (latest) shows the gaps. */
	use(&T);
	hg_sub *tl = hg_sub_open_from("test.log", &DEPTH_TYPE, HG_NEWEST);
	hg_sub *tg = hg_sub_open_from("test.depth", &DEPTH_TYPE, HG_NEWEST);
	read_link(&V, &lv);
	uint64_t retx0 = lv.retransmits;
	atomic_store(&fwd_drop_pct, 20);
	uint64_t gaps_l = 0, gaps_d = 0, n_l = 0, first_l = 0, last_l = 0, jumps = 0;
	t0 = now_ms();
	while (now_ms() - t0 < 3000) {
		hg_sub *both[2] = { tl, tg };
		hg_wait(both, 2, 100);
		while (hg_next(tl, &d, sizeof d, &in) == HG_MSG) {
			if (!first_l)
				first_l = in.seq;
			else if (in.seq != last_l + 1 && jumps++ < 4)
				printf("    jump %llu -> %llu, gap %llu, flags %u\n", (unsigned long long)last_l,
				       (unsigned long long)in.seq, (unsigned long long)in.gap, in.flags);
			last_l = in.seq;
			gaps_l += in.gap;
			n_l++;
		}
		while (hg_next(tg, &d, sizeof d, &in) == HG_MSG)
			gaps_d += in.gap;
	}
	atomic_store(&fwd_drop_pct, 0);
	/* What was sent in the lossy window keeps arriving after it. */
	t0 = now_ms();
	while (now_ms() - t0 < 1500) {
		hg_wait(&tl, 1, 100);
		while (hg_next(tl, &d, sizeof d, &in) == HG_MSG) {
			if (in.seq != last_l + 1)
				jumps++;
			last_l = in.seq;
			gaps_l += in.gap;
			n_l++;
		}
	}
	read_link(&V, &lv);
	read_link(&T, &lt);
	if (last_l - first_l + 1 != n_l)
		printf("    %llu seq jumps; topside dropped_old %llu dropped_late %llu clamped %llu\n",
		       (unsigned long long)jumps, (unsigned long long)lt.dropped_old,
		       (unsigned long long)lt.dropped_late, (unsigned long long)lt.clamped);
	EXPECT(n_l > 400 && gaps_l == 0 && last_l - first_l + 1 == n_l && lv.retransmits > retx0,
	      "20%% loss: test.log (reliable) %llu messages, seqs %llu..%llu, no gap; %llu retransmits",
	      (unsigned long long)n_l, (unsigned long long)first_l, (unsigned long long)last_l,
	      (unsigned long long)(lv.retransmits - retx0));
	EXPECT(gaps_d > 0, "20%% loss: test.depth (latest) shows %llu lost messages as gaps", (unsigned long long)gaps_d);
	read_link(&T, &lt);
	EXPECT(lt.state == 3, "the link stayed up through the loss");

	/* The topside reboots: its manager stopped, its topics gone, its boot id
	 * new. pilot.depth here comes back on the new boot id, a new epoch for
	 * its readers, with the sender's seqs from 1. */
	use(&V);
	hg_sub *pr = hg_sub_open("pilot.depth", &DEPTH_TYPE);
	struct hg_topic_meta m0, m1;
	uint64_t seq0 = 0;
	uint32_t epoch0 = 0;
	if (pr && hg_latest(pr, &d, sizeof d, &in) == HG_MSG) {
		seq0 = in.seq;
		epoch0 = in.epoch;
	}
	int have0 = pr && hg_sub_meta(pr, &m0) == 0;
	use(&T);
	manager_stop(T.mgr);
	rm_tree(T.topics);
	manifest("bridge",
		 "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_OFFSET_NS=5000000000 HANGAR_BRIDGE_TEST_BOOT=second "
		 "%s %s\n[Bridge]\n",
		 bridge_bin, T.conf);
	t0 = now_ms();
	start(&T);
	int back = 0;
	uint64_t seq1 = 0;
	uint32_t epoch1 = 0;
	use(&V);
	while (pr && !back && now_ms() - t0 < 8000) {
		hg_wait(&pr, 1, 100);
		while (hg_next(pr, &d, sizeof d, &in) == HG_MSG) {
			if (in.seq < seq0 && hg_sub_meta(pr, &m1) == 0 && memcmp(m1.origin_boot, m0.origin_boot, 16)) {
				back = 1;
				seq1 = in.seq;
				epoch1 = in.epoch;
			}
		}
	}
	EXPECT(have0 && back && epoch1 != epoch0,
	       "the topside rebooted: pilot.depth back on its new boot id, epoch %u -> %u, seq %llu -> %llu, %.2f s from the manager's start",
	       epoch0, epoch1, (unsigned long long)seq0, (unsigned long long)seq1, (now_ms() - t0) / 1e3);

	/* Idle CPU: each bridge over 2 s with its traffic (about 400 msgs/s). */
	pid_t bv = pid_of(&V, "bridge"), bt = pid_of(&T, "bridge");
	long long a0 = th_cpu_ticks(bv), b0 = th_cpu_ticks(bt);
	sleep_ms(2000);
	double cv = th_cpu_percent(a0, th_cpu_ticks(bv), 2000), ct = th_cpu_percent(b0, th_cpu_ticks(bt), 2000);
	EXPECT(cv >= 0 && cv < 10 && ct >= 0 && ct < 10, "CPU with traffic: vehicle bridge %.1f%%, topside %.1f%%", cv,
	      ct);
	(void)text;
}

/* A topside whose msg/ differs: config_match false, the difference named,
 * nothing mirrored. And the CPU of two bridges with nothing to carry. */
static void mismatch_run(void)
{
	setup_side(&V, "mm", NULL);
	manifest("talker", "[Module]\nExec=%s/talker --topic test.depth --rate 100\n"
			   "[Publish test.depth]\nType=depth\nRate=100\n", T_toys);
	setup_side(&T, "mm", NULL);
	char p[1024];
	tfmt(p, sizeof p, "%s/depth.msg", T.msg);
	FILE *f = fopen(p, "a");
	if (f) {
		fputs("# a comment the vehicle's copy has not\n", f);
		fclose(f);
	}
	start(&V);
	start(&T);
	struct bridge_link lt;
	int seen = wait_link(&T, 0, 8000, &lt);
	EXPECT(seen && strstr(lt.detail, "msg/"), "msg/ trees differ: config_match false, detail '%s'", lt.detail);
	sleep_ms(500);
	use(&T);
	hg_sub *td = hg_sub_open("test.depth", &DEPTH_TYPE);
	struct depth d;
	struct hg_info in;
	EXPECT(!td || hg_latest(td, &d, sizeof d, &in) != HG_MSG, "nothing mirrored");
	pid_t bv = pid_of(&V, "bridge"), bt = pid_of(&T, "bridge");
	long long a0 = th_cpu_ticks(bv), b0 = th_cpu_ticks(bt);
	sleep_ms(2000);
	double cv = th_cpu_percent(a0, th_cpu_ticks(bv), 2000), ct = th_cpu_percent(b0, th_cpu_ticks(bt), 2000);
	EXPECT(cv >= 0 && cv < 2 && ct >= 0 && ct < 2, "idle CPU (pings, the status topic): vehicle %.1f%%, topside %.1f%%",
	      cv, ct);
}

/* Two clock bases, three days apart: the topside's manager, its modules and
 * its tester run in a time namespace. Stamps come out in the vehicle's
 * clock, and a call whose OK reply has an all-zero body is answered. */
static void clocks_run(void)
{
	if (!timens_works()) {
		printf("skip  clocks: no user and time namespace here (unshare CLONE_NEWUSER|CLONE_NEWTIME)\n");
		return;
	}
	setup_side(&V, "clk", NULL);
	manifest("server", "[Module]\nExec=%s/server\n"
			   "[Serve secret]\nRequest=depth\nReply=depth\nAllow=topside:tester\n", T_toys);
	setup_side(&T, "clk", NULL);
	manifest("pilot", "[Module]\nExec=%s/talker --topic pilot.depth --rate 50\n"
			  "[Publish pilot.depth]\nType=depth\nRate=50\n", T_toys);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Call server.secret]\nRequest=depth\nReply=depth\n");
	start(&V);
	start_ahead(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	EXPECT(up, "link up between two clock bases");
	if (!up)
		return;
	sleep_ms(1000);
	read_link(&V, &lv);
	double off_s = lv.clock_offset_ns / 1e9;
	EXPECT(off_s > TIMENS_S - 2 && off_s < TIMENS_S + 2,
	       "the vehicle measures the topside's clock %.3f s ahead (%d s set), uncertainty %.3f ms", off_s, TIMENS_S,
	       lv.clock_uncertainty_ns / 1e6);
	use(&V);
	hg_sub *pd = hg_sub_open("pilot.depth", &DEPTH_TYPE);
	struct depth d;
	struct hg_info in;
	int n = 0, good = 0;
	uint64_t t0 = now_ms();
	while (pd && n < 50 && now_ms() - t0 < 3000) {
		hg_wait(&pd, 1, 100);
		while (hg_next(pd, &d, sizeof d, &in) == HG_MSG) {
			uint64_t now = hg_now_ns();
			n++;
			good += in.stamp_ns <= now && now - in.stamp_ns < 50000000ull;
		}
	}
	EXPECT(n >= 50 && good == n, "pilot.depth here: %d of %d stamps within 50 ms before now, in this clock", good, n);
	const char *res = run_tester("--tester-zero", 1, 20000);
	int zero = -1, body = -1;
	const char *l = res ? strstr(res, "\nzero ") : NULL;
	if (l)
		sscanf(l, "\nzero %d body %d", &zero, &body);
	EXPECT(res && strstr(res, "first HG_OK") && body == 5, "calls with a body in the reply: %d of 5 answered", body);
	EXPECT(zero == 5, "calls whose OK reply is all zeros (as Reply=empty): %d of 5 answered%s%.*s", zero,
	       zero == 5 ? "" : "; the last: ", l ? (int)strcspn(strstr(l, "last ") + 5, "\n") : 0,
	       l ? strstr(l, "last ") + 5 : "");
}

/* Reads pilot.depth on the vehicle for ms, or until max messages: how many
 * came, how many with a stamp within 50 ms before now, how many stamped
 * over a second ahead of now. */
struct stamps {
	int n, good, ahead;
};

static struct stamps read_stamps(hg_sub *pd, long ms, int max)
{
	struct stamps st = { 0, 0, 0 };
	struct depth d;
	struct hg_info in;
	uint64_t t0 = now_ms();
	while (st.n < max && now_ms() - t0 < (uint64_t)ms) {
		hg_wait(&pd, 1, 50);
		while (st.n < max && hg_next(pd, &d, sizeof d, &in) == HG_MSG) {
			uint64_t now = hg_now_ns();
			st.n++;
			st.good += in.stamp_ns <= now && now - in.stamp_ns < 500000000ull; /* the offsets tested are seconds; a loaded machine reads late */
			st.ahead += in.stamp_ns > now + 1000000000ull;
		}
	}
	return st;
}

/* No clock estimate, nothing translated (docs/08 "Clocks"). The forwarder
 * holds back the pongs on their way to the vehicle, so its bridge has a link
 * and no estimate: no command may be published there meanwhile, a call
 * waits and is answered once the estimate comes, and no stamp is clamped
 * afterwards. Then the same with the topside rebooted onto another clock. */
static void hold_run(void)
{
	setup_side(&V, "hold", NULL);
	manifest("server", "[Module]\nExec=%s/server\n"
			   "[Serve secret]\nRequest=depth\nReply=depth\nAllow=topside:tester\n", T_toys);
	manifest("pilotread", "[Module]\nExec=%s/listener --topic pilot.depth\n[Subscribe pilot.depth]\n", T_toys);
	setup_side(&T, "hold", NULL);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_OFFSET_NS=5000000000 %s %s\n[Bridge]\n",
		 bridge_bin, T.conf);
	manifest("pilot", "[Module]\nExec=%s/talker --topic pilot.depth --rate 50\n"
			  "[Publish pilot.depth]\nType=depth\nRate=50\n", T_toys);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Call server.secret]\nRequest=depth\nReply=depth\n");
	atomic_store(&fwd_drop_kind_v, 4);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	EXPECT(up, "link up, config_match on both sides, with the vehicle's pongs held back");
	if (!up)
		return;
	use(&V);
	hg_sub *pd = hg_sub_open_from("pilot.depth", &DEPTH_TYPE, HG_OLDEST);
	/* A call made now: its request reaches a vehicle that cannot yet say
	 * when its deadline is. */
	char out[600];
	tfmt(out, sizeof out, "%s/tester.out", T.root);
	unlink(out);
	pid_t c = fork();
	if (c == 0) {
		execl(self_path, self_path, "--tester-one", T.root, out, (char *)NULL);
		_exit(127);
	}
	struct stamps st = read_stamps(pd, 1200, 1000);
	read_link(&V, &lv);
	use(&V);
	struct mstat ms;
	long served = -1;
	if (module_status("server", &ms))
		sscanf(ms.status, "served %ld", &served);
	EXPECT(st.n == 0 && lv.dropped_late > 0 && lv.state == 3,
	       "no estimate on the vehicle for 1.2 s: %d pilot.depth messages published there (%d stamped over 1 s ahead), "
	       "%llu dropped and counted",
	       st.n, st.ahead, (unsigned long long)lv.dropped_late);
	EXPECT(served == 0, "the call's request not handed to the server meanwhile (served %ld)", served);
	atomic_store(&fwd_drop_kind_v, 0);
	st = read_stamps(pd, 3000, 100);
	read_link(&V, &lv);
	EXPECT(st.n == 100 && st.good == st.n && lv.clamped == 0,
	       "pongs through: %d messages, %d with their stamp within 500 ms before now, %llu clamped", st.n, st.good,
	       (unsigned long long)lv.clamped);
	int rc = wait_pid_ms(c, 8000);
	const char *res = rc == 0 ? read_file(out) : NULL;
	EXPECT(res && strstr(res, "one HG_OK"), "the call made meanwhile: %.*s", res ? (int)strcspn(res, "\n") : 9,
	       res ? res : "no result");

	/* The topside rebooted onto another clock (9 s ahead, not 5): the old
	 * boot's offset is no estimate of the new one. */
	atomic_store(&fwd_drop_kind_v, 4);
	unsigned char boot0[16];
	memcpy(boot0, lv.peer_boot_id, 16);
	use(&T);
	manager_stop(T.mgr);
	rm_tree(T.topics);
	manifest("bridge",
		 "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_OFFSET_NS=9000000000 HANGAR_BRIDGE_TEST_BOOT=second "
		 "%s %s\n[Bridge]\n",
		 bridge_bin, T.conf);
	start(&T);
	uint64_t t0 = now_ms();
	int newboot = 0;
	while (!newboot && now_ms() - t0 < 8000) {
		newboot = wait_link(&V, 1, 200, &lv) && memcmp(lv.peer_boot_id, boot0, 16) != 0;
	}
	uint64_t late0 = lv.dropped_late;
	use(&V);
	(void)read_stamps(pd, 100, 1000); /* what was left of the old boot's */
	st = read_stamps(pd, 800, 1000);
	read_link(&V, &lv);
	EXPECT(newboot && st.n == 0 && lv.dropped_late > late0,
	       "the topside rebooted, no estimate of its new clock for 0.8 s: %d messages published (%d stamped over 1 s ahead)",
	       st.n, st.ahead);
	atomic_store(&fwd_drop_kind_v, 0);
	st = read_stamps(pd, 3000, 50);
	read_link(&V, &lv);
	EXPECT(st.n == 50 && st.good == st.n && lv.clamped == 0 && lv.clock_offset_ns > 8990000000ll &&
		       lv.clock_offset_ns < 9010000000ll,
	       "pongs through: %d messages, %d with their stamp within 500 ms before now, %llu clamped, offset %+.3f ms",
	       st.n, st.good, (unsigned long long)lv.clamped, lv.clock_offset_ns / 1e6);
}

static int count_in_file(const char *path, const char *needle)
{
	const char *t = read_file(path);
	int n = 0;
	for (const char *q = t; q && (q = strstr(q, needle)); q += strlen(needle))
		n++;
	return n;
}

/* Watches the vehicle's link for ms: 1 if it was ever up, 1 in *boot_seen
 * if its peer was ever the boot id given. Reads pd (a subscription of the
 * vehicle's) as it goes: how many messages came, how many were lost. */
static int watch_link(long ms, const unsigned char *boot, int *boot_seen, hg_sub *pd, int *came, uint64_t *gaps)
{
	struct bridge_link lv;
	struct depth d;
	struct hg_info in;
	int up_seen = 0;
	uint64_t t0 = now_ms();
	do {
		if (read_link(&V, &lv) == 0) {
			up_seen |= lv.state == 3;
			if (boot && boot_seen && lv.state == 3 && memcmp(lv.peer_boot_id, boot, 16) == 0)
				*boot_seen = 1;
		}
		use(&V);
		while (pd && hg_next(pd, &d, sizeof d, &in) == HG_MSG) {
			*came += 1;
			*gaps += in.gap;
		}
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)ms);
	return up_seen;
}

/* A recorded session played back (docs/08 "The link": a hello replayed from
 * an earlier connection starts at most an attempt that never completes).
 * Someone on the tether with no key has recorded everything the topside
 * sent, hello first, and sends it again, from the topside's address: into
 * the session it came from; after the topside has rebooted and the link is
 * up on new keys; with the topside gone; and after the vehicle's bridge has
 * restarted. None of it may bring a link up, change the peer of one that is
 * up, or put a message on a topic. */
static void replay_run(void)
{
	setup_side(&V, "rpl", NULL);
	manifest("pilotread", "[Module]\nExec=%s/listener --topic pilot.depth\n[Subscribe pilot.depth]\n", T_toys);
	setup_side(&T, "rpl", NULL);
	manifest("pilot", "[Module]\nExec=%s/talker --topic pilot.depth --rate 50\n"
			  "[Publish pilot.depth]\nType=depth\nRate=50\n", T_toys);
	atomic_store(&fwd_rec_on, 1);
	char blog[600]; /* the vehicle's bridge says "link up with" for each link made */
	tfmt(blog, sizeof blog, "%s/run/hangar/log/bridge.log", V.root);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	EXPECT(up, "link up, the topside's datagrams recorded from its first hello");
	if (!up)
		return;
	sleep_ms(1500);
	read_link(&V, &lv);
	unsigned char boot1[16], boot2[16];
	memcpy(boot1, lv.peer_boot_id, 16);
	int n1 = atomic_load(&fwd_nrec);
	int hello1 = n1 > 0 && recs[0].d[4] == 1;

	/* Into its own session: every datagram is a duplicate. */
	uint64_t rej0 = lv.rejected;
	int ups0 = count_in_file(blog, "link up with");
	replay_to_vehicle(0, n1, 200);
	sleep_ms(300);
	read_link(&V, &lv);
	EXPECT(hello1 && lv.state == 3 && lv.rejected - rej0 >= (uint64_t)n1 / 2 &&
		       count_in_file(blog, "link up with") == ups0,
	       "%d datagrams replayed into their own session: %llu rejected, the link up, no new link", n1,
	       (unsigned long long)(lv.rejected - rej0));

	/* The topside reboots: a new boot id, a new nonce, new keys. */
	use(&T);
	manager_stop(T.mgr);
	n1 = atomic_load(&fwd_nrec);
	rm_tree(T.topics);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_BOOT=second %s %s\n[Bridge]\n", bridge_bin,
		 T.conf);
	start(&T);
	uint64_t t0 = now_ms();
	int newboot = 0;
	while (!newboot && now_ms() - t0 < 8000)
		newboot = wait_link(&V, 1, 200, &lv) && memcmp(lv.peer_boot_id, boot1, 16) != 0;
	memcpy(boot2, lv.peer_boot_id, 16);
	sleep_ms(500);
	int n2 = atomic_load(&fwd_nrec);
	ups0 = count_in_file(blog, "link up with");
	read_link(&V, &lv);
	rej0 = lv.rejected;
	use(&V);
	hg_sub *pd = hg_sub_open("pilot.depth", &DEPTH_TYPE);
	struct depth d;
	struct hg_info in;
	struct hg_topic_meta m;
	while (pd && hg_next(pd, &d, sizeof d, &in) == HG_MSG)
		;
	uint32_t epoch0 = in.epoch;
	/* The first session again, into the link the second one made. */
	replay_to_vehicle(0, n1, 200);
	int old_seen = 0, came = 0;
	uint64_t gaps = 0;
	watch_link(1500, boot1, &old_seen, pd, &came, &gaps);
	read_link(&V, &lv);
	int ups1 = count_in_file(blog, "link up with");
	use(&V);
	(void)hg_latest(pd, &d, sizeof d, &in);
	int meta = pd && hg_sub_meta(pd, &m) == 0;
	EXPECT(newboot && !old_seen && ups1 == ups0 && lv.state == 3 && memcmp(lv.peer_boot_id, boot2, 16) == 0,
	       "the topside rebooted, its earlier session replayed (%d datagrams): %s, %d new link(s), %llu rejected",
	       n1, old_seen ? "the vehicle's link went over to the replayed session" : "the link stays with the topside",
	       ups1 - ups0, (unsigned long long)(lv.rejected - rej0));
	EXPECT(meta && memcmp(m.origin_boot, boot2, 16) == 0 && in.epoch == epoch0 && came >= 60 && gaps < 5,
	       "pilot.depth on the vehicle meanwhile: the rebooted topside's, %d messages in 1.5 s, %llu lost, epoch %u -> %u",
	       came, (unsigned long long)gaps, epoch0, in.epoch);

	/* The topside gone (unplugged, or off): both sessions replayed at a
	 * vehicle with no link. */
	use(&T);
	manager_stop(T.mgr);
	T.mgr = 0;
	n2 = atomic_load(&fwd_nrec);
	t0 = now_ms();
	int down = 0;
	while (!down && now_ms() - t0 < 6000) {
		down = read_link(&V, &lv) == 0 && lv.state != 3;
		sleep_ms(50);
	}
	use(&V);
	while (pd && hg_next(pd, &d, sizeof d, &in) == HG_MSG)
		;
	ups0 = count_in_file(blog, "link up with");
	int backs0 = count_in_file(blog, "link back up");
	replay_to_vehicle(0, n2, 200);
	int published = 0;
	int up_seen = watch_link(1500, NULL, NULL, pd, &published, &gaps);
	ups1 = count_in_file(blog, "link up with") + count_in_file(blog, "link back up") - backs0;
	EXPECT(down && !up_seen && ups1 == ups0 && published == 0,
	       "the topside gone, both its sessions replayed (%d datagrams): %s, %d message(s) published on pilot.depth",
	       n2, up_seen || ups1 != ups0 ? "the vehicle's link came up with nobody there" : "no link", published);

	/* And at a vehicle whose bridge has restarted since. */
	use(&V);
	pid_t b0 = pid_of(&V, "bridge");
	ctl("restart bridge");
	int again = wait_new_pid("bridge", b0, "ready", 5000);
	sleep_ms(300);
	replay_to_vehicle(0, n2, 200);
	up_seen = watch_link(1500, NULL, NULL, pd, &published, &gaps);
	EXPECT(again && !up_seen && published == 0,
	       "the vehicle's bridge restarted, both sessions replayed: %s, %d message(s) published",
	       up_seen ? "a link came up" : "no link", published);
}

/* Sends the recorded hello at index h to the vehicle from sock, n times
 * gap_ms apart, reading test.depth on the topside meanwhile; returns how
 * many datagrams came back to sock (from anyone) until 300 ms after. */
static int hello_from_elsewhere(int sock, int h, int n, int gap_ms, hg_sub *td, int *came, uint64_t *gaps)
{
	int got = 0;
	struct depth d;
	struct hg_info in;
	unsigned char buf[1500];
	for (int i = 0; i < n + 300 / gap_ms; i++) {
		if (i < n)
			sendto(sock, recs[h].d, recs[h].len, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
		sleep_ms(gap_ms);
		while (recv(sock, buf, sizeof buf, MSG_DONTWAIT) > 0)
			got++;
		use(&T);
		while (td && hg_next(td, &d, sizeof d, &in) == HG_MSG) {
			*came += 1;
			*gaps += in.gap;
		}
	}
	return got;
}

/* A hello does not move a link that is up (docs/08 "The link"). The
 * vehicle's bridge has Peer=auto, so it takes its peer's address from what
 * it hears. A recorded hello of the topside, sent again from another address:
 * the hello of the session in force, then, after the topside's bridge has
 * restarted, the hello of the session before. The vehicle must send nothing
 * to that address and lose nothing of what it sends up.
 *
 * (Peer=auto beacons to the broadcast address of this host once a second
 * while the run lasts, on a port of its own: a few datagrams nobody reads.) */
static void auto_run(void)
{
	V.peer = "auto";
	V.dport = T.dport = free_port();
	setup_side(&V, "auto", NULL);
	manifest("talker", "[Module]\nExec=%s/talker --topic test.depth --rate 100\n"
			   "[Publish test.depth]\nType=depth\nRate=100\n", T_toys);
	setup_side(&T, "auto", NULL);
	atomic_store(&fwd_rec_on, 1);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	EXPECT(up, "link up, the vehicle with Peer=auto: its peer is whoever it heard (%s)", up ? lv.peer_addr : "-");
	if (!up)
		return;
	sleep_ms(500);
	int port, h1 = -1, h2 = -1;
	int atk = udp_bound(&port);
	for (int i = 0; i < atomic_load(&fwd_nrec) && h1 < 0; i++)
		if (recs[i].d[4] == 1)
			h1 = i;
	use(&T);
	hg_sub *td = hg_sub_open("test.depth", &DEPTH_TYPE);
	struct depth d;
	struct hg_info in;
	while (td && hg_next(td, &d, sizeof d, &in) == HG_MSG)
		;
	int came = 0;
	uint64_t gaps = 0;
	int got = h1 < 0 ? -1 : hello_from_elsewhere(atk, h1, 200, 5, td, &came, &gaps);
	read_link(&V, &lv);
	EXPECT(got == 0 && gaps == 0 && came > 100 && lv.state == 3,
	       "the session's own hello from another address, 200 times in 1 s: %d datagrams sent there by the vehicle; "
	       "test.depth on the topside %d messages, %llu lost",
	       got, came, (unsigned long long)gaps);

	/* A new session: the topside's bridge restarts. Its old hello is now one
	 * of an earlier connection. */
	use(&T);
	pid_t b0 = pid_of(&T, "bridge");
	int n0 = atomic_load(&fwd_nrec);
	ctl("restart bridge");
	wait_new_pid("bridge", b0, "ready", 5000);
	up = wait_link(&T, 1, 8000, &lt) && wait_link(&V, 1, 3000, &lv);
	sleep_ms(500);
	for (int i = n0; i < atomic_load(&fwd_nrec) && h2 < 0; i++)
		if (recs[i].d[4] == 1)
			h2 = i;
	use(&T);
	while (td && hg_next(td, &d, sizeof d, &in) == HG_MSG)
		;
	came = 0;
	gaps = 0;
	got = !up || h2 < 0 ? -1 : hello_from_elsewhere(atk, h1, 200, 5, td, &came, &gaps);
	read_link(&V, &lv);
	EXPECT(got == 0 && gaps == 0 && came > 100 && lv.state == 3,
	       "the hello of the session before, from another address, 200 times in 1 s: %d datagrams sent there; "
	       "test.depth %d messages, %llu lost",
	       got, came, (unsigned long long)gaps);
	close(atk);

	/* A handshake under way is not knocked over either: the topside's bridge
	 * restarts once more while its first hello is sent at the vehicle a
	 * thousand times a second, from the address the vehicle answers. */
	uint32_t id0 = lv.peer_id;
	use(&T);
	b0 = pid_of(&T, "bridge");
	ctl("restart bridge");
	uint64_t t0 = now_ms();
	int relinked = 0;
	while (!relinked && now_ms() - t0 < 5000) {
		for (int i = 0; i < 20; i++) {
			sendto(fwd_sock, recs[h1].d, recs[h1].len, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
			usleep(1000);
		}
		relinked = read_link(&V, &lv) == 0 && lv.state == 3 && lv.peer_id != id0 && lv.config_match;
	}
	EXPECT(relinked, "the topside's bridge restarted under 1000 old hellos a second: linked again after %.2f s",
	       (now_ms() - t0) / 1e3);
}

/* Reads test.ev on the topside until it has seen seq `until` or ms have
 * passed: messages, lost ones, the last seq. */
static void read_ev(hg_sub *ev, uint64_t until, long ms, int *came, uint64_t *gaps, uint64_t *last)
{
	struct depth d;
	struct hg_info in;
	uint64_t t0 = now_ms();
	use(&T);
	while (*last < until && now_ms() - t0 < (uint64_t)ms) {
		hg_wait(&ev, 1, 50);
		while (hg_next(ev, &d, sizeof d, &in) == HG_MSG) {
			*came += 1;
			*gaps += in.gap;
			*last = in.seq;
		}
	}
}

/* A reliable topic whose writer restarts while the link is down (docs/08
 * "Calls": what is sent while the link is down goes through when it is
 * back). The vehicle's `ev` publishes at 20 Hz into a ring of 256; with the
 * tether cut it is restarted twice, each restart a new file for the topic.
 * What the bridge read from the first two files while it could not send is
 * in no file when the link returns: it must arrive all the same, every seq
 * up to the newest. */
static void writer_run(void)
{
	const char *mirror = "[Mirror test.ev]\nDirection=up\nDelivery=reliable\n";
	setup_side(&V, "wr", mirror);
	manifest("ev", "[Module]\nExec=%s/talker --topic test.ev --rate 20\n"
		       "[Publish test.ev]\nType=depth\nRate=20\nSlots=256\n", T_toys);
	setup_side(&T, "wr", mirror);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	use(&T);
	hg_sub *ev = hg_sub_open_from("test.ev", &DEPTH_TYPE, HG_OLDEST);
	int came = 0;
	uint64_t gaps = 0, last = 0;
	if (up && ev)
		read_ev(ev, 20, 3000, &came, &gaps, &last);
	EXPECT(up && came >= 20 && gaps == 0, "test.ev (reliable, 20 Hz): %d messages on the topside, to seq %llu, none lost",
	       came, (unsigned long long)last);
	if (!up)
		return;
	/* The tether cut. */
	atomic_store(&fwd_drop_pct, 100);
	uint64_t t0 = now_ms(), cut = last;
	int down = 0;
	while (!down && now_ms() - t0 < 6000) {
		down = read_link(&V, &lv) == 0 && lv.state != 3;
		sleep_ms(50);
	}
	int restarts = 0;
	for (int i = 0; i < 2; i++) {
		sleep_ms(600); /* some 12 messages, read by the bridge */
		use(&V);
		pid_t p0 = pid_of(&V, "ev");
		ctl("restart ev");
		restarts += wait_new_pid("ev", p0, "ready", 5000);
	}
	sleep_ms(600);
	use(&V);
	hg_sub *lev = hg_sub_open("test.ev", &DEPTH_TYPE);
	struct depth d;
	struct hg_info in;
	uint64_t there = lev && hg_latest(lev, &d, sizeof d, &in) == HG_MSG ? in.seq : 0;
	/* And mended. */
	atomic_store(&fwd_drop_pct, 0);
	came = 0;
	read_ev(ev, there, 6000, &came, &gaps, &last);
	EXPECT(down && restarts == 2 && there > cut + 40 && last >= there && gaps == 0,
	       "the link down, the writer restarted twice, the link back: seqs %llu to %llu arrive, %d messages, %llu lost",
	       (unsigned long long)cut + 1, (unsigned long long)last, came, (unsigned long long)gaps);
}

/* What a peer that holds the key may still not do to this side. A record
 * on the reliable stream larger than any this side takes (the vehicle's
 * bridge sends one, 1 MB and a little, told to by the environment): it is
 * passed over, and the records after it are read. A ring for a mirrored
 * topic as large as the peer says its own is: bounded here. The topside's
 * bridge runs with its bound at 4 KB (the real one is 32 MB a topic, which
 * a test should not fill): test.ev, 256 slots on the vehicle, gets the ring
 * that fits, and is mirrored all the same. */
static void limits_run(void)
{
	const char *mirror = "[Mirror test.ev]\nDirection=up\nDelivery=reliable\n";
	setup_side(&V, "lim", mirror);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_BIG_RECORD=1 %s %s\n[Bridge]\n", bridge_bin,
		 V.conf);
	manifest("ev", "[Module]\nExec=%s/talker --topic test.ev --rate 20\n"
		       "[Publish test.ev]\nType=depth\nRate=20\nSlots=256\n", T_toys);
	setup_side(&T, "lim", mirror);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_RING_MAX=4096 %s %s\n[Bridge]\n", bridge_bin,
		 T.conf);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	use(&T);
	hg_sub *ev = hg_sub_open_from("test.ev", &DEPTH_TYPE, HG_OLDEST);
	int came = 0;
	uint64_t gaps = 0, last = 0;
	if (up && ev)
		read_ev(ev, 40, 6000, &came, &gaps, &last);
	EXPECT(up && came >= 40 && gaps == 0,
	       "a record of 1 MB + 4 KB on the stream, then test.ev (reliable): %d messages on the topside, to seq %llu, %llu lost",
	       came, (unsigned long long)last, (unsigned long long)gaps);
	struct hg_topic_meta mt, mv;
	use(&V);
	hg_sub *lev = hg_sub_open("test.ev", &DEPTH_TYPE);
	int have = ev && lev && hg_sub_meta(ev, &mt) == 0 && hg_sub_meta(lev, &mv) == 0;
	EXPECT(have && mv.slot_count == 256 && mt.slot_count >= 2 && mt.slot_count * (mt.payload_bytes + 128) <= 4096,
	       "test.ev: %u slots on the vehicle, %u in its mirror here under a 4 KB bound (%u bytes a slot)",
	       have ? mv.slot_count : 0, have ? mt.slot_count : 0, have ? mt.payload_bytes + 128 : 0);
}

/* ---- Discovery on loopback ---------------------------------------------------- */

/* The key of setup_side's bridge.key. The suite holds it to stand for two
 * things: a second machine that holds the key (the twin vehicle below, the
 * third machine of peer_run), and a recording. A beacon cannot be recorded
 * here: a bridge sends it to the broadcast address, which no second socket on
 * loopback is sure to hear. So the suite writes the lines the bridges would
 * have sent, each with the tag a recording of it would carry, and whoever
 * sends them again in a run needs no key to do it. */
static const unsigned char TEST_KEY[32] = { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
					    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

#define AIR_V 1u       /* the vehicle's beacon, heard by the topside, once a second */
#define AIR_T 2u       /* the topside's, heard by the vehicle */
#define AIR_STALE 4u   /* nine beacons of earlier ids of each side, ten times a second, from another address */
#define AIR_MOVED 8u   /* the two beacons now on the air, sent again from another address */
#define AIR_HELLOS 16u /* an old hello of each side at the other, ten times a second, from another address */
#define AIR_TWIN 32u   /* a second vehicle with the key: its beacon, and its answer to every hello */
#define ID_V 0x0a000001u
#define ID_T 0x0b000001u
#define ID_STALE_V 0x0c000100u
#define ID_STALE_T 0x0c000200u
#define ID_TWIN 0x0d000001u
static atomic_uint air_what, air_id_t;
static atomic_int air_stop;
static atomic_long air_atk_got, air_twin_asked;
static int air_relay, air_atk, air_twin, air_twin_port;

static void send_beacon(int sock, int dport, const char *role, int port, uint32_t id)
{
	char line[200];
	unsigned char tg[16];
	int n = snprintf(line, sizeof line, "HANGAR/1 role=%s pair=test port=%d id=%08x", role, port, id);
	blake2s(tg, 16, TEST_KEY, 32, line, (size_t)n);
	n += snprintf(line + n, sizeof line - (size_t)n, " tag=");
	for (int i = 0; i < 16; i++)
		n += snprintf(line + n, sizeof line - (size_t)n, "%02x", tg[i]);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)dport) };
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sendto(sock, line, (size_t)n, 0, (const struct sockaddr *)&to, sizeof to);
}

/* A hello of pair test under the key: role 1 the vehicle, 2 the topside. */
#define HELLO_LEN (24 + 92 + 16)
static void make_hello(unsigned char *d, int role, uint32_t id, const unsigned char *nonce, const unsigned char *echo)
{
	uint32_t bl = 92;
	uint64_t seq = 1;
	memset(d, 0, HELLO_LEN);
	memcpy(d, "HGB1", 4);
	d[4] = 1;
	memcpy(d + 12, &bl, 4);
	memcpy(d + 16, &seq, 8);
	unsigned char *b = d + 24;
	b[0] = (unsigned char)role;
	memcpy(b + 4, &id, 4);
	memcpy(b + 8, "test", 4);
	memcpy(b + 40, nonce, 16);
	memset(b + 56, 0xee, 16);
	if (echo)
		memcpy(b + 72, echo, 16);
	blake2s(d + 24 + 92, 16, TEST_KEY, 32, d, 24 + 92);
}

/* What the two bridges hear beside each other's datagrams. Their own beacons
 * name the forwarder's port, so their hellos and their link go through it. */
static void *air(void *arg)
{
	(void)arg;
	unsigned char old_t[HELLO_LEN], old_v[HELLO_LEN], nonce[16], buf[1500];
	memset(nonce, 0x51, sizeof nonce);
	make_hello(old_t, 2, ID_STALE_T, nonce, NULL);
	memset(nonce, 0x52, sizeof nonce);
	make_hello(old_v, 1, ID_STALE_V, nonce, NULL);
	memset(nonce, 0x53, sizeof nonce); /* the twin's */
	struct sockaddr_in v = fwd_v, t = fwd_t;
	for (unsigned i = 0; !atomic_load(&air_stop); i++) {
		unsigned w = atomic_load(&air_what);
		if (i % 5 == 0) {
			if (w & AIR_HELLOS) {
				sendto(air_atk, old_t, sizeof old_t, 0, (const struct sockaddr *)&v, sizeof v);
				sendto(air_atk, old_v, sizeof old_v, 0, (const struct sockaddr *)&t, sizeof t);
			}
			/* Ten times a second, not once: a topside's bridge that has
			 * just started has then heard of both vehicles before either
			 * answers it, which is the case this is about. */
			if (w & AIR_TWIN)
				send_beacon(air_twin, T.dport, "vehicle", air_twin_port, ID_TWIN);
			if ((w & AIR_T) && i % 50 == 0)
				send_beacon(air_relay, V.dport, "topside", fwd_port, atomic_load(&air_id_t));
			if ((w & AIR_V) && i % 50 == 0)
				send_beacon(air_relay, T.dport, "vehicle", fwd_port, ID_V);
			if (w & AIR_MOVED) {
				send_beacon(air_atk, V.dport, "topside", fwd_port, atomic_load(&air_id_t));
				send_beacon(air_atk, T.dport, "vehicle", fwd_port, ID_V);
			}
			for (uint32_t k = 0; (w & AIR_STALE) && k < 9; k++) {
				send_beacon(air_atk, V.dport, "topside", fwd_port, ID_STALE_T + k);
				send_beacon(air_atk, T.dport, "vehicle", fwd_port, ID_STALE_V + k);
			}
		}
		while (recv(air_atk, buf, sizeof buf, MSG_DONTWAIT) > 0)
			atomic_fetch_add(&air_atk_got, 1);
		for (;;) {
			struct sockaddr_in from;
			socklen_t fl = sizeof from;
			ssize_t n = recvfrom(air_twin, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
			if (n <= 0)
				break;
			if (!(w & AIR_TWIN) || n != HELLO_LEN || buf[4] != 1)
				continue;
			unsigned char h[HELLO_LEN];
			make_hello(h, 1, ID_TWIN, nonce, buf + 24 + 40);
			sendto(air_twin, h, sizeof h, 0, (const struct sockaddr *)&from, fl);
			atomic_fetch_add(&air_twin_asked, 1);
		}
		usleep(20000);
	}
	return NULL;
}

struct both {
	long up_ms;                    /* when both links were first up; -1: never */
	int n, amb_v, amb_t, up_v, up_t; /* samples, 20 ms apart: how many ambiguous, how many up */
};

/* Watches both links for ms, or until both are up. */
static struct both watch_both(long ms, int until_up)
{
	struct both s = { .up_ms = -1 };
	struct bridge_link lv, lt;
	uint64_t t0 = now_ms();
	do {
		int hv = read_link(&V, &lv) == 0, ht = read_link(&T, &lt) == 0;
		s.n++;
		s.amb_v += hv && lv.state == 4;
		s.amb_t += ht && lt.state == 4;
		s.up_v += hv && lv.state == 3;
		s.up_t += ht && lt.state == 3;
		if (s.up_ms < 0 && hv && ht && lv.state == 3 && lt.state == 3)
			s.up_ms = (long)(now_ms() - t0);
		if (until_up && s.up_ms >= 0)
			break;
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)ms);
	return s;
}

/* Both bridges started again: no keys, no candidates, nothing remembered. */
static int restart_bridges(void)
{
	use(&V);
	pid_t bv = pid_of(&V, "bridge");
	ctl("restart bridge");
	use(&T);
	pid_t bt = pid_of(&T, "bridge");
	ctl("restart bridge");
	int ok = wait_new_pid("bridge", bt, "ready", 5000);
	use(&V);
	return wait_new_pid("bridge", bv, "ready", 5000) && ok;
}

/* Peer=auto on both sides (docs/08 "Discovery"). A beacon's tag is under the
 * shared key, which never changes, and nothing in a beacon says when it was
 * made: one sent again checks like the first time. So no beacon may decide
 * anything: not who the peer is, not where it is, not that there are two.
 * What is on the air beside the two bridges' own beacons, each time with both
 * bridges started again so that a link has to be made:
 *
 * - beacons of nine earlier ids of each side (a bridge draws a new id at
 *   every start), from another address;
 * - the beacons now on the air, sent again from another address;
 * - an old hello of each side at the other, ten times a second;
 * - a second vehicle that holds the key and answers: the one case in which
 *   the topside must choose none, and say so.
 *
 * And what a restart of the peer costs with nobody else there. */
static void beacon_run(void)
{
	V.peer = T.peer = "auto";
	V.dport = free_port();
	T.dport = free_port();
	setup_side(&V, "bcn", NULL);
	setup_side(&T, "bcn", NULL);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)fwd_port) };
	a.sin_addr.s_addr = htonl(0x7f000002); /* 127.0.0.2: whoever else is on the tether */
	int p;
	air_relay = udp_bound(&p);
	air_twin = udp_bound(&air_twin_port);
	air_atk = socket(AF_INET, SOCK_DGRAM, 0);
	if (bind(air_atk, (struct sockaddr *)&a, sizeof a) < 0) {
		EXPECT(0, "a socket on 127.0.0.2:%d: %s", fwd_port, strerror(errno));
		return;
	}
	atomic_store(&air_stop, 0);
	atomic_store(&air_id_t, ID_T);
	atomic_store(&air_what, AIR_V | AIR_T);
	pthread_t th;
	pthread_create(&th, NULL, air, NULL);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	uint64_t t0 = now_ms();
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	EXPECT(up, "Peer=auto on both sides, each hearing the other's beacon: link up %.2f s after the managers' start (%s)",
	       (now_ms() - t0) / 1e3, up ? lv.peer_addr : "-");
	if (!up)
		goto out;

	/* The peer's bridge restarts: a new id in its beacons from then on. */
	uint32_t id0 = lv.peer_id;
	use(&T);
	pid_t b0 = pid_of(&T, "bridge");
	atomic_store(&air_id_t, ID_T + 1);
	t0 = now_ms();
	ctl("restart bridge");
	int amb = 0, notup = 0, re = 0;
	while (!re && now_ms() - t0 < 8000) {
		if (read_link(&V, &lv) == 0) {
			amb += lv.state == 4;
			notup += lv.state != 3;
			re = lv.state == 3 && lv.peer_id != id0 && read_link(&T, &lt) == 0 && lt.state == 3;
		}
		sleep_ms(20);
	}
	use(&T);
	wait_new_pid("bridge", b0, "ready", 5000);
	EXPECT(re && amb == 0,
	       "the topside's bridge restarted (a new id): linked again after %.2f s; the vehicle's link not up in %d "
	       "samples of 20 ms, ambiguous in %d",
	       (now_ms() - t0) / 1e3, notup, amb);

	/* The topside off for 3.5 s and back: the vehicle's link is down by then
	 * (3 s), and the beacon of the bridge before is not 5 s old yet. */
	atomic_store(&air_what, AIR_V);
	use(&T);
	manager_stop(T.mgr);
	T.mgr = 0;
	rm_tree(T.topics);
	sleep_ms(3500);
	int down = read_link(&V, &lv) == 0 && lv.state != 3;
	atomic_store(&air_id_t, ID_T + 2);
	atomic_store(&air_what, AIR_V | AIR_T);
	t0 = now_ms();
	start(&T);
	struct both s = watch_both(8000, 1);
	EXPECT(down && s.up_ms >= 0 && s.amb_v == 0,
	       "the topside off for 3.5 s and back (a new id, the old one heard under 5 s ago): link up %.2f s after its "
	       "manager's start; the vehicle ambiguous in %d samples of 20 ms",
	       (now_ms() - t0) / 1e3, s.amb_v);

	/* Beacons of earlier ids, from another address, at both sides. */
	atomic_store(&air_what, AIR_V | AIR_T | AIR_STALE);
	sleep_ms(300);
	atomic_store(&air_atk_got, 0);
	int again = restart_bridges();
	s = watch_both(8000, 1);
	EXPECT(again && s.up_ms >= 0 && s.amb_v == 0 && s.amb_t == 0,
	       "nine earlier ids of each side replayed at 10 Hz from 127.0.0.2, both bridges started again: %s (%.2f s); "
	       "ambiguous in %d and %d of %d samples; %ld datagrams sent to 127.0.0.2",
	       s.up_ms >= 0 ? "link up" : "no link", (s.up_ms >= 0 ? s.up_ms : 8000) / 1e3, s.amb_v, s.amb_t, s.n,
	       atomic_load(&air_atk_got));
	if (s.up_ms < 0) {
		/* How long it lasts: as long as the replay, and the candidates' 5 s. */
		atomic_store(&air_what, AIR_V | AIR_T);
		s = watch_both(12000, 1);
		printf("    the replay stopped: %s after %.2f s\n", s.up_ms >= 0 ? "link up" : "still no link",
		       (s.up_ms >= 0 ? s.up_ms : 12000) / 1e3);
	}

	/* The beacons now on the air, sent again from another address. */
	atomic_store(&air_what, AIR_V | AIR_T | AIR_MOVED);
	sleep_ms(300);
	atomic_store(&air_atk_got, 0);
	again = restart_bridges();
	s = watch_both(8000, 1);
	EXPECT(again && s.up_ms >= 0,
	       "the two beacons on the air sent again from 127.0.0.2 at 10 Hz, both bridges started again: %s (%.2f s); "
	       "%ld datagrams sent to 127.0.0.2",
	       s.up_ms >= 0 ? "link up" : "no link", (s.up_ms >= 0 ? s.up_ms : 8000) / 1e3, atomic_load(&air_atk_got));
	sleep_ms(500);
	read_link(&V, &lv);
	long got0 = atomic_load(&air_atk_got);
	sleep_ms(1500);
	EXPECT(s.up_ms < 0 || (strncmp(lv.peer_addr, "127.0.0.1:", 10) == 0 && atomic_load(&air_atk_got) == got0),
	       "with the link up: the vehicle's peer is %s, and %ld datagrams went to 127.0.0.2 in 1.5 s", lv.peer_addr,
	       atomic_load(&air_atk_got) - got0);

	/* Old hellos, ten a second at each side from another address. */
	atomic_store(&air_what, AIR_V | AIR_T | AIR_HELLOS);
	sleep_ms(300);
	again = restart_bridges();
	s = watch_both(8000, 1);
	EXPECT(again && s.up_ms >= 0, "an old hello of each side at the other, 10 Hz from 127.0.0.2, both bridges started again: %s (%.2f s)",
	       s.up_ms >= 0 ? "link up" : "no link", (s.up_ms >= 0 ? s.up_ms : 8000) / 1e3);

	/* A second vehicle with the key, there and answering. */
	atomic_store(&air_twin_asked, 0);
	atomic_store(&air_what, AIR_V | AIR_T | AIR_TWIN);
	sleep_ms(300);
	again = restart_bridges();
	s = watch_both(7000, 0);
	read_link(&T, &lt);
	EXPECT(again && s.up_t == 0 && s.up_v == 0 && s.amb_t > s.n / 2 && atomic_load(&air_twin_asked) > 0,
	       "two vehicles with the key, both answering, for 7 s: the topside up in %d of %d samples, ambiguous in %d "
	       "('%s'); the second vehicle was sent %ld hellos",
	       s.up_t, s.n, s.amb_t, lt.detail, atomic_load(&air_twin_asked));
	atomic_store(&air_what, AIR_V | AIR_T);
	s = watch_both(15000, 1);
	EXPECT(s.up_ms >= 0, "the second vehicle gone: %s after %.2f s", s.up_ms >= 0 ? "link up" : "no link",
	       (s.up_ms >= 0 ? s.up_ms : 15000) / 1e3);
out:
	atomic_store(&air_stop, 1);
	pthread_join(th, NULL);
	close(air_relay);
	close(air_twin);
	close(air_atk);
}

/* Peer=<address> (docs/08 "The link"): the peer is that address and port. A
 * datagram from anywhere else is not read, whatever it is: a recorded hello
 * (no key needed), and a datagram of the session itself with a sequence number
 * far ahead, which only a holder of the key can make (here the suite, as a
 * third machine that has it). Neither may make this side send anything, nor
 * move its replay window: the second, taken, would have every datagram of the
 * real peer refused as old from then on. */
static void peer_run(void)
{
	setup_side(&V, "peer", NULL);
	setup_side(&T, "peer", NULL);
	atomic_store(&fwd_rec_on, 1);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	EXPECT(up, "link up, both sides with a fixed Peer= (%s)", up ? lv.peer_addr : "-");
	if (!up)
		return;
	sleep_ms(500);
	int port, n = atomic_load(&fwd_nrec), h1 = -1, sample = -1;
	int atk = udp_bound(&port);
	uint64_t hi = 0;
	for (int i = 0; i < n; i++) {
		uint64_t seq;
		memcpy(&seq, recs[i].d + 16, 8);
		if (recs[i].d[4] == 1 && h1 < 0)
			h1 = i;
		if (recs[i].d[4] != 1) {
			sample = i;
			if (seq > hi)
				hi = seq;
		}
	}
	unsigned char buf[1500];
	read_link(&V, &lv);
	uint64_t rej0 = lv.rejected;
	long hv0 = atomic_load(&fwd_hello_v);
	for (int i = 0; h1 >= 0 && i < 50; i++) {
		sendto(atk, recs[h1].d, recs[h1].len, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
		usleep(2000);
	}
	sleep_ms(300);
	int got = 0;
	while (recv(atk, buf, sizeof buf, MSG_DONTWAIT) > 0)
		got++;
	read_link(&V, &lv);
	EXPECT(h1 >= 0 && lv.state == 3 && lv.rejected - rej0 >= 50 && atomic_load(&fwd_hello_v) == hv0 && got == 0,
	       "the topside's recorded hello from another port, 50 times: %llu refused; the vehicle sent %ld hello(s) to "
	       "its peer and %d datagram(s) back",
	       (unsigned long long)(lv.rejected - rej0), atomic_load(&fwd_hello_v) - hv0, got);

	/* The session's key for what the topside sends, from the nonces of its
	 * hello: K_down = BLAKE2s-256(key, "down" | n_v | n_t). */
	unsigned char kdown[32], tag[16];
	int keys = 0;
	for (int i = 0; i < n && !keys && sample >= 0; i++) {
		unsigned char in[36];
		if (recs[i].d[4] != 1 || recs[i].len != HELLO_LEN)
			continue;
		memcpy(in, "down", 4);
		memcpy(in + 4, recs[i].d + 24 + 72, 16);
		memcpy(in + 20, recs[i].d + 24 + 40, 16);
		blake2s(kdown, 32, TEST_KEY, 32, in, 36);
		blake2s(tag, 16, kdown, 32, recs[sample].d, (size_t)recs[sample].len - 16);
		keys = memcmp(tag, recs[sample].d + recs[sample].len - 16, 16) == 0;
	}
	unsigned char f[24 + 8 + 16];
	if (keys) {
		uint32_t bl = 8;
		uint64_t seq = hi + 1000000;
		memset(f, 0, sizeof f);
		memcpy(f, recs[sample].d, 24); /* the magic and the link's id */
		f[4] = 3;                      /* a ping */
		f[5] = 0;
		memcpy(f + 12, &bl, 4);
		memcpy(f + 16, &seq, 8);
		blake2s(f + 32, 16, kdown, 32, f, 32);
		read_link(&V, &lv);
		rej0 = lv.rejected;
		sendto(atk, f, sizeof f, 0, (const struct sockaddr *)&fwd_v, sizeof fwd_v);
	}
	int notup = 0, samples = 0;
	uint64_t t0 = now_ms();
	while (keys && now_ms() - t0 < 4500) {
		samples++;
		notup += read_link(&V, &lv) != 0 || lv.state != 3;
		sleep_ms(20);
	}
	EXPECT(keys && notup == 0 && lv.rejected - rej0 == 1,
	       "a ping of this session, its sequence number a million ahead, from another port (%s): %llu refused; the "
	       "vehicle's link not up in %d of %d samples over 4.5 s",
	       keys ? "made with the key" : "the session's key not found", (unsigned long long)(lv.rejected - rej0), notup,
	       samples);
	close(atk);
}

/* ---- Files (docs/08 "Files") --------------------------------------------------- */

static const char *FILES_CONF = "[Bridge]\nFileRate=800kbit\n"
				"[Files weights]\nDirection=down\nMaxSize=512KB\nMaxFiles=3\nAllow=admin\n"
				"[Files closed]\nDirection=down\nAllow=nobody\n"
				"[Files logs]\nDirection=up\nAllow=admin\n";
static const char *SERVE_SEND = "[Serve send]\nRequest=bridge_send_req\nReply=bridge_send_rep\n";

/* n bytes that no two offsets share a run of. */
static unsigned char *pattern(size_t n, unsigned seed)
{
	unsigned char *p = malloc(n);
	unsigned x = seed * 2654435761u + 1;
	for (size_t i = 0; p && i < n; i++) {
		x = x * 1664525u + 1013904223u;
		p[i] = (unsigned char)(x >> 24);
	}
	return p;
}

static void outbox_put(const struct side *s, const char *name, const unsigned char *d, size_t n)
{
	char p[1024];
	tfmt(p, sizeof p, "%s/var/lib/hangar/bridge/outbox/%s", s->root, name);
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	if (fd >= 0 && write(fd, d, n) != (ssize_t)n)
		printf("    short write of %s\n", p);
	if (fd >= 0)
		close(fd);
}

static void arrived_path(const struct side *s, const char *dest, const char *name, char *out, size_t len)
{
	tfmt(out, len, "%s/var/lib/hangar/bridge/files/%s/%s", s->root, dest, name);
}

/* 1 if the file is there with exactly these bytes. */
static int arrived_is(const struct side *s, const char *dest, const char *name, const unsigned char *d, size_t n)
{
	char p[1024];
	arrived_path(s, dest, name, p, sizeof p);
	struct stat st;
	if (stat(p, &st) < 0 || (size_t)st.st_size != n)
		return 0;
	unsigned char *got = malloc(n);
	int fd = open(p, O_RDONLY);
	int same = got && fd >= 0 && read(fd, got, n) == (ssize_t)n && memcmp(got, d, n) == 0;
	if (fd >= 0)
		close(fd);
	free(got);
	return same;
}

/* What the directory of a destination holds: files under their names, and
 * the bytes of the one hidden name a transfer under way has. */
static void arrived_dir(const struct side *s, const char *dest, int *visible, int *hidden, long *part_bytes)
{
	char p[1024];
	tfmt(p, sizeof p, "%s/var/lib/hangar/bridge/files/%s", s->root, dest);
	*visible = *hidden = 0;
	*part_bytes = 0;
	DIR *d = opendir(p);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (e->d_name[0] != '.') {
			(*visible)++;
			continue;
		}
		(*hidden)++;
		char f[1300];
		struct stat st;
		tfmt(f, sizeof f, "%s/%s", p, e->d_name);
		if (stat(f, &st) == 0)
			*part_bytes = (long)st.st_size;
	}
	if (d)
		closedir(d);
}

/* hangar call bridge.send on a side: the id, or 0 with the command's output. */
static uint64_t send_file(const struct side *s, const char *dest, const char *name, char *out, size_t outlen)
{
	char a[80], b[160];
	use(s);
	tfmt(a, sizeof a, "dest=%s", dest);
	tfmt(b, sizeof b, "name=%s", name);
	int rc = run_cli(out, outlen, 5000, "call", "bridge.send", a, b, "--timeout", "2000", (char *)NULL);
	if (rc != 0 || !strstr(out, "HG_OK"))
		return 0;
	return strtoull(cli_field(out, "id"), NULL, 0);
}

static hg_sub *files_sub(const struct side *s)
{
	char name[48];
	use(s);
	tfmt(name, sizeof name, "bridge.%s.files", s->role);
	uint64_t t0 = now_ms();
	hg_sub *sub;
	while (!(sub = hg_sub_open_from(name, &BRIDGE_FILE_TYPE, HG_OLDEST)) && now_ms() - t0 < 5000)
		sleep_ms(20);
	return sub;
}

/* The event of a transfer, by id (0: the next one). */
static int wait_file(hg_sub *sub, uint64_t id, long ms, struct bridge_file *out)
{
	uint64_t t0 = now_ms();
	struct hg_info in;
	memset(out, 0, sizeof *out);
	if (!sub)
		return 0;
	do {
		while (hg_next(sub, out, sizeof *out, &in) == HG_MSG)
			if (!id || out->id == id)
				return 1;
		sleep_ms(10);
	} while (now_ms() - t0 < (uint64_t)ms);
	memset(out, 0, sizeof *out);
	return 0;
}

static void hash_hex(const unsigned char *d, size_t n, char out[65])
{
	unsigned char h[32];
	blake2s(h, 32, NULL, 0, d, n);
	for (int i = 0; i < 32; i++)
		tfmt(out + 2 * i, 3, "%02x", h[i]);
}

/* Until the hidden file of the transfer under way holds `bytes`. */
static long wait_part(const struct side *s, const char *dest, long bytes, long ms)
{
	uint64_t t0 = now_ms();
	int vis, hid;
	long part = 0;
	do {
		arrived_dir(s, dest, &vis, &hid, &part);
		if (part >= bytes)
			return part;
		sleep_ms(10);
	} while (now_ms() - t0 < (uint64_t)ms);
	return part;
}

/* What has come on the vehicle's pilot.depth since the last look: the
 * count, the seqs skipped, the longest time between two. Looked at often:
 * its ring is 16 messages, a third of a second. */
struct pilot_seen {
	hg_sub *sub;
	int msgs;
	uint64_t gaps, late, prev;
};

static void pilot_look(struct pilot_seen *s)
{
	struct depth d;
	struct hg_info in;
	use(&V);
	while (s->sub && hg_next(s->sub, &d, sizeof d, &in) == HG_MSG) {
		s->msgs++;
		s->gaps += in.gap;
		if (s->prev && in.publish_ns - s->prev > s->late)
			s->late = in.publish_ns - s->prev;
		s->prev = in.publish_ns;
	}
}

static void files_run(void)
{
	char out[1024], p[1024], hex[65];
	setup_side(&V, "files", FILES_CONF);
	manifest("bridge", "[Module]\nExec=%s %s\n[Bridge]\n%s", bridge_bin, V.conf, SERVE_SEND);
	/* What a file must not hold up: a command topic and a call. */
	manifest("server", "[Module]\nExec=%s/server\n"
			   "[Serve adminonly]\nRequest=depth\nReply=depth\nAllow=admin topside:admin\n", T_toys);
	manifest("pilotread", "[Module]\nExec=%s/listener --topic pilot.depth\n[Subscribe pilot.depth]\n", T_toys);
	setup_side(&T, "files", FILES_CONF);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_FILE_CORRUPT=bad.bin %s %s\n[Bridge]\n%s", bridge_bin,
		 T.conf, SERVE_SEND);
	manifest("pilot", "[Module]\nExec=%s/talker --topic pilot.depth --rate 50\n"
			  "[Publish pilot.depth]\nType=depth\nRate=50\n", T_toys);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Call server.adminonly]\nRequest=depth\nReply=depth\n");
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	hg_sub *ev = files_sub(&V), *et = files_sub(&T);
	EXPECT(up && ev && et, "link up; bridge.vehicle.files and bridge.topside.files exist");
	if (!up || !ev || !et)
		return;
	struct bridge_file fv, ft;
	/* The pilot's topic on the vehicle, and the first call across (which
	 * waits for the request and reply topics to exist on both sides). */
	use(&V);
	struct pilot_seen ps = { 0 };
	uint64_t tw = now_ms();
	while (!(ps.sub = hg_sub_open("pilot.depth", &DEPTH_TYPE)) && now_ms() - tw < 5000)
		sleep_ms(20);
	int called = 0;
	use(&T);
	while (!called && now_ms() - tw < 10000)
		called = run_cli(out, sizeof out, 3000, "call", "server.adminonly", "depth_m=1.5", "--timeout", "1000",
				 (char *)NULL) == 0 && strstr(out, "HG_OK");
	pilot_look(&ps);
	ps.msgs = 0;
	ps.gaps = ps.late = 0;

	/* One file down: nothing under its name until it is whole and checked. */
	size_t na = 150000;
	unsigned char *a = pattern(na, 1);
	outbox_put(&T, "a.hgw", a, na);
	uint64_t t0 = now_ms();
	uint64_t id = send_file(&T, "weights", "a.hgw", out, sizeof out);
	long part = 0;
	while (part < 20000 && now_ms() - t0 < 5000) {
		part = wait_part(&V, "weights", 20000, 40);
		pilot_look(&ps);
	}
	arrived_path(&V, "weights", "a.hgw", p, sizeof p);
	int early = access(p, F_OK) == 0;
	/* A call across while the file is on its way. */
	char cout[512];
	use(&T);
	uint64_t tc = now_ms();
	int mid = run_cli(cout, sizeof cout, 3000, "call", "server.adminonly", "depth_m=2.5", "--timeout", "1000",
			  (char *)NULL) == 0 && strstr(cout, "HG_OK") && strstr(cout, "2.5");
	uint64_t call_ms = now_ms() - tc;
	long part_after = wait_part(&V, "weights", 1, 0);
	pilot_look(&ps);
	/* The pilot's topic through the transfer: every message, none late. */
	int got_v = 0;
	while (!got_v && now_ms() - t0 < 10000) {
		got_v = wait_file(ev, id, 40, &fv);
		pilot_look(&ps);
	}
	int got_t = wait_file(et, id, 3000, &ft);
	uint64_t took = now_ms() - t0;
	hash_hex(a, na, hex);
	struct stat st;
	int mode = stat(p, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
	int vis, hid;
	long pb;
	arrived_dir(&V, "weights", &vis, &hid, &pb);
	tfmt(p, sizeof p, "%s/var/lib/hangar/bridge/outbox/a.hgw", T.root);
	EXPECT(id && part >= 20000 && !early && got_v && got_t && fv.state == 2 && ft.state == 1 && fv.size == na &&
		       !strcmp(fv.blake2s, hex) && !strcmp(ft.blake2s, hex) && !strcmp(fv.name, "a.hgw") &&
		       !strcmp(fv.dest, "weights") && !strncmp(fv.from, "admin:", 6) && arrived_is(&V, "weights", "a.hgw", a, na) &&
		       mode == 0640 && vis == 1 && hid == 0 && access(p, F_OK) < 0,
	       "bridge.send of 150 KB down: with %ld bytes of it there, no a.hgw; then received (state %u) and sent (state %u), "
	       "the same bytes, the hash both sides name is the file's, from %s, mode %o, taken from the outbox; %llu ms at "
	       "FileRate=800kbit%s%s",
	       part, fv.state, ft.state, fv.from, mode, (unsigned long long)took, id ? "" : ": ", id ? "" : out);
	EXPECT(called && mid && part_after > 0 && part_after < (long)na,
	       "hangar call server.adminonly from the topside with the file on its way (%ld of 150000 bytes across when it "
	       "returned): answered, %llu ms with the command's start",
	       part_after, (unsigned long long)call_ms);
	EXPECT(ps.msgs >= (int)(took / 20) * 8 / 10 && ps.gaps == 0 && ps.late < 100000000ull,
	       "pilot.depth (command priority, 50 Hz) on the vehicle through the transfer: %d messages in %llu ms, %llu "
	       "skipped, at most %.1f ms between two",
	       ps.msgs, (unsigned long long)took, (unsigned long long)ps.gaps, (double)ps.late / 1e6);
	/* FileRate holds it: 150 KB at 100 KB/s is 1.5 s. */
	EXPECT(took >= 1200, "FileRate=800kbit: 150 KB took %llu ms (1500 at the rate)", (unsigned long long)took);

	/* The tether cut midway, for longer than the link's timeout, and mended. */
	size_t nb = 300000;
	unsigned char *b = pattern(nb, 2);
	outbox_put(&T, "b.hgw", b, nb);
	pid_t pv = pid_of(&V, "bridge"), pt = pid_of(&T, "bridge");
	id = send_file(&T, "weights", "b.hgw", out, sizeof out);
	part = wait_part(&V, "weights", 60000, 5000);
	atomic_store(&fwd_drop_pct, 100);
	t0 = now_ms();
	int down = 0;
	while (!down && now_ms() - t0 < 6000) {
		down = read_link(&V, &lv) == 0 && lv.state != 3 && read_link(&T, &lt) == 0 && lt.state != 3;
		sleep_ms(20);
	}
	long held;
	arrived_dir(&V, "weights", &vis, &hid, &held);
	arrived_path(&V, "weights", "b.hgw", p, sizeof p);
	early = access(p, F_OK) == 0;
	atomic_store(&fwd_drop_pct, 0);
	got_v = wait_file(ev, id, 12000, &fv);
	got_t = wait_file(et, id, 3000, &ft);
	EXPECT(id && part >= 60000 && down && !early && hid == 1 && held < (long)nb && got_v && got_t && fv.state == 2 &&
		       ft.state == 1 && arrived_is(&V, "weights", "b.hgw", b, nb) && pid_of(&V, "bridge") == pv &&
		       pid_of(&T, "bridge") == pt,
	       "the tether cut with %ld of 300000 bytes across, both links down, %ld bytes kept under a hidden name and no "
	       "b.hgw; mended (the same keys, the same stream, resent from what was not acknowledged): received (state %u) "
	       "and sent (state %u), the same bytes, neither bridge restarted",
	       part, held, fv.state, ft.state);

	/* The receiving bridge restarted midway: what it had is gone with it,
	 * and the file comes again from its first byte. */
	size_t nc = 200000;
	unsigned char *c = pattern(nc, 3);
	outbox_put(&T, "c.hgw", c, nc);
	id = send_file(&T, "weights", "c.hgw", out, sizeof out);
	part = wait_part(&V, "weights", 60000, 5000);
	use(&V);
	pv = pid_of(&V, "bridge");
	ctl("restart bridge");
	int back = wait_new_pid("bridge", pv, "ready", 5000);
	hg_sub *ev2 = files_sub(&V); /* the topic is the new instance's */
	got_t = wait_file(et, id, 15000, &ft);
	got_v = wait_file(ev2, id, 3000, &fv);
	arrived_dir(&V, "weights", &vis, &hid, &pb);
	EXPECT(id && part >= 60000 && back && got_t && ft.state == 1 && got_v && fv.state == 2 &&
		       arrived_is(&V, "weights", "c.hgw", c, nc) && hid == 0,
	       "the vehicle's bridge restarted with %ld of 200000 bytes across: the file sent again from the start, received "
	       "(state %u), the same bytes, nothing left under a hidden name",
	       part, fv.state);
	if (ev2) {
		hg_sub_close(ev);
		ev = ev2;
	}

	/* A byte changed on the way (the topside's bridge changes it after
	 * hashing, told to by its environment): never visible. */
	unsigned char *bad = pattern(40000, 4);
	outbox_put(&T, "bad.bin", bad, 40000);
	id = send_file(&T, "weights", "bad.bin", out, sizeof out);
	got_v = wait_file(ev, id, 8000, &fv);
	got_t = wait_file(et, id, 3000, &ft);
	arrived_dir(&V, "weights", &vis, &hid, &pb);
	arrived_path(&V, "weights", "bad.bin", p, sizeof p);
	EXPECT(id && got_v && got_t && fv.state == 4 && ft.state == 3 && strstr(fv.reason, "hash") && strstr(ft.reason, "hash") &&
		       access(p, F_OK) < 0 && hid == 0,
	       "a file with one byte changed on the way: the vehicle says '%s' (state %u), the topside is told (state %u, '%s'); "
	       "no bad.bin, nothing hidden left",
	       fv.reason, fv.state, ft.state, ft.reason);

	/* Two sends at once: both taken, one after the other. And MaxFiles=3:
	 * a.hgw, b.hgw, c.hgw are there, so the two oldest go. */
	unsigned char *d1 = pattern(50000, 5), *d2 = pattern(30000, 6);
	outbox_put(&T, "d1.hgw", d1, 50000);
	outbox_put(&T, "d2.hgw", d2, 30000);
	uint64_t id1 = send_file(&T, "weights", "d1.hgw", out, sizeof out);
	uint64_t id2 = send_file(&T, "weights", "d2.hgw", out, sizeof out);
	struct bridge_file f1, f2;
	int g1 = wait_file(ev, 0, 8000, &f1), g2 = wait_file(ev, 0, 8000, &f2);
	arrived_dir(&V, "weights", &vis, &hid, &pb);
	arrived_path(&V, "weights", "a.hgw", p, sizeof p);
	int a_gone = access(p, F_OK) < 0;
	arrived_path(&V, "weights", "b.hgw", p, sizeof p);
	int b_gone = access(p, F_OK) < 0;
	EXPECT(id1 && id2 && id1 != id2 && g1 && g2 && f1.id == id1 && f2.id == id2 && f1.state == 2 && f2.state == 2 &&
		       arrived_is(&V, "weights", "d1.hgw", d1, 50000) && arrived_is(&V, "weights", "d2.hgw", d2, 30000),
	       "two sends, the second while the first is under way: both received, in the order asked, each its own bytes");
	EXPECT(vis == 3 && a_gone && b_gone && arrived_is(&V, "weights", "c.hgw", c, nc),
	       "MaxFiles=3: %d files kept on the vehicle, the two oldest (a.hgw, b.hgw) removed", vis);

	/* What send refuses, and why. */
	struct refusal {
		const char *dest, *name, *want, *what;
	} ref[] = {
		{ "weights", "../x.hgw", "name:", "a name with a path" },
		{ "weights", ".hidden", "name:", "a hidden name" },
		{ "weights", "absent.hgw", "No such file", "a file that is not in the outbox" },
		{ "weights", "big.hgw", "MaxSize", "a file over the section's MaxSize" },
		{ "weights", "link.hgw", "outbox/link.hgw", "a symbolic link in the outbox (to the bridge's key)" },
		{ "weights", "hard.hgw", "one link", "a second hard link to a file elsewhere (the bridge's key)" },
		{ "nosuch", "x.hgw", "no such [Files]", "a destination bridge.conf does not have" },
		{ "logs", "x.hgw", "no such [Files]", "a destination that goes the other way" },
		{ "closed", "x.hgw", "Allow", "a destination whose Allow= does not name the caller" },
	};
	unsigned char *big = pattern(600000, 7);
	outbox_put(&T, "big.hgw", big, 600000);
	outbox_put(&T, "x.hgw", d2, 100);
	char key[1024], slink[1024];
	tfmt(key, sizeof key, "%s/etc/hangar/bridge.key", T.root);
	tfmt(slink, sizeof slink, "%s/var/lib/hangar/bridge/outbox/link.hgw", T.root);
	if (symlink(key, slink) < 0)
		printf("    symlink: %s\n", strerror(errno));
	char hard[1024];
	tfmt(hard, sizeof hard, "%s/var/lib/hangar/bridge/outbox/hard.hgw", T.root);
	if (link(key, hard) < 0)
		printf("    link: %s\n", strerror(errno));
	int refused = 0;
	char last[200] = "";
	for (size_t i = 0; i < sizeof ref / sizeof ref[0]; i++) {
		id = send_file(&T, ref[i].dest, ref[i].name, out, sizeof out);
		if (!id && strstr(out, "HG_E_REFUSED") && strstr(out, ref[i].want))
			refused++;
		else
			tfmt(last, sizeof last, "%s: %.150s", ref[i].what, out);
	}
	arrived_dir(&V, "weights", &vis, &hid, &pb);
	tfmt(p, sizeof p, "%s/var/lib/hangar/bridge/outbox/x.hgw", T.root);
	int quiet = vis == 3 && hid == 0 && access(p, F_OK) == 0 && access(slink, F_OK) == 0;
	EXPECT(refused == (int)(sizeof ref / sizeof ref[0]) && quiet,
	       "send refuses, each with its reason: a name with a path, a hidden name, a missing file, one over MaxSize, a "
	       "symbolic link, a hard link to a file elsewhere, an unknown destination, one of the other direction, a "
	       "caller the section does not allow "
	       "(%d of %zu%s%s); nothing was taken from the outbox, nothing arrived",
	       refused, sizeof ref / sizeof ref[0], last[0] ? "; " : "", last);

	/* send is never the other side's to call: the topside's requests went
	 * down on its mirrored admin topic, and the vehicle's bridge answered
	 * none of them. */
	use(&V);
	EXPECT(topic_seq("bridge.rep") == 0 && topic_seq("hangar.topside.admin.req") > 0,
	       "the topside's %llu requests reached the vehicle on hangar.topside.admin.req; its bridge answered %llu of them",
	       (unsigned long long)topic_seq("hangar.topside.admin.req"), (unsigned long long)topic_seq("bridge.rep"));

	/* And up: the vehicle sends, the topside keeps. */
	outbox_put(&V, "dive.log", d1, 50000);
	id = send_file(&V, "logs", "dive.log", out, sizeof out);
	got_t = wait_file(et, id, 8000, &ft);
	got_v = wait_file(ev, id, 3000, &fv);
	EXPECT(id && got_t && got_v && ft.state == 2 && fv.state == 1 && arrived_is(&T, "logs", "dive.log", d1, 50000),
	       "a file up ([Files logs] Direction=up): received on the topside (state %u), sent (state %u), the same bytes%s%s",
	       ft.state, fv.state, id ? "" : ": ", id ? "" : out);

	/* The link was never without its pings while files moved. */
	read_link(&V, &lv);
	EXPECT(lv.state == 3 && lv.rtt_ns < 50000000ull, "after the files: the link up, round trip %.2f ms",
	       (double)lv.rtt_ns / 1e6);
	free(a);
	free(b);
	free(c);
	free(bad);
	free(d1);
	free(d2);
	free(big);
	hg_sub_close(ev);
	hg_sub_close(et);
}

/* A transfer that cannot end. Both bridges give a file up after 1.5 s without
 * progress here (60 s in use). The sender gone midway (its bridge restarted:
 * the queue is the process's): the receiver drops what it had. The link
 * down for longer than that: the sender says so. Neither leaves a file. */
static void filestall_run(void)
{
	char out[1024], p[1024];
	setup_side(&V, "fstall", FILES_CONF);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_FILE_STALL_MS=1500 %s %s\n[Bridge]\n%s", bridge_bin,
		 V.conf, SERVE_SEND);
	setup_side(&T, "fstall", FILES_CONF);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_FILE_STALL_MS=1500 %s %s\n[Bridge]\n%s", bridge_bin,
		 T.conf, SERVE_SEND);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	hg_sub *ev = files_sub(&V), *et = files_sub(&T);
	if (!up || !ev || !et) {
		EXPECT(0, "link up, the files topics there");
		return;
	}
	struct bridge_file fv, ft;
	size_t n = 300000;
	unsigned char *a = pattern(n, 9);
	outbox_put(&T, "a.hgw", a, n);
	uint64_t id = send_file(&T, "weights", "a.hgw", out, sizeof out);
	long part = wait_part(&V, "weights", 50000, 5000);
	use(&T);
	pid_t pt = pid_of(&T, "bridge");
	ctl("restart bridge");
	int back = wait_new_pid("bridge", pt, "ready", 5000);
	int got_v = wait_file(ev, id, 8000, &fv);
	int vis, hid;
	long pb;
	arrived_dir(&V, "weights", &vis, &hid, &pb);
	EXPECT(id && part >= 50000 && back && got_v && fv.state == 4 && vis == 0 && hid == 0,
	       "the sending bridge restarted with %ld of 300000 bytes across: the vehicle gives the file up (state %u, '%s'); "
	       "nothing visible, nothing hidden left",
	       part, fv.state, fv.reason);

	/* The link down from before the send until past the wait. */
	hg_sub *et2 = files_sub(&T);
	wait_link(&T, 1, 8000, &lt);
	atomic_store(&fwd_drop_pct, 100);
	outbox_put(&T, "b.hgw", a, n);
	id = send_file(&T, "weights", "b.hgw", out, sizeof out);
	int got_t = wait_file(et2, id, 8000, &ft);
	atomic_store(&fwd_drop_pct, 0);
	int relinked = wait_link(&V, 1, 10000, &lv) && wait_link(&T, 1, 10000, &lt);
	/* What was on the stream when the link went (the begin, then the
	 * abort) may still come: the vehicle drops it as it reads it. */
	/* Two more, the second asked while the first is on its way: 250 KB at
	 * 100 KB/s is longer than the 1.5 s wait, and the second waits behind
	 * it for all of that without being given up. */
	outbox_put(&T, "c.hgw", a, 250000);
	outbox_put(&T, "d.hgw", a, 20000);
	uint64_t idc = send_file(&T, "weights", "c.hgw", out, sizeof out);
	uint64_t idd = send_file(&T, "weights", "d.hgw", out, sizeof out);
	struct bridge_file fc, fd;
	int got_c = wait_file(et2, idc, 10000, &fc);
	int got_d = wait_file(et2, idd, 8000, &fd);
	arrived_dir(&V, "weights", &vis, &hid, &pb);
	arrived_path(&V, "weights", "b.hgw", p, sizeof p);
	EXPECT(id && got_t && ft.state == 3 && relinked && access(p, F_OK) < 0,
	       "the tether cut for longer than the wait: the topside's send ends failed (state %u, '%s'); with the link back, "
	       "no b.hgw on the vehicle",
	       ft.state, ft.reason);
	EXPECT(idc && got_c && fc.state == 1 && arrived_is(&V, "weights", "c.hgw", a, 250000) && idd && got_d &&
		       fd.state == 1 && arrived_is(&V, "weights", "d.hgw", a, 20000) && vis == 2 && hid == 0,
	       "the next two files after both failures, the second queued behind the first for longer than the wait: sent "
	       "(states %u, %u), the same bytes; they are the two files there, nothing hidden",
	       fc.state, fd.state);
	free(a);
	hg_sub_close(ev);
	hg_sub_close(et);
	if (et2)
		hg_sub_close(et2);
}

/* A link made anew under a file, with both bridges alive. In use a link that
 * comes back keeps its keys and its streams, and a file goes on by the
 * stream's own resends (files_run). Here both bridges forget their keys when
 * the link goes down (told to by the environment), so the link that returns
 * is a new one with new streams: the sender begins the file again and goes
 * on from the byte the receiver says it has. */
static void filerekey_run(void)
{
	char out[1024];
	setup_side(&V, "frekey", FILES_CONF);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_FORGET_KEYS=1 %s %s\n[Bridge]\n%s", bridge_bin, V.conf,
		 SERVE_SEND);
	setup_side(&T, "frekey", FILES_CONF);
	manifest("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_FORGET_KEYS=1 %s %s\n[Bridge]\n%s", bridge_bin, T.conf,
		 SERVE_SEND);
	start(&V);
	start(&T);
	struct bridge_link lv, lt;
	int up = wait_link(&V, 1, 8000, &lv) && wait_link(&T, 1, 3000, &lt);
	hg_sub *ev = files_sub(&V), *et = files_sub(&T);
	if (!up || !ev || !et) {
		EXPECT(0, "link up, the files topics there");
		return;
	}
	struct bridge_file fv, ft;
	size_t n = 300000;
	unsigned char *a = pattern(n, 11);
	outbox_put(&T, "a.hgw", a, n);
	pid_t pv = pid_of(&V, "bridge"), pt = pid_of(&T, "bridge");
	uint64_t id = send_file(&T, "weights", "a.hgw", out, sizeof out);
	long part = wait_part(&V, "weights", 60000, 5000);
	atomic_store(&fwd_drop_pct, 100);
	uint64_t t0 = now_ms();
	int down = 0;
	while (!down && now_ms() - t0 < 6000) {
		down = read_link(&V, &lv) == 0 && lv.state != 3 && read_link(&T, &lt) == 0 && lt.state != 3;
		sleep_ms(20);
	}
	int vis, hid;
	long held;
	arrived_dir(&V, "weights", &vis, &hid, &held);
	atomic_store(&fwd_drop_pct, 0);
	int got_v = wait_file(ev, id, 15000, &fv), got_t = wait_file(et, id, 3000, &ft);
	char lp[1024];
	tfmt(lp, sizeof lp, "%s/run/hangar/log/bridge.log", T.root);
	const char *log = read_file(lp);
	const char *from = log ? strstr(log, ": on from byte ") : NULL;
	long at = from ? atol(from + 15) : -1;
	EXPECT(id && part >= 60000 && down && got_v && got_t && fv.state == 2 && ft.state == 1 &&
		       arrived_is(&V, "weights", "a.hgw", a, n) && pid_of(&V, "bridge") == pv && pid_of(&T, "bridge") == pt,
	       "the tether cut with %ld of 300000 bytes across and the keys forgotten on both sides; a new link: received "
	       "(state %u) and sent (state %u), the same bytes, neither bridge restarted",
	       part, fv.state, ft.state);
	EXPECT(at > 0 && at == held, "the sender began again on the new link and went on from byte %ld, what the vehicle had (%ld)",
	       at, held);
	free(a);
	hg_sub_close(ev);
	hg_sub_close(et);
}

static const struct run {
	const char *name;
	void (*fn)(void);
} runs[] = {
	{ "main", main_run },
	{ "mismatch", mismatch_run },
	{ "clocks", clocks_run },
	{ "hold", hold_run },
	{ "replay", replay_run },
	{ "auto", auto_run },
	{ "writer", writer_run },
	{ "limits", limits_run },
	{ "beacon", beacon_run },
	{ "peer", peer_run },
	{ "files", files_run },
	{ "filestall", filestall_run },
	{ "filerekey", filerekey_run },
};

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (getenv("HANGAR_TEST_TIMENS"))
		timens_enter(argv);
	if (argc == 3 && strcmp(argv[1], "--clock-ahead-of") == 0)
		return hg_now_ns() / 1000000000u + 5 >= strtoull(argv[2], NULL, 10) + TIMENS_S ? 0 : 1;
	if (argc == 4 && strcmp(argv[1], "--tester") == 0)
		tester(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "--tester-zero") == 0)
		tester_zero(argv[2], argv[3]);
	if (argc == 4 && strcmp(argv[1], "--tester-one") == 0)
		tester_one(argv[2], argv[3]);
	const char *only = argc == 3 && strcmp(argv[1], "--only") == 0 ? argv[2] : NULL;
	ssize_t sl = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
	self_path[sl > 0 ? sl : 0] = 0;
	T_hangard = getenv("HANGARD_BIN");
	T_hangar = getenv("HANGAR_BIN");
	T_toys = getenv("HANGARD_TOYS");
	bridge_bin = getenv("BRIDGE_BIN");
	if (!T_hangard || !T_toys || !bridge_bin) {
		printf("HANGARD_BIN, HANGARD_TOYS and BRIDGE_BIN must be set\n");
		return 2;
	}
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_PEER_TOPIC_DIR");
	unsetenv("NOTIFY_SOCKET");
	signal(SIGPIPE, SIG_IGN);
	recs = malloc(REC_MAX * sizeof *recs);
	if (!recs)
		return 2;
	fwd_sock = udp_bound(&fwd_port);
	struct timeval tv = { 0, 100000 };
	setsockopt(fwd_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	pthread_t th;
	pthread_create(&th, NULL, forwarder, NULL);

	for (size_t run = 0; run < sizeof runs / sizeof runs[0]; run++) {
		if (only && strcmp(only, runs[run].name) != 0)
			continue;
		printf("-- %s\n", runs[run].name);
		atomic_store(&fwd_drop_pct, 0);
		atomic_store(&fwd_drop_kind_v, 0);
		atomic_store(&fwd_rec_on, 0);
		atomic_store(&fwd_nrec, 0);
		V.peer = T.peer = NULL;
		V.dport = T.dport = 0;
		V.port = free_port();
		T.port = free_port();
		fwd_v = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)V.port) };
		fwd_t = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)T.port) };
		fwd_v.sin_addr.s_addr = fwd_t.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		runs[run].fn();
		if (failed) {
			printf("    logs: %s, %s\n", V.log, T.log);
			setenv("HD_KEEP", "1", 1);
		}
		stop_both();
	}
	atomic_store(&fwd_stop, 1);
	pthread_join(th, NULL);
	return failed;
}
