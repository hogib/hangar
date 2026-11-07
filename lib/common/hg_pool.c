// SPDX-License-Identifier: LGPL-3.0-or-later
/* hg_pool: see hg_pool.h.
 *
 * Three words and a futex each, the way libgomp's own barrier works, rather
 * than a mutex and condition variables: a condvar broadcast wakes every
 * worker into a fight for the mutex, and with four participants and the
 * blur's sixty barriers a frame that serialisation was measurable (paced at
 * 30 frames/s, the visibility correction's median went from 0.8 ms under
 * libgomp to 1.0 ms on the condvar version; this one matches libgomp).
 * The words are plain integers touched only through the __atomic builtins
 * (lib/common/compat.h); the futex is just where a thread sleeps until a
 * word changes. ThreadSanitizer follows the atomics, so the suite runs
 * clean under it. */
#define _GNU_SOURCE
#include "hg_pool.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* futex(2), declared by hand: <linux/futex.h> clashes with musl's headers
 * (see lib/hg/hg_sys.h, which does the same). Private futexes: the words
 * are never shared between processes. */
#define POOL_FUTEX_WAIT_PRIVATE (0 | 128)
#define POOL_FUTEX_WAKE_PRIVATE (1 | 128)

static void futex_wait(uint32_t *word, uint32_t expected) {
  // Returns at once (EAGAIN) if *word != expected, on a wake, or on a
  // signal (EINTR); every caller re-checks the word in a loop.
  syscall(SYS_futex, word, POOL_FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
}

static void futex_wake(uint32_t *word, int how_many) {
  syscall(SYS_futex, word, POOL_FUTEX_WAKE_PRIVATE, how_many, NULL, NULL, 0);
}

struct hg_pool {
  unsigned n;         /* participants, the caller included */
  pthread_t *workers; /* n - 1 of them; worker i is participant i + 1 */
  unsigned started;   /* workers actually running (create may stop early) */

  /* The current run's body and argument: written by the caller before it
   * publishes run_seq, read by a worker after it sees the new run_seq. */
  hg_pool_team_fn fn;
  void *ctx;
  int stop; /* atomic: set before the last run_seq bump */

  /* Atomic words (compat.h). Each worker sleeps on run_seq until it changes,
   * the caller on busy until it reaches zero, everyone at a barrier on
   * bar_gen until it moves. */
  uint32_t run_seq;   /* bumped once per run */
  uint32_t busy;      /* workers still inside the current run */
  uint32_t bar_count; /* participants arrived at the open barrier */
  uint32_t bar_gen;   /* bumped when a barrier opens */
};

unsigned hg_pool_default_threads(void) {
  const char *env = getenv("OMP_NUM_THREADS");
  if (env && *env) {
    char *end;
    unsigned long v = strtoul(env, &end, 10);
    // libgomp reads the first value of a list and ignores a malformed
    // setting; a zero or negative one is malformed too.
    if (end != env && (*end == '\0' || *end == ',') && v >= 1)
      return v > HG_POOL_MAX_THREADS ? HG_POOL_MAX_THREADS : (unsigned)v;
  }
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  if (online < 1)
    return 1;
  return online > (long)HG_POOL_MAX_THREADS ? HG_POOL_MAX_THREADS
                                              : (unsigned)online;
}

struct worker_arg {
  struct hg_pool *pool;
  unsigned index;
};

static void *worker_main(void *arg) {
  struct worker_arg *wa = arg;
  struct hg_pool *p = wa->pool;
  const unsigned index = wa->index;
  free(wa);
  uint32_t seen = 0; // run_seq starts at 0: the first run is 1

  for (;;) {
    uint32_t seq;
    while ((seq = __atomic_load_n(&p->run_seq, __ATOMIC_ACQUIRE)) == seen)
      futex_wait(&p->run_seq, seen);
    seen = seq;
    if (__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE))
      break;

    // fn and ctx were stored before run_seq's release; the acquire above
    // orders them.
    p->fn(p->ctx, index, p->n);

    // The last worker out wakes the caller.
    if (__atomic_sub_fetch(&p->busy, 1, __ATOMIC_ACQ_REL) == 0)
      futex_wake(&p->busy, 1);
  }
  return NULL;
}

struct hg_pool *hg_pool_create(unsigned threads) {
  if (threads == 0)
    threads = 1;
  if (threads > HG_POOL_MAX_THREADS)
    threads = HG_POOL_MAX_THREADS;

  struct hg_pool *p = calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->n = threads;

  if (threads > 1) {
    p->workers = calloc(threads - 1, sizeof *p->workers);
    if (!p->workers) {
      free(p);
      errno = ENOMEM;
      return NULL;
    }
    for (unsigned i = 0; i + 1 < threads; i++) {
      int err;
      struct worker_arg *wa = malloc(sizeof *wa);
      if (!wa) {
        err = ENOMEM;
      } else {
        wa->pool = p;
        wa->index = i + 1;
        err = pthread_create(&p->workers[i], NULL, worker_main, wa);
        if (err != 0)
          free(wa);
      }
      if (err != 0) {
        hg_pool_destroy(p); // joins the ones that did start
        errno = err;
        return NULL;
      }
      p->started = i + 1;
#if defined(__linux__)
      // Shows in top/ps as what it is; failure is cosmetic.
      pthread_setname_np(p->workers[i], "hg_pool");
#endif
    }
  }
  return p;
}

void hg_pool_destroy(struct hg_pool *pool) {
  if (!pool)
    return;
  if (pool->started) {
    __atomic_store_n(&pool->stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&pool->run_seq, 1, __ATOMIC_RELEASE);
    futex_wake(&pool->run_seq, INT_MAX);
    for (unsigned i = 0; i < pool->started; i++)
      pthread_join(pool->workers[i], NULL);
  }
  free(pool->workers);
  free(pool);
}

unsigned hg_pool_threads(const struct hg_pool *pool) {
  return pool ? pool->n : 1;
}

void hg_pool_band(size_t n_items, unsigned bands, unsigned band,
                  size_t *begin, size_t *end) {
  if (bands == 0)
    bands = 1;
  if (band >= bands) {
    *begin = *end = n_items;
    return;
  }
  // gcc's expansion of a static schedule without a chunk size (omp-expand:
  // q = n / nthreads, tt = n % nthreads, threads below tt take one more),
  // and libomp's __kmp_for_static_init for kmp_sch_static: the same split.
  size_t q = n_items / bands, r = n_items % bands;
  size_t extra = band < r ? band : r;
  *begin = (size_t)band * q + extra;
  *end = *begin + q + (band < r ? 1u : 0u);
}

void hg_pool_team(struct hg_pool *pool, hg_pool_team_fn fn, void *ctx) {
  if (!pool || pool->n == 1) {
    fn(ctx, 0, 1);
    return;
  }
  struct hg_pool *p = pool;
  // No run is in progress (one at a time per pool), so every worker is
  // waiting on run_seq, and nobody reads fn/ctx/busy until it changes.
  p->fn = fn;
  p->ctx = ctx;
  __atomic_store_n(&p->busy, p->n - 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&p->run_seq, 1, __ATOMIC_RELEASE);
  futex_wake(&p->run_seq, INT_MAX);

  fn(ctx, 0, p->n);

  uint32_t left;
  while ((left = __atomic_load_n(&p->busy, __ATOMIC_ACQUIRE)) != 0)
    futex_wait(&p->busy, left);
}

void hg_pool_barrier(struct hg_pool *pool) {
  if (!pool || pool->n == 1)
    return;
  struct hg_pool *p = pool;
  // Sense-reversing: the generation read before arriving is the one to
  // wait out. The last to arrive resets the count (nobody else touches it
  // until they have seen the new generation) and opens the barrier.
  uint32_t gen = __atomic_load_n(&p->bar_gen, __ATOMIC_ACQUIRE);
  if (__atomic_add_fetch(&p->bar_count, 1, __ATOMIC_ACQ_REL) == p->n) {
    __atomic_store_n(&p->bar_count, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&p->bar_gen, 1, __ATOMIC_RELEASE);
    futex_wake(&p->bar_gen, INT_MAX);
  } else {
    while (__atomic_load_n(&p->bar_gen, __ATOMIC_ACQUIRE) == gen)
      futex_wait(&p->bar_gen, gen);
  }
}

struct run_ctx {
  hg_pool_fn fn;
  void *ctx;
  size_t n_items;
};

static void run_band(void *arg, unsigned index, unsigned count) {
  const struct run_ctx *r = arg;
  size_t begin, end;
  hg_pool_band(r->n_items, count, index, &begin, &end);
  r->fn(r->ctx, begin, end, index);
}

void hg_pool_run(struct hg_pool *pool, hg_pool_fn fn, void *ctx,
                 size_t n_items) {
  if (!pool || pool->n == 1) {
    fn(ctx, 0, n_items, 0);
    return;
  }
  struct run_ctx r = {fn, ctx, n_items};
  hg_pool_team(pool, run_band, &r);
}
