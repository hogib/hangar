// SPDX-License-Identifier: GPL-3.0-or-later
// Manifests (docs/02): every section and key parsed, every parse refusal
// with its file and line, the graph checks of "How the manager uses
// manifests", the implicit topics and subscriptions, and the wiring text
// round-tripped through lib/hg's parser.
#include "hd_test.h"
#include "manifest.h"
#include "wiring.h"

static struct mf_set set;
static char err[MF_ERR];

/* Parses text as <name>.module in the test module directory. */
static int parse(const char *name, const char *text, struct mf_module *m)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.module", T_modules, name);
	write_file(path, text);
	err[0] = 0;
	return mf_parse_file(path, m, err, sizeof err);
}

/* The refusal must name the file and the line, and say why. */
static void expect_refusal(const char *name, const char *text, int line, const char *why)
{
	struct mf_module *m = calloc(1, sizeof *m);
	int rc = parse(name, text, m);
	CHECK(rc == -1);
	char expect[128];
	snprintf(expect, sizeof expect, "/%s.module:%d: ", name, line);
	if (!strstr(err, expect) || !strstr(err, why))
		printf("    expected '%s' and '%s', got: %s\n", expect, why, err);
	CHECK(strstr(err, expect) != NULL);
	CHECK(strstr(err, why) != NULL);
	free(m);
}

static const char FULL[] =
	"# every key of docs/02, once\n"
	"[Module]\n"
	"Description=Depth hold controller\n"
	"Exec=/usr/lib/hangar/depth_hold --i2c \"/dev/i2c 6\" --flag\n"
	"Environment=HG_X=1\n"
	"Environment=HG_Y=two words\n"
	"User=hg-depth\n"
	"Group=hangar\n"
	"SupplementaryGroups=i2c gpio\n"
	"Capabilities=CAP_SYS_NICE\n"
	"\n"
	"[Subscribe nav.depth]\n"
	"Type=depth\n"
	"[Subscribe nav.imu]\n"
	"[Subscribe nav.*]\n"
	"\n"
	"[Publish ctl.depth_hold]\n"
	"Type=wrench\n"
	"Rate=50\n"
	"Slots=32\n"
	"Inputs=nav.depth nav.imu\n"
	"Sources=nav.depth\n"
	"StaleAfter=100ms\n"
	"\n"
	"[Publish ctl.depth_hold.target]\n"
	"Type=depth\n"
	"Rate=0.5\n"
	"Inputs=topside:cam.pilot ctl.depth_hold\n"
	"\n"
	"[Serve reset]\n"
	"Request=empty\n"
	"Reply=empty\n"
	"Allow=topside:pilot_console admin topside:admin *\n"
	"Deadline=250ms\n"
	"\n"
	"[Call safety.disarm]\n"
	"Request=empty\n"
	"Reply=empty\n"
	"\n"
	"[Params]\n"
	"Type=depth\n"
	"Allow=topside:pilot_console admin\n"
	"\n"
	"[Restart]\n"
	"Policy=on-failure\n"
	"Backoff=0.2s..4s\n"
	"ResetAfter=1min\n"
	"Burst=3/30s\n"
	"OnGiveUp=failsafe\n"
	"StopTimeout=2.5s\n"
	"\n"
	"[Health]\n"
	"StartGrace=10s\n"
	"StaleGrace=500ms\n"
	"Watchdog=2s\n"
	"\n"
	"[Schedule]\n"
	"Policy=fifo\n"
	"Priority=60\n"
	"CPUs=1-2,3\n"
	"LockMemory=yes\n";

static void test_every_key(void)
{
	struct mf_module *m = calloc(1, sizeof *m);
	int rc = parse("depth_hold", FULL, m);
	if (rc < 0)
		printf("    %s\n", err);
	CHECK_EQ_INT(rc, 0);
	CHECK(strcmp(m->name, "depth_hold") == 0);
	CHECK(strcmp(m->description, "Depth hold controller") == 0);
	CHECK_EQ_INT(m->exec.argc, 4);
	CHECK(strcmp(m->exec.argv[0], "/usr/lib/hangar/depth_hold") == 0);
	CHECK(strcmp(m->exec.argv[2], "/dev/i2c 6") == 0); // "..." keeps the blank
	CHECK_EQ_INT(m->n_env, 2);
	CHECK(strcmp(m->env[1], "HG_Y=two words") == 0);
	CHECK(strcmp(m->user, "hg-depth") == 0 && m->user_set);
	CHECK_EQ_INT(m->n_groups, 2);
	CHECK(strcmp(m->groups[1], "gpio") == 0);
	CHECK_EQ_INT(m->n_caps, 1);
	CHECK_EQ_INT(m->n_subs, 3);
	CHECK(strcmp(m->subs[0].type, "depth") == 0);
	CHECK(m->subs[2].glob == 1);
	CHECK_EQ_INT(m->n_pubs, 2);
	CHECK_EQ_INT(m->pubs[0].rate_mhz, 50000);
	CHECK_EQ_INT(m->pubs[0].slots, 32);
	CHECK_EQ_INT(m->pubs[0].n_inputs, 2);
	CHECK_EQ_INT(m->pubs[0].n_sources, 1);
	CHECK_EQ_INT(m->pubs[0].stale_after_ns, 100000000);
	CHECK_EQ_INT(m->pubs[1].rate_mhz, 500);
	CHECK_EQ_INT(m->pubs[1].stale_after_ns, 6000000000ull); // three periods
	CHECK_EQ_INT(m->pubs[1].slots, 16);
	CHECK(strcmp(m->pubs[1].inputs[0], "topside:cam.pilot") == 0);
	CHECK_EQ_INT(m->n_serves, 1);
	CHECK_EQ_INT(m->serves[0].n_allow, 4);
	CHECK_EQ_INT(m->serves[0].deadline_ns, 250000000);
	CHECK_EQ_INT(m->n_calls, 1);
	CHECK(strcmp(m->calls[0].server, "safety") == 0 && strcmp(m->calls[0].method, "disarm") == 0);
	CHECK(m->has_params && strcmp(m->params_type, "depth") == 0 && m->n_params_allow == 2);
	CHECK_EQ_INT(m->policy, MF_RESTART_ON_FAILURE);
	CHECK_EQ_INT(m->backoff_first_ns, 200000000);
	CHECK_EQ_INT(m->backoff_max_ns, 4000000000ull);
	CHECK_EQ_INT(m->reset_after_ns, 60000000000ull);
	CHECK_EQ_INT(m->burst_count, 3);
	CHECK_EQ_INT(m->burst_period_ns, 30000000000ull);
	CHECK_EQ_INT(m->giveup, MF_GIVEUP_FAILSAFE);
	CHECK_EQ_INT(m->stop_timeout_ns, 2500000000ull);
	CHECK_EQ_INT(m->start_grace_ns, 10000000000ull);
	CHECK_EQ_INT(m->stale_grace_ns, 500000000);
	CHECK_EQ_INT(m->watchdog_ns, 2000000000ull);
	CHECK_EQ_INT(m->sched, MF_SCHED_FIFO);
	CHECK_EQ_INT(m->priority, 60);
	CHECK(m->cpus_set && m->cpu_mask[0] == 0xeu);
	CHECK_EQ_INT(m->lock_memory, 1);
	free(m);
}

static void test_defaults(void)
{
	struct mf_module *m = calloc(1, sizeof *m);
	CHECK_EQ_INT(parse("plain", "[Module]\nExec=/bin/true\n[Publish a.b]\nType=depth\nRate=10\n", m), 0);
	CHECK(strcmp(m->user, "hangar") == 0 && !m->user_set);
	CHECK_EQ_INT(m->policy, MF_RESTART_ALWAYS);
	CHECK_EQ_INT(m->backoff_first_ns, 100000000);
	CHECK_EQ_INT(m->backoff_max_ns, 10000000000ull);
	CHECK_EQ_INT(m->reset_after_ns, 30000000000ull);
	CHECK_EQ_INT(m->burst_count, 5);
	CHECK_EQ_INT(m->giveup, MF_GIVEUP_DEGRADE);
	CHECK_EQ_INT(m->stop_timeout_ns, 5000000000ull);
	CHECK_EQ_INT(m->start_grace_ns, 5000000000ull);
	CHECK_EQ_INT(m->stale_grace_ns, 1000000000ull);
	CHECK_EQ_INT(m->watchdog_ns, 0);
	CHECK_EQ_INT(m->pubs[0].slots, 16);
	CHECK_EQ_INT(m->pubs[0].stale_after_ns, 300000000); // three periods of 100 ms
	// A frame topic defaults to 4 slots.
	CHECK_EQ_INT(parse("cam", "[Module]\nExec=/bin/true\n[Publish cam.raw]\nType=frame\n", m), 0);
	CHECK_EQ_INT(m->pubs[0].slots, 4);
	// Actuator defaults: failsafe, 500 ms; Module Neutral: 500 ms.
	CHECK_EQ_INT(parse("act", "[Module]\nExec=/bin/true\nUser=hg-a\n[Subscribe ctl.x]\n[Subscribe safety.state]\n"
				"[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\n"
				"[Actuator]\nCommand=ctl.x\nNeutral=/bin/true\n", m), 0);
	CHECK_EQ_INT(m->giveup, MF_GIVEUP_FAILSAFE);
	CHECK_EQ_INT(m->stop_timeout_ns, 500000000);
	CHECK_EQ_INT(parse("leg", "[Module]\nExec=/bin/true\nLegacy=yes\nNeutral=/bin/true --neutral\n", m), 0);
	CHECK_EQ_INT(m->stop_timeout_ns, 500000000);
	free(m);
}

static void test_values(void)
{
	CHECK_EQ_INT(mf_parse_duration("100ms"), 100000000);
	CHECK_EQ_INT(mf_parse_duration("2.5s"), 2500000000ll);
	CHECK_EQ_INT(mf_parse_duration("1min"), 60000000000ll);
	CHECK_EQ_INT(mf_parse_duration("0.001ms"), 1000);
	CHECK_EQ_INT(mf_parse_duration("5"), -1);     // no unit
	// What does not fit 63 bits of nanoseconds is malformed, not wrapped.
	CHECK(mf_parse_duration("9223372036s") == 9223372036000000000ll);
	CHECK_EQ_INT(mf_parse_duration("9223372037s"), -1);
	CHECK_EQ_INT(mf_parse_duration("1125899906842624s"), -1);
	CHECK(mf_parse_duration("153722867min") == 9223372020000000000ll);
	CHECK_EQ_INT(mf_parse_duration("200000000min"), -1);
	CHECK_EQ_INT(mf_parse_duration("5us"), -1);   // not a docs/02 unit
	CHECK_EQ_INT(mf_parse_duration("ms"), -1);
	CHECK_EQ_INT(mf_parse_duration("1.2.3s"), -1);
	CHECK_EQ_INT(mf_parse_bool("yes"), 1);
	CHECK_EQ_INT(mf_parse_bool("no"), 0);
	CHECK_EQ_INT(mf_parse_bool("true"), -1);
	struct mf_exec x;
	CHECK_EQ_INT(mf_parse_exec("/a/b \"c d\" e", &x), 0);
	CHECK_EQ_INT(x.argc, 3);
	CHECK(strcmp(x.argv[1], "c d") == 0);
	CHECK_EQ_INT(mf_parse_exec("relative/path", &x), -1);
	CHECK_EQ_INT(mf_parse_exec("/a \"unterminated", &x), -1);
	CHECK(mf_valid_allow_token("*", "vehicle"));
	CHECK(mf_valid_allow_token("admin", "vehicle"));
	CHECK(mf_valid_allow_token("topside:*", "vehicle"));
	CHECK(mf_valid_allow_token("topside:admin", "vehicle"));
	CHECK(mf_valid_allow_token("topside:pilot_console", "vehicle"));
	CHECK(mf_valid_allow_token("safety", "vehicle"));
	CHECK(!mf_valid_allow_token("vehicle:safety", "vehicle")); // this machine's own role
	CHECK(!mf_valid_allow_token("Safety", "vehicle"));
	CHECK(!mf_valid_allow_token("other:x", "vehicle"));
}

static void test_parse_refusals(void)
{
	const char *ok = "[Module]\nExec=/bin/true\n";
	expect_refusal("r1", "[Module]\nExec=/bin/true\n[Bogus]\nKey=1\n", 3, "unknown section");
	expect_refusal("r2", "[Module]\nExec=/bin/true\nColor=red\n", 3, "unknown key Color");
	expect_refusal("r3", "[Module]\nExec=/bin/true\nExec=/bin/false\n", 3, "Exec given twice");
	expect_refusal("r4", "[Module]\nExec=/bin/true\nLegacy=maybe\n", 3, "not yes or no");
	expect_refusal("r5", "[Module]\nExec=/bin/true\n[Publish a.b]\nType=depth\nRate=fast\n", 5, "not a rate");
	expect_refusal("r6", "[Module]\nExec=/bin/true\n[Publish a.b]\nType=depth\nSlots=12\n", 5, "power of two");
	expect_refusal("r7", "[Module]\nExec=/bin/true\n[Publish a.b]\nType=depth\nStaleAfter=1s\n", 5,
		       "StaleAfter: only with a Rate");
	expect_refusal("r8", "[Module]\nExec=/bin/true\n[Publish a.b]\nRate=1\n", 5, "needs Type=");
	expect_refusal("r9", "[Module]\nExec=/bin/true\n[Publish a.b]\nType=depth\nInputs=a b c d e\n", 5,
		       "more than 4");
	expect_refusal("r10", "[Module]\nExec=/bin/true\n[Subscribe nav.*]\nType=depth\n", 4,
		       "not allowed with a glob");
	expect_refusal("r11", "[Module]\nExec=/bin/true\n[Restart]\nBackoff=10s..1s\n", 4, "not a duration range");
	expect_refusal("r12", "[Module]\nExec=/bin/true\n[Restart]\nBurst=5\n", 4, "<count>/<duration>");
	expect_refusal("r13", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=sometimes\n", 4, "always, on-failure or never");
	expect_refusal("r14", "[Module]\nExec=/bin/true\n[Restart]\nOnGiveUp=panic\n", 4, "degrade or failsafe");
	expect_refusal("r15", "[Module]\nExec=/bin/true\n[Schedule]\nPolicy=fifo\n", 5, "Priority is required");
	expect_refusal("r16", "[Module]\nExec=/bin/true\n[Schedule]\nPriority=100\n", 4, "1 to 99");
	expect_refusal("r17", "[Module]\nExec=/bin/true\n[Schedule]\nCPUs=a\n", 4, "list or ranges");
	expect_refusal("r18", "[Module]\nExec=/bin/true\n[Health]\nWatchdog=0.5\n", 4, "not a duration");
	expect_refusal("r19", "Exec=/bin/true\n", 1, "before any section");
	expect_refusal("r20", "[Module]\nExec=/bin/true\nthis is not a key\n", 3, "not a Key=Value");
	expect_refusal("r21", "[Publish a.b]\nType=depth\n", 3, "no [Module] section");
	expect_refusal("r22", "[Module]\nDescription=x\n", 1, "needs Exec=");
	expect_refusal("r23", "[Module]\nExec=/bin/true\n[Module]\n", 3, "given twice");
	expect_refusal("r24", "[Module]\nExec=/bin/true\n[Publish a.b]\nType=depth\n[Publish a.b]\nType=depth\n", 5,
		       "given twice");
	expect_refusal("r25", "[Module]\nExec=/bin/true\n[Serve m]\nRequest=empty\n", 5, "needs Request= and Reply=");
	expect_refusal("r26", "[Module]\nExec=/bin/true\n[Call x]\n", 3, "want <module>.<method>");
	expect_refusal("r27", "[Module]\nExec=/bin/true\n[Call a.b]\nRequest=empty\n", 5, "go together");
	expect_refusal("r28", "[Module]\nExec=/bin/true\n[Actuator]\nCommand=a.b\n", 5, "needs Command= and Neutral=");
	expect_refusal("r29", "[Module]\nExec=/bin/true\n[Params]\nAllow=admin\n", 5, "needs Type=");
	expect_refusal("r30", "[Module]\nExec=/bin/true\n[Bridge]\nKey=1\n", 4, "has no keys");
	expect_refusal("r31", "[Module]\nExec=/bin/true\nCapabilities=CAP_SYS_ADMIN\n", 3, "not in the manager's set");
	expect_refusal("r32", "[Module]\nExec=/bin/true\n[Serve m]\nRequest=empty\nReply=empty\nAllow=Bad-Token\n", 6,
		       "not a caller token");
	expect_refusal("r33", "[Module]\nExec=/bin/true\n[Publish Bad.Name]\n", 3, "not a topic name");
	expect_refusal("r34", "[Module]\nExec=/bin/true\nEnvironment=novalue\n", 3, "want NAME=value");
	expect_refusal("r35", "[Module]\nExec=/bin/true\n[Module x]\n", 3, "takes no argument");
	expect_refusal("r36", "[Module]\nExec=/bin/true\n[Publish]\n", 3, "needs an argument");
	expect_refusal("r37", "[Module]\nExec=relative\n", 2, "absolute path");
	// A '#' inside a value is part of the value; blank space around '=' is ignored.
	struct mf_module *m = calloc(1, sizeof *m);
	CHECK_EQ_INT(parse("hash", "[Module]\n  Exec =  /bin/true --tag #1  \n; a comment\n  # another\n", m), 0);
	CHECK_EQ_INT(m->exec.argc, 3);
	CHECK(strcmp(m->exec.argv[2], "#1") == 0);
	CHECK_EQ_INT(parse("ok", ok, m), 0);
	// Not a module name: the file name.
	char path[1024];
	snprintf(path, sizeof path, "%s/Bad-Name.module", T_modules);
	write_file(path, ok);
	CHECK_EQ_INT(mf_parse_file(path, m, err, sizeof err), -1);
	CHECK(strstr(err, "not a module name") != NULL);
	free(m);
}

/* ---- Graph checks ------------------------------------------------------ */

static struct mf_module *add(const char *name, const char *text)
{
	struct mf_module *m = calloc(1, sizeof *m);
	if (parse(name, text, m) < 0) {
		printf("    %s\n", err);
		m->refused = 1;
		snprintf(m->reason, sizeof m->reason, "%s", err);
	}
	mf_set_add(&set, m);
	return m;
}

static void fresh_set(void)
{
	mf_set_free(&set);
	mf_set_init(&set, "vehicle", "installer", T_msg);
}

/* Checks the set and expects name refused with a reason containing why at
 * the given line. */
static void expect_graph(const char *name, int line, const char *why)
{
	mf_set_check(&set);
	struct mf_module *m = mf_find(&set, name);
	CHECK(m != NULL);
	if (!m)
		return;
	char at[64];
	snprintf(at, sizeof at, "/%s.module:%d: ", name, line);
	if (!m->refused || !strstr(m->reason, why) || !strstr(m->reason, at))
		printf("    %s: refused=%d reason='%s' (want line %d, '%s')\n", name, m->refused, m->reason, line, why);
	CHECK(m->refused);
	CHECK(strstr(m->reason, why) != NULL);
	CHECK(strstr(m->reason, at) != NULL);
}

static void expect_accepted(const char *name)
{
	struct mf_module *m = mf_find(&set, name);
	CHECK(m != NULL);
	if (m && m->refused)
		printf("    %s unexpectedly refused: %s\n", name, m->reason);
	CHECK(m && !m->refused);
}

#define MOD "[Module]\nExec=/bin/true\n"

static void test_graph_refusals(void)
{
	fresh_set();
	add("a", MOD "[Publish x.y]\nType=depth\n");
	add("b", MOD "[Publish x.y]\nType=depth\n");
	mf_set_check(&set);
	// Two publishers of one topic: neither is started.
	CHECK(mf_find(&set, "a")->refused && mf_find(&set, "b")->refused);
	CHECK(strstr(mf_find(&set, "a")->reason, "also published by b") != NULL);
	CHECK(strstr(mf_find(&set, "b")->reason, "neither is started") != NULL);

	fresh_set();
	add("c", MOD "[Publish c.out]\nType=depth\nInputs=nav.depth\n");
	expect_graph("c", 3, "neither a subscription nor an output");
	fresh_set();
	add("c", MOD "[Subscribe nav.*]\n[Publish c.out]\nType=depth\nInputs=nav.depth\n");
	expect_graph("c", 4, "only by glob");
	fresh_set();
	add("c", MOD "[Publish c.out]\nType=depth\nSources=nav.depth\n");
	expect_graph("c", 3, "Sources: nav.depth is not a subscription");
	fresh_set();
	add("c", MOD "[Publish c.out]\nType=depth\nInputs=vehicle:cam.pilot\n");
	expect_graph("c", 3, "this machine's own role");
	fresh_set();
	add("c", MOD "[Publish c.out]\nType=depth\nInputs=topside:cam.pilot\n");
	mf_set_check(&set);
	expect_accepted("c"); // the other side's topic, without a subscription
	fresh_set();
	add("p", MOD "[Publish x.y]\nType=depth\n");
	add("s", MOD "[Subscribe x.y]\nType=wrench\n");
	expect_graph("s", 3, "but p publishes it as depth");
	fresh_set();
	/* dataset:<name>: the writer's row type, like frame (policy_check.c);
	 * never a call's request or reply. */
	add("d", MOD "[Publish x.y]\nType=dataset:rows\n");
	mf_set_check(&set);
	expect_accepted("d");
	fresh_set();
	add("dc", MOD "[Serve m]\nRequest=dataset:rows\nReply=empty\n");
	expect_graph("dc", 3, "a request or reply cannot be a dataset row");
	fresh_set();
	add("t", MOD "[Publish x.y]\nType=nosuchtype\n");
	expect_graph("t", 3, "cannot read");
	// Actuator rules.
	fresh_set();
	add("act", MOD "[Subscribe ctl.x]\n[Subscribe safety.state]\n[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\n"
		       "[Actuator]\nCommand=ctl.x\nNeutral=/bin/true\n");
	expect_graph("act", 5, "dedicated User");
	fresh_set();
	add("act", MOD "User=hg-a\n[Subscribe ctl.x]\n[Subscribe safety.state]\n[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\n");
	expect_graph("act", 6, "[Actuator] section is required");
	fresh_set();
	add("act", MOD "User=hg-a\n[Subscribe safety.state]\n[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\n"
		       "[Actuator]\nCommand=ctl.x\nNeutral=/bin/true\n");
	expect_graph("act", 9, "Command=ctl.x is not one of the module's subscriptions");
	fresh_set();
	add("act", MOD "User=hg-a\n[Subscribe ctl.x]\n[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\n"
		       "[Actuator]\nCommand=ctl.x\nNeutral=/bin/true\n");
	expect_graph("act", 9, "must subscribe to safety.state");
	fresh_set();
	add("act", MOD "User=hg-a\n[Subscribe ctl.x]\n[Subscribe safety.state]\n[Publish thr.cmd]\nType=depth\nRate=50\nActuator=yes\n"
		       "[Actuator]\nCommand=ctl.x\nNeutral=/bin/true\nGroups=pwm\n");
	add("other", MOD "User=hg-a\nSupplementaryGroups=pwm\n");
	mf_set_check(&set);
	CHECK(mf_find(&set, "act")->refused && strstr(mf_find(&set, "act")->reason, "user hg-a is shared with other"));
	CHECK(mf_find(&set, "other")->refused && strstr(mf_find(&set, "other")->reason, "group pwm, which [Actuator] Groups of act"));
	fresh_set();
	add("noact", MOD "[Actuator]\nCommand=a.b\nNeutral=/bin/true\n");
	expect_graph("noact", 3, "only for a module with an actuator output");
	// Legacy rules.
	fresh_set();
	add("leg", MOD "Legacy=yes\n[Publish a.b]\nType=depth\n");
	expect_graph("leg", 1, "declares no topics");
	fresh_set();
	add("leg", MOD "Legacy=yes\nExclusiveGroups=spi\nSupplementaryGroups=spi\n");
	expect_graph("leg", 1, "ExclusiveGroups needs a [Module] Neutral");
	fresh_set();
	add("leg", MOD "Legacy=yes\nNeutral=/bin/true\nExclusiveGroups=spi\n");
	expect_graph("leg", 1, "not one of the module's SupplementaryGroups");
	fresh_set();
	add("notleg", MOD "Neutral=/bin/true\n");
	expect_graph("notleg", 1, "only for Legacy=yes");
	fresh_set();
	add("ardusub", MOD "Legacy=yes\nNeutral=/bin/true\nExclusiveGroups=spi gpio\nSupplementaryGroups=spi gpio\n");
	add("intruder", MOD "SupplementaryGroups=gpio\n");
	mf_set_check(&set);
	expect_accepted("ardusub");
	CHECK(mf_find(&set, "intruder")->refused &&
	      strstr(mf_find(&set, "intruder")->reason, "intruder is in group gpio, which ExclusiveGroups of ardusub"));
	// Reserved names.
	fresh_set();
	add("hangar", MOD);
	expect_graph("hangar", 1, "may not be named hangar");
	fresh_set();
	add("x", MOD "[Publish hangar.x]\nType=depth\n");
	expect_graph("x", 3, "reserved");
	fresh_set();
	add("x", MOD "[Publish a.req]\nType=depth\n");
	expect_graph("x", 3, "reserved");
	fresh_set();
	add("x", MOD "[Publish a.rep]\nType=depth\n");
	expect_graph("x", 3, "reserved");
	fresh_set();
	add("x", MOD "[Publish a.params]\nType=depth\n");
	expect_graph("x", 3, "reserved");
	fresh_set();
	add("x", MOD "[Serve set_params]\nRequest=empty\nReply=empty\n");
	expect_graph("x", 3, "reserved; declare [Params]");
	fresh_set();
	add("x", MOD "[Publish hangar.vehicle.system]\nType=depth\n");
	expect_graph("x", 3, "reserved");
	// Privileged.
	fresh_set();
	add("notinstaller", MOD "Privileged=yes\n");
	expect_graph("notinstaller", 1, "only the module PrivilegedModule= names");
	fresh_set();
	add("installer", MOD "Privileged=yes\nUser=root\n");
	expect_graph("installer", 1, "no User, Group");
	// Root is the Privileged module's alone: not by User=, Group= or a
	// supplementary group of any other.
	fresh_set();
	add("sly", MOD "User=root\n[Publish x.y]\nType=depth\nRate=10\n");
	expect_graph("sly", 1, "only the Privileged module runs as root");
	fresh_set();
	add("sly", MOD "Group=root\n");
	expect_graph("sly", 1, "only the Privileged module runs as root");
	fresh_set();
	add("sly", MOD "SupplementaryGroups=video root\n");
	expect_graph("sly", 1, "only the Privileged module runs as root");
	fresh_set();
	add("installer", MOD "Privileged=yes\n[Publish a.b]\nType=depth\n");
	expect_graph("installer", 4, "no [Publish]");
	fresh_set();
	add("installer", MOD "Privileged=yes\n[Subscribe a.b]\n");
	expect_graph("installer", 4, "no [Subscribe]");
	fresh_set();
	add("installer", MOD "Privileged=yes\n[Call a.b]\nRequest=empty\nReply=empty\n");
	expect_graph("installer", 4, "no [Call]");
	fresh_set();
	add("installer", MOD "Privileged=yes\n[Params]\nType=depth\n");
	expect_graph("installer", 4, "no [Params]");
	fresh_set();
	add("installer", MOD "Privileged=yes\n[Serve format]\nRequest=empty\nReply=empty\n");
	expect_graph("installer", 4, "serves only apply and rollback");
	// Allow and calls.
	fresh_set();
	add("server", MOD "[Serve arm]\nRequest=empty\nReply=empty\nAllow=safety\n");
	add("caller", MOD "[Call server.arm]\n");
	expect_graph("caller", 3, "not in the method's Allow list");
	fresh_set();
	add("caller", MOD "[Call nobody.arm]\n");
	expect_graph("caller", 3, "needs Request= and Reply=");
	fresh_set();
	add("server", MOD "[Serve arm]\nRequest=empty\nReply=empty\n");
	add("caller", MOD "[Call server.arm]\nRequest=depth\nReply=empty\n");
	expect_graph("caller", 3, "differ from the server's");
	// The 127 limit, implicit subscriptions counted.
	fresh_set();
	static char big[8192];
	int n = snprintf(big, sizeof big, MOD);
	for (int i = 0; i < 126; i++)
		n += snprintf(big + n, sizeof big - (size_t)n, "[Subscribe t.s%d]\n", i);
	add("server", MOD "[Serve ping]\nRequest=empty\nReply=empty\n");
	add("wide", big);
	mf_set_check(&set);
	expect_accepted("wide");
	fresh_set();
	n += snprintf(big + n, sizeof big - (size_t)n, "[Call server.ping]\n"); // + server.rep: 127 + ...
	add("server", MOD "[Serve ping]\nRequest=empty\nReply=empty\n");
	add("wide", big);
	mf_set_check(&set);
	expect_accepted("wide"); // 126 + server.rep = 127, the limit
	fresh_set();
	n += snprintf(big + n, sizeof big - (size_t)n, "[Subscribe t.one_more]\n");
	add("server", MOD "[Serve ping]\nRequest=empty\nReply=empty\n");
	add("wide", big);
	expect_graph("wide", 1, "128 subscriptions by name");
	// Topic ids: a collision is refused (two names with one FNV-1a 32 hash).
	fresh_set();
	add("i1", MOD "[Publish costarring]\nType=depth\n");
	add("i2", MOD "[Publish liquid]\nType=depth\n");
	mf_set_check(&set);
	if (hg_topic_id("costarring") == hg_topic_id("liquid")) {
		CHECK(mf_find(&set, "i2")->refused);
		CHECK(strstr(mf_find(&set, "i2")->reason, "same topic id") != NULL);
	} else {
		printf("    (no known FNV-1a 32 collision among valid names: id check not exercised)\n");
	}
	// The bridge without a BridgeConf, with a bad one, and twice.
	fresh_set();
	add("bridge", MOD "[Bridge]\n");
	expect_graph("bridge", 3, "no BridgeConf");
	char conf[1024];
	snprintf(conf, sizeof conf, "%s/bridge.conf", T_root);
	write_file(conf, "[Bridge]\nPair=lab\n[Mirror nav.depth]\nDelivery=latest\n");
	fresh_set();
	snprintf(set.bridge_conf_path, sizeof set.bridge_conf_path, "%s", conf);
	add("bridge", MOD "[Bridge]\n");
	expect_graph("bridge", 3, "Direction");
	write_file(conf, "[Bridge]\nPair=lab\n");
	fresh_set();
	snprintf(set.bridge_conf_path, sizeof set.bridge_conf_path, "%s", conf);
	add("bridge", MOD "[Bridge]\n");
	add("bridge2", MOD "[Bridge]\n");
	expect_graph("bridge2", 3, "one module per machine");
}

/* The bridge's wiring from bridge.conf (docs/08): a sub for each topic it
 * sends, a mirror line for each it receives, and the calls [Calls] Remote=
 * names as request and reply topics, on the vehicle. */
static void test_bridge(void)
{
	char conf[1024], from[1024], to[1024];
	const char *src = getenv("HANGAR_MSG_SRC");
	snprintf(from, sizeof from, "%s/bridge_link.msg", src && *src ? src : "msg");
	snprintf(to, sizeof to, "%s/bridge_link.msg", T_msg);
	copy_file(from, to);
	snprintf(conf, sizeof conf, "%s/bridge.conf", T_root);
	write_file(conf, "[Bridge]\nPair=lab\n[Calls]\nRemote=safety.*\n"
			 "[Mirror nav.*]\nDirection=up\n"
			 "[Mirror pilot.wrench]\nDirection=down\nPriority=command\nExpectRate=50\nStaleAfter=100ms\n"
			 "MaxAge=200ms\n");
	fresh_set();
	snprintf(set.bridge_conf_path, sizeof set.bridge_conf_path, "%s", conf);
	add("bridge", MOD "[Bridge]\n");
	add("safety", MOD "[Serve arm]\nRequest=empty\nReply=empty\nAllow=topside:joystick\n");
	add("nav", MOD "[Publish nav.depth]\nType=depth\nRate=10\n");
	add("mux", MOD "[Subscribe pilot.wrench]\n");
	int ok = mf_set_check(&set);
	for (int i = 0; i < set.n_modules; i++)
		if (set.modules[i]->refused)
			printf("    %s\n", set.modules[i]->reason);
	CHECK_EQ_INT(ok, 4);
	// No warning for pilot.wrench: the bridge brings it.
	for (int i = 0; i < set.n_warnings; i++) {
		printf("    warning: %s\n", set.warnings[i]);
		CHECK(strstr(set.warnings[i], "pilot.wrench") == NULL);
	}
	static char text[16384];
	int len = wiring_text(&set, mf_find(&set, "bridge"), text, sizeof text);
	CHECK(len > 0);
	const char *want[] = { "pub bridge.vehicle.link type=bridge_link", "subglob nav.*", "sub safety.rep",
			       "sub hangar.vehicle.admin.req", "mirror pilot.wrench type=any rate=50 stale_after=100ms",
			       "mirror *.req type=any", "mirror *.rep type=any", NULL };
	for (int i = 0; want[i]; i++)
		if (!strstr(text, want[i])) {
			printf("    the bridge's wiring lacks '%s':\n%s", want[i], text);
			CHECK(0);
		}
	// The server of a remote method finds its callers' mirrored request topics.
	len = wiring_text(&set, mf_find(&set, "safety"), text, sizeof text);
	if (!strstr(text, "subglob *.req")) {
		printf("    safety's wiring lacks 'subglob *.req':\n%s", text);
		CHECK(0);
	}
	// The shipped example (modules/bridge/bridge.conf, the docs' own), on
	// both roles: the vehicle sends its own bridge.vehicle.link up.
	char example[1024];
	snprintf(example, sizeof example, "%s/../modules/bridge/bridge.conf", src && *src ? src : "msg");
	const char *roles[] = { "vehicle", "topside" };
	for (int r = 0; r < 2; r++) {
		mf_set_free(&set);
		mf_set_init(&set, roles[r], "", T_msg);
		snprintf(set.bridge_conf_path, sizeof set.bridge_conf_path, "%s", example);
		add("bridge", MOD "[Bridge]\n");
		add("safety", MOD "[Serve arm]\nRequest=empty\nReply=empty\n");
		CHECK_EQ_INT(mf_set_check(&set), 2);
		if (mf_find(&set, "bridge")->refused)
			printf("    %s: %s\n", roles[r], mf_find(&set, "bridge")->reason);
		len = wiring_text(&set, mf_find(&set, "bridge"), text, sizeof text);
		CHECK(len > 0);
		CHECK(strstr(text, r == 0 ? "sub bridge.vehicle.link" : "mirror bridge.vehicle.link") != NULL);
		CHECK(strstr(text, r == 0 ? "mirror pilot.wrench" : "sub pilot.wrench") != NULL);
	}
	mf_set_free(&set);
}

static void test_implicit(void)
{
	fresh_set();
	add("safety", MOD "[Call mavlink.arm]\n[Publish safety.state]\nType=safety_state\nRate=10\n");
	add("mavlink", MOD "[Serve arm]\nRequest=empty\nReply=empty\nAllow=safety\n[Params]\nType=depth\n"
			   "[Publish ardusub.state]\nType=depth\nRate=1\n");
	add("installer", MOD "Privileged=yes\n[Serve apply]\nRequest=empty\nReply=empty\n");
	int ok = mf_set_check(&set);
	for (int i = 0; i < set.n_modules; i++)
		if (set.modules[i]->refused)
			printf("    %s\n", set.modules[i]->reason);
	CHECK_EQ_INT(ok, 3);
	struct mf_module *s = mf_find(&set, "safety"), *m = mf_find(&set, "mavlink"), *in = mf_find(&set, "installer");
	// The caller: safety.req (type req:safety, 16 slots, implicit) and sub mavlink.rep.
	const struct mf_pub *req = NULL;
	for (int i = 0; i < s->n_pubs; i++)
		if (strcmp(s->pubs[i].topic, "safety.req") == 0)
			req = &s->pubs[i];
	CHECK(req && req->implicit && req->slots == 16 && req->rate_mhz == 0);
	CHECK(req && strcmp(mf_type_at(&set, req->type_idx)->name, "req:safety") == 0);
	CHECK(req && mf_type_at(&set, req->type_idx)->payload_bytes == 112 + 8);
	int have = 0;
	for (int i = 0; i < s->n_subs; i++)
		if (strcmp(s->subs[i].topic, "mavlink.rep") == 0 && s->subs[i].implicit)
			have = 1;
	CHECK(have);
	// The server: mavlink.rep (rep:mavlink), mavlink.params (params:mavlink),
	// sub safety.req and sub hangar.vehicle.admin.req; the request topics are
	// Sources of every output, the .rep among them.
	const struct mf_pub *rep = NULL, *par = NULL, *state = NULL;
	for (int i = 0; i < m->n_pubs; i++) {
		if (strcmp(m->pubs[i].topic, "mavlink.rep") == 0)
			rep = &m->pubs[i];
		if (strcmp(m->pubs[i].topic, "mavlink.params") == 0)
			par = &m->pubs[i];
		if (strcmp(m->pubs[i].topic, "ardusub.state") == 0)
			state = &m->pubs[i];
	}
	CHECK(rep && rep->implicit && rep->slots == 16);
	CHECK(rep && strcmp(mf_type_at(&set, rep->type_idx)->name, "rep:mavlink") == 0);
	CHECK(rep && mf_type_at(&set, rep->type_idx)->payload_bytes == 96 + 16); // set_params_rep: 16 bytes
	// [Params] serves set_params: the implicit serve line with the generated
	// types and the default Allow list, "* admin".
	const struct mf_serve *sp = NULL;
	for (int i = 0; i < m->n_serves; i++)
		if (strcmp(m->serves[i].method, "set_params") == 0)
			sp = &m->serves[i];
	CHECK(sp && sp->implicit && strcmp(sp->req, "set_params:depth") == 0 && strcmp(sp->rep, "set_params_rep") == 0);
	CHECK(sp && sp->allow_set && sp->n_allow == 2 && strcmp(sp->allow[0], "*") == 0 && strcmp(sp->allow[1], "admin") == 0);
	CHECK(sp && mf_type_at(&set, sp->req_idx)->payload_bytes == 8 + 16);
	CHECK(sp && mf_type_at(&set, sp->rep_idx)->payload_bytes == 16);
	CHECK(par && par->implicit && strcmp(mf_type_at(&set, par->type_idx)->name, "params:mavlink") == 0);
	CHECK(par && mf_type_at(&set, par->type_idx)->payload_bytes == 64 + 16); // 8+48+1+7, then depth's 12 -> 16
	int have_req = 0, have_admin = 0;
	for (int i = 0; i < m->n_subs; i++) {
		if (strcmp(m->subs[i].topic, "safety.req") == 0 && m->subs[i].implicit)
			have_req = 1;
		if (strcmp(m->subs[i].topic, "hangar.vehicle.admin.req") == 0 && m->subs[i].implicit)
			have_admin = 1;
	}
	CHECK(have_req && have_admin);
	CHECK(rep && rep->n_sources == 2);
	CHECK(state && state->n_sources == 2);
	// The privileged module: sub safety.state, its callers' request topics (none), pub installer.rep, nothing else.
	CHECK_EQ_INT(in->n_pubs, 1);
	CHECK(strcmp(in->pubs[0].topic, "installer.rep") == 0);
	CHECK_EQ_INT(in->n_subs, 2); // safety.state and hangar.vehicle.admin.req
	CHECK(strcmp(in->subs[0].topic, "hangar.vehicle.admin.req") == 0 || strcmp(in->subs[0].topic, "safety.state") == 0);
	// The implicit subscription to safety.state carries the publisher's type.
	for (int i = 0; i < in->n_subs; i++)
		if (strcmp(in->subs[i].topic, "safety.state") == 0) {
			CHECK(strcmp(in->subs[i].pub_type, "safety_state") == 0);
			CHECK_EQ_INT(in->subs[i].rate_mhz, 10000);
		}
	// A warning, not a refusal: a subscription nobody publishes.
	fresh_set();
	add("lonely", MOD "[Subscribe nobody.home]\n");
	mf_set_check(&set);
	expect_accepted("lonely");
	CHECK(set.n_warnings == 1 && strstr(set.warnings[0], "no enabled module publishes") != NULL);
	// Legacy with [Module] Neutral, accepted; a parse failure in the
	// directory is listed refused with its reason and the others load.
	fresh_set();
	add("ardusub", MOD "Legacy=yes\nNeutral=/bin/true --neutral\n");
	char bad[1024];
	snprintf(bad, sizeof bad, "%s/broken.module", T_modules);
	write_file(bad, "[Module]\nExec=/bin/true\nUnknown=1\n");
	mf_set_free(&set);
	mf_set_init(&set, "vehicle", "", T_msg);
	int read_ = mf_set_load_dir(&set, T_modules);
	CHECK(read_ > 2);
	mf_set_check(&set);
	struct mf_module *broken = mf_find(&set, "broken");
	CHECK(broken && broken->refused && strstr(broken->reason, "broken.module:3: unknown key Unknown"));
	expect_accepted("ardusub");
	unlink(bad);
}

static void test_wiring_roundtrip(void)
{
	fresh_set();
	add("depth_hold", FULL);
	add("safety", MOD "[Serve disarm]\nRequest=empty\nReply=empty\nAllow=depth_hold\n");
	add("nav", MOD "[Publish nav.depth]\nType=depth\nRate=50\n[Publish nav.imu]\nType=imu\nRate=100\n");
	int ok = mf_set_check(&set);
	for (int i = 0; i < set.n_modules; i++)
		if (set.modules[i]->refused)
			printf("    %s\n", set.modules[i]->reason);
	CHECK_EQ_INT(ok, 3);
	struct mf_module *m = mf_find(&set, "depth_hold");
	static char text[65536];
	int len = wiring_text(&set, m, text, sizeof text);
	CHECK(len > 0);
	printf("%s", text);
	struct hg_wiring *w = calloc(1, sizeof *w);
	char werr[160] = "";
	int rc = hg_wiring_parse(text, (size_t)len, w, werr, sizeof werr);
	if (rc < 0)
		printf("    %s\n", werr);
	CHECK_EQ_INT(rc, 0);
	CHECK(strcmp(w->module, "depth_hold") == 0);
	CHECK_EQ_INT(w->n_globs, 1);
	CHECK(strcmp(w->globs[0].pattern, "nav.*") == 0);
	// subs: nav.depth, nav.imu, safety.rep (implicit), hangar.vehicle.admin.req (implicit)
	CHECK_EQ_INT(w->n_subs, 4);
	const struct hg_wiring_sub *sd = hg_wiring_find_sub(w, "nav.depth");
	CHECK(sd && strcmp(sd->type, "depth") == 0 && sd->rate_mhz == 50000 && sd->hash != 0);
	const struct hg_wiring_sub *sr = hg_wiring_find_sub(w, "safety.rep");
	CHECK(sr && sr->implicit && strcmp(sr->type, "rep:safety") == 0 && sr->slots == 16);
	// The manager's own request topic carries its generated type too.
	sr = hg_wiring_find_sub(w, "hangar.vehicle.admin.req");
	CHECK(sr && sr->implicit && strcmp(sr->type, "req:hangar") == 0 && sr->slots == 16 && sr->hash != 0);
	CHECK(sr && mf_type_find(&set, "req:hangar") && sr->hash == mf_type_find(&set, "req:hangar")->hash);
	CHECK(mf_type_find(&set, "req:hangar") && mf_type_find(&set, "req:hangar")->payload_bytes == 112 + 16384);
	// pubs: ctl.depth_hold, ctl.depth_hold.target, depth_hold.req, depth_hold.rep, depth_hold.params
	CHECK_EQ_INT(w->n_pubs, 5);
	const struct hg_wiring_pub *p = hg_wiring_find_pub(w, "ctl.depth_hold");
	CHECK(p && p->slots == 32 && p->rate_mhz == 50000 && p->stale_after_ns == 100000000);
	CHECK(p && p->n_inputs == 2 && strcmp(p->inputs[1], "nav.imu") == 0);
	CHECK(p && p->n_sources == 2); // nav.depth, and the operators' request topic the server reads
	p = hg_wiring_find_pub(w, "ctl.depth_hold.target");
	CHECK(p && p->rate_mhz == 500 && strcmp(p->inputs[0], "topside:cam.pilot") == 0);
	p = hg_wiring_find_pub(w, "depth_hold.req");
	CHECK(p && p->implicit && strcmp(p->type, "req:depth_hold") == 0 && p->slots == 16 && p->rate_mhz == 0);
	CHECK(p && p->hash == mf_type_find(&set, "req:depth_hold")->hash);
	p = hg_wiring_find_pub(w, "depth_hold.params");
	CHECK(p && p->implicit && strcmp(p->type, "params:depth_hold") == 0);
	CHECK_EQ_INT(w->n_serves, 2); // reset, and the implicit set_params
	CHECK(strcmp(w->serves[0].method, "reset") == 0 && w->serves[0].n_allow == 4 && w->serves[0].deadline_ns == 250000000);
	CHECK(strcmp(w->serves[1].method, "set_params") == 0 && strcmp(w->serves[1].m.rep_type, "set_params_rep") == 0 &&
	      w->serves[1].m.rep_size == 16);
	CHECK(strcmp(w->role, "vehicle") == 0);
	CHECK(w->serves[0].m.req_size == 8 && w->serves[0].m.req_hash == mf_type_find(&set, "empty")->hash);
	CHECK_EQ_INT(w->n_calls, 1);
	CHECK(strcmp(w->calls[0].target, "safety.disarm") == 0 && w->calls[0].remote == 0);
	// The default Allow list is written out; an actuator's is none.
	m = mf_find(&set, "safety");
	len = wiring_text(&set, m, text, sizeof text);
	CHECK(len > 0 && strstr(text, "serve disarm ") && strstr(text, "allow=depth_hold"));
	CHECK(strstr(text, "sub depth_hold.req type=req:depth_hold") != NULL);
	CHECK(strstr(text, "sub hangar.vehicle.admin.req") != NULL);
	CHECK(strstr(text, "pub safety.rep type=rep:safety") != NULL);
	CHECK(strstr(text, "sources=depth_hold.req,hangar.vehicle.admin.req") != NULL);
	CHECK_EQ_INT(hg_wiring_parse(text, (size_t)len, w, werr, sizeof werr), 0);
	// A sealed memfd that the library reads as the module would.
	int fd = wiring_memfd(text, (size_t)len);
	CHECK(fd >= 0);
	memset(w, 0, sizeof *w);
	CHECK_EQ_INT(hg_wiring_read_fd(fd, w, werr, sizeof werr), 0);
	CHECK(strcmp(w->module, "safety") == 0);
	CHECK(write(fd, "x", 1) < 0); // sealed
	close(fd);
	free(w);
}

/* An Allow list longer than 127 bytes reaches the library whole: five
 * tokens, the fourth of which a cut at 127 bytes turns into another name
 * ("topside:pilot" from "topside:pilot_console"). And a value the library
 * has no room for is an error, never a shorter value. */
static void test_long_allow_list(void)
{
	fresh_set();
	struct mf_module *m = add("srv", MOD "[Serve arm]\nRequest=empty\nReply=empty\n"
					     "Allow=topside:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa topside:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb "
					     "topside:ccccccccccccccccccccccccc topside:pilot_console topside:joystick\n");
	mf_set_check(&set);
	CHECK(!m->refused);
	static char text[16384];
	int len = wiring_text(&set, m, text, sizeof text);
	CHECK(len > 0);
	static struct hg_wiring w;
	char werr[200] = "";
	CHECK_EQ_INT(hg_wiring_parse(text, (size_t)len, &w, werr, sizeof werr), 0);
	const struct hg_wiring_serve *sv = NULL;
	for (int i = 0; i < w.n_serves; i++)
		if (strcmp(w.serves[i].method, "arm") == 0)
			sv = &w.serves[i];
	CHECK(sv != NULL);
	if (!sv)
		return;
	CHECK_EQ_INT(sv->n_allow, 5);
	int console = 0, pilot = 0, joystick = 0;
	for (int i = 0; i < sv->n_allow; i++) {
		console |= strcmp(sv->allow[i], "topside:pilot_console") == 0;
		pilot |= strcmp(sv->allow[i], "topside:pilot") == 0;
		joystick |= strcmp(sv->allow[i], "topside:joystick") == 0;
	}
	CHECK(console && joystick && !pilot);
	/* A value past any buffer: a type name of 2000 bytes on a sub line. */
	static char big[4096];
	int n = snprintf(big, sizeof big, "hangar-wiring 1\nmodule x\nsub a.b type=");
	memset(big + n, 'x', 2000);
	n += 2000;
	n += snprintf(big + n, sizeof big - (size_t)n, " hash=0000000000000001\n");
	CHECK(hg_wiring_parse(big, (size_t)n, &w, werr, sizeof werr) < 0);
}

/* The 127 limit holds for the bridge's subscriptions too, which come from
 * bridge.conf after the manifest's own were counted: 135 topics to send by
 * name are refused with the reason, 120 make a wiring the library takes. */
static void test_bridge_subscription_limit(void)
{
	char conf[1024], from[1024], to[1024];
	static char text[65536];
	const char *src = getenv("HANGAR_MSG_SRC");
	snprintf(from, sizeof from, "%s/bridge_link.msg", src && *src ? src : "msg");
	snprintf(to, sizeof to, "%s/bridge_link.msg", T_msg);
	copy_file(from, to);
	snprintf(conf, sizeof conf, "%s/bridge.conf", T_root);
	for (int sections = 9; sections >= 8; sections--) {
		int n = snprintf(text, sizeof text, "[Bridge]\nPair=lab\n");
		for (int s = 0; s < sections; s++) {
			n += snprintf(text + n, sizeof text - (size_t)n, "[Mirror");
			for (int k = 0; k < 15; k++)
				n += snprintf(text + n, sizeof text - (size_t)n, " t%d.x%d", s, k);
			n += snprintf(text + n, sizeof text - (size_t)n, "]\nDirection=up\n");
		}
		write_file(conf, text);
		fresh_set();
		snprintf(set.bridge_conf_path, sizeof set.bridge_conf_path, "%.*s", (int)sizeof set.bridge_conf_path - 1, conf);
		struct mf_module *b = add("bridge", MOD "[Bridge]\n");
		mf_set_check(&set);
		if (sections == 9) {
			CHECK(b->refused);
			CHECK(strstr(b->reason, "subscriptions by name") != NULL);
			continue;
		}
		CHECK(!b->refused);
		int len = wiring_text(&set, b, text, sizeof text);
		static struct hg_wiring w;
		char werr[200] = "";
		CHECK(len > 0 && hg_wiring_parse(text, (size_t)len, &w, werr, sizeof werr) == 0);
	}
}

/* A set holds 128 manifests. The directory's 129th and 130th, in name
 * order, cannot be listed as refused, having no entry: a warning names how
 * many were left out and the first of them. */
static void test_more_manifests_than_a_set_holds(void)
{
	char dir[1024], path[1200];
	snprintf(dir, sizeof dir, "%s/many", T_root);
	mkdir_p(dir);
	for (int i = 0; i < 130; i++) {
		snprintf(path, sizeof path, "%s/m%03d.module", dir, i);
		write_file(path, MOD);
	}
	fresh_set();
	CHECK_EQ_INT(mf_set_load_dir(&set, dir), 128);
	mf_set_check(&set);
	CHECK_EQ_INT(set.n_modules, 128);
	CHECK(mf_find(&set, "m127") != NULL && mf_find(&set, "m128") == NULL);
	int said = 0;
	for (int i = 0; i < set.n_warnings; i++)
		if (strstr(set.warnings[i], "2 manifests") && strstr(set.warnings[i], "m128.module"))
			said = 1;
	CHECK(said);
}

int main(void)
{
	hd_setup("manifest", NULL);
	RUN_TEST(test_every_key);
	RUN_TEST(test_defaults);
	RUN_TEST(test_values);
	RUN_TEST(test_parse_refusals);
	RUN_TEST(test_graph_refusals);
	RUN_TEST(test_implicit);
	RUN_TEST(test_bridge);
	RUN_TEST(test_wiring_roundtrip);
	RUN_TEST(test_long_allow_list);
	RUN_TEST(test_bridge_subscription_limit);
	RUN_TEST(test_more_manifests_than_a_set_holds);
	mf_set_free(&set);
	hd_teardown();
	TEST_MAIN_END();
}
