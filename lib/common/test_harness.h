#pragma once
/*
 * Minimal assertion harness. No framework: these tests link against the
 * project's own translation units and run under `make check`, and a
 * dependency would be one more thing to install on a Pi before the tests
 * can be trusted.
 *
 * Each test file defines tests as plain functions, registers them in
 * main() with RUN_TEST, and returns TEST_EXIT_CODE. A failing check
 * reports file/line and keeps going, so one run shows every failure
 * rather than only the first.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int checks_failed = 0;
static int current_test_failures = 0;
static const char *current_test_name = "";

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      checks_failed++;                                                         \
      current_test_failures++;                                                 \
    }                                                                          \
  } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
  do {                                                                         \
    long long a_ = (long long)(actual), e_ = (long long)(expected);            \
    if (a_ != e_) {                                                            \
      printf("    FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__,     \
             __LINE__, #actual, #expected, a_, e_);                            \
      checks_failed++;                                                         \
      current_test_failures++;                                                 \
    }                                                                          \
  } while (0)

// Reports the first differing index rather than just "buffers differ" --
// with image planes that difference is the entire diagnostic.
#define CHECK_MEM_EQ(actual, expected, n)                                      \
  do {                                                                         \
    const uint8_t *a_ = (const uint8_t *)(actual);                             \
    const uint8_t *e_ = (const uint8_t *)(expected);                           \
    size_t n_ = (size_t)(n);                                                    \
    size_t i_ = 0;                                                             \
    for (; i_ < n_; i_++)                                                      \
      if (a_[i_] != e_[i_])                                                    \
        break;                                                                 \
    if (i_ != n_) {                                                            \
      printf("    FAIL %s:%d: %s != %s at byte %zu (got %u, want %u)\n",       \
             __FILE__, __LINE__, #actual, #expected, i_, a_[i_], e_[i_]);      \
      checks_failed++;                                                         \
      current_test_failures++;                                                 \
    }                                                                          \
  } while (0)

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    current_test_name = #fn;                                                   \
    current_test_failures = 0;                                                 \
    tests_run++;                                                               \
    fn();                                                                      \
    printf("  %-4s %s\n", current_test_failures ? "FAIL" : "ok", #fn);         \
  } while (0)

// A suite that ran no test has not passed: a filter that matches nothing
// ($HD_ONLY in hangard's suites) is a failure, not a green run of nothing.
#define TEST_MAIN_END()                                                        \
  do {                                                                         \
    int bad_ = checks_failed || !tests_run;                                    \
    printf("%s: %d tests, %d failed checks\n", bad_ ? "FAILED" : "PASSED",     \
           tests_run, checks_failed);                                          \
    return bad_ ? 1 : 0;                                                       \
  } while (0)

/* CPU used by a process, from /proc/<pid>/stat (utime + stime, in clock
 * ticks); -1 when it cannot be read. th_cpu_percent turns two readings ms
 * apart into percent of one core. For "a module asleep costs nothing": a
 * module that spins on a wait shows about 100 (hangar, 2026-11-19). */
#include <sys/types.h>
#include <unistd.h>

static inline long long th_cpu_ticks(pid_t pid)
{
  char path[64], line[1024];
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  size_t n = fread(line, 1, sizeof line - 1, f);
  fclose(f);
  line[n] = 0;
  const char *p = strrchr(line, ')'); /* the name may hold spaces and parentheses */
  if (!p)
    return -1;
  unsigned long long ut = 0, st = 0;
  /* After ") ": state, then fields 4..13, then utime (14) and stime (15). */
  if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2)
    return -1;
  return (long long)(ut + st);
}

static inline double th_cpu_percent(long long ticks0, long long ticks1, long ms)
{
  long hz = sysconf(_SC_CLK_TCK);
  if (ticks0 < 0 || ticks1 < 0 || ms <= 0 || hz <= 0)
    return -1;
  return (double)(ticks1 - ticks0) * 100.0 * 1000.0 / ((double)hz * (double)ms);
}
