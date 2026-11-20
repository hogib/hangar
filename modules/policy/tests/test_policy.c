// SPDX-License-Identifier: GPL-3.0-or-later
// hangar-policy under a manager (docs/09-learning.md "Tests"), with the real
// mux beside it: this process is `tester`, the toy dataset's anchor and input
// part (toy.anchor, toy.target), the mux's other sources, and the caller of
// the policy's methods and of mux.set_mode. Models are made at the start by
// tests/make_models.py with python/hangar's writer and reference (no PyTorch).
// Neutral and flagged with no model; runtime_info; a load (checks, warm-up,
// swap) with no tick missed, its times measured; each check failing in turn
// leaves the running model untouched; a warm-up that overruns; the features
// the model sees; revert; a restart reloads current.hgw and previous.hgw; a
// non-finite output never published; Allow; the mux applying the residual
// only after set_mode names the running model and demoting a stale output;
// idle CPU. Under 30 s.
#include "hd_test.h"

#include <math.h>

#include "blake2s.h"
#include "depth.h"
#include "empty.h"
#include "hg_call.h"
#include "load_weights_rep.h"
#include "load_weights_req.h"
#include "mux_state.h"
#include "policy_active.h"
#include "policy_load.h"
#include "policy_version.h"
#include "runtime_info.h"
#include "safety_state.h"
#include "set_mode_rep.h"
#include "set_mode_req.h"
#include "wrench.h"

static pid_t mgr;
static const char *T_policy, *T_mux, *T_python, *T_pairer;
static char models[600], incoming[600], state[600];
static hg_pub *P_anchor, *P_target, *P_pilot, *P_ctl, *P_depth, *P_safety;
static hg_sub *S_out, *S_active, *S_load, *S_cmd, *S_mux, *S_rows;

static struct {
	float a, t, ctl_heave;
	int publish_target;
	int vary; /* a and t change at every tick */
	uint64_t next_ns, anchors;
} T = { .a = 1.0f, .t = 2.0f, .publish_target = 1 };

static struct {
	struct wrench out;
	struct hg_info out_info;
	uint64_t outs, outs_stale;
	struct policy_active active;
	struct hg_info active_info;
	uint64_t actives;
	struct policy_load load;
	uint64_t loads;
	uint64_t checking_ns, warming_ns, done_ns; /* since_ns of the current load's states */
	struct wrench cmd;
	struct mux_state mux;
	int have_out, have_active, have_load, have_cmd, have_mux;
	uint64_t lat_max_ns; /* anchor stamp to the output read here, the largest since reset */
} O;

static void copy_types(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	const char *types[] = { "policy_active", "policy_load", "load_weights_req", "load_weights_rep", "policy_version",
				"runtime_info", "mux_state", "mux_params", "set_mode_req", "set_mode_rep", "recorder_session",
				"pairer_status", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src ? src : "msg", types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		if (copy_file(from, to) < 0) {
			fprintf(stderr, "cannot copy %s\n", from);
			exit(2);
		}
	}
}

static char arch[32];

static void write_files(void)
{
	char cmd[2048], path[700];
	tfmt(cmd, sizeof cmd, "POLICY_PYTHON=%s python3 %s/../modules/policy/tests/make_models.py arch", T_python, T_python);
	FILE *p = popen(cmd, "r");
	if (!p || !fgets(arch, sizeof arch, p)) {
		fprintf(stderr, "make_models.py arch failed\n");
		exit(2);
	}
	pclose(p);
	arch[strcspn(arch, "\n")] = 0;
	tfmt(path, sizeof path, "%s/etc/hangar/datasets", T_root);
	mkdir_p(path);
	tfmt(path, sizeof path, "%s/etc/hangar/datasets/toy.dataset", T_root);
	write_file(path, "[Dataset]\nAnchor=toy.anchor\nAnchorRole=input\nFields=depth_m\nLateness=0\nResetAfter=200ms\n\n"
			 "[Input toy.target]\nJoin=asof:0\nFields=depth_m\n");
	tfmt(path, sizeof path, "%s/etc/hangar/policies", T_root);
	mkdir_p(path);
	char text[256];
	tfmt(text, sizeof text, "[Policy]\nName=toy\nDataset=toy\nArch=%s\nTickBudget=50%%\n", arch);
	tfmt(path, sizeof path, "%s/etc/hangar/policies/toy.policy", T_root);
	write_file(path, text);
	tfmt(state, sizeof state, "%s/var/lib/hangar/policy_toy", T_root);
	tfmt(incoming, sizeof incoming, "%s/incoming", state);
	tfmt(models, sizeof models, "%s/models", T_root);
	tfmt(path, sizeof path, "%s/etc/hangar/pairer.conf", T_root);
	tfmt(text, sizeof text, "[Pairer]\nDatasets=toy\nDatasetDir=%s/etc/hangar/datasets\n", T_root);
	write_file(path, text);
}

static void write_manifests(void)
{
	manifest("policy_toy", "[Module]\nExec=%s %s/etc/hangar/policies/toy.policy\n"
			       "[Subscribe toy.anchor]\n[Subscribe toy.target]\n"
			       "[Publish policy.toy.wrench]\nType=wrench\nRate=50\nInputs=toy.anchor toy.target policy.toy.active\n"
			       "[Publish policy.toy.active]\nType=policy_active\n"
			       "[Publish policy.toy.load]\nType=policy_load\n"
			       "[Serve load_weights]\nRequest=load_weights_req\nReply=load_weights_rep\nAllow=tester admin\n"
			       "[Serve revert]\nRequest=empty\nReply=policy_version\nAllow=tester admin\n"
			       "[Serve reset_state]\nRequest=empty\nReply=empty\nAllow=tester admin\n"
			       "[Serve runtime_info]\nRequest=empty\nReply=runtime_info\nAllow=tester admin\n"
			       "[Restart]\nBackoff=0.1s..0.2s\n",
		 T_policy, T_root);
	manifest("mux", "[Module]\nExec=%s\n"
			"[Subscribe pilot.wrench]\n[Subscribe ctl.depth_hold]\n[Subscribe nav.depth]\n[Subscribe safety.state]\n"
			"[Subscribe policy.toy.wrench]\n[Subscribe policy.toy.active]\n"
			"[Publish ctl.wrench]\nType=wrench\nRate=50\nSources=pilot.wrench ctl.depth_hold policy.toy.wrench nav.depth\n"
			"[Publish mux.state]\nType=mux_state\nRate=10\n"
			"[Params]\nType=mux_params\nAllow=tester admin\n"
			"[Serve set_mode]\nRequest=set_mode_req\nReply=set_mode_rep\nAllow=tester admin\n"
			"[Restart]\nBackoff=0.1s..0.2s\n",
		 T_mux);
	/* The pairer, on the same spec: its rows are what the policy's features
	 * must equal (docs/09 "Features at run time"). */
	manifest("pairer", "[Module]\nExec=%s %s/etc/hangar/pairer.conf\n"
			   "[Subscribe toy.anchor]\n[Subscribe toy.target]\n"
			   "[Publish dataset.toy]\nType=dataset:toy\nSlots=256\n"
			   "[Publish pairer.status]\nType=pairer_status\n",
		 T_pairer, T_root);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Subscribe dataset.toy]\n[Subscribe policy.toy.wrench]\n[Subscribe policy.toy.active]\n[Subscribe policy.toy.load]\n"
			   "[Subscribe ctl.wrench]\n[Subscribe mux.state]\n"
			   "[Publish toy.anchor]\nType=depth\nRate=50\n"
			   "[Publish toy.target]\nType=depth\nRate=50\n"
			   "[Publish pilot.wrench]\nType=wrench\nRate=50\n"
			   "[Publish ctl.depth_hold]\nType=wrench\nRate=50\n"
			   "[Publish nav.depth]\nType=depth\nRate=50\n"
			   "[Publish safety.state]\nType=safety_state\nRate=10\n"
			   "[Call policy_toy.load_weights]\n[Call policy_toy.revert]\n[Call policy_toy.reset_state]\n"
			   "[Call policy_toy.runtime_info]\n[Call mux.set_mode]\n");
	/* The manager's check: hangar-policy under another name, and without
	 * its .load output. */
	manifest("misnamed", "[Module]\nExec=%s %s/etc/hangar/policies/toy.policy\n[Restart]\nPolicy=never\n"
			     "[Publish policy.toy2.wrench]\nType=wrench\nRate=50\n",
		 T_policy, T_root);
	manifest("policy_lacking", "[Module]\nExec=%s %s/etc/hangar/policies/lacking.policy\n[Restart]\nPolicy=never\n"
				   "[Publish policy.lacking.wrench]\nType=wrench\nRate=50\n"
				   "[Publish policy.lacking.active]\nType=policy_active\n",
		 T_policy, T_root);
	char lp[700];
	tfmt(lp, sizeof lp, "%s/etc/hangar/policies/lacking.policy", T_root);
	write_file(lp, "[Policy]\nName=lacking\nDataset=toy\nArch=0000000000000001\n");
	manifest("intruder", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n[Call policy_toy.runtime_info]\n");
}

static void become(const char *module)
{
	char path[600];
	if (wiring_to_file(module, path, sizeof path) < 0) {
		printf("    cannot fetch the wiring of %s\n", module);
		exit(2);
	}
	setenv("HANGAR_WIRING_FILE", path, 1);
	setenv("HANGAR_MODULE", module, 1);
	unsetenv("HANGAR_CONTROL");
	if (hg_init() < 0) {
		printf("    hg_init as %s: %s\n", module, hg_init_error());
		exit(2);
	}
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_MODULE");
}

static void read_outputs(void)
{
	struct hg_info info;
	struct wrench w;
	while (hg_next(S_out, &w, sizeof w, &info) == HG_MSG) {
		O.out = w;
		O.out_info = info;
		O.outs++;
		O.outs_stale += (info.flags & 1) != 0;
		uint64_t lat = hg_now_ns() - info.stamp_ns;
		if (lat > O.lat_max_ns)
			O.lat_max_ns = lat;
		O.have_out = 1;
	}
	struct policy_active a;
	while (hg_next(S_active, &a, sizeof a, &info) == HG_MSG) {
		O.active = a;
		O.active_info = info;
		O.actives++;
		O.have_active = 1;
	}
	struct policy_load l;
	while (hg_next(S_load, &l, sizeof l, &info) == HG_MSG) {
		if (!O.have_load || l.version != O.load.version)
			O.checking_ns = O.warming_ns = O.done_ns = 0;
		O.load = l;
		O.loads++;
		O.have_load = 1;
		if (l.state == 1)
			O.checking_ns = l.since_ns;
		else if (l.state == 2)
			O.warming_ns = l.since_ns;
		else if (l.state == 3)
			O.done_ns = l.since_ns;
	}
	while (hg_next(S_cmd, &w, sizeof w, &info) == HG_MSG) {
		O.cmd = w;
		O.have_cmd = 1;
	}
	struct mux_state ms;
	while (hg_next(S_mux, &ms, sizeof ms, &info) == HG_MSG) {
		O.mux = ms;
		O.have_mux = 1;
	}
}

/* The sources at 50 Hz: the part before the anchor, so that the asof join
 * of the anchor finds the part's message of the same tick. */
static void pump(long ms)
{
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000u;
	do {
		uint64_t now = hg_now_ns();
		if (now >= T.next_ns) {
			T.next_ns = now + 20000000u;
			if (T.vary) {
				T.a = 0.37f * (float)(T.anchors % 97);
				T.t = -0.21f * (float)(T.anchors % 89) + 1.0f;
			}
			struct depth d = { .temp_c = 10, .pressure_mbar = 1013 };
			if (T.publish_target) {
				d.depth_m = T.t;
				hg_publish(P_target, &d, sizeof d, now);
			}
			d.depth_m = T.a;
			hg_publish(P_anchor, &d, sizeof d, now);
			T.anchors++;
			struct wrench zero = { 0 }, ctl = { .heave = T.ctl_heave };
			hg_publish(P_pilot, &zero, sizeof zero, now);
			hg_publish(P_ctl, &ctl, sizeof ctl, now);
			d.depth_m = 2.0f;
			hg_publish(P_depth, &d, sizeof d, now);
			struct safety_state s;
			memset(&s, 0, sizeof s);
			s.state = 1;
			s.outputs_enabled = 1;
			hg_publish(P_safety, &s, sizeof s, now);
		}
		hg_sub *subs[5] = { S_out, S_active, S_load, S_cmd, S_mux };
		hg_wait(subs, 5, 5);
		read_outputs();
	} while (hg_now_ns() < end);
}

static hg_call_result call(const char *target, const struct hg_type *rq, const void *req, const struct hg_type *rp,
			   void *rep)
{
	hg_pending *pc;
	hg_call_result r = { .status = HG_E_NOROUTE };
	int st = hg_call_start(target, rq, req, rp, &pc, 2000);
	if (st != HG_OK) {
		r.status = st;
		return r;
	}
	do {
		pump(10);
		r = hg_call_poll(pc, rep);
	} while (r.status == HG_PENDING);
	return r;
}

static int digest_hex(const char *path, char hex[65], uint64_t *hash)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	static unsigned char buf[1 << 20];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	unsigned char d[32];
	nn_blake2s(d, 32, buf, n, NULL, 0);
	for (int i = 0; i < 32; i++)
		snprintf(hex + 2 * i, 3, "%02x", d[i]);
	if (hash)
		memcpy(hash, d, 8);
	return 0;
}

/* hd_test.h's copy_file is for text; models are binary. */
static int bin_copy(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb");
	if (!in)
		return -1;
	FILE *out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	char buf[65536];
	size_t n;
	int rc = 0;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		if (fwrite(buf, 1, n, out) != n)
			rc = -1;
	fclose(in);
	if (fclose(out) != 0)
		rc = -1;
	return rc;
}

/* Copies a model into the incoming directory and asks for it; the reply's
 * status, and the version into *version. */
static int load(const char *model, const char *bad_hex, uint32_t *version, uint64_t *hash)
{
	char from[800], to[800];
	tfmt(from, sizeof from, "%s/%s.hgw", models, model);
	tfmt(to, sizeof to, "%s/%s.hgw", incoming, model);
	if (bin_copy(from, to) < 0)
		return -100;
	load_weights_req req;
	memset(&req, 0, sizeof req);
	tfmt(req.name, sizeof req.name, "%s.hgw", model);
	if (digest_hex(to, req.blake2s, hash) < 0)
		return -101;
	if (bad_hex)
		memcpy(req.blake2s, bad_hex, 64);
	load_weights_rep rep;
	memset(&rep, 0, sizeof rep);
	hg_call_result r = call("policy_toy.load_weights", &LOAD_WEIGHTS_REQ_TYPE, &req, &LOAD_WEIGHTS_REP_TYPE, &rep);
	if (r.status != HG_OK)
		printf("    load_weights %s: %s %s\n", model, hg_status_name(r.status), r.reason);
	if (version)
		*version = rep.version;
	return r.status;
}

/* Waits for the load of version v to end: 3 done or 4 rejected. */
static int wait_load(uint32_t v, long timeout_ms)
{
	uint64_t t0 = now_ms();
	while (now_ms() - t0 < (uint64_t)timeout_ms) {
		pump(10);
		if (O.have_load && O.load.version == v && (O.load.state == 3 || O.load.state == 4))
			return O.load.state;
	}
	return 0;
}

static int near(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

/* ---- Tests ----------------------------------------------------------------- */

static uint64_t ds_hash;

static void test_start_neutral(void)
{
	write_files();
	write_manifests();
	mgr = manager_start();
	CHECK(wait_state("tester", "stopped", 5000));
	become("tester");
	P_anchor = hg_pub_open("toy.anchor", &DEPTH_TYPE, NULL);
	P_target = hg_pub_open("toy.target", &DEPTH_TYPE, NULL);
	P_pilot = hg_pub_open("pilot.wrench", &WRENCH_TYPE, NULL);
	P_ctl = hg_pub_open("ctl.depth_hold", &WRENCH_TYPE, NULL);
	P_depth = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);
	P_safety = hg_pub_open("safety.state", &SAFETY_STATE_TYPE, NULL);
	S_out = hg_sub_open("policy.toy.wrench", &WRENCH_TYPE);
	S_active = hg_sub_open("policy.toy.active", &POLICY_ACTIVE_TYPE);
	S_load = hg_sub_open("policy.toy.load", &POLICY_LOAD_TYPE);
	S_cmd = hg_sub_open("ctl.wrench", &WRENCH_TYPE);
	S_mux = hg_sub_open("mux.state", &MUX_STATE_TYPE);
	CHECK(P_anchor && P_target && P_pilot && P_ctl && P_depth && P_safety && S_out && S_active && S_load && S_cmd && S_mux);
	pump(300);
	CHECK(wait_state("policy_toy", "ready", 5000));
	CHECK(wait_state("mux", "ready", 5000));
	/* No model: a neutral wrench, flagged stale, on every tick. */
	uint64_t o0 = O.outs, a0 = T.anchors;
	pump(500);
	CHECK(O.have_out);
	CHECK(O.outs - o0 + 2 >= T.anchors - a0);
	CHECK(O.out_info.flags & 1);
	CHECK(O.out.heave == 0 && O.out.surge == 0);
	CHECK(!O.have_active);
}

static void test_runtime_info_and_models(void)
{
	runtime_info ri;
	memset(&ri, 0, sizeof ri);
	empty e = { 0 };
	hg_call_result r = call("policy_toy.runtime_info", &EMPTY_TYPE, &e, &RUNTIME_INFO_TYPE, &ri);
	CHECK_EQ_INT(r.status, HG_OK);
	CHECK(strcmp(ri.dataset, "toy") == 0 && ri.dataset_hash != 0);
	CHECK(strstr(ri.ops, "dense:1") != NULL && strstr(ri.ops, "lstm:1") != NULL);
	char ah[24];
	tfmt(ah, sizeof ah, "%016llx", (unsigned long long)ri.arch);
	CHECK(strcmp(ah, arch) == 0);
	CHECK(ri.tick_period_ns >= 19000000 && ri.tick_period_ns <= 21000000);
	ds_hash = ri.dataset_hash;
	printf("    runtime_info: %u ops (%s), dataset %s %016llx, tick %llu us\n", ri.n_ops, ri.ops, ri.dataset,
	       (unsigned long long)ri.dataset_hash, (unsigned long long)(ri.tick_period_ns / 1000));
	char cmd[2048];
	tfmt(cmd, sizeof cmd, "POLICY_PYTHON=%s python3 %s/../modules/policy/tests/make_models.py models %s %016llx",
	     T_python, T_python, models, (unsigned long long)ds_hash);
	CHECK_EQ_INT(system(cmd), 0);
}

static uint64_t hash_a, hash_b;

static void test_load_swap(void)
{
	O.lat_max_ns = 0;
	pump(500);
	uint64_t rest_ns = O.lat_max_ns;
	uint32_t v = 0;
	O.lat_max_ns = 0;
	uint64_t a0 = T.anchors, o0 = O.outs;
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(load("good_a", NULL, &v, &hash_a), HG_OK);
	CHECK_EQ_INT(wait_load(v, 5000), 3);
	uint64_t t1 = hg_now_ns();
	pump(100);
	uint64_t load_ns = O.lat_max_ns;
	printf("    anchor to output, largest: %.0f us at rest (500 ms), %.0f us across the load and swap\n",
	       rest_ns / 1e3, load_ns / 1e3);
	CHECK(load_ns < 10000000u);
	CHECK(O.have_active && O.active.version == v && O.active.cause == 1 && O.active.hash == hash_a);
	CHECK(O.active.rows == 123 && strcmp(O.active.dataset, "toy") == 0);
	/* The output now: heave = 0.5 a + 0.25 t + 0.1, surge = a - t, not
	 * stale, referencing the anchor and the active message. */
	CHECK(near(O.out.heave, 0.5f * T.a + 0.25f * T.t + 0.1f) && near(O.out.surge, T.a - T.t));
	CHECK(!(O.out_info.flags & 1));
	int refs_active = 0, refs_anchor = 0, refs_target = 0;
	for (uint32_t i = 0; i < O.out_info.n_inputs; i++) {
		refs_active |= O.out_info.inputs[i].topic_id == hg_topic_id("policy.toy.active") &&
			       O.out_info.inputs[i].seq == O.active_info.seq;
		refs_anchor |= O.out_info.inputs[i].topic_id == hg_topic_id("toy.anchor");
		refs_target |= O.out_info.inputs[i].topic_id == hg_topic_id("toy.target");
	}
	CHECK(refs_active && refs_anchor && refs_target);
	/* The active message of a load references the load_weights request. */
	int refs_req = 0;
	for (uint32_t i = 0; i < O.active_info.n_inputs; i++)
		refs_req |= O.active_info.inputs[i].topic_id == hg_topic_id("tester.req");
	CHECK(refs_req);
	/* No tick missed across the load and the swap. */
	uint64_t anchors = T.anchors - a0, outs = O.outs - o0;
	CHECK(outs + 2 >= anchors);
	printf("    load: checks %.2f ms, warm-up %.2f ms (%u ticks), request to swap %.1f ms; %llu outputs for %llu anchors\n",
	       (double)(O.warming_ns - O.checking_ns) / 1e6, (double)(O.done_ns - O.warming_ns) / 1e6, 3,
	       (double)(t1 - t0) / 1e6, (unsigned long long)outs, (unsigned long long)anchors);
	/* The files: current.hgw is the new one. */
	char cur[700], hex[65];
	tfmt(cur, sizeof cur, "%s/current.hgw", state);
	pump(200);
	uint64_t h = 0;
	CHECK(digest_hex(cur, hex, &h) == 0 && h == hash_a);
}

static void test_checks_fail(void)
{
	const struct {
		const char *model;
		const char *bad_hex;
		int check;
	} cases[] = {
		{ "good_b", "00000000000000000000000000000000000000000000000000000000000000ff", 1 },
		{ "bad_arch", NULL, 2 },
		{ "wrong_ds", NULL, 4 },
		{ "bad_nan", NULL, 5 },
		{ "bad_probe", NULL, 6 },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		uint32_t v = 0;
		CHECK_EQ_INT(load(cases[i].model, cases[i].bad_hex, &v, NULL), HG_OK);
		CHECK_EQ_INT(wait_load(v, 5000), 4);
		CHECK_EQ_INT(O.load.check, cases[i].check);
		if (O.load.check != cases[i].check)
			printf("    %s: check %u: %s\n", cases[i].model, O.load.check, O.load.reason);
		pump(60);
		/* The running model untouched, and the outputs name it. */
		CHECK(O.active.hash == hash_a);
		int names_running = 0;
		for (uint32_t k = 0; k < O.out_info.n_inputs; k++)
			names_running |= O.out_info.inputs[k].topic_id == hg_topic_id("policy.toy.active") &&
					 O.out_info.inputs[k].seq == O.active_info.seq;
		CHECK(names_running);
		CHECK(near(O.out.heave, 0.5f * T.a + 0.25f * T.t + 0.1f));
	}
	/* A second load while one is under way is refused. */
	uint32_t v1 = 0;
	CHECK_EQ_INT(load("good_b", NULL, &v1, NULL), HG_OK);
	CHECK_EQ_INT(load("good_b", NULL, NULL, NULL), HG_E_REFUSED);
	CHECK_EQ_INT(wait_load(v1, 5000), 3);
	pump(60);
	CHECK(near(O.out.heave, -T.a + 0.1f) && near(O.out.surge, 2 * T.t));
	char cur[700], hex[65];
	tfmt(cur, sizeof cur, "%s/current.hgw", state);
	pump(200);
	digest_hex(cur, hex, &hash_b);
	CHECK(O.active.hash == hash_b);
}

static void test_warmup_overrun(void)
{
	char path[700];
	tfmt(path, sizeof path, "%s/test-warm-delay-us", state);
	write_file(path, "30000\n");
	uint32_t v = 0;
	CHECK_EQ_INT(load("good_a", NULL, &v, NULL), HG_OK);
	CHECK_EQ_INT(wait_load(v, 5000), 4);
	CHECK_EQ_INT(O.load.check, 8);
	CHECK(strstr(O.load.reason, "overran") != NULL);
	unlink(path);
	pump(60);
	CHECK(O.active.hash == hash_b);
}

static void test_features(void)
{
	uint32_t v = 0;
	CHECK_EQ_INT(load("ident", NULL, &v, NULL), HG_OK);
	CHECK_EQ_INT(wait_load(v, 5000), 3);
	T.a = 1.25f;
	T.t = -0.75f;
	pump(100);
	/* The model sees the anchor's field and the asof-joined part's field. */
	CHECK(near(O.out.heave, 1.25f) && near(O.out.surge, -0.75f));
	/* The input part goes stale: the output is flagged (docs/09 "Features at run time"). */
	T.publish_target = 0;
	pump(200);
	CHECK(O.out_info.flags & 1);
	T.publish_target = 1;
	pump(100);
	CHECK(!(O.out_info.flags & 1));
	T.a = 1.0f;
	T.t = 2.0f;
}

/* The policy's features against the pairer's rows, live: with ident
 * running (heave = the anchor's depth_m, surge = the target's, as the model
 * sees them), each output equals its row's two input fields, bit for bit,
 * for the same anchor seq; with the input part stopped, both sides the same
 * (zeros with the missing bit, or the stale value). */
static void test_features_vs_pairer(void)
{
	S_rows = hg_sub_open("dataset.toy", NULL);
	CHECK(S_rows != NULL);
	if (!S_rows)
		return;
	CHECK(wait_state("pairer", "ready", 5000));
	pump(200);
	uint32_t slen;
	const char *text = hg_sub_schema(S_rows, &slen);
	static struct hg_schema sch;
	CHECK(text && hg_schema_parse(text, slen, &sch, NULL, 0) == 0);
	const struct hg_field *fa = hg_type_field(&sch.type, "toy.anchor.depth_m");
	const struct hg_field *ft = hg_type_field(&sch.type, "toy.target.depth_m");
	CHECK(fa && ft);
	if (!fa || !ft)
		return;
	static struct {
		uint64_t seq;
		float heave, surge;
	} outs[512];
	int n_out = 0, matched = 0, differ = 0, phase_missing = 0;
	static unsigned char rowbuf[4096];
	for (int phase = 0; phase < 2; phase++) {
		T.vary = 1;
		T.publish_target = phase == 0;
		uint64_t end = now_ms() + 600;
		while (now_ms() < end) {
			uint64_t o0 = O.outs;
			pump(20);
			if (O.outs != o0 && n_out < 512) {
				for (uint32_t i = 0; i < O.out_info.n_inputs; i++)
					if (O.out_info.inputs[i].topic_id == hg_topic_id("toy.anchor"))
						{
						outs[n_out].seq = O.out_info.inputs[i].seq;
						outs[n_out].heave = O.out.heave;
						outs[n_out++].surge = O.out.surge;
					}
			}
			struct hg_info info;
			while (hg_next(S_rows, rowbuf, sizeof rowbuf, &info) == HG_MSG) {
				uint64_t aseq;
				float a, t;
				memcpy(&aseq, rowbuf, 8);
				memcpy(&a, rowbuf + fa->offset, 4);
				memcpy(&t, rowbuf + ft->offset, 4);
				for (int k = 0; k < n_out; k++)
					if (outs[k].seq == aseq) {
						matched++;
						phase_missing += phase;
						if (memcmp(&outs[k].heave, &a, 4) != 0 || memcmp(&outs[k].surge, &t, 4) != 0) {
							if (differ++ < 3)
								printf("    anchor %llu: policy %g %g, row %g %g\n", (unsigned long long)aseq,
								       outs[k].heave, outs[k].surge, a, t);
						}
					}
			}
		}
	}
	T.vary = 0;
	T.publish_target = 1;
	T.a = 1.0f;
	T.t = 2.0f;
	printf("    features vs the pairer's rows: %d anchors compared (%d with the input part stopped), %d differ\n",
	       matched, phase_missing, differ);
	/* The pairer emits only the rows the outputs read here had time to meet: at least half. */
	CHECK(matched >= 20 && phase_missing >= 10);
	CHECK_EQ_INT(differ, 0);
	pump(100);
}

static void test_revert(void)
{
	/* Running: ident, previous: good_b. */
	policy_version pv;
	memset(&pv, 0, sizeof pv);
	empty e = { 0 };
	hg_call_result r = call("policy_toy.revert", &EMPTY_TYPE, &e, &POLICY_VERSION_TYPE, &pv);
	CHECK_EQ_INT(r.status, HG_OK);
	pump(100);
	CHECK(O.active.cause == 2 && O.active.hash == hash_b && pv.hash == hash_b && O.active.version == pv.version);
	CHECK(near(O.out.heave, -T.a + 0.1f));
	/* The active message references the revert request. */
	int refs_req = 0;
	for (uint32_t i = 0; i < O.active_info.n_inputs; i++)
		refs_req |= O.active_info.inputs[i].topic_id == hg_topic_id("tester.req");
	CHECK(refs_req);
	pump(200);
	char cur[700], hex[65];
	uint64_t h = 0;
	tfmt(cur, sizeof cur, "%s/current.hgw", state);
	CHECK(digest_hex(cur, hex, &h) == 0 && h == hash_b);
}

static void test_restart_reloads(void)
{
	struct mstat m0;
	CHECK(module_status("policy_toy", &m0));
	char out[4096];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "restart", "policy_toy", (char *)NULL), 0);
	uint64_t t0 = now_ms();
	while (now_ms() - t0 < 5000) {
		pump(20);
		if (O.active.cause == 0 && O.active.hash == hash_b)
			break;
	}
	CHECK(O.active.cause == 0 && O.active.hash == hash_b);
	pump(100);
	CHECK(near(O.out.heave, -T.a + 0.1f));
	/* previous.hgw came back too: revert works after the restart. */
	policy_version pv;
	empty e = { 0 };
	CHECK_EQ_INT(call("policy_toy.revert", &EMPTY_TYPE, &e, &POLICY_VERSION_TYPE, &pv).status, HG_OK);
	pump(100);
	CHECK(O.active.cause == 2 && O.active.hash != hash_b);
}

static void test_nonfinite(void)
{
	uint32_t v = 0;
	T.a = 0.0f;
	T.t = 0.0f;
	CHECK_EQ_INT(load("explode", NULL, &v, NULL), HG_OK);
	CHECK_EQ_INT(wait_load(v, 5000), 3);
	pump(100);
	uint64_t o0 = O.outs;
	T.a = 1.0f;
	T.t = 1.0f;
	pump(300);
	/* inf: nothing published at those ticks. */
	CHECK(O.outs - o0 <= 2);
	/* Counted, in the status line (refreshed at 1 Hz). */
	struct mstat m;
	int counted = 0;
	for (uint64_t t0 = now_ms(); !counted && now_ms() - t0 < 2500;) {
		pump(100);
		counted = module_status("policy_toy", &m) && strstr(m.status, "non-finite") && !strstr(m.status, " 0 non-finite");
	}
	CHECK(counted);
	printf("    status: %s\n", m.status);
	T.a = 0.0f;
	T.t = 0.0f;
	pump(100);
	CHECK(O.outs - o0 >= 3);
	T.a = 1.0f;
	T.t = 2.0f;
	CHECK_EQ_INT(load("good_a", NULL, &v, NULL), HG_OK);
	CHECK_EQ_INT(wait_load(v, 5000), 3);
}

static void test_allow(void)
{
	/* admin, from the command line. */
	char out[4096];
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "call", "policy_toy.runtime_info", (char *)NULL), 0);
	CHECK(strstr(out, "dense:1") != NULL);
	/* A module not in Allow: the manager refuses its manifest, so it never
	 * gets a wiring to call with. */
	CHECK(file_contains(T_log, "intruder is not in the method's Allow list"));
	char path[600];
	CHECK(wiring_to_file("intruder", path, sizeof path) < 0);
	CHECK(file_contains(T_log, "hangar-policy runs policy toy, so the module must be policy_toy"));
	CHECK(file_contains(T_log, "needs [Publish policy.lacking.load] Type=policy_load"));
}

/* hangar.learn and hangar.topic, run while this process drives the anchor. */
static void test_python_side(void)
{
	char ds[24], ha[24], script[700];
	tfmt(ds, sizeof ds, "%016llx", (unsigned long long)ds_hash);
	tfmt(ha, sizeof ha, "%016llx", (unsigned long long)hash_a);
	tfmt(script, sizeof script, "%s/../modules/policy/tests/learn_live.py", T_python);
	CHECK(O.active.hash == hash_a);
	pid_t pid = fork();
	if (pid == 0) {
		setenv("PYTHONPATH", T_python, 1);
		setenv("POLICY_PYTHON", T_python, 1);
		setenv("HANGAR_BIN", T_hangar, 1);
		execlp("python3", "python3", script, models, ds, ha, (char *)NULL);
		_exit(127);
	}
	int status = -1;
	uint64_t t0 = now_ms();
	while (now_ms() - t0 < 20000) {
		pump(20);
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid) {
			status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
			break;
		}
	}
	if (status < 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}
	CHECK_EQ_INT(status, 0);
	pump(60);
	CHECK(O.active.hash == hash_a);
}

static void test_mux_residual(void)
{
	/* Running good_a: heave 0.5 a + 0.25 t + 0.1 = 0.1 with a = t = 0, inside
	 * the residual envelope (a residual held at it would be demoted). */
	T.ctl_heave = 0.2f;
	T.a = 0.0f;
	T.t = 0.0f;
	pump(100);
	set_mode_req req;
	set_mode_rep rep;
	memset(&req, 0, sizeof req);
	tfmt(req.mode, sizeof req.mode, "residual:toy");
	req.hash = O.active.hash ^ 1;
	CHECK_EQ_INT(call("mux.set_mode", &SET_MODE_REQ_TYPE, &req, &SET_MODE_REP_TYPE, &rep).status, HG_E_REFUSED);
	req.hash = O.active.hash;
	CHECK_EQ_INT(call("mux.set_mode", &SET_MODE_REQ_TYPE, &req, &SET_MODE_REP_TYPE, &rep).status, HG_OK);
	pump(200);
	CHECK(strcmp(O.mux.mode, "residual:toy") == 0);
	if (strcmp(O.mux.mode, "residual:toy") != 0)
		printf("    mux mode %s\n", O.mux.mode);
	/* The controller's heave plus the policy's. */
	CHECK(fabsf(O.cmd.heave - 0.3f) < 0.02f);
	printf("    residual: ctl.wrench heave %.3f (controller 0.2, policy 0.1)\n", O.cmd.heave);
	/* A load and a revert are followed, not demoted: good_b's heave is
	 * -a + 0.1, the same 0.1 at a = 0. */
	uint32_t v = 0;
	CHECK_EQ_INT(load("good_b", NULL, &v, NULL), HG_OK);
	CHECK_EQ_INT(wait_load(v, 5000), 3);
	pump(200);
	CHECK(strcmp(O.mux.mode, "residual:toy") == 0 && fabsf(O.cmd.heave - 0.3f) < 0.02f);
	policy_version pv;
	empty e = { 0 };
	CHECK_EQ_INT(call("policy_toy.revert", &EMPTY_TYPE, &e, &POLICY_VERSION_TYPE, &pv).status, HG_OK);
	pump(200);
	CHECK(strcmp(O.mux.mode, "residual:toy") == 0 && O.active.hash == hash_a);
	/* The policy's input part stops: its output is flagged, the mux demotes. */
	T.publish_target = 0;
	pump(400);
	CHECK(strncmp(O.mux.mode, "residual", 8) != 0);
	T.publish_target = 1;
	T.a = 1.0f;
	T.t = 2.0f;
	pump(100);
}

static void test_idle_cpu(void)
{
	struct mstat m;
	CHECK(module_status("policy_toy", &m) && m.pid > 0);
	long long t0 = th_cpu_ticks(m.pid);
	uint64_t s = now_ms();
	pump(1000);
	double pct = th_cpu_percent(t0, th_cpu_ticks(m.pid), (long)(now_ms() - s));
	printf("    policy at 50 Hz with a model: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 10);
}

static void test_shutdown(void)
{
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(void)
{
	T_policy = getenv("POLICY_BIN");
	T_mux = getenv("MUX_BIN");
	T_python = getenv("POLICY_PYTHON");
	T_pairer = getenv("PAIRER_BIN");
	if (!T_policy || !T_mux || !T_python || !T_pairer) {
		fprintf(stderr, "POLICY_BIN, MUX_BIN, PAIRER_BIN and POLICY_PYTHON must be set\n");
		return 2;
	}
	hd_setup("policy", NULL);
	copy_types();
	RUN_TEST(test_start_neutral);
	RUN_TEST(test_runtime_info_and_models);
	RUN_TEST(test_load_swap);
	RUN_TEST(test_checks_fail);
	RUN_TEST(test_warmup_overrun);
	RUN_TEST(test_features);
	RUN_TEST(test_features_vs_pairer);
	RUN_TEST(test_revert);
	RUN_TEST(test_restart_reloads);
	RUN_TEST(test_nonfinite);
	RUN_TEST(test_allow);
	RUN_TEST(test_python_side);
	RUN_TEST(test_mux_residual);
	RUN_TEST(test_idle_cpu);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
