// SPDX-License-Identifier: GPL-3.0-or-later
// bench/pairs/moos_pair.cpp: MOOS as a node pair for bench/pairs (README.md there).
// MOOS is what many marine vehicles pass messages with, and it is a different shape
// from every other system here: a broker. Every process is a client of one MOOSDB
// and talks to it over TCP; a message goes from its publisher to the database and
// from the database to each subscriber: two hops, and the database's work between.
//
//   moos_pair --role pub|sub ... [-- --port N] [-- --host NAME]
//
// The client is MOOS V10's asynchronous one (MOOS::MOOSAsyncCommClient): a notify is
// handed to the client's own writing thread and sent at once, and the database pushes
// mail to its clients as it comes, where the older client asked for it at a fixed
// rate. The subscriber takes its messages the V10 way, through an active queue: the
// client runs the callback in a thread of that queue. The callback's first statement
// reads the clock; the message and that reading are then handed to the thread the
// harness waits in. The message is a binary notify, as it is: no MOOS type.
//
// The MOOSDB is a helper in the registry: started before the pair, counted with the
// system. The driver starts one for each run, on the port of --port.
//
// MOOS has no notion of a publisher that has gone: the publisher notifies a variable
// of its own when it closes. That is the harness's, not MOOS's.
#include "pair.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
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
  uint64_t now;
  std::vector<unsigned char> data;
};

MOOS::MOOSAsyncCommClient comms;
std::mutex mu;
std::condition_variable cv;
std::deque<Item> inbox;
bool gone = false, waited_after_gone = false;
std::string var, gone_var, host = "localhost";
int port = 9000;

bool on_message(CMOOSMsg & m, void *)
{
  uint64_t now = pair_now_ns();
  Item it;
  it.now = now;
  m.GetBinaryData(it.data);
  {
    std::lock_guard<std::mutex> lock(mu);
    inbox.push_back(std::move(it));
  }
  cv.notify_one();
  return true;
}

bool on_gone(CMOOSMsg &, void *)
{
  {
    std::lock_guard<std::mutex> lock(mu);
    gone = true;
  }
  cv.notify_one();
  return true;
}

bool setup(const pair_opts * o)
{
  for (int i = 0; i < o->argc; i++) {
    std::string a = o->argv[i];
    if (a == "--port" && i + 1 < o->argc) {
      port = atoi(o->argv[++i]);
    } else if (a == "--host" && i + 1 < o->argc) {
      host = o->argv[++i];
    } else {
      fprintf(stderr, "moos_pair: unknown option %s\n", a.c_str());
      return false;
    }
  }
  var = std::string("PAIR_") + o->topic;
  gone_var = var + "_GONE";
  comms.SetQuiet(true);
  return true;
}

bool connect(const pair_opts * o)
{
  std::string name = std::string("pair_") + o->role + std::to_string(o->id) + "_" + o->topic;
  if (!comms.Run(host, port, name) || !comms.WaitUntilConnected(10000)) {
    fprintf(stderr, "moos_pair: no MOOSDB at %s:%d within 10 s\n", host.c_str(), port);
    return false;
  }
  return true;
}

int pub_open(const pair_opts * o)
{
  return setup(o) && connect(o) ? 0 : -1;
}

int pub_send(void * msg, size_t n)
{
  return comms.Notify(var, msg, static_cast<unsigned int>(n)) ? 0 : -1;
}

void pub_close()
{
  comms.Notify(gone_var, std::string("gone"));
  comms.Flush();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  comms.Close();
}

int sub_open(const pair_opts * o)
{
  if (!setup(o)) {
    return -1;
  }
  comms.AddMessageRouteToActiveQueue("pair", var, on_message, nullptr);
  comms.AddMessageRouteToActiveQueue("gone", gone_var, on_gone, nullptr);
  if (!connect(o)) {
    return -1;
  }
  return comms.Register(var, 0.0) && comms.Register(gone_var, 0.0) ? 0 : -1;
}

long sub_recv(const void **, int timeout_ms)
{
  Item it;
  {
    std::unique_lock<std::mutex> lock(mu);
    if (inbox.empty() && timeout_ms > 0) {
      cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] {return !inbox.empty() || gone;});
    }
    if (inbox.empty() && gone && !waited_after_gone) {
      // "Gone" comes through a queue of its own and may overtake the last message:
      // once, a little longer for what is still on its way.
      waited_after_gone = true;
      cv.wait_for(lock, std::chrono::milliseconds(500), [] {return !inbox.empty();});
    }
    if (inbox.empty()) {
      return gone ? -1 : 0;
    }
    it = std::move(inbox.front());
    inbox.pop_front();
  }
  pair_deliver_at(it.data.data(), static_cast<long>(it.data.size()), it.now);
  return 0;
}

void sub_close()
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
  static pair_system sys;
  sys.name = "moos";
  sys.version = version;
  sys.pub_open = pub_open;
  sys.pub_send = pub_send;
  sys.pub_close = pub_close;
  sys.sub_open = sub_open;
  sys.sub_recv = sub_recv;
  sys.sub_close = sub_close;
  return pair_main(argc, argv, &sys);
}
