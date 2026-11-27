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
bench/pairs/driver.py --plan bench/pairs/plans/full.json --var bin=$HOME/hangar-root/bin \
    --out ~/hangar-results/<date>-pairs       # the measurement (on the Pi: below)
bench/pairs/driver.py --report ~/hangar-results/*-pairs > pooled.md
```

`make check-pairs` (part of `make check`) runs a plan of four cells over hangar, the
null system and the Unix sockets and checks the counting, the records and the report: 44 checks, and no
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
three conditions, and start-up. A cell that two sweeps name is run once.

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
a whole message. A system that delivers by callback hands the message from its callback
to `sub_recv` (its executor run inside `sub_recv` until one has come). Build it with
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
(`--role both`). A system whose programs are not there is named, with its `install`
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
| `unix-seqpacket` | Unix domain sockets, `SOCK_SEQPACKET`: the kernel's own IPC with no middleware. **The floor** for anything that sends through the kernel | two copies and one system call a side; the reader asleep in `recv`, woken by the kernel. The publisher never waits: a subscriber whose buffer is full does not get the message. A socket then drops the **newest** and keeps the old (hangar's ring drops the oldest), and the buffer's depth is the kernel's count of bytes (`net.core.wmem_default`, 212992 B: some hundreds of small messages), not 16. A message must fit that buffer whole: up to 131072 B here, and the larger cells are not run. No discovery beyond a path, no type, no metadata |
| `unix-stream` | the same with `SOCK_STREAM`, for any size | **another contract**: the publisher's send blocks while the reader's buffer is full, so nothing is lost and a slow reader slows the writer (it shows under "intended"). A megabyte is several send and receive calls. The reader has the message when it has collected all its bytes |

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

Not in this directory yet: the node pairs of the other baselines (Cyclone DDS, Fast DDS
and Zenoh under ROS 2, iceoryx2 woken and polling, MOOS, LCM). Each comes with its entry
and its own lines for this table.

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
frozen: `tools/rov/README.md`, "Freeze and thaw"). hangar's two nodes are in the release
(`bin/hangar_pair`, `bin/null_pair`); a baseline's node is built for aarch64 beforehand.

```sh
# from the laptop: the driver, the plans and the preflight beside the release
tar -czf - bench/pairs tools/rov/preflight.py | ssh bluerov2@<the Pi> 'mkdir -p hangar-bench && tar -C hangar-bench -xzf -'
# on the Pi, frozen (lab.sh freeze), hangar down:
cd ~/hangar-bench
bench/pairs/driver.py --plan bench/pairs/plans/smoke.json --var bin=$HOME/hangar-root/bin --out ~/pairs-smoke
bench/pairs/driver.py --plan bench/pairs/plans/full.json  --var bin=$HOME/hangar-root/bin --out ~/pairs-$(date +%F)
```

The full plan is 15 cells, 26 minutes a system a round (the runs at 100 Hz and below are
two minutes each, for their samples): ten rounds are about four and a half hours for
each system in the registry, so with the baselines it is a matter of days, not of one
visit. It is made for that: `--reps N` runs fewer rounds at a time, Ctrl-C writes what
there is, every session's folder carries its boot id, and `--report` pools as many
folders as it is given and shows what the sessions differ by.
