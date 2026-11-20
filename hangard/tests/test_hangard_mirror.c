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

int main(void)
{
	hd_setup("mirror", NULL);
	HD_RUN(test_zero_boot_refused);
	HD_RUN(test_name_of_an_enabled_module_is_reported);
	HD_RUN(test_topic_id_collision_refused);
	hd_teardown();
	TEST_MAIN_END();
}
