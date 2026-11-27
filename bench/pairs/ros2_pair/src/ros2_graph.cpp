// SPDX-License-Identifier: GPL-3.0-or-later
// bench/pairs/ros2_pair: a module of a graph as a ROS 2 node (graph.h; README.md, "A
// vehicle's graph"). One rclcpp node with every publisher and every subscription of
// the module, and a SingleThreadedExecutor, for whichever middleware
// RMW_IMPLEMENTATION names: what a ROS 2 node of that module would be.
//
//   ros2_graph_node --module NAME ... [-- --qos queue|latest] [--loan]
//
// Each subscription's callback has the harness's graph_deliver as its first
// statement. The node waits in the executor's spin_once, with a timeout at its next
// publication, and publishes from the same thread: a node with wall timers does the
// same in one. Sizes are those the package has a fixed-size message type for: 16, 32,
// 64, 128, 256, 512, 4096, 65536 and 1048576 bytes.
#include "graph.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rmw/rmw.h"

#include "ros2_pair/msg/pair128.hpp"
#include "ros2_pair/msg/pair16.hpp"
#include "ros2_pair/msg/pair1m.hpp"
#include "ros2_pair/msg/pair256.hpp"
#include "ros2_pair/msg/pair32.hpp"
#include "ros2_pair/msg/pair4k.hpp"
#include "ros2_pair/msg/pair512.hpp"
#include "ros2_pair/msg/pair64.hpp"
#include "ros2_pair/msg/pair64k.hpp"

namespace
{

std::string qos_name = "queue";
bool want_loan = false;
rclcpp::Node::SharedPtr node;
std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> exec;

rclcpp::QoS make_qos()
{
  if (qos_name == "latest") {
    return rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile();
  }
  return rclcpp::QoS(rclcpp::KeepLast(16)).reliable().durability_volatile();
}

std::string ros_name(const char * topic)
{
  std::string s = std::string("g_") + topic;
  for (auto & c : s) {
    if (c == '.' || c == '-') {
      c = '_';
    }
  }
  return s;
}

struct Pub
{
  virtual ~Pub() = default;
  virtual void * buffer() = 0;
  virtual int send() = 0;
};

template<typename M>
struct PubOf : Pub
{
  typename rclcpp::Publisher<M>::SharedPtr pub;
  std::unique_ptr<M> scratch;
  std::unique_ptr<rclcpp::LoanedMessage<M>> lent;
  bool lending = false;

  explicit PubOf(const char * topic)
  {
    pub = node->create_publisher<M>(ros_name(topic), make_qos());
    lending = want_loan && pub->can_loan_messages();
    if (!lending) {
      scratch = std::make_unique<M>();
    }
  }
  void * buffer() override
  {
    if (!lending) {
      return scratch->w.data();
    }
    lent = std::make_unique<rclcpp::LoanedMessage<M>>(pub->borrow_loaned_message());
    return lent->get().w.data();
  }
  int send() override
  {
    if (lending) {
      if (!lent) {
        return -1;  // the loan was refused: nothing to publish
      }
      pub->publish(std::move(*lent));
      lent.reset();
    } else {
      pub->publish(*scratch);
    }
    return 0;
  }
};

std::vector<std::unique_ptr<Pub>> pubs;
std::vector<rclcpp::SubscriptionBase::SharedPtr> subs;
bool any_loaned = false;

template<typename M>
void add(bool is_pub, int i, const char * topic)
{
  if (is_pub) {
    auto p = std::make_unique<PubOf<M>>(topic);
    any_loaned = any_loaned || p->lending;
    pubs.push_back(std::move(p));
  } else {
    subs.push_back(node->create_subscription<M>(ros_name(topic), make_qos(),
      [i](typename M::ConstSharedPtr m) {
        graph_deliver(i, m->w.data(), static_cast<long>(sizeof m->w));
      }));
  }
}

bool add_end(bool is_pub, int i, const graph_end & e)
{
  switch (e.size) {
    case 16: add<ros2_pair::msg::Pair16>(is_pub, i, e.topic); break;
    case 32: add<ros2_pair::msg::Pair32>(is_pub, i, e.topic); break;
    case 64: add<ros2_pair::msg::Pair64>(is_pub, i, e.topic); break;
    case 128: add<ros2_pair::msg::Pair128>(is_pub, i, e.topic); break;
    case 256: add<ros2_pair::msg::Pair256>(is_pub, i, e.topic); break;
    case 512: add<ros2_pair::msg::Pair512>(is_pub, i, e.topic); break;
    case 4096: add<ros2_pair::msg::Pair4k>(is_pub, i, e.topic); break;
    case 65536: add<ros2_pair::msg::Pair64k>(is_pub, i, e.topic); break;
    case 1048576: add<ros2_pair::msg::Pair1m>(is_pub, i, e.topic); break;
    default:
      std::fprintf(stderr, "ros2_graph: no message type of %u bytes for %s\n", e.size, e.name);
      return false;
  }
  return true;
}

int open_all(const graph_opts * o)
{
  for (int i = 0; i < o->argc; i++) {
    std::string a = o->argv[i];
    if (a == "--loan") {
      want_loan = true;
    } else if (a == "--qos" && i + 1 < o->argc) {
      qos_name = o->argv[++i];
    } else {
      std::fprintf(stderr, "ros2_graph: unknown option %s\n", a.c_str());
      return -1;
    }
  }
  try {
    rclcpp::init(0, nullptr, rclcpp::InitOptions(), rclcpp::SignalHandlerOptions::None);
    rclcpp::NodeOptions no;
    no.start_parameter_services(false);
    no.start_parameter_event_publisher(false);
    std::string name = std::string("g_") + o->prefix + "_" + o->module;
    for (auto & c : name) {
      if (c == '.' || c == '-') {
        c = '_';
      }
    }
    node = std::make_shared<rclcpp::Node>(name, no);
    exec = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    exec->add_node(node);
    for (int i = 0; i < o->npub; i++) {
      if (!add_end(true, i, o->pub[i])) {
        return -1;
      }
    }
    for (int i = 0; i < o->nsub; i++) {
      if (!add_end(false, i, o->sub[i])) {
        return -1;
      }
    }
  } catch (const std::exception & e) {
    std::fprintf(stderr, "ros2_graph: %s\n", e.what());
    return -1;
  }
  std::fprintf(stderr, "ros2_graph: rmw %s, qos %s, loan asked %s, a publisher lends %s\n",
    rmw_get_implementation_identifier(), qos_name.c_str(), want_loan ? "yes" : "no",
    any_loaned ? "yes" : "no");
  return 0;
}

// A loan the middleware does not grant, or a publish it refuses, is a publication
// that failed, and is counted: the node goes on.
void * pub_buffer(int i)
{
  try {
    return pubs[i]->buffer();
  } catch (const std::exception & e) {
    static int said = 0;
    if (said++ < 5) {
      std::fprintf(stderr, "ros2_graph: publication %d: no sample to build in: %s\n", i, e.what());
    }
    return nullptr;
  }
}

int pub_send(int i, void *, size_t)
{
  try {
    return pubs[i]->send();
  } catch (const std::exception & e) {
    static int said = 0;
    if (said++ < 5) {
      std::fprintf(stderr, "ros2_graph: publication %d: publish: %s\n", i, e.what());
    }
    return -1;
  }
}

int wait_any(int64_t timeout_ns)
{
  try {
    exec->spin_once(std::chrono::nanoseconds(timeout_ns));
    exec->spin_some();  // whatever else has come meanwhile
  } catch (const std::exception & e) {
    std::fprintf(stderr, "ros2_graph: %s\n", e.what());
    return -1;
  }
  return 0;
}

void close_all()
{
  subs.clear();
  pubs.clear();
  exec.reset();
  node.reset();
  if (rclcpp::ok()) {
    rclcpp::shutdown();
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
  static graph_system sys;
  sys.name = "ros2";
  sys.version = version;
  sys.open = open_all;
  sys.pub_buffer = pub_buffer;
  sys.pub_send = pub_send;
  sys.wait = wait_any;
  sys.close = close_all;
  return graph_main(argc, argv, &sys);
}
