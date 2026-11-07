// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * hg_pool's suite. Three things matter: the partition is OpenMP's (so a
 * row-banded operator sees the same bands it saw under libgomp), no run
 * loses or repeats a band over many thousands of runs, and one participant
 * means the calling thread does everything itself. Built with -fopenmp the
 * partition is also checked against a live `#pragma omp parallel for
 * schedule(static)` loop, which is the actual oracle; without it, against
 * the rule both runtimes document. Meant to run under ThreadSanitizer as
 * well as plain (make check-tsan-pool).
 */
#include "hg_pool.h"
#include "test_harness.h"
#include <pthread.h>
#include <stdlib.h>
#ifdef _OPENMP
#include <omp.h>
#endif

// --- The partition ---------------------------------------------------------

// The rule as gcc expands a static schedule without a chunk size and as
// libomp's __kmp_for_static_init computes it: q = n / nt, r = n % nt, the
// first r threads take q + 1 items, in order.
static void reference_band(size_t n, unsigned nt, unsigned t, size_t *b,
                           size_t *e) {
  size_t q = n / nt, r = n % nt;
  *b = t * q + (t < r ? t : r);
  *e = *b + q + (t < r ? 1 : 0);
}

static void test_band_matches_rule(void) {
  const size_t ns[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 48,
                       64, 100, 479, 480, 481, 718, 719, 720, 1079, 1080,
                       1081, 2160, 921600};
  for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++)
    for (unsigned nt = 1; nt <= 64; nt++) {
      size_t covered = 0, prev_end = 0;
      for (unsigned t = 0; t < nt; t++) {
        size_t b, e, rb, re;
        hg_pool_band(ns[i], nt, t, &b, &e);
        reference_band(ns[i], nt, t, &rb, &re);
        CHECK(b == rb && e == re);
        CHECK(b == prev_end); // contiguous, in order, no gaps
        CHECK(e >= b);
        prev_end = e;
        covered += e - b;
      }
      CHECK(covered == ns[i]); // every item exactly once
      // The first n % nt bands are the longer ones.
      if (nt > 1 && ns[i] >= nt) {
        size_t b0, e0, bl, el;
        hg_pool_band(ns[i], nt, 0, &b0, &e0);
        hg_pool_band(ns[i], nt, nt - 1, &bl, &el);
        CHECK(e0 - b0 >= el - bl);
        CHECK((e0 - b0) - (el - bl) <= 1);
      }
    }
  // A band index past the count is empty, at the end.
  size_t b, e;
  hg_pool_band(10, 4, 4, &b, &e);
  CHECK(b == 10 && e == 10);
  hg_pool_band(10, 0, 0, &b, &e); // 0 bands counts as 1
  CHECK(b == 0 && e == 10);
}

#ifdef _OPENMP
// The live oracle: which thread OpenMP's static schedule gives each
// iteration, compared with hg_pool_band for the same team size.
static void test_band_matches_openmp(void) {
  const size_t ns[] = {0, 1, 2, 3, 5, 7, 16, 17, 33, 48, 479, 720, 1081};
  const int team_sizes[] = {1, 2, 3, 4, 5, 7, 8, 12};
  for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
    size_t n = ns[i];
    int *owner = malloc((n + 1) * sizeof *owner);
    CHECK(owner != NULL);
    if (!owner)
      return;
    for (size_t k = 0; k < sizeof team_sizes / sizeof team_sizes[0]; k++) {
      int nt = team_sizes[k];
      int got_nt = 0;
      for (size_t j = 0; j < n; j++)
        owner[j] = -1;
#pragma omp parallel num_threads(nt)
      {
#pragma omp single
        got_nt = omp_get_num_threads();
#pragma omp for schedule(static)
        for (size_t j = 0; j < n; j++)
          owner[j] = omp_get_thread_num();
      }
      // The runtime may hand out fewer threads than asked; compare against
      // what it actually used.
      for (int t = 0; t < got_nt; t++) {
        size_t b, e;
        hg_pool_band(n, (unsigned)got_nt, (unsigned)t, &b, &e);
        for (size_t j = b; j < e; j++)
          CHECK(owner[j] == t);
      }
      for (size_t j = 0; j < n; j++)
        CHECK(owner[j] >= 0);
    }
    free(owner);
  }
}
#endif

// --- Runs ------------------------------------------------------------------

typedef struct {
  unsigned char *seen; // per item: how many times a body covered it
  size_t n;
  pthread_t caller;
  int band0_on_caller;    // band 0 ran on the calling thread
  int other_on_caller;    // some other band ran on the calling thread
  unsigned bands_run;     // atomically counted
  unsigned bands_expected;
} RunCheck;

static void mark_body(void *arg, size_t begin, size_t end, unsigned band) {
  RunCheck *rc = arg;
  for (size_t i = begin; i < end; i++)
    rc->seen[i]++;
  // Bands are disjoint, so seen[] needs no atomics; the counters below are
  // shared.
  int here = pthread_equal(pthread_self(), rc->caller) != 0;
  if (band == 0)
    __atomic_store_n(&rc->band0_on_caller, here, __ATOMIC_RELAXED);
  else if (here)
    __atomic_store_n(&rc->other_on_caller, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&rc->bands_run, 1, __ATOMIC_RELAXED);
}

// Every run covers every item exactly once and every band is called; over
// many runs with changing sizes nothing is lost or doubled.
static void run_many(unsigned threads, unsigned runs) {
  struct hg_pool *pool = hg_pool_create(threads);
  CHECK(pool != NULL);
  if (!pool)
    return;
  CHECK_EQ_INT(hg_pool_threads(pool), threads == 0 ? 1 : threads);
  enum { max_n = 4097 };
  unsigned char *seen = calloc(max_n, 1);
  CHECK(seen != NULL);
  RunCheck rc;
  rc.seen = seen;
  rc.caller = pthread_self();
  unsigned bad_runs = 0;
  for (unsigned r = 0; r < runs; r++) {
    // Sizes around the band count and well past it, including 0 and 1.
    size_t n = (r % 7 == 0) ? (r / 7) % 9 : (size_t)((r * 2654435761u) % max_n);
    rc.n = n;
    rc.band0_on_caller = 0;
    rc.other_on_caller = 0;
    rc.bands_run = 0;
    rc.bands_expected = hg_pool_threads(pool);
    hg_pool_run(pool, mark_body, &rc, n);
    int ok = 1;
    for (size_t i = 0; i < n; i++)
      if (seen[i] != 1)
        ok = 0;
    if (n < max_n && seen[n] != 0)
      ok = 0;
    if (__atomic_load_n(&rc.bands_run, __ATOMIC_RELAXED) != rc.bands_expected)
      ok = 0;
    if (!rc.band0_on_caller || rc.other_on_caller)
      ok = 0;
    if (!ok)
      bad_runs++;
    for (size_t i = 0; i < n; i++)
      seen[i] = 0;
  }
  CHECK_EQ_INT(bad_runs, 0);
  free(seen);
  hg_pool_destroy(pool);
}

static void test_runs_lose_nothing_4(void) { run_many(4, 10000); }
static void test_runs_lose_nothing_3(void) { run_many(3, 10000); }
static void test_runs_lose_nothing_12(void) { run_many(12, 2000); }

// N == 1: the body runs inline, on the caller, as one band, with no pool
// thread involved; a NULL pool behaves the same.
static void test_one_thread_runs_inline(void) {
  run_many(1, 1000);
  struct hg_pool *pool = hg_pool_create(1);
  CHECK(pool != NULL);
  unsigned char seen[10] = {0};
  RunCheck rc = {seen, 10, pthread_self(), 0, 0, 0, 1};
  hg_pool_run(pool, mark_body, &rc, 10);
  CHECK(rc.band0_on_caller);
  CHECK_EQ_INT(rc.bands_run, 1);
  for (int i = 0; i < 10; i++)
    CHECK_EQ_INT(seen[i], 1);
  hg_pool_destroy(pool);

  memset(seen, 0, sizeof seen);
  rc.bands_run = 0;
  rc.band0_on_caller = 0;
  hg_pool_run(NULL, mark_body, &rc, 10);
  CHECK(rc.band0_on_caller);
  CHECK_EQ_INT(rc.bands_run, 1);
  CHECK_EQ_INT(hg_pool_threads(NULL), 1);
  for (int i = 0; i < 10; i++)
    CHECK_EQ_INT(seen[i], 1);
  // 0 participants means 1.
  pool = hg_pool_create(0);
  CHECK(pool != NULL);
  CHECK_EQ_INT(hg_pool_threads(pool), 1);
  hg_pool_destroy(pool);
  hg_pool_destroy(NULL);
}

// --- Teams and barriers ----------------------------------------------------

typedef struct {
  struct hg_pool *pool;
  unsigned *phase1; // per participant, written in phase 1
  unsigned *sum;    // per participant, the sum of everyone's phase1
  unsigned count_seen;
} TeamCheck;

static void team_body(void *arg, unsigned index, unsigned count) {
  TeamCheck *tc = arg;
  if (index == 0)
    tc->count_seen = count;
  tc->phase1[index] = index * 10 + 1;
  hg_pool_barrier(tc->pool);
  // After the barrier every participant's phase1 is visible to all.
  unsigned s = 0;
  for (unsigned i = 0; i < count; i++)
    s += tc->phase1[i];
  tc->sum[index] = s;
  hg_pool_barrier(tc->pool);
  // A second barrier in the same run: reuse must not deadlock or let a
  // fast participant through early.
  tc->phase1[index] = 0;
  hg_pool_barrier(tc->pool);
  for (unsigned i = 0; i < count; i++)
    if (tc->phase1[i] != 0)
      __atomic_store_n(&tc->sum[index], 0xffffffffu, __ATOMIC_RELAXED);
}

static void test_team_barrier(void) {
  for (unsigned threads = 1; threads <= 6; threads++) {
    struct hg_pool *pool = hg_pool_create(threads);
    CHECK(pool != NULL);
    if (!pool)
      continue;
    unsigned phase1[8], sum[8];
    TeamCheck tc = {pool, phase1, sum, 0};
    unsigned expected = 0;
    for (unsigned i = 0; i < threads; i++)
      expected += i * 10 + 1;
    for (int rep = 0; rep < 2000; rep++) {
      hg_pool_team(pool, team_body, &tc);
      CHECK_EQ_INT(tc.count_seen, threads);
      for (unsigned i = 0; i < threads; i++)
        if (sum[i] != expected) {
          CHECK_EQ_INT(sum[i], expected);
          break;
        }
    }
    hg_pool_destroy(pool);
  }
  // Barrier on a NULL pool: a no-op.
  hg_pool_barrier(NULL);
}

// --- The default thread count ----------------------------------------------

static void test_default_threads(void) {
  setenv("OMP_NUM_THREADS", "3", 1);
  CHECK_EQ_INT(hg_pool_default_threads(), 3);
  setenv("OMP_NUM_THREADS", "4,2", 1); // a list: the first value counts
  CHECK_EQ_INT(hg_pool_default_threads(), 4);
  setenv("OMP_NUM_THREADS", "1000", 1); // clamped
  CHECK_EQ_INT(hg_pool_default_threads(), HG_POOL_MAX_THREADS);
  setenv("OMP_NUM_THREADS", "0", 1); // malformed: ignored
  unsigned cpus = hg_pool_default_threads();
  CHECK(cpus >= 1 && cpus <= HG_POOL_MAX_THREADS);
  setenv("OMP_NUM_THREADS", "abc", 1);
  CHECK_EQ_INT(hg_pool_default_threads(), cpus);
  unsetenv("OMP_NUM_THREADS");
  CHECK_EQ_INT(hg_pool_default_threads(), cpus);
  // What a pool made from it reports.
  struct hg_pool *pool = hg_pool_create(hg_pool_default_threads());
  CHECK(pool != NULL);
  CHECK_EQ_INT(hg_pool_threads(pool), cpus);
  hg_pool_destroy(pool);
}

int main(void) {
  RUN_TEST(test_band_matches_rule);
#ifdef _OPENMP
  RUN_TEST(test_band_matches_openmp);
#else
  printf("  skip test_band_matches_openmp (built without OpenMP)\n");
#endif
  RUN_TEST(test_one_thread_runs_inline);
  RUN_TEST(test_runs_lose_nothing_3);
  RUN_TEST(test_runs_lose_nothing_4);
  RUN_TEST(test_runs_lose_nothing_12);
  RUN_TEST(test_team_barrier);
  RUN_TEST(test_default_threads);
  TEST_MAIN_END();
}
