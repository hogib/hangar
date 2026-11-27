// SPDX-License-Identifier: GPL-3.0-or-later
// bench/pairs/moos_graph.cpp: a module of a graph, in MOOS (graph.h; README.md, "A
// vehicle's graph"). One MOOS V10 asynchronous client a process, all of them clients
// of the one MOOSDB the driver starts as a helper: a variable a topic, a notify a
// publication, and an active queue whose callback takes in every subscription's mail,
// reads the clock as its first statement and hands the message to the module's thread,
// which waits on a condition variable with a timeout at its next publication.
//
//   moos_graph --module NAME ... [-- --port N] [-- --host NAME]
#include "graph.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "MOOS/libMOOS/Comms/MOOSAsyncCommClient.h"
#include "MOOS/libMOOS/MOOSVersion.h"

#ifndef MOOS_TAG
#define MOOS_TAG "unknown"
#endif

namespace
{

struct Item
{
  int index;
  uint64_t now;
  std::vector<unsigned char> data;
};

const graph_opts * O;
MOOS::MOOSAsyncCommClient comms;
std::mutex mu;
std::condition_variable cv;
std::deque<Item> inbox;
std::vector<std::string> pub_vars;
std::map<std::string, int> index_of;  // a subscription's variable to its index

std::string var_of(const char * topic)
{
  std::string s = std::string("G_") + topic;
  for (auto & c : s) {
    if (c == '.' || c == '-') {
      c = '_';
    }
  }
  return s;
}

// One active queue for all the module's subscriptions, so one thread and their order
// kept; which subscription a message is of is its variable's name.
bool on_message(CMOOSMsg & m, void *)
{
  uint64_t now = pair_now_ns();
  auto which = index_of.find(m.GetKey());
  if (which == index_of.end()) {
    return true;
  }
  Item it;
  it.index = which->second;
  it.now = now;
  m.GetBinaryData(it.data);
  {
    std::lock_guard<std::mutex> lock(mu);
    inbox.push_back(std::move(it));
  }
  cv.notify_one();
  return true;
}

int open_all(const graph_opts * o)
{
  std::string host = "localhost";
  int port = 9000;
  O = o;
  for (int i = 0; i < o->argc; i++) {
    std::string a = o->argv[i];
    if (a == "--port" && i + 1 < o->argc) {
      port = atoi(o->argv[++i]);
    } else if (a == "--host" && i + 1 < o->argc) {
      host = o->argv[++i];
    } else {
      fprintf(stderr, "moos_graph: unknown option %s\n", a.c_str());
      return -1;
    }
  }
  comms.SetQuiet(true);
  for (int i = 0; i < o->npub; i++) {
    pub_vars.push_back(var_of(o->pub[i].topic));
  }
  for (int i = 0; i < o->nsub; i++) {
    index_of[var_of(o->sub[i].topic)] = i;
    comms.AddMessageRouteToActiveQueue("in", var_of(o->sub[i].topic), on_message, nullptr);
  }
  std::string name = std::string("g_") + o->prefix + "_" + o->module;
  if (!comms.Run(host, port, name) || !comms.WaitUntilConnected(10000)) {
    fprintf(stderr, "moos_graph: no MOOSDB at %s:%d within 10 s\n", host.c_str(), port);
    return -1;
  }
  for (int i = 0; i < o->nsub; i++) {
    if (!comms.Register(var_of(o->sub[i].topic), 0.0)) {
      fprintf(stderr, "moos_graph: could not register for %s\n", o->sub[i].name);
      return -1;
    }
  }
  return 0;
}

int pub_send(int i, void * msg, size_t n)
{
  return comms.Notify(pub_vars[i], msg, static_cast<unsigned int>(n)) ? 0 : -1;
}

int wait_any(int64_t timeout_ns)
{
  if (!O->nsub) {
    graph_sleep(timeout_ns);
    return 0;
  }
  std::deque<Item> got;
  {
    std::unique_lock<std::mutex> lock(mu);
    if (inbox.empty()) {
      cv.wait_for(lock, std::chrono::nanoseconds(timeout_ns), [] {return !inbox.empty();});
    }
    got.swap(inbox);
  }
  for (auto & it : got) {
    graph_deliver_at(it.index, it.data.data(), static_cast<long>(it.data.size()), it.now);
  }
  return 0;
}

void close_all()
{
  comms.Close();
}

const char * version()
{
  // The tag the source was taken at, and what the library calls itself (they differ).
  return "core-moos " MOOS_TAG " (the library says " MOOS_VERSION_NUMBER "), MOOSAsyncCommClient";
}

}  // namespace

int main(int argc, char ** argv)
{
  static graph_system sys;
  sys.name = "moos";
  sys.version = version;
  sys.open = open_all;
  sys.pub_send = pub_send;
  sys.wait = wait_any;
  sys.close = close_all;
  return graph_main(argc, argv, &sys);
}
