# hangar on the lab's BlueROV2

How hangar drives the lab's vehicle in phase 1 (`docs/07`, `docs/10`): ArduSub held in
MANUAL, hangar between QGroundControl and the autopilot. Both roles run on the
vehicle's Pi until the bridge exists; `rov.sh` lays them out from `profiles/rov-vehicle`
and `profiles/rov-topside` and starts them. In a release the script is
`share/hangar/rov.sh` and this file `share/doc/ROV.md`.

## The vehicle

Found on 2026-11-09 (`bench/PI-TEST.md`, results):

| | |
|---|---|
| Computer | Raspberry Pi 5, 8 GB, Ubuntu 24.04, kernel 6.8; login `bluerov2`, `192.168.2.10` on the tether |
| Tether | Fathom-X; the laptop is `192.168.2.1` (a USB Ethernet adapter) |
| Autopilot | a Pixhawk (fmuv3) on USB, `/dev/ttyACM0`, ArduSub V4.5.5, frame VECTORED_6DOF |
| Depth | no external sensor: `nav.depth` from the Pixhawk's barometer (`From=SCALED_PRESSURE`) |
| Also on the Pi | ROS 2 Jazzy with MAVROS (idle); electric-eye's `eeye` and `hellion` as systemd services; no MAVLink router of its own |
| Clock | no RTC and no time sync over the tether: set it by hand (`sudo date -u -s "<laptop's UTC>"`) |

ArduSub's parameters as found (the full set: `~/hangar-visit/pixhawk/pixhawk-before.yaml`
on the Pi): `ARMING_CHECK 0` (ArduSub's own pre-arm checks are off; hangar's `safety` is
the only check), `JS_GAIN_DEFAULT 0.1`, `FS_GCS_ENABLE 2`, `BTN15_FUNCTION 0`,
`SYSID_MYGCS 255`.

## What runs where

```
 laptop 192.168.2.1                 Pi 192.168.2.10
 QGroundControl :14550 ◄─────────── topside_mavlink (topside root, :14650)
                                        │ pilot.wrench, safety.arm   (same-host shim)
                                        ▼
                                    mux ─► mavlink (vehicle root, 127.0.0.1:14560)
                                                │ UDP
                                    MAVROS (127.0.0.1:14555) ── USB ── Pixhawk / ArduSub
```

The vehicle root is `/dev/shm/hangar-vehicle` (safety, mux, mavlink, recorder, console on
`127.0.0.1:8080`); the topside root `/dev/shm/hangar-topside` (topside_mavlink, console
on `:8081`). Recordings go to `~/hangar-sessions`, one session per arming. The consoles
are reached with `ssh -L 8080:127.0.0.1:8080 -L 8081:127.0.0.1:8081 bluerov2@192.168.2.10`.

MAVROS is only the serial-to-UDP relay: the vehicle module speaks UDP and nothing else
on the Pi owns the Pixhawk. It runs as MAVLink system 2: before 3a1dfeb the vehicle
module took MAVROS's own system-1 heartbeat for ArduSub's (`modules/mavlink/README.md`,
deviation 15).

## A session, in order

Props off until step 6 has passed. Stop at the first step that does not do what it says.

0. **The Pi as it was.** If an earlier run may still be up: `rov.sh down` (or, after the
   2026-11-09 run, `~/hangar-bench.sh down` and `pkill -INT -f "[m]avros_node"`). Then
   `pgrep -a hangard; pgrep -a mavros` shows nothing.
1. **Install.** Copy the release tarball, unpack, `sh share/hangar/install-root.sh
   ~/hangar-root`. The script is then `~/hangar-root/share/hangar/rov.sh`.
2. **Power.** The main battery connected (or a 12–16 V supply on the power module).
   Without it the Pixhawk reads under a volt, the ESCs have no power, and `safety` holds
   the vehicle in failsafe (`battery_min` 11 V): nothing arms.
3. **Up.** `rov.sh up`, then `rov.sh status` after half a minute: every module `ready`;
   `mavlink` says `ardusub disarmed mode 19` and counts frames received; `safety` says
   `disarmed`.
4. **ArduSub's parameters.** `rov.sh params save` (a full copy, before anything is
   written), then `HANGAR_ROOT=/dev/shm/hangar-vehicle ~/hangar-root/bin/hangar
   ardusub-setup` and `... hangar check`, which must say `ok 1`.
5. **QGroundControl** on the laptop: it shows the vehicle (hangar's topside module), in
   MANUAL, disarmed. The joystick under Vehicle Setup.
6. **Props off, then:** arm from QGC → `safety.state` armed and ArduSub armed; disarm →
   both disarmed within a second; arm again and `kill -KILL` the `hangar-safety` process
   → neutral and ArduSub disarmed, failsafe latched; `hangar call safety.clear`. Note
   each time from `safety.log` and `mavlink.log` under `/dev/shm/hangar-vehicle/run/hangar/log/`.
7. **Drive**, props on, the vehicle held or in the tank. The joystick gains are pinned to
   0.25 (`profiles/rov-vehicle/ardusub.params`); the mux's `rate_max` of 8/s felt
   sluggish in the simulator, 50 did not (`hangar param set mux rate_max=50,50,50,50,50,50`).
8. **Put it back.** `rov.sh params restore <the file from step 4>` (it sets back only
   what differs, then compares again), `rov.sh down`, and check `systemctl is-active
   eeye hellion`. The sessions in `~/hangar-sessions` come home.

## Results, 2026-11-19

The second visit, with the v0.1.1 release; props off, the battery connected.

- **Arm and disarm through hangar**, from `hangar call` and from QGroundControl
  (`armed by topside:topside_mavlink`): ArduSub followed within about 0.5 s each way.
- **`safety` killed with `SIGKILL` while armed:** ArduSub disarmed and the gate neutral
  within 267 ms; the manager restarted `safety`. Killed while in failsafe, it came back
  in its latched failsafe.
- **ArduSub armed while hangar was in failsafe:** disarmed by hangar within about
  0.5 s, three times.
- **Driving from QGroundControl** with an Xbox controller, through `topside_mavlink`,
  the mux and the gate: the stick reached `thr.out`. The joystick page is Q icon →
  Vehicle Setup → Joystick (enable it there; QGC sends nothing until then).
- **`rov.sh params restore`** put every parameter back; `params diff` then compared
  clean. ArduSub's own counters (`STAT_*`) and the barometer's ground pressure differ
  after any session and are left out of the comparison.
- **electric-eye alone**, streaming 1280×720 to a topside hellion: 0.91 of a core
  (`ffmpeg`'s libx264 encode 0.51, `eeye` 0.24, `hellion` 0.16), 117 MB PSS; the Pi
  was soft-throttled at 1.6 GHz then, so these are high.

Found on the vehicle:

1. **ArduSub 4.5 sends no telemetry for about a second at every arm** (1015 ms on
   both `nav.depth` and `nav.attitude`). Not its SD-card log: `LOG_DISARMED 1` left it
   unchanged. With `nav.depth`'s `StaleAfter` at 500 ms `safety`'s input trigger fired
   at every arm, so this profile has 1.5 s (`profiles/rov-vehicle/mavlink.module`).
2. **The Pixhawk reads no battery voltage** (0.73 V with the battery connected;
   `BATT_MONITOR 4`, pins 2 and 3): the power module's sense cable, most likely. So
   ArduSub's own battery failsafe cannot work on this vehicle, and the bench session
   ran with `battery_min=0 battery_required=0` (live only; the profile keeps 11 V).
   For the lab to look at.
3. **`hangar-safety`, `hangar-mux` and the recorder spin at a full core each**,
   since phase 1, in the simulator too; nobody had measured. A glob match that a
   module never reads keeps `hg_wait` returning at once: `hg_serve` serves a request
   topic through its named subscription and never reads the `*.req` glob's copy of it
   (`lib/hg/hg_call.c`, `hg_serve`'s `is_named_sub`), and the recorder skips matches
   bigger than its read buffer or past its topic limit without reading them
   (`modules/recorder/recorder.c`, the read loop). On the vehicle that was 1.7 cores,
   and with the closed tube on the bench the Pi reached 91 °C and throttled to
   1.0 GHz. **Fixed on 2026-11-20** (4787cae, 39d4679; hangar#1): four places, not
   two (the mux and the pairer as well, and `safety` also on another caller's
   replies), each with a test that fails on the old code. To confirm on the vehicle
   with `bench/procbench.py`: every hangar module under 1 % of a core at rest.
4. **The Pi runs hot on the bench:** 84–88 °C with electric-eye alone, soft-throttled
   at 1.6 GHz. Keep closed-tube bench sessions short, or cool the tube.
5. Ubuntu's `unattended-upgrades` runs on the Pi and was installing in the middle of
   the session.

## Not tried yet

- Signing against the real ArduSub (`Signing=no` here).
- Driving with props on, in the tank.
