// SPDX-License-Identifier: GPL-3.0-or-later
/* hgb_listener: the subscribing process of the ROS 2 side of hangar's
 * benchmark. One node, one subscription, a SingleThreadedExecutor.
 *
 *   rate     hg_bench's `wake` reader: for every message, one clock read at
 *            the top of the callback minus the message's stamp
 *            (CLOCK_MONOTONIC both). The first `warmup` seconds' messages
 *            are received and not measured; p50, p99, p99.9 and max are
 *            over the `seconds` after them. Lost is what was published in
 *            that part and never reached the callback. CPU time and
 *            context switches are the measured part's, every thread of the
 *            process. It ends at the talker's end marker.
 *   idle     matched with a talker that publishes nothing: `idle-seconds`
 *            in the executor, context switches and CPU time.
 *   startup  ends at the first message and reports its CLOCK_MONOTONIC
 *            time; the runner knows when it started the two processes. */
#include <chrono>
#include <future>
#include <memory>

#include "common.hpp"

using namespace hgb;

template<class M>
static int run(const Opts & o)
{
  auto node = std::make_shared<rclcpp::Node>("hgb_listener");
  const unsigned long long warm = static_cast<unsigned long long>(o.rate) * o.warmup;
  const unsigned long long expect = static_cast<unsigned long long>(o.rate) * o.seconds;
  std::vector<uint32_t> lat;
  lat.reserve(expect + 16);
  unsigned long long received = 0, all = 0, torn = 0, out_of_order = 0;
  uint64_t first_ns = 0, last_seq = 0;
  bool have_a = false, have_b = false, startup = o.mode == "startup";
  Usage a, b;
  std::promise<void> done;
  auto done_f = done.get_future();
  bool done_set = false;

  auto cb = [&](std::shared_ptr<const M> m) {
      uint64_t now = now_ns();
      if (first_ns == 0) {
        first_ns = now;
        if (startup && !done_set) {
          done_set = true;
          done.set_value();
        }
      }
      if (m->seq == END_SEQ) {
        if (!have_b) {
          b = usage_now();
          have_b = true;
        }
        if (!done_set) {
          done_set = true;
          done.set_value();
        }
        return;
      }
      all++;
      if (m->seq < warm || m->seq >= warm + expect) {
        return;
      }
      if (!have_a) {
        a = usage_now();
        have_a = true;
      } else if (m->seq <= last_seq) {
        out_of_order++;
      }
      last_seq = m->seq;
      received++;
      lat.push_back(clamp32(now > m->stamp_ns ? now - m->stamp_ns : 0));
      torn += !words_intact(*m, 0);
      if (m->seq == warm + expect - 1 && !have_b) {
        b = usage_now();
        have_b = true;
      }
    };
  auto sub = node->create_subscription<M>(TOPIC, make_qos(o), cb);
  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(node);

  char head[512];
  snprintf(
    head, sizeof head,
    "\"role\": \"listener\", \"rmw\": \"%s\", \"mode\": \"%s\", \"qos\": \"%s\", \"size\": %u, "
    "\"can_loan\": %s", rmw_get_implementation_identifier(), o.mode.c_str(), o.qos.c_str(),
    o.size, sub->can_loan_messages() ? "true" : "false");

  if (o.mode == "idle") {
    std::promise<void> never;
    auto never_f = never.get_future();
    for (int i = 0; sub->get_publisher_count() == 0; i++) {
      if (i > 1200 || !rclcpp::ok()) {
        printf("{%s, \"error\": \"no publisher matched in 60 s\"}\n", head);
        return 3;
      }
      exec.spin_until_future_complete(never_f, std::chrono::milliseconds(50));
    }
    exec.spin_until_future_complete(never_f, std::chrono::milliseconds(1000));
    Usage i0 = usage_now();
    exec.spin_until_future_complete(never_f, std::chrono::seconds(o.idle_seconds));
    Usage i1 = usage_now();
    printf(
      "{%s, %s, \"msgs\": %llu, \"threads\": %ld, \"hwm_kb\": %ld}\n", head,
      usage_json(i0, i1).c_str(), all, status_kb("Threads"), status_kb("VmHWM"));
    return 0;
  }

  /* Discovery (up to 60 s), the warm-up, the run, and 30 s to spare. */
  auto limit = std::chrono::seconds(60 + o.warmup + o.seconds + 30);
  auto rc = exec.spin_until_future_complete(done_f, limit);
  if (!have_b) {
    b = usage_now();
  }
  if (startup) {
    printf(
      "{%s, \"first_msg_ns\": %" PRIu64 ", \"timed_out\": %s}\n", head, first_ns,
      rc == rclcpp::FutureReturnCode::SUCCESS ? "false" : "true");
    return first_ns ? 0 : 3;
  }
  Pct p = percentiles(lat, 1000.0);
  printf(
    "{%s, \"rate_hz\": %u, \"seconds\": %u, \"expected\": %llu, \"received\": %llu, "
    "\"lost\": %llu, \"torn\": %llu, \"out_of_order\": %llu, \"timed_out\": %s, \"samples\": %zu, "
    "\"p50_us\": %.1f, \"p99_us\": %.1f, \"p999_us\": %.1f, \"max_us\": %.1f, \"mean_us\": %.1f, "
    "%s, \"first_msg_ns\": %" PRIu64 ", \"threads\": %ld, \"hwm_kb\": %ld}\n",
    head, o.rate, o.seconds, expect, received, expect - received, torn, out_of_order,
    rc == rclcpp::FutureReturnCode::SUCCESS ? "false" : "true", lat.size(), p.p50, p.p99, p.p999,
    p.max, p.mean, usage_json(have_a ? a : b, b).c_str(), first_ns, status_kb("Threads"),
    status_kb("VmHWM"));
  return received ? 0 : 3;
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
