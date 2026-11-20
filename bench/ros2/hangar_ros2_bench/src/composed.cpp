// SPDX-License-Identifier: GPL-3.0-or-later
/* hgb_composed: the talker node and the listener node in ONE process, on one
 * SingleThreadedExecutor, with rclcpp's intra-process delivery (a
 * std::unique_ptr handed from publisher to subscription, no middleware in
 * the path) unless --no-intra. This is ROS 2's own answer to the cost of
 * separate processes, and it is not what hangar is compared with: a hangar
 * module is a process, restarted alone when it fails. The run is in the
 * table because the question will be asked, and it is labelled.
 *
 * A wall timer publishes at `rate`; the stamp is the clock right before
 * publish(), the sample the clock at the top of the subscription's callback
 * minus the stamp, as in the two-process pair. `warmup` seconds are not
 * measured. CPU time and context switches are the one process's, over the
 * measured part. */
#include <chrono>
#include <future>
#include <memory>
#include <utility>

#include "common.hpp"

using namespace hgb;

template<class M>
static int run(const Opts & o)
{
  rclcpp::NodeOptions no;
  no.use_intra_process_comms(o.intra);
  auto talker = std::make_shared<rclcpp::Node>("hgb_talker", no);
  auto listener = std::make_shared<rclcpp::Node>("hgb_listener", no);
  const unsigned long long warm = static_cast<unsigned long long>(o.rate) * o.warmup;
  const unsigned long long expect = static_cast<unsigned long long>(o.rate) * o.seconds;
  std::vector<uint32_t> lat;
  lat.reserve(expect + 16);
  unsigned long long received = 0, torn = 0, seq = 0;
  Usage a, b;
  std::promise<void> done;
  auto done_f = done.get_future();
  bool done_set = false;

  auto pub = talker->create_publisher<M>(TOPIC, make_qos(o));
  auto sub = listener->create_subscription<M>(
    TOPIC, make_qos(o), [&](std::unique_ptr<M> m) {
      uint64_t now = now_ns();
      if (m->seq < warm || m->seq >= warm + expect) {
        return;
      }
      received++;
      lat.push_back(clamp32(now > m->stamp_ns ? now - m->stamp_ns : 0));
      torn += !words_intact(*m, 0);
    });
  auto timer = talker->create_wall_timer(
    std::chrono::nanoseconds(1000000000ull / o.rate), [&]() {
      if (seq == warm) {
        a = usage_now();
      }
      if (seq >= warm + expect) {
        if (!done_set) {
          b = usage_now();
          done_set = true;
          done.set_value();
        }
        return;
      }
      auto m = std::make_unique<M>();
      fill_words(*m, seq, 0);
      m->seq = seq++;
      m->stamp_ns = now_ns();
      pub->publish(std::move(m));
    });

  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(talker);
  exec.add_node(listener);
  auto rc = exec.spin_until_future_complete(
    done_f, std::chrono::seconds(2 * (o.warmup + o.seconds) + 30));
  if (!done_set) {
    b = usage_now();
  }
  timer->cancel();
  Pct p = percentiles(lat, 1000.0);
  printf(
    "{\"role\": \"composed\", \"rmw\": \"%s\", \"mode\": \"rate\", \"qos\": \"%s\", \"size\": %u, "
    "\"intra_process\": %s, \"rate_hz\": %u, \"seconds\": %u, \"expected\": %llu, "
    "\"received\": %llu, \"lost\": %llu, \"torn\": %llu, \"timed_out\": %s, \"samples\": %zu, "
    "\"p50_us\": %.1f, \"p99_us\": %.1f, \"p999_us\": %.1f, \"max_us\": %.1f, \"mean_us\": %.1f, "
    "%s, \"threads\": %ld, \"hwm_kb\": %ld}\n",
    rmw_get_implementation_identifier(), o.qos.c_str(), o.size, o.intra ? "true" : "false",
    o.rate, o.seconds, expect, received, expect - received, torn,
    rc == rclcpp::FutureReturnCode::SUCCESS ? "false" : "true", lat.size(), p.p50, p.p99, p.p999,
    p.max, p.mean, usage_json(a, b).c_str(), status_kb("Threads"), status_kb("VmHWM"));
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
