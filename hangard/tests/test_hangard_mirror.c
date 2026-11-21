// SPDX-License-Identifier: GPL-3.0-or-later
// The manager's side of the bridge's mirrored topics (docs/03 "Opening
// topics", step 2; docs/08 "Mirrored topics"). The bridge here is a stand-in:
// the toy `stubborn` under a manifest with [Bridge], which sends the raw
// `open ... boot=` requests the suite wrote for it, as the bridge module's
// main process. What a mirrored open must be refused for does not depend on
// the bridge being honest.
#include "hd_test.h"

#include "depth.h"
#include "hg_call.h"

static pid_t mgr;

#define BOOT "000102030405060708090a0b0c0d0e0f"
#define ZERO "00000000000000000000000000000000"

/* Request n of the stand-in: a mirrored open of topic, type depth. */
static void open_req(int n, const char *topic, const char *boot, const char *writer)
{
	char path[1024], req[8192];
	tfmt(path, sizeof path, "%s/ctl.%d", T_root, n);
	int len = tfmt(req, sizeof req, "open %s hash=%016llx payload=%u schema=%u boot=%s first=1 writer=%s\n", topic,
		       (unsigned long long)DEPTH_TYPE.hash, DEPTH_TYPE.payload_bytes, DEPTH_TYPE.schema_bytes, boot,
		       writer);
	memcpy(req + len, DEPTH_TYPE.schema, DEPTH_TYPE.schema_bytes);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0 || write(fd, req, (size_t)len + DEPTH_TYPE.schema_bytes) < 0)
		printf("    cannot write %s\n", path);
	if (fd >= 0)
		close(fd);
}

/* The manager's reply to request n, once the stand-in has written it. */
static const char *reply_of(int n)
{
	static char rep[4][256];
	char path[1024];
	tfmt(path, sizeof path, "%s/ctl.%d.reply", T_root, n);
	for (int i = 0; i < 150 && !*read_file(path); i++)
		sleep_ms(20);
	tfmt(rep[n & 3], sizeof rep[0], "%s", read_file(path));
	return rep[n & 3];
}

/* The caller a server would take a request on this topic to come from. */
static void token_of(const char *topic, char *out)
{
	struct hg_call_request_envelope env;
	memset(&env, 0, sizeof env);
	out[0] = 0;
	hg_sub *s = hg_sub_open(topic, NULL);
	if (!s)
		return;
	unsigned char buf[64];
	struct hg_info info;
	hg_latest(s, buf, sizeof buf, &info); /* attaches */
	hg_call_caller_token(s, &env, "vehicle", out);
	hg_sub_close(s);
}

static void bridge_up(const char *conf_text)
{
	char from[1024], to[1024], conf[1024];
	const char *src = getenv("HANGAR_MSG_SRC");
	tfmt(from, sizeof from, "%s/bridge_link.msg", src && *src ? src : "msg");
	tfmt(to, sizeof to, "%s/bridge_link.msg", T_msg);
	copy_file(from, to);
	tfmt(conf, sizeof conf, "%s/etc/hangar/bridge.conf", T_root);
	write_file(conf, conf_text);
	manifest("bridge", "[Module]\nExec=%s/stubborn --polite --ctl %s/ctl\n[Bridge]\n[Restart]\nPolicy=never\n", T_toys,
		 T_root);
	mgr = manager_start();
}

static void bridge_down(void)
{
	manager_stop(mgr);
	manifest_remove("bridge");
	for (int n = 1; n < 8; n++) {
		char path[1024];
		tfmt(path, sizeof path, "%s/ctl.%d", T_root, n);
		unlink(path);
		tfmt(path, sizeof path, "%s/ctl.%d.reply", T_root, n);
		unlink(path);
	}
	rm_tree(T_topics);
	unlink(T_log);
}

/* A boot id of all zeros is refused: the file would read as a local topic,
 * and a server would take its writer for a module of this machine. */
static void test_zero_boot_refused(void)
{
	open_req(1, "pilot.req", ZERO, "safety");
	open_req(2, "pilot2.req", BOOT, "safety");
	bridge_up("[Bridge]\nPair=lab\n[Calls]\nRemote=srv.*\n");
	CHECK(strncmp(reply_of(1), "err", 3) == 0);
	CHECK(strstr(reply_of(1), "boot") != NULL);
	CHECK(strncmp(reply_of(2), "ok", 2) == 0);
	char tok[HG_CALL_CALLER] = "";
	token_of("pilot2.req", tok);
	CHECK(strcmp(tok, "topside:safety") == 0);
	unsigned char h[4096];
	CHECK(topic_header("pilot.req", h) < 0);
	bridge_down();
}

/* The .req of a module enabled on this machine that has no [Call], so no
 * .req of its own: docs/03 refuses it, the manager opens it and says so in
 * its log (README, deviations: the shipped profiles have `console` on both
 * machines). The same of a module that is not enabled here is silent. */
static void test_name_of_an_enabled_module_is_reported(void)
{
	manifest("quiet", "[Module]\nExec=%s/stubborn --polite\n", T_toys);
	open_req(1, "quiet.req", BOOT, "quiet");
	open_req(2, "far.req", BOOT, "far");
	bridge_up("[Bridge]\nPair=lab\n[Calls]\nRemote=srv.*\n");
	CHECK(strncmp(reply_of(1), "ok", 2) == 0);
	CHECK(strncmp(reply_of(2), "ok", 2) == 0);
	CHECK(file_contains(T_log, "warning: quiet.req is mirrored from the topside while a module quiet is enabled here"));
	CHECK(!file_contains(T_log, "warning: far.req"));
	bridge_down();
	manifest_remove("quiet");
}

/* A topic whose id is that of a local topic under another name is refused:
 * "costarring" and "liquid" have one FNV-1a 32. */
static void test_topic_id_collision_refused(void)
{
	CHECK(hg_topic_id("costarring") == hg_topic_id("liquid"));
	manifest("local", "[Module]\nExec=%s/stubborn --polite\n[Publish costarring]\nType=depth\n", T_toys);
	open_req(1, "liquid", BOOT, "far");
	open_req(2, "solid", BOOT, "far");
	bridge_up("[Bridge]\nPair=lab\n[Mirror liquid solid]\nDirection=down\n");
	CHECK(strncmp(reply_of(1), "err", 3) == 0);
	CHECK(strstr(reply_of(1), "topic id") != NULL);
	CHECK(strncmp(reply_of(2), "ok", 2) == 0);
	bridge_down();
	manifest_remove("local");
}

/* A mirrored topic's header names the module that writes it on the other
 * side and the pid of the bridge here. The exit of a local module that
 * happens to have the name leaves it alone; the bridge's exit flags it. */
static void test_bridge_exit_flags_its_mirrors(void)
{
	manifest("pilot", "[Module]\nExec=%s/stubborn --polite\n[Restart]\nPolicy=never\n", T_toys);
	open_req(1, "pilot.wrench", BOOT, "pilot");
	bridge_up("[Bridge]\nPair=lab\n[Mirror pilot.wrench]\nDirection=down\n");
	CHECK(strncmp(reply_of(1), "ok", 2) == 0);
	CHECK(wait_state("pilot", "ready", 3000));
	unsigned char h[4096];
	struct mstat m;
	CHECK(strncmp(ctl("stop pilot"), "ok", 2) == 0);
	CHECK(wait_state("pilot", "stopped", 3000));
	CHECK(topic_header("pilot.wrench", h) == 0);
	CHECK(module_status("bridge", &m) && m.pid > 0);
	CHECK_EQ_INT(h32(h, H_WRITER_PID), m.pid);
	CHECK(strcmp((const char *)h + H_WRITER, "pilot") == 0);
	CHECK(!(h32(h, H_FLAGS) & HG_FLAG_DIED));
	kill(m.pid, SIGKILL); /* the bridge: a running module of our manager */
	CHECK(wait_state("bridge", "stopped", 3000));
	CHECK(topic_header("pilot.wrench", h) == 0);
	CHECK(h32(h, H_FLAGS) & HG_FLAG_DIED);
	bridge_down();
	manifest_remove("pilot");
}

/* The same for a bridge that died while no manager ran: start-up step 3
 * flags what it wrote, by the pid the registry has for it. */
static void test_bridge_dead_at_startup_flags_its_mirrors(void)
{
	open_req(1, "pilot.wrench", BOOT, "pilot");
	bridge_up("[Bridge]\nPair=lab\n[Mirror pilot.wrench]\nDirection=down\n");
	CHECK(strncmp(reply_of(1), "ok", 2) == 0);
	CHECK(wait_state("bridge", "ready", 3000));
	struct mstat m;
	CHECK(module_status("bridge", &m) && m.pid > 0);
	manager_kill9(mgr);
	kill(m.pid, SIGKILL); /* alive: its manager is gone and nothing reaped it */
	for (int i = 0; i < 100 && kill(m.pid, 0) == 0; i++)
		sleep_ms(20);
	manifest_remove("bridge");
	log_clear();
	mgr = manager_start();
	CHECK(wait_file_contains(T_log, "bridge: pid", 3000));
	unsigned char h[4096];
	CHECK(topic_header("pilot.wrench", h) == 0);
	CHECK(h32(h, H_FLAGS) & HG_FLAG_DIED);
	bridge_down();
}

/* Request n: a mirrored open of topic with a payload of `bytes` and the
 * sender's slot count. */
static void open_sized(int n, const char *topic, unsigned bytes, unsigned slots)
{
	static struct hg_schema sch;
	char path[1024], text[64], req[1024];
	int tl = tfmt(text, sizeof text, "u8 data[%u]\n", bytes);
	if (hg_schema_parse(text, (size_t)tl, &sch, NULL, 0) < 0) {
		printf("    the schema of %u bytes does not parse\n", bytes);
		return;
	}
	tfmt(path, sizeof path, "%s/ctl.%d", T_root, n);
	int len = tfmt(req, sizeof req, "open %s hash=%016llx payload=%u schema=%d boot=%s first=1 writer=far slots=%u\n%s",
		       topic, (unsigned long long)sch.type.hash, sch.type.payload_bytes, tl, BOOT, slots, text);
	write_file(path, req);
	(void)len;
}

/* What a mirrored open makes the manager reserve is the sender's numbers:
 * its slot count and its type's size. The bridge bounds them (docs/08
 * "Delivery": 32 MB a ring, 256 MB together), and the manager took whatever
 * the bridge then asked for: 65536 slots of any type its mirror lines let
 * in, reserved by root in /dev/shm. Now the manager holds the request to the
 * same two bounds itself. */
static void test_ring_bounds(void)
{
	/* 65536 slots of 1000 bytes: 70 MB. The same type in 16 slots, and a
	 * ring just under the bound (4 slots of 8 MB less the slot headers), are
	 * opened. */
	open_sized(1, "far.big", 1000, 65536);
	open_sized(2, "far.small", 1000, 16);
	open_sized(3, "far.edge", (8u << 20) - 128, 4);
	open_sized(4, "far.over", (8u << 20) - 120, 4);
	bridge_up("[Bridge]\nPair=lab\n[Mirror far.*]\nDirection=down\n");
	printf("    65536 slots of 1000 bytes: %s", reply_of(1));
	CHECK(strncmp(reply_of(1), "err", 3) == 0);
	CHECK(strstr(reply_of(1), "a mirrored topic's ring is at most 32768 KB") != NULL);
	CHECK(strncmp(reply_of(2), "ok", 2) == 0);
	CHECK(strncmp(reply_of(3), "ok", 2) == 0);
	CHECK(strncmp(reply_of(4), "err", 3) == 0);
	unsigned char h[4096];
	CHECK(topic_header("far.big", h) < 0);
	CHECK(topic_header("far.over", h) < 0);
	CHECK(topic_header("far.small", h) == 0);
	bridge_down();
	/* Together: nine files of 30 MB that no module of this machine declares
	 * are in the directory (sparse: they take no memory here), 270 MB. One
	 * more topic, of any size, is not opened; a local module's still is. */
	mkdir_p(T_topics);
	for (int i = 0; i < 9; i++) {
		char path[1024];
		tfmt(path, sizeof path, "%s/far.held%d", T_topics, i);
		int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0640);
		CHECK(fd >= 0 && ftruncate(fd, 30 << 20) == 0);
		if (fd >= 0)
			close(fd);
	}
	manifest("local", "[Module]\nExec=%s/talker --topic near.depth --rate 20\n[Publish near.depth]\nType=depth\nRate=20\n",
		 T_toys);
	open_sized(1, "far.small", 1000, 16);
	bridge_up("[Bridge]\nPair=lab\n[Mirror far.*]\nDirection=down\n");
	printf("    one more beside 270 MB: %s", reply_of(1));
	CHECK(strncmp(reply_of(1), "err", 3) == 0);
	CHECK(strstr(reply_of(1), "would pass the 262144 KB they may take together") != NULL);
	CHECK(topic_header("far.small", h) < 0);
	CHECK(wait_state("local", "ready", 3000));
	CHECK(topic_header("near.depth", h) == 0);
	bridge_down();
	manifest_remove("local");
}

int main(void)
{
	hd_setup("mirror", NULL);
	HD_RUN(test_zero_boot_refused);
	HD_RUN(test_name_of_an_enabled_module_is_reported);
	HD_RUN(test_topic_id_collision_refused);
	HD_RUN(test_ring_bounds);
	HD_RUN(test_bridge_exit_flags_its_mirrors);
	HD_RUN(test_bridge_dead_at_startup_flags_its_mirrors);
	hd_teardown();
	TEST_MAIN_END();
}
