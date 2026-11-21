# Lab checklist: the first session with v0.1.3

What to run at the next lab visit, in order, with what a pass looks like. Written
2026-11-21 for v0.1.3. Nothing new in v0.1.3 has run on the vehicle: steps 1 and 2 repeat
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
| ☐ | `git pull` on the laptop; `git describe` says `v0.1.3` or later |
| ☐ | `make -j8 all` (a plain `make` does not build the modules) |
| ☐ | The vehicle's release is there: `ls out/release/v0.1.3/hangar-v0.1.3-aarch64.tar.zst`. If the checkout is past the tag, build one for it (`tools/release.sh`): with the bridge, **both ends must be the same commit**, or nothing crosses |
| ☐ | A rehearsal on the laptop passes: `LAB_HOST=local` with `deploy`, `up`, `status`, `down` (QUICKSTART, "Rehearsing without the vehicle") |
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
| ☐ | `lab: installed v0.1.3 (…)`, then `lab: READY TO ARM` |
| ☐ | `status`: every module `ready`, `throttled=0x0` |
| ☐ | Arm in QGroundControl, disarm: ArduSub follows within about 0.5 s each way |
| ☐ | Armed, kill safety on the Pi (`pkill -KILL -f "[h]angar-safety"`): ArduSub disarmed and the thrusters neutral within about 0.3 s (267 ms on 2026-11-19). Then `hangar call safety.clear` |

New in v0.1.3, to notice here: an `arm` sent while safety is restarting is refused with
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

The topside role moves to the laptop. `lab.sh` does not drive this yet: it is `rov.sh` on
both machines (README, "The topside on the laptop").

```sh
tools/rov/lab.sh down                      # the shim's session, closed
# on the Pi:
rov.sh reset && BRIDGE=1 rov.sh up
# on the laptop, the key once (never into the repository):
mkdir -p ~/hangar-rov && scp bluerov2@192.168.2.10:hangar-rov/bridge.key ~/hangar-rov/ && chmod 600 ~/hangar-rov/bridge.key
BRIDGE=1 tools/rov/rov.sh topside up
HANGAR_ROOT=/dev/shm/hangar-topside builddir/hangar status
```

`rov.sh` alone does not save ArduSub's parameters as `lab.sh up` does: on the Pi,
`rov.sh params save` before arming, and `rov.sh params restore <that file>` at the end.

| | Pass |
|---|---|
| ☐ | The laptop's `status`: `bridge  ready  up 192.168.2.10:5614, rtt … ms`; the Pi's the same toward `192.168.2.1:5614`. Write down the rtt and the clock offset |
| ☐ | `config_match` is not false. If it is, the line says what differs (usually the two ends are not the same commit) |
| ☐ | `hangar call safety.check` on the laptop answers across the tether |
| ☐ | QGroundControl on the laptop connects, arms and disarms through the bridge |
| ☐ | The controller drives (thrusters turn, props off) |

If the link does not come up, three things are new in v0.1.3:

- A fixed `Peer=` must be the address **and port** the other end sends from. A laptop with
  two addresses on the tether interface is refused; the bridge's log says so once.
- The laptop's firewall must let UDP 5614 in from the Pi.
- `hangar echo bridge.topside.link` has the counters: a rising `rejected` means datagrams
  arrive and are refused.

## 4. The tether trigger (15 min). First time, props off

With `BRIDGE=1` the trigger is on: arming needs the link up. Stay in step 3's setup.

| | Do | Pass |
|---|---|---|
| ☐ | Link down (stop the laptop's bridge: `BRIDGE=1 tools/rov/rov.sh topside down`), then try to arm on the Pi: `hangar call safety.arm` | Refused, and the reason names the link |
| ☐ | Topside up again, arm, then **unplug the tether** at the laptop | Neutral failsafe and ArduSub disarmed about 3 s after the unplug (`tether_timeout`). Time it |
| ☐ | Plug in again; when the link is up, `hangar call safety.clear` | Cleared; arming works again. Before the link is back, `clear` is refused |
| ☐ | Armed, kill the **vehicle's** bridge: on the Pi `pkill -KILL -f "[h]angar-bridge"` | Neutral within about 0.3 s (the `input` trigger: the link topic went stale). The manager restarts the bridge; when the link is up, `clear` |
| ☐ | Armed, unplug for 1 s and plug in again | Nothing fires |

This was tested against a stand-in for the bridge only. If a short hiccup of the link
disarms the vehicle, or the 0.3 s rule fires on an ordinary bridge restart more often
than is bearable, write down when: the alternative (judge a silent bridge by the 3 s
timeout alone) is a small change.

## 5. Serial MAVLink, without MAVROS (30 min). First time

MAVROS is the largest cost on the vehicle (28 % of a core, 128 MB). The mavlink module can
hold the Pixhawk's USB serial itself. **Not rehearsed with `rov.sh`;** if the module does
not come up `ready`, put the two files back and go on with the relay.

```sh
# on the Pi, hangar down first (rov.sh down)
cd ~/hangar-root/share/hangar/profiles/rov-vehicle
cp mavlink.conf mavlink.conf.udp && cp mavlink.module mavlink.module.udp
cp mavlink.conf.serial mavlink.conf && cp mavlink.module.serial mavlink.module
pgrep -af "[m]avros_node"                  # nothing may hold /dev/ttyACM0
rov.sh reset && RELAY=0 rov.sh up          # with BRIDGE=1 too, if step 3 passed
```

| | Pass |
|---|---|
| ☐ | `hangar status`: `mavlink  ready`, ArduSub heard, MANUAL |
| ☐ | `rov.sh params via` says `hangar`; `rov.sh params save` writes a full file (about a thousand lines) |
| ☐ | Arm, disarm, kill safety while armed: as in step 1 |
| ☐ | The cost again, on the Pi: `sudo python3 -I ~/hangar-rov/procbench.py 60` (step 2's `lab.sh bench` left the script there): no `mavros_node`, and the mavlink module's own CPU written down |
| ☐ | The module's log has few `resync` lines (noise on the line) |

Afterwards, to go back: copy `mavlink.conf.udp` and `mavlink.module.udp` over the two
files, `rov.sh reset`.

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
| ☐ | ArduSub's parameters as found: `lab.sh down` does it for a `lab.sh` session; after `rov.sh` sessions, `rov.sh params restore <the file saved first>`, and `rov.sh params diff <it>` says nothing differs |
| ☐ | Step 5's two files put back, if they were changed |
| ☐ | hangar down on both machines (`rov.sh down` on the Pi, `BRIDGE=1 tools/rov/rov.sh topside down` on the laptop), then `tools/rov/lab.sh down` |
| ☐ | The lab's services running again on the Pi: `systemctl is-active eeye hellion` says `active` twice |
| ☐ | `lab: everything as found` |
| ☐ | The results under `~/hangar-results/`, and for every step: passed, failed with what it said, or not reached |

## What to bring back

- For each step, pass or fail, and the numbers asked for (times to neutral, the rtt, CPU).
- The logs of anything that failed: on the Pi, `/dev/shm/hangar-vehicle/hangard.log` and
  `/dev/shm/hangar-vehicle/run/hangar/log/<module>.log` (copy them before `rov.sh reset`
  or `lab.sh down`, which remove the root), and `~/hangar-rov/relay.log`.
- Whether the tether trigger's two rules (step 4) are bearable in practice.
- The ROS 2 results directory.
