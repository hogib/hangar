// SPDX-License-Identifier: GPL-3.0-or-later
/* What the talker, the listener and the composed pair share: the options,
 * the clock hg_bench uses (CLOCK_MONOTONIC), the process's own CPU time and
 * context switches (getrusage: every thread of the process, the middleware's
 * included), percentiles as hg_bench takes them, and the two QoS profiles.
 *
 * Every program prints one line of JSON on stdout when it is done;
 * bench/ros2/matrix.py reads it. */
#ifndef HANGAR_ROS2_BENCH_COMMON_HPP
#define HANGAR_ROS2_BENCH_COMMON_HPP

#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"

#include "hangar_ros2_bench/msg/fixed16.hpp"
#include "hangar_ros2_bench/msg/fixed256.hpp"
#include "hangar_ros2_bench/msg/fixed4k.hpp"
#include "hangar_ros2_bench/msg/fixed1m.hpp"

namespace hgb
{

constexpr uint64_t END_SEQ = UINT64_MAX;  // the talker's "that was all"
constexpr const char * TOPIC = "hgb";

inline uint64_t now_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

inline void sleep_ms(long ms)
{
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0) {
  }
}

struct Opts
{
  std::string mode = "rate";   // rate | cost | take | idle | startup
  std::string qos = "latest";  // latest: best-effort keep-last 1; queue: reliable keep-last depth
  unsigned size = 256;         // 16, 256, 4096, 1048576
  unsigned rate = 1000;        // Hz
  unsigned seconds = 20;       // the measured part
  unsigned warmup = 2;         // seconds published and received before it, not measured
  unsigned depth = 16;         // the queue's depth: hangar's default ring has 16 slots
  unsigned idle_seconds = 30;
  unsigned long long count = 200000;  // cost, take: publishes per run
  unsigned runs = 5;
  int cpu = -1;                // pin the whole process (every thread) here; -1: do not
  bool loan = false;           // ask the middleware for loaned messages
  bool intra = true;           // composed only: rclcpp's intra-process delivery
};

[[noreturn]] inline void usage(const char * prog)
{
  fprintf(
    stderr,
    "usage: %s [--mode rate|cost|take|idle|startup] [--size 16|256|4096|1048576] [--rate HZ]\n"
    "       [--seconds N] [--warmup N] [--qos latest|queue] [--depth N] [--idle-seconds N]\n"
    "       [--count N] [--runs N] [--cpu N] [--loan] [--no-intra]\n", prog);
  exit(2);
}

/* Options first, ROS's own arguments (--ros-args ...) after them, untouched. */
inline Opts parse(int argc, char ** argv)
{
  Opts o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--ros-args") {
      break;
    }
    if (a == "--loan") {
      o.loan = true;
      continue;
    }
    if (a == "--no-intra") {
      o.intra = false;
      continue;
    }
    if (i + 1 >= argc) {
      usage(argv[0]);
    }
    const char * v = argv[++i];
    if (a == "--mode") {
      o.mode = v;
    } else if (a == "--qos") {
      o.qos = v;
    } else if (a == "--size") {
      o.size = static_cast<unsigned>(strtoul(v, nullptr, 10));
    } else if (a == "--rate") {
      o.rate = static_cast<unsigned>(strtoul(v, nullptr, 10));
    } else if (a == "--seconds") {
      o.seconds = static_cast<unsigned>(strtoul(v, nullptr, 10));
    } else if (a == "--warmup") {
      o.warmup = static_cast<unsigned>(strtoul(v, nullptr, 10));
    } else if (a == "--depth") {
      o.depth = static_cast<unsigned>(strtoul(v, nullptr, 10));
    } else if (a == "--idle-seconds") {
      o.idle_seconds = static_cast<unsigned>(strtoul(v, nullptr, 10));
    } else if (a == "--count") {
      o.count = strtoull(v, nullptr, 10);
    } else if (a == "--runs") {
      o.runs = static_cast<unsigned>(strtoul(v, nullptr, 10));
    } else if (a == "--cpu") {
      o.cpu = atoi(v);
    } else {
      usage(argv[0]);
    }
  }
  if ((o.size != 16 && o.size != 256 && o.size != 4096 && o.size != 1048576) || o.rate == 0 ||
    o.rate > 100000 || o.seconds == 0 || o.depth == 0 || o.runs == 0 || o.count == 0 ||
    (o.qos != "latest" && o.qos != "queue"))
  {
    usage(argv[0]);
  }
  return o;
}

/* Before rclcpp::init: the middleware's threads inherit the mask, so the whole
 * process sits on one CPU, as a hangar module (one thread) does in hg_bench. */
inline void pin(int cpu)
{
  if (cpu < 0) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof set, &set) != 0) {
    perror("sched_setaffinity");
    exit(1);
  }
}

/* latest: what hg_latest gives, the newest message and nothing kept for a
 * slow reader. queue: what hg_next gives over a ring of `depth` slots, every
 * message in order while the reader keeps up. */
inline rclcpp::QoS make_qos(const Opts & o)
{
  if (o.qos == "queue") {
    return rclcpp::QoS(rclcpp::KeepLast(o.depth)).reliable().durability_volatile();
  }
  return rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile();
}

struct Usage
{
  uint64_t t_ns = 0;
  double cpu_s = 0;
  long vol = 0, invol = 0;
};

inline Usage usage_now()
{
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  Usage u;
  u.t_ns = now_ns();
  u.cpu_s = static_cast<double>(ru.ru_utime.tv_sec) + ru.ru_utime.tv_usec / 1e6 +
    static_cast<double>(ru.ru_stime.tv_sec) + ru.ru_stime.tv_usec / 1e6;
  u.vol = ru.ru_nvcsw;
  u.invol = ru.ru_nivcsw;
  return u;
}

/* "window_s": .., "cpu_s": .., "cpu_pct": .., "ctxsw": [vol, invol] between two snapshots. */
inline std::string usage_json(const Usage & a, const Usage & b)
{
  double w = static_cast<double>(b.t_ns - a.t_ns) / 1e9;
  char buf[256];
  snprintf(
    buf, sizeof buf,
    "\"window_s\": %.3f, \"cpu_s\": %.4f, \"cpu_pct\": %.3f, \"ctxsw\": [%ld, %ld]", w,
    b.cpu_s - a.cpu_s, w > 0 ? 100.0 * (b.cpu_s - a.cpu_s) / w : 0.0, b.vol - a.vol,
    b.invol - a.invol);
  return buf;
}

inline long status_kb(const char * key)
{
  FILE * f = fopen("/proc/self/status", "r");
  if (!f) {
    return -1;
  }
  char line[256];
  long v = -1;
  size_t n = strlen(key);
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, key, n) && line[n] == ':') {
      v = strtol(line + n + 1, nullptr, 10);
      break;
    }
  }
  fclose(f);
  return v;
}

/* Sorted in place; the indices are hg_bench's: n/2, n*0.99, n*0.999, n-1. */
struct Pct
{
  double p50 = 0, p99 = 0, p999 = 0, max = 0, mean = 0;
};

inline Pct percentiles(std::vector<uint32_t> & v, double unit)
{
  Pct p;
  size_t n = v.size();
  if (n == 0) {
    return p;
  }
  std::sort(v.begin(), v.end());
  auto at = [&](double q) {
      size_t i = static_cast<size_t>(static_cast<double>(n) * q);
      return v[i < n ? i : n - 1] / unit;
    };
  double sum = 0;
  for (uint32_t x : v) {
    sum += x;
  }
  p.p50 = v[n / 2] / unit;
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  p.max = v[n - 1] / unit;
  p.mean = sum / static_cast<double>(n) / unit;
  return p;
}

inline uint32_t clamp32(uint64_t d)
{
  return d > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(d);
}

/* Every word of message n holds n, as in hg_bench; Fixed16 has no words. */
template<class M>
auto fill_words(M & m, uint64_t seq, int)->decltype(m.w, void())
{
  for (auto & x : m.w) {
    x = seq;
  }
}
template<class M>
void fill_words(M &, uint64_t, long) {}

template<class M>
auto words_intact(const M & m, int)->decltype(m.w, bool())
{
  for (auto x : m.w) {
    if (x != m.seq) {
      return false;
    }
  }
  return true;
}
template<class M>
bool words_intact(const M &, long) {return true;}

/* run<M>(opts) for the message of opts.size. */
#define HGB_DISPATCH(fn, o) \
  ((o).size == 16 ? fn<hangar_ros2_bench::msg::Fixed16>(o) : \
  (o).size == 256 ? fn<hangar_ros2_bench::msg::Fixed256>(o) : \
  (o).size == 4096 ? fn<hangar_ros2_bench::msg::Fixed4k>(o) : \
  fn<hangar_ros2_bench::msg::Fixed1m>(o))

}  // namespace hgb

#endif
