# Safety

What keeps the vehicle from doing harm when a module, the manager, the tether, a
trained model or the computer itself fails. The learning loop
([09-learning](09-learning.md)) puts freshly trained models in a position to command
thrusters; this doc is why that is acceptable.

## The rules

- **Thrusters move only when the vehicle is armed, and only on fresh commands.** Every
  actuator module checks both on every output, through one library gate, and outputs
  neutral otherwise. Silence, from any module, means stop.
- **One module owns the vehicle's state**: `safety`. It arms, disarms, enters
  failsafe, and publishes the state. The manager stays generic.
- **One path to the thrusters**: every control source goes through the `mux`, which
  enforces the envelope (thrust, depth and rate limits). No source, a neural network
  least of all, can exceed it.
- **The pilot always wins.** Pilot input above the deadband on an axis takes that axis.
- **A new model starts in shadow**: its outputs are published and recorded, never
  applied, until an operator promotes it.
- **Failsafe ends only by an operator**: `safety.clear` once the cause is gone, then a
  fresh `arm`.
- **While ArduSub drives the thrusters, it stays the last line**: hangar acts through
  it and leaves its own failsafes on.

## Vehicle states

`safety.state`, published at 10 Hz and on every change:

```
u8   state            # 0 disarmed, 1 armed, 2 failsafe
u8   outputs_enabled  # whether actuators may drive (below)
u8   action           # in failsafe: 0 neutral, 1 surface, 2 hold
u8   reserved
u32  triggers         # failsafe: bit per trigger that fired
u64  since_ns
char by[32]           # who armed, or what triggered
char reason[64]
```

| State | `outputs_enabled` | Entered by | Left by |
|---|---|---|---|
| `disarmed` | no | start; `safety.disarm`; `clear` after failsafe | `safety.arm`, if the checklist passes |
| `armed` | yes | `safety.arm` | `disarm`; any failsafe trigger |
| `failsafe` | only for `surface` and `hold` | a trigger; `safety.estop` | `safety.clear`, once no trigger is active; it leads to `disarmed` |

**Arming** passes only if every module named in the `required` parameter is `ready`
(from `hangar.modules`, which also means its rated topics are fresh), no failsafe
trigger is active, and the leak, battery and tether inputs are present and good. A
refused `arm` says which check failed.

| Call | `Allow` | |
|---|---|---|
| `safety.arm` | the pilot console, `hangar-admin` | |
| `safety.disarm` | every module | making the vehicle safer is never refused |
| `safety.estop` | every module | failsafe at once, action `neutral` |
| `safety.clear` | the pilot console, `hangar-admin` | refused while a trigger is active |
| `safety.check` | every module | runs the arming checklist without arming, and says what would fail (`hangar check`) |

## The actuator gate

Every module with an actuator output declares where its commands come from:

```ini
# thrusters.module
[Actuator]
Command=ctl.wrench
CommandTimeout=200ms
SafetyTimeout=300ms
```

| `[Actuator]` key | Default | |
|---|---|---|
| `Command` | — (required) | The topic the module turns into outputs; one of its subscriptions |
| `Neutral` | — (required) | A command that puts the hardware to neutral by itself: the manager runs it whenever the module exits, crashed or not, before anything else ([03-manager](03-manager.md#states)) |
| `Groups` | `""` | The groups that give access to the actuator's hardware (the PWM controller's bus, its output-enable line). No other module may be in them; a udev rule gives the devices to these groups alone |
| `CommandTimeout` | `200ms` | A command older than this is not acted on |
| `SafetyTimeout` | `300ms` | `safety.state` older than this counts as disarmed (three of its periods) |

The section is required for a module with an actuator output, and refused for any
other. The module drives its outputs through the library's gate:

```c
hg_gate *g = hg_gate_open();                 // Command and timeouts from the wiring
for (;;) {
  hg_wait(...);
  const wrench *cmd = hg_gate_check(g, &why); // NULL: output neutral; why says why
  write_outputs(cmd ? allocate(cmd) : NEUTRAL);
}
```

`hg_gate_check` returns the newest command only if `safety.state` is fresh, says
`outputs_enabled`, the command was published within `CommandTimeout`, and the command
is not flagged as computed from stale inputs ([01-topics](01-topics.md#stale-inputs));
otherwise NULL, with the reason (`disarmed`, `safety state stale 410 ms`, `command
stale 230 ms`, `command computed from stale nav.depth`), which the module puts in its
status line. A neutral output published while the gate is closed names no input. A dead safety module, a dead mux, a
dead controller and a lost tether all end the same way: neutral within one timeout
and one output period.

On `SIGTERM` an actuator module writes neutral before it exits, and the manager stops
actuator modules first ([03-manager](03-manager.md#shutdown)). A module that cannot
(it crashed, or hangs) is covered by its `Neutral` command, which the manager runs as
soon as the module exits; a hung module is killed after its `StopTimeout` (500 ms for
actuators), then neutralized. Where the output hardware has an enable line, the
actuator module keeps it enabled by refreshing a hardware watchdog, so that outputs
drop by themselves if nothing refreshes them, even if the computer hangs.

## The mux

`ctl.wrench` has one writer: the `mux`. Every control source publishes its own topic
(`pilot.wrench` from the pilot's input, `ctl.depth_hold`, `policy.<name>.wrench`) and
the mux chooses, per axis, by mode:

| Mode | Source |
|---|---|
| `manual` | the pilot only |
| `assisted` | the pilot, with the axes a controller holds (depth, heading) taken from it while the pilot leaves them alone |
| `auto` | an autonomy source |
| `policy:<name>` | a learned policy ([09-learning](09-learning.md)) |
| `residual:<name>` | `assisted`, with a learned policy's output added to the controllers' axes, within the residual envelope |

**Pilot override.** On any axis where the pilot's input exceeds the deadband, the
pilot has the axis, in every mode, and keeps it for 1 s after letting go. A pilot
input that goes stale counts as zero.

**The envelope**, the mux's parameters: the largest command per axis (in phase 1,
normalized commands from −1 to 1 of full; [10-mavlink](10-mavlink.md#commands-to-ardusub)), the
largest rate of change, and a depth band outside which no command pushing further out
is passed. Limits are applied to every source, the pilot's included, after the choice.
In `residual:<name>` the policy's contribution is first held within the **residual
envelope** (by default 20% of the envelope on each axis), then the sum within the
envelope: the controller stays in the loop, and the network can only correct it by a
bounded amount. This is how online adaptation runs ([09-learning](09-learning.md)).

**Shadow.** A policy's topic is published and recorded whenever the policy runs; the
mux applies it only in mode `policy:<name>` or `residual:<name>`, which only
`mux.set_mode` sets, allowed to the pilot console and `hangar-admin`. That call is the
promotion of a model.

**Demotion.** In either mode the mux falls back to `assisted`, and says why, when the
policy's output goes stale, when its model is reverted, when its model is replaced
(except as below), or when its output is held at an envelope limit for more than
`demote_saturation` (a parameter, 20%) of the last `demote_window` (5 s). It never
promotes on its own.

**Updates while active.** A policy whose manifest allows it
([09-learning](09-learning.md#updates-while-active)) may have its weights replaced
while in `residual:<name>` without being demoted, so that online adaptation can keep
learning; in `policy:<name>`, where the network has the whole axis, a replaced model is
always demoted and must be promoted again.

`mux.state`, on every change and at 10 Hz: the mode, the source of each axis, and which
limits are acting. With `ctl.wrench`'s provenance (the source messages it used), the
recording says who was driving at every moment, which a dataset can select on
([06-recorder](06-recorder.md#datasets)).

The mux is `OnGiveUp=failsafe`, and its calls allow only the pilot console and
`hangar-admin`.

## Failsafe

The `safety` module watches its inputs and enters failsafe when a trigger fires. Each
trigger has an action, a parameter of the safety module:

| Trigger | Fires when | Default action |
|---|---|---|
| `module` | `hangar.system` is `failsafe` (a module given up with `OnGiveUp=failsafe`) | `neutral` |
| `tether` | `bridge.link` says no packet from the topside for `tether_timeout` (3 s) | `neutral` |
| `leak` | the leak topic reports water | `surface` |
| `battery` | voltage below `battery_min` for 5 s | `surface` |
| `depth` | depth beyond the envelope's band by `depth_margin` | `surface` |
| `autopilot` | ArduSub disarmed itself, reported a failsafe of its own, or went silent for 1 s ([10-mavlink](10-mavlink.md#arming-and-failsafe)) | `neutral` |
| `estop` | `safety.estop` | `neutral` |
| `input` | a topic the safety module needs (leak, battery, depth, tether) stale | `neutral` |

| Action | |
|---|---|
| `neutral` | `outputs_enabled` off: every actuator to neutral; the vehicle's positive buoyancy brings it up |
| `surface` | the mux commands a vertical thrust up (`surface_thrust`), ignoring every source but the pilot's override |
| `hold` | the mux keeps the current depth through the depth controller for `hold_time`, then `neutral`; `neutral` at once if the depth controller is not ready |

Several triggers at once: the most conservative action wins (`neutral` over `hold`
over `surface`, since `neutral` depends on the fewest modules working).

## Phase 1: ArduSub drives the thrusters

Until hangar drives thrusters itself, the actuator is the MAVLink bridge module
([10-mavlink](10-mavlink.md)), which turns `ctl.wrench` into ArduSub's manual control
messages through the same gate. hangar's state acts through ArduSub:

| hangar | sent to ArduSub |
|---|---|
| `arm` | arm |
| `disarm`, failsafe `neutral` | disarm |
| failsafe `surface` | mode SURFACE |
| gate closed (neutral) | neutral manual control |

ArduSub's own failsafes (pilot input lost, leak, battery, depth) stay enabled. If the
MAVLink bridge dies, ArduSub stops receiving manual control and its pilot-input
failsafe acts: hangar adds protection and replaces none.

## The computer itself

systemd's `RuntimeWatchdogSec` (in `/etc/systemd/system.conf`) drives the Pi's
hardware watchdog: a computer that hangs is rebooted. What the thrusters do between the
hang and the reboot depends on whether the PWM generator keeps its last pulse widths by
itself, which is a property of the hardware, not of hangar. It is measured, not assumed
([Bench tests](#bench-tests-before-the-first-wet-run)).

## Bench tests before the first wet run

Each is run on the vehicle's own hardware, with propellers off, and its timings
recorded. None is optional.

1. **Computer hang.** Hang the Pi (a real-time busy loop on every core, then a kernel
   panic): what the PWM outputs do, and how long until they are neutral.
2. **Each module killed** in turn (`SIGKILL`), armed with a non-neutral command: time to
   neutral. For the actuator module itself, this tests its `Neutral` command.
3. **A sensor killed** behind a controller (depth behind depth hold): the controller's
   commands are flagged stale, and the gate goes neutral (or the pilot keeps
   control, in `manual`).
4. **The safety module killed**: neutral within `SafetyTimeout` plus one output period.
5. **The manager killed and restarted**, armed: the vehicle keeps its state; nothing
   jumps.
6. **The tether pulled**: `tether` failsafe at `tether_timeout`; ArduSub's own failsafe
   behind it.
7. **A policy in shadow** producing large commands: nothing reaches the thrusters.
8. **Pilot override** during `policy:<name>` and `auto`: the pilot's axis wins at once.
9. **Envelope**: every source, the pilot's included, held within the limits.

## Tests

The bench tests above, and in CI: the gate's three conditions and their reasons; every
state transition and refused transition; each trigger and the combination rule; the
mux's per-axis choice, override hold and envelope; shadow mode never applied.
