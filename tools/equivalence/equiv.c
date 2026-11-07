/*
 * equiv: hashes every lib/vision operator's output over a fixed,
 * deterministic input set and prints one line per case,
 *
 *   <case-name> <fnv1a64-hex>      or      <case-name> skip
 *
 * so that two builds can be compared byte for byte: hangar's lib/vision
 * against electric-eye v0.3.1 (tools/equivalence/hashes-v0.3.1.txt, made
 * once from v0.3.1's sources with this same program), x86-64 against the
 * aarch64 NEON paths under qemu, one thread against several. The first
 * lines spell out the case set, so a reference and a check can never
 * disagree silently about what was hashed. README.md beside this file says
 * how the reference was made and when it may be regenerated.
 *
 * Nothing here depends on the machine: the random pattern is xorshift64
 * from fixed seeds, the hash is FNV-1a 64 over the output's Y, U, V planes
 * in that order, and the thread count only comes in through OpenMP's own
 * OMP_NUM_THREADS -- which is the point: the hash must not change with it.
 *
 * Built against v0.3.1 with -DEQUIV_V031, where the chain still read its
 * stages from eeye's Config.
 */
#define _POSIX_C_SOURCE 200809L
#include "auto_vis.h"
#include "conv.h"
#include "effect_chain.h"
#include "img_stats.h"
#include "point_opps.h"
#include "video_frame.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef EQUIV_V031
// electric-eye v0.3.1: apply_effect_chain*() took the whole Config, whose
// stages / stage_count / auto_visibility are the three fields hangar's
// EffectChainConfig kept, under the same names.
typedef Config EffectChainConfig;
#endif

// --- The input set ---------------------------------------------------

static const struct {
  uint32_t w, h;
} sizes[] = {{1280, 720}, {640, 480}, {65, 48}, {79, 33}, {31, 9}, {2, 2}, {1, 1}};

// Degenerate inputs catch saturation and sign bugs that random content
// misses; non-multiple-of-16 sizes catch the SIMD remainder tails.
typedef enum { PAT_RANDOM, PAT_FLAT, PAT_MAX, PAT_CHECKER, PAT_RAMP, PAT_COUNT } Pattern;
static const char *const pattern_names[PAT_COUNT] = {"random", "flat", "max",
                                                     "checker", "ramp"};

static const uint8_t step32[] = {0, 32, 64, 96, 128, 160, 192, 224, 255};
static const uint8_t step16[] = {0,   16,  32,  48,  64,  80,  96,  112, 128,
                                 144, 160, 176, 192, 208, 224, 240, 255};
static const uint8_t uv64[] = {0, 64, 128, 192, 255};
static const uint8_t canny_lows[] = {0, 20, 40, 80, 120, 160, 200};
static const uint8_t canny_highs[] = {40, 60, 90, 130, 180, 255};
enum { blur_max_strength = 20, log_max_strength = 4, canny_max_strength = 4 };
enum { auto_vis_frames = 60 };
#define COUNT(a) (sizeof(a) / sizeof(a)[0])

static uint64_t xorshift64(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

// Fills three I422 planes with `pat`. `k` is the frame index of a
// sequence: 0 for the operator cases; for the visibility correction's
// sequences it moves the content frame to frame (fresh noise, a
// brightening flat field, a drifting checkerboard and ramp), so the
// smoothing has something to follow.
static void fill(uint8_t *const planes[3], uint32_t w, uint32_t h,
                 const size_t stride[3], Pattern pat, unsigned k) {
  uint64_t seed = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)w << 40) ^
                  ((uint64_t)h << 20) ^ ((uint64_t)(k + 1) * 0x100000001B3ULL);
  for (int p = 0; p < 3; p++) {
    uint32_t pw = p ? (w + 1) / 2 : w;
    for (uint32_t y = 0; y < h; y++) {
      uint8_t *row = planes[p] + (size_t)y * stride[p];
      for (uint32_t x = 0; x < pw; x++) {
        unsigned v = 0;
        switch (pat) {
        case PAT_RANDOM: v = (unsigned)(xorshift64(&seed) >> 56); break;
        case PAT_FLAT: v = p == 0 ? 64 + k : p == 1 ? 150 : 110; break;
        case PAT_MAX: v = 255; break;
        case PAT_CHECKER: {
          bool cell = (((x + k) / 4 + y / 4) & 1) != 0;
          v = p == 0 ? (cell ? 255 : 0) : p == 1 ? (cell ? 180 : 90) : (cell ? 70 : 160);
          break;
        }
        case PAT_RAMP: {
          unsigned rx = (x * 255u) / (pw > 1 ? pw - 1 : 1) + 3 * k;
          unsigned ry = (y * 255u) / (h > 1 ? h - 1 : 1);
          v = p == 0 ? rx : p == 1 ? ry : 255 - ry;
          break;
        }
        default: break;
        }
        row[x] = (uint8_t)v;
      }
    }
  }
}

// --- Hashing and the skip rule ----------------------------------------

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 0x100000001B3ULL;
  }
  return h;
}

static uint64_t hash_planes(const uint8_t *const planes[3],
                            const size_t sizes3[3]) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int p = 0; p < 3; p++)
    h = fnv1a(h, planes[p], sizes3[p]);
  return h;
}

static bool all_bytes(const uint8_t *p, size_t n, uint8_t v) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != v)
      return false;
  return true;
}

// Every output buffer (work and spare; raw is the input) is set to
// `poison` before a case runs, so an operator that leaves a plane
// unwritten shows up in the hash rather than passing on stale memory.
static void poison(VideoFrame *f, uint8_t v) {
  vf_restore_roles(f);
  size_t total = f->plane_sizes[0] + f->plane_sizes[1] + f->plane_sizes[2];
  memset(f->pixel_data, v, total);
  memset(f->spare_data, v, total);
}

// A case: runs one operator on f's raw planes and says where its output is.
typedef void (*CaseFn)(VideoFrame *f, const void *arg, const uint8_t *out[3]);

// An operator that refuses a size (canny below 3x3) returns without writing
// dst, so its luma is still the poison afterwards. A legitimate output can
// be all-poison too, in principle, so that is confirmed by running again
// under a different poison: only an operator that wrote nothing leaves
// both. An operator that did write gives the same bytes both times.
static void run_case(const char *name, VideoFrame *f, CaseFn fn,
                     const void *arg) {
  const uint8_t *out[3];
  poison(f, 0xA5);
  fn(f, arg, out);
  if (all_bytes(out[0], f->plane_sizes[0], 0xA5)) {
    poison(f, 0x5A);
    fn(f, arg, out);
    if (all_bytes(out[0], f->plane_sizes[0], 0x5A)) {
      printf("%s skip\n", name);
      return;
    }
  }
  printf("%s %016" PRIx64 "\n", name, hash_planes(out, f->plane_sizes));
}

// --- The operators ----------------------------------------------------

static void raw_of(const VideoFrame *f, const uint8_t *src[3]) {
  src[0] = f->raw_planes[0];
  src[1] = f->raw_planes[1];
  src[2] = f->raw_planes[2];
}

static void spare_of(const VideoFrame *f, const uint8_t *out[3]) {
  out[0] = f->spare_planes[0];
  out[1] = f->spare_planes[1];
  out[2] = f->spare_planes[2];
}

// The neighbourhood ops and contrast, called directly: raw in, spare out.
static void op_blur(VideoFrame *f, const void *arg, const uint8_t *out[3]) {
  const uint8_t *src[3];
  raw_of(f, src);
  gaussian_blur(src, f->spare_planes, f->width, f->height, f->stride,
                *(const uint8_t *)arg);
  spare_of(f, out);
}

static void op_blur_luma(VideoFrame *f, const void *arg, const uint8_t *out[3]) {
  const uint8_t *src[3];
  raw_of(f, src);
  gaussian_blur_luma(src, f->spare_planes, f->width, f->height, f->stride,
                     *(const uint8_t *)arg);
  spare_of(f, out);
}

static void op_sobel(VideoFrame *f, const void *arg, const uint8_t *out[3]) {
  const uint8_t *src[3];
  raw_of(f, src);
  sobel_edges(src, f->spare_planes, f->width, f->height, f->stride,
              *(const uint8_t *)arg);
  spare_of(f, out);
}

static void op_log(VideoFrame *f, const void *arg, const uint8_t *out[3]) {
  const uint8_t *src[3], *p = arg;
  raw_of(f, src);
  log_edges(src, f->spare_planes, f->width, f->height, f->stride, p[0], p[1]);
  spare_of(f, out);
}

static void op_canny(VideoFrame *f, const void *arg, const uint8_t *out[3]) {
  const uint8_t *src[3], *p = arg;
  raw_of(f, src);
  canny_edges(src, f->spare_planes, f->width, f->height, f->stride, p[0], p[1],
              p[2]);
  spare_of(f, out);
}

static void op_contrast(VideoFrame *f, const void *arg, const uint8_t *out[3]) {
  (void)arg;
  gs_contrast_normalize(f->raw_planes[0], f->spare_planes[0], f->width,
                        f->height, f->stride[0]);
  spare_of(f, out);
}

// The point ops have no entry point of their own: they exist as LUTs the
// chain composes, so a one-stage chain is how they are run -- and so the
// chain itself, which the hashes then cover too.
static void op_chain(VideoFrame *f, const void *arg, const uint8_t *out[3]) {
  apply_effect_chain(f, (const EffectChainConfig *)arg);
  out[0] = f->planes[0];
  out[1] = f->planes[1];
  out[2] = f->planes[2];
}

// The chain with auto visibility ahead of it, from a fresh AutoVis each
// time (its state is per frame sequence; the sequences are hashed below).
static void op_chain_auto(VideoFrame *f, const void *arg,
                          const uint8_t *out[3]) {
  const uint8_t *src[3];
  raw_of(f, src);
  ImgStats st;
  img_stats_compute(src, f->width, f->height, f->stride, &st);
  AutoVis *av = auto_vis_create();
  if (!av) {
    fprintf(stderr, "equiv: out of memory\n");
    exit(2);
  }
  apply_effect_chain_auto(f, (const EffectChainConfig *)arg, av, &st);
  auto_vis_destroy(av);
  out[0] = f->planes[0];
  out[1] = f->planes[1];
  out[2] = f->planes[2];
}

static EffectChainConfig one_stage(EffectStage s) {
  EffectChainConfig c;
  memset(&c, 0, sizeof c);
  c.stages[0] = s;
  c.stage_count = 1;
  return c;
}

// A compound literal for the one-stage calls below; the static table uses
// plain designated initializers, since a compound literal in a static
// initializer is a GNU extension.
#define ST(...) ((EffectStage){__VA_ARGS__})

// A few chains of more than one stage, for what only a chain exercises: LUT
// fusion across stages, the raw/work/spare ping-pong, dead-chroma skipping.
static const struct {
  const char *name;
  EffectStage stages[4];
  size_t n;
} chains[] = {
    {"empty", {{.effect = EFFECT_NONE}}, 0},
    {"blur2-sobel20",
     {{.effect = EFFECT_BLUR, .blur_strength = 2},
      {.effect = EFFECT_SOBEL, .sobel_threshold = 20}},
     2},
    {"blur1-canny1-40-90",
     {{.effect = EFFECT_BLUR, .blur_strength = 1},
      {.effect = EFFECT_CANNY, .canny_strength = 1, .canny_low = 40,
       .canny_high = 90}},
     2},
    {"tint-invert-threshold100",
     {{.effect = EFFECT_TINT, .tint_u = 90, .tint_v = 150, .tint_strength = 180},
      {.effect = EFFECT_INVERT},
      {.effect = EFFECT_THRESHOLD, .threshold_value = 100}},
     3},
    {"grayscale-log2-32",
     {{.effect = EFFECT_GRAYSCALE},
      {.effect = EFFECT_LOG, .log_strength = 2, .log_threshold = 32}},
     2},
    {"contrast-sobel-threshold128",
     {{.effect = EFFECT_CONTRAST},
      {.effect = EFFECT_SOBEL},
      {.effect = EFFECT_THRESHOLD, .threshold_value = 128}},
     3},
    {"light160-blur3-tint",
     {{.effect = EFFECT_LIGHT, .light_level = 160},
      {.effect = EFFECT_BLUR, .blur_strength = 3},
      {.effect = EFFECT_TINT, .tint_u = 100, .tint_v = 140, .tint_strength = 120}},
     3},
    {"invert-blur5-tint-grayscale",
     {{.effect = EFFECT_INVERT},
      {.effect = EFFECT_BLUR, .blur_strength = 5},
      {.effect = EFFECT_TINT, .tint_u = 200, .tint_v = 60, .tint_strength = 255},
      {.effect = EFFECT_GRAYSCALE}},
     4},
};

// --- One size, one pattern -------------------------------------------

static void run_operators(VideoFrame *f, const char *sz, const char *pat) {
  char name[128];

  for (unsigned s = 0; s <= blur_max_strength; s++) {
    uint8_t st = (uint8_t)s;
    snprintf(name, sizeof name, "blur/%s/%s/s%u", sz, pat, s);
    run_case(name, f, op_blur, &st);
    snprintf(name, sizeof name, "blur_luma/%s/%s/s%u", sz, pat, s);
    run_case(name, f, op_blur_luma, &st);
  }
  for (size_t i = 0; i < COUNT(step16); i++) {
    snprintf(name, sizeof name, "sobel/%s/%s/t%u", sz, pat, step16[i]);
    run_case(name, f, op_sobel, &step16[i]);
  }
  for (unsigned s = 0; s <= log_max_strength; s++)
    for (size_t i = 0; i < COUNT(step32); i++) {
      uint8_t p[2] = {(uint8_t)s, step32[i]};
      snprintf(name, sizeof name, "log/%s/%s/s%u_t%u", sz, pat, s, step32[i]);
      run_case(name, f, op_log, p);
    }
  for (unsigned s = 1; s <= canny_max_strength; s++)
    for (size_t l = 0; l < COUNT(canny_lows); l++)
      for (size_t hi = 0; hi < COUNT(canny_highs); hi++) {
        uint8_t p[3] = {(uint8_t)s, canny_lows[l], canny_highs[hi]};
        snprintf(name, sizeof name, "canny/%s/%s/s%u_l%u_h%u", sz, pat, s,
                 canny_lows[l], canny_highs[hi]);
        run_case(name, f, op_canny, p);
      }
  snprintf(name, sizeof name, "contrast/%s/%s", sz, pat);
  run_case(name, f, op_contrast, NULL);

  // Point ops, one stage each.
  EffectChainConfig c;
  c = one_stage(ST(.effect = EFFECT_NONE));
  snprintf(name, sizeof name, "none/%s/%s", sz, pat);
  run_case(name, f, op_chain, &c);
  c = one_stage(ST(.effect = EFFECT_GRAYSCALE));
  snprintf(name, sizeof name, "grayscale/%s/%s", sz, pat);
  run_case(name, f, op_chain, &c);
  c = one_stage(ST(.effect = EFFECT_INVERT));
  snprintf(name, sizeof name, "invert/%s/%s", sz, pat);
  run_case(name, f, op_chain, &c);
  c = one_stage(ST(.effect = EFFECT_CONTRAST));
  snprintf(name, sizeof name, "chain-contrast/%s/%s", sz, pat);
  run_case(name, f, op_chain, &c);
  for (size_t i = 0; i < COUNT(step32); i++) {
    c = one_stage(ST(.effect = EFFECT_THRESHOLD, .threshold_value = step32[i]));
    snprintf(name, sizeof name, "threshold/%s/%s/v%u", sz, pat, step32[i]);
    run_case(name, f, op_chain, &c);
  }
  for (size_t i = 0; i < COUNT(step32); i++) {
    c = one_stage(ST(.effect = EFFECT_LIGHT, .light_level = step32[i]));
    snprintf(name, sizeof name, "light/%s/%s/l%u", sz, pat, step32[i]);
    run_case(name, f, op_chain, &c);
  }
  for (size_t u = 0; u < COUNT(uv64); u++)
    for (size_t v = 0; v < COUNT(uv64); v++)
      for (size_t s = 0; s < COUNT(step32); s++) {
        c = one_stage(ST(.effect = EFFECT_TINT, .tint_u = uv64[u],
                         .tint_v = uv64[v], .tint_strength = step32[s]));
        snprintf(name, sizeof name, "tint/%s/%s/u%u_v%u_s%u", sz, pat, uv64[u],
                 uv64[v], step32[s]);
        run_case(name, f, op_chain, &c);
      }

  // Chains.
  for (size_t i = 0; i < COUNT(chains); i++) {
    memset(&c, 0, sizeof c);
    for (size_t k = 0; k < chains[i].n; k++)
      c.stages[k] = chains[i].stages[k];
    c.stage_count = chains[i].n;
    snprintf(name, sizeof name, "chain/%s/%s/%s", sz, pat, chains[i].name);
    run_case(name, f, op_chain, &c);
  }
  c = one_stage(ST(.effect = EFFECT_INVERT));
  c.auto_visibility = true;
  snprintf(name, sizeof name, "chain/%s/%s/auto-invert", sz, pat);
  run_case(name, f, op_chain_auto, &c);
}

// The visibility correction over a sequence: it smooths what it measures
// over ~0.4s of capture time, so a frame's output depends on the frames
// before it, and every frame of the sequence is hashed.
static void run_auto_vis(VideoFrame *f, const char *sz, Pattern pat) {
  AutoVis *av = auto_vis_create();
  if (!av) {
    fprintf(stderr, "equiv: out of memory\n");
    exit(2);
  }
  char name[128];
  for (unsigned k = 0; k < auto_vis_frames; k++) {
    fill(f->raw_planes, f->width, f->height, f->stride, pat, k);
    const uint8_t *src[3];
    raw_of(f, src);
    ImgStats st;
    img_stats_compute(src, f->width, f->height, f->stride, &st);
    poison(f, 0xA5);
    if (!auto_vis_apply(av, &st, src, f->planes, f->width, f->height,
                        f->stride, (uint64_t)(k + 1) * 33333333ULL)) {
      fprintf(stderr, "equiv: auto_vis_apply failed\n");
      exit(2);
    }
    const uint8_t *out[3] = {f->planes[0], f->planes[1], f->planes[2]};
    snprintf(name, sizeof name, "auto_vis/%s/%s/f%u", sz, pattern_names[pat], k);
    printf("%s %016" PRIx64 "\n", name, hash_planes(out, f->plane_sizes));
  }
  auto_vis_destroy(av);
}

int main(void) {
#ifdef _OPENMP
  fprintf(stderr, "equiv: OpenMP, %d thread(s)\n", omp_get_max_threads());
#else
  fprintf(stderr, "equiv: no OpenMP, 1 thread\n");
#endif

  // The case set, spelled out: a reference made from a different set
  // fails the diff on these lines before any hash is compared.
  printf("# equiv: lib/vision operator hashes, FNV-1a 64 over the output's Y, U, V planes\n");
  printf("# sizes:");
  for (size_t i = 0; i < COUNT(sizes); i++)
    printf(" %ux%u", sizes[i].w, sizes[i].h);
  printf("\n# patterns:");
  for (int p = 0; p < PAT_COUNT; p++)
    printf(" %s", pattern_names[p]);
  printf("\n# blur, blur_luma: strength 0..%u\n", blur_max_strength);
  printf("# sobel: threshold 0..240 step 16, and 255\n");
  printf("# log: strength 0..%u x threshold 0..224 step 32, and 255\n", log_max_strength);
  printf("# canny: strength 1..%u x low {0,20,40,80,120,160,200} x high {40,60,90,130,180,255}\n",
         canny_max_strength);
  printf("# contrast: direct; none grayscale invert chain-contrast: one-stage chains\n");
  printf("# threshold: value 0..224 step 32, and 255; light: level the same\n");
  printf("# tint: u, v in {0,64,128,192,255} x strength 0..224 step 32, and 255\n");
  printf("# chain: %u fixed chains (equiv.c), and auto-invert (auto visibility, fresh state)\n",
         (unsigned)COUNT(chains));
  printf("# auto_vis: %u-frame sequence per size and pattern, capture_ns = (frame+1) * 33333333\n",
         auto_vis_frames);
  printf("# output buffers poisoned 0xA5 first; skip = the operator refused the size\n");

  for (size_t i = 0; i < COUNT(sizes); i++) {
    VideoFrame *f = vf_create(sizes[i].w, sizes[i].h, 0);
    if (!f) {
      fprintf(stderr, "equiv: out of memory\n");
      return 2;
    }
    char sz[32];
    snprintf(sz, sizeof sz, "%ux%u", sizes[i].w, sizes[i].h);
    for (int p = 0; p < PAT_COUNT; p++) {
      fill(f->raw_planes, f->width, f->height, f->stride, (Pattern)p, 0);
      run_operators(f, sz, pattern_names[p]);
      run_auto_vis(f, sz, (Pattern)p);
    }
    vf_free(f);
  }
  return 0;
}
