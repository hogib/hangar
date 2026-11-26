# thrusters: ctl.wrench to pulse widths, without ArduSub

`hangar-thrusters`, the actuator of a vehicle hangar drives without ArduSub: the
simulator's standalone mode (modules/sim_vehicle, phase 2) and, later, ESCs driven
directly. C11 over lib/hg, with lib/sim's frame, allocation matrix and T200 table;
static for musl.

```
hangar-thrusters                               # mixing as ArduSub mixes
hangar-thrusters --frame FILE [--voltage V]    # allocating through the frame's geometry
hangar-thrusters --neutral                     # the manager's Neutral command
```

- At 50 Hz: `ctl.wrench` through the actuator gate (`hg_gate`, docs/07), mixed, to
  eight pulse widths. Gate closed: 1500 µs on all eight, naming no input.
- Publishes `thr.cmd` (`thruster_cmd`, `Actuator=yes`), what sim_vehicle reads, and
  `thr.out` (`thruster_out`), the pulse widths sent, as the mavlink module publishes
  ArduSub's servo outputs, so a dataset's labels read the same topic either way. Both
  reference the `ctl.wrench` message they were computed from.
- `--neutral` (the manifest's `Neutral=`) writes nothing and exits 0. docs/07 asks a
  `Neutral` command to put the hardware to neutral by itself; this module has no
  hardware (its output is a topic, whose reader, the simulator, stops the thrusters
  when `thr.cmd` goes stale), and a `Neutral` command runs outside the module's
  wiring, so it could not publish on `thr.cmd` either. docs/07 "The actuator gate"
  says so. A driver of real ESCs must write 1500 µs to its hardware there and fail
  when it cannot: do not copy this one. Stopping, the module publishes 1500 µs once.
- One writer per topic: not in a root with `mavlink.module`.
- It waits only on its tick; the gate's topics are read every tick (hangar#1).

## The two mixings

**Without `--frame`: ArduSub's** (`thr_mix`). Each output is the sum of the axis inputs
times ArduPilot's motor factors for the vectored 6DOF frame (lib/sim/README.md
"thruster directions"), 1500 ± 400 µs, every output scaled down by the largest when
one passes full scale. Linear in pulse width, so not in thrust, and blind to where the
thrusters sit: surge comes with the horizontals' pitch moment, 0.085 N m a newton.
`profiles/sim-depth-ff` and everything measured with it use this one, unchanged.

**With `--frame FILE`: the allocation** (`thr_alloc`). The frame file
(`lib/sim/sim_frame.h`) gives each thruster's position, direction and whether it is
installed reversed; the simulator is given the same file.

```
wrench = T thrust          T from the frame (sim_alloc_matrix), 6 x 8
thrust = T+ wrench         the pseudo-inverse: the smallest thrusts that make the wrench
pulse width from thrust    the T200's table at --voltage, inverted
```

- **What a command of 1 is.** Each axis's capacity: the largest wrench on that axis
  alone, the same both ways, that no thruster's limit cuts short. For the heavy
  configuration at 16 V (one T200: −39.9 to +51.4 N): surge and sway 112.9 N, heave
  159.6 N, roll 34.80 N m, pitch 19.16 N m, yaw 30.14 N m. The module logs them at
  start. They move with `--voltage`.
- **The wrench asked for is the wrench applied**, to the pulse width's whole
  microsecond: within 0.39 N and 0.042 N m on every axis at every command, none
  across. Surge at 0.5 is 56.4 N with 0.01 N m of pitch; the verticals carry the
  correction.
- **Saturation.** Heave, roll and pitch are served first, scaled together when they
  alone are too much; surge, sway and yaw get what is left, scaled together. Each group
  keeps its direction, and holding depth and attitude comes before moving. The cost is
  at the top of the heave range: surge needs the verticals for its pitch correction, so
  up to half the heave command the whole surge is there, at 0.75 half of it, at full
  heave none. No thruster is ever asked past 1100 or 1900 µs.
- **The dead band.** A thrust under 1 mN is 1500 µs; above it the table is followed
  through the ESC's dead band, so the smallest thrust that turns a propeller is the
  table's (0.15 N at worst between what is asked and given).
- **A frame that cannot produce an axis** (six thrusters, no pitch) has capacity 0 on
  it: the command on that axis is ignored, the others are served, and the module logs
  which. Outputs the frame does not describe stay at 1500 µs.
- **The supply voltage is a constant** (`--voltage`, 16 V unless given, 10 to 20). The
  module does not read `power.battery`: as the battery runs down the thrust applied
  falls a few per cent below what was asked, and the controllers' feedback takes it up.
- A frame file that cannot be read or parsed, or whose thrusters make no wrench, is a
  configuration error: exit 78, the reason in the log.

## What this does not do

- **No hardware.** Nothing here writes to a PWM generator; the output is a topic. The
  driver, its `Neutral` command and its hardware watchdog are docs/07's phase 2 and
  are not written.
- **No sensors.** The simulator stands in for the IMU, the pressure sensor and the
  battery monitor; there is no module for any of them, nor for a leak probe.
- **Nothing verified on a vehicle.** The allocation has met lib/sim's model and no
  water. What real outputs do when the module, the manager or the computer stops is
  unmeasured (docs/07 "Bench tests").
- **The lab's vehicle keeps its Pixhawk.** There ArduSub cannot be removed, only
  reduced to passing per-thruster commands through, and whether its firmware allows
  that, and what its outputs do when the commands stop, has not been checked.

## Tests

`make check-thrusters` (2 s): both mixings against lib/sim's `sim_applied_wrench`, the
simulator's own command path.
- The factors: neutral and non-finite inputs give 1500 µs; each axis at 0.5 pushes the
  right way (heave up is −z, yaw clockwise is +Mz), with no force across; heave is
  monotonic; a saturated mix is scaled to stay within 1100 to 1900 µs.
- The allocation: rank and capacities; asked against applied on every axis at nine
  commands each; the same sign as the factors on every axis, and none of their pitch
  moment; the order past the limits, with every combination of −1, 0 and 1 on six axes
  inside the pulse-width range; thrust to pulse width and back; a reversed thruster; a
  made-up frame of six that cannot pitch.
- lib/sim's model flown at a fixed step, the same numbers every run: surge in open
  loop with each mixing and each damping (the three combinations that tumble, and the
  one that does not: lib/sim/README.md "Pitch under surge"); depth held with and
  without levelling; and the whole flight of `profiles/sim-direct` in one process:
  2 m reached and held to 3 cm, 4.6 m forward in 8 s at half surge and 6.7 m at full
  with pitch within half a degree, 4.0 m to the right at half sway, a full turn in
  4.7 s, level and at depth at the end.

`make check-sim_direct` (75 s): `profiles/sim-direct` under a manager, the profile's
own files with this build's binaries. The profile's README has what it checks.

`make check-thrusters_run` (10 s): the binary under a manager with the real safety
module and no autopilot, the suite publishing `ctl.wrench` and reading `thr.cmd`.
Disarmed, 1500 µs whatever the command; `arm` leads to `armed` at once; armed, heave
0.5 moves the four vertical thrusters and the message references `ctl.wrench`; the
command stopped, neutral after 216 ms (`CommandTimeout` 200 ms and a period); the
safety module stopped with `SIGSTOP`, neutral after 296 ms (`SafetyTimeout` 300 ms
from its last message); a disarm; `hangar stop thrusters` while driving leaves a
neutral last message and the system nominal. Every neutral message names no input
(`HG_NO_REFS`): before this suite the module published them with `hg_publish`, so
each referenced the `ctl.wrench` it had not acted on. The whole stack runs in
`tools/sim/e2e-depth-ff.sh`.
