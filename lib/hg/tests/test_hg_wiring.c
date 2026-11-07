// SPDX-License-Identifier: LGPL-3.0-or-later
// The wiring text (docs/03-manager.md "Wiring"): every line kind parsed
// with its keys, the refusals, durations and rates, the glob matcher, and
// the library wired through a sealed memfd in $HANGAR_WIRING refusing topics
// the wiring does not list.
#include "hg_test.h"

static const char WIRING[] =
	"hangar-wiring 1\n"
	"module depth_hold\n"
	"# a comment line\n"
	"sub nav.depth type=depth hash=9f3a61c20b7e4d15 rate=50\n"
	"sub nav.imu type=imu hash=0c41e2aa93f81d70 rate=50\n"
	"sub hangar.vehicle.admin.req type=req:hangar hash=0000000000000001 slots=16 implicit\n"
	"subglob *.req implicit\n"
	"subglob nav.*\n"
	"sub safety.rep type=rep:safety hash=0000000000000002 slots=16 implicit\n"
	"pub ctl.depth_hold type=wrench hash=77d0b3a1c5e29f04 slots=16 rate=50 stale_after=60ms inputs=nav.depth,nav.imu\n"
	"pub depth_hold.req type=req:depth_hold hash=0000000000000003 slots=16 implicit\n"
	"pub depth_hold.rep type=rep:depth_hold hash=0000000000000004 slots=16 implicit sources=*.req,hangar.vehicle.admin.req\n"
	"pub depth_hold.state type=state hash=0000000000000005 rate=0.5 inputs=vehicle:cam.pilot,ctl.depth_hold\n"
	"serve reset req=empty hash=000000000000000a size=8 rep=empty hash=000000000000000b size=8 deadline=100ms allow=topside:pilot_console,admin\n"
	"call safety.disarm req=empty hash=000000000000000c size=8 rep=empty hash=000000000000000d size=16 remote=0\n"
	"call topside.ping req=ping hash=000000000000000e size=24 rep=pong hash=000000000000000f size=32 remote=1\n"
	"mirror cam.* type=any rate=30 stale_after=100ms\n"
	"mirror topside.ping.rep type=any\n";

static struct hg_wiring w;

static void test_every_line_kind(void)
{
	char err[128] = "";
	CHECK_EQ_INT(hg_wiring_parse(WIRING, strlen(WIRING), &w, err, sizeof err), 0);
	if (err[0])
		printf("    %s\n", err);
	CHECK(strcmp(w.module, "depth_hold") == 0);
	CHECK_EQ_INT(w.n_subs, 4);
	CHECK(strcmp(w.subs[0].topic, "nav.depth") == 0);
	CHECK(strcmp(w.subs[0].type, "depth") == 0);
	CHECK(w.subs[0].hash == 0x9f3a61c20b7e4d15ull);
	CHECK_EQ_INT(w.subs[0].rate_mhz, 50000);
	CHECK_EQ_INT(w.subs[0].implicit, 0);
	CHECK(strcmp(w.subs[2].topic, "hangar.vehicle.admin.req") == 0);
	CHECK_EQ_INT(w.subs[2].slots, 16);
	CHECK_EQ_INT(w.subs[2].implicit, 1);
	CHECK(strcmp(w.subs[3].type, "rep:safety") == 0);
	CHECK_EQ_INT(w.n_globs, 2);
	CHECK(strcmp(w.globs[0].pattern, "*.req") == 0);
	CHECK_EQ_INT(w.globs[0].implicit, 1);
	CHECK_EQ_INT(w.globs[1].implicit, 0);
	CHECK_EQ_INT(w.n_pubs, 4);
	const struct hg_wiring_pub *p = hg_wiring_find_pub(&w, "ctl.depth_hold");
	CHECK(p != NULL);
	if (p) {
		CHECK(strcmp(p->type, "wrench") == 0);
		CHECK(p->hash == 0x77d0b3a1c5e29f04ull);
		CHECK_EQ_INT(p->slots, 16);
		CHECK_EQ_INT(p->rate_mhz, 50000);
		CHECK_EQ_INT(p->stale_after_ns, 60000000);
		CHECK_EQ_INT(p->n_inputs, 2);
		CHECK(strcmp(p->inputs[0], "nav.depth") == 0);
		CHECK(strcmp(p->inputs[1], "nav.imu") == 0);
		CHECK_EQ_INT(p->n_sources, 0);
		CHECK_EQ_INT(p->implicit, 0);
	}
	p = hg_wiring_find_pub(&w, "depth_hold.rep");
	CHECK(p && p->implicit);
	CHECK(p && p->n_sources == 2);
	CHECK(p && strcmp(p->sources[0], "*.req") == 0);
	CHECK(p && p->rate_mhz == 0 && p->stale_after_ns == 0); // an event topic
	p = hg_wiring_find_pub(&w, "depth_hold.state");
	CHECK(p && p->rate_mhz == 500);
	CHECK(p && p->stale_after_ns == 6000000000ull); // three periods of 2 s, the default
	CHECK(p && strcmp(p->inputs[0], "vehicle:cam.pilot") == 0);
	CHECK(hg_wiring_find_pub(&w, "nav.depth") == NULL);
	CHECK_EQ_INT(w.n_serves, 1);
	CHECK(strcmp(w.serves[0].method, "reset") == 0);
	CHECK(strcmp(w.serves[0].m.req_type, "empty") == 0);
	CHECK(w.serves[0].m.req_hash == 0xa);
	CHECK_EQ_INT(w.serves[0].m.req_size, 8);
	CHECK(w.serves[0].m.rep_hash == 0xb);
	CHECK_EQ_INT(w.serves[0].deadline_ns, 100000000);
	CHECK_EQ_INT(w.serves[0].n_allow, 2);
	CHECK(strcmp(w.serves[0].allow[0], "topside:pilot_console") == 0);
	CHECK(strcmp(w.serves[0].allow[1], "admin") == 0);
	CHECK_EQ_INT(w.n_calls, 2);
	CHECK(strcmp(w.calls[0].target, "safety.disarm") == 0);
	CHECK(w.calls[0].m.req_hash == 0xc && w.calls[0].m.rep_hash == 0xd);
	CHECK_EQ_INT(w.calls[0].m.rep_size, 16);
	CHECK_EQ_INT(w.calls[0].remote, 0);
	CHECK(strcmp(w.calls[1].m.rep_type, "pong") == 0);
	CHECK_EQ_INT(w.calls[1].m.req_size, 24);
	CHECK_EQ_INT(w.calls[1].remote, 1);
	CHECK_EQ_INT(w.n_mirrors, 2);
	CHECK(strcmp(w.mirrors[0].pattern, "cam.*") == 0);
	CHECK_EQ_INT(w.mirrors[0].rate_mhz, 30000);
	CHECK_EQ_INT(w.mirrors[0].stale_after_ns, 100000000);
	CHECK_EQ_INT(w.mirrors[1].rate_mhz, 0);
	CHECK(hg_wiring_match_mirror(&w, "cam.pilot") == &w.mirrors[0]);
	CHECK(hg_wiring_match_mirror(&w, "topside.ping.rep") == &w.mirrors[1]);
	CHECK(hg_wiring_match_mirror(&w, "nav.depth") == NULL);
	CHECK(hg_wiring_find_sub(&w, "nav.imu") == &w.subs[1]);
	CHECK(hg_wiring_find_sub(&w, "nav.other") == NULL);
	CHECK(hg_wiring_find_glob(&w, "*.req") == &w.globs[0]);
	CHECK(hg_wiring_find_glob(&w, "*.rep") == NULL);
	CHECK(hg_wiring_glob_matches(&w, "pilot_console.req"));
	CHECK(hg_wiring_glob_matches(&w, "nav.state.depth"));
	CHECK(!hg_wiring_glob_matches(&w, "thr.cmd"));
}

static void refuse(const char *text, const char *expect)
{
	struct hg_wiring x;
	char err[128] = "";
	CHECK_EQ_INT(hg_wiring_parse(text, strlen(text), &x, err, sizeof err), -1);
	CHECK_EQ_INT(errno, EINVAL);
	if (!strstr(err, expect))
		printf("    expected '%s' in: %s\n", expect, err);
	CHECK(strstr(err, expect) != NULL);
}

static void test_refusals(void)
{
	refuse("", "empty");
	refuse("hangar-wiring 2\nmodule m\n", "hangar-wiring 1");
	refuse("module m\n", "hangar-wiring 1");
	refuse("hangar-wiring 1\n", "module");
	refuse("hangar-wiring 1\nmodule m\nfoo bar\n", "unknown line kind");
	refuse("hangar-wiring 1\nmodule m\nsub\n", "sub needs a topic");
	refuse("hangar-wiring 1\nmodule m\nsub a.b rate=fast\n", "sub line");
	refuse("hangar-wiring 1\nmodule m\npub a.b slots=16\n", "type=");
	refuse("hangar-wiring 1\nmodule m\npub a.b type=t slots=3\n", "pub line");
	refuse("hangar-wiring 1\nmodule m\npub a.b type=t hash=xyz\n", "pub line");
	refuse("hangar-wiring 1\nmodule m\npub a.b type=t inputs=a,b,c,d,e\n", "at most 4");
	refuse("hangar-wiring 1\nmodule m\nserve x rep=t\n", "req= and rep=");
	refuse("hangar-wiring 1\nmodule m\nserve x req=t hash=0000000000000001 rep=t size=x\n", "bad key");
	refuse("hangar-wiring 1\nmodule m\nserve x hash=0000000000000001 req=t rep=t\n", "bad key");
	refuse("hangar-wiring 1\nmodule m\ncall nomethod req=t rep=t\n", "call needs");
	refuse("hangar-wiring 1\nmodule m\ncall a.b req=t rep=t remote=2\n", "bad key");
	refuse("hangar-wiring 1\nmodule m\nmirror a.* type=depth\n", "mirror line");
	refuse("hangar-wiring 1\nmodule m\nsubglob\n", "subglob needs");
	// The 128th subscription by name: refused as the manager refuses it.
	static char many[16384];
	int n = snprintf(many, sizeof many, "hangar-wiring 1\nmodule m\n");
	for (int i = 0; i < 128; i++)
		n += snprintf(many + n, sizeof many - (size_t)n, "sub t%d\n", i);
	refuse(many, "127");
	n = snprintf(many, sizeof many, "hangar-wiring 1\nmodule m\n");
	for (int i = 0; i < 127; i++)
		n += snprintf(many + n, sizeof many - (size_t)n, "sub t%d\n", i);
	struct hg_wiring x;
	CHECK_EQ_INT(hg_wiring_parse(many, strlen(many), &x, NULL, 0), 0);
	CHECK_EQ_INT(x.n_subs, 127);
}

static void test_durations_and_rates(void)
{
	CHECK_EQ_INT(hg_parse_duration("60ms"), 60000000);
	CHECK_EQ_INT(hg_parse_duration("1s"), 1000000000);
	CHECK_EQ_INT(hg_parse_duration("2.5s"), 2500000000ll);
	CHECK_EQ_INT(hg_parse_duration("250us"), 250000);
	CHECK_EQ_INT(hg_parse_duration("7ns"), 7);
	CHECK_EQ_INT(hg_parse_duration("0"), 0);
	CHECK_EQ_INT(hg_parse_duration("0.5ms"), 500000);
	CHECK_EQ_INT(hg_parse_duration("60"), -1);
	CHECK_EQ_INT(hg_parse_duration("ms"), -1);
	CHECK_EQ_INT(hg_parse_duration("1.5ns"), -1);
	CHECK_EQ_INT(hg_parse_duration("1m"), -1);
	CHECK_EQ_INT(hg_parse_rate_mhz("50"), 50000);
	CHECK_EQ_INT(hg_parse_rate_mhz("0.5"), 500);
	CHECK_EQ_INT(hg_parse_rate_mhz("12.345"), 12345);
	CHECK_EQ_INT(hg_parse_rate_mhz("0"), 0);
	CHECK_EQ_INT(hg_parse_rate_mhz("1.2345"), -1);
	CHECK_EQ_INT(hg_parse_rate_mhz("50hz"), -1);
	CHECK_EQ_INT(hg_parse_rate_mhz(""), -1);
}

static void test_glob_match(void)
{
	CHECK(hg_glob_match("*", "anything.at.all"));
	CHECK(hg_glob_match("*.req", "caller.req"));
	CHECK(hg_glob_match("*.req", "hangar.vehicle.admin.req"));
	CHECK(!hg_glob_match("*.req", "caller.rep"));
	CHECK(!hg_glob_match("*.req", "req"));
	CHECK(hg_glob_match("nav.*", "nav.depth"));
	CHECK(hg_glob_match("nav.*", "nav.state.depth"));
	CHECK(!hg_glob_match("nav.*", "nav"));
	CHECK(!hg_glob_match("nav.*", "navi.depth"));
	CHECK(hg_glob_match("cam.?", "cam.a"));
	CHECK(!hg_glob_match("cam.?", "cam.ab"));
	CHECK(hg_glob_match("a*b*c", "axxbyyc"));
	CHECK(!hg_glob_match("a*b*c", "axxbyy"));
	CHECK(hg_glob_match("exact", "exact"));
	CHECK(!hg_glob_match("exact", "exac"));
}

static void test_wired_module_refuses_undeclared(void)
{
	// The library, wired through a sealed memfd as the manager hands it.
	char text[2048];
	snprintf(text, sizeof text,
		 "hangar-wiring 1\nmodule wiretest\n"
		 "sub wr.in type=words hash=%016llx\n"
		 "sub wr.any\n"
		 "subglob wr.glob.*\n"
		 "pub wr.out type=words hash=%016llx slots=4 rate=10\n"
		 "pub wr.frame type=frame\n"
		 "mirror wr.mirror.* type=any rate=5 stale_after=1s\n",
		 (unsigned long long)WORDS_TYPE->hash, (unsigned long long)WORDS_TYPE->hash);
	int fd = hg_stub_memfd(text);
	CHECK(fd >= 0);
	char fds[16];
	snprintf(fds, sizeof fds, "%d", fd);
	setenv("HANGAR_WIRING", fds, 1);
	CHECK_EQ_INT(hg_init(), 0);
	if (hg_init_error()[0])
		printf("    %s\n", hg_init_error());
	CHECK(hg_wiring() != NULL);
	CHECK(strcmp(hg_module_name(), "wiretest") == 0);
	// Outputs: only the declared ones, of the declared type.
	hg_pub *p = hg_pub_open("wr.out", WORDS_TYPE, NULL);
	CHECK(p != NULL);
	CHECK(hg_pub_open("wr.undeclared", WORDS_TYPE, NULL) == NULL);
	CHECK_EQ_INT(errno, EACCES);
	CHECK(hg_pub_open("wr.out", DEPTH_TYPE, NULL) == NULL);
	CHECK_EQ_INT(errno, EPROTOTYPE);
	// Slots and rate come from the wiring, whatever the opts say.
	struct hg_pub_opts o = { .slots = 64 };
	hg_pub_close(p);
	p = hg_pub_open("wr.out", WORDS_TYPE, &o);
	CHECK(p != NULL);
	hg_sub *s = hg_sub_open("wr.out", NULL); // not a sub line...
	CHECK(s == NULL);
	CHECK_EQ_INT(errno, EACCES);
	// A line without a hash (frame): any type.
	struct hg_frame_type ft;
	hg_frame_type(16, 8, HG_GREY, &ft);
	hg_pub *pf = hg_pub_open("wr.frame", &ft.type, NULL);
	CHECK(pf != NULL);
	hg_pub_close(pf);
	// Subscriptions: the declared ones, glob matches, the declared type.
	CHECK(hg_sub_open("wr.in", WORDS_TYPE) != NULL);
	CHECK(hg_sub_open("wr.in", DEPTH_TYPE) == NULL);
	CHECK_EQ_INT(errno, EPROTOTYPE);
	CHECK(hg_sub_open("wr.any", DEPTH_TYPE) != NULL); // no hash on the line: any type
	CHECK(hg_sub_open("wr.glob.x", NULL) != NULL);
	CHECK(hg_sub_open("wr.glob.*", NULL) != NULL);
	CHECK(hg_sub_open("wr.other.*", NULL) == NULL);
	CHECK_EQ_INT(errno, EACCES);
	CHECK(hg_sub_open("wr.nope", NULL) == NULL);
	CHECK_EQ_INT(errno, EACCES);
	// Mirrors: a mirror line, with its rate and staleness.
	static const unsigned char boot[16] = { 7 };
	hg_pub *m = hg_pub_open_mirror("wr.mirror.cam", WORDS_TYPE, boot, 1, NULL);
	CHECK(m != NULL);
	CHECK(hg_pub_open_mirror("wr.elsewhere", WORDS_TYPE, boot, 1, NULL) == NULL);
	CHECK_EQ_INT(errno, EACCES);
	hg_sub *ms = hg_sub_open("wr.glob.*", NULL);
	(void)ms;
	hg_sub *rd = hg_sub_open("wr.in", NULL);
	(void)rd;
	unsigned char h[4096];
	CHECK_EQ_INT(hg_stub_read_header("wr.mirror.cam", h), 0);
	uint32_t rate;
	uint64_t stale;
	memcpy(&rate, h + 60, 4);
	memcpy(&stale, h + 176, 8);
	CHECK_EQ_INT(rate, 5000);
	CHECK_EQ_INT(stale, 1000000000);
	CHECK_EQ_INT(hg_stub_read_header("wr.out", h), 0);
	memcpy(&rate, h + 60, 4);
	memcpy(&stale, h + 176, 8);
	uint32_t slots;
	memcpy(&slots, h + 24, 4);
	CHECK_EQ_INT(rate, 10000);
	CHECK_EQ_INT(stale, 300000000); // three periods of 100 ms
	CHECK_EQ_INT(slots, 4);
	CHECK(strcmp((const char *)h + 120, "wiretest") == 0);
	hg_pub_close(m);
	hg_pub_close(p);
}

static void test_bad_wiring_fails_init(void)
{
	// A fresh process would refuse to start; here only the parser can be
	// shown, since hg_init is once per process.
	struct hg_wiring x;
	char err[128];
	CHECK_EQ_INT(hg_wiring_read_file("/nonexistent/wiring", &x, err, sizeof err), -1);
	CHECK_EQ_INT(errno, ENOENT);
	int fd = hg_stub_memfd("hangar-wiring 1\nmodule m\nbogus\n");
	CHECK_EQ_INT(hg_wiring_read_fd(fd, &x, err, sizeof err), -1);
	CHECK(strstr(err, "line 3") != NULL);
	close(fd);
}

int main(void)
{
	hg_test_setup("wiring");
	RUN_TEST(test_every_line_kind);
	RUN_TEST(test_refusals);
	RUN_TEST(test_durations_and_rates);
	RUN_TEST(test_glob_match);
	RUN_TEST(test_bad_wiring_fails_init);
	RUN_TEST(test_wired_module_refuses_undeclared);
	hg_test_teardown();
	TEST_MAIN_END();
}
