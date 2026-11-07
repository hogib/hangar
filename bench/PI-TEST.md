# hangar on a Raspberry Pi: the first test on real hardware

The plan for the visit, in order. One release tarball
(`tools/release.sh`), one Pi running 64-bit Raspberry Pi OS, model 4 or 5 (unknown
until the visit), an SSH session, about an hour. Nothing here touches ArduSub, the
Navigator or the thrusters ([What not to do](#what-not-to-do)).

What comes home: lib/hg's test suites run on AArch64 for the first time, the
`hg_bench all` report, a manager that supervised `eeye` through a kill, a restart and
its own crash, and the logs. [What the results feed](#what-the-results-feed) says where
each number goes.

Everything below runs as the login user (`pi`, or whoever) out of one directory,
`$HANGAR_ROOT=$HOME/hangar-root`; nothing is installed under `/etc`, `/usr` or
`/var`, and nothing is enabled in systemd. If `hangard` is not in the tarball yet,
skip to [Fallback](#fallback-no-hangard-in-the-tarball).

## 0. Before leaving

```sh
# the release: static aarch64 musl, -mcpu=cortex_a72 (runs on a Pi 4 and a Pi 5;
# --cpu cortex_a76 would be the Pi 5-only build, not for this visit)
tools/release.sh
ls -la out/release/
# the MANIFEST's "skipped" line says what the tarball does not have yet
tar --zstd -xOf out/release/hangar-*-aarch64.tar.zst --wildcards '*/MANIFEST' | head -9
```

Bring: the tarball, this file (it is in the tarball too, `share/doc/PI-TEST.md`), the
Pi's address or hostname, a USB-C/micro-USB power supply, a camera attached to the Pi
(the one `eeye` is to acquire), and a laptop with `ssh`, `scp`, `zstd`.

## 1. On the Pi: what it is

```sh
ssh pi@<pi>
mkdir -p ~/hangar-visit && cd ~/hangar-visit
{
  cat /proc/device-tree/model; echo
  uname -a
  cat /etc/os-release | head -3
  nproc; free -m
  mount | grep /dev/shm                  # must say tmpfs
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
  vcgencmd measure_temp; vcgencmd get_throttled; vcgencmd measure_clock arm
  groups                                 # must include video for the camera
  ls -la /dev/video* 2>/dev/null; rpicam-hello --list-cameras 2>&1 | head -20
  systemctl list-units --type=service --state=running | grep -iE 'ardusub|mavproxy|eeye|hellion|navigator|blueos|docker' || echo "no ArduSub/electric-eye/BlueOS services running"
  pgrep -a -f 'ardusub|eeye|hellion|mavproxy' || echo "no such processes"
} 2>&1 | tee 00-machine.txt
```

Checks, each a stop if it fails:

| Check | Why |
|---|---|
| `/proc/device-tree/model` says Raspberry Pi 4 or 5 | the build is `cortex_a72`: the Pi 4's core, which the Pi 5's A76 also runs. A Pi 3 (A53) runs it as well; a 32-bit OS does not (`uname -m` must be `aarch64`) |
| `uname -r` is 5.16 or later | `futex_waitv` ([docs/00](../docs/00-overview.md#platform)). Pi OS Bookworm ships 6.1, 6.6 or 6.12. Older: `hg_wait` fails with `ENOSYS` and every futex suite fails at its first wait; nothing else to do that day but the publish/latest numbers |
| `/dev/shm` is `tmpfs` | the topic files and electric-eye's frame bus live there |
| `groups` lists `video` | `eeye` opens the camera as this user |
| No ArduSub, MAVProxy, `eeye` or `hellion` service running | a running `eeye` fights for the camera and the frame bus files; ArduSub is left as found ([What not to do](#what-not-to-do)). If electric-eye's own services are enabled from an earlier install: `sudo systemctl stop eeye hellion` for the visit (do **not** disable them; stop only) |
| `vcgencmd get_throttled` is `0x0` | a Pi that has throttled has no steady clock for a benchmark; let it cool or fix the supply first |

## 2. Copy and unpack

```sh
# laptop
scp out/release/hangar-*-aarch64.tar.zst pi@<pi>:~/hangar-visit/
# Pi
cd ~/hangar-visit
tar --zstd -xf hangar-*-aarch64.tar.zst        # needs zstd: sudo apt install zstd
cd hangar-*-aarch64/
head -9 MANIFEST                               # version, target, commit, built, binaries, skipped
grep -E '^[0-9a-f]{64}  ' MANIFEST | sha256sum -c --quiet && echo "hashes ok"
file bin/* | sed 's/,.*statically linked.*/: static ok/'
bin/hg_bench sizes bin/*                       # sizes, static and stripped as the ELF says
```

Every line of `file` must say `statically linked`; a binary that says `dynamically
linked` or `interpreter` was mis-built and is not to be run.

## 3. lib/hg's test suites: the first run on AArch64

The suites have run natively on x86-64 and, five of eight, under `qemu-aarch64`
(user-mode QEMU has no `futex_waitv`, so `test_hg_wait`, `test_hg_waitfd` and
`test_hg_replace` have never run on an AArch64 kernel). This is the run that matters:
the publish/wait protocol's memory-ordering argument (`write_seq` and the wake flag
`seq_cst` on both sides, an STLR on AArch64, docs/01 "Publishing") has only ever been
exercised on x86's stronger TSO model.

```sh
mkdir -p ~/hangar-visit/suites
for run in 1 2 3; do
  for t in bin/test_hg_*; do
    n=$(basename $t)
    if timeout 300 $t > ~/hangar-visit/suites/$n.$run.log 2>&1; then echo "ok    $n ($run)"
    else echo "FAIL  $n ($run) -- log: suites/$n.$run.log"; fi
  done
done 2>&1 | tee ~/hangar-visit/suites/summary.txt
```

Three passes because an ordering bug is probabilistic. The suites make their topic
directory under `/dev/shm/hangar-test-<uid>` and remove it.

What a failure would mean (save the log, change nothing on site, bring it home):

| Suite | A failure means |
|---|---|
| `test_hg_layout` | the on-disk layout differs between x86-64 and AArch64: a struct's alignment or padding, which would also break a reader written from docs/01 alone. Serious, and a build-side fix |
| `test_hg_pubsub`, `test_hg_prov`, `test_hg_wiring` | portable logic that passed under QEMU: an AArch64 compiler difference or an endianness/width assumption. Read the log's first failing CHECK |
| `test_hg_torn` | a torn copy got through the seqlock: the acquire/release fences in the reader's copy are wrong on a weakly-ordered machine. The one failure that would change the design |
| `test_hg_wait` (`test_lost_wake` or `test_publish_order_guard`) | a lost wake-up: the writer's flag exchange and the reader's flag store/`write_seq` load do not exclude each other on AArch64 as C11 says they must. Also design-level |
| `test_hg_wait`, `test_hg_waitfd`, `test_hg_replace` failing at the first wait with `ENOSYS` | the kernel is older than 5.16: see the table in step 1 |
| `test_hg_waitfd` alone | the helper thread's eventfd path; a bug, not a design flaw |
| a hang (the `timeout` fires) | a missed wake-up with no timeout to catch it; the log shows how far it got |

## 4. Governor and temperature, then the benchmark

The targets in docs/00 are stated for a Pi 4; the ROS 2 baseline will be measured on
this same Pi later, so what matters is that every run records its governor and
temperature. Two runs: as the Pi comes (`ondemand` on Pi OS), then pinned to
`performance`.

```sh
cd ~/hangar-visit/hangar-*-aarch64/
vcgencmd measure_temp; vcgencmd get_throttled; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
bin/hg_bench all --bin bin/hangard --bin bin/hangar --bin bin/hg_bench --bin bin/eeye --bin bin/hellion \
  --json ~/hangar-visit/bench-default.json 2>&1 | tee ~/hangar-visit/bench-default.txt
vcgencmd measure_temp; vcgencmd get_throttled

echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
sleep 5; vcgencmd measure_temp; vcgencmd measure_clock arm
bin/hg_bench all --bin bin/hangard --bin bin/hangar --bin bin/hg_bench --bin bin/eeye --bin bin/hellion \
  --json ~/hangar-visit/bench-performance.json 2>&1 | tee ~/hangar-visit/bench-performance.txt
vcgencmd measure_temp; vcgencmd get_throttled
echo ondemand | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor   # leave it as found
```

Drop a `--bin` for a binary the tarball does not have. Each `all` run takes about
three minutes: three 20 s wake runs, a 30 s idle run, the rest seconds. The report's
`machine` object records the governor itself; the temperatures go in a note beside the
JSON (`~/hangar-visit/NOTES.txt`: model, temperatures before and after, anything odd
on the screen). If `get_throttled` is not `0x0` after a run, the run is suspect: let
the Pi cool and run again.

Reading the human table against the targets ([docs/00](../docs/00-overview.md#targets)):

| Line | Target on a Pi 4 | |
|---|---|---|
| `publish 256 B` | under 1000 ns | no reader, no system call |
| `latest 256 B` | under 500 ns | |
| `wake 1000 Hz p50/p99` | measured and reported | one futex wake per message; `wakes` should be close to `msgs`, `gaps 0`, `late 0` |
| `wake 10000 Hz` | may say `(did not keep up)` on a Pi 4 | then its line is reported as such, not as a latency |
| `idle 30 s` | `ctxsw +0/+0, cpu ticks +0` | a sleeping module costs nothing |
| `rss` | the module library's footprint | static musl: a few hundred kB Pss each |
| `torn`, `lost_wake` | `torn 0`, `timeouts 0`, got = sent | correctness, not speed |

If `wake` at 1 kHz shows a p99 far above its p50 with `late` well above 0, run it once
more with `--no-pin` (`bin/hg_bench wake --no-pin --json ~/hangar-visit/wake-nopin.json`):
the harness pins the pair to the two highest CPUs, and something else on those CPUs
would show exactly like that.

## 5. hangard with electric-eye as phase-A legacy modules

The layout under `$HANGAR_ROOT`, from the tarball's `share/hangar/`:

```sh
export HANGAR_ROOT=$HOME/hangar-root
sh share/hangar/install-root.sh $HANGAR_ROOT     # bin/, share/, etc/modules/, etc/hangard.conf, var/eeye, etc/hellion
cat $HANGAR_ROOT/etc/hangard.conf
cat $HANGAR_ROOT/etc/modules/eeye.module         # Exec points at $HANGAR_ROOT/bin/eeye; User/Group commented out
ls -la /dev/shm/                                 # no eeye.* and no hangar/ yet
```

`install-root.sh` comments out the manifests' `User=eeye`/`Group=video` lines because
`hangard` runs here as the login user and cannot switch users. (`--system` keeps them,
for a run under `sudo` with the `eeye` user created as `legacy/install.sh` does; not
this visit.) `hellion.module` is enabled too; `hellion` needs `ffmpeg` on the Pi for
video but runs its page and discovery without it. If `hellion` is unwanted, move its
manifest aside: `mv $HANGAR_ROOT/etc/modules/hellion.module{,.off}`.

Start the manager in the foreground, in a second SSH session so the first is free:

```sh
# session 2
export HANGAR_ROOT=$HOME/hangar-root
bin/hangard --help                                # the flag that names the configuration file
bin/hangard -c $HANGAR_ROOT/etc/hangard.conf 2>&1 | tee ~/hangar-visit/hangard.log
```

(`-c` is a placeholder for whatever `hangard --help` says; if the manager reads only
`/etc/hangar/hangard.conf` and `/run/hangar`, this step needs `sudo` and the
`--system` layout; write down what it needed.)

In session 1, within the first ten seconds:

```sh
export HANGAR_ROOT=$HOME/hangar-root
bin/hangar top --once                             # eeye: starting -> ready; hellion likewise
bin/hangar topics                                 # the manager's own: hangar.vehicle.system, .modules, .admin.req
bin/hangar echo hangar.vehicle.modules --json | head -40
ls -la /dev/shm/eeye.* /dev/shm/hangar/           # eeye's frame bus files appear when the camera is acquired
cat $HANGAR_ROOT/var/eeye/effects.json.health     # eeye's own health file: camera, fps
pgrep -a eeye
```

`eeye` is a `Legacy=yes` module: it declares no hangar topics, so `hangar topics`
lists the manager's topics and `hangar top` shows `eeye` as `ready` while its process
runs. Its frames are on electric-eye's frame bus, `/dev/shm/eeye.raw` and
`/dev/shm/eeye.pilot`, read by `hellion` and, once the adapter of docs/01 is built
(`hg_sub_open("eeye:raw", …)`), by `hangar echo eeye:raw`. For this visit, "eeye
acquires the camera and publishes" is: `eeye` is `ready` in `hangar top`, its health
file names the camera and a frame rate, and `/dev/shm/eeye.raw` grows a sequence
(`watch -n1 'ls -la /dev/shm/eeye.*'`). If `hellion` is up, `bin/hellion top` shows
the frame rate from the bus.

Kill `eeye` with SIGKILL and watch the restart:

```sh
pgrep -x eeye                                     # pid A
pkill -KILL -x eeye
sleep 1; bin/hangar top --once; pgrep -x eeye     # pid B, state starting/ready, failures 1, delay 0.5 s
```

The manager logs the death by signal 9 and the restart after `Backoff`'s first value
(0.5 s in `eeye.module`). Kill it three more times, a second apart, and the delay
doubles: 0.5, 1, 2, 4 s (`hangar top` shows `backoff` with the delay). Then leave it
for `ResetAfter` (30 s) and it is healthy with the count reset. Five kills within 60 s
(`Burst=5/60s`) would make the manager give up (`failed`, system `degraded`); if that
happens, `bin/hangar restart eeye` brings it back. Record what the log shows for one
kill in `NOTES.txt`.

Kill the manager and start it again (adoption):

```sh
pgrep -x eeye                                     # pid C
pgrep -x hangard; pkill -KILL -x hangard
sleep 1; pgrep -x eeye                            # still pid C: modules outlive the manager
# session 2: the manager is gone; start it again exactly as before
bin/hangard -c $HANGAR_ROOT/etc/hangard.conf 2>&1 | tee -a ~/hangar-visit/hangard.log
# session 1
bin/hangar top --once; pgrep -x eeye              # still pid C: adopted, not restarted
```

The new manager's log should say it adopted `eeye` (and `hellion`) from the
registry, PIDs and start times matching. An `eeye` restarted instead of adopted (a new
PID) is a finding, not a failure of the visit: note it.

Stop everything:

```sh
# session 2: Ctrl-C (SIGINT): the manager stops its modules and exits
# session 1
pgrep -a -f 'hangard|eeye|hellion' || echo "all stopped"
ls -la /dev/shm/                                  # the topic files may remain; that is by design
```

## 6. What to bring home

```sh
cd ~/hangar-visit
cp -r $HANGAR_ROOT/etc . 2>/dev/null; cp -r $HANGAR_ROOT/log hangard-logs 2>/dev/null
dmesg | tail -50 > dmesg-tail.txt 2>&1 || sudo dmesg | tail -50 > dmesg-tail.txt
vcgencmd measure_temp >> NOTES.txt
tar czf ~/hangar-visit-$(date +%Y%m%d).tar.gz --exclude='hangar-*-aarch64' --exclude='*.tar.zst' .
# laptop
scp pi@<pi>:~/hangar-visit-*.tar.gz .
```

The archive holds: `00-machine.txt`; `suites/` (every log and `summary.txt`);
`bench-default.json/.txt`, `bench-performance.json/.txt`, `wake-nopin.json` if run;
`hangard.log`; `etc/` (the manifests and `hangard.conf` as run); `dmesg-tail.txt`;
`NOTES.txt` (temperatures, `get_throttled`, what `hangard --help` said, anything odd).

## What not to do

- **Nothing touches ArduSub, the Navigator or the thrusters.** No `hangar
  ardusub-setup`, no MAVLink module, no `mavlink` manifest, no parameter changes. If
  ArduSub (or BlueOS) is installed and running, leave it as found; if it is stopped,
  leave it stopped. hangar phase A drives nothing ([docs/11](../docs/11-electric-eye.md)).
- Do not `systemctl enable` or `disable` anything. Electric-eye's own services, if
  present, are stopped for the visit and left enabled.
- Do not install hangar under `/etc`, `/usr` or `/var`; everything lives under
  `~/hangar-visit` and `~/hangar-root`.
- Do not run the benchmark while `eeye` or `hellion` is running (steps 4 and 5 are in
  that order for a reason), and do not run it from a throttled Pi.
- Do not fix a failing suite on site. Save the log.
- Leave the governor as found (`ondemand`).

## Fallback: no hangard in the tarball

The MANIFEST's `skipped` line names `hangard` and `hangar`. Then steps 1 to 4 are the
visit (the suites and the benchmark do not need the manager), with `eeye` run by hand
to confirm the camera:

```sh
export HANGAR_ROOT=$HOME/hangar-root
sh share/hangar/install-root.sh $HANGAR_ROOT
bin/eeye $HANGAR_ROOT/var/eeye/effects.json &    # logs to stderr
sleep 5; ls -la /dev/shm/eeye.*; cat $HANGAR_ROOT/var/eeye/effects.json.health
bin/hellion -c $HANGAR_ROOT/etc/hellion/hellion.json &   # optional
sleep 3; bin/hellion top                          # q to quit
kill %2 %1 2>/dev/null; wait
```

Bring home the same archive minus `hangard.log`. The bench numbers, the suite logs and
"eeye runs on this Pi as a static musl binary" are the result; the supervision test
moves to the next visit.

## Checklist

Before:
- [ ] `tools/release.sh` run today; MANIFEST's `skipped` line read
- [ ] tarball, this file, the Pi's address, a power supply, the camera
- [ ] laptop has `ssh`, `scp`, `zstd`

On the Pi:
- [ ] `00-machine.txt`: model, kernel ≥ 5.16, `/dev/shm` tmpfs, `video` group, `get_throttled` 0x0
- [ ] no ArduSub/eeye/hellion process running; electric-eye services stopped if present
- [ ] tarball unpacked, hashes ok, every binary `statically linked`
- [ ] suites: 3 passes, `summary.txt` all `ok`; any FAIL log saved
- [ ] `bench-default.json` (governor as found) with temperatures before/after
- [ ] `bench-performance.json` with temperatures before/after; governor restored
- [ ] `hangard` foreground; `eeye` ready; frame bus files growing; `hangar topics`, `hangar echo` output seen
- [ ] `eeye` SIGKILL: restart seen, back-off doubling seen, one log excerpt noted
- [ ] `hangard` SIGKILL: `eeye` survived; restarted manager adopted it (same PID)
- [ ] everything stopped; `pgrep` clean
- [ ] `hangar-visit-<date>.tar.gz` on the laptop

## What the results feed

| Result | Goes to |
|---|---|
| `publish 256 B`, `latest 256 B`, `idle`, `wake 1 kHz` from `bench-performance.json` (and `-default`) | the Targets table of [docs/00](../docs/00-overview.md#targets): each target beside its measured value, with the Pi model and governor |
| `sizes` of `hangard` (once in the tarball), `eeye`, `hellion` | the manager's "under 100 KB stripped" line; the electric-eye sizes beside `tools/cross/README.md`'s |
| the whole report, both governors | the paper's measurement table (`proposal/`): hangar's column, with machine, kernel and commit from the report's header |
| the same report | the ROS 2 comparison: `bench/ros2/` (to be written) runs a talker/listener pair in a container on this same Pi, same sizes, same rates, same governor, and the comparison script states the factor per line ([docs/12](../docs/12-tools.md#the-benchmark)). The container goes on the Pi only for that run, never on the vehicle |
| `suites/summary.txt` | `lib/hg/NOTES.md`'s "running the suites on AArch64" item, which stops being open |
| `hangard.log` (restart, back-off, adoption) | docs/03's tests list, as the first manager run on the target; findings become issues |
| `00-machine.txt`, `NOTES.txt` | the report's `machine` object is checked against them; the Pi model decides whether the next release is built `cortex_a72` or `cortex_a76` |
