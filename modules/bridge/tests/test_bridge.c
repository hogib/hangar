// SPDX-License-Identifier: GPL-3.0-or-later
/* The bridge, end to end on loopback (docs/08-bridge.md): two managers, a
 * vehicle and a topside, each with its own root and topic directory, each
 * with a bridge; between the two bridges a UDP forwarder in this process,
 * which drops, replays, modifies and forges datagrams when told to. No
 * HANGAR_PEER_TOPIC_DIR: only the bridge joins the two.
 *
 * Takes HANGARD_BIN, HANGAR_BIN, HANGARD_TOYS, HANGAR_MSG_SRC and BRIDGE_BIN
 * from the environment (modules/bridge/bridge.mk sets them). */
#include "../../../hangard/tests/hd_test.h"

#include "blake2s.h"
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
#include <stdatomic.h>

struct side {
	const char *role;
	char root[256], modules[512], msg[512], control[512], topics[512], log[512], conf[512];
	int port;
	pid_t mgr;
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
static atomic_int fwd_drop_pct, fwd_stop, fwd_tamper; /* tamper: 1 replay, 2 modify, 3 forge */
static atomic_long fwd_count;

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
		rng = rng * 1103515245u + 12345u;
		if ((int)((rng >> 16) % 100) < atomic_load(&fwd_drop_pct))
			continue;
		sendto(fwd_sock, d, (size_t)n, 0, (const struct sockaddr *)to, sizeof *to);
		int t = atomic_exchange(&fwd_tamper, 0);
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
	tfmt(text, sizeof text,
	     "[Bridge]\nPair=test\nKey=%s/etc/hangar/bridge.key\nPort=%d\nPeer=127.0.0.1:%d\nMsgDir=%s\n%s%s",
	     s->root, s->port, fwd_port, s->msg, MIRRORS, extra_conf ? extra_conf : "");
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
		if (read_link(s, out) == 0 && out->state == 3 && out->config_match == want_match &&
		    (want_match || out->detail[0]))
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

/* ---- The tester: calls from the topside ---------------------------------------- */

/* Run as a fresh process (this binary again, --tester <topside root> <out>):
 * the suite has used lib/hg unwired already, and a module's wiring is read
 * once per process. */
static void tester(const char *root, const char *out_path)
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
				"a hello under the wrong key" };
	for (int t = 1; t <= 3; t++) {
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

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 4 && strcmp(argv[1], "--tester") == 0)
		tester(argv[2], argv[3]);
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
	fwd_sock = udp_bound(&fwd_port);
	struct timeval tv = { 0, 100000 };
	setsockopt(fwd_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	pthread_t th;
	pthread_create(&th, NULL, forwarder, NULL);

	for (int run = 0; run < 2; run++) {
		V.port = free_port();
		T.port = free_port();
		fwd_v = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)V.port) };
		fwd_t = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)T.port) };
		fwd_v.sin_addr.s_addr = fwd_t.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (run == 0)
			main_run();
		else
			mismatch_run();
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
