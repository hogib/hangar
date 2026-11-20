# joystick: the pilot's controller, read natively

`hangar-joystick` (identifier `joystick`, topside) reads a game controller through
Linux's joystick interface (`/dev/input/jsN`, `struct js_event`) and publishes the
pilot's input as hangar topics, with no QGroundControl and no MAVLink between: the
controller half of making QGroundControl optional (hangar#3; the console half is
hangar#2).

| | |
|---|---|
| Publishes | `pilot.wrench` (`wrench`, 50 Hz) and `pilot.buttons` (`pilot_buttons`, 50 Hz): the topics, types and rate `topside_mavlink` publishes for QGroundControl |
| Calls | `safety.arm`, `safety.disarm`: a button marked arm or disarm, once per press |
| Reads | the controller; the reply topics of its calls |
| Configuration | `joystick.conf` (manifest syntax; every key has a default) |

## What it does

- **Axes.** Each of the wrench's six axes (surge, sway, heave, roll, pitch, yaw) takes
  one of the controller's axes, or none: `-32767..32767` normalized to `-1..1`,
  inverted if asked, then the deadband (`|x|` at or below it is 0, the rest
  rescaled to reach 1 at full travel), then the expo (`(1 − e) x + e x³`). The
  wrench is normalized as `topside_mavlink`'s is: the gain is the mux's envelope.
- **The defaults** are QGroundControl's ArduSub layout on an Xbox controller under
  Linux: the left stick heave and yaw, the right stick surge and sway (axes 0 1 left,
  3 4 right; stick up reads negative, hence the inversions of surge and heave). A
  section `[Axis <name>]` replaces its axis's mapping: a key it does not give is plain
  (`Source=none`, `Invert=no`, `Deadband=0.05`, `Expo=0`). The shipped
  `joystick.conf` states every key; its suite checks that it says the defaults.
- **Buttons** do nothing unless `[Button N] Function=` says: `arm` or `disarm` call
  `safety.arm` or `safety.disarm` once per press; `pass` sets the button's bit in
  `pilot.buttons` while it is held. A button already held when the controller
  appears (the driver's INIT events) is its state, not a press: it calls nothing.
- **The device**: `Device=/dev/input/js0` (the default), or `Device=auto` with
  `Name=`, the first `/dev/input/js*` whose name (the driver's, `JSIOCGNAME`) holds it.
- **Unplugged, or never there**: the module keeps publishing at 50 Hz, a neutral
  wrench and no buttons, **flagged as computed from stale input**, says so once in its
  log and on its status line, and looks for the device again every `Reopen` (1 s); a
  controller that comes back is read without a restart.
- **No busy-waiting** (hangar#1): one epoll over the 20 ms timer, the device and the
  reply topics of its calls, all read on every pass.

## Instead of QGroundControl

`joystick` and `topside_mavlink` both write `pilot.wrench` and `pilot.buttons`, and a
topic has one writer: a profile enables one or the other. `profiles/rov-topside/` and
`profiles/sim-topside/` ship `joystick.module.off` and `joystick.conf`; nothing changes
until it is enabled. In a laid-out topside root (`/dev/shm/hangar-topside` from
`tools/rov/rov.sh` or `tools/sim/hangar-up.sh`), with the manager stopped or followed
by `hangar reload`:

```sh
R=/dev/shm/hangar-topside; BIN=~/hangar-root/bin      # or the build directory
mv $R/etc/hangar/modules/topside_mavlink.module $R/etc/hangar/modules/topside_mavlink.module.off
sed -e "s|/usr/lib/hangar/|$BIN/|g" -e "s|/etc/hangar/|$R/etc/hangar/|g" \
    <profile>/joystick.module.off > $R/etc/hangar/modules/joystick.module
sed -e "s|/etc/hangar/|$R/etc/hangar/|g" <profile>/joystick.conf > $R/etc/hangar/joystick.conf
HANGAR_ROOT=$R $BIN/hangar reload
```

QGroundControl then has nothing to connect to: the pilot's picture, telemetry and arm
button are Dawn Patrol's (hangar#2). The controller must be on the machine that runs
the topside role: until the bridge, the vehicle's Pi (the shim), so a controller
plugged into the laptop needs the topside role on the laptop, which the bridge brings.

## Deviations from hangar#3, with reasons

1. **Unplugged, it publishes neutral and flagged stale, rather than stopping.** The
   issue asks that `pilot.wrench` go stale. A module that stops publishing a rated
   output is restarted by the manager after `StaleGrace` (docs/02, "Staleness is the
   watchdog"), so stopping would restart it at every unplug. The mux reads a message
   flagged stale as no pilot input (`pilot_ok` requires the flag clear), exactly as it
   reads a stale topic: neutral, unreferenced. The suite checks both: neutral and
   flagged within 25 ms of the unplug, the module still `ready`, its pid and failure
   count unchanged.
2. **No `uinput` in the tests.** The suite feeds `struct js_event` records through a
   FIFO standing in for the device node (no root needed); unplugging is the node
   removed and the writer closed, as when a controller is pulled out.
3. **Profiles: `.module.off`, enabled by hand.** `tools/rov/rov.sh` gets no switch for
   it here, since `lab.sh` (hangar#4) is being written beside this; the steps above.
4. **The button functions are arm, disarm, pass and none.** The issue leaves room for
   the console's (`preset_next`, `record`, `snapshot`): they come with hangar#2.

## Tests (`make check-joystick`, about 8 s)

`joystick.conf` (defaults, every key, refusals with their reasons, the shipped file
equal to the defaults, the section-replaces rule); the shaping (range and clamp,
inversion, deadband, expo, INIT events not presses); then the module under a manager,
this process playing `safety`: 50 messages a second; the axes mapped, inverted, with
deadband and expo; arm and disarm, one call per press, none for a button held at
plug-in; a pass button's bit while held; unplugged, neutral and flagged stale within
25 ms, still 50 Hz, not restarted, said once; plugged in again, read within 50 ms (the
test's `Reopen=100ms`); idle CPU with and without a controller (0.0 % of a core here).

## Unverified

The lab's Xbox Series controller (`/dev/input/js0` on the laptop) has not been read by
the module: its axis numbers are the Linux `xpad` layout the defaults assume, and the
first flight with it should check each stick's direction (`hangar echo pilot.wrench`).
`Device=auto`'s name match has run only against the FIFO, which has no name.
