# Lab checklist: the first session with v0.1.4

What to run at the next lab visit, in order, with what a pass looks like. Written
2026-11-21 for v0.1.4. Nothing new since v0.1.2 has run on the vehicle: steps 1 and 2 repeat
what v0.1.2 passed, and every step after them is a first.

The commands are `tools/rov/lab.sh` and `tools/rov/rov.sh`; [QUICKSTART.md](QUICKSTART.md)
has the five commands and what to do when one fails, [README.md](README.md) the detail.
Below, `rov.sh` on the Pi is `~/hangar-root/share/hangar/rov.sh`, and `hangar` on the Pi
is `HANGAR_ROOT=/dev/shm/hangar-vehicle ~/hangar-root/bin/hangar`.

**Rules for the whole session**

- **Props off** for every armed step here. Nothing below needs water.
- **Stop at the first step that fails,** write down what it said, and go to "Before
  leaving". Later steps build on earlier ones.
- **`lab.sh down` before leaving, whatever happened.** It restores ArduSub's parameters
  and the lab's services, and names anything it could not put back.
- A fan on the Pi, or the tube's lid off: closed on the bench it reached 86 to 91 °C in
  earlier sessions and throttled.

## Before leaving for the lab

| | Check |
|---|---|
| ☐ | `git pull` on the laptop; `git describe` says `v0.1.4` or later |
| ☐ | `make -j8 all` (a plain `make` does not build the modules) |
| ☐ | The vehicle's release: v0.1.4's `hangar-v0.1.4-aarch64.tar.zst` under `out/release/v0.1.4/` with the checkout at the tag, or one **built from this checkout** if it is past the tag (`tools/release.sh`; `lab.sh deploy` takes the newest under `out/release/`). The v0.1.3 tarball will not do for steps 3 to 5: `up --bridge` and `up --serial` need this checkout's `rov.sh` on the Pi and say so if it is older, and with the bridge both ends must be the same build, or nothing crosses (`up --bridge` compares them before it starts anything) |
| ☐ | A rehearsal on the laptop passes: `tools/release.sh --host`, then `tools/rov/rehearse.sh out/release/hangar-<…>-x86_64.tar.zst` ends with `rehearse: all passed` (QUICKSTART, "Rehearsing without the vehicle"; `make check` first, for the fake ArduSub) |
| ☐ | The controller, and QGroundControl starting |
| ☐ | The battery or a 12 to 16 V supply for the vehicle. The Pixhawk reads no battery voltage, so `up` needs `--no-battery-check` (QUICKSTART, "The battery check") |

## 1. Deploy, and the v0.1.2 checks again (15 min)

This is also the first run of the AArch64 manager built with the size flags.

```sh
tools/rov/lab.sh deploy
tools/rov/lab.sh up --no-battery-check
tools/rov/lab.sh status
```

| | Pass |
|---|---|
| ☐ | `lab: installed v0.1.4 (…)`, then `lab: READY TO ARM` |
| ☐ | `status`: every module `ready`, `throttled=0x0` |
| ☐ | Arm in QGroundControl, disarm: ArduSub follows within about 0.5 s each way |
| ☐ | Armed, kill safety on the Pi (`pkill -KILL -f "[h]angar-safety"`): ArduSub disarmed and the thrusters neutral within about 0.3 s (267 ms on 2026-11-19). Then `hangar call safety.clear` |

New since v0.1.2, to notice here: an `arm` sent while safety is restarting is refused with
"safety restarted after this arm was sent: send it again". Send it again.

## 2. The cost on the Pi (5 min)

```sh
tools/rov/lab.sh bench 60
```

| | Pass |
|---|---|
| ☐ | All of hangar at 5 to 6 % of a core, as on 2026-11-20. The table now has a wake-ups column: write down the two largest |
| ☐ | Saved under `~/hangar-results/<date>-lab` |

## 3. The bridge on the tether (30 min). First time

The topside role moves to the laptop. Step 1's session stays open (its parameter backup
is the one `down` restores); `up` again lays the vehicle out with the bridge:

```sh
tools/rov/lab.sh up --bridge --no-battery-check
tools/rov/lab.sh status
tools/rov/lab.sh bench 60
```

`up --bridge` compares the two builds, starts the vehicle role on the Pi, copies
`bridge.key` from the Pi to the laptop's `~/hangar-rov/` (mode 600; never into the
repository), starts the topside role on the laptop from `builddir/`, and waits for the
link. Rehearsed on loopback with both roles on the laptop; the copy of the key over
`scp`, and the link on the tether, have never run. By hand, if `lab.sh` gets in the way:
README, "The topside on the laptop" (`BRIDGE=1 rov.sh up` on the Pi, `BRIDGE=1
tools/rov/rov.sh topside up` on the laptop; then the parameters are yours to save and restore).

Below, `hangar` on the laptop is `HANGAR_ROOT=/dev/shm/hangar-topside builddir/hangar`.

| | Pass |
|---|---|
| ☐ | `up`: `lab: both ends are the same build`, then `lab: the link is up: up 192.168.2.10:5614, rtt … ms, offset … ms`, then `lab: READY TO ARM`. `NOT THE SAME BUILD` stops it before anything starts: deploy a release of this checkout |
| ☐ | `status`: under `== the Pi`, `bridge  ready … up 192.168.2.1:5614`; under `== this laptop`, `bridge  ready … up 192.168.2.10:5614` and `topside_mavlink … link up`. Write down the rtt and the clock offset |
| ☐ | `bench`: `hangar-bridge` in both tables; the link topics before and after are in the results' README (`loss_rx`, `retransmits`, `rejected`) |
| ☐ | `hangar call safety.check` on the laptop answers across the tether |
| ☐ | QGroundControl on the laptop connects, arms and disarms through the bridge |
| ☐ | The controller drives (thrusters turn, props off) |

If the link does not come up, three things are new since v0.1.2:

- A fixed `Peer=` must be the address **and port** the other end sends from. A laptop with
  two addresses on the tether interface is refused; the bridge's log says so once.
- The laptop's firewall must let UDP 5614 in from the Pi.
- Before this checkout, `rov.sh topside up` had the laptop's `topside_mavlink` bind the
  Pi's address, which fails on the tether (it never showed on loopback). Fixed, and the
  fix has not met the tether either: if QGroundControl does not connect, look at
  `/dev/shm/hangar-topside/run/hangar/log/topside_mavlink.log` first.
- `hangar echo bridge.topside.link` has the counters: a rising `rejected` means datagrams
  arrive and are refused.

## 4. The tether trigger (15 min). First time, props off

With the bridge the trigger is on: arming needs the link up. Stay in step 3's setup
(`lab.sh up --bridge`).

| | Do | Pass |
|---|---|---|
| ☐ | Link down (stop the laptop's topside: `BRIDGE=1 tools/rov/rov.sh topside down`), wait 2 s, then try to arm on the Pi: `hangar call safety.arm` | Refused: `tether: no packet from the topside for … s` (from 1.5 s after the last packet, half of `tether_timeout`), and `bridge.vehicle.link connecting` once the Pi's bridge has called the link lost (about 3 s) |
| ☐ | Topside up again (`BRIDGE=1 tools/rov/rov.sh topside up`; `lab.sh status` shows the link), arm, then **unplug the tether** at the laptop | Neutral failsafe and ArduSub disarmed about 3 s after the unplug (`tether_timeout`). Time it |
| ☐ | Plug in again; when the link is up, `hangar call safety.clear` | Cleared; arming works again. Before the link is back, `clear` is refused |
| ☐ | Armed, kill the **vehicle's** bridge: on the Pi `pkill -KILL -f "[h]angar-bridge"` | Neutral within about 0.3 s (the `input` trigger: the link topic went stale). The manager restarts the bridge; when the link is up, `clear` |
| ☐ | Armed, unplug for 1 s and plug in again | Nothing fires |

This was tested against a stand-in for the bridge only. If a short hiccup of the link
disarms the vehicle, or the 0.3 s rule fires on an ordinary bridge restart more often
than is bearable, write down when: the alternative (judge a silent bridge by the 3 s
timeout alone) is a small change.

## 5. Serial MAVLink, without MAVROS (30 min). First time

MAVROS is the largest cost on the vehicle (28 % of a core, 128 MB). The mavlink module can
hold the Pixhawk's USB serial itself:

```sh
tools/rov/lab.sh up --serial --no-battery-check      # with --bridge too, if step 3 passed
tools/rov/lab.sh status
tools/rov/lab.sh bench 60
```

`up --serial` lays the vehicle out from the profile's `mavlink.conf.serial` and
`mavlink.module.serial` (`rov.sh`'s `MAVLINK=serial`); the installed profile is not
changed, so there are no files to put back. Rehearsed against the tests' fake ArduSub on
a pseudo-terminal, parameters saved, set and restored; **never on the Pixhawk's
`/dev/ttyACM0`.** If the module does not come up `ready`, `lab.sh up --no-battery-check`
(without `--serial`) goes back to the relay in the same session. By hand: `MAVLINK=serial
rov.sh up` on the Pi after `rov.sh reset`.

| | Pass |
|---|---|
| ☐ | `up` does not say `/dev/ttyACM0 on the Pi is held by something else`. If it does, it has changed nothing and names the process and its systemd unit: stop it, or put the unit in `LAB_SERIAL_SERVICES` (`lab.conf`) for `up` to stop and `down` to start again; write down what it was |
| ☐ | `up`: `lab: saving ArduSub's parameters …` only if this is the session's first `up`; then `lab: READY TO ARM` |
| ☐ | `status`: `mavlink  ready … serial open; ardusub disarmed`, MANUAL; `MAVROS relay: not running (--serial: …)` |
| ☐ | On the Pi, `rov.sh params via` says `hangar`; `rov.sh params diff <the session's backup>` lists only what `ardusub-setup` set |
| ☐ | Arm, disarm, kill safety while armed: as in step 1 |
| ☐ | `bench`: no `mavros_node` in the Pi's table, and the mavlink module's own CPU written down |
| ☐ | The module's log has few `resync` lines (noise on the line) |

## 6. The ROS 2 baseline (60 min, mostly waiting)

For the paper's comparison. The steps and their commands are in
[bench/ros2/README.md](../../bench/ros2/README.md), "On the Pi, step by step"; in short:

| | |
|---|---|
| ☐ | hangar down on the Pi, the lab's `eeye` and `hellion` stopped, nothing else running, `throttled=0x0` |
| ☐ | `bench/ros2` and `bench/procbench.py` copied to the Pi (the README's `tar … \| ssh` line) |
| ☐ | `bench/ros2/run.sh --quick --hg-bench ~/hangar-root/bin/hg_bench` runs and names the middlewares it found |
| ☐ | The run itself (about 40 minutes with Fast DDS alone), then under `performance` if there is time |
| ☐ | `eeye` and `hellion` started again; the results copied to `~/hangar-results/<date>-ros2` |

It needs `cmake` and a C++ compiler on the Pi; if either is missing it stops, having
changed nothing, and the comparison waits for whoever owns the Pi.

## 7. If there is time

| | | Pass |
|---|---|---|
| ☐ | **The native video path.** Today `lab.sh up --video` uses electric-eye on the laptop. hangar's own `video_relay`, `vision` and `topside_encoder` have not met the real camera | A picture in QGroundControl through them; this decides when `legacy/` can go |
| ☐ | **The camera unplugged** while the encoder runs | The encoder exits and is restarted with a back-off; plugged in again, the picture returns without a restart of hangar |
| ☐ | **The console in a browser** (the address `lab.sh up` prints; by address, not by host name) | The Fly view shows the state; Arm is greyed out while the state is unknown; Disarm and E-STOP never are |
| ☐ | **The joystick module** instead of QGroundControl's (README; needs the bridge) | The thrusters follow the sticks with QGroundControl closed |

## Before leaving

| | |
|---|---|
| ☐ | Disarmed |
| ☐ | `tools/rov/lab.sh down`, whichever of `--bridge`, `--serial` and `--video` the last `up` had: it disarms, restores ArduSub's parameters from the session's first backup and compares them (over the serial line if the session was on it), stops the topside on the laptop and hangar on the Pi, and starts the lab's services it stopped |
| ☐ | `lab: everything as found`. If it names a step that failed, fix that and run `down` again |
| ☐ | Only if `rov.sh` was used by hand instead: `rov.sh params restore <the file saved first>` and `rov.sh params diff <it>` says nothing differs; `rov.sh down` on the Pi, `BRIDGE=1 tools/rov/rov.sh topside reset` on the laptop |
| ☐ | The lab's services running again on the Pi: `systemctl is-active eeye hellion` says `active` twice (and any unit named in `LAB_SERIAL_SERVICES`) |
| ☐ | `pgrep -a hangard` on the laptop shows nothing. `~/hangar-rov/bridge.key` stays on the laptop (and on the Pi) for the next visit; it is not in the repository |
| ☐ | The results under `~/hangar-results/`, and for every step: passed, failed with what it said, or not reached |

## What to bring back

- For each step, pass or fail, and the numbers asked for (times to neutral, the rtt, CPU).
- The logs of anything that failed: on the Pi, `/dev/shm/hangar-vehicle/hangard.log` and
  `/dev/shm/hangar-vehicle/run/hangar/log/<module>.log` (copy them before `rov.sh reset`
  or `lab.sh down`, which remove the root), and `~/hangar-rov/relay.log`.
- Whether the tether trigger's two rules (step 4) are bearable in practice.
- The ROS 2 results directory.
