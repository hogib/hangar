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

## Not tried yet

- `rov.sh params restore`: it loads the differing parameters into MAVROS's parameter
  node, which sends them to the Pixhawk; the diff it runs afterwards is the proof.
  Until it has passed once, `hangar ardusub-setup` with a file of the old values is the
  other way back.
- Signing against the real ArduSub (`Signing=no` here).
- Steps 4 to 8 on this vehicle: on 2026-11-09 the battery was not connected, and
  `safety` refused to clear the failsafe, correctly.
