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
`profiles/rov-laptop/`. The steps below are what it does, by hand. `lab.sh up --bridge`
is "The topside on the laptop" below, and `lab.sh up --serial` is `MAVLINK=serial`
("Without MAVROS"), each with the session's save and restore of ArduSub's parameters;
`tools/rov/rehearse.sh` runs them all on the laptop, without the vehicle.

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

0. **The Pi as it was.** If an earlier run may still be up: `rov.sh down` (refused
   unless the vehicle reads disarmed: [The guard](#the-guard); or, after the
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

## The guard

Nothing of hangar is stopped under an armed vehicle. On 2026-11-24 `rov.sh down` was run
on the Pi while the vehicle was armed; the mavlink module sent neutral and a disarm as it
stopped, which is its last resort and not a way to end a session. Since then the tools
ask before they stop anything, and the answer is the vehicle's own two topics.

**What is read.** `safety.state` and `ardusub.state` from the vehicle's root, two
messages of each within 3 s (`hangar echo --count 2`; both are sent at 10 Hz). The second
is one the module sent while the guard looked: a topic's file and its last message
outlive a module that died or hangs, and that last word is not the state now.
`rov.sh armed` prints the reading: `disarmed: …` (status 0), `armed: …` (4),
`unknown: …` (5), or `no manager`.

`rov.sh spin` tries the manager's one-second limit: it starts the release's `bin/spin`,
a module that never sleeps, under `fifo` at priority 5 (below every module of hangar's;
`SPIN_PRIORITY`), and waits for the kernel to end it. Status 0: ended, within about a
second; 3: real time is not in force on this machine, so there was nothing to try; 1: it
ran on for 5 s and was stopped, which is the finding. It refuses under an armed vehicle,
and removes its manifest whatever happened.

| The reading | `down`, `reset`, `relay down` |
|---|---|
| no manager under the vehicle's root | proceed: nothing of hangar drives anything |
| `safety.state` disarmed and `ardusub.state` armed 0 | proceed |
| the same, and the mavlink module has never heard ArduSub (`last_rx_ns` 0: a rehearsal with `RELAY=0`, or the Pixhawk's cable) | proceed, and say that "disarmed" is hangar's side only |
| `safety.state` failsafe with the outputs off (action neutral: after a tether or a module failsafe) and ArduSub armed 0 | proceed: ArduSub was disarmed, and a restart keeps the latch |
| arming, armed, a failsafe that drives (surface, hold), ArduSub armed or still disarming | **refused**, status 4, nothing stopped |
| a manager runs and one of the two topics is not being published, or cannot be read | **refused**, status 4: a safety or mavlink module that is down or hangs has said nothing, and nothing is not "disarmed" |

`relay down` is guarded because the relay is the mavlink module's only way to ArduSub:
without it hangar can neither drive nor disarm. `--force` anywhere on the line (or
`ROV_FORCE=1`) does it all the same and says what it overrode; it is for a vehicle that
QGroundControl shows disarmed while hangar cannot say so, and for one that has to come
down as it is.

**The topside on the laptop** (`BRIDGE=1 rov.sh topside down|reset`, and `down` or
`reset` there) reads the bridge's copies of the same two topics. Armed: refused, since
stopping the pilot's end of the tether under an armed vehicle is the tether failsafe
fired on purpose (neutral and a disarm about 3 s later, the sticks dead meanwhile). No
copies arriving: the link is down already, the vehicle has acted on that by itself, and
the laptop's side staying up changes nothing for it; that is said, and the topside is
stopped. A laptop that could not stop its own topside while the tether is out would be
stuck exactly when the cable is the problem.

**`lab.sh`** holds to the same. `up` lays hangar out afresh, which stops it: while the
vehicle is not disarmed, or its state cannot be read, `up` refuses before it changes
anything and disarms nothing. `down` is the session's end and disarms an armed vehicle
itself (`safety.disarm`, hangar's ordinary way), says that it found it armed, then reads
the state again: still not disarmed, or not readable, and it stops nothing, on the Pi or
on the laptop, the pilot's video included. `lab.sh down --force` goes on regardless.
`deploy` refuses while hangar runs at all. The Pi's `rov.sh` asks again at the moment
it stops (`rov.sh reset`), so an arm between `down`'s check and the stop is caught too.
With a release on the Pi from before the guard, `lab.sh` reads the safety and mavlink
modules' status lines instead, and that `rov.sh` stops without asking.

**What is not guarded.** `hangar stop|restart <module>`, `hangar restart-manager` and a
signal to `hangard` or to a module: the failure tests (`failtest.py`) do exactly these
to an armed vehicle on purpose. The guard is in the scripts that end or re-lay a
session, where stopping under an armed vehicle is never what was meant. It is tested
without a vehicle by `make check-rov` (`tools/rov/tests/test_guard.sh`, every row above
against a stub of `hangar echo`) and against a real stack, armed, by `rehearse.sh both`.

## Preflight

`lab.sh preflight` reads what the two machines are doing besides hangar and prints one
line a check, `ok`, `warn` or `FAIL`, with the command that would put it right. It
changes nothing. Runs were spoiled or held up on three visits by things no part of
hangar looks at: Ubuntu's `unattended-upgrade` holding a core on the Pi (it starts by
itself some 25 minutes after boot, and `systemctl stop` does not end the process), the
Pi's clock days behind (it has no battery clock), a Pi at its temperature limit, and
this laptop's own `hellion.service` on the port the camera's video arrives at.

It has two modes. `preflight` alone asks what would keep a session from coming up as
meant; `lab.sh up` runs it before it changes anything and stops at a `FAIL`
(`up --no-preflight` goes on). `preflight --measure` asks what would spoil a number;
`lab.sh bench` runs it before and after and keeps both reports in the run's folder,
with a file `NOT-CLEAN` and the first lines of its README saying so if either had a
`FAIL`; `failtest.py` runs it before each block, keeps the report as
`<block>.preflight.txt` and writes the verdict into every row. A measurement is never
refused: it is marked.

A measurement on a Pi that is not frozen is a `FAIL` ("Freeze and thaw", below): the
checks of this table say what the machine was doing when it was looked at, and only a
frozen Pi stays that way until the block is over.

| Check | Before a session | Before a measurement | Why that line |
|---|---|---|---|
| The Pi not frozen (`lab.sh freeze`) | ok | FAIL | the package system can start by itself, the governor and the lab's services are as they happen to be |
| Package system: a process of `apt`, `dpkg` or `unattended-upgrade` (not Ubuntu's idle `unattended-upgrade-shutdown`) | warn | FAIL | it holds a core for minutes and heats the Pi |
| `apt-daily.timer`, `apt-daily-upgrade.timer` active | warn | warn | it can start in the middle of a block |
| The Pi's clock against this laptop's, over 2 s | warn (`up` sets it) | FAIL | recordings and logs are stamped by it; 2 s is what `up` tolerates |
| The Pi's temperature: 70 °C and over | warn | warn | with the heat sink every real state read 50 to 62 °C (2026-11-25) |
| 80 °C and over | warn | FAIL | the Pi 5's soft limit: its clock is lowered from there |
| `vcgencmd get_throttled`: a bit of now (under-voltage, capped, throttled, soft limit) | warn | FAIL | |
| a bit of earlier in this boot | ok | warn | |
| Governor other than `performance` | ok | warn | latencies read higher and wider; the results must say which |
| Memory free under 500 MB / 200 MB | warn / FAIL | warn / FAIL | |
| `/dev/shm` free under 512 MB / 128 MB | warn / FAIL | warn / FAIL | the roots and every topic live there |
| Home free under 1000 MB / 100 MB | warn / FAIL | warn / FAIL | recorder sessions and results |
| A process that is not the session's above 20 % of a core, over one second | warn | warn | its load is in every number; the session's own are hangar's, the relay and this laptop's video |
| A UDP port the session binds, taken by another | FAIL | | MAVLink, the bridge, the camera's video: without it the session is not the one asked for |
| A console's TCP port taken | warn | | the session flies without a console |
| The lab's own services (`LAB_SERVICES`) active | ok | warn | their load is in the numbers |
| A `[Schedule]` not in force (`hangar status`) | warn | FAIL | docs/03, "What is in force" |
| Real time not reachable by the installed manager | ok | warn | every module then runs at the ordinary priority |
| The Pi's release against this laptop's | warn | warn | with the bridge the two refuse each other |

The ports are those of the session asked for (`--video`, `--bridge`, `--serial`, or an
open session's own): on the Pi 8080, 14560, the relay's 14555 unless `--serial`, and
8081 and 14650 or, with `--bridge`, the bridge's port; on this laptop, with `--bridge`
the bridge's port, 8081 and 14650, with `--video` 5610 and hellion's page, and the
consoles' forwards. A port held by this session's own process is ok. hangar's modules
make themselves non-dumpable, so the kernel names no holder for their sockets: a port
held by nobody it can name is taken for the session's while a manager of the session
runs on that machine (`up` on a session that is up), and for another's otherwise.

The temperature limits are the Pi 5's and are applied to the Pi; this laptop's
temperature is printed and not judged.

The probe is `tools/rov/preflight.py`, sent to the Pi's `python3` on its standard
input, so the release installed there need not have it. `make check-rov` runs its
judge over written facts, one case a line of the table, and the probe on the machine
it runs on.

## Freeze and thaw

`lab.sh freeze` holds the Pi still for a session of measurements, and `lab.sh thaw` puts
it back. Three things made earlier numbers unequal: `unattended-upgrade` starting by
itself (three visits), the lab's eeye and hellion running beside some blocks and not
beside others (2026-11-25), and the governor changed half-way through a session. A
freeze takes all three out of the session at once, and the preflight refuses a
measurement on a Pi that is not frozen.

What freeze does, in this order, as root under **one** sudo (it prints every command
first, read from the Pi as it is at that moment):

1. **The state as found, written down**: whether each timer, unit and service runs, and
   each CPU's governor, in `/run/hangar-frozen` on the Pi (a copy is kept on this
   laptop, in lab.sh's state directory). Before anything is changed, so that a freeze
   interrupted half-way is thawed from it.
2. **Nothing of the package system starts again in this boot**:
   `systemctl mask --runtime --now` for `apt-daily.timer` and `apt-daily-upgrade.timer`,
   `systemctl mask --runtime` for the two services they start (`LAB_FREEZE_TIMERS`,
   `LAB_FREEZE_UNITS` in `lab.conf`).
3. **What of it runs now is waited for**: `unattended-upgrade` is sent SIGTERM, on
   which it ends after the package it is at; `apt`, `apt-get` and `dpkg` are sent
   nothing. Then freeze waits, up to `LAB_FREEZE_WAIT` (900 s), and says every 30 s
   what it waits for. Nothing is ever sent SIGKILL, and dpkg is never interrupted: a
   package half installed on the lab's vehicle is worse than a late measurement. If
   the wait runs out, freeze does the rest, ends with status 3 and says so; the
   preflight's `package system` line then keeps a measurement from being taken for
   clean until it has ended.
4. **The lab's own services stopped** (`LAB_SERVICES`: eeye and hellion), those that
   ran.
5. **The governor `performance`** on every CPU (`LAB_FREEZE_GOVERNOR`).
6. **The clock set from this laptop's**, if it is more than 2 s off.

Ubuntu's `unattended-upgrades.service` is left alone: it is the idle waiter that is
there from boot to shutdown (`unattended-upgrade-shutdown`), holds no core, and the
preflight does not count it. snapd's refreshes are not held either: if one runs, the
preflight's `other load` line shows it.

`thaw` reads the record and puts back exactly that: the masks taken off, each timer
and each of the lab's services started again **if it ran when the Pi was frozen** (one
that was already stopped stays stopped), each governor as it was. The clock stays as
set. The package system's services are not started: their timers are, and run them when
they are due. Run twice, the second finds no record and does nothing. thaw refuses
while hangar runs on the Pi (`lab.sh down` first): it starts the lab's services, which
hold the camera.

**A thaw that was forgotten** ends with the Pi's next boot: the masks are systemd's
runtime ones and the record is under `/run`, so both are gone, the governor is the
boot's own again and the lab's services start as they always do. The lab's Pi cannot be
left without updates by this. So that it is not forgotten quietly all the same,
`lab.sh status` says `THE PI IS FROZEN` while it is, and `lab.sh down` on a frozen Pi
ends with that line in place of `everything as found`. While the Pi is frozen, `down`
leaves the lab's services stopped (a session with `--video` would otherwise start them
between two blocks); thaw starts them.

This laptop is not frozen: it is its user's own machine. The preflight goes on
reporting its governor, its load and its own `hellion.service`.

The work on the Pi is `tools/rov/freeze.sh`, sent there by `lab.sh` (the release
installed on the Pi need not have it). `make check-rov` runs it against a Pi that is a
directory (`tests/fake-pi.sh`): the record, the masks, a timer that was already stopped,
the governor back to `ondemand`, thaw twice, an upgrade that takes a while to end, dpkg
at work, a package system that does not end, a freeze stopped half-way. A rehearsal
(`LAB_HOST=local`) freezes the same stand-in, never the laptop. Not shown by either:
the real `systemctl` and the real `unattended-upgrade` on the Pi.

## Real time (opt-in, an experiment)

Every number of the visits up to 2026-11-25 was taken with every module at the ordinary
priority and with unlocked memory: the profiles' manifests have no `[Schedule]`
(docs/00: real time is not required), and a manager that is not root could have given
none. To measure with one:

1. `lab.sh deploy --realtime`: after the install, `sudo setcap
   cap_sys_nice,cap_ipc_lock+ep` on the Pi's `hangard`, closed to other users first
   (docs/13, "Real-time scheduling without root"; the risk is stated there). It asks for
   the Pi's sudo password, and is needed again after every deploy.
2. `lab.sh up --realtime` (`REALTIME=1` for `rov.sh`): at lay-out, `safety`, `mux`,
   `mavlink` and `bridge` get `[Schedule] Policy=fifo`, priorities 60, 50, 50 and 40,
   and `LockMemory=yes` (`REALTIME_MODULES`, `name:priority` each). These priorities
   are a first guess for the measurement, not a design.
3. `up` ends with one line a module, `OK schedule` or `WARN schedule`, and `hangar
   status` on the Pi prints `not in force:` under its table for each module without
   what it asks. `lab.sh preflight --measure` makes that a `FAIL`.

A module under `fifo` that runs for a second without sleeping is killed by the kernel
and restarted by the manager (docs/03, step 4). On this vehicle that has never
happened and never been provoked: the first `up --realtime` is props off.

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

`lab.sh` writes down what `up` changed (the backup's name, the services it stopped,
whether the vehicle is laid out with the bridge or on the serial line, the topside it
started on the laptop) in its session file on the laptop, `~/.cache/hangar-lab/session`, and copies it to the Pi's
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

## Without MAVROS (the serial line, opt-in)

With `MAVLINK=serial` the vehicle's mavlink module holds the Pixhawk's USB serial itself
(`modules/mavlink/README.md`, "A serial line") and no relay runs; MAVROS was the largest
cost on the Pi (28 % of a core, 128 MB, 2026-11-20). `rov.sh` lays the vehicle's root out
from the profile's `mavlink.conf.serial` and `mavlink.module.serial` in the place of the
two beside them, with `Serial=` and `Baud=` from `FCU`; the installed profile is read,
never changed.

1. Nothing else may hold `/dev/ttyACM0`: the module opens it exclusively, and a MAVROS
   left running has it get `EBUSY`. `rov.sh up` refuses while its own relay runs
   (`rov.sh relay down`); `lab.sh up --serial` looks for any other holder first, names it
   and changes nothing, and stops the units named in `LAB_SERIAL_SERVICES`
   (`lab.conf`), which `down` starts again.
2. `rov.sh reset` if the roots were laid out with the relay, then `MAVLINK=serial rov.sh
   up` (with `BRIDGE=1` as well, if wanted). `rov.sh status`: `mavlink  ready … serial
   open; ardusub disarmed`.
3. The parameters, the arming and the rest are as in "A session, in order"; `rov.sh
   params` goes through hangar, over the serial line.

Run against the tests' fake ArduSub at the far end of a pseudo-terminal
(`tools/rov/rehearse.sh`, 2026-11-21): the parameters saved, set and restored, armed and
disarmed across the bridge. **Not yet on the Pixhawk's own serial device.**

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
them over UDP 5614. The default stays the shim, both roles on the Pi. `lab.sh up
--bridge` does steps 1 to 4 from the laptop, after comparing the two builds, and `lab.sh
down` step 6; by hand:

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
6. **At the end:** disarmed; then `BRIDGE=1 tools/rov/rov.sh topside down` on the
   laptop, `rov.sh down` on the Pi. Both refuse while the vehicle is armed
   ([The guard](#the-guard)).

`ROV_IP` and `GCS_IP` set the two addresses in both `bridge.conf` files at lay-out, as
for the rest of the profile; on the laptop, `topside_mavlink` binds `GCS_IP` (until
2026-11-21 it was laid out with the Pi's address there, which only loopback let it bind).
A firewall on the laptop must let UDP 5614 in from the Pi. `BRIDGE_PORT` and
`BRIDGE_PEER_PORT` are for both roles on one machine, where the two bridges cannot share
a port.
Run on loopback only (two roots, two ports, the lab profiles' real modules;
`tools/rov/rehearse.sh`, 2026-11-21): the link up, `topside_mavlink` `link up`, `hangar
call safety.check` answered across, armed and disarmed from the topside with the fake
ArduSub on the serial line, and with the topside stopped an arm on the vehicle refused
once its bridge had called the link lost (about 3 s). Since then the safety module
refuses earlier: 1.5 s after the last packet, half of `tether_timeout` (its suite).
Not yet on the tether.

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

- Real time (`lab.sh deploy --realtime`, `up --realtime`): file capabilities on the
  Pi's `hangard`, modules under `fifo` with locked memory, and what the tail of the
  latencies does then. Props off first.
- `lab.sh preflight` against the Pi itself: `vcgencmd`, the clock comparison over SSH,
  a running `unattended-upgrade` found by it.

- The encoder (`ENCODER=1`) on this vehicle: its camera's H.264 node, the keyframe
  control, the latency and the CPU against electric-eye's 0.91 of a core.
- Signing against the real ArduSub (`Signing=no` here).
- Driving with props on, in the tank.
- The bridge (`BRIDGE=1`, `lab.sh up --bridge`) across the tether: the topside on the
  laptop, pilot.wrench's latency and the call round trip over the Fathom-X.
- The serial line (`MAVLINK=serial`, `lab.sh up --serial`) on the Pixhawk's
  `/dev/ttyACM0`: whether anything of the lab's holds it, the parameters over it, the
  module's CPU against the relay's.
