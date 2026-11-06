# hangar: overview

hangar is a module system for remotely operated underwater vehicles, native to Linux
and small enough for the vehicle's own computer. Every function of the vehicle (a
sensor driver, a camera, a controller, the tether link, a neural network) is a
**module**: its own process, doing its own job. One **manager**, `hangard`, starts the
modules, watches them, restarts them when they fail, and knows how they are wired.

It does what a robot needs from ROS, and is built for one vehicle and its topside,
not for a robot fleet: no DDS, no runtime type system, no daemon between publisher
and subscriber. If it is not much lighter than ROS 2 on the same hardware, it has no
reason to exist; Section [Budgets](#budgets) makes that a test.

## Three planes

| Plane | What | Doc |
|---|---|---|
| **Data** | **Topics**: one shared-memory ring per output, written by one module, read by any. The writer never waits for a reader. No process sits in the path. | [01-topics](01-topics.md) |
| **Control** | **The manager** starts, supervises, restarts and configures modules. It moves no data. If it crashes or is re-executed for an upgrade, the modules keep running and keep exchanging data. | [03-manager](03-manager.md) |
| **Calls** | Request and reply ("arm", "start recording", "set parameter") over Unix sockets, straight to the serving module. The manager owns the listening sockets and is in no call's path. | [04-calls](04-calls.md) |

A module declares, in its **manifest**, what it publishes and what it reads. The
manager therefore knows the whole graph, and every published message records which
input messages it was computed from (**provenance**). The record of those
input→output pairs, time-aligned across the vehicle, is the dataset that higher-level
neural networks learn from ([06-recorder](06-recorder.md)), and not only after the
dive: the same samples stream live to a trainer on the topside GPU, and what it learns
is deployed back to the vehicle while it runs ([09-learning](09-learning.md)). That
loop, from provenance-joined samples to a model running on the vehicle, is what hangar
is for; everything else exists to make it cheap and trustworthy. A neural network is
itself just another module, and so is the trainer on the topside.

The vehicle and the topside each run their own manager and modules; `Role=vehicle` or
`Role=topside` in each machine's `hangard.conf` says which it is. The tether bridge
joins them: it mirrors chosen topics and forwards calls ([08-bridge](08-bridge.md)).
Module and topic names are unique across the vehicle and the topside together, so a
name means one thing on either side; the manager's and the bridge's own topics carry
the machine's role (`hangar.vehicle.system`, `bridge.topside.link`). The two bridges
compare their module and topic lists when they connect and refuse to mirror a name
that exists on both.

## Principles

1. **The writer never waits.** No reader, however slow, stuck or crashed, can delay a
   producer. This is what lets a recorder or a network read everything for free.
2. **Nothing between publisher and subscriber.** Data moves by shared memory; no
   broker, no serialization, no copy beyond the one into the ring and the one out.
3. **Fixed sizes, allocated up front.** Messages have a fixed layout; rings are sized
   at start and never grow. A message is one row of a tensor with no decoding step.
4. **One writer per topic.** Two sources that must merge go through a mux module.
5. **Failure is local and visible.** A crashed module is restarted alone; a module
   that is alive but no longer publishes at its declared rate, or misses its watchdog,
   is treated as failed; anything that gives up says why in one line. The bridge is
   judged by its own watchdog and `bridge.<role>.link`, not by the topics it receives,
   which go quiet when the tether does.
6. **Documented bytes.** Every file format is specified to the byte, so a reader can
   be written in any language. Python needs `mmap` and `numpy`.
7. **Static binaries, no runtime dependencies.** The build produces programs that run
   on the target with nothing installed.

## Platform

| | |
|---|---|
| Language | **Open: C11 or Rust**, decided before any code is written. The file formats, protocols and byte layouts in these docs are the contract and do not depend on it; the API sketches are written in C for illustration only. Python for tools and training only, never required on the vehicle |
| Kernel | Linux 5.16 or later (`futex_waitv`, `pidfd`) |
| Targets | AArch64 (primary: Raspberry Pi 4/5, 64-bit Pi OS), ARMv7 with NEON, x86-64. Little-endian only |
| Build | Static release binaries linked against musl, not glibc: a static glibc program alone is about 770 KB, past the manager's budget, and glibc's user and group lookups load shared libraries at run time. Users and groups are read from `/etc/passwd` and `/etc/group` directly. Tests run on the ARM targets under full-system emulation (`qemu-system`) or on a Pi: QEMU's user-mode emulation implements neither `futex_waitv` nor `clone3` |
| License | GPL-3.0. Code carried over from electric-eye keeps its MIT notices; libx264, which the video encoder links, is GPL |
| Real time | Not required. Manifests carry scheduling fields so a module can run `SCHED_FIFO` on a PREEMPT_RT kernel when control loops move into hangar |

## Budgets

Each budget is a test in CI, measured on the release build; a change that breaks one
fails the build. The manager binary's budget is settled together with the language:
a Rust program that only prints a line, built with its standard library and every
size option set, is 288 KB dynamically linked, before a static libc is added; a C
program that calls everything the manager needs from libc is 54 KB, static with musl,
on AArch64.

| What | Budget |
|---|---|
| Manager binary (static, stripped) | under 200 KB |
| Manager resident memory, 20 modules | under 2 MB |
| Manager CPU when nothing changes | no wake-ups other than its own timers (event-driven: `epoll`, `pidfd`, `signalfd`, `timerfd`) |
| Module library | no threads (except one helper in a module that asks to wait on file descriptors too, `hg_wait_fd`), no allocation after opening a topic, no serialization |
| Publish a message of up to 256 bytes, no reader waiting | under 1 µs on a Pi 4, no system call |
| Read the newest message of up to 256 bytes | under 0.5 µs on a Pi 4, no system call |
| Publish → a waiting reader wakes | measured and reported (one futex wake) |
| Video, frame topic to frame topic through `encoder` and `video_relay` over loopback | no worse than electric-eye v0.3.1: 3.8 ms at 640×480, 8.3 ms at 1280×720 (median, 30 frames/s) |

**The baseline is measured, not quoted**: a ROS 2 talker/listener pair with the
default middleware, on the same Pi, for the same message sizes and rates, reporting
resident memory, CPU at idle and at rate, and latency. The comparison is reproduced
by a script in the repository.

## Non-goals

- Multi-robot fleets, wide-area networking, discovery beyond one link.
- Several writers on one topic.
- Variable-length messages (strings and arrays have fixed capacities).
- Containers. Modules are processes; isolation uses users and permissions.
- Big-endian machines, and anything not Linux.
- Replacing ArduSub in the first version: it runs as a supervised module. Only the
  vehicle's `mavlink` module and its Neutral command talk to it, over signed MAVLink;
  messages from the ground station reach it only through that module's allow-list
  ([10-mavlink](10-mavlink.md)).

## Documents

| # | Doc | Status |
|---|---|---|
| 00 | Overview and budgets | agreed |
| 01 | [Topics](01-topics.md) | agreed |
| 02 | [Module manifest](02-manifest.md) | agreed |
| 03 | [Manager](03-manager.md) | agreed |
| 04 | [Calls](04-calls.md) | agreed |
| 05 | [Parameters](05-parameters.md) | agreed |
| 06 | [Recorder, provenance and datasets](06-recorder.md) | agreed |
| 07 | [Safety](07-safety.md) | agreed |
| 08 | [Tether bridge and discovery](08-bridge.md) | agreed |
| 09 | [Learning loop](09-learning.md) | agreed |
| 10 | [MAVLink and ArduSub](10-mavlink.md) | agreed |
| 11 | [electric-eye in hangar](11-electric-eye.md) | agreed |
| 12 | [Tools](12-tools.md) | agreed |

hangar descends from electric-eye (`eeye` and `hellion`), whose frame bus, child
process handling, discovery, status file and transactional configuration are the
starting points of the topic transport, the manager, the tether bridge and the
parameter store.
