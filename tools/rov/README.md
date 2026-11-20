# hangar on the lab's BlueROV2

How hangar drives the lab's vehicle in phase 1 (`docs/07`, `docs/10`): ArduSub held in
MANUAL, hangar between QGroundControl and the autopilot. Both roles run on the
vehicle's Pi by default; `rov.sh` lays them out from `profiles/rov-vehicle`
and `profiles/rov-topside` and starts them. With `BRIDGE=1` the topside role runs on the
laptop instead, joined by the tether bridge ("The topside on the laptop" below). In a release the script is
`share/hangar/rov.sh` and this file `share/doc/ROV.md`.

**A session from the laptop:** `tools/rov/lab.sh` (deploy, up, status, bench, down) does
every step below over SSH and puts it all back at the end; `tools/rov/QUICKSTART.md` is
the page to have open at the lab. It runs from a checkout of hangar, not from a release:
it uses the laptop's own builds (`builddir/hellion`, `builddir/eeye`), `bench/` and
`profiles/rov-laptop/`. The steps below are what it does, by hand.

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
   written; [below](#the-parameter-backup)), then `HANGAR_ROOT=/dev/shm/hangar-vehicle
   ~/hangar-root/bin/hangar ardusub-setup` and `... hangar check`, which must say `ok 1`.
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

## The parameter backup

`rov.sh params save|diff|restore` go through hangar whenever the vehicle's manager is
up: `hangar ardusub-params` (docs/12), the vehicle's `mavlink` module reading and
writing the Pixhawk itself, over MAVROS's UDP or over `Serial=` alike, with
MANUAL_CONTROL kept at 50 Hz meanwhile (418 parameters take 1.5 s against the fake;
the Pixhawk's 979 are not measured yet).
Only when hangar is not up do they go through MAVROS, as before (`rov.sh params via`
says which). `lab.sh up` saves through hangar and stops if the save fails; `lab.sh
down` restores and compares through hangar too, starting it again for that if it has
stopped. A dump with no parameter in it is not a backup: `params save` removes the file
and fails, either way.

`lab.sh` writes down what `up` changed (the backup's name, the services it stopped) in
its session file on the laptop, `~/.cache/hangar-lab/session`, and copies it to the Pi's
`~/hangar-rov/lab-session`. A `down` or a second `up` on a laptop that has lost the file
takes the Pi's copy and goes on from it. With no session on either side, `down` stops
hangar if it runs but does not say "everything as found": it says that the parameters
were not restored, and the newest `~/hangar-rov/lab-*-before.params` on the Pi is then
the one to restore by hand (`rov.sh up`, `rov.sh params restore <file>`, `rov.sh reset`).

The file is text, one parameter a line:

```
# hangar: ArduSub's parameters, 979, read 2026-11-21T09:12:03Z (mavlink.params_dump)
# NAME VALUE TYPE (MAV_PARAM_TYPE); hangar ardusub-params restore sets them back
SYSID_THISMAV 1 4
ATC_RAT_RLL_P 0.135000005 9
```

`TYPE` is the `MAV_PARAM_TYPE` ArduSub reports (9 a float, 1 to 8 the integer types),
so a restore writes each parameter with its own type; an integer's value is written as
an integer, a float's with nine significant digits, so a value read back is the value
written. A restore sets only what differs, each set read back before it counts, and
refuses while armed; the values it replaced are kept beside the module's copy as
`<name>.before`. ArduSub's own counters (`STAT_*`) and the barometers' ground pressure
(`BARO*_GND_PRESS`) change by themselves: never set, left out of `diff`.

The backups taken through MAVROS before (`pixhawk-*.yaml`, `lab-*-before.yaml`, the
YAML of `ros2 param dump`) restore through hangar the same way. The MAVROS fallback
reads its own YAML only. **Not yet tried on the real Pixhawk**: hangar's way is tested
against the fake ArduSub over UDP and over a pseudo-terminal.

## The camera's own H.264 (the encoder, opt-in)

The vehicle sends the camera's H.264 as the camera makes it, with no encode on the Pi
(`modules/encoder/README.md`; docs/11 "encoder"), and the effects run on the laptop:
electric-eye's `hellion` and `eeye` there in placement A, until hangar's `video_relay`
and `vision` exist. Unverified on this vehicle: that its camera offers H.264 at all.

On the Pi, first:

1. **Which node has H.264.** `sudo apt install v4l-utils` if `v4l2-ctl` is missing, then
   `for d in /dev/video[0-9]; do echo "== $d"; v4l2-ctl -d $d --list-formats-ext | grep -B1 -A3 H264; done`.
   Put that node in `profiles/rov-vehicle/encoder.conf` (`Source=uvc:/dev/videoN`), or in
   the laid-out `/dev/shm/hangar-vehicle/etc/hangar/encoder.conf` and `hangar restart
   encoder`. No node with H264: this path has nothing to send on this camera; stop here.
2. **The lab's electric-eye off the camera:** `sudo systemctl stop eeye hellion` (they
   hold the camera and send to the laptop's 5610 too); `start` them again at the end.
3. **`ENCODER=1 rov.sh up`** (after `rov.sh reset` if the roots were laid out without it).
   `hangar status` shows `encoder` ready, its frames and kbit/s; `hangar echo
   video.index` a message a frame.

On the laptop, electric-eye's topside in placement A:

4. **`hellion`** with `legacy/hellion/examples/a-topside.json`, two changes for this
   laptop: `"control": "8092"` (8080 is taken) and the `v4l2:/dev/video10` sink removed
   (no v4l2loopback). Its `camera-in` pipeline receives `rtp:5610` and decodes into the
   frame bus stream `camera`; its `pilot-out` pipeline encodes the stream `pilot` for
   QGroundControl on 127.0.0.1:5600.
5. **`eeye`** with a config of its own: `"capture_source": "bus"`, `"capture_stream":
   "camera"`, `"output_device": ""`, the effects as wanted. It reads `camera` and
   publishes `pilot`.
6. **QGroundControl**: the video, with the effects, on UDP 5600 as before; the effects
   from hellion's page on http://127.0.0.1:8092.

What the encoder's RTP is to `hellion`: payload type 96, H264/90000,
packetization-mode 1, on 5610, the marker on each frame's last packet, parameter sets in
band before each IDR: what its SDP (`lib/video/ffmpeg.c`, `ff_write_sdp`) and its relay
(`lib/video/udp_relay.c`, the delimiter after each marker) take. `hellion` receives
without discovering a peer; its `"peer": "auto"` and `"camera_controls": "peer"` find no
`hellion` on the vehicle, so the page's camera controls do nothing in this setup (the
encoder has no control channel yet).

## The topside on the laptop (the bridge, opt-in)

With `BRIDGE=1` the shim is gone: the vehicle role runs alone on the Pi, the topside
role on the laptop, and the tether bridge (`modules/bridge/README.md`; docs/08) joins
them over UDP 5614. The default stays the shim, both roles on the Pi.

```
 laptop 192.168.2.1                                Pi 192.168.2.10
 QGroundControl :14550 ◄── topside_mavlink            safety, mux, mavlink, recorder
                            console :8081             console 127.0.0.1:8080
                            bridge ◄──── UDP 5614 ────► bridge
              pilot.wrench, pilot.buttons, calls ──►  ◄── safety.state, nav.*, ardusub.state, …
```

What crosses is `profiles/rov-vehicle/bridge.conf` (the same as
`profiles/rov-topside/bridge.conf` but for `Peer=`): `safety.state`,
`hangar.vehicle.system` and `.modules`, `nav.*`, `power.battery`, `mux.state`,
`ardusub.state`, `recorder.status` (10 Hz), `video.index` and `ardusub.param` up;
`pilot.wrench` and `pilot.buttons` down; calls to `safety.*`, `mux.*`, `recorder.*` and
`mavlink.setup` across. The two copies must otherwise match, and both machines must run
the same msg/ tree (the same commit), or `config_match` is false and nothing crosses.

1. **On the Pi** (a checkout or a release, built at the same commit as the laptop):
   `rov.sh reset` if the roots were laid out without the bridge, then
   `BRIDGE=1 rov.sh up`. The first run makes `~/hangar-rov/bridge.key` (32 random
   bytes, mode 600).
2. **The key to the laptop**, once: `scp bluerov2@192.168.2.10:hangar-rov/bridge.key
   ~/hangar-rov/` (`mkdir -p ~/hangar-rov` first), mode 600. Never in the repository.
3. **On the laptop**, from the checkout: `make -j8 all`, then
   `BRIDGE=1 tools/rov/rov.sh topside up`. It needs `hangard`, `hangar`,
   `hangar-mavlink`, `hangar-bridge` and `dawn-patrold` in `builddir/`.
4. **The link:** `HANGAR_ROOT=/dev/shm/hangar-topside builddir/hangar status` shows
   `bridge  ready  up 192.168.2.10:5614, rtt … ms, offset … ms, N topics in, …` and
   `topside_mavlink … link up`; the Pi's `hangar status` the same toward
   `192.168.2.1:5614`. `config_match false: …` says what differs. `hangar echo
   bridge.topside.link` has the counters (`rejected`, `retransmits`, `loss_rx`).
5. **QGroundControl** on the laptop as before (UDP 14550, now from the laptop itself);
   the topside console on http://127.0.0.1:8081, its buttons calling the vehicle's
   `safety` and `recorder` across; `hangar call safety.check` on the laptop the same.
6. **At the end:** `BRIDGE=1 tools/rov/rov.sh topside down` on the laptop, `rov.sh down`
   on the Pi.

`ROV_IP` and `GCS_IP` set the two addresses in both `bridge.conf` files at lay-out, as
for the rest of the profile. A firewall on the laptop must let UDP 5614 in from the Pi.
Run on loopback only (two roots, two ports, the lab profiles' real modules, no relay,
2026-11-20): the link up, `topside_mavlink` `link up`, `hangar call safety.disarm`,
`safety.check` and `recorder.stop` answered across. Not yet on the tether.

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

## Results, 2026-11-20

The third visit, with main at `e559888` (the busy-loop fix and the encoder); props off.

- **The fix on the vehicle:** all of hangar, ten processes on both roles, 5.3–6.3 % of a
  core at rest and armed (168.6 % on 11-19); every module at 1.7 % or less, the Pi at
  48–66 °C, not throttled. (`mavlink` and `safety` at 1.6 and 1.2 % carry ArduSub's
  50 Hz telemetry: work, not a spin.)
- **The camera's H.264 on `/dev/video2`** (1920×1080, 1280×720, 800×600 at 30, 25, 15
  fps; `/dev/video0` is its MJPEG): no controls on that node, so no keyframe request
  (the camera sends one every ~30 frames on its own) and no bitrate setting.
- **End to end, armed from QGroundControl:** the encoder at 30 frames/s (1.1 % of a core,
  7.3 MB) → electric-eye's hellion and eeye on the laptop in placement A → QGC with the
  effects; the recorder recorded the armed session with no gap. Against the old setup
  (electric-eye with ffmpeg on the ROV, 89.5 % of a core, 116 MB): the ROV's video cost
  1/80 of it; the tether carries 11.4 Mbit/s instead of 4.7 (the camera's own rate);
  the laptop does the decode, the effects and the re-encode (0.84 of a core).
- **The MAVROS relay** is now the largest cost on the vehicle: 28 % of a core, 128 MB.
- Ubuntu's unattended-upgrades ran as root at a full core mid-session and, with the
  closed tube, took the Pi to 86 °C and throttling; two of the runs read high for it.
  Results and caveats: the user's `~/hangar-results/2026-11-20-lab/` on the laptop.

## Not tried yet

- The encoder (`ENCODER=1`) on this vehicle: its camera's H.264 node, the keyframe
  control, the latency and the CPU against electric-eye's 0.91 of a core.
- Signing against the real ArduSub (`Signing=no` here).
- Driving with props on, in the tank.
- The bridge (`BRIDGE=1`) across the tether: the topside on the laptop, pilot.wrench's
  latency and the call round trip over the Fathom-X.
