// SPDX-License-Identifier: GPL-3.0-or-later
/* hgb_talker: the publishing process of the ROS 2 side of hangar's benchmark.
 *
 *   rate     hg_bench's `wake` writer: one message every period on an
 *            absolute clock_nanosleep schedule (CLOCK_MONOTONIC), stamped
 *            with the clock right before publish(). It waits for the
 *            listener to be matched first, publishes `warmup` seconds that
 *            nobody measures, then `seconds` that are, then an end marker.
 *            Its CPU time and context switches are those of the measured
 *            part only (hg_bench's are the whole run: its start costs
 *            nothing worth excluding, a DDS participant's does).
 *   cost     hg_bench's `publish`: ns per publish() back to back, no
 *            subscription anywhere, the median of `runs` runs of `count`.
 *   take     the nearest thing to hg_bench's `latest`: a publisher and a
 *            subscription in this one process, no executor; each turn
 *            publishes and polls take() until the message is there, and
 *            a second loop publishes alone. The difference per turn is
 *            the read: with a middleware that delivers within a process
 *            synchronously it is one take(), with one that does not it
 *            is the delivery too (`empty_polls` says which).
 *   idle     a publisher matched with the listener that publishes nothing
 *            for `idle-seconds`: context switches and CPU time, which a
 *            hangar module asleep in hg_wait has none of.
 *   startup  publishes at `rate` from the moment the publisher exists,
 *            waiting for nobody, for `seconds`. */
#include <memory>
#include <utility>

#include "common.hpp"

using namespace hgb;

/* timed: the words filled and the clock read, as hg_bench's wake writer does;
 * else a constant payload and no clock, as its publish loop (what is timed
 * there is the library's copy and protocol, not filling the buffer). */
template<class M>
static void publish_one(
  rclcpp::Publisher<M> & pub, M & scratch, bool loan, uint64_t seq, bool timed)
{
  if (loan) {
    auto l = pub.borrow_loaned_message();
    M & m = l.get();
    if (timed) {
      fill_words(m, seq, 0);
    }
    m.seq = seq;
    m.stamp_ns = timed ? now_ns() : seq;
    pub.publish(std::move(l));
  } else {
    if (timed) {
      fill_words(scratch, seq, 0);
    }
    scratch.seq = seq;
    scratch.stamp_ns = timed ? now_ns() : seq;
    pub.publish(scratch);
  }
}

template<class M>
static int run(const Opts & o)
{
  auto node = std::make_shared<rclcpp::Node>("hgb_talker");
  auto pub = node->create_publisher<M>(TOPIC, make_qos(o));
  auto scratch = std::make_unique<M>();
  bool loan = o.loan && pub->can_loan_messages();
  const char * common = "\"role\": \"talker\", \"rmw\": \"%s\", \"mode\": \"%s\", \"qos\": \"%s\", "
    "\"size\": %u, \"loan_asked\": %s, \"loaned\": %s";
  char head[512];
  snprintf(
    head, sizeof head, common, rmw_get_implementation_identifier(), o.mode.c_str(),
    o.qos.c_str(), o.size, o.loan ? "true" : "false", loan ? "true" : "false");

  if (o.mode == "cost") {
    std::vector<double> ns;
    for (unsigned r = 0; r < o.runs; r++) {
      uint64_t t0 = now_ns();
      for (unsigned long long i = 0; i < o.count; i++) {
        publish_one(*pub, *scratch, loan, i, false);
      }
      ns.push_back(static_cast<double>(now_ns() - t0) / static_cast<double>(o.count));
    }
    std::vector<double> sorted = ns;
    std::sort(sorted.begin(), sorted.end());
    printf("{%s, \"count\": %llu, \"ns\": %.1f, \"raw\": [", head, o.count, sorted[sorted.size() / 2]);
    for (size_t i = 0; i < ns.size(); i++) {
      printf("%s%.1f", i ? ", " : "", ns[i]);
    }
    printf("]}\n");
    return 0;
  }

  if (o.mode == "take") {
    /* A subscription polled by hand: its callback never runs (no executor). */
    auto sub = node->create_subscription<M>(TOPIC, make_qos(o), [](std::shared_ptr<const M>) {});
    for (int i = 0; i < 3000 && pub->get_subscription_count() == 0; i++) {
      sleep_ms(10);
    }
    sleep_ms(200);
    auto got = std::make_unique<M>();
    rclcpp::MessageInfo info;
    std::vector<double> ns;
    unsigned long long polls = 0, wrong = 0, gave_up = 0;
    for (unsigned r = 0; r < o.runs; r++) {
      uint64_t t0 = now_ns();
      for (unsigned long long i = 0; i < o.count; i++) {
        publish_one(*pub, *scratch, loan, i, false);
        unsigned long long tries = 0;
        while (!sub->take(*got, info)) {
          if (++tries > 100000000ull) {
            gave_up++;
            break;
          }
        }
        polls += tries;
        wrong += got->seq != i;
      }
      uint64_t t1 = now_ns();
      /* The publish part alone, the subscription still there; what it queues
       * is drained outside the timing. */
      for (unsigned long long i = 0; i < o.count; i++) {
        publish_one(*pub, *scratch, loan, i, false);
      }
      uint64_t t2 = now_ns();
      for (int quiet = 0; quiet < 3; ) {
        if (sub->take(*got, info)) {
          quiet = 0;
        } else {
          quiet++;
          sleep_ms(5);
        }
      }
      ns.push_back(
        (static_cast<double>(t1 - t0) - static_cast<double>(t2 - t1)) /
        static_cast<double>(o.count));
    }
    std::vector<double> sorted = ns;
    std::sort(sorted.begin(), sorted.end());
    printf(
      "{%s, \"count\": %llu, \"ns\": %.1f, \"empty_polls\": %llu, "
      "\"wrong\": %llu, \"gave_up\": %llu, \"raw\": [", head, o.count, sorted[sorted.size() / 2],
      polls, wrong, gave_up);
    for (size_t i = 0; i < ns.size(); i++) {
      printf("%s%.1f", i ? ", " : "", ns[i]);
    }
    printf("]}\n");
    return 0;
  }

  if (o.mode != "startup") {
    /* Up to 60 s for discovery to match the listener. */
    for (int i = 0; pub->get_subscription_count() == 0; i++) {
      if (i > 6000 || !rclcpp::ok()) {
        printf("{%s, \"error\": \"no subscription matched in 60 s\"}\n", head);
        return 3;
      }
      sleep_ms(10);
    }
    sleep_ms(500);
  }

  if (o.mode == "idle") {
    Usage a = usage_now();
    sleep_ms(o.idle_seconds * 1000L);
    Usage b = usage_now();
    printf(
      "{%s, %s, \"threads\": %ld, \"hwm_kb\": %ld}\n", head, usage_json(a, b).c_str(),
      status_kb("Threads"), status_kb("VmHWM"));
    fflush(stdout);
    sleep_ms(1500);  /* the listener's window ends with ours still matched */
    return 0;
  }

  /* rate, startup */
  prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);  /* as hg_bench's writer */
  bool measured = o.mode == "rate";
  unsigned long long warm = measured ? static_cast<unsigned long long>(o.rate) * o.warmup : 0;
  unsigned long long total = warm + static_cast<unsigned long long>(o.rate) * o.seconds;
  uint64_t period = 1000000000ull / o.rate;
  std::vector<uint32_t> pub_ns;
  pub_ns.reserve(total - warm);
  unsigned long long late = 0;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  Usage a = usage_now(), b;
  for (unsigned long long i = 0; i < total && rclcpp::ok(); i++) {
    next.tv_nsec += static_cast<long>(period);
    while (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr) == EINTR) {
    }
    if (i == warm) {
      a = usage_now();
    }
    uint64_t t0 = now_ns();
    uint64_t due = static_cast<uint64_t>(next.tv_sec) * 1000000000ull +
      static_cast<uint64_t>(next.tv_nsec);
    publish_one(*pub, *scratch, loan, i, true);
    uint64_t t1 = now_ns();
    if (i >= warm) {
      late += t0 > due + period;
      pub_ns.push_back(clamp32(t1 - t0));
    }
  }
  b = usage_now();
  long hwm = status_kb("VmHWM"), threads = status_kb("Threads");
  /* The end marker, a few times: a best-effort one may be lost. */
  for (int i = 0; i < 5 && measured; i++) {
    sleep_ms(20);
    publish_one(*pub, *scratch, loan, END_SEQ, true);
  }
  Pct p = percentiles(pub_ns, 1.0);
  printf(
    "{%s, \"rate_hz\": %u, \"seconds\": %u, \"warmup_msgs\": %llu, \"published\": %llu, "
    "\"late\": %llu, \"publish_p50_ns\": %.0f, \"publish_p99_ns\": %.0f, \"publish_mean_ns\": %.0f, "
    "%s, \"threads\": %ld, \"hwm_kb\": %ld}\n",
    head, o.rate, o.seconds, warm, total - warm, late, p.p50, p.p99, p.mean,
    usage_json(a, b).c_str(), threads, hwm);
  fflush(stdout);
  sleep_ms(300);  /* a reliable reader's last acknowledgements */
  return 0;
}

int main(int argc, char ** argv)
{
  Opts o = parse(argc, argv);
  pin(o.cpu);
  rclcpp::init(argc, argv);
  int rc = HGB_DISPATCH(run, o);
  rclcpp::shutdown();
  return rc;
}
