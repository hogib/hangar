// SPDX-License-Identifier: GPL-3.0-or-later
// vision (modules/vision/README.md): the presets and the chain grammar as
// pure functions; then the module under a manager, with this process as
// `tester` publishing cam.decoded at 1280x720: every cam.pilot frame
// byte-identical to lib/vision's own result on the same frame and chain (the
// equivalence target's approach, the frames followed one by one so the
// visibility correction's smoothing sees the same sequence), its reference to
// the frame it was made from, set_params of each preset and of a custom chain,
// refusals, the @restart thread count, the cost per frame and idle CPU.
#include "hd_test.h"

#include "auto_vis.h"
#include "effect_chain.h"
#include "hg_type.h"
#include "img_stats.h"
#include "presets.h"
#include "video_frame.h"
#include "vision_params.h"

#define W 1280
#define H 720
#define FRAMES 10

static pid_t mgr;
static const char *T_vision;
static char conf_path[600];
static hg_pub *P_in;
static hg_sub *S_out;
static struct hg_frame_type ftype;
static unsigned char *inbuf, *outbuf;
static VideoFrame *lf;
static AutoVis *lav;
static uint64_t stamp = 1000000000ull;

/* ---- The presets and the grammar, without a manager ------------------------------ */

static void test_presets(void)
{
	struct vision_conf c;
	vision_conf_defaults(&c);
	CHECK_EQ_INT(c.n_presets, 5);
	CHECK(strcmp(c.input, "cam.decoded") == 0 && strcmp(c.output, "cam.pilot") == 0);
	const char *names[] = { "normal", "auto", "edges", "sharpen", "lowlight" };
	for (int i = 0; i < 5; i++)
		CHECK(strcmp(c.presets[i].name, names[i]) == 0);
	CHECK(c.presets[0].chain.stage_count == 0 && !c.presets[0].chain.auto_visibility);
	CHECK(c.presets[1].chain.stage_count == 0 && c.presets[1].chain.auto_visibility);
	CHECK(c.presets[2].chain.stage_count == 2 && c.presets[2].chain.stages[0].effect == EFFECT_BLUR &&
	      c.presets[2].chain.stages[0].blur_strength == 2 && c.presets[2].chain.stages[1].effect == EFFECT_SOBEL &&
	      c.presets[2].chain.stages[1].sobel_threshold == 16);
	CHECK(c.presets[3].chain.stage_count == 1 && c.presets[3].chain.stages[0].effect == EFFECT_CONTRAST);
	CHECK(c.presets[4].chain.auto_visibility && c.presets[4].chain.stages[0].effect == EFFECT_LIGHT &&
	      c.presets[4].chain.stages[0].light_level == 176);

	EffectChainConfig ch;
	char err[160];
	CHECK_EQ_INT(vision_chain_parse("blur:strength=3 sobel:threshold=32 canny:low=10,high=50 light", &ch, err,
					sizeof err), 0);
	CHECK(ch.stage_count == 4 && ch.stages[2].canny_low == 10 && ch.stages[2].canny_high == 50 &&
	      ch.stages[2].canny_strength == 0 && ch.stages[3].light_level == 128);
	CHECK_EQ_INT(vision_chain_parse("sharpen", &ch, err, sizeof err), -1);
	CHECK(strstr(err, "not an operator") != NULL);
	CHECK_EQ_INT(vision_chain_parse("blur:level=2", &ch, err, sizeof err), -1);
	CHECK(strstr(err, "no setting level") != NULL);
	CHECK_EQ_INT(vision_chain_parse("blur:strength=300", &ch, err, sizeof err), -1);
	CHECK_EQ_INT(vision_chain_parse("invert invert invert invert invert invert invert invert invert", &ch, err,
					sizeof err), -1);
	CHECK(strstr(err, "more than 8") != NULL);

	const char *conf = "[Vision]\nInput=cam.raw\n[Preset edges]\nChain=canny\n[Preset mine]\nAuto=yes\nChain=contrast\n";
	CHECK_EQ_INT(vision_conf_parse(&c, conf, strlen(conf), err, sizeof err), 0);
	CHECK(strcmp(c.input, "cam.raw") == 0 && c.n_presets == 6);
	CHECK(c.presets[2].chain.stage_count == 1 && c.presets[2].chain.stages[0].effect == EFFECT_CANNY);
	CHECK(strcmp(c.presets[5].name, "mine") == 0 && c.presets[5].chain.auto_visibility);
	const char *bad = "[Preset custom]\nChain=invert\n";
	CHECK_EQ_INT(vision_conf_parse(&c, bad, strlen(bad), err, sizeof err), -1);
	const char *bad2 = "[Vision]\nColour=red\n";
	CHECK_EQ_INT(vision_conf_parse(&c, bad2, strlen(bad2), err, sizeof err), -1);

	vision_params p;
	memset(&p, 0, sizeof p);
	snprintf(p.preset, sizeof p.preset, "custom");
	p.stage_count = 2;
	p.effect[0] = EFFECT_TINT;
	p.tint_u[0] = 90;
	p.tint_v[0] = 170;
	p.tint_strength[0] = 200;
	p.effect[1] = EFFECT_THRESHOLD;
	p.threshold_value[1] = 77;
	CHECK_EQ_INT(vision_effective_chain(&c, &p, &ch, err, sizeof err), 0);
	CHECK(ch.stage_count == 2 && ch.stages[0].tint_u == 90 && ch.stages[0].tint_strength == 200 &&
	      ch.stages[1].threshold_value == 77);
	snprintf(p.preset, sizeof p.preset, "nosuch");
	CHECK_EQ_INT(vision_effective_chain(&c, &p, &ch, err, sizeof err), -1);
	CHECK(strstr(err, "no preset nosuch") != NULL);
}

/* ---- The module under a manager -------------------------------------------------- */

static void copy_types(void)
{
	const char *src = getenv("HANGAR_MSG_SRC");
	char from[1024], to[1024];
	tfmt(from, sizeof from, "%s/vision_params.msg", src && *src ? src : "msg");
	tfmt(to, sizeof to, "%s/vision_params.msg", T_msg);
	if (copy_file(from, to) < 0) {
		fprintf(stderr, "cannot copy %s\n", from);
		exit(2);
	}
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

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint8_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint8_t)(rng >> 24);
}

/* A frame the operators have something to do with: gradients, a bright
 * square, a colour cast, and noise. */
static void make_frame(int k)
{
	struct hg_frame_header hd = { .width = W, .height = H, .fourcc = HG_I422 };
	memcpy(hd.stride, ftype.stride, sizeof hd.stride);
	memcpy(hd.offset, ftype.offset, sizeof hd.offset);
	memcpy(inbuf, &hd, sizeof hd);
	unsigned char *d = inbuf + sizeof hd;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			int v = (x + y + 7 * k) / 8 % 160 + 20 + (rnd() & 15);
			if (x > 400 + 10 * k && x < 600 + 10 * k && y > 200 && y < 400)
				v = 240;
			d[ftype.offset[0] + (size_t)y * W + x] = (uint8_t)v;
		}
	for (int p = 1; p <= 2; p++)
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W / 2; x++)
				d[ftype.offset[p] + (size_t)y * (W / 2) + x] =
					(uint8_t)((p == 1 ? 150 : 110) + (x * 7 + y) % 9 - 4);
}

/* What lib/vision makes of the frame in inbuf, into lf. */
static void local_chain(const EffectChainConfig *cfg)
{
	const unsigned char *d = inbuf + sizeof(struct hg_frame_header);
	for (int p = 0; p < 3; p++) {
		uint32_t pw = p ? W / 2 : W;
		for (uint32_t y = 0; y < H; y++)
			memcpy(lf->raw_planes[p] + (size_t)y * lf->stride[p], d + ftype.offset[p] + (size_t)y * pw, pw);
	}
	lf->capture_ns = stamp;
	ImgStats stats;
	memset(&stats, 0, sizeof stats);
	if (cfg->auto_visibility) {
		const uint8_t *raw[3] = { lf->raw_planes[0], lf->raw_planes[1], lf->raw_planes[2] };
		img_stats_compute(raw, W, H, lf->stride, &stats);
	}
	apply_effect_chain_auto(lf, cfg, lav, &stats);
}

/* The newest cam.pilot after seq `after`, waited for up to 5 s. */
static int next_out(uint64_t after, struct hg_info *info)
{
	uint64_t t0 = hg_now_ns();
	while (hg_now_ns() - t0 < 5000000000ull) {
		enum hg_result r = hg_latest(S_out, outbuf, ftype.type.payload_bytes, info);
		if (r == HG_MSG && info->seq > after)
			return 0;
		sleep_ms(1);
	}
	return -1;
}

static uint64_t out_seq;
static double lat_ms[FRAMES];

/* FRAMES frames through the module and through lib/vision: the bytes that
 * differ (0 when identical), the provenance checked; latencies into lat_ms. */
static long run_frames(const EffectChainConfig *cfg)
{
	long diff = 0;
	for (int k = 0; k < FRAMES; k++) {
		make_frame(k);
		stamp += 33333333u;
		uint64_t seq = hg_pub_next_seq(P_in), t0 = hg_now_ns();
		CHECK_EQ_INT(hg_publish(P_in, inbuf, ftype.type.payload_bytes, stamp), 0);
		struct hg_info info;
		if (next_out(out_seq, &info) < 0) {
			printf("    no cam.pilot for frame %d\n", k);
			return -1;
		}
		lat_ms[k] = (double)(hg_now_ns() - t0) / 1e6;
		out_seq = info.seq;
		CHECK(info.n_inputs >= 1 && info.inputs[0].topic_id == hg_topic_id("cam.decoded") &&
		      info.inputs[0].seq == seq);
		CHECK(info.stamp_ns == stamp);
		local_chain(cfg);
		const unsigned char *o = outbuf + sizeof(struct hg_frame_header);
		for (int p = 0; p < 3; p++) {
			uint32_t pw = p ? W / 2 : W;
			for (uint32_t y = 0; y < H; y++) {
				const uint8_t *a = o + ftype.offset[p] + (size_t)y * pw;
				const uint8_t *b = lf->planes[p] + (size_t)y * lf->stride[p];
				for (uint32_t x = 0; x < pw; x++) {
					uint8_t want = p && lf->chroma_neutral ? 128 : b[x];
					diff += a[x] != want;
				}
			}
		}
	}
	return diff;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static double median_ms(void)
{
	double s[FRAMES];
	memcpy(s, lat_ms, sizeof s);
	qsort(s, FRAMES, sizeof s[0], cmp_d);
	return s[FRAMES / 2];
}

static int param_set(char *out, size_t len, const char *kv)
{
	return run_cli(out, len, 5000, "param", "set", "vision", kv, (char *)NULL);
}

static void test_start(void)
{
	tfmt(conf_path, sizeof conf_path, "%s/vision.conf", T_root);
	write_file(conf_path, "[Vision]\nInput=cam.decoded\nOutput=cam.pilot\n");
	manifest("vision", "[Module]\nExec=%s %s\n[Subscribe cam.decoded]\n"
			   "[Publish cam.pilot]\nType=frame\nSlots=4\nInputs=cam.decoded\n"
			   "[Params]\nType=vision_params\nAllow=admin tester\n"
			   "[Restart]\nBackoff=0.1s..0.2s\n", T_vision, conf_path);
	manifest("tester", "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
			   "[Publish cam.decoded]\nType=frame\nSlots=4\n[Subscribe cam.pilot]\n");
	mgr = manager_start();
	CHECK(wait_state("tester", "stopped", 5000));
	CHECK(wait_state("vision", "ready", 5000));
	become("tester");
	CHECK_EQ_INT(hg_frame_type(W, H, HG_I422, &ftype), 0);
	ftype.type.name = "frame";
	struct hg_pub_opts o = { .slots = 4 };
	P_in = hg_pub_open("cam.decoded", &ftype.type, &o);
	S_out = hg_sub_open("cam.pilot", NULL);
	inbuf = calloc(1, ftype.type.payload_bytes);
	outbuf = calloc(1, ftype.type.payload_bytes);
	lf = vf_create(W, H, 0);
	lav = auto_vis_create();
	CHECK(P_in && S_out && inbuf && outbuf && lf && lav);
}

/* Each preset, set as the Fly view sets it (one value), then a custom chain:
 * every frame identical to lib/vision's. */
static void test_presets_through_the_module(void)
{
	struct vision_conf c;
	vision_conf_defaults(&c);
	const char *order[] = { "normal", "edges", "sharpen", "auto", "lowlight" };
	char out[4096];
	for (int i = 0; i < 5; i++) {
		char kv[64];
		tfmt(kv, sizeof kv, "preset=%s", order[i]);
		if (i) /* normal is the default: the first frames run before any set_params */
			CHECK_EQ_INT(param_set(out, sizeof out, kv), 0);
		vision_params p;
		memset(&p, 0, sizeof p);
		snprintf(p.preset, sizeof p.preset, "%s", order[i]);
		EffectChainConfig cfg;
		char err[160];
		CHECK_EQ_INT(vision_effective_chain(&c, &p, &cfg, err, sizeof err), 0);
		long diff = run_frames(&cfg);
		printf("    %-9s %d frames, %ld bytes differ, %.2f ms median from publish to cam.pilot\n", order[i],
		       FRAMES, diff, median_ms());
		CHECK_EQ_INT(diff, 0);
	}
	/* A custom chain: blur 3 passes, then Sobel at 40, given stage by stage. */
	CHECK_EQ_INT(param_set(out, sizeof out, "preset=custom"), 0);
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "set", "vision", "stage_count=2", "effect=6,5,0,0,0,0,0,0",
			     "blur_strength=3,1,1,1,1,1,1,1", "sobel_threshold=0,40,0,0,0,0,0,0", (char *)NULL), 0);
	EffectChainConfig cfg;
	char err[160];
	CHECK_EQ_INT(vision_chain_parse("blur:strength=3 sobel:threshold=40", &cfg, err, sizeof err), 0);
	long diff = run_frames(&cfg);
	printf("    custom    %d frames, %ld bytes differ\n", FRAMES, diff);
	CHECK_EQ_INT(diff, 0);
	/* The module's own measure, once its status line has caught up. */
	sleep_ms(1200);
	char status[256] = "";
	uint64_t inst, ready, alive;
	CHECK_EQ_INT(page_read("vision", &inst, &ready, &alive, status), 0);
	printf("    status: %s\n", status);
	CHECK(strstr(status, "custom (blur+sobel)") != NULL);
}

static void test_refusals(void)
{
	char out[4096];
	CHECK(param_set(out, sizeof out, "preset=nosuch") != 0);
	CHECK(strstr(out, "no preset nosuch") != NULL);
	if (!strstr(out, "no preset nosuch"))
		printf("    %s\n", out);
	CHECK(param_set(out, sizeof out, "effect=11,0,0,0,0,0,0,0") != 0);
	CHECK(param_set(out, sizeof out, "stage_count=9") != 0);
	/* Nothing changed: still the custom chain. */
	CHECK_EQ_INT(run_cli(out, sizeof out, 5000, "param", "get", "vision", (char *)NULL), 0);
	CHECK(strstr(out, "custom") != NULL);
}

static void test_threads_restart(void)
{
	char out[4096];
	struct mstat m;
	CHECK(module_status("vision", &m) && m.pid > 0);
	CHECK_EQ_INT(param_set(out, sizeof out, "threads=2"), 0);
	CHECK(wait_new_pid("vision", m.pid, "ready", 8000));
	char status[256] = "";
	uint64_t inst, ready, alive;
	/* The status line says the thread count once a second. */
	uint64_t t0 = hg_now_ns();
	while (hg_now_ns() - t0 < 3000000000ull &&
	       (page_read("vision", &inst, &ready, &alive, status) < 0 || !strstr(status, " 2 threads")))
		sleep_ms(100);
	CHECK(strstr(status, " 2 threads") != NULL);
	if (!strstr(status, " 2 threads"))
		printf("    status: %s\n", status);
	/* A fresh subscription returns the newest message once (lib/hg NOTES,
	 * deviation 2): the new instance made a picture of the last frame from
	 * before its restart. That one is taken here, not by run_frames. */
	struct hg_info info;
	CHECK_EQ_INT(next_out(out_seq, &info), 0);
	out_seq = info.seq;
	/* The restarted module still makes lib/vision's picture (custom chain, saved). */
	EffectChainConfig cfg;
	char err[160];
	CHECK_EQ_INT(vision_chain_parse("blur:strength=3 sobel:threshold=40", &cfg, err, sizeof err), 0);
	long diff = run_frames(&cfg);
	CHECK_EQ_INT(diff, 0);
	printf("    after the restart, 2 threads: %ld bytes differ, %.2f ms median\n", diff, median_ms());
}

static void test_idle_cpu(void)
{
	struct mstat m;
	CHECK(module_status("vision", &m) && m.pid > 0);
	sleep_ms(300);
	long long t0 = th_cpu_ticks(m.pid);
	uint64_t a = hg_now_ns();
	sleep_ms(1000);
	double pct = th_cpu_percent(t0, th_cpu_ticks(m.pid), (long)((hg_now_ns() - a) / 1000000u));
	printf("    vision at rest: %.1f %% of a core\n", pct);
	CHECK(pct >= 0 && pct < 2);
}

static void test_shutdown(void)
{
	hg_pub_close(P_in);
	CHECK_EQ_INT(manager_stop(mgr), 0);
}

int main(void)
{
	T_vision = getenv("VISION_BIN");
	if (!T_vision) {
		fprintf(stderr, "VISION_BIN must be set\n");
		return 2;
	}
	RUN_TEST(test_presets);
	hd_setup("vision", NULL);
	copy_types();
	RUN_TEST(test_start);
	RUN_TEST(test_presets_through_the_module);
	RUN_TEST(test_refusals);
	RUN_TEST(test_threads_restart);
	RUN_TEST(test_idle_cpu);
	RUN_TEST(test_shutdown);
	hd_teardown();
	TEST_MAIN_END();
}
