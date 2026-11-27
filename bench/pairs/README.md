# bench/pairs: every system on equal terms

One publisher and one or more subscribers of each system, through the same cells, started
and measured by the same code. `driver.py` runs a **plan** (the cells) over a **registry**
(the systems) and writes a results folder; `driver.py --report` prints the tables from
one folder or from several sessions pooled.

This file is the protocol. A figure from here is quoted with the plan it came from, and
a reader who doubts one should find below what was done and what was not.

```sh
make pairs                                    # hangar_pair, null_pair and unix_pair, in builddir/
bench/pairs/driver.py --plan bench/pairs/plans/smoke.json --var bin=$PWD/builddir \
    --var base=$PWD/builddir \
    --out /tmp/pairs-smoke --no-preflight     # a minute a system: the harness proved, no number
bench/pairs/driver.py --plan bench/pairs/plans/main.json --estimate    # what a plan takes
bench/pairs/driver.py --plan bench/pairs/plans/main.json --var bin=$HOME/hangar-root/bin \
    --out ~/hangar-results/<date>-pairs       # the measurement (on the Pi, and which plan: below)
bench/pairs/driver.py --report ~/hangar-results/*-pairs > pooled.md
```

`make check-pairs` (part of `make check`) runs a plan of four cells over hangar, the
null system and the Unix sockets and checks the counting, the records and the report: 58 checks, and no
latency compared with anything.

## What is the same for every system

Everything a number depends on lives in `pair.h`, which every system's node includes,
or in the plan. A system's own code is how it opens, sends and receives.

**The message.** `size` bytes: three 64-bit words and a fill.

| word | what |
|---|---|
| `seq` | 1, 2, 3, ... |
| `publish_ns` | `CLOCK_MONOTONIC`, read immediately before the system's send call |
| `intended_ns` | the instant of the publisher's schedule the message belongs to |

Every word of the fill holds `seq`. The smallest size is 32 bytes.

**The publisher** sends `rate × (warmup + seconds)` messages on an absolute schedule:
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` to each instant, timer slack 1 ns. It
never skips an instant: a publisher that falls behind sends late, and the lateness is in
the message (`intended_ns`), where it cannot be hidden.

**The subscriber** reads the clock once, as soon as the system has handed it the
message, and takes two samples from that one reading:

| sample | from | to | what it answers |
|---|---|---|---|
| **call** | `publish_ns` | the reader has the message | what the transport costs. The headline |
| **intended** | `intended_ns` | the same reading | what a consumer of a periodic stream sees, with the publisher's own lateness in it |

Both are recorded for every message and both are in the report. "Has the message" is
the return of the system's receive call or the first statement of its callback: the
bytes are the subscriber's to read. Reading them is not in the sample: the fill's first
and last word are checked afterwards (`--check-all`, a plan's `check_all`: every word).

**Warm-up.** The first `warmup_s` seconds of messages are sent and received and not
counted: discovery, page faults and caches settle there. A message lost in the warm-up
is not a lost message. After it, every message counts.

**Counting.** A subscriber keeps a bit a message of the measured part: `unique`,
`lost` (`expected − unique`), `duplicated` (a seq seen again), `out_of_order` (a new seq
below the highest seen), `corrupt` (a wrong length or fill), `discarded` (a view the
system said was overwritten while it was read; it is then lost too). All of them are
reported for every system; none is ever dropped from a table because it is zero
somewhere.

**The end of a run.** The last seq, or 5 s after the run should have been over (the
subscriber then says `timed_out`, and the driver marks the run failed), or the system
saying the publisher is gone.

## The cells

A cell is **size × rate × subscribers × condition**, and a plan's sweeps are data:

```json
{"name": "fan-out", "size": [256], "rate": [1000], "subscribers": [1, 2, 4, 8], "condition": ["quiet"]}
```

`plans/full.json` has a size sweep (32 B to 1 MB at 100 Hz), a rate sweep (10 Hz to
10 kHz at 256 B), 1, 2, 4 and 8 subscribers, a camera's frames (1 MB at 30 Hz), the
three conditions, start-up, and the vehicle's whole graph under each condition ("A
vehicle's graph" below). A cell that two sweeps name is run once.

**Conditions** are the plan's too:

| condition | what |
|---|---|
| `quiet` | nothing beside the system but what the machine does by itself |
| `load2` | `"spinners": 2`: two processes that never sleep, left to the scheduler over every allowed CPU, the pair's included |
| `realtime` | `"realtime": {"policy": "fifo", "priority": 50}`: every process of the system, helpers included, under that policy. Run only where the user may have it (`RLIMIT_RTPRIO`, or as docs/13 says); elsewhere the cell is recorded as not run, with that reason |

**Pinning.** The publisher on one CPU, the subscribers on the plan's subscriber CPUs
(by default the two highest allowed; with more subscribers than subscriber CPUs they
take them in turn), helpers and the driver on the rest. The same for every system, and
in the results.

**A start-up cell** is the same for every system: with the publisher running, a
subscriber is started and the time to its first message taken, from just before the
process is created (so the process's own start is in it, the same for all); then a
subscriber that has been receiving is killed with SIGKILL and another started, and the
time to its first message taken again (`rejoin`). `open` (to the system's subscribe call
returning) is recorded beside both.

## A vehicle's graph

A pair is one topic. A vehicle is some processes with several publications and several
subscriptions each, at different rates, all running at once, and what a system costs
there is not the sum of its pairs. A **graph cell** runs that:

```json
{"name": "vehicle", "cell": "graph", "graph": "rov-vehicle", "seconds": 120, "condition": ["quiet", "load2", "realtime"]}
```

**The graph is data**, `graphs/rov-vehicle.json`: the vehicle's role of the lab
BlueROV2, read from hangar's own profile (`profiles/rov-vehicle`, with the bridge, the
serial line and the encoder). Eight processes (the manager, `mavlink`, `mux`, `safety`,
`bridge`, `encoder`, `recorder`, `console`), 16 topics, 53 subscriptions; 474 messages a
second published and 1406 delivered. Its `rules` say how it was read from the profile,
and they are the first thing to question:

- a process a module, the manager included;
- only topics with a rate (event topics carry nothing while a vehicle just runs, and
  calls are not topics here);
- a size is the type's payload rounded up to a power of two, 16 bytes at least, so that
  every system carries the same sizes and ROS 2 has one fixed-size type a size;
- `recorder` and `console` subscribe to everything;
- the payload is the harness's (a count and a stamp), not the type's fields: what is
  measured is the transport of messages of these sizes at these rates between these
  processes, not what a module computes;
- a topic's `inputs` are its manifest's `Inputs=`: the topics a message of it is
  computed from, which hangar records a reference to in each message
  (`ardusub.manual_control` from `ctl.wrench`, `ardusub.state` from `safety.state`;
  no other topic of this profile declares any). A system that records no such thing
  ignores them.

hangar's own modules do more than carry messages: they record those references and
check staleness against the declared rates. The `hangar` row leaves that out, as the
other systems' rows have nothing like it; **`hangar-module` has it, and is the row for
what the vehicle runs** ("hangar's own rows", below). Calls and the recorder are in no
row.

**The module** is `graph.h`, the same for every system, as `pair.h` is for a pair: one
thread that waits for any of its inputs with a timeout at its next publication, counts
what has come, and sends what is due. A system's node (`hangar_graph.c`,
`unix_graph.c`, `iceoryx2_graph.c`, `ros2_pair/src/ros2_graph.cpp`, `lcm_graph.c`,
`moos_graph.cpp`) is how it opens its
publications and subscriptions, sends, and waits for several things at once: `hg_wait`
over all subscriptions; `ppoll`; a wait set with every listener attached; an executor;
`lcm_handle_timeout`; a condition variable the client's thread signals.
The waiting may return late by what the system's call can express (`hg_wait` takes
milliseconds), and a publication is then late by that, as a real module's is; every
message carries the time it was really sent, so the latency is not touched.

**A run**: the helpers, then one process a module, all left to the scheduler over every
allowed CPU (a vehicle's modules are not pinned). The measured part begins a warm-up
after the last was started, at an instant every process is given, and lasts the cell's
`seconds`; a sample is a delivery's clock reading minus the stamp taken immediately
before its send. Per topic the samples of all its subscribers are one distribution.
**Cost** is every process and helper together, as for a pair.

**Counting.** A subscription counts a gap between two measured messages as missing and
a count not above the one before as out of order. A message missing at the very edge of
the measured part is not seen by this; `Deliveries measured` beside the rates says
whether a subscription came short.

**Which entries run it, and why not the others** (the report lists each with its
reason): `hangar`, `hangar-inplace`, `hangar-module`, `hangar-module-refs4`,
`unix-seqpacket`, `iceoryx2`, `lcm`, `lcm-rcvbuf`, `moos` and the seven ROS 2 entries
that are two-process systems.
A run in which a system failed publications is flagged `publications-failed`, carries
the mark † in every table, and is listed with the count.

| not run | why |
|---|---|
| `null` | not a system |
| `unix-stream` | its writer waits for its reader, and a module that waits for the slowest of its readers is not what a vehicle's module may be |
| `iceoryx2-poll` | a polling module spins a core for the whole run: eight modules want eight cores of a computer that has four, and the row would measure the shortage |
| `ros2-fastdds-waitset` | the graph's ROS 2 node is a node as they are written, with an executor |
| `ros2-fastdds-composed` | one process is not a graph of processes |

**What is not like for like in this cell.** A ROS 2 process is a node with its own
discovery and its own threads, which is what ROS 2 costs for eight processes, and is in
the row. Fast DDS and Cyclone DDS discover one another between all eight; iceoryx2
creates two services a topic (the data and the event that wakes); hangar's `hg_wait` and
the others' waits differ in what a timeout can express. The ROS 2 entries with loaned
messages lend for every size here, small ones included, where a deployment would lend
only its large messages.

**Percentiles.** The same rule: only where a run has the samples. A topic at 1 Hz has
120 deliveries a subscriber in the full plan's two minutes, and shows its largest
sample only; the topics at 50 Hz have the samples for a 99th percentile.

## One run, one round

A run is one system in one cell: the condition's load, the system's **helpers** (a
broker, a daemon, a router: started first, stopped last), the subscribers, each waited
for until it says it is ready, then the publisher. A fresh directory in `/dev/shm` and a
fresh topic name per run.

A **round** is every cell once and, in a cell, every system once. The cells' order is
shuffled per round and so is the systems' order within each cell, from a seed that is
recorded (or given: `--seed`). No system is measured all at once, first or last: what
drifts over a session (temperature, mostly) falls on all of them alike, and two systems'
runs of the same cell in the same round are minutes apart, not hours.

Before each round the driver runs the **preflight's measurement check**
(`tools/rov/preflight.py`: frozen, the package system, temperature, throttling,
governor, other load) and keeps its report as `preflight-round-N.txt`. A `FAIL` stops
the session there (`--ignore-preflight` goes on; the verdict stays in the results).

Before each run it waits for the machine to be **below the plan's `max_temp_c`** (60 °C
in the full plan: the Pi with its heat sink reads 50 to 62 in every real state, and
its soft limit is 80), up to `cool_wait_s`. A run during which the Pi **throttled** is
kept, marked as repeated, and run again, up to `retries` times; one that never ran clean
keeps its last attempt, marked. The report lists every repeated, flagged, failed and
not-run cell; none is dropped silently.

## Statistics

**Per run**, the whole distribution of both samples is kept, as a histogram with 32
bins to the octave (a bin is at most 3.1 % wide) with its count, its exact smallest and
largest sample and its sum. Any percentile and the ECDF can be computed from
`results.json` afterwards. A percentile read from it is the upper edge of the bin, so
it is never an underestimate, and never above the largest sample.

**A percentile is printed only when a run has the samples for it**: 100 beyond the
percentile, so 200 for the median, 10⁴ for p99, 10⁵ for p99.9. A run's length is sized
from its rate for that (`target_samples`), between the plan's `seconds` and
`max_seconds`: at 10 kHz, 20 s; at 1 kHz, 100 s; at 100 Hz the bound of 120 s gives
12 000 samples, enough for p99 and not for p99.9. Where no single run has enough, the
report takes the percentile over the pooled samples of all the cell's runs and marks it
‡; where even those are too few it prints `n/a` and says how many there were. The
largest sample of a run is printed as what it is, a maximum of n, not an estimate of
anything.

**Across repetitions**: the median of the runs' figures and a 95 % bootstrap interval
of that median (the runs resampled with replacement, 2000 times, seeded), not a range.
Ten repetitions in the full plan. Fewer than three runs: no interval, and `n` is shown.

**Across sessions.** Every run carries its session: the date and the boot id. The
report pools several results folders: the tables are over all runs, and a further table
gives, for the headline figure, the spread within a session beside the spread between
the sessions' medians. They are different things and are not added: the first is what
ten runs in one boot differ by, the second what a reboot, another day and another
ambient temperature add.

## Cost

**CPU**: user plus system time from `/proc/<pid>/stat` of **every process of the
system**, helpers included, between two readings by the driver: half a second into the
measured part and half a second before its end (the interval is in the results). One
tick is 10 ms, so over 20 s the resolution is 0.05 % of a core.

**Memory**: PSS from `/proc/<pid>/smaps_rollup` at the second reading, for every process
the driver's user may read it for (its own processes: no root needed), RSS from
`/proc/<pid>/statm` for one it may not. The report says which was used; RSS counts
shared pages in full and is not comparable with PSS.

## The harness's own floor

`null` is a system of its own: the publisher's send keeps a pointer, the subscriber's
receive returns it, in one thread. Its `call` row is what the two clock readings and
the bookkeeping between them cost; its `intended` row is the schedule's own lateness
(the wake-up after `clock_nanosleep`). No system can read below it, and a difference
between two systems that is of its size is not a difference.

## A cross-check against performance_test

The harness is ours, and so is every figure from it. One cell is therefore measured a
second time by a tool that is not: `performance_test` (Apex.AI's, the one the ROS 2
project measures its middlewares with; the package `ros-jazzy-performance-test`,
2.3.0). `crosscheck.sh` runs it and `ros2_pair_node` in turn, five rounds of 20 s each,
on the same two CPUs: 256 bytes at 1 kHz, a publisher process and a subscriber
process, Fast DDS as it comes, reliable, keep last 16 in both.

What can be compared is **the mean of publish call to callback**: performance_test
stamps its message before `publish()`, as `pair.h` does, and reports a mean, a
smallest and a largest for each second, and no percentile. Its message is its own
type (`Array256`: the 256 bytes, a time stamp and an id), its executor the same
single-threaded one.

Where it was run (2026-11-28): in the container on the development laptop, x86-64,
nothing isolated or frozen. **The figures are not results**; what the run says is
whether two tools agree on one machine in one quarter of an hour:

| | median of five rounds' means | the five means | smallest sample | lost |
|---|---|---|---|---|
| performance_test | 57.3 µs | 55.3 to 57.8 µs | 30.0 to 31.9 µs | 0 of 100 011 |
| `ros2_pair_node` | 57.6 µs | 54.0 to 61.5 µs | 27.6 to 30.0 µs | 0 of 100 000 |

They agree: the medians are 0.5 % apart, and each tool's five means lie inside or
within 4 µs of the other's (ours spread more from round to round, 7.5 µs against
2.5 µs, and five rounds do not say why). So for this cell the harness's clock readings, its
message and its counting give what an independent tool gives. What it does not show:
anything about the percentiles (performance_test has none to compare), about the
other systems (it speaks only ROS 2), or about the Pi.

**On the Pi** the same check is `ros-jazzy-performance-test` added to the `.deb` list
(`tools/baselines-debs.sh ros-jazzy-rmw-cyclonedds-cpp ros-jazzy-rmw-zenoh-cpp
ros-jazzy-performance-test`), then, frozen and with ROS 2 sourced,
`bash bench/pairs/crosscheck.sh $PWD/ros2/install/ros2_pair/lib/ros2_pair/ros2_pair_node --out ~/crosscheck-$(date +%F)`:
four minutes. It has not been run there.

## What is recorded with every session

`results.json`: the session (date, boot id, host), the machine (model, kernel, kernel
command line, CPUs, uptime), the CPUs pinned, the priority class, the plan (and
`plan.json`, the file as it was) with its hash, the seed and each round's order, each
round's preflight verdict; for each system its registry entry, its version and the
compiler and flags its node was built with (`node --version`), a helper's version, and
its configuration files copied into `config/<system>/`; for each run the governor,
temperature, throttling word and the pinned CPUs' frequencies before and after, how
long it waited to cool, how busy the other CPUs were, its flags, and what every process
printed (`logs/`).

## Adding a system

Two things: a node and a registry entry. Nothing in the driver changes.

**The node** is one source file that includes `pair.h`, fills a `struct pair_system` and
calls `pair_main` (C99 or C++11; `hangar_pair.c` is 136 lines, `null_pair.c` 34):

```c
#include "pair.h"
static int pub_open(const struct pair_opts *o) { /* o->topic, o->size, o->rate */ }
static int pub_send(void *msg, size_t n)       { /* the system's publish */ }
static long sub_recv(const void **msg, int timeout_ms) { /* length, 0 nothing yet, -1 gone */ }
...
int main(int argc, char **argv) {
    static const struct pair_system sys = { .name = "lcm", .version = version, .pub_open = pub_open, ... };
    return pair_main(argc, argv, &sys);
}
```

`pub_buffer` lends the harness the system's own slot to build the message in (zero
copy); `sub_done` lets a system say, after the harness has read a view, that it was not
a whole message. A system that delivers by callback calls `pair_deliver(bytes, length)`
as the callback's first statement: the clock is read there and the message read before
the callback returns, and `sub_recv`, inside which the system's executor ran, returns 0. Build it with
`-DPAIR_CFLAGS='"<compiler and flags>"'`, so that they are in the results.

**The entry**, in `systems.json` or in a file of its own given with `--systems`:

```json
{
  "name": "moos",
  "title": "MOOS V10, asynchronous comms, through a MOOSDB",
  "node": ["{bin}/moos_pair"],
  "args": ["--port", "9123"],
  "helpers": [{"name": "moosdb", "cmd": ["MOOSDB", "--moos_port=9123"], "ready_s": 1.0, "version": ["MOOSDB", "--version"]}],
  "config_files": ["{here}/moos.conf"],
  "env": {},
  "max_size": 65536, "max_size_why": "one datagram",
  "notes": "what is not like for like: see below"
}
```

`{bin}` and any other `{NAME}` come from `--var NAME=value`; `{rundir}` is the run's
directory in `/dev/shm`, `{topic}` its topic name, `{here}` the entry's own directory.
`pub` and `sub` in place of `node` for two programs; `"kind": "single"` for one process
(`--role both`). `"sizes"` lists the only sizes a system has (with `sizes_why`),
`"needs"` files the node loads at run time and cannot run without, `"packages"` the
installed packages whose versions belong in the results (`dpkg-query`). A system whose programs are not there is named, with its `install`
line, and skipped: nothing is ever installed from here.

**`notes` is not optional in spirit.** It is printed in the report beside the system,
and it must say what a referee would ask of this system's row:

- Is the subscriber **woken** by the publish, or does it **poll** (and then: how often,
  and what does the idle polling cost)? Both rows if the system offers both.
- What is its **delivery contract** here: may it drop for a slow reader, does the
  writer ever wait, how deep is its history? The contract nearest to hangar's (the
  newest 16, the writer never waits) is the one to configure, and the entry says what
  was configured.
- How many **copies** of the message between the two clock readings?
- A **helper** (broker, daemon, router): is the message's path through it, or is it for
  discovery only?
- What does `publish_ns` mean for it: is serialisation inside the send call (it is in
  the sample) or before it?
- Anything the system does that the others do not (encryption, a wire format across
  machines, logging), or does not that the others do.

## The systems here, and what is not like for like

| system | what it is | not like for like |
|---|---|---|
| `null` | the harness alone | not a system |
| `hangar` | lib/hg: a 16-slot ring in shared memory, the reader asleep in one futex and woken by the publish | one copy in, one copy out. A reader 16 messages behind loses the oldest; the writer never waits. No helper. The stamp is the harness's, carried in the payload like every system's (lib/hg has its own in the slot's header, not used here) |
| `hangar-inplace` | the same, the message built in a loaned slot and read as a view | no copy. A view the writer overwrote while it was read is dropped and counted, which takes a reader about the ring's length behind |
| `hangar-module` | hangar **as a vehicle's module runs it**: the process wired as the manager wires a module, the topic's rate declared, the references its manifest's Inputs call for | the transport of `hangar` and, on top, what lib/hg does for a module and the bare transport leaves out: every read says whether its topic is stale against its rate, every publish records a reference to the message last read from each of its Inputs and whether any was stale. In a pair the topic has no Inputs; in the graph, the profile's. **This is the row that says what a vehicle pays** |
| `hangar-module-refs4` | the same, every message carrying four references, the most lib/hg records | the upper bound of the provenance work, not what the profile asks for. In a pair the four are topics of the publisher's own, published once (it has read nothing): the recording is in the row, the reading of four inputs is not. In the graph, the first four subscriptions of each module |
| `unix-seqpacket` | Unix domain sockets, `SOCK_SEQPACKET`: the kernel's own IPC with no middleware. **The floor** for anything that sends through the kernel | two copies and one system call a side; the reader asleep in `recv`, woken by the kernel. The publisher never waits: a subscriber whose buffer is full does not get the message. A socket then drops the **newest** and keeps the old (hangar's ring drops the oldest), and the buffer's depth is the kernel's count of bytes (`net.core.wmem_default`, 212992 B: some hundreds of small messages), not 16. A message must fit that buffer whole: up to 131072 B here, and the larger cells are not run. No discovery beyond a path, no type, no metadata |
| `unix-stream` | the same with `SOCK_STREAM`, for any size | **another contract**: the publisher's send blocks while the reader's buffer is full, so nothing is lost and a slow reader slows the writer (it shows under "intended"). A megabyte is several send and receive calls. The reader has the message when it has collected all its bytes |
| `ros2-fastdds` | ROS 2 Jazzy with Fast DDS as installed, ROS 2's default: one rclcpp node a process | serialised into Fast DDS's shared-memory transport and out of it, inside the sample. Reliable, keep-last 16 (see below). No helper |
| `ros2-fastdds-loan` | the same with Fast DDS's data sharing and loaned messages (`ros2_pair/config/fastdds-datasharing.xml`) | no serialisation and no copy: the publisher builds the message in a sample it borrowed, the subscription is handed that sample. The nearest thing in ROS 2 to `hangar-inplace`. Whether the loan was granted is in the node's standard error (`logs/`). **In the graph its publications fail in most runs** ("Fast DDS, data sharing and a reliable writer", below) |
| `ros2-fastdds-loan-besteffort` | the same zero-copy path, best effort and keep-last 1 | **another contract** than the other ROS 2 rows (reliable, keep-last 16): a reader that is not ready when a message comes does not get it. Here because the reliable entry fails in the graph and this one does not |
| `ros2-fastdds-waitset` | Fast DDS as installed, the subscriber waiting in an `rclcpp::WaitSet` and taking the message itself | **not how a ROS 2 node is written.** Beside `ros2-fastdds` it separates what the executor costs from what the middleware costs |
| `ros2-fastdds-composed` | publisher and subscription in **one process**, rclcpp's intra-process delivery | **not the comparison**: nothing leaves the process, and one failing node takes the other with it. Its own row because the question is asked |
| `ros2-cyclonedds` | ROS 2 Jazzy with Cyclone DDS as installed | serialised, through the loopback network stack. No helper |
| `ros2-cyclonedds-iceoryx` | Cyclone DDS over iceoryx (classic) shared memory, loaned messages (`ros2_pair/config/cyclonedds-iceoryx.xml`) | no copy for a fixed-size type. Needs iceoryx's daemon **RouDi, a helper**: started before the pair, counted with the system. The messages do not pass through it; it owns the pools, whose default sizes bound how many samples can be out at once |
| `ros2-zenoh` | ROS 2 Jazzy with Zenoh (`rmw_zenoh`) as installed | not DDS. Its **router is a helper**, counted: for discovery only, the messages then go peer to peer over TCP on the loopback |
| `ros2-zenoh-shm` | the same with Zenoh's shared-memory transport on (`ZENOH_CONFIG_OVERRIDE`) | Zenoh uses it above a size threshold of its own; the small cells are as `ros2-zenoh` |
| `iceoryx2` | iceoryx2 through its C binding: samples lent from shared memory, no daemon; the subscriber **woken** by an event service beside the publish-subscribe service | the nearest system in kind, and in contract: no copy, a subscriber buffer of 16 with safe overflow (the publisher never waits, a reader that far behind loses the oldest). iceoryx2's publish-subscribe by itself wakes nobody: the publisher notifies after each send, inside the sample, and the subscriber sleeps in its listener, as hangar's sleeps in `hg_wait` |
| `iceoryx2-poll` | the same, the subscriber asking in a loop that never sleeps | **not like for like**: iceoryx2's best latency at the price of a whole core, spent whether or not anything is published. No event is sent. Beside `iceoryx2` it is what waking costs |
| `lcm` | LCM 1.5.3: no broker, no discovery; a message is a UDP multicast datagram on the loopback to a group every process has joined | lossy by design: a datagram the kernel has no room for is gone. A process is handed every topic's datagrams and drops in user space what it has not subscribed to. LCM reads its socket in a thread of its own and wakes the handling thread through a pipe: two wake-ups a message. Bytes as they are (no `lcm-gen` type, so no marshalling in the row). Above 65499 bytes a message is 65487-byte datagrams, and lost if one of them is: with the kernel's default receive buffer (212992 bytes, three of a megabyte's seventeen) **the 1 MB cells lose a large share** |
| `lcm-rcvbuf` | the same with the 2 MB receive buffer LCM's documentation asks for | what its documentation says for large messages. The kernel grants at most `net.core.rmem_max`, 212992 on a stock Ubuntu: **where root has not raised it this row is `lcm` again**, and the report prints the limit |
| `moos` | MOOS (core-moos v10.4.0), V10 asynchronous comms, every process a client of one `MOOSDB` over TCP | **a broker, the only one here**: a message goes from its publisher to the database and from the database to each subscriber, two hops by design. The `MOOSDB` is a **helper**, counted, and every message passes through it. A subscriber's callback runs in a thread of the client's and hands the message to the waiting thread: a wake-up more. TCP: nothing is dropped, what a slow reader has not taken waits for it |

Where each system's two stamps are taken is the same by construction: `publish_ns` is
read in `pair.h` immediately before the node's `pub_send`, and the subscriber's clock is
read in `pair.h` as soon as the node's `sub_recv` returns. What differs is what lies
between, and that is this table's last column.

**Choices made for the Unix sockets, and the alternatives.** The buffers are the
kernel's defaults (`-- --sndbuf N` sets the publisher's): a buffer sized to hold 16
messages would be nearer hangar's ring in depth, but the kernel accounts a small message
at several times its size, so a byte count does not give a message count, and the
default is what a program gets. `SOCK_DGRAM` would do as well as `SOCK_SEQPACKET` for one
subscriber; the connection is what lets a publisher have several and learn that one has
gone. The publisher takes new connections in before it reads the clock, outside the
sample. A baseline's node is found through `--var base=DIR` (hangar's own through
`--var bin=DIR`), since on the Pi they come from different archives.

### hangar's own rows, and which one a vehicle runs

hangar has four rows, and they are an ablation of its own features:

| row | wired | staleness | references a message | copies |
|---|---|---|---|---|
| `hangar` | no | not computed | none | in and out |
| `hangar-inplace` | no | not computed | none | none |
| `hangar-module` | yes | computed at every read and publish | those its manifest's Inputs call for | in and out |
| `hangar-module-refs4` | yes | computed | four | in and out |

- **`hangar` is the bare transport**, and the row that stands beside the other systems'
  transports: none of them records what a message was computed from or says that a
  topic is stale, and neither does this row.
- **`hangar-module` is what a vehicle runs.** A module under the manager has a wiring,
  declared rates and the Inputs of its manifest, and the library does that work on
  every read and every publish. A figure quoted as "what hangar costs on the vehicle"
  is this row's. `hangar-module` minus `hangar` is the price of the feature in the
  vehicle's own configuration; `hangar-module-refs4` minus `hangar` is the most it can
  be.
- A wired node writes its own wiring file (the text the manager would hand it) into the
  run's directory and calls `hg_init`; there is no manager in these cells. What the
  manager itself publishes (its two topics) is in the graph as the `hangard` process's
  publications; its supervision is not in any cell.
- A wired subscriber's log line (`logs/*-sub*.err`, `*-mod-*.err`) says how many
  references the messages it read carried and how many were flagged as computed from
  stale inputs, so a row said to carry references can be seen to. In the graph with
  `refs4`, where every module computes from the others, the flag stays set once a
  first message was computed before its inputs existed: the work is the same.
- Still not in any row: calls, parameters, the recorder.

### ROS 2

One node, `ros2_pair/` (an ament package: `ros2_pair_node`, five fixed-size message
types, the two shared-memory profiles), for every middleware: the entries differ in
their environment (`RMW_IMPLEMENTATION` and a profile), not in code.

- **The subscriber is a ROS 2 node as they are written**: a `SingleThreadedExecutor`
  spun until a message has come, and a subscription callback. The callback's first
  statement is the harness's `pair_deliver`, which reads the clock and reads the message
  where it lies (a loaned sample is the subscription's only for the callback's length).
  So the sample ends where `bench/ros2` ended it, at the callback's first statement, and
  everything rclcpp does between the middleware and the callback is in it, as it is in
  every ROS 2 system. `ros2-fastdds-waitset` is the same without the executor.
- **The publisher's stamp** is the harness's, read immediately before `publish()`:
  serialisation, where the middleware has one, is inside the sample.
- **QoS: reliable, keep-last 16, volatile** (`-- --qos queue`, the default here). It is
  the nearest to hangar's ring of 16 read in order: every message while the reader
  keeps up, and a keep-last history does not make the writer wait. The alternative,
  `-- --qos latest` (best effort, keep-last 1), is what a sensor stream is often given
  in ROS 2 and is what `hg_latest` means; it is not in the registry, to keep the table
  readable, and an entry for it is the same lines with that argument.
- **Sizes.** A middleware may only lend a sample of a fixed-size type, so there is one
  message type a size (32, 256, 4096, 65536 and 1048576 bytes: an array of `uint64`), and
  a cell of another size is recorded as not run for the ROS 2 entries.
- **Discovery** falls in the warm-up: the subscribers are started first and the publisher
  publishes from the moment it exists. A reliable volatile writer's messages from before
  the match are not delivered, and are not counted. The start-up cell measures it.
- **As installed and as recommended.** For each middleware one entry is what `apt`
  gives and one is what its own documentation gives for one host: data sharing with
  loans for Fast DDS, iceoryx shared memory for Cyclone DDS, the shared-memory transport
  for Zenoh. Nothing else is tuned: no socket buffers, no segment sizes, no thread
  priorities. A referee who wants a tuned row gets the file to change in
  `config/<system>/` of every results folder.
- **Isolation.** `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST` and `ROS_DOMAIN_ID=77` for
  every entry; the node starts no parameter services.
- **Does Jazzy's Cyclone DDS lend messages?** Its documentation is of two minds. The
  binary does: `librmw_cyclonedds_cpp.so` (2.2.4) links `libiceoryx_binding_c`, and with
  `SharedMemory` enabled and RouDi running both the publisher and the subscription
  report loans (checked in the x86-64 container on 2026-11-27; the Pi's own binary is to
  be checked the same way, by the node's line in `logs/`).

#### Fast DDS, data sharing and a reliable writer

`ros2-fastdds-loan` fails publications in the graph cell, in most runs. This is what was
found, so that nobody reads those runs as Fast DDS's latency or as this node's misuse.

- **What is seen.** A writer sends about 17 messages (sometimes a few more) and from
  then on every publication fails, to the end of the run: with loans the borrow is
  refused, without them `publish()` fails. Which topics it hits differs from run to run.
- **What bounds it, by Fast DDS's own account.** With data sharing "Fast DDS will
  pre-allocate a pool of `max_samples` + `extra_samples` samples", loans are taken from
  that pool, and "the samples in the preallocated pool are never reused unless they
  have been acknowledged ... If there is no reusable sample in the pool, the writing
  operation in the DataWriter will be blocked until one is available or until
  `max_blocking_time` is reached" (Fast DDS 2.14 documentation, "Data-sharing delivery":
  "Blocking reuse of samples until acknowledged"; "Zero-Copy communication"). For a
  keep-last history the pool's `max_samples` is the history depth
  (`DataWriterHistory.cpp`, `to_history_attributes`, 2.14.x), and `extra_samples` is 1
  by default: 16 + 1 = 17, the number the stalled writers stop at. A loan cannot wait:
  `DataWriterImpl::loan_sample` returns `RETCODE_OUT_OF_RESOURCES` when the pool has
  no free sample.
- **What it depends on, by experiment** (x86-64, the container of `bench/ros2`, Fast DDS
  2.14.7, 2026-11-28; `ros2_pair/repro-datasharing.sh` is the smallest case):

  | case | stalled |
  |---|---|
  | one writer, 1 or 5 readers started before it; a reader stopped for 3 s; a reader killed; five readers started together 1.5 s after it | 0 of 1 each |
  | one writer, seven readers joining 0.15 s apart while it publishes; reliable, loaned | 2 runs of 6 |
  | the same, reliable, **not loaned** | 3 runs of 8 |
  | the same, **best effort**, loaned | 0 runs of 8 |
  | the graph: reliable, loaned | 5 runs of 6 |
  | the graph: reliable, loaned, `extra_samples` 64 | 3 runs of 6, each stopping at 80 to 89 messages (16 + 64 + 1) |
  | the graph: reliable, not loaned; reliable, loaned, subscribers copying | 6 of 6; 6 of 6 |
  | the graph: best effort, loaned | 0 of 6 |

- **What follows.** It is not the loan, and not how this node takes or returns one: it
  happens without loans. It needs a reliable writer and readers that join while it
  publishes, which is what a system coming up is. `extra_samples`, which the
  documentation offers against blocking, only moves the point at which the pool is
  empty. Why the pool is not refilled was not found; a reader that had read everything
  and is then stopped does not cause it. In a pair cell it has not been seen in the
  driver (the subscribers are up before the publisher is started). One early 1 MB run
  of the pair lost 3 of 60 with a mean of 45 ms; thirty repetitions of 1 MB at 30 Hz with
  one reader (2026-11-28) lost nothing and failed nothing, so that one is not explained
  and not reproduced.
- **What the registry does about it.** `ros2-fastdds-loan` stays as it is, reliable,
  like every other ROS 2 row, and its failed publications are counted and printed; a
  run of it with failed publications is not quoted as a latency. Beside it,
  `ros2-fastdds-loan-besteffort` is the same zero-copy path with best effort and
  keep-last 1, which did not fail: another contract (a reader that is not ready does
  not get the message), said in its row, and the Fast DDS zero-copy configuration
  that does run the vehicle's graph. `ros2-fastdds`, without data sharing, is reliable
  and has not failed.

Running it: with ROS 2 Jazzy and the built package sourced,

```sh
colcon build --base-paths bench/pairs/ros2_pair --cmake-args -DCMAKE_BUILD_TYPE=Release
source install/setup.bash
bench/pairs/driver.py ... --var ros=$PWD/install/ros2_pair/lib/ros2_pair --var ros_root=/opt/ros/jazzy
```

On a machine without ROS 2 the same in the container of `bench/ros2/Dockerfile`
(`tools/baselines.sh ros2-smoke` does it): a proof of the harness, never a measurement.
An entry whose middleware is not installed is named and skipped (`needs`).

### iceoryx2

`iceoryx2_pair.c`, on iceoryx2's C binding (a static library built from its source by
`tools/baselines.sh`; version and hash are pinned there and in the archive's
`MANIFEST`).

- **Woken or polling.** iceoryx2's publish-subscribe does not wake a subscriber; its
  documentation and examples pair it with an event service for that. `iceoryx2` does
  so and is the like-for-like row. `iceoryx2-poll` is the row its authors' own
  benchmarks correspond to: the lowest latency, with a subscriber that never sleeps.
  Polling on a timer, the third way, is not run: its latency is the timer's period and
  says nothing of the transport.
- **The buffer is 16**, to match hangar's ring, where iceoryx2's default is 2;
  `-- --buffer 2` gives the default. Safe overflow is iceoryx2's default and is kept.
- **The payload** is one fixed-size value of the cell's size, aligned to 8, declared as
  such to the service; nothing is serialised.
- **The end of a run.** The publisher sends one event, "gone", when it closes, in both
  entries; a polling subscriber looks for it ten times a second, outside the samples.
- **What it leaves behind.** iceoryx2 keeps its files under `/dev/shm` and
  `/tmp/iceoryx2`, not in the run's directory. A subscriber killed in the start-up
  cell leaves its node's files until another node cleans up after it, which iceoryx2
  does when a node is created.
- **Nothing here is iceoryx2's supervision or hangar's**: no provenance, no stale flag,
  no schema in the file on iceoryx2's side; no dead-node clean-up on hangar's, which
  has a manager for it.

### LCM

`lcm_pair.c` and `lcm_graph.c`, on LCM 1.5.3's C library (built from its pinned source
by `tools/baselines.sh`, in a container, because LCM needs GLib: these two nodes are not
static programs and use the machine's own `libglib2.0-0`).

- **As it comes and as its documentation says.** `lcm` is LCM's default URL with `ttl=0`
  (the datagrams stay on the host; LCM's default of 1 would put them on the tether).
  `lcm-rcvbuf` asks for the receive buffer of 2097152 bytes that LCM's documentation
  tells a user of large messages to set ("Kernel UDP receive buffer sizing":
  `sysctl -w net.core.rmem_max=2097152`). A process may ask; only root raises the
  kernel's limit. On the Pi that is one command, and one more to put it back:
  `tools/rov/LAB-CHECKLIST.md` has both.
- **1 MB at 30 Hz, judged.** With the default buffer LCM lost 19 of 60 messages in one
  trial here, with 2 MB none (x86-64, the container, where `rmem_max` happens to be
  7.5 MB; not a measurement). The loss with the default is LCM's behaviour with its
  default, and the cell stands as it comes out; a figure for LCM at 1 MB is quoted from
  `lcm-rcvbuf` and only from a session whose report shows `rmem_max` at 2097152 or
  more. LCM is not made for megabyte messages, and its documentation does not say it is.
- **No types.** LCM is normally used with types from `lcm-gen`, which marshal each
  field; `lcm_publish` takes bytes, and the nodes use that, as every other row carries
  bytes. A real LCM system pays the marshalling on top.
- **The queue** of a subscription is 16 (LCM's default is 30), and LCM drops the newest
  when it is full.
- **The end of a run**: the publisher says "gone" on a channel of its own, three
  times; that is the harness's, LCM has no such notion.
- In the graph every process receives every topic's datagrams (one group): 474
  datagrams a second into each of eight processes, most of them dropped in user space.

### MOOS

`moos_pair.cpp` and `moos_graph.cpp`, on core-moos v10.4.0's `libMOOS`
(`MOOS::MOOSAsyncCommClient`), with core-moos's own `MOOSDB` as the helper. Built only
by `tools/baselines.sh --with-moos`.

- **Its licence is not one statement**, which is why it is built only when asked for.
  The source archive of the tag has no licence file for the whole.
  `Core/GPLCore.txt` is the GNU General Public License, "either version 2 of the
  License, or (at your option) any later version". Of its 182 source files, 102 say
  "made available under the terms of the GNU Lesser Public License v2.1" (the client
  library, which the nodes link); 14, the database's, say "the GNU Public License"
  and point at `gpl.txt`, with no version; one more is GPL; 33 are under the Boost
  licence and 2 under the MIT licence (third-party parts); 30 say nothing. The
  project's pages that a search finds (2026-11-28) state no licence for core-moos as a
  whole; older MOOS trees carry the same `GPLCore.txt`. Read plainly: an LGPL-2.1
  library and a GPL database. Linking the library into these nodes and running the
  database on one's own machines raises nothing; **an archive that contains `MOOSDB`
  is a GPL binary to whoever it is passed on to**, with the source owed (the pinned
  archive in the `MANIFEST`).
- **V10 asynchronous comms**, as MOOS's authors would have it run: the older client
  talks to the database at a fixed rate and would read as that rate.
- **A subscriber** takes its mail through an active queue, V10's way; the queue's
  callback runs in a thread of the client's. Its first statement reads the clock; the
  message and the reading go to the thread the harness waits in (`pair_deliver_at`).
- **The database** is started by the driver for each run, on port 9123, with its
  "suicide" listener off, and stopped after; its CPU and memory are in the system's
  cost. It is in the path of every message.
- **The version**: the tag is v10.4.0 (2018, the newest); the library calls itself
  10.0.2. Both are in the node's version line.
- **Binary notifies**, as they are. What MOOS does with a slow reader (it queues; no
  bound was looked for) is not verified beyond "nothing was lost in the trials".

Every baseline that was chosen is in this directory.

## Restarting a process

hangar's failure tests (`tools/rov/failtest.py`) kill a module under an armed vehicle
and time what the system does about it. No system in this directory has anything to set
beside that: they are transports, and none of them supervises a process. What a
conventional stack has is something that **restarts** a process, and `restart.py`
measures that, beside hangar's manager doing the same:

```sh
bench/pairs/restart.py --bin builddir --out ~/restart-$(date +%F)        # about 50 minutes
bench/pairs/restart.py --bin builddir --out /tmp/restart-smoke --smoke   # a minute: the script proved
```

**These rows restart a process, and that is all they compare.** None of them, hangar's
included, is asked here for what `safety` does when a module dies under an armed vehicle
(thrusters to neutral, ArduSub disarmed). systemd and ROS 2's launch have nothing that
corresponds to it: they do not know what a process publishes, whether it is stale, or
that there is a vehicle.

| row | what restarts the process |
|---|---|
| `hangard` | hangar's manager, a manifest's defaults: `Backoff=0.1s..10s`, doubling, `ResetAfter=30s`, giving up after `Burst=5/60s` |
| `hangard-fast` | the same with `Backoff=1ms..10s` |
| `systemd` | `systemd --user`, a transient service (`systemd-run`), `Restart=always`, the rest at its defaults (`RestartSec=100ms`; 5 starts in 10 s and the unit is left failed) |
| `systemd-fast` | the same with `RestartSec=0` and no start limit: the fastest setting that is still a supervisor |
| `ros2-launch` | `ros2 launch`, `ExecuteProcess(respawn=True)`, `respawn_delay` 0: its default and its fastest |

**The process** is the same under every one: `restart_node`, a publisher and a
subscriber of one lib/hg topic at 100 Hz. Under `ros2 launch` too: launch respawns any
executable, and what is compared is the restarting, not rclcpp. The subscriber is killed
with `SIGKILL` ten times, then the publisher ten times, and from each kill two times are
taken, on the clock the node writes its log with:

| time | from the kill to |
|---|---|
| **running** | the first statement of the new process's `main` |
| **messages** | the first message in a subscriber's hands that involves the new process (its own first, or the first from the new publisher). At 100 Hz no such figure is finer than 10 ms after the process runs |

**Spacing is part of the protocol.** Every supervisor here treats a process that keeps
dying differently from one that died once: hangard doubles its delay until the module
has been healthy for 30 s, systemd stops after five starts in ten seconds. The kills are
35 s apart, so that each is a first failure for all of them. What a supervisor does
with the fifth failure in a minute is a policy, not a latency, and is in the table
above, not in the numbers.

**What each row needs.** `hangard`: the manager's program and the `depth` message type
(both in the baselines' archive). `systemd`: a user manager to talk to, which exists
while the user has a session: an SSH login is one, so on the Pi it runs from the SSH
session as it is. Only a run that must outlive every session needs lingering
(`loginctl enable-linger bluerov2`), which on Ubuntu asks for authentication, so sudo;
a `tmux` inside the SSH session does not need it while the session stays. There is no
systemd in the container of `bench/ros2`, so that row is not in `check-baselines`.
`ros2-launch`: ROS 2 sourced. A row whose supervisor is not there is named and skipped.

`make check-restart` (part of `make check`) proves the script under hangard: two kills
of each role, each restarted and its messages back. It judges no time.

## What this does not measure, and what could mislead

- **One machine.** Publisher and subscribers on one host: nothing here is a tether or
  a network.
- **The load condition is not pinned.** Two spinners over four CPUs land where the
  scheduler puts them; the condition is a fixed amount of load, not a fixed placement.
- **The driver is a Python process on the machine**, on the CPUs the pair is not on. It
  sleeps through a run but for two readings of `/proc`.
- **Real time is the same class for every process of a system.** A system whose design
  wants different priorities for different threads is not given them.
- **Start-up includes creating the process**, in Python, before the node's own code
  runs: some milliseconds, the same for every system.
- **A percentile from the histogram is up to 3.1 % above the sample it stands for.**
- **Idle cost is not a cell.** What a matched pair costs with nothing published
  (`hg_bench idle`, `bench/ros2`) is not measured here yet.
- **The publisher's own cost of a send** is recorded (`send` in the publisher's line)
  and not yet in the report's tables.

## Beside the older benchmarks

`bench/hg_bench.c` measures lib/hg from inside (the stamp is the library's, written at
the start of the publish) and `bench/ros2/matrix.py` put it beside a ROS 2 talker and
listener (docs/00's measured column, the 2026-11-25 session). Their figures and this
directory's are not the same measurement and are not to be mixed in one row: here every
system, hangar included, is measured by the same node harness, with the same stamps,
the same warm-up and the same statistics. The earlier results stand as what they were.

## On the Pi

Nothing is compiled on the Pi during a session (it heats it, and the session's state is
frozen: `tools/rov/README.md`, "Freeze and thaw"). Everything is made on the laptop:

```sh
tools/baselines-debs.sh --status pi-packages.txt   # the .deb files of the middlewares the Pi lacks
tools/baselines.sh                                 # out/baselines/baselines-<version>-aarch64.tar.zst
tools/rov/lab.sh baselines --debs out/baselines/debs    # both to the Pi; dpkg -i under one sudo
```

The archive has every node, hangar's included, built from one `pair.h` with one
compiler and one set of flags (static musl programs; `MANIFEST` says which, with each
third-party source's version and hash), the ROS 2 node cross-built against an arm64
ROS 2 Jazzy, this directory, and the preflight. `lab.sh baselines` unpacks it in
`~/hangar-baselines`, checks every file against the `MANIFEST`, and has each node say
its version there: one that does not run on the Pi is found then, not in round 3.

```sh
# on the Pi, frozen (lab.sh freeze), hangar down:
cd ~/hangar-baselines/current && B=$PWD/bin
. /opt/ros/jazzy/setup.sh && . ros2/install/setup.sh      # for the ROS 2 entries
V="--var bin=$B --var base=$B --var ros=$PWD/ros2/install/ros2_pair/lib/ros2_pair --var ros_root=/opt/ros/jazzy"
bench/pairs/driver.py --plan bench/pairs/plans/smoke.json $V --out ~/pairs-smoke
bench/pairs/driver.py --plan bench/pairs/plans/main.json  $V --out ~/pairs-main-$(date +%F) --reps 2
```

Which plan and how many rounds: "Sessions", below.

## Sessions

Everything at full depth (`plans/full.json`: 18 cells, ten rounds, all 21 entries of the
registry) is 634 minutes a round and 106 hours. That is not the plan. A reader of the
comparison needs **the main table deep and the side rows honest**, and a side row does
not get more honest from ten rounds of cells it has nothing to say about. So there are
three plans, each naming its systems, its cells and its rounds, and
**each has `null`, `hangar` and `hangar-module` in it**: every session has the same
three reference rows, and sessions are laid beside each other through them.

| Plan | What it answers | Systems | Cells | A round | Rounds | In all |
|---|---|---|---|---|---|---|
| `plans/main.json` | how the systems compare, each as its users run it | `null`, `hangar`, `hangar-module`, `unix-seqpacket`, `iceoryx2`, `ros2-fastdds`, `ros2-cyclonedds`, `ros2-zenoh`, `lcm`, `moos` | all 18 | 309 min | 10 | 51.4 h |
| `plans/shared-memory.json` | what the ROS 2 middlewares do once set up for shared memory and loans | the three, `hangar-inplace`, `iceoryx2`, `ros2-fastdds-loan`, `ros2-fastdds-loan-besteffort`, `ros2-cyclonedds-iceoryx`, `ros2-zenoh-shm` | 8 | 147 min | 5 | 12.2 h |
| `plans/variants.json` | what one changed thing changes | the three, `hangar-module-refs4`, `unix-stream`, `iceoryx2-poll`, `ros2-fastdds-waitset`, `ros2-fastdds-composed`, `lcm-rcvbuf` | 8 | 138 min | 5 | 11.5 h |

75 hours in place of 106. The times are the driver's own, computed from the plan
(`driver.py --plan P --estimate` prints them, a system a line, and needs nothing
installed); they hold if nothing waits to cool or is repeated, and **none was measured
on the Pi**.

**Why these three.**

- *Main* has one entry a system: the one a user gets by following that system's own
  documentation, and hangar as a vehicle's module runs it (`hangar-module`) beside the
  bare library (`hangar`). Every cell, ten rounds: the interval of a median over ten
  runs is what a claim "A is faster than B" rests on, and the claims are made here.
- *Shared memory* is its own table because none of those configurations is how the
  middleware comes: each needs a profile, a daemon or a QoS a user must know to ask
  for. It is the fair answer to "but ROS 2 can do zero copy". `iceoryx2` is in it a
  second time, as the system those configurations are nearest to, and `hangar-inplace`
  because it is hangar's own form of a loan.
- *Variants* are rows that are **not like for like** with the main ones (one process,
  a spinning reader, a stream) or are a main row with one knob turned. Each is read
  against its own main row and nothing else.
- *The eight cells of the two side plans*: the five sizes at 100 Hz, 1 MB at 30 Hz,
  256 B at 1 kHz (the cell every sweep of the main plan passes through, here as the
  sweep `reference`) and the vehicle's graph, quiet. What is left out is what a side
  row would only repeat: the rates, the fan-out, the conditions under load and real
  time, start-up. A variant that turns out to matter in one of those is run through
  `plans/full.json` with `--only`.
- *Five rounds* for the side plans. The report prints an interval from three runs
  up, and with five it is coarse: it can only fall on one of the five figures. That
  is enough to say "the same as its main row within what a day changes" or "clearly
  not", which is what a side row is for, and not enough to rank two side rows a few
  per cent apart, **which the paper must then not do**. A side row that lands close to
  a claim gets more rounds: the folders pool.

**How to run them.** A plan is run a few rounds at a time: `--reps 2` of the main plan
is ten and a third hours, a night. Ctrl-C writes what there is; every session's folder
carries its boot id; `--report` pools as many folders as it is given and shows what the
sessions differ by. `--only` narrows a plan to some of its systems; keep the three
reference rows in.

What splitting costs: two systems of different plans were not measured minutes apart,
so a difference between them is read against the table "Within a session and between
sessions" for the reference rows, which says how much a day changes a figure. A
difference smaller than that spread is not a finding.

`moos` is in the main plan and is built only by `tools/baselines.sh --with-moos`
("MOOS" above): without it the driver names it as skipped and runs the rest.
