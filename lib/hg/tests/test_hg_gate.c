// SPDX-License-Identifier: LGPL-3.0-or-later
// The actuator gate (docs/07 "The actuator gate"): the three conditions and
// their reasons. One process plays the safety module, the mux and the
// actuator: a wiring with an `actuator` line gives the gate its Command
// topic and timeouts; safety.state and ctl.wrench are published here and
// read back through the gate. Absent, disarmed, stale safety state (and the
// SafetyTimeout boundary), command absent, stale, flagged as computed from
// stale inputs, open with the newest command, the safety view the gate
// judged, and the standalone open with explicit timeouts.
#include "hg_test.h"

#include "hg_gate.h"

static char wiring_path[600];

/* msg/safety_state.msg's layout, the first bytes of it. */
static const char SAFETY_SCHEMA[] =
	"u8 state\nu8 outputs_enabled\nu8 action\nu8 reserved\nu32 triggers\nu64 since_ns\n"
	"char by[48]\nchar reason[64]\n";
static const char WRENCH_SCHEMA[] = "f32 surge\nf32 sway\nf32 heave\nf32 roll\nf32 pitch\nf32 yaw\n";
static struct hg_schema safety_parsed, wrench_parsed, depth_in_parsed;

static void write_wiring(void)
{
	snprintf(wiring_path, sizeof wiring_path, "%s/wiring.txt", topic_dir);
	FILE *f = fopen(wiring_path, "w");
	if (!f) {
		perror("wiring");
		exit(2);
	}
	fprintf(f, "hangar-wiring 1\nmodule actuator\nrole vehicle\n");
	fprintf(f, "actuator command=ctl.wrench command_timeout=200ms safety_timeout=300ms\n");
	fprintf(f, "sub ctl.wrench type=wrench hash=%016llx rate=50\n", (unsigned long long)wrench_parsed.type.hash);
	fprintf(f, "sub safety.state type=safety_state hash=%016llx rate=10\n",
		(unsigned long long)safety_parsed.type.hash);
	fprintf(f, "sub nav.depth type=depth hash=%016llx rate=50\n", (unsigned long long)depth_in_parsed.type.hash);
	/* This process publishes what the gate reads. */
	fprintf(f, "pub safety.state type=safety_state hash=%016llx slots=16 rate=10 stale_after=300ms\n",
		(unsigned long long)safety_parsed.type.hash);
	fprintf(f, "pub nav.depth type=depth hash=%016llx slots=16 rate=50 stale_after=60ms\n",
		(unsigned long long)depth_in_parsed.type.hash);
	fprintf(f, "pub ctl.wrench type=wrench hash=%016llx slots=16 rate=50 stale_after=60ms sources=nav.depth\n",
		(unsigned long long)wrench_parsed.type.hash);
	fprintf(f, "pub thr.cmd type=wrench hash=%016llx slots=16 rate=50 stale_after=60ms inputs=ctl.wrench actuator\n",
		(unsigned long long)wrench_parsed.type.hash);
	fclose(f);
	setenv("HANGAR_WIRING_FILE", wiring_path, 1);
}

struct safety_msg {
	uint8_t state, outputs_enabled, action, reserved;
	uint32_t triggers;
	uint64_t since_ns;
	char by[48];
	char reason[64];
};

static hg_pub *P_safety, *P_cmd, *P_depth;

static void pub_safety(uint8_t state, uint8_t enabled)
{
	struct safety_msg s;
	memset(&s, 0, sizeof s);
	s.state = state;
	s.outputs_enabled = enabled;
	s.since_ns = hg_now_ns();
	CHECK_EQ_INT(hg_publish(P_safety, &s, sizeof s, hg_now_ns()), 0);
}

static void pub_cmd(float surge)
{
	float w[6] = { surge, 0, 0, 0, 0, 0 };
	CHECK_EQ_INT(hg_publish_with(P_cmd, w, sizeof w, hg_now_ns(), HG_NO_REFS, NULL, 0), 0);
}

static void test_open_and_absent(void)
{
	hg_gate *g = hg_gate_open();
	CHECK(g != NULL);
	if (!g) {
		printf("    hg_gate_open: %s\n", strerror(errno));
		exit(2);
	}
	CHECK_EQ_INT(hg_gate_command_timeout_ns(g), 200000000);
	CHECK_EQ_INT(hg_gate_safety_timeout_ns(g), 300000000);
	hg_sub *subs[2];
	CHECK_EQ_INT(hg_gate_subs(g, subs, 2), 2);
	CHECK(strcmp(hg_sub_name(subs[0]), "ctl.wrench") == 0);
	CHECK(strcmp(hg_sub_name(subs[1]), "safety.state") == 0);
	const char *why = NULL;
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strcmp(why, "safety state absent") == 0);
	struct hg_gate_safety s;
	hg_gate_safety(g, &s);
	CHECK(!s.present);
	/* The safety state appears, disarmed: the command is not even looked at. */
	P_safety = hg_pub_open("safety.state", &safety_parsed.type, NULL);
	CHECK(P_safety != NULL);
	pub_safety(0, 0);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strcmp(why, "disarmed") == 0);
	hg_gate_safety(g, &s);
	CHECK(s.present && s.fresh && s.state == 0 && !s.outputs_enabled && s.seq == 1);
	pub_safety(3, 0); /* arming: outputs still off */
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strcmp(why, "arming") == 0);
	pub_safety(2, 0); /* failsafe neutral */
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strcmp(why, "failsafe") == 0);
	/* Armed, but no command yet. */
	pub_safety(1, 1);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strcmp(why, "command absent") == 0);
	hg_gate_close(g);
}

static void test_open_closed_stale(void)
{
	hg_gate *g = hg_gate_open();
	CHECK(g != NULL);
	P_cmd = hg_pub_open("ctl.wrench", &wrench_parsed.type, NULL);
	P_depth = hg_pub_open("nav.depth", &depth_in_parsed.type, NULL);
	CHECK(P_cmd && P_depth);
	const char *why = NULL;
	pub_safety(1, 1);
	pub_cmd(0.25f);
	const float *w = hg_gate_check(g, &why);
	CHECK(w != NULL);
	CHECK(w && w[0] == 0.25f);
	CHECK(why && strcmp(why, "open") == 0);
	CHECK_EQ_INT(hg_gate_command_info(g)->seq, 1);
	CHECK_EQ_INT(hg_gate_command_bytes(g), 24);
	/* A newer command replaces it; nothing newer keeps it. */
	pub_cmd(-0.5f);
	w = hg_gate_check(g, &why);
	CHECK(w && w[0] == -0.5f);
	CHECK_EQ_INT(hg_gate_command_info(g)->seq, 2);
	w = hg_gate_check(g, &why);
	CHECK(w && w[0] == -0.5f && hg_gate_command_info(g)->seq == 2);
	/* The command ages past CommandTimeout while the safety state stays fresh. */
	sleep_ms(120);
	pub_safety(1, 1);
	sleep_ms(120);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strncmp(why, "command stale ", 14) == 0);
	if (why)
		printf("    %s\n", why);
	/* A fresh command, but the safety state is now older than SafetyTimeout
	 * (published 120 ms ago; 200 ms more passes 300). */
	sleep_ms(200);
	pub_cmd(0.1f);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strncmp(why, "safety state stale ", 19) == 0);
	if (why)
		printf("    %s\n", why);
	struct hg_gate_safety s;
	hg_gate_safety(g, &s);
	CHECK(s.present && !s.fresh && s.age_ns > 300000000ull);
	/* Both fresh again: open. The boundary: a safety state 250 ms old is
	 * within 300 ms, one 350 ms old is not. */
	pub_safety(1, 1);
	pub_cmd(0.2f);
	CHECK(hg_gate_check(g, &why) != NULL);
	sleep_ms(250);
	pub_cmd(0.3f);
	w = hg_gate_check(g, &why);
	CHECK(w != NULL && w[0] == 0.3f);
	sleep_ms(100);
	pub_cmd(0.4f);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strncmp(why, "safety state stale ", 19) == 0);
	/* A disarm closes the gate at once, whatever the command. */
	pub_safety(0, 0);
	pub_cmd(0.5f);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strcmp(why, "disarmed") == 0);
	hg_gate_close(g);
}

static void test_flagged_command(void)
{
	hg_gate *g = hg_gate_open();
	CHECK(g != NULL);
	const char *why = NULL;
	pub_safety(1, 1);
	/* The mux names nav.depth, which is fresh: the command is clean. The
	 * reference resolves to this process's subscription of nav.depth (the
	 * mux's view of its source), so a subscription must be open. */
	hg_sub *depth_sub = hg_sub_open("nav.depth", NULL);
	CHECK(depth_sub != NULL);
	struct depth d = { 3.0f, 20.0f, 1300.0f };
	CHECK_EQ_INT(hg_publish(P_depth, &d, sizeof d, hg_now_ns()), 0);
	struct hg_input ref = { .topic_id = hg_topic_id("nav.depth"), .seq = hg_pub_next_seq(P_depth) - 1 };
	float w[6] = { 0.6f, 0, 0, 0, 0, 0 };
	CHECK_EQ_INT(hg_publish_with(P_cmd, w, sizeof w, hg_now_ns(), 0, &ref, 1), 0);
	const float *c = hg_gate_check(g, &why);
	CHECK(c != NULL && c[0] == 0.6f);
	/* nav.depth goes stale (60 ms); a command computed from it is flagged
	 * by the library, and the gate treats it as no command. */
	sleep_ms(80);
	pub_safety(1, 1);
	CHECK_EQ_INT(hg_publish_with(P_cmd, w, sizeof w, hg_now_ns(), 0, &ref, 1), 0);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strncmp(why, "command computed from stale inputs", 34) == 0);
	if (why)
		printf("    %s\n", why);
	/* HG_STALE set by the publisher itself (a source past the 4 references). */
	CHECK_EQ_INT(hg_publish_with(P_cmd, w, sizeof w, hg_now_ns(), HG_STALE | HG_NO_REFS, NULL, 0), 0);
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strcmp(why, "command computed from stale inputs") == 0);
	/* A command that names nothing (neutral from an open gate) is clean. */
	pub_cmd(0);
	CHECK(hg_gate_check(g, &why) != NULL);
	hg_sub_close(depth_sub);
	hg_gate_close(g);
}

static void test_closed_writer(void)
{
	hg_gate *g = hg_gate_open();
	CHECK(g != NULL);
	const char *why = NULL;
	pub_safety(1, 1);
	pub_cmd(0.7f);
	CHECK(hg_gate_check(g, &why) != NULL);
	/* The mux closes its topic (it exited): its last command is no command. */
	hg_pub_close(P_cmd);
	P_cmd = NULL;
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strncmp(why, "command closed", 14) == 0);
	/* The safety module closes: closed before stale. */
	hg_pub_close(P_safety);
	P_safety = NULL;
	CHECK(hg_gate_check(g, &why) == NULL);
	CHECK(why && strncmp(why, "safety state closed", 19) == 0);
	hg_gate_close(g);
}

static void test_standalone_open(void)
{
	/* Without an actuator line the wired open is refused; the explicit one
	 * takes its settings from the caller. */
	unsetenv("HANGAR_WIRING_FILE");
	/* hg_init already read the wiring in this process: hg_gate_open still
	 * finds the actuator line. The explicit open overrides the timeouts. */
	hg_gate *g = hg_gate_open_with("ctl.wrench", 50000000, 0);
	CHECK(g != NULL);
	CHECK_EQ_INT(hg_gate_command_timeout_ns(g), 50000000);
	CHECK_EQ_INT(hg_gate_safety_timeout_ns(g), 300000000);
	CHECK(hg_gate_open_with(NULL, 0, 0) == NULL);
	hg_gate_close(g);
}

int main(void)
{
	hg_test_setup("gate");
	if (hg_schema_parse(SAFETY_SCHEMA, strlen(SAFETY_SCHEMA), &safety_parsed, NULL, 0) < 0 ||
	    hg_schema_parse(WRENCH_SCHEMA, strlen(WRENCH_SCHEMA), &wrench_parsed, NULL, 0) < 0 ||
	    hg_schema_parse(DEPTH_SCHEMA, strlen(DEPTH_SCHEMA), &depth_in_parsed, NULL, 0) < 0) {
		fprintf(stderr, "schemas\n");
		return 2;
	}
	safety_parsed.type.name = "safety_state";
	wrench_parsed.type.name = "wrench";
	depth_in_parsed.type.name = "depth";
	write_wiring();
	RUN_TEST(test_open_and_absent);
	RUN_TEST(test_open_closed_stale);
	RUN_TEST(test_flagged_command);
	RUN_TEST(test_closed_writer);
	RUN_TEST(test_standalone_open);
	if (P_depth)
		hg_pub_close(P_depth);
	unlink(wiring_path);
	hg_test_teardown();
	TEST_MAIN_END();
}
