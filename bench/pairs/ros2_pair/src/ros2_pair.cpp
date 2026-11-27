// SPDX-License-Identifier: GPL-3.0-or-later
// bench/pairs/ros2_pair: ROS 2's node pair for bench/pairs (README.md there). One
// rclcpp node, a publisher or a subscription in a process of its own, for whichever
// middleware RMW_IMPLEMENTATION names: the registry's entries differ in their
// environment, not in this file.
//
//   ros2_pair_node --role pub|sub|both ... [-- --qos queue|latest] [--loan] [--intra]
//                  [--waitset]
//
// The message is a fixed-size type (msg/Pair32 ... Pair1m: an array of uint64 and
// nothing else), so that a middleware that can lend its samples is allowed to. The
// sizes are those five; a cell of another size is refused.
//
// --qos queue (the default): reliable, keep-last 16, volatile. What hangar's ring of
//   16 slots read in order means: every message while the reader keeps up, and in DDS
//   a keep-last history does not make the writer wait either. --qos latest: best
//   effort, keep-last 1.
// --loan: the publisher builds the message in a sample borrowed from the middleware
//   where the middleware lends one (it says whether it does: "loaned" on stderr), and
//   the subscription takes loans where ROS_DISABLE_LOANED_MESSAGES=0 lets it.
// --intra: with --role both, publisher and subscription in one node with rclcpp's
//   intra-process delivery. Not the comparison (hangar's modules are processes): its
//   own entry, because the question is asked.
// --waitset: the subscriber waits in an rclcpp::WaitSet and takes the message itself,
//   with no executor. Not how a ROS 2 node is usually written; it separates what the
//   executor costs from what the middleware costs.
//
// The subscriber, as a ROS 2 node is written: a SingleThreadedExecutor spun until a
// message has come, and the subscription's callback, whose first statement is the
// harness's pair_deliver: the clock is read there and the message read in place. The
// publisher's stamp is read by the harness immediately before publish(), so the
// middleware's serialisation, where it has one, is inside the sample.
#include "pair.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "rcl/subscription.h"
#include "rclcpp/rclcpp.hpp"
#include "rmw/rmw.h"

#include "ros2_pair/msg/pair1m.hpp"
#include "ros2_pair/msg/pair256.hpp"
#include "ros2_pair/msg/pair32.hpp"
#include "ros2_pair/msg/pair4k.hpp"
#include "ros2_pair/msg/pair64k.hpp"

namespace
{

struct Opts
{
  std::string qos = "queue";
  bool loan = false, intra = false, waitset = false;
  std::string topic;
};

struct Side
{
  virtual ~Side() = default;
  virtual bool open_pub() = 0;
  virtual void * buffer() = 0;
  virtual int send(void * msg) = 0;
  virtual bool open_sub() = 0;
  virtual long recv(const void ** msg, int timeout_ms) = 0;
  virtual int done() = 0;
};

Opts opts;
rclcpp::Node::SharedPtr node;
std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> exec;
std::unique_ptr<Side> side;

rclcpp::QoS make_qos()
{
  if (opts.qos == "latest") {
    return rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile();
  }
  return rclcpp::QoS(rclcpp::KeepLast(16)).reliable().durability_volatile();
}

template<typename M>
struct SideOf : Side
{
  typename rclcpp::Publisher<M>::SharedPtr pub;
  typename rclcpp::Subscription<M>::SharedPtr sub;
  std::unique_ptr<M> scratch;
  std::unique_ptr<rclcpp::LoanedMessage<M>> lent;
  std::unique_ptr<rclcpp::WaitSet> ws;
  void * taken = nullptr;       // --waitset: a loan taken, to give back in done()
  bool lending = false, had_publisher = false;

  bool open_pub() override
  {
    pub = node->create_publisher<M>(opts.topic, make_qos());
    lending = opts.loan && pub->can_loan_messages();
    std::fprintf(stderr, "ros2_pair: rmw %s, qos %s, loan asked %s, loaned %s\n",
      rmw_get_implementation_identifier(), opts.qos.c_str(), opts.loan ? "yes" : "no",
      lending ? "yes" : "no");
    if (!lending) {
      scratch = std::make_unique<M>();
    }
    return true;
  }

  void * buffer() override
  {
    if (!lending) {
      return scratch->w.data();
    }
    lent = std::make_unique<rclcpp::LoanedMessage<M>>(pub->borrow_loaned_message());
    return lent->get().w.data();
  }

  int send(void *) override
  {
    if (lending) {
      pub->publish(std::move(*lent));
      lent.reset();
    } else {
      pub->publish(*scratch);
    }
    return 0;
  }

  bool open_sub() override
  {
    rclcpp::SubscriptionOptions so;
    if (opts.waitset) {
      // Never given to an executor: taken by hand in recv().
      sub = node->create_subscription<M>(opts.topic, make_qos(),
          [](typename M::ConstSharedPtr) {}, so);
      ws = std::make_unique<rclcpp::WaitSet>();
      ws->add_subscription(sub);
      scratch = std::make_unique<M>();
    } else {
      sub = node->create_subscription<M>(opts.topic, make_qos(),
          [](typename M::ConstSharedPtr m) {
            pair_deliver(m->w.data(), static_cast<long>(sizeof m->w));
          }, so);
    }
    std::fprintf(stderr, "ros2_pair: rmw %s, qos %s, subscription takes loans %s, %s\n",
      rmw_get_implementation_identifier(), opts.qos.c_str(),
      sub->can_loan_messages() ? "yes" : "no", opts.waitset ? "wait set" : "executor");
    return true;
  }

  // The publisher was there and is no more: the run is over for this subscriber.
  long idle()
  {
    size_t n = sub->get_publisher_count();
    if (n) {
      had_publisher = true;
    }
    return had_publisher && !n ? -1 : 0;
  }

  long recv(const void ** msg, int timeout_ms) override
  {
    if (!opts.waitset) {
      if (timeout_ms <= 0) {
        // --role both: what was published a statement ago. rclcpp's intra-process
        // delivery wakes the executor through a guard condition, and one pass of
        // spin_some may only take that in: a few passes, none of them waiting.
        unsigned before = pair_cb.n;
        for (int i = 0; i < 4 && pair_cb.n == before; i++) {
          exec->spin_some();
        }
        return 0;
      }
      unsigned before = pair_cb.n;
      exec->spin_once(std::chrono::milliseconds(timeout_ms));
      return pair_cb.n != before ? 0 : idle();
    }
    auto r = ws->wait(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0));
    if (r.kind() != rclcpp::WaitResultKind::Ready) {
      return timeout_ms > 0 ? idle() : 0;
    }
    rclcpp::MessageInfo info;
    if (sub->can_loan_messages()) {
      // rclcpp keeps the taking of a loan to its executor: rcl's own call.
      void * m = nullptr;
      if (rcl_take_loaned_message(sub->get_subscription_handle().get(), &m,
        &info.get_rmw_message_info(), nullptr) != RCL_RET_OK || !m)
      {
        return 0;
      }
      taken = m;
      *msg = static_cast<M *>(m)->w.data();
      return static_cast<long>(sizeof scratch->w);
    }
    if (!sub->take(*scratch, info)) {
      return 0;
    }
    *msg = scratch->w.data();
    return static_cast<long>(sizeof scratch->w);
  }

  int done() override
  {
    if (taken) {
      if (rcl_return_loaned_message_from_subscription(sub->get_subscription_handle().get(),
        taken) != RCL_RET_OK)
      {
        std::fprintf(stderr, "ros2_pair: a loan could not be given back\n");
      }
      taken = nullptr;
    }
    return 1;
  }
};

bool setup(const pair_opts * o)
{
  if (node) {
    return true;  // --role both: the second open, in the same node
  }
  for (int i = 0; i < o->argc; i++) {
    std::string a = o->argv[i];
    if (a == "--loan") {
      opts.loan = true;
    } else if (a == "--intra") {
      opts.intra = true;
    } else if (a == "--waitset") {
      opts.waitset = true;
    } else if (a == "--qos" && i + 1 < o->argc) {
      opts.qos = o->argv[++i];
    } else {
      std::fprintf(stderr, "ros2_pair: unknown option %s\n", a.c_str());
      return false;
    }
  }
  if (opts.qos != "queue" && opts.qos != "latest") {
    std::fprintf(stderr, "ros2_pair: --qos queue or latest\n");
    return false;
  }
  opts.topic = std::string("pair_") + o->topic;
  switch (o->size) {
    case 32: side = std::make_unique<SideOf<ros2_pair::msg::Pair32>>(); break;
    case 256: side = std::make_unique<SideOf<ros2_pair::msg::Pair256>>(); break;
    case 4096: side = std::make_unique<SideOf<ros2_pair::msg::Pair4k>>(); break;
    case 65536: side = std::make_unique<SideOf<ros2_pair::msg::Pair64k>>(); break;
    case 1048576: side = std::make_unique<SideOf<ros2_pair::msg::Pair1m>>(); break;
    default:
      std::fprintf(stderr,
        "ros2_pair: no message type of %u bytes (32, 256, 4096, 65536, 1048576)\n", o->size);
      return false;
  }
  try {
    rclcpp::init(0, nullptr, rclcpp::InitOptions(), rclcpp::SignalHandlerOptions::None);
    rclcpp::NodeOptions no;
    no.use_intra_process_comms(opts.intra);
    no.start_parameter_services(false);
    no.start_parameter_event_publisher(false);
    node = std::make_shared<rclcpp::Node>(
      std::string("pair_") + o->role + "_" + std::to_string(o->id) + "_" + o->topic, no);
    exec = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    exec->add_node(node);
  } catch (const std::exception & e) {
    std::fprintf(stderr, "ros2_pair: %s\n", e.what());
    return false;
  }
  return true;
}

int pub_open(const pair_opts * o)
{
  try {
    return setup(o) && side->open_pub() ? 0 : -1;
  } catch (const std::exception & e) {
    std::fprintf(stderr, "ros2_pair: %s\n", e.what());
    return -1;
  }
}

void * pub_buffer() {return side->buffer();}
int pub_send(void * msg, size_t) {return side->send(msg);}

void shut()
{
  side.reset();
  exec.reset();
  node.reset();
  if (rclcpp::ok()) {
    rclcpp::shutdown();
  }
}

void pub_close()
{
  // Reliable: what is still in the writer's history goes out before the writer does.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  shut();
}

int sub_open(const pair_opts * o)
{
  try {
    return setup(o) && side->open_sub() ? 0 : -1;
  } catch (const std::exception & e) {
    std::fprintf(stderr, "ros2_pair: %s\n", e.what());
    return -1;
  }
}

long sub_recv(const void ** msg, int timeout_ms)
{
  try {
    return side->recv(msg, timeout_ms);
  } catch (const std::exception & e) {
    std::fprintf(stderr, "ros2_pair: %s\n", e.what());
    return -1;
  }
}

int sub_done() {return side->done();}

void sub_close()
{
  if (std::string(pair_cb.o ? pair_cb.o->role : "") != "both") {
    shut();
  }
}

const char * version()
{
  static std::string v;
  const char * d = std::getenv("ROS_DISTRO");
  v = std::string("ROS 2 ") + (d ? d : "?") + ", " + rmw_get_implementation_identifier();
  return v.c_str();
}

}  // namespace

int main(int argc, char ** argv)
{
  static pair_system sys;
  sys.name = "ros2";
  sys.version = version;
  sys.pub_open = pub_open;
  sys.pub_buffer = pub_buffer;
  sys.pub_send = pub_send;
  sys.pub_close = pub_close;
  sys.sub_open = sub_open;
  sys.sub_recv = sub_recv;
  sys.sub_done = sub_done;
  sys.sub_close = sub_close;
  return pair_main(argc, argv, &sys);
}
