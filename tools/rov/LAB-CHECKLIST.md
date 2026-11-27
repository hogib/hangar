# Lab plan: the session of measurements on equal terms (v0.1.6)

What to run at the lab next, in order, with what a pass looks like. Written 2026-11-27
for v0.1.6. It replaces the checklist of the v0.1.4 and v0.1.5 sessions (in this file's
history; what those sessions found is in [README.md](README.md), "Results", and in
`~/hangar-results/`).

**What this session is for.** One set of numbers that stands before a referee: hangar
and each baseline through the same cells, on a Pi that is in the same state for all of
them. Three things made earlier numbers unequal, and each has its answer here:

| What went wrong | Answer |
|---|---|
| `unattended-upgrade` started by itself and held a core (three visits) | `lab.sh freeze` (step 1): it cannot start until the Pi's next boot, and a measurement on a Pi that is not frozen is refused |
| The lab's eeye and hellion ran beside some blocks and not beside others; the governor changed half-way | the same freeze: both stopped and `performance` set for the whole session |
| Systems were measured one after the other, the later on a warmer Pi | `bench/pairs` (step 3): interleaved rounds, a temperature gate before every run, throttled runs repeated |

**There is no time limit.** The transport matrix alone is about four and a half hours
for each system, and it is made to be taken over several days and pooled
(`bench/pairs/README.md`). A day at the lab is: freeze, some rounds, thaw. The steps
below say what one day must hold and what can wait for the next.

The commands are `tools/rov/lab.sh` and `tools/rov/rov.sh`; [QUICKSTART.md](QUICKSTART.md)
has the commands and what to do when one fails, [README.md](README.md) the detail.
`hangar` on the Pi is `HANGAR_ROOT=/dev/shm/hangar-vehicle ~/hangar-root/bin/hangar`.

**Rules for every day**

- **Props off** for every armed step. Nothing here needs water.
- **A step marked 🔑 asks for the Pi's sudo password**: run it yourself, in your own
  terminal. Each prints what it will run as root before it asks.
- **`lab.sh down`, then `lab.sh thaw`, before leaving, whatever happened.** `down`
  restores ArduSub's parameters; `thaw` the apt timers, the lab's services and the
  governor. `status` and `down` say `THE PI IS FROZEN` until it is thawed. A thaw that
  is forgotten ends with the Pi's next boot by itself.
- **Nothing is built or installed on the Pi during a measurement.** Building heats it,
  and what is installed half-way through changes the machine between two rounds.
- **A run that was not clean is not repaired by hand.** The driver repeats a throttled
  run by itself and marks what never ran clean; a block with `NOT-CLEAN` in its folder
  is taken again another day, not quoted with a footnote.
- **Disarm before anything that stops hangar** (README, "The guard").

## Before leaving home

| | Check |
|---|---|
| ☐ | `git describe` on the laptop says `v0.1.6` or later; `make -j8 all` |
| ☐ | The vehicle's release under `out/release/` (`tools/release.sh`): it has `bin/hangar_pair` and `bin/null_pair` beside `hg_bench` (`tar -tf … \| grep _pair`) |
| ☐ | A rehearsal passes: `tools/release.sh --host`, then `tools/rov/rehearse.sh out/release/hangar-<…>-x86_64.tar.zst plain bridge serial both` ends with `rehearse: all passed`. The `plain` run freezes and thaws a stand-in Pi |
| ☐ | The harness proved here: `make check-pairs`, and the smoke plan (`bench/pairs/README.md`, the first lines) |
| ☐ | **The baselines, packaged for aarch64**, so that nothing compiles on the Pi: "Baselines" below. Each one's node pair built, its entry in the registry, and the smoke plan run over it here or in an aarch64 container |
| ☐ | For step 2's one-second limit: nothing to prepare. The release has the module that never sleeps (`bin/spin`) and `rov.sh spin` runs the check |
| ☐ | The controller; QGroundControl starting; the battery charged (the soak of step 6 needs a full one) or a 12 to 16 V supply. The Pixhawk reads no battery voltage: `up --no-battery-check` |
| ☐ | The heat sink is on the Pi. Without it the Pi throttles under two busy cores and the matrix cannot run |

### Baselines

Decided (2026-11-27): the systems hangar is measured against. Everything for the Pi is
made on the laptop beforehand; nothing is downloaded or compiled there.

**At home, the day before** (each prints what it made):

| | | |
|---|---|---|
| ☐ | The Pi's own package list, if the Pi can be reached before the visit: `ssh bluerov2@192.168.2.10 "dpkg-query -W -f='\${Package} \${Version} \${Architecture}\n'" > pi-packages.txt` | without it the `.deb` list is computed against a stock image, which is a guess |
| ☐ | `tools/baselines-debs.sh --status pi-packages.txt` | `N files in out/baselines/debs`; **read the "upgraded" line**: apt wanting newer versions of packages the Pi already has means the Pi's ROS 2 is older than today's, and those files are in the list too |
| ☐ | `tools/baselines.sh` | `out/baselines/baselines-<version>-aarch64.tar.zst`: four static programs, and the ROS 2 node cross-built |
| ☐ | `tools/baselines.sh ros2-smoke` | every entry `ok`, a report with a row for each: the registry proved in a container |

**At the lab, in step 1**, after `deploy`:

| | | |
|---|---|---|
| ☐ | 🔑 `tools/rov/lab.sh baselines --debs out/baselines/debs` | every file as its MANIFEST says; `ok` for each program with its version; `ok ros2_pair_node (prebuilt) loads against this ROS 2`; the `dpkg -i` line shown, one password, and dpkg ending without an error |
| ☐ | If the prebuilt ROS 2 node does **not** load (the Pi's ROS 2 is of another date): on the Pi, before `freeze`'s measurements, `. /opt/ros/jazzy/setup.sh && sh ~/hangar-baselines/current/ros2-build.sh` | `ros2-build.sh: …/ros2_pair_node`. The one thing compiled on the Pi; then wait until it is cool again |

| System | Rows (registry entries) | On the Pi | How it gets there |
|---|---|---|---|
| ROS 2 Jazzy, Fast DDS | `ros2-fastdds`, `ros2-fastdds-loan`; and, said to be no comparison, `ros2-fastdds-waitset`, `ros2-fastdds-composed` | installed (2026-11-09) | the node in the archive |
| ROS 2 Jazzy, Cyclone DDS | `ros2-cyclonedds`, `ros2-cyclonedds-iceoryx` (RouDi a helper) | **not installed** | `.deb` files 🔑 |
| ROS 2 Jazzy, Zenoh (`rmw_zenoh`) | `ros2-zenoh`, `ros2-zenoh-shm` (the router a helper) | **not installed** | `.deb` files 🔑 |
| iceoryx2 0.10.1 | `iceoryx2` (woken), `iceoryx2-poll` (never sleeping) | nothing needed | a static program in the archive |
| Unix domain sockets | `unix-seqpacket`, `unix-stream`: the floor | nothing needed | a static program in the archive |
| MOOS, V10 asynchronous comms | through a MOOSDB (a helper) | not installed | **node pair not written yet** |
| LCM | UDP multicast on loopback | not installed | **node pair not written yet** |

Every package is brought along as a file: nothing at the lab runs `apt update` or needs
the Pi to reach the internet.

## 0. The state as found (10 min)

| | | Pass |
|---|---|---|
| ☐ | The tether up, `ssh bluerov2@192.168.2.10 true` answers | no password asked (the key; `ssh-copy-id` if it is gone again) |
| ☐ | `tools/rov/lab.sh status` | what runs on the Pi before anything is touched: write down the lab's services, the temperature, `throttled=` |
| ☐ | `tools/rov/lab.sh preflight` | read every `warn` and `FAIL`: it changes nothing. The Pi's clock days behind is expected (no battery clock) |
| ☐ | ArduSub's parameters are saved by the first `up` of the session (step 2) and restored by `down` | — |

## 1. Freeze, deploy, install (30 min, more if an upgrade is running)

| | | Pass |
|---|---|---|
| ☐ | 🔑 `tools/rov/lab.sh freeze` | it prints each command, asks once, and ends with `frozen.` If an upgrade was running it waits for it (it says what it waits for, and never interrupts dpkg): let it. Status 3 means the package system still ran after 15 minutes: wait, and run `freeze` again |
| ☐ | 🔑 `tools/rov/lab.sh deploy --realtime` | `lab: installed v0.1.6 (…)`, then `cap_ipc_lock,cap_sys_nice=ep` on `hangard`. (`--realtime` is for step 2's check; a second password) |
| ☐ | 🔑 `tools/rov/lab.sh baselines --debs out/baselines/debs` ("Baselines" above) | each program `ok` with its version, the ROS 2 node loads, dpkg ends without an error |
| ☐ | The harness is in the baselines' archive (`~/hangar-baselines/current/bench/pairs`, with the preflight beside it): nothing more to copy | — |
| ☐ | Wait until the Pi is cool, then `tools/rov/lab.sh preflight --measure` | `ok pi frozen: since …`, no `FAIL`. A `warn` for real time is expected before step 2 |

From here on the Pi stays frozen until step 7 (the end of the day).

## 2. Checks, props off (about 75 min; once, on the first day)

Each of these is a first on the vehicle. They come before the measurements because
they decide whether real time is measured at all, and whether the preflight can be
believed.

| | | Pass |
|---|---|---|
| ☐ | **The preflight on the real Pi.** `lab.sh preflight` and `--measure` | the temperature and throttling lines read as `lab.sh status` does; the clock line within 2 s after `freeze`; nothing of the package system. Write down any line that reads wrong: it was built without the Pi |
| ☐ | **hangar up**: `tools/rov/lab.sh up --serial --bridge --no-battery-check` | `lab: READY TO ARM`, `lab: the link is up`; arm in QGroundControl, the thrusters follow, disarm |
| ☐ | **Real time in force, for the first time anywhere.** `lab.sh down`, then `tools/rov/lab.sh up --serial --bridge --no-battery-check --realtime` | `OK schedule` lines at the end of `up`. On the Pi: `hangar check --strict` exits 0; `hangar status` has no `not in force:` line; `hangar show safety`: its `in force` line says `fifo`, and locked memory above 0 kB |
| ☐ | The same session: arm, sticks, disarm | as before. A module that does not start under `fifo` is the finding: its log, and `hangard.log` |
| ☐ | **The one-second limit.** With `--realtime` up and disarmed, on the Pi: `~/hangar-root/share/hangar/rov.sh spin` (a module that never sleeps, under fifo at priority 5, below every module of hangar's) | `spin: ended after about 1.0 s: the limit holds`, status 0; the kernel kills it within about a second; the manager says so and backs off; SSH stays usable the whole time. **If SSH hangs: the Pi's power, and that is the finding** |
| ☐ | **`encoder.params` across a topside restart, with video.** `lab.sh down`, `lab.sh up --video --bridge --serial --no-battery-check`; `lab.sh camera brightness=140`; on the laptop `BRIDGE=1 tools/rov/rov.sh topside reset`, then `topside up`, then `HANGAR_ROOT=/dev/shm/hangar-topside builddir/hangar echo encoder.params --count 1` | the message comes at once, with the brightness that was set, and nothing was set again (before v0.1.6 the restarted topside saw none until the next change) |
| ☐ | **The camera's controls.** On the Pi: `v4l2-ctl -d /dev/video2 --list-ctrls`. Then `lab.sh camera brightness=…`, `auto_exposure=0 exposure_us=8000`, `gain=…` | the list (write it down: does the H.264 node take controls at all?); the picture in QGroundControl changes with each |
| ☐ | `tools/rov/lab.sh down` | `the session is down` and `THE PI IS FROZEN` (it is: that is right) |

## 3. The transport matrix, all systems interleaved (4.5 h a system; over as many days as it takes)

hangar down (`lab.sh status`: no manager), the Pi frozen. On the Pi, in
`~/hangar-baselines/current`, in a `tmux` or `screen` so that it outlives the SSH
connection. Every node, hangar's included, is the archive's (`B=$PWD/bin`): they were built
from one `pair.h` with one compiler. For the ROS 2 entries the shell has ROS 2 in it first:

```sh
cd ~/hangar-baselines/current && B=$PWD/bin
. /opt/ros/jazzy/setup.sh && . ros2/install/setup.sh
V="--var bin=$B --var base=$B --var ros=$PWD/ros2/install/ros2_pair/lib/ros2_pair --var ros_root=/opt/ros/jazzy"
```

| | | Pass |
|---|---|---|
| ☐ | The smoke plan first: `bench/pairs/driver.py --plan bench/pairs/plans/smoke.json $V --out ~/pairs-smoke` (a minute a system) | every system has `ok` at the start (one that is `SKIP`ped is named with what is missing); `report.md` has a row for each; nothing failed |
| ☐ | The plan: `bench/pairs/driver.py --plan bench/pairs/plans/full.json $V --out ~/pairs-$(date +%F) --reps N` | `round 0: preflight clean: …`. It stops by itself at a preflight `FAIL` and says which file has the line. `N` is what the day holds: one round is 26 minutes a system |
| ☐ | While it runs: leave the Pi alone. `tmux attach` to look; `vcgencmd measure_temp` is fine, a build or a copy over the tether is not | no `FLAGGED other-load` lines |
| ☐ | At the end of the day: the folder home, `scp -r bluerov2@192.168.2.10:pairs-<date> ~/hangar-results/` | `report.md` in it; its section "Runs repeated, flagged, failed or not run" read |
| ☐ | The next day: `freeze`, another `--out`, more rounds. Then `bench/pairs/driver.py --report ~/hangar-results/pairs-*` | ten rounds or more a cell in all; the table "Within a session and between sessions" says whether the days agree |
| ☐ | Once, on any of the days, in the same shell (ROS 2 sourced, from the SSH session itself: `systemd --user` needs the session): `bench/pairs/restart.py --bin $B --msg $PWD/msg --out ~/restart-$(date +%F)` (about 50 minutes: 20 kills a supervisor, 35 s apart) | five supervisors, none `SKIP`ped; `restart.md` has a row for each, and "not restarted" is `0 of 10` in every row. These rows restart a process and say nothing of thrusters or arming: step 5 does |

The plan's cells, conditions and repetitions are `bench/pairs/plans/full.json`; change
the file, not the command: the file as it was is copied into the results.

## 4. The same with real time on, hangar only (4.5 h, a day of its own)

The full plan has a `realtime` condition, which the driver runs only where the user may
have a real-time priority. On the Pi that is one more sudo, for the shell the driver is
started from, and gone when that shell ends:

| | | Pass |
|---|---|---|
| ☐ | 🔑 On the Pi, in the shell that will run the driver: `sudo prlimit --pid $$ --rtprio=50:50` | `ulimit -r` says 50. **Not tried anywhere yet** |
| ☐ | `bench/pairs/driver.py --plan bench/pairs/plans/full.json --only null,hangar,hangar-inplace --var bin=$HOME/hangar-root/bin --out ~/pairs-rt-$(date +%F)` | no `NOTE condition realtime: this user may not …` at the start; the `realtime` cell has figures, beside `quiet` and `load2` of the same rounds |
| ☐ | The whole matrix under `fifo` (every cell, not the one condition) is a plan of its own: a copy of `full.json` with `"realtime"` in every sweep's `condition` | optional; the results say which plan it was |

## 5. Cost and failure tests on serial (about 2 h)

| | | Pass |
|---|---|---|
| ☐ | `tools/rov/lab.sh up --serial --bridge --no-battery-check`, then `tools/rov/lab.sh bench 60`, three times, disarmed; once armed with the sticks moving | a folder each under `~/hangar-results/`, none with `NOT-CLEAN`; hangar's CPU near 1.9 % of a core idle (2026-11-25: 1.86) |
| ☐ | The same with `--video` | the encoder's share, the frame rate |
| ☐ | `tools/rov/failtest.py --props-off all` | every block's `preflight: no FAIL`; the times to neutral as on 2026-11-25. Now on a frozen Pi, with eeye and hellion stopped in every block alike |
| ☐ | `tools/rov/failtest.py --props-off all --load 2` | the same under two busy cores |
| ☐ | With `--realtime` (step 2 passed): `bench 60` and `failtest.py --props-off all` again | beside the numbers without: this is where real time should show, if it shows anywhere |

## 6. Soak, if battery and time allow (1 h)

| | | Pass |
|---|---|---|
| ☐ | Armed, serial with video, an hour: `~/hangar-results/2026-11-25/scripts/soak.py` (written for that session, not run then) | no failsafe, no module restarted, the temperature flat, no growth in any process's memory |

## 7. Thaw, everything as found (10 min; every day)

| | | Pass |
|---|---|---|
| ☐ | Disarmed. `tools/rov/lab.sh down` | `the session is down`, `THE PI IS FROZEN`; ArduSub's parameters restored and compared |
| ☐ | 🔑 `tools/rov/lab.sh thaw` | `lab: thawed`: the apt timers, eeye, hellion and the governor as `freeze` found them |
| ☐ | `tools/rov/lab.sh status` | no `FROZEN` line; `eeye: active`, `hellion: active` (if they were); no manager |
| ☐ | If `deploy --realtime` was used and the lab should not keep it: on the Pi, `sudo setcap -r ~/hangar-root/bin/hangard` 🔑 | `getcap` prints nothing. (It is on hangar's own file, in hangar's own install) |
| ☐ | `pgrep -a hangard` on the laptop shows nothing | — |

## If the time is short

What one day must hold: steps 0, 1, the first two lines of 2, the smoke plan and at
least one round of step 3, and 7. In this order, drop: step 6; step 4; the camera's
controls and the one-second limit of step 2; the `--load` and `--realtime` lines of
step 5. Do not drop the freeze, the preflight or the thaw, and do not shorten a plan to
fit a day: run fewer rounds of the whole plan, and pool.

## What to bring back

- Every results folder, with its `report.md` read: `~/pairs-*` from the Pi,
  `~/hangar-results/<date>-lab*` and `-failtests` from the laptop.
- For each line of step 2: passed, failed with what it said, or not reached. The four
  that have never run in force anywhere: a module under `fifo`, memory locked, the
  kill of a module that never sleeps, file capabilities on the Pi's filesystem.
- `v4l2-ctl --list-ctrls` of the camera's H.264 node.
- What `freeze` printed the first time, whole: it is the first run of it on a real
  systemd, and the first SIGTERM to a real `unattended-upgrade` if one was running.
- Anything the preflight said that was wrong for this Pi.
- The logs of anything that failed: on the Pi, `/dev/shm/hangar-vehicle/hangard.log`
  and `/dev/shm/hangar-vehicle/run/hangar/log/<module>.log`, copied before `lab.sh down`
  removes the root.
