/*
 * bench: how long two of lib/vision's operators take per 1280x720 frame on
 * this machine, at the thread count OMP_NUM_THREADS names (else every CPU):
 *
 *   visibility  img_stats_compute + auto_vis_apply, what eeye's effects
 *               thread runs per frame with auto visibility on
 *   auto_vis    auto_vis_apply alone
 *   img_stats   img_stats_compute alone
 *   blur4       gaussian_blur, strength 4, all three planes
 *
 * Each is run `-n N` times (default 300) after 20 warm-up frames and the
 * median and mean are printed in ms. `-p` paces the frames at 30 per
 * second, which is how the pipeline runs them: the pool's workers (or
 * libgomp's, under OMP_WAIT_POLICY=passive) are asleep when a frame
 * arrives, so this includes their wake-up. Random content (xorshift64,
 * fixed seed) over a fresh AutoVis state each run, so every frame does the
 * same work.
 *
 * -DBENCH_OPENMP builds it against electric-eye's OpenMP sources (no pool
 * to create: OpenMP reads OMP_NUM_THREADS itself), for the before/after
 * figures in the port's report.
 */
#define _POSIX_C_SOURCE 200809L
#include "auto_vis.h"
#include "conv.h"
#include "img_stats.h"
#include "video_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef BENCH_OPENMP
#include "hg_pool.h"
#include "vision_pool.h"
#else
#ifdef _OPENMP
#include <omp.h>
#endif
#endif

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void sleep_ms(double ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1e3);
  ts.tv_nsec = (long)((ms - ts.tv_sec * 1e3) * 1e6);
  nanosleep(&ts, NULL);
}

static void fill_random(VideoFrame *f) {
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  for (int p = 0; p < 3; p++) {
    uint8_t *plane = f->raw_planes[p];
    for (size_t i = 0; i < f->plane_sizes[p]; i++) {
      s ^= s << 13;
      s ^= s >> 7;
      s ^= s << 17;
      plane[i] = (uint8_t)(s >> 56);
    }
  }
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

typedef void (*Op)(VideoFrame *f, AutoVis *av, ImgStats *st, unsigned k);

static void op_visibility(VideoFrame *f, AutoVis *av, ImgStats *st, unsigned k) {
  const uint8_t *src[3] = {f->raw_planes[0], f->raw_planes[1], f->raw_planes[2]};
  img_stats_compute(src, f->width, f->height, f->stride, st);
  auto_vis_apply(av, st, src, f->planes, f->width, f->height, f->stride,
                 (uint64_t)(k + 1) * 33333333ULL);
}

static void op_auto_vis(VideoFrame *f, AutoVis *av, ImgStats *st, unsigned k) {
  const uint8_t *src[3] = {f->raw_planes[0], f->raw_planes[1], f->raw_planes[2]};
  auto_vis_apply(av, st, src, f->planes, f->width, f->height, f->stride,
                 (uint64_t)(k + 1) * 33333333ULL);
}

static void op_img_stats(VideoFrame *f, AutoVis *av, ImgStats *st, unsigned k) {
  (void)av;
  (void)k;
  const uint8_t *src[3] = {f->raw_planes[0], f->raw_planes[1], f->raw_planes[2]};
  img_stats_compute(src, f->width, f->height, f->stride, st);
}

static void op_blur4(VideoFrame *f, AutoVis *av, ImgStats *st, unsigned k) {
  (void)av;
  (void)st;
  (void)k;
  const uint8_t *src[3] = {f->raw_planes[0], f->raw_planes[1], f->raw_planes[2]};
  gaussian_blur(src, f->spare_planes, f->width, f->height, f->stride, 4);
}

static void bench(const char *name, Op op, VideoFrame *f, unsigned runs,
                  double pace_ms) {
  AutoVis *av = auto_vis_create();
  ImgStats st;
  double *t = malloc(runs * sizeof *t);
  if (!av || !t) {
    fprintf(stderr, "bench: out of memory\n");
    exit(2);
  }
  // The stats the auto_vis-only case steers by.
  const uint8_t *src[3] = {f->raw_planes[0], f->raw_planes[1], f->raw_planes[2]};
  img_stats_compute(src, f->width, f->height, f->stride, &st);
  for (unsigned k = 0; k < 20; k++) {
    if (pace_ms > 0)
      sleep_ms(pace_ms);
    op(f, av, &st, k);
  }
  double sum = 0;
  for (unsigned k = 0; k < runs; k++) {
    if (pace_ms > 0)
      sleep_ms(pace_ms);
    double t0 = now_ms();
    op(f, av, &st, 20 + k);
    t[k] = now_ms() - t0;
    sum += t[k];
  }
  qsort(t, runs, sizeof *t, cmp_double);
  printf("%-11s median %7.3f ms  mean %7.3f ms  min %7.3f ms  max %7.3f ms\n",
         name, t[runs / 2], sum / runs, t[0], t[runs - 1]);
  free(t);
  auto_vis_destroy(av);
}

int main(int argc, char **argv) {
  unsigned runs = 300;
  double pace_ms = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
      runs = (unsigned)atoi(argv[++i]);
    else if (strcmp(argv[i], "-p") == 0)
      pace_ms = 1000.0 / 30;
    else {
      fprintf(stderr, "usage: bench [-n runs] [-p]\n");
      return 2;
    }
  }
  if (runs == 0)
    runs = 1;

#ifndef BENCH_OPENMP
  struct hg_pool *pool = hg_pool_create(hg_pool_default_threads());
  if (!pool) {
    perror("bench: hg_pool_create");
    return 2;
  }
  vision_set_pool(pool);
  printf("bench: pool, %u thread(s), 1280x720, %u runs%s\n",
         hg_pool_threads(pool), runs, pace_ms > 0 ? ", paced at 30/s" : "");
#elif defined(_OPENMP)
  printf("bench: OpenMP, %d thread(s), 1280x720, %u runs%s\n",
         omp_get_max_threads(), runs, pace_ms > 0 ? ", paced at 30/s" : "");
#else
  printf("bench: no OpenMP, 1 thread, 1280x720, %u runs%s\n", runs,
         pace_ms > 0 ? ", paced at 30/s" : "");
#endif

  VideoFrame *f = vf_create(1280, 720, 0);
  if (!f) {
    fprintf(stderr, "bench: out of memory\n");
    return 2;
  }
  fill_random(f);
  bench("visibility", op_visibility, f, runs, pace_ms);
  bench("auto_vis", op_auto_vis, f, runs, pace_ms);
  bench("img_stats", op_img_stats, f, runs, pace_ms);
  bench("blur4", op_blur4, f, runs, pace_ms);
  vf_free(f);
#ifndef BENCH_OPENMP
  vision_set_pool(NULL);
  hg_pool_destroy(pool);
#endif
  return 0;
}
