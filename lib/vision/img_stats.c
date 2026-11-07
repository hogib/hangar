#include "img_stats.h"
#include "hg_pool.h"
#include "vision_pool.h"
#include <math.h>
#include <string.h>

// Chroma is only trusted between these luma levels (see ImgStats.cast_u).
enum { chroma_min_luma = 24, chroma_max_luma = 235 };

// One band's share of the sampling (what each thread of the old `#pragma
// omp parallel` region counted before the critical section merged it).
// Cache-line aligned so no two bands' counters share a line: measured at
// 1280x720 on 4 threads, the unaligned array cost 40% on this operator.
typedef struct {
  uint32_t hist[256];
  int64_t su, sv, sy;
  uint32_t nc;
} __attribute__((aligned(64))) StatsPartial;

typedef struct {
  const uint8_t *const *planes;
  const size_t *stride;
  uint32_t width;
  StatsPartial *partial; // one per band
} StatsCtx;

// Per-band partials rather than a lock: the merge then happens in band
// order on the calling thread, and no thread waits on another. 64 KB of
// zero pages until a band touches its slot; img_stats_compute is called
// from one thread (eeye's effects thread, the vision module), like the
// rest of the operators.
static StatsPartial band_partials[HG_POOL_MAX_THREADS];

// Sampled rows r in [begin, end): luma row 2r. I422: chroma is half width,
// full height, so luma column x pairs with chroma column x/2. Even x only,
// so every sample has its own chroma.
static void stats_rows(void *arg, size_t begin, size_t end, unsigned band) {
  const StatsCtx *c = arg;
  StatsPartial *p = &c->partial[band];
  const uint32_t width = c->width;
  // Counted in locals, as the OpenMP body counted them, and copied to the
  // band's slot once at the end: a histogram the compiler knows nothing
  // else can alias, and sums that stay in registers.
  uint32_t hist[256];
  int64_t su = 0, sv = 0, sy = 0;
  uint32_t nc = 0;
  memset(hist, 0, sizeof hist);
  for (size_t r = begin; r < end; r++) {
    size_t y = 2 * r;
    const uint8_t *yr = c->planes[0] + y * c->stride[0];
    const uint8_t *ur = c->planes[1] + y * c->stride[1];
    const uint8_t *vr = c->planes[2] + y * c->stride[2];
    for (uint32_t x = 0, cc = 0; x < width; x += 2, cc++) {
      uint32_t l = yr[x];
      hist[l]++;
      if (l >= chroma_min_luma && l <= chroma_max_luma) {
        su += (int)ur[cc] - 128;
        sv += (int)vr[cc] - 128;
        sy += l;
        nc++;
      }
    }
  }
  memcpy(p->hist, hist, sizeof hist);
  p->su = su;
  p->sv = sv;
  p->sy = sy;
  p->nc = nc;
}

void img_stats_compute(const uint8_t *const planes[3], uint32_t width,
                       uint32_t height, const size_t stride[3],
                       ImgStats *out) {
  memset(out, 0, sizeof *out);
  int64_t sum_u = 0, sum_v = 0, sum_y = 0;
  uint32_t n_chroma = 0;
  const uint32_t rows = (height + 1) / 2;
  // Each band counts into its own histogram and sums; merged below, in
  // band order. Integer counts and sums, so the result does not depend on
  // the split or the order: it is the serial loop's total exactly.
  struct hg_pool *pool = vision_pool();
  const unsigned bands = hg_pool_threads(pool);
  StatsCtx ctx = {planes, stride, width, band_partials};
  hg_pool_run(pool, stats_rows, &ctx, rows);
  for (unsigned b = 0; b < bands; b++) {
    const StatsPartial *p = &band_partials[b];
    for (int v = 0; v < 256; v++)
      out->hist[v] += p->hist[v];
    sum_u += p->su;
    sum_v += p->sv;
    sum_y += p->sy;
    n_chroma += p->nc;
  }
  out->samples = ((height + 1) / 2) * ((width + 1) / 2);
  out->chroma_samples = n_chroma;
  if (n_chroma) {
    out->cast_u = (float)sum_u / (float)n_chroma;
    out->cast_v = (float)sum_v / (float)n_chroma;
    out->mean_luma = (float)sum_y / (float)n_chroma;
  }
}

uint8_t img_stats_percentile(const ImgStats *s, double fraction) {
  if (s->samples == 0)
    return 0;
  double want = fraction * (double)s->samples;
  uint64_t cum = 0;
  for (int v = 0; v < 256; v++) {
    cum += s->hist[v];
    if ((double)cum >= want)
      return (uint8_t)v;
  }
  return 255;
}

static int8_t clamp_i8(float v) {
  long r = lroundf(v);
  return (int8_t)(r < -127 ? -127 : r > 127 ? 127 : r);
}

void img_stats_summarize(const ImgStats *s, FrameStats *out) {
  memset(out, 0, sizeof *out);
  if (s->samples == 0)
    return;
  out->valid = true;
  uint64_t dark = 0, clip = 0;
  for (int b = 0; b < 32; b++) {
    uint64_t sum = 0;
    for (int v = b * 8; v < b * 8 + 8; v++)
      sum += s->hist[v];
    out->hist32[b] = (uint16_t)((sum * 1000 + s->samples / 2) / s->samples);
  }
  for (int v = 0; v < img_dark_level; v++)
    dark += s->hist[v];
  for (int v = img_clip_level; v < 256; v++)
    clip += s->hist[v];
  out->dark_pm = (uint16_t)((dark * 1000 + s->samples / 2) / s->samples);
  out->clip_pm = (uint16_t)((clip * 1000 + s->samples / 2) / s->samples);
  out->p1 = img_stats_percentile(s, 0.01);
  out->p50 = img_stats_percentile(s, 0.50);
  out->p99 = img_stats_percentile(s, 0.99);
  out->cast_u = clamp_i8(s->cast_u);
  out->cast_v = clamp_i8(s->cast_v);
}
