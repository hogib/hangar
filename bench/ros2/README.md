# bench/ros2: the ROS 2 baseline

[docs/00](../../docs/00-overview.md#targets) promises a comparison: on every line the
benchmark measures, hangar must be lighter than a ROS 2 talker/listener pair on the same
hardware, and the report states the factor. This directory is the ROS 2 side and the
script that runs both and writes the table. It is not part of hangar's build or CI:
`make all` and `make check` never touch it, and nothing here is needed on a vehicle.

| File | |
|---|---|
| `hangar_ros2_bench/` | a ROS 2 package (rclcpp, ament_cmake, nothing beyond `ros-jazzy-ros-base`): `hgb_talker`, `hgb_listener`, `hgb_composed`, and four fixed-size messages |
| `run.sh` | builds the package into a work directory, finds hg_bench, calls `matrix.py`; `--docker` does it in a container |
| `matrix.py` | the matrix: hangar and every ROS 2 middleware configuration found, N repetitions, the thermal state around every run; `results.json` and `results.md` |
| `config/` | the shared-memory profiles: Fast DDS data-sharing, Cyclone DDS over iceoryx |
| `linkbench.py` | the second-level comparison's measuring half: MAVROS against hangar's mavlink module ([below](#second-level-mavros-against-hangars-mavlink-module)) |
| `Dockerfile` | ROS 2 Jazzy with Cyclone DDS and Zenoh added, for a machine without ROS 2 |

> **For a comparison across several systems, `bench/pairs/` is the harness**
> ([its README](../pairs/README.md)): there every system, hangar included, is measured
> by the same node code, in interleaved rounds, with the whole latency distribution
> kept. This directory compares `hg_bench` with a ROS 2 talker and listener, each with
> its own stamps, and stays as what produced the figures of 2026-11-25. The two are
> not the same measurement, and their figures do not go into one row.

## What is compared

Two processes on one machine, one publishing fixed-size messages and one receiving them,
which is what two hangar modules do. The same measurement on both sides:

| Line | hangar (`bench/hg_bench.c`) | ROS 2 (`hangar_ros2_bench`) |
|---|---|---|
| Publish cost | `publish`: ns per `hg_publish`, no reader, 16 B, 256 B, 4 KB, 1 MB, constant payload | `hgb_talker --mode cost`: ns per `publish()`, no subscription anywhere, the same sizes, constant payload |
| Reading the newest message | `latest`: a publish+`hg_latest` loop minus a publish loop | `--mode take`: a publish+`take()` loop minus a publish loop, publisher and subscription in one process, no executor. The nearest thing ROS 2 has; with a middleware that delivers asynchronously within a process it includes the delivery (`empty_polls` in the JSON says so) |
| Latency | `wake`: writer and reader pinned to two CPUs, 256 B at 100 Hz, 1 kHz, 10 kHz on an absolute `clock_nanosleep` schedule; the stamp is CLOCK_MONOTONIC at the publish, the sample is taken when `hg_next` has copied the message out (`copied_*`) | the same writer loop; the stamp is CLOCK_MONOTONIC right before `publish()`, carried in the message; the sample is the first statement of the subscription's callback. p50, p99, p99.9, max over the run after a warm-up |
| The same, unpinned | `wake_nopin`, 1 kHz | the 1 kHz pair without `--cpu` |
| A camera's frames | `wake --wake-bytes 1048576 --wake-rate 30` | 1 MB at 30 Hz |
| CPU | both processes' CPU time over the run | both processes' CPU time (every thread, `getrusage`) over the measured part |
| Context switches | both processes', from `wait4`'s rusage | both processes', `getrusage`, over the measured part |
| Idle | `idle`: a reader asleep in `hg_wait` for 30 s: context switches and CPU ticks, which must be 0 | talker and listener matched, nothing published, 30 s: context switches and CPU of each |
| Memory | `rss`: PSS of writer and reader at 1 kHz, each reading its own `smaps_rollup` | PSS of talker and listener at 1 kHz, sampled from outside twice a second by `procbench.pss_kb`, the median; a middleware's daemon (RouDi, the Zenoh router) beside it |
| Lost | `gaps`: messages overwritten in the 16-slot ring before the reader took them | messages published in the measured part that never reached the callback |
| Start | not measured by hg_bench | from starting the two processes to the listener's first callback, the talker publishing at 1 kHz from the moment its publisher exists |

**Messages** are fixed-size on both sides, so that the middlewares that can loan samples
(zero copy) are allowed to: `Fixed16`, `Fixed256`, `Fixed4k`, `Fixed1m` are `uint64 seq`,
`uint64 stamp_ns` and a fixed array of `uint64` that fills the size; every word of
message *n* holds *n*, as in hg_bench, and the receiver checks them. hangar's stamp and
sequence number live in the slot's header, outside its payload, so a 256-byte hangar
payload stands beside a 256-byte ROS 2 message that carries the two inside.

**QoS**, chosen to mean what hangar's two ways of reading mean, and both are run:

| Name | ROS 2 | hangar |
|---|---|---|
| `latest` | best-effort, keep-last 1, volatile | `hg_latest`: the newest message, nothing kept for a slow reader |
| `queue` | reliable, keep-last 16, volatile | `hg_next` over the default ring of 16 slots: every message in order while the reader keeps up; the writer never waits (in DDS a keep-last history does not block the writer either) |

hg_bench's `wake` reads with `hg_next`, so the one hangar figure stands beside both ROS 2
rows; `hg_latest` on the same ring is no slower.

**Executor and processes.** Each node has a SingleThreadedExecutor, and talker and
listener are separate processes with no intra-process shortcut, because hangar's modules
are separate processes. One run puts both nodes in one process with rclcpp's
intra-process delivery (`hgb_composed`); it is in the table under its own heading,
labelled as not the comparison, because the question will be asked.

**Middlewares.** Every one that is installed gets a column; one that is missing is named
with the `apt` line that would add it, and nothing is installed. Each proves itself with
a one-second pair before it gets its column, and a configuration that asks for loaned
messages records whether the middleware granted them.

| Column | What |
|---|---|
| `fastdds` | `rmw_fastrtps_cpp` as installed, ROS 2's default. On one host Fast DDS already uses its shared-memory *transport* beside UDP |
| `fastdds-datasharing` | Fast DDS data-sharing delivery with loaned messages: `config/fastdds-datasharing.xml`, `RMW_FASTRTPS_USE_QOS_FROM_XML=1`, `ROS_DISABLE_LOANED_MESSAGES=0` |
| `cyclonedds` | `rmw_cyclonedds_cpp` as installed |
| `cyclonedds-iceoryx` | Cyclone DDS over iceoryx shared memory with loaned messages: `config/cyclonedds-iceoryx.xml`, RouDi started and stopped by the script |
| `zenoh` | `rmw_zenoh_cpp` as installed, its router started and stopped by the script |
| `zenoh-shm` | the same with `ZENOH_CONFIG_OVERRIDE=transport/shared_memory/enabled=true` |

All ROS 2 processes run with `ROS_DOMAIN_ID=77` and
`ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`: away from a ROS 2 graph already on the machine
(the lab's MAVROS), and with no discovery traffic on the tether. `--domain` and
`--discovery-range SUBNET` change both.

## What is NOT compared

ROS 2 does more than hangar, and this benchmark measures none of it:

- discovery across hosts and networks, with no configuration;
- QoS: reliability, durability, deadlines, liveliness, lifespan, per topic and per reader;
- typed IDL with variable-length messages, type introspection, several languages;
- several publishers on one topic; services and actions as first-class things;
- the tooling: `ros2 topic`, `rosbag2`, `rviz`, `rqt`, and the packages written against it.

hangar has one writer per topic, fixed-size messages, one machine and one tether
([docs/00, Non-goals](../../docs/00-overview.md#non-goals)). The claim the table supports
is therefore narrow: **for what a small vehicle's computer needs (a few dozen fixed-size
topics between processes on one board) this is what each costs.** It is not "hangar is
faster than ROS 2", and the paper should not say so. Where a ROS 2 configuration is
close to hangar or ahead of it on a line, the table says that too.

## Running it

```sh
bench/ros2/run.sh --quick                       # a smoke test: 1 repetition of 3 s runs
bench/ros2/run.sh                               # the measurement: 3 repetitions of 20 s runs
bench/ros2/run.sh --hg-bench ~/hangar-root/bin/hg_bench --label "Pi 5, ondemand"
bench/ros2/run.sh --configs hangar,fastdds --reps 5
bench/ros2/run.sh --docker --image hangar-ros2-bench:jazzy --docker-cpus 8-11 --quick
python3 bench/ros2/matrix.py --report results.json > results.md    # the table again
```

`run.sh -h` has the options; `matrix.py -h` the ones it passes on (`--reps`, `--seconds`,
`--idle-seconds`, `--cpus W,R`, `--configs`, `--max-temp`, `--cool-wait`, `--hot`,
`--busy`, `--label`). It needs no root, installs nothing, and writes only under `--work`
(`~/hangar-ros2-bench`) and `--out` (`<work>/results/<UTC time>`), and under `/dev/shm`
while it runs. Without colcon (or with `--no-colcon`) it builds with cmake alone.

The pair is pinned as hg_bench pins its own: the writer on the second-highest CPU the
script may use, the reader on the highest (`--cpus W,R` to choose), every thread of a ROS 2
process on its one CPU; `matrix.py` itself and a middleware's daemon run on the other
CPUs. hangar and the configurations take turns at going first from one repetition to the
next.

**Time.** One ROS 2 configuration is about 5 minutes per repetition at the defaults, hangar
about 3. With Fast DDS alone installed (two columns): 3 × (2 × 5 + 3) ≈ 40 minutes; with all
six columns about 100. `--reps 1` is a third of it. The script prints its estimate before
it starts.

**Thermal state.** Before and after every single run the script records the governor of
every CPU, the temperature, the Pi firmware's throttling word (`vcgencmd get_throttled`
or its sysfs file) and the pinned CPUs' current and maximum frequency. Before a run it
waits (up to `--cool-wait`, 180 s) for the machine to be under `--max-temp` (75 °C). A run
that began or ended at `--hot` (80 °C) or above, throttled, frequency-capped, with the
governor changed under it, or during which the CPUs beside the pair's were `--busy` (25 %)
or more with something else (Ubuntu's unattended-upgrades took a core mid-session on
2026-11-20) is kept and **flagged**: its cell carries a † and the run is listed under the
table with its flags. A flagged cell is not a number for the paper: cool the machine,
quieten it, and run again.

### Reading the results

`results.md` is the table: one row per metric, one column per system, each cell the median
over the repetitions, (min–max), and **×N**, the ROS 2 figure divided by hangar's.
`results.json` has everything behind it:

```
label, date, smoke_test, args, skipped[]
environment   model, arch, kernel, cpus, writer_cpu, reader_cpu, container, governor,
              ros_distro, hg_commit, hg_compiler, commit, running_beside[], loadavg1
configs       per column: title, rmw, env, and loaned (was the loan granted)
state_start, state_end     temp_c, governor[], throttled, freq_khz, loadavg1
runs[]        rep, config, case, cooled_s, before, after (states), flags[], result
              (the processes' own JSON lines; for hangar, hg_bench's report)
summary       metric -> column -> median, min, max, n, flagged
factors       metric -> column -> ROS 2 median / hangar median
```

```sh
jq '.factors | {publish_ns_256, latest_ns_256, lat_p50_1000_latest, lat_p99_1000_latest, pss_kb}' results.json
jq '.runs[] | select(.flags != []) | {rep, config, case, flags}' results.json
```

## On the Pi, step by step

The lab's BlueROV2 (`tools/rov/README.md`): a Pi 5, Ubuntu 24.04, ROS 2 Jazzy installed
natively, governor `ondemand`. The benchmark itself runs as the Pi's user with no sudo.
Two things around it do need the Pi's sudo password, exactly as `tools/rov/lab.sh` needs
it: stopping the lab's services, and (optionally) the governor.

1. **On the laptop, before leaving:** build the release, which carries the `hg_bench` this
   comparison needs (`--wake-bytes`, the `copied_*` percentiles), and have this branch
   checked out.

   ```sh
   make -j4 all && tools/release.sh
   ```

2. **At the lab, from the laptop:** the release onto the Pi as always, and this directory
   beside it (with `procbench.py`, which `matrix.py` imports):

   ```sh
   tools/rov/lab.sh deploy
   tar -czf - bench/ros2 bench/procbench.py | ssh bluerov2@<the Pi> 'mkdir -p hangar-bench && tar -C hangar-bench -xzf -'
   ```

3. **On the Pi: a quiet machine.** hangar down (`tools/rov/lab.sh down` if a session is
   open: no manager, no MAVROS relay), and the lab's electric-eye stopped the way
   `lab.sh up --video` stops it, to be started again in step 6:

   ```sh
   sudo systemctl stop eeye hellion
   pgrep -a 'hangard|mavros|eeye|hellion' ; vcgencmd measure_temp ; vcgencmd get_throttled
   ```

   Nothing should be listed, and `throttled=0x0`. The script names what it finds running
   beside it (`running_beside`), and says so if the Pi has throttled since boot. Take the
   tube's lid off or point a fan at the Pi: closed, on the bench, it reached 86–91 °C in
   earlier sessions and throttled.

4. **A smoke test, then the run** (about 2 minutes, then about 40 with Fast DDS alone):

   ```sh
   cd ~/hangar-bench
   bench/ros2/run.sh --quick --hg-bench ~/hangar-root/bin/hg_bench
   bench/ros2/run.sh --hg-bench ~/hangar-root/bin/hg_bench --label "Pi 5, ondemand, $(date -u +%F)"
   ```

   The first lines say which middlewares it found and which it skipped. If colcon is
   missing it builds with cmake; if `cmake` or `c++` is missing it stops and says so,
   having changed nothing (the comparison then waits for whoever owns the Pi to install
   `build-essential cmake`).

5. **Optionally, under `performance`**, the governor docs/00's measured column was taken
   under; `PI-TEST.md` section 4 has the same commands, and the governor goes back as found:

   ```sh
   gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
   sudo sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > $f; done'
   bench/ros2/run.sh --no-build --hg-bench ~/hangar-root/bin/hg_bench --label "Pi 5, performance, $(date -u +%F)"
   sudo sh -c "for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo $gov > \$f; done"
   ```

6. **Put it back and bring the results home:**

   ```sh
   sudo systemctl start eeye hellion                 # on the Pi
   scp -r bluerov2@<the Pi>:hangar-ros2-bench/results ~/hangar-results/$(date +%F)-ros2    # on the laptop
   ```

   `~/hangar-ros2-bench` on the Pi (the build, a few MB) can stay for the next visit or be
   removed; nothing else was written.

## Threats to validity

- **Governor.** Under `ondemand` a pair at 100 Hz or 1 kHz leaves its CPUs mostly idle and
  they clock down; latencies are then the governor's as much as the middleware's (hangar's
  own 1 kHz median was 8.8 µs under `ondemand` and 5.5 µs under `performance`). Both systems
  run under the same governor in the same session, and the report names it; report both
  governors, and never compare across them.
- **Thermal state.** The Pi throttled in earlier sessions. A throttled or hot run is
  flagged, not dropped; a table with † in it is not finished. hangar and ROS 2 alternate
  within a repetition, so a slow drift reaches both, but a throttle that lasts one run
  spoils that run alone: that is what the repetitions and the (min–max) are for.
- **DDS tuning.** Each middleware runs as installed, plus one documented shared-memory
  configuration. A DDS expert can do better than "as installed" (socket buffers and
  `net.core.rmem_max` for the 1 MB messages, which needs root; segment sizes; publish mode;
  executor choice; a real-time kernel and priorities). The columns say what a user gets
  from the distribution and from the vendor's documented zero-copy setting, not the best
  possible ROS 2. Losses of 1 MB best-effort messages in the as-installed columns are the
  default buffer sizes and are reported as they are.
- **Pinning.** Every thread of a ROS 2 process is pinned to one CPU, because a hangar
  module is one thread on one CPU and the two should stand on the same ground. A DDS
  process has a dozen threads and might do better spread out: the unpinned 1 kHz row is
  there to show it.
- **Composed against separate processes.** The composed row is ROS 2 at its best and is a
  different architecture: one process, one crash domain. It stands in the table beside the
  comparison, not in it.
- **Message sizes and rates** are hg_bench's (16 B to 1 MB; 100 Hz to 10 kHz), chosen
  before this comparison existed and typical of a vehicle (IMU, attitude, thruster
  commands; a camera frame). ROS 2's fixed costs weigh most on small, frequent messages,
  which is where a vehicle's topics are; at 1 MB a memcpy dominates on both sides and the
  factors shrink. Variable-length messages, where ROS 2 serializes and hangar has nothing
  to offer, are not measured.
- **What "latency" is.** hangar's stamp is taken by the library at the start of
  `hg_publish`, ROS 2's by the talker right before `publish()`: the same point. hangar's
  sample is taken after the copy out of the ring, ROS 2's at the top of the callback: the
  message in hand on both sides. hg_bench also reports the earlier wake-up time; the
  table uses the later one.
- **CPU windows.** hg_bench counts a process's whole life, the ROS 2 programs the measured
  part only (start-up and discovery excluded): this favours ROS 2, deliberately.
- **Start-up** has no hangar figure: hg_bench forks its pair and does not time it.
- **Docker.** `--docker` shares the host's kernel, so shared memory, futexes and the
  scheduler are the host's; but the cpuset is a cgroup, `/dev/shm` is the container's
  (`--shm-size 2g`), the seccomp profile filters system calls, and the libc and ROS 2
  build are the image's. A container run is labelled "inside a container" in the report.
  Use it for smoke tests and for a machine that must stay clean; docs/00's numbers come
  from a native ROS 2.
- **One machine, a few repetitions.** Three repetitions give a median and a range, not a
  distribution. Say so beside the table.

## What has been run so far

Nothing on the Pi. On 2026-11-20 the package and the runner were checked in Docker on an
x86-64 laptop (`ros:jazzy-ros-base`, and `bench/ros2/Dockerfile`'s image for Cyclone DDS
and Zenoh), `--quick`, with hangar's static `hg_bench` in the same container: a smoke
test of the harness, on a machine at 95–100 °C with other jobs on it, so every run was
flagged and **none of its numbers is a result**. What it did show:

- the package builds with colcon and with cmake alone; all six columns run and prove
  themselves; Fast DDS data-sharing and Cyclone DDS over iceoryx both granted loaned
  messages for the fixed-size types, in a container;
- hangar's static binary runs in the ROS 2 image, so a same-container comparison works;
- the flags fire: hot, and other load on the CPUs beside the pair's;
- three things to look at when the Pi's numbers come: with small messages hangar was
  several times lighter at the median, in CPU, in context switches and in memory, while
  the tails (p99 and beyond) were that machine's noise on both sides; at 1 MB the loaned-message
  configurations were *ahead* of hangar on publish cost and on latency (they copy nothing,
  hangar copies the frame into the ring and out of it); and Fast DDS as installed lost a
  fifth of the 1 MB best-effort messages (default buffer sizes).

Not verified without the Pi: which middlewares it has (Fast DDS is expected, the others
are probably not installed), whether loans are granted there, colcon's presence, and every
number.

## Adding the result to docs/00

Only a run on the Pi, not flagged, not `--quick`, not in a container. docs/00's Targets
table has a "Measured" column for hangar; the comparison adds one column beside it,
"ROS 2 Jazzy, Fast DDS as installed, same Pi and governor", with the ROS 2 figure and the
factor, on the rows both systems measure:

| docs/00 row | `summary` / `factors` key |
|---|---|
| Publish a message of up to 256 bytes, no reader waiting | `publish_ns_256` |
| Read the newest message of up to 256 bytes | `latest_ns_256` |
| Publish → a waiting reader wakes | `lat_p50_1000_latest`, `lat_p99_1000_latest` (and the idle row: `idle_listener_ctxsw_s`) |
| (new row) Resident memory, a 1 kHz pair | `pss_kb` |
| (new row) CPU, a 1 kHz pair | `cpu_pct_1000_latest` |

```sh
jq -r '.summary as $s | .factors as $f | ["publish_ns_256","latest_ns_256","lat_p50_1000_latest","lat_p99_1000_latest","pss_kb","cpu_pct_1000_latest"][]
       | "\(.)  hangar \($s[.].hangar.median)  fastdds \($s[.].fastdds.median)  x\($f[.].fastdds)"' results.json
```

Then replace the sentence under the table ("the ROS 2 baseline on the same Pi is still to
be measured") with the date, the governor, the commit and the path of the results
directory, and copy `results.md` and `results.json` beside `PI-TEST.md`'s results. The
shared-memory columns and the other middlewares belong in the paper's table, not in
docs/00's: docs/00 promises the default middleware.

## Second level: MAVROS against hangar's mavlink module

The first level compares transports. The second compares one job done both ways: the
autopilot link. On 2026-11-20 on the lab's Pi, MAVROS as the serial-to-UDP relay between
the Pixhawk and hangar cost 28 % of a core and 128 MB, and hangar's mavlink module 1.6 %
(`tools/rov/README.md`). That was not a fair pair: MAVROS was a relay in front of the
module, and the two were measured in different sessions. The module now reads the
Pixhawk itself (`Serial=`, `modules/mavlink/README.md`), so the two can do the same job,
one after the other. **Not run yet;** this is the protocol, and `linkbench.py` is the
measuring half of it. It starts nothing and talks to nothing: it reads `/proc` and `/sys`
for the named processes.

The fair protocol, props off, vehicle disarmed throughout:

1. **Same device, one at a time.** `/dev/ttyACM0` at the same baud rate; only one of the
   two holds the port. Never both, and no relay in front of either.
2. **Same streams.** ArduSub sends what it is asked for. hangar's module asks with
   `MAV_CMD_SET_MESSAGE_INTERVAL` for its table (`ATTITUDE`, `SCALED_IMU`, the depth's
   pressure message, `SYS_STATUS`, `SERVO_OUTPUT_RAW`, at the rates of
   `profiles/rov-vehicle/mavlink.conf`). Give MAVROS the same: its plugin allow-list cut
   down to the plugins that publish those five, and the same message intervals set
   through its `set_message_interval` service. Also measure MAVROS as it is launched by
   default, every plugin loaded, and report that figure as what it is: the default, not
   the like-for-like one. Write the ArduSub `SR0_*`/`SR1_*` parameters down
   before and after: a stream left on by one run feeds the other. Record the received
   message rates on both sides (`ros2 topic hz` once, outside the measured window;
   `hangar hz nav.attitude`), and do not compare runs whose rates differ.
3. **Same consumers.** A publisher with no subscriber does less work. Either none on both
   sides, or one reader per topic on both (`ros2 topic echo --no-arr` processes against
   `hangar echo` processes), counted or not counted alike. State which.
4. **Same input.** No manual control in either run, or 50 Hz of it in both.
5. **Same duration and state.** 120 s each, after 30 s of settling, the Pi under 75 °C
   and not throttled before each (`linkbench.py` records it and flags a hot run); order
   A, B, B, A to cancel drift.
6. **What is counted.** Every process the job needs: for MAVROS, `mavros_node` (and the
   ROS 2 daemon if one is running); for hangar, `hangar-mavlink` alone and, as a second
   figure, with `hangard`, since a module does not run without its manager. CPU as a
   percentage of one core, PSS, context switches per second, threads.

```sh
# A: hangar's module on the serial port (modules/mavlink/README.md, "Instead of MAVROS")
sudo python3 -I bench/ros2/linkbench.py --seconds 120 --out link-hangar.json hangar-mavlink
# B: hangar down, MAVROS alone on the same port, the plugins and intervals of step 2
python3 -I bench/ros2/linkbench.py --seconds 120 --out link-mavros.json mavros_node
python3 -I bench/ros2/linkbench.py --compare link-hangar.json link-mavros.json
```

`sudo` on line A is for the PSS: a hangar module is not dumpable, so only root reads its
`smaps_rollup`. What the result can claim: the cost of the autopilot link for the five
telemetry streams a BlueROV2 pilot needs. MAVROS also offers the whole MAVLink dialect as
ROS 2 topics, services and parameters, and that breadth is what its memory buys.
