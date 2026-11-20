# A lab session in five commands

Everything below runs on the laptop, from a checkout of hangar, with the laptop on the
vehicle's tether (Fathom-X: the laptop is 192.168.2.1, the Pi 192.168.2.10).
`tools/rov/lab.sh` drives the Pi over SSH and writes down what it changes, so that
`down` can put it all back. The values for the lab's BlueROV2 are in
`tools/rov/lab.conf`; what is behind each step is in `tools/rov/README.md`.

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
| `tools/rov/lab.sh up [--video] [--no-battery-check]` | hangar up on the Pi; ArduSub's parameters saved (once a session) and set; the pre-arm check; the consoles tunnelled. `--video`: the lab's electric-eye stopped, the camera's own H.264 sent by the encoder, and the laptop's hellion and eeye started (the effects here, QGroundControl's video on 5600) | `lab: READY TO ARM`, then the consoles' and the video's addresses. With `--video`, four `[ OK ] pipeline` lines |
| `tools/rov/lab.sh status` | both managers, the relay, the lab's services, the Pi's temperature and throttling, the laptop's video, the session | `safety … disarmed` or `armed`; `throttled=0x0` |
| `tools/rov/lab.sh bench [SECONDS]` | CPU, memory, frame rates and the tether's traffic on both machines, 60 s by default | `lab: saved in ~/hangar-results/<date>-lab`, with a README of the state |
| `tools/rov/lab.sh down` | everything put back: disarmed, ArduSub's parameters restored and compared with the backup, hangar stopped and its roots removed, the lab's electric-eye started again, the laptop's video and tunnels stopped | `lab: everything as found` |

Then arm in QGroundControl (**Disarmed → Arm**), drive with the sticks, disarm, and
`lab.sh down` before leaving. Run `down` even when something went wrong: it does every
step it can and names the ones that failed.

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
| `the parameters could not be saved` | MAVROS cannot reach the Pixhawk: the USB cable, `/dev/ttyACM0`. Nothing was written to the Pixhawk. `lab.sh status`, then `lab.sh down` |
| `mavlink, after 60 s: …` | ArduSub is not heard: as above. The relay's log is `~/hangar-rov/relay.log` on the Pi |
| `NOT READY TO ARM: <reasons>` | the reasons are `hangar check`'s: `power.battery … below` → `--no-battery-check` (see above); `ArduSub … not MANUAL` → set MANUAL; anything else → `lab.sh status` |
| no `[ OK ] pipeline` lines with `--video` | wait ten seconds and `lab.sh status`: the encoder starts after hangar. Still nothing: the camera's H.264 node (`H264_NODE` in `lab.conf`; `v4l2-ctl --list-formats-ext` on the Pi) |
| `port 8092 is taken on this laptop` | another hellion runs: stop it, or set `HELLION_CONTROL_PORT` |
| `sudo … needs a password and there is no terminal` | run `lab.sh` from a terminal, not from a script |
| `not everything could be put back: …` (down) | fix what it names (usually the link) and run `lab.sh down` again: the session file keeps what is left to do |

## Rehearsing without the vehicle

`LAB_HOST=local` runs it all on the laptop: an x86-64 release (`tools/release.sh --host`)
installed in `~/hangar-labtest`, no Pixhawk (no parameters touched), the root steps skipped
and said. A video source without a camera is an H.264 file of under 64 MB:

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 90 -c:v libx264 -preset ultrafast \
       -b:v 4M -g 30 -bf 0 -f h264 /tmp/test.h264
export LAB_HOST=local H264_NODE=file:/tmp/test.h264
tools/rov/lab.sh deploy out/release/<…>/hangar-<…>-x86_64.tar.zst
tools/rov/lab.sh up --video && tools/rov/lab.sh status && tools/rov/lab.sh bench 20
tools/rov/lab.sh down
```

What a rehearsal cannot show: SSH and sudo on the Pi, the clock, MAVROS and ArduSub's
parameters, the lab's services, and the camera.
