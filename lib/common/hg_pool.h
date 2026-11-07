// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * hg_pool: a fixed pool of worker threads for the row-banded loops of
 * lib/vision, in place of OpenMP. A static musl build has no OpenMP
 * runtime (zig ships none), and without one every `#pragma omp` loop ran
 * single-threaded; this is the few hundred lines that get the operators
 * their threads back on every target, with the same partition OpenMP gave
 * them, so that the equivalence hashes (tools/equivalence/) do not move.
 *
 * Shape. A pool is created once with N participants: N - 1 worker threads
 * plus whichever thread calls into it. hg_pool_run() splits [0, n_items)
 * into N contiguous bands exactly as OpenMP's static schedule without a
 * chunk size does (hg_pool_band), runs band b on participant b -- band 0
 * on the caller -- and returns once every band is done. hg_pool_team()
 * is the `#pragma omp parallel` form: one call of fn per participant, with
 * hg_pool_barrier() available inside, for the operators whose passes
 * share scratch (the blur's rolling window, the visibility correction's
 * three passes). N == 1, or a NULL pool, runs everything inline on the
 * caller: no other thread is involved at all.
 *
 * Waiting. Workers sleep on a futex between runs, the caller sleeps until
 * the last band finishes, a barrier sleeps until the last participant
 * arrives; nothing spins. electric-eye had to re-exec itself with
 * OMP_WAIT_POLICY=passive to get libgomp to behave this way (its workers
 * otherwise spun for milliseconds after every region, ~178% CPU at 30
 * frames/s); here it is the only policy. Measured against libgomp under
 * that policy (tools/equivalence/bench.c, 1280x720, 4 threads, paced at
 * 30 frames/s) the per-frame times are the same within noise.
 *
 * Rules. Bands of one run must not write what another band reads (the
 * operators are written that way: see conv.c's note on the raw/work
 * split). A body must not call back into the same pool. In a team, every
 * participant must reach every barrier, the same number of times. One run
 * at a time per pool: the operators are called from one thread, and the
 * pool does not make them re-entrant.
 */
#ifndef HG_POOL_H
#define HG_POOL_H

#include <stddef.h>

/* Participants per pool, at most. hg_pool_create() and
 * hg_pool_default_threads() clamp to it, so callers may size per-band
 * partials (a histogram per band, say) statically. */
#define HG_POOL_MAX_THREADS 64u

struct hg_pool;

/* The thread count OpenMP would have used (omp_get_max_threads()):
 * OMP_NUM_THREADS when it is set to a positive integer (the first value of
 * a comma-separated list, as libgomp reads it), else the number of CPUs
 * online. At least 1, at most HG_POOL_MAX_THREADS. */
unsigned hg_pool_default_threads(void);

/* Creates a pool of `threads` participants: threads - 1 workers, plus the
 * thread that later calls hg_pool_run(). 0 counts as 1. NULL with errno set
 * when a thread could not be created or memory ran out; the caller may
 * then go on with a NULL pool, which runs everything inline. */
struct hg_pool *hg_pool_create(unsigned threads);

/* Stops and joins the workers. No run may be in progress. NULL is fine. */
void hg_pool_destroy(struct hg_pool *pool);

/* Participants, caller included: 1 for a NULL pool. */
unsigned hg_pool_threads(const struct hg_pool *pool);

/* The band OpenMP's static schedule (no chunk size) gives thread `band` of
 * `bands` over `n_items` iterations: with q = n_items / bands and r =
 * n_items % bands, the first r bands hold q + 1 items and the rest q, in
 * order, so band b is [b*q + min(b, r), (b+1)*q + min(b+1, r)). This is
 * what both libgomp (gcc) and libomp (clang) compute; a band past the end
 * is empty (begin == end). */
void hg_pool_band(size_t n_items, unsigned bands, unsigned band,
                  size_t *begin, size_t *end);

/* The body of hg_pool_run(): items [begin, end) of band `band`, which runs
 * on participant `band` (0: the calling thread). Called once per band,
 * empty bands included, so a body may index per-band partials by `band`
 * and the caller combine them in band order afterwards. */
typedef void (*hg_pool_fn)(void *ctx, size_t begin, size_t end, unsigned band);

/* `#pragma omp parallel for` over [0, n_items): fn on every band at once,
 * then returns. The partition is hg_pool_band(n_items, hg_pool_threads(pool), b). */
void hg_pool_run(struct hg_pool *pool, hg_pool_fn fn, void *ctx, size_t n_items);

/* The body of hg_pool_team(): participant `index` of `count`. */
typedef void (*hg_pool_team_fn)(void *ctx, unsigned index, unsigned count);

/* `#pragma omp parallel`: fn once on every participant at the same time
 * (index 0 is the caller), returning when all have finished. Inside, an
 * `omp for` is hg_pool_band(n, count, index, ...) followed by
 * hg_pool_barrier(); the return itself is the region's closing barrier. */
void hg_pool_team(struct hg_pool *pool, hg_pool_team_fn fn, void *ctx);

/* `#pragma omp barrier`, inside hg_pool_team()'s fn only: returns once every
 * participant has arrived. A no-op for a NULL pool or one participant. */
void hg_pool_barrier(struct hg_pool *pool);

#endif
