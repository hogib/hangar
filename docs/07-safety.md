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
- **The pilot always wins.** Pilot input above the deadband on an axis takes that axis,
  in every mode and in every failsafe action.
- **A new model drives only after an operator promotes it**, naming the model by its
  hash, and in version 1 only in `residual:<name>`: added to a controller's command,
  within the residual envelope, under automatic demotion. Until then its outputs are
  published and recorded, never applied. Once a policy is promoted, a model it swaps
  in after its checks and warm-up drives in the old one's place ([The mux](#the-mux)).
- **Failsafe ends only by an operator**: `safety.clear` once the cause is gone, then a
  fresh `arm`. A restart of the safety module does not end it.
- **While ArduSub drives the thrusters, it is held in MANUAL and stays the last line**:
  hangar acts through it, nothing else talks to it, and its own failsafes stay on.

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
In phase 1 it also needs ArduSub disarmed, in MANUAL, and reporting the parameters
`hangar ardusub-setup` set (`FS_*`, the joystick gains, the button functions,
`SYSID_MYGCS`), as the vehicle's `mavlink` module reads them
([10-mavlink](10-mavlink.md#arming-and-failsafe)). A refused `arm` says which check
failed.

Arming is asynchronous. `safety.arm` runs the checklist and answers `accepted` at once,
or refuses. On `accepted`, `safety` first publishes `safety.state` with state
`arming`, with `outputs_enabled` off, and only then calls `mavlink.arm` with
`hg_call_start` ([04-calls](04-calls.md#the-api)), so that neither its loop nor
`safety.state` ever waits on the autopilot. `mavlink.arm` refuses unless the
`safety.state` it reads says `arming`: the window in which ArduSub may be armed exists
before the arm is sent. When `ardusub.state` reports ArduSub armed, `safety` becomes
`armed`. On a refusal, or with no report after 2 s, it returns to `disarmed` with
ArduSub's reason in `reason` (the `PreArm:` and `Arm:` status texts sent around the
refusal, otherwise the result's name). An arm that ArduSub carries out after that is
undone by the armed agreement ([Phase 1](#phase-1-ardusub-drives-the-thrusters)).
Without an autopilot behind the actuators, `arming` leads to `armed` at once.

| Call | `Allow` | |
|---|---|---|
| `safety.arm` | `topside:pilot_console topside:topside_mavlink admin topside:admin` | answers `accepted` or a refusal; the result is on `safety.state` |
| `safety.disarm` | `* topside:* admin topside:admin` | making the vehicle safer is never refused |
| `safety.estop` | `* topside:* admin topside:admin` | failsafe at once, action `neutral` |
| `safety.clear` | `topside:pilot_console topside:topside_mavlink admin topside:admin` | refused while a trigger is active ([Leaving failsafe](#failsafe)) |
| `safety.check` | `* topside:* admin topside:admin` | runs the arming checklist without arming, and says what would fail (`hangar check`) |

`Allow` tokens are those of [04-calls](04-calls.md#allow-tokens): a module name,
`topside:<module>` for a module of the topside, `*` for every module on this machine,
`topside:*` for every module of the topside, `admin` for `hangar-admin` on this
machine, `topside:admin` for `hangar-admin` on the topside. The default, `* admin`,
admits nobody from the topside, so each method names its topside callers. Disarming,
stopping and checking are open to every module and operator on both sides; arming
and clearing only to the pilot's console, the topside's MAVLink module and the
operators. `topside:topside_mavlink` is the topside's MAVLink module, which turns the
pilot's arm and disarm requests from QGroundControl, commands and buttons alike, into
these calls ([10-mavlink](10-mavlink.md#the-topsides-mavlink-module)). `hangar check`
run on the topside calls `safety.check` as `topside:admin:<user>`.

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
| `residual:<name>` | `assisted`, with a learned policy's output added to the controllers' axes, within the residual envelope ([09-learning](09-learning.md)) |

These are the modes of version 1. `auto` (an autonomy source with an axis) and
`policy:<name>` (a learned policy with the whole axis) come later (**Later**, below);
`mux.set_mode` refuses them until then.

```ini
# /etc/hangar/modules/mux.module
[Module]
Exec=/usr/lib/hangar/mux

[Subscribe pilot.wrench]
[Subscribe ctl.depth_hold]
[Subscribe policy.depth_ff.wrench]
[Subscribe policy.depth_ff.active]
[Subscribe nav.depth]
[Subscribe safety.state]

[Publish ctl.wrench]
Type=wrench
Rate=50
Sources=pilot.wrench ctl.depth_hold policy.depth_ff.wrench nav.depth

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
1; `hangar ardusub-setup` pins ArduSub's joystick gains to 1 and gives every button
function none, so that the fraction does not change with a gain button
([10-mavlink](10-mavlink.md#commands-to-ardusub)). Limits are applied to every source,
the pilot's included, after the choice. In `residual:<name>` the policy's contribution
is first held within the **residual envelope** (by default 20% of the envelope on each
axis), then the sum within the envelope: the controller stays in the loop, and the
network can only correct it by a bounded amount. This is how online adaptation runs
([09-learning](09-learning.md)).

**Provenance.** The mux lists every topic it may reference in `Sources=`
([02-manifest](02-manifest.md#publish-topic-one-per-output)) and publishes each
`ctl.wrench` with `hg_publish_with`, referencing the source messages it used: the one
that drove each axis, and `nav.depth` when the depth band limited a command. The stale
check and its transitive flag apply to those references only
([01-topics](01-topics.md#stale-inputs)). So a flagged source that drives an axis flags
`ctl.wrench`, and the gate goes neutral; a source that drives nothing (an idle
controller, a policy not promoted) never does. A stale pilot input counts as zero and
is not referenced. A message holds at most 4 references: when more sources were used,
the mux references the 4 that drove the most axes, and sets the message's stale flag
itself if any source it used and left out carries one. `mux.state`
(`msg/mux_state.msg`), on every change and at 10 Hz, holds the mode, the source of
each axis (`source_surge`, `source_sway`, `source_heave`, `source_roll`,
`source_pitch`, `source_yaw`: 0 none, 1 the pilot, 2 a controller, 3 an autonomy
source, later, 4 a policy), and which limits are acting. With `ctl.wrench`'s
provenance, the recording says who was driving at every moment, which a dataset can
select on ([06-recorder](06-recorder.md#datasets)).

**Promotion names the model.** A policy's topic is published and recorded whenever the
policy runs; the mux applies it only in mode `residual:<name>`, which only
`mux.set_mode` sets. That call is the promotion of a model: it carries the hash of the
model the operator checked, and is refused unless it is the hash of the policy's
running model, as the newest `policy.<name>.active` message announces it. A policy
with no running model publishes, every tick, a neutral wrench flagged as computed from
stale inputs; the mux treats it as no contribution
([09-learning](09-learning.md#policy-modules)).

**Which model drives.** Every `policy.<name>.wrench` message references, in its
provenance, the `policy.<name>.active` message of the model that computed it
([09-learning](09-learning.md#lineage)). The mux applies a message only if that
reference is the newest `policy.<name>.active` message it has read; a message from any
other model is no contribution for that tick. In `residual:<name>` a new `active`
message, a load that passed the policy's checks and warm-up or a revert, makes that
model the driving one without demotion: the controller stays in the loop and the
network's contribution stays within the residual envelope, which is what lets online
adaptation keep learning during the dive ([09-learning](09-learning.md#loading-weights)).

**Demotion.** The mux falls back to `assisted`, and says why, when the policy's output
goes stale, skips a tick (a policy publishes no output that is not finite), or is
flagged as computed from stale inputs; or when its output is held at an envelope limit
for more than `demote_saturation` (a parameter, 20%) of the last `demote_window`
(5 s). It never promotes on its own.

**Later.** `policy:<name>` gives the network the whole axis; there a replaced model is
always demoted and must be promoted again, and a policy spec's `UpdateWhileActive`
says whether a swap in `residual:<name>` keeps the mode. Shadow evaluation
(`policy.<name>.eval`, the policy's output compared live with what drove) is the
evidence an operator promotes on. Neither is in version 1
([09-learning](09-learning.md#later)).

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
| 5 | `autopilot` | ArduSub disarmed itself while hangar is armed; `ardusub.state` says ArduSub armed while the `safety.state` it references says neither `arming` nor `outputs_enabled` ([Armed agreement](#phase-1-ardusub-drives-the-thrusters)); ArduSub reports a mode other than MANUAL; or ArduSub sent no message of any kind for 3 s ([10-mavlink](10-mavlink.md#arming-and-failsafe)) | `neutral` |
| 6 | `estop` | `safety.estop` | `neutral` |
| 7 | `input` | a topic the safety module needs (`vehicle.leak`, `power.battery`, `nav.depth`, `bridge.vehicle.link`) stale | `neutral` |

`vehicle.leak` is published at 10 Hz by the `leak` module, which reads the leak probe's
GPIO line itself, through a device granted to it alone
([10-mavlink](10-mavlink.md#telemetry)), so a dry probe is a fresh
message, not the absence of one. ArduSub's own failsafe conditions (leak, battery,
ground station lost) are not triggers by themselves: ArduSub acts on them, and hangar's
`leak`, `battery` and `tether` triggers watch the same conditions on hangar's own
topics. A failsafe in which ArduSub disarms itself, or leaves MANUAL, fires `autopilot`.

| Action | |
|---|---|
| `neutral` | `outputs_enabled` off: every actuator to neutral; the vehicle's positive buoyancy brings it up |
| `surface` | the mux commands a vertical thrust up (`surface_thrust`), ignoring every source but the pilot's override |
| `hold` | the mux keeps the current depth through the depth controller for `hold_time`, then `neutral`; `neutral` at once if the depth controller is not ready |

In phase 1 `surface` and `hold` reach ArduSub, held in MANUAL, as `MANUAL_CONTROL`
through the gate like any other command ([Phase 1](#phase-1-ardusub-drives-the-thrusters)).

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

Until hangar drives thrusters itself, the actuator is the vehicle's `mavlink` module
([10-mavlink](10-mavlink.md)), which turns `ctl.wrench` into ArduSub's `MANUAL_CONTROL`
through the same gate. ArduSub is held in MANUAL at all times, armed and disarmed by
hangar, and nothing else talks to it: QGroundControl is not connected to ArduSub, and
the vehicle's `mavlink` module is its only MAVLink peer
([10-mavlink](10-mavlink.md#the-rules)). In MANUAL a `MANUAL_CONTROL` axis is thrust
and neutral input is no thrust, so the gate's neutral stops the thrusters.
Stabilization, depth hold and the failsafe actions are hangar's. hangar's state acts
through ArduSub:

| hangar | sent to ArduSub |
|---|---|
| `arming` | arm, only after `arming` is published; `armed` once ArduSub reports armed ([Arming](#vehicle-states)) |
| `disarm`, failsafe `neutral` | disarm |
| everything else: `armed`, failsafe `surface` and `hold` | `MANUAL_CONTROL` from the gate's command; neutral while the gate is closed |

**Failsafe `surface` and `hold`** run through the gate like any command: `safety.state`
is `failsafe` with `outputs_enabled` on, ArduSub stays armed in MANUAL, and the mux's
heave command (`surface_thrust`, or the depth controller's) reaches it as
`MANUAL_CONTROL`. The pilot's override holds, as in every mode. When `hold` ends, or a
`neutral` trigger joins, hangar disarms ArduSub.

**Armed agreement, both ways.** ArduSub may be armed only while `safety.state` says
`arming`, or says `outputs_enabled` (`armed`, or a failsafe `surface` or `hold`).
ArduSub disarming itself while hangar is `armed` fires the `autopilot` trigger.
Whenever `ardusub.state` says armed while the `safety.state` it references says
neither, the vehicle's `mavlink` module disarms ArduSub and the `autopilot` trigger
fires. `arming` is the only window; there is no other grace; a stale `safety.state`
counts as neither. A disarm hangar asks for is not a disagreement: from the moment
`safety.state` stops saying `arming` or `outputs_enabled`, until ArduSub answers the
disarm, `ardusub.state` says `disarming`, not armed; a refused or unanswered disarm is
a disagreement ([10-mavlink](10-mavlink.md#arming-and-failsafe)).

**MANUAL, always.** hangar never requests a flight mode, and QGroundControl's mode
requests do not reach ArduSub ([10-mavlink](10-mavlink.md#the-topsides-mavlink-module)).
`ardusub.state` reporting any mode other than MANUAL therefore means ArduSub acted on
its own (one of its failsafes, or a mode set from elsewhere): the `autopilot` trigger
fires and the module disarms it, since in STABILIZE or DEPTH HOLD neutral input holds
attitude or depth and the thrusters keep acting.

**The Navigator belongs to ArduSub.** The `ardusub` module lists the Navigator's buses
and GPIO groups in `[Module] ExclusiveGroups`
([02-manifest](02-manifest.md#module-required-once)), so no hangar module can drive the
PWM controller, or read a sensor on those buses, behind it. In phase 1, depth, IMU
and other readings on those buses come from ArduSub's telemetry; a module reads a
sensor itself only on a device granted to it apart from them, as the `leak` module
reads the leak probe ([10-mavlink](10-mavlink.md#ardusub-as-a-module)).

**Nothing else moves the thrusters.** The pilot's joystick reaches the vehicle only as
`pilot.wrench` and `pilot.buttons`, through the mux and the gate; arm and disarm reach
it only as `safety.arm` and `safety.disarm`; a mode request is answered on the topside
and goes no further. ArduSub's parameters are set once, before the dive, by
`hangar ardusub-setup` from a checked-in file, and `hangar check` verifies them
([12-tools](12-tools.md#the-hangar-command)): every `BTN` function none, so no button
can arm, change a mode or a gain, or latch an input, whatever reaches ArduSub; the
joystick gains pinned to 1; the failsafes below as hangar needs them. The link between
the module and ArduSub is signed, so no other local process can command ArduSub
([10-mavlink](10-mavlink.md#the-rules)).

**ArduSub's own failsafes stay enabled** as the last line, for when hangar's module is
not there to act: ground station lost, leak and battery, set by `hangar ardusub-setup`
and checked at every arm. The vehicle's `mavlink` module sends no `HEARTBEAT` under the
ground station's id; its `MANUAL_CONTROL`, sent under that id at 50 Hz, is the contact
ArduSub's GCS failsafe watches, so that failsafe acts `FS_GCS_TIMEOUT` after the module
stops sending: when it dies, and when it stops on purpose, once `bridge.vehicle.link`
has reported no packet from the topside for `tether_timeout`
([10-mavlink](10-mavlink.md#arming-and-failsafe)). On tether loss it therefore stands
behind hangar's `tether` trigger, whatever hangar did.

If the vehicle's `mavlink` module dies, its `Neutral` command sends ArduSub neutral
manual control and a disarm, and ArduSub, no longer hearing from its ground station,
has its GCS and pilot-input failsafes as well. If ArduSub dies, its module's `Neutral`
command (a `[Module] Neutral=` key, [02-manifest](02-manifest.md#module-required-once))
puts the PWM outputs off; how is confirmed on the vehicle's hardware. hangar adds
protection and replaces none.

**Phase 2.** hangar drives the Navigator's PWM itself: the `thrusters` module of
[The actuator gate](#the-actuator-gate) is the actuator and ArduSub is removed. The
gate, the mux and the safety module are unchanged; the `autopilot` trigger no longer
applies, and `arming` leads to `armed` at once ([10-mavlink](10-mavlink.md#phase-2)).

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
7. **A policy not promoted** producing large commands: nothing reaches the thrusters.
8. **Pilot override** during `assisted` and `residual:<name>`: the pilot's axis wins at
   once.
9. **Envelope**: every source, the pilot's included, held within the limits.
10. **The leak probe** wetted: the `leak` failsafe within one `vehicle.leak` period.
11. **Phase 1 failsafe actions**, with ArduSub in MANUAL: `surface` sends heave up at
    `surface_thrust` as `MANUAL_CONTROL`, with the pilot's override; `hold` holds depth
    through hangar's controller for `hold_time`, then disarms ArduSub.
12. **ArduSub out of MANUAL**: put in STABILIZE from outside hangar; disarmed at the
    first `ardusub.state` that says so, and `autopilot` fires.

## Tests

The bench tests above, and in CI:

- the gate's three conditions and their reasons;
- every state transition and refused transition: `arming` published before
  `mavlink.arm` is called; `arming` to `armed` on ArduSub's report, back to
  `disarmed` on its refusal (with its reason) and after 2 s; ArduSub armed while the
  `safety.state` it references says neither `arming` nor `outputs_enabled` (an arm
  after the 2 s, an arm from elsewhere), disarmed and `autopilot` fired at the first
  `ardusub.state` that says so; an ordinary disarm passes through `disarming` and
  fires nothing; ArduSub armed through a failsafe `surface` or `hold` is no
  disagreement; ArduSub in any mode but MANUAL disarmed and `autopilot` fired;
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
- a policy's output never applied outside `residual:<name>`; `mux.set_mode` refused
  with a hash other than the running model's, and refused for `auto` and
  `policy:<name>`; in `residual:<name>` a model swapped in after a load or a revert
  drives without demotion, and a message referencing any other `active` message is no
  contribution; a policy with no running model treated as no contribution; each
  demotion rule.
