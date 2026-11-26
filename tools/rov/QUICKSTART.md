# A lab session in five commands

Everything below runs on the laptop, from a checkout of hangar, with the laptop on the
vehicle's tether (Fathom-X: the laptop is 192.168.2.1, the Pi 192.168.2.10).
`tools/rov/lab.sh` drives the Pi over SSH and writes down what it changes (on the laptop,
with a copy on the Pi), so that `down` can put it all back. The values for the lab's BlueROV2 are in
`tools/rov/lab.conf`; what is behind each step is in `tools/rov/README.md`. What to try
at the next visit, in order, is in [LAB-CHECKLIST.md](LAB-CHECKLIST.md).

**Props off** until the arm and disarm work on the bench (README, "A session, in order").

## Once, before the first session

1. **Build on the laptop:** `make -j8 all` in the checkout (a plain `make` does not build
   the modules), and the vehicle's release, `tools/release.sh`.
2. **SSH by key** (ask whoever owns the Pi first): `ssh-copy-id bluerov2@192.168.2.10`. After
   that no command asks for the login password. sudo on the Pi still asks for its own
   password when a step needs root (setting the clock, stopping the lab's electric-eye); it
   is typed at the prompt and never stored.
3. **QGroundControl,** until the Fly view and the joystick module (issues #2, #3) make it
   optional:
   - if it does not start: `sudo chmod -R a+rX /opt/qgroundcontrol`;
   - the controller: **Q icon → Vehicle Setup → Joystick**, pick it, calibrate, tick
     **Enable joystick input** (the page exists only while a vehicle is connected).

## The five commands

| Command | What it does | What good looks like |
|---|---|---|
| `tools/rov/lab.sh deploy` | the newest `out/release/*/hangar-*-aarch64.tar.zst` to the Pi, checked, installed in `~/hangar-root`; the Pi's clock set from the laptop's | `lab: installed v0.1.2 (…) at /home/bluerov2/hangar-root` |
| `tools/rov/lab.sh up [--video] [--bridge] [--serial] [--no-battery-check]` | hangar up on the Pi; ArduSub's parameters saved (once a session) and set; the pre-arm check; the consoles tunnelled. `--video`: the lab's electric-eye stopped, the camera's own H.264 sent by the encoder, and the laptop's hellion and eeye started (the effects here, QGroundControl's video on 5600). `--bridge` and `--serial`: below | `lab: READY TO ARM`, then the consoles' and the video's addresses. With `--video`, four `[ OK ] pipeline` lines. With `--bridge`, `lab: the link is up` |
| `tools/rov/lab.sh status` | both managers (with `--bridge`: the vehicle's on the Pi, the topside's on the laptop), the relay, the lab's services, the Pi's temperature and throttling, the laptop's video, the session | `safety … disarmed` or `armed`; `throttled=0x0`; with `--bridge`, `bridge  ready … up` on both |
| `tools/rov/lab.sh bench [SECONDS]` | CPU, memory, frame rates and the tether's traffic on both machines, 60 s by default; with `--bridge`, both bridges' link topics before and after | `lab: saved in ~/hangar-results/<date>-lab`, with a README of the state |
| `tools/rov/lab.sh down [--force]` | everything put back: disarmed first (an armed vehicle is disarmed, and said; if it then does not read disarmed, nothing is stopped), ArduSub's parameters restored and compared with the backup (through hangar, started again for it if it stopped), the topside on the laptop stopped, hangar stopped and its roots removed, the lab's services started again, the laptop's video and tunnels stopped | `lab: everything as found` |

Then arm in QGroundControl (**Disarmed → Arm**), drive with the sticks, disarm, and
`lab.sh down` before leaving. Run `down` even when something went wrong: it does every
step it can and names the ones that failed.

Nothing here stops hangar under an armed vehicle (README, "The guard"). `up` on a
session that is already up lays hangar out again, so it refuses while the vehicle is
armed, and disarms nothing. `down` disarms first and stops nothing unless the vehicle
then reads disarmed.

## The bridge and the serial line

Two flags of `up`, each alone or together, with each other and with the rest. A second
`up` with other flags continues the session: hangar is laid out afresh, and the
parameter backup stays the first one.

**`up --bridge`** runs the topside role on the laptop (from `builddir/`) and the vehicle
role alone on the Pi, joined by the tether bridge over UDP 5614 (README, "The topside on
the laptop"). It first compares the two builds, because the bridge carries nothing
between two that differ (`config_match`): `NOT THE SAME BUILD` stops it before anything
is started, and the cure is a release of this checkout on the Pi (`tools/release.sh`,
`lab.sh deploy`). It copies `bridge.key` from the Pi to the laptop's `~/hangar-rov/`
(mode 600; never into the repository), where it stays, as the Pi's does. With the bridge
the tether trigger is on: arming needs the link up, and losing it for 3 s is a failsafe.
The topside's console is then the laptop's own, http://127.0.0.1:8081.

**`up --serial`** puts the mavlink module on the Pixhawk's serial line itself (`FCU` in
`lab.conf`), with no MAVROS relay. The installed profile is not changed. Nothing else may
hold the device: if something does, `up` changes nothing and names the process and its
systemd unit. Stop it by hand, or name the unit in `LAB_SERIAL_SERVICES` (`lab.conf`):
`up` then stops it, as it stops the lab's electric-eye for `--video`, and `down` starts
it again.

By hand instead, on the Pi: `BRIDGE=1 MAVLINK=serial rov.sh up` (and `BRIDGE=1
tools/rov/rov.sh topside up` on the laptop); the parameters are then yours to save and
restore (`rov.sh params`).

## The battery check

The lab's Pixhawk reads no battery voltage (0.73 V with the battery in; its power-module
sense cable, most likely), so hangar's `safety` refuses to arm on it. `up
--no-battery-check` turns the check off for the session only; `down` removes it with the
session. Never on a vehicle that reads its battery.

## When a step fails

| What it says | What to do |
|---|---|
| `cannot reach bluerov2@192.168.2.10` | the tether's link (`ping 192.168.2.10`), the laptop's address on it, the Pi powered |
| `hangar runs on the Pi: lab.sh down first` (deploy) | `lab.sh down`, then `deploy` again |
| `no hangar at hangar-root on the Pi` (up) | `lab.sh deploy` first |
| `THE VEHICLE IS NOT DISARMED` (up) | a second `up` would stop hangar under an armed vehicle: disarm (QGroundControl, the console), then `up` again. Nothing was changed |
| `whether the vehicle is disarmed cannot be read` (up) | hangar runs, and its safety or mavlink module is down or hangs (the line says which topic is missing): `lab.sh status`. `lab.sh down --force` brings it down as it is |
| `THE VEHICLE DOES NOT READ DISARMED` (down) | `down` asked for a disarm and the vehicle is still armed, or cannot say: nothing was stopped. Disarm in QGroundControl and `lab.sh down` again. If hangar cannot say (a module down) and QGroundControl shows it disarmed, or it has to come down as it is: `lab.sh down --force` |
| `the parameters could not be saved` | hangar's mavlink module cannot read the Pixhawk's parameters: the USB cable, `/dev/ttyACM0`; `~/hangar-rov/up.log` and the module's log on the Pi. Nothing was written to the Pixhawk. `lab.sh status`, then `lab.sh down` |
| `mavlink, after 60 s: …` | ArduSub is not heard: as above. The relay's log is `~/hangar-rov/relay.log` on the Pi |
| `NOT READY TO ARM: <reasons>` | the reasons are `hangar check`'s: `power.battery … below` → `--no-battery-check` (see above); `ArduSub … not MANUAL` → set MANUAL; anything else → `lab.sh status` |
| no `[ OK ] pipeline` lines with `--video` | wait ten seconds and `lab.sh status`: the encoder starts after hangar. Still nothing: the camera's H.264 node (`H264_NODE` in `lab.conf`; `v4l2-ctl --list-formats-ext` on the Pi) |
| `port 8092 is taken on this laptop` | another hellion runs: stop it, or set `HELLION_CONTROL_PORT` |
| `--video needs port 5610 free` | something on this laptop already receives the camera's video, usually its own `hellion.service`: `sudo systemctl stop hellion` |
| `THE VIDEO IS NOT WORKING on this laptop` | a pipeline of the session's hellion is in error (the line above says which); `lab.sh status` shows it again; the vehicle is not affected |
| `the two ends are NOT THE SAME BUILD` (up --bridge) | the Pi's release and this checkout differ in their `msg/` tree or `bridge.conf`: `tools/release.sh`, `lab.sh deploy` (after `lab.sh down` if hangar runs), `up --bridge` again. Nothing was started |
| `the link did not come up in 30 s` (up --bridge) | the two `bridge` lines it prints say where each end is. The laptop's firewall must let UDP 5614 in from the Pi; the laptop must have one address on the tether, the one in `lab.conf` (`LAPTOP_IP`). `lab.sh down`, or `up` again |
| `/dev/ttyACM0 on the Pi is held by something else` (up --serial) | the lines under it name the process and its unit: stop it on the Pi, or put the unit in `LAB_SERIAL_SERVICES`. Without `--serial`, `up` uses the relay as before |
| `the Pi's rov.sh does not know MAVLINK=serial`, `… a rov.sh older than this lab.sh` | the release on the Pi is older than this checkout: `tools/release.sh`, `lab.sh deploy` |
| `sudo … needs a password and there is no terminal` | run `lab.sh` from a terminal, not from a script |
| `NO SESSION FILE … not claimed: everything as found` (down) | the session file is gone from this laptop and the Pi has no copy, yet hangar ran: `down` stopped it and restored nothing. On the Pi, `ls -t ~/hangar-rov/lab-*-before.params`; if there is one from this session, `rov.sh up`, `rov.sh params restore <it>`, `rov.sh reset`; then `systemctl is-active eeye hellion` (README, "The parameter backup") |
| `not everything could be put back: …` (down) | fix what it names (usually the link) and run `lab.sh down` again: the session file keeps what is left to do. `NOT DONE (…): the vehicle is not disarmed` above it is the Pi's `rov.sh` refusing to stop hangar: someone armed during `down`; disarm and run it again |

## Rehearsing without the vehicle

`LAB_HOST=local` runs it all on the laptop: an x86-64 release (`tools/release.sh --host`)
installed in `~/hangar-labtest`, no Pixhawk (no parameters touched), the root steps skipped
and said. `tools/rov/rehearse.sh` runs four sessions that way and checks each, in a home
of its own that it removes:

```sh
make -j8 all && make -k -j8 check          # check builds the fake ArduSub
tools/release.sh --host                    # at the commit of the checkout, after committing
tools/rov/rehearse.sh out/release/hangar-<…>-x86_64.tar.zst
```

- `plain`: `up`, `status`, `down`, both roles on "the Pi".
- `bridge`: `up --bridge` with the two bridges on loopback (UDP 5614 and 5615): the link
  up on both ends, `status` and `bench` on both, a release with another `msg/` tree
  refused, `down` from the Pi's copy of the session.
- `serial`: `up --serial` with the tests' fake ArduSub at the far end of a
  pseudo-terminal: refused while another process holds the line; the parameters saved,
  set, and restored by a `down` that has to start hangar again.
- `both`: `up --bridge --serial`: armed from the topside across the link, and while it
  is armed `rov.sh down` and `reset` on the Pi, `rov.sh topside down` on the laptop and
  `lab.sh up` each refused with both managers still running (the guard); disarmed;
  the topside stopped, and an arm on the vehicle refused (`bridge.vehicle.link
  connecting`) once its bridge has called the link lost, about 3 s later; armed again,
  and `down` disarms before it stops anything.

Each must end with `lab: everything as found`, and the whole with `rehearse: all passed`.

By hand, a video source without a camera is an H.264 file of under 64 MB:

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 90 -c:v libx264 -preset ultrafast \
       -b:v 4M -g 30 -bf 0 -f h264 /tmp/test.h264
export LAB_HOST=local H264_NODE=file:/tmp/test.h264
tools/rov/lab.sh deploy out/release/<…>/hangar-<…>-x86_64.tar.zst
tools/rov/lab.sh up --video && tools/rov/lab.sh status && tools/rov/lab.sh bench 20
tools/rov/lab.sh down
```

What a rehearsal cannot show: SSH and sudo on the Pi, the clock, MAVROS and the real
ArduSub's parameters, the lab's services (stopping and starting them is skipped and
said), the camera, the copy of `bridge.key` over `scp`, the bridge on the tether's two
addresses, and the Pixhawk's own serial device. `--video` is not in `rehearse.sh`: by
hand, as above.
