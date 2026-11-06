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
  The one exception is a failsafe `surface` or `hold` that ArduSub carries out in
  phase 1 ([Phase 1](#phase-1-ardusub-drives-the-thrusters)).
- **A new model starts in shadow**: its outputs are published and recorded, never
  applied, until an operator promotes it. The one exception is a policy with
  `UpdateWhileActive=yes` that an operator has promoted to `residual:<name>`: there a
  new model takes over after its load checks and warm-up, within the residual envelope
  and under automatic demotion ([The mux](#the-mux)).
- **Failsafe ends only by an operator**: `safety.clear` once the cause is gone, then a
  fresh `arm`. A restart of the safety module does not end it.
- **While ArduSub drives the thrusters, it stays the last line**: hangar acts through
  it and leaves its own failsafes on.

## Vehicle states

`safety.state`, published at 10 Hz and on every change:

```
u8   state            # 0 disarmed, 1 armed, 2 failsafe, 3 arming
u8   outputs_enabled  # whether actuators may drive (below)
u8   action           # in failsafe: 0 neutral, 1 surface, 2 hold
u8   reserved
u32  triggers         # failsafe: bit per trigger that fired (Failsafe)
u64  since_ns
char by[48]           # who armed ("topside:pilot_console", "admin:<user>",
                      # "topside:admin:<user>"), or what triggered
char reason[64]       # why: a refused arm, ArduSub's refusal, a trigger's cause
```

| State | `outputs_enabled` | Entered by | Left by |
|---|---|---|---|
| `disarmed` | no | start, with no failsafe latched; `safety.disarm`; `clear` after failsafe; a refused or timed-out arm | `safety.arm`, if the checklist passes |
| `arming` | no | `safety.arm` | `armed` when the autopilot reports armed; `disarmed` on its refusal or after 2 s; `disarm`; any failsafe trigger. The only state in which the autopilot may be armed before hangar is |
| `armed` | yes | `arming` | `disarm`; any failsafe trigger |
| `failsafe` | only for `surface` and `hold` | a trigger; `safety.estop`; start, with a failsafe latched | `safety.clear`, once no trigger is active; it leads to `disarmed` |

**Arming** passes the checklist only if every module named in the `required` parameter
is `ready` (from `hangar.vehicle.modules`, which also means its rated topics are
fresh), no failsafe trigger is active, `vehicle.leak` is fresh and dry,
`power.battery` is fresh and above `battery_min`, and `bridge.vehicle.link` is `up`.
In phase 1 it also needs ArduSub's `FS_GCS_ENABLE` and `FS_LEAK_ENABLE` on, as the
vehicle's `mavlink` module reads them ([10-mavlink](10-mavlink.md#arming-and-failsafe)).
A refused `arm` says which check failed.

Arming is asynchronous. `safety.arm` runs the checklist and answers `accepted` at once,
or refuses. On `accepted`, `safety` first publishes `safety.state` with state
`arming`, with `outputs_enabled` off, and only then calls `mavlink.arm` with
`hg_call_start` ([04-calls](04-calls.md#using-calls)), so that neither its loop nor
`safety.state` ever waits on the autopilot. `mavlink.arm` refuses unless the
`safety.state` it reads says `arming`: the window in which ArduSub may be armed exists
before the arm is sent. When `ardusub.state` reports ArduSub armed, `safety` becomes
`armed`. On a refusal, or with no report after 2 s, it returns to `disarmed` with ArduSub's reason in `reason` (the
`PreArm:` and `Arm:` status texts sent around the refusal, otherwise the result's
name). An arm that ArduSub carries out after that is undone by the armed agreement
([Phase 1](#phase-1-ardusub-drives-the-thrusters)). Without an autopilot behind the
actuators, `arming` leads to `armed` at once.

| Call | `Allow` | |
|---|---|---|
| `safety.arm` | `topside:pilot_console topside:topside_mavlink admin topside:admin` | answers `accepted` or a refusal; the result is on `safety.state` |
| `safety.disarm` | `* topside:* admin topside:admin` | making the vehicle safer is never refused |
| `safety.estop` | `* topside:* admin topside:admin` | failsafe at once, action `neutral` |
| `safety.clear` | `topside:pilot_console topside:topside_mavlink admin topside:admin` | refused while a trigger is active ([Leaving failsafe](#failsafe)) |
| `safety.check` | `* topside:* admin topside:admin` | runs the arming checklist without arming, and says what would fail (`hangar check`) |

`Allow` tokens are those of [04-calls](04-calls.md#callers): a module name,
`topside:<module>` for a module of the topside, `*` for every module on this machine,
`topside:*` for every module of the topside, `admin` for `hangar-admin` on this
machine, `topside:admin` for `hangar-admin` on the topside. The default, `* admin`,
admits nobody from the topside, so each method names its topside callers. Disarming,
stopping and checking are open to every module and operator on both sides; arming
and clearing only to the pilot's console, QGroundControl's module and the operators.
`topside:topside_mavlink` is the topside's MAVLink module, which turns
QGroundControl's arm and disarm requests, and the pilot's arm and disarm buttons,
into these calls ([10-mavlink](10-mavlink.md#the-topsides-mavlink-module)).
`hangar check` run on the topside calls `safety.check` as `topside:admin:<user>`.

The safety module's parameters (the trigger actions, `tether_timeout`, `battery_min`,
`depth_margin`, `hold_time`, the `required` list) decide what arming checks and what a
failsafe does. Its `[Params] Allow` is `topside:pilot_console admin topside:admin`; for
the safety module the default is none ([05-parameters](05-parameters.md#declaring-parameters)).

## The actuator gate

Every module with an actuator output declares where its commands come from:

```ini
# thrusters.module
[Subscribe ctl.wrench]
[Subscribe safety.state]

[Actuator]
Command=ctl.wrench
Neutral=/usr/lib/hangar/navigator_pwm --neutral
CommandTimeout=200ms
SafetyTimeout=300ms
```

| `[Actuator]` key | Default | |
|---|---|---|
| `Command` | — (required) | The topic the module turns into outputs; one of its subscriptions |
| `Neutral` | — (required) | A command that puts the hardware to neutral by itself: the manager runs it whenever the module exits, crashed or not, before anything else ([03-manager](03-manager.md#states)) |
| `Groups` | `""` | The groups that give access to the actuator's hardware (the PWM controller's bus, its output-enable line). No other module may be in them; a udev rule gives the devices to these groups alone. A `Legacy` module that drives hardware has the same protection through `[Module] ExclusiveGroups` ([02-manifest](02-manifest.md#module-required-once)) |
| `CommandTimeout` | `200ms` | A command older than this is not acted on |
| `SafetyTimeout` | `300ms` | `safety.state` older than this counts as disarmed (three of its periods) |

The section is required for a module with an actuator output, and refused for any
other. The module subscribes to its `Command` topic and to `safety.state`. It drives
its outputs through the library's gate:

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
status line. A neutral output published while the gate is closed names no input. A
dead safety module, a dead mux and a dead controller all end the same way: neutral
within one timeout and one output period. A lost tether zeroes the pilot's input once
`pilot.wrench` goes stale on the vehicle (its mirror's `StaleAfter`,
[08-bridge](08-bridge.md#configuration)), and fires the `tether` trigger at
`tether_timeout`.

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

```ini
# /etc/hangar/modules/mux.module
[Module]
Exec=/usr/lib/hangar/mux

[Subscribe pilot.wrench]
[Subscribe ctl.depth_hold]
[Subscribe policy.pilot.wrench]
[Subscribe policy.pilot.active]
[Subscribe policy.depth_ff.wrench]
[Subscribe policy.depth_ff.active]
[Subscribe nav.depth]
[Subscribe safety.state]

[Publish ctl.wrench]
Type=wrench
Rate=50
Sources=pilot.wrench ctl.depth_hold policy.pilot.wrench policy.depth_ff.wrench nav.depth

[Publish mux.state]
Type=mux_state
Rate=10

[Params]
Type=mux_params
Allow=topside:pilot_console admin topside:admin

[Serve set_mode]
Request=set_mode_req
Reply=set_mode_rep
Allow=topside:pilot_console admin topside:admin

[Restart]
OnGiveUp=failsafe
```

**Pilot override.** On any axis where the pilot's input exceeds the deadband, the
pilot has the axis, in every mode, and keeps it for 1 s after letting go. A pilot
input that goes stale counts as zero.

**The envelope**, the mux's parameters: the largest command per axis, the largest rate
of change, and a depth band outside which no command pushing further out is passed. In
phase 1 a command is a fraction of ArduSub's full command at pilot gain 1, from −1 to
1; the vehicle's `mavlink` module pins ArduSub's gain to 1, so that the fraction does
not change with a gain button ([10-mavlink](10-mavlink.md#commands-to-ardusub)). Limits
are applied to every source, the pilot's included, after the choice. In
`residual:<name>` the policy's contribution is first held within the **residual
envelope** (by default 20% of the envelope on each axis), then the sum within the
envelope: the controller stays in the loop, and the network can only correct it by a
bounded amount. This is how online adaptation runs ([09-learning](09-learning.md)).

**Provenance.** The mux lists every topic it may reference in `Sources=`
([02-manifest](02-manifest.md#publish-topic-one-per-output)) and publishes each
`ctl.wrench` with `hg_publish_with`, referencing the source messages it used: the one
that drove each axis, and `nav.depth` when the depth band limited a command. The stale
check and its transitive flag apply to those references only
([01-topics](01-topics.md#stale-inputs)). So a flagged source that drives an axis flags
`ctl.wrench`, and the gate goes neutral; a source that drives nothing (an idle
controller, a policy in shadow, an absent autonomy source) never does. A stale pilot
input counts as zero and is not referenced. A message holds at most 4 references:
when more sources were used, the mux references the 4 that drove the most axes, and
sets the message's stale flag itself if any source it used and left out carries one.
`mux.state` (`msg/mux_state.msg`), on every change and at 10 Hz, holds the mode, the
source of each axis (`source_surge`, `source_sway`, `source_heave`, `source_roll`,
`source_pitch`, `source_yaw`: 0 none, 1 the pilot, 2 a controller, 3 an autonomy
source, 4 a policy), and which limits are acting. With `ctl.wrench`'s provenance, the recording says who was
driving at every moment, which a dataset can select on
([06-recorder](06-recorder.md#datasets)).

**Shadow.** A policy's topic is published and recorded whenever the policy runs; the
mux applies it only in mode `policy:<name>` or `residual:<name>`, which only
`mux.set_mode` sets. That call is the promotion of a model. A policy with no running
model publishes, every tick, a neutral wrench flagged as computed from stale inputs;
the mux treats it as no contribution ([09-learning](09-learning.md#policy-modules)).

**Promotion names the model.** `mux.set_mode` into `policy:<name>` or
`residual:<name>` carries the hash of the model the operator evaluated, and is refused
unless it is the hash of the policy's running model, as the newest
`policy.<name>.active` message announces it. Every `policy.<name>.wrench` message
references, in its provenance, the `policy.<name>.active` message of the model that
computed it ([09-learning](09-learning.md#lineage)). The mux applies a message only if
that reference is the promoted model's; a message from any other model is never
applied.

**Demotion.** In either mode the mux falls back to `assisted`, and says why, when the
policy's output goes stale, skips a tick (a policy publishes no output that is not
finite), or is flagged as computed from stale inputs; when a message references a model
other than the promoted one (a replacement or a revert), except as below; or when its
output is held at an envelope limit for more than `demote_saturation` (a parameter,
20%) of the last `demote_window` (5 s). It never promotes on its own.

**Updates while active.** A policy whose spec sets `UpdateWhileActive=yes`
([09-learning](09-learning.md#updates-while-active)) says so in each
`policy.<name>.active` message. In `residual:<name>`, when a message references a new
`policy.<name>.active` message that carries the flag, the mux takes that model as the
promoted one without demotion, so that online adaptation can keep learning; every load
has still passed the policy's checks and warm-up. In `policy:<name>`, where the network
has the whole axis, a replaced model is always demoted and must be promoted again.

The mux is `OnGiveUp=failsafe`. Its calls and its parameters allow only
`topside:pilot_console`, `admin` and `topside:admin`; for the mux the `[Params] Allow`
default is none.

## Failsafe

The `safety` module watches its inputs and enters failsafe when a trigger fires. Each
trigger has a bit in `safety.state.triggers` and an action, a parameter of the safety
module:

| Bit | Trigger | Fires when | Default action |
|---|---|---|---|
| 0 | `module` | `hangar.vehicle.system` is `failsafe` (a module given up with `OnGiveUp=failsafe`, or an actuator's `Neutral` command failed) | `neutral` |
| 1 | `tether` | `bridge.vehicle.link` says no packet from the topside for `tether_timeout` (3 s) | `neutral` |
| 2 | `leak` | `vehicle.leak` reports water | `surface` |
| 3 | `battery` | `power.battery` voltage below `battery_min` for 5 s | `surface` |
| 4 | `depth` | `nav.depth` beyond the envelope's band by `depth_margin` | `surface` |
| 5 | `autopilot` | ArduSub disarmed itself while hangar is armed; `ardusub.state` says ArduSub armed while `safety.state` is neither `armed` nor `arming`, outside a failsafe `surface` or `hold` ([Armed agreement](#phase-1-ardusub-drives-the-thrusters)); or ArduSub sent no message of any kind for 3 s ([10-mavlink](10-mavlink.md#arming-and-failsafe)) | `neutral` |
| 6 | `estop` | `safety.estop` | `neutral` |
| 7 | `input` | a topic the safety module needs (`vehicle.leak`, `power.battery`, `nav.depth`, `bridge.vehicle.link`) stale | `neutral` |

`vehicle.leak` is published at 10 Hz by the `leak` module, which reads the leak probe's
GPIO line itself, through a device granted to it alone
([10-mavlink](10-mavlink.md#telemetry)), so a dry probe is a fresh
message, not the absence of one. ArduSub's own failsafe conditions (leak, battery,
pilot input lost) are not triggers by themselves: ArduSub acts on them, and hangar's
`leak` and `battery` triggers watch the same conditions on hangar's own topics. A
failsafe in which ArduSub disarms itself fires `autopilot` through the disarm.

| Action | |
|---|---|
| `neutral` | `outputs_enabled` off: every actuator to neutral; the vehicle's positive buoyancy brings it up |
| `surface` | the mux commands a vertical thrust up (`surface_thrust`), ignoring every source but the pilot's override |
| `hold` | the mux keeps the current depth through the depth controller for `hold_time`, then `neutral`; `neutral` at once if the depth controller is not ready |

In phase 1 ArduSub carries out `surface` and `hold` in its own modes
([Phase 1](#phase-1-ardusub-drives-the-thrusters)).

Several triggers at once: the most conservative action wins (`neutral` over `hold`
over `surface`, since `neutral` depends on the fewest modules working).

**The latch.** On entering failsafe, and on every change of its triggers, the safety
module writes the state, the action, the triggers and the reason to a file in its
state directory (`/var/lib/hangar/safety`), renamed into place before it publishes
them; `clear` removes the file. At start it reads the file and, if one is there,
starts in `failsafe` with the same action and triggers. A safety module that crashes,
is restarted by `hangar restart` or a reload, or restarts for a `@restart` parameter
while in failsafe therefore comes back in failsafe, and `arm` is refused until
`safety.clear`.

**Leaving failsafe.** `safety.clear` is refused while any trigger is active. The
`module` trigger stays active while `hangar.vehicle.system` is `failsafe`, which the
manager leaves only on a `clear-failsafe` request
([03-manager](03-manager.md#control-socket)). During `safety.clear`, when `module` is
the only active trigger, the safety module sends that request; the manager accepts it
only when no module given up with `OnGiveUp=failsafe` is still failed. If the manager
refuses, `clear` is refused with the manager's reason. Otherwise the vehicle goes to
`disarmed`, and a fresh `arm` is needed.

## Phase 1: ArduSub drives the thrusters

Until hangar drives thrusters itself, the actuator is the vehicle's MAVLink module
([10-mavlink](10-mavlink.md)), which turns `ctl.wrench` into ArduSub's manual control
messages through the same gate. hangar's state acts through ArduSub:

| hangar | sent to ArduSub |
|---|---|
| `arming` | arm, only after `arming` is published; `armed` once ArduSub reports armed ([Arming](#vehicle-states)) |
| `disarm`, failsafe `neutral` | disarm |
| failsafe `surface` | mode SURFACE, with neutral manual control (heave 500) |
| failsafe `hold` | mode DEPTH HOLD, with neutral manual control, for `hold_time`, then disarm |
| gate closed | neutral manual control; disarm, once closed for longer than `CommandTimeout` while ArduSub is in any mode other than MANUAL, outside a failsafe `surface` or `hold` |
| ArduSub armed while `safety.state` is neither `armed` nor `arming` (outside a failsafe `surface` or `hold`) | disarm; the `autopilot` trigger fires at once |

**Failsafe `surface` and `hold`.** ArduSub runs them in its own modes, and the mux
outputs neutral (heave 500) while either runs; the failsafe's mode takes precedence
over the mode table of [10-mavlink](10-mavlink.md#flight-modes). So in phase 1:

- the pilot has no heave override during `surface`, and no override on any axis
  during either;
- `surface` climbs at ArduSub's own rate (`WPNAV_SPEED_UP`), not at `surface_thrust`;
- on reaching the surface ArduSub switches itself to DEPTH HOLD (reason
  `SURFACE_COMPLETE`). That is expected: the module does not command SURFACE again,
  and the mux keeps outputting neutral;
- `hold` uses ArduSub's depth hold, not hangar's depth controller, so the rule
  "`neutral` at once if the depth controller is not ready" does not apply;
- if ArduSub refuses either mode, the module disarms it, as for `neutral`.

**A closed gate in a stabilizing mode.** Neutral manual control stops the thrusters
only in ArduSub's MANUAL mode. In STABILIZE or DEPTH HOLD, neutral input means "hold
attitude" or "hold depth", and the thrusters keep acting. So a gate closed for longer
than `CommandTimeout` while ArduSub is in any other mode, and not running a failsafe
`surface` or `hold`, makes the module disarm ArduSub.

**Armed agreement, both ways.** ArduSub disarming itself while hangar is armed fires
the `autopilot` trigger. Whenever `ardusub.state` says armed while `safety.state` is
neither `armed` nor `arming`, the vehicle's `mavlink` module disarms ArduSub and the
`autopilot` trigger fires. `arming` is the only window; there is no other grace. The
one exception is a failsafe `surface` or `hold`, which ArduSub carries out armed; a
stale `safety.state` counts as neither `armed` nor `arming`. A disarm hangar asks for
is not a disagreement: from the moment `safety.state` leaves `armed`, `arming`, or a
failsafe `surface` or `hold`, until ArduSub answers the disarm, `ardusub.state` says
`disarming`, not armed; a refused or unanswered disarm is a disagreement
([10-mavlink](10-mavlink.md#arming-and-failsafe)).

**The Navigator belongs to ArduSub.** The `ardusub` module lists the Navigator's buses
and GPIO groups in `[Module] ExclusiveGroups`
([02-manifest](02-manifest.md#module-required-once)), so no hangar module can drive the
PWM controller, or read a sensor on those buses, behind it. In phase 1, depth, IMU
and other readings on those buses come from ArduSub's telemetry; a module reads a
sensor itself only on a device granted to it apart from them, as the `leak` module
reads the leak probe ([10-mavlink](10-mavlink.md#ardusub-as-a-module)).

**Nothing else moves the thrusters.** The vehicle's `mavlink` module passes
QGroundControl's traffic to ArduSub through an allow-list that drops every message
that could command a thruster, change a failsafe or arm (`RC_CHANNELS_OVERRIDE`,
`MANUAL_CONTROL`, motor tests, servo commands, `PARAM_SET` of protected parameters).
Only pilot buttons with harmless functions reach ArduSub; arm, disarm and mode buttons
become calls, and gain, trim and input-hold functions are masked. The link between the
module and ArduSub is signed, so no other local process can command ArduSub
([10-mavlink](10-mavlink.md#the-rules)).

**ArduSub's own failsafes stay enabled** (GCS lost, pilot input lost, leak, battery,
depth), and arming checks `FS_GCS_ENABLE` and `FS_LEAK_ENABLE`. On tether loss,
ArduSub's GCS failsafe stands behind hangar's `tether` trigger:

- the vehicle's `mavlink` module never sends `HEARTBEAT` under the ground station's
  system id; QGroundControl's heartbeats reach ArduSub through the tunnel unchanged, and
  stop with the tether;
- once `bridge.vehicle.link` has reported no packet from the topside for
  `tether_timeout`, the module also stops sending `MANUAL_CONTROL`, which ArduSub counts
  as contact from its ground station too;
- ArduSub's GCS failsafe then acts `FS_GCS_TIMEOUT` later, whatever hangar did.

If the vehicle's `mavlink` module dies, its `Neutral` command sends ArduSub neutral
manual control and a disarm, and ArduSub, no longer receiving manual control, has its
own pilot-input failsafe as well. If ArduSub dies, its module's `Neutral` command (a
`[Module] Neutral=` key, [02-manifest](02-manifest.md#module-required-once)) puts the
PWM outputs off; how is confirmed on the vehicle's hardware. hangar adds protection
and replaces none.

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
   neutral. For the actuator module itself, this tests its `Neutral` command; for
   ArduSub, its module's `Neutral` command.
3. **A sensor killed** behind a controller (depth behind depth hold): the controller's
   commands are flagged stale, and the gate goes neutral (or the pilot keeps
   control, in `manual`).
4. **The safety module killed**: neutral within `SafetyTimeout` plus one output period;
   in phase 1, ArduSub disarmed. Killed again while in failsafe: it restarts in
   failsafe with the same triggers, and refuses `arm` until `clear`.
5. **The manager killed and restarted**, armed: the vehicle keeps its state; nothing
   jumps. With the manager stopped, the actuator module killed: the restarted manager
   runs its `Neutral` command before anything else.
6. **The tether pulled**, in `manual` with a stick held: the pilot's command drops to
   zero within `pilot.wrench`'s `StaleAfter`, and the `tether` failsafe fires at
   `tether_timeout`. Pulled again with the `tether` action set to `surface`, so that
   ArduSub stays armed: ArduSub's GCS failsafe acts `FS_GCS_TIMEOUT` after the module
   stops sending manual control.
7. **A policy in shadow** producing large commands: nothing reaches the thrusters.
8. **Pilot override** during `policy:<name>` and `auto`: the pilot's axis wins at once.
9. **Envelope**: every source, the pilot's included, held within the limits.
10. **The leak probe** wetted: the `leak` failsafe within one `vehicle.leak` period.
11. **Phase 1 failsafe actions**: `surface` puts ArduSub in SURFACE with neutral input,
    and its switch to DEPTH HOLD at the surface is left alone; `hold` puts it in DEPTH
    HOLD for `hold_time`, then disarms it.
12. **A closed gate in STABILIZE**: the mux stopped while ArduSub is in STABILIZE;
    ArduSub disarmed `CommandTimeout` after the last command.

## Tests

The bench tests above, and in CI:

- the gate's three conditions and their reasons;
- every state transition and refused transition: `arming` published before
  `mavlink.arm` is called; `arming` to `armed` on ArduSub's report, back to
  `disarmed` on its refusal (with its reason) and after 2 s; ArduSub armed while
  `safety.state` is neither `armed` nor `arming` (an arm after the 2 s, an arm from
  elsewhere), disarmed and `autopilot` fired at the first `ardusub.state` that says
  so; an ordinary disarm passes through `disarming` and fires nothing; ArduSub left
  armed through a failsafe `surface` or `hold`;
- each trigger and the combination rule;
- the latch: a safety module restarted in failsafe comes back in failsafe; `clear`
  refused while a module given up with `OnGiveUp=failsafe` is still failed, and
  accepted once it runs again;
- `Allow`: a module not listed is denied `safety.arm`, `safety.clear` and
  `safety.set_params`; any module of either side, `topside:topside_mavlink` included,
  may call `safety.disarm`, `safety.estop` and `safety.check`;
- the mux's per-axis choice, override hold and envelope; its references: a stale or
  absent source that drives nothing leaves `ctl.wrench` unflagged, a flagged source
  that drives flags it;
- shadow mode never applied; `mux.set_mode` refused with a hash other than the running
  model's; a replaced model demoted, except under `UpdateWhileActive=yes` in
  `residual:<name>`; a policy with no running model treated as no contribution.
