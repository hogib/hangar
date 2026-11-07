# MAVLink and ArduSub

In phase 1 ArduSub drives the thrusters and hangar holds it in MANUAL: hangar arms and
disarms it, hangar's mux, controllers and safety module decide every command, and
ArduSub turns the command into PWM. The vehicle's `mavlink` module is ArduSub's only
MAVLink peer. QGroundControl is not connected to ArduSub at all: the pilot keeps it for
the video and the joystick, and the topside's `topside_mavlink` module speaks MAVLink
to it as if it were the vehicle. ArduSub's Navigator Linux build runs as a hangar
module; BlueOS is not used.

## The rules

- **Nothing talks to ArduSub but the vehicle's `mavlink` module** and its `Neutral`
  command, over MAVLink 2 signing. No MAVLink crosses the tether.
- **ArduSub stays in MANUAL.** hangar never requests a flight mode; any other mode is
  a failsafe. Stabilization, depth hold and the failsafe actions `surface` and `hold`
  are hangar's, through the mux ([07-safety](07-safety.md#phase-1-ardusub-drives-the-thrusters)).
  A mode request from QGroundControl is answered on the topside: hangar owns modes.
- **The pilot's commands go through the mux**, and arming through the safety module.
  QGroundControl's `MANUAL_CONTROL` becomes `pilot.wrench` and `pilot.buttons` on the
  topside; its arm and disarm requests become the calls `safety.arm` and
  `safety.disarm` ([04-calls](04-calls.md#the-two-topics)).
- **hangar and ArduSub never disagree about being armed**, either way. hangar arms
  ArduSub; ArduSub disarming itself is a failsafe in hangar; ArduSub armed when hangar
  has not allowed it is disarmed at once, and is a failsafe too.
- **ArduSub alone drives the Navigator.** The `ardusub` module holds the Navigator's
  buses and GPIO exclusively.
- **ArduSub's parameters are set once, before the dive**, by `hangar ardusub-setup`
  from a checked-in file ([12-tools](12-tools.md#hangar-ardusub-setup)); `hangar check`
  verifies them, and `mavlink.arm` refuses while they differ.
- **ArduSub's own failsafes stay on as the backstop**: ground station lost, pilot
  input lost, leak, battery. hangar acts first, on its own topics.
- **Whatever the autopilot reports is a topic**, stamped in hangar's time, and its raw
  MAVLink is recorded with every session.

## The path

```
 topside                                                 vehicle
 ───────                                                 ───────
 QGroundControl                                          ArduSub (module `ardusub`)
   ▲ udp 127.0.0.1:14550                                   ▲ udp 127.0.0.1:14660, signed
   ▼                                                       ▼
 topside_mavlink ═ pilot.wrench, pilot.buttons ═► mux ─ ctl.wrench ─► mavlink
   │ ═══ topside_mavlink.req (safety.arm, safety.disarm) ═► safety ─ safety.req ─► mavlink.arm
   │ ◄══ safety.rep ═══════════════════════════════
   │ ◄══ safety.state, ardusub.state, nav.*, power.battery ═══  telemetry topics

 ═══ crosses the tether inside the bridge, as topics
```

Module names are unique across the vehicle and the topside together
([01-topics](01-topics.md#names)): `mavlink` on the vehicle, `topside_mavlink` on the
topside.

## ArduSub as a module

```ini
# /etc/hangar/modules/ardusub.module
[Module]
Description=ArduSub autopilot (Navigator)
Exec=/usr/lib/hangar/ardusub <serial and board options; MAVLink: a UDP client to 127.0.0.1:14660>
User=hg-ardusub
SupplementaryGroups=nav-spi nav-i2c nav-gpio hg-autopilot
ExclusiveGroups=nav-spi nav-i2c nav-gpio
Capabilities=CAP_SYS_NICE
Legacy=yes
Neutral=/usr/lib/hangar/ardusub-pwm-off

[Restart]
Policy=always
OnGiveUp=failsafe

[Schedule]
LockMemory=yes
```

ArduSub does not use hangar's library, so it is a `Legacy` module
([02-manifest](02-manifest.md#module-required-once)): ready while its process runs,
restarted when it exits. Its health is judged by the `mavlink` module, through
`ardusub.state` ([Arming and failsafe](#arming-and-failsafe)). It drives the PWM
hardware, so it has a `[Module] Neutral` command, which the manager runs whenever the
module exits: it puts the PWM outputs off. How it does that on the Navigator
(output-enable line, or the PWM controller's outputs written off) is confirmed on the
hardware. For a `Legacy` module, `LockMemory=yes` only leaves `RLIMIT_MEMLOCK` raised;
ArduPilot's Linux HAL locks its own memory.

**The Navigator is ArduSub's.** A udev rule gives its SPI and I2C buses and its GPIO
chip to the groups `nav-spi`, `nav-i2c` and `nav-gpio`, listed in
`[Module] ExclusiveGroups`: the manager refuses any other module in them. Depth, IMU
and the other readings on those buses come from ArduSub's telemetry
([Telemetry](#telemetry)); a hangar module reads a sensor itself only on a device apart
from them, as the `leak` module does. ArduPilot's Linux builds set their own threads'
priorities, so the module is granted `CAP_SYS_NICE`; the devices and options the
deployed build needs are confirmed on the vehicle. ArduSub is a UDP client to
`127.0.0.1:14660`, which the `mavlink` module binds (and, when the module is down, its
`Neutral` command).

### Signing

The link between ArduSub and the `mavlink` module is MAVLink 2 signed. The key is
`/etc/hangar/mavlink.key`, readable only by group `hg-autopilot`, whose members are
`hg-mavlink` and `hg-ardusub`. ArduSub holds the same key and requires signing on that
channel; how the key is installed in the deployed build is confirmed on the vehicle.
The module signs everything it sends, and so does its `Neutral` command, which the
manager runs as the module's user and groups. A message from any other local process
sent to ArduSub's port is rejected by ArduSub.

### The ground station's id

ArduSub accepts pilot input only from its ground station's system id (`SYSID_MYGCS`,
renamed `MAV_GCS_SYSID` in later ArduPilot versions; the module uses the name of the
deployed version). The `mavlink` module is that ground station: it sends
`MANUAL_CONTROL` under that id, and **no `HEARTBEAT`**. ArduSub counts
`MANUAL_CONTROL` from that id as contact with its ground station and as pilot input,
so its ground-station and pilot-input failsafes fire `FS_GCS_TIMEOUT` after the module
stops sending: when the module dies, and when it stops `MANUAL_CONTROL` on purpose,
once `bridge.vehicle.link` has reported no packet from the topside for the safety
module's `tether_timeout` (from `safety.params`). On tether loss ArduSub therefore acts
behind hangar's `tether` trigger, whatever hangar did.

### ArduSub's parameters

`/etc/hangar/ardusub.params` pins the joystick gains to 1, every button function to
none, the failsafes hangar relies on, and `SYSID_MYGCS`; its contents and the
`hangar ardusub-setup` command are in [12-tools](12-tools.md#hangar-ardusub-setup).
The module serves `mavlink.setup`, accepted only while `safety.state` is `disarmed`
and ArduSub is disarmed: beside its loop it sends a `PARAM_SET` for each line of the
file and publishes each `PARAM_VALUE` it receives on `ardusub.param`. At start, and
after each ArduSub restart, it reads every parameter the file names the same way.
`ardusub.state.params` says whether all of them read as the file says; `mavlink.arm`
refuses otherwise, naming the first that differs.

`ardusub.param`, an event topic, one message per `PARAM_VALUE` received:

```
char id[17]      # the parameter's name
u8   type        # MAV_PARAM_TYPE
u8   reserved[2]
f32  value
u16  index
u16  count
```

## The vehicle's `mavlink` module

```ini
# /etc/hangar/modules/mavlink.module   (vehicle)
[Module]
Exec=/usr/lib/hangar/hangar-mavlink /etc/hangar/mavlink.conf
User=hg-mavlink
SupplementaryGroups=hg-autopilot hangar-rec

[Subscribe ctl.wrench]
[Subscribe safety.state]
[Subscribe safety.params]
[Subscribe bridge.vehicle.link]
[Subscribe recorder.session]
[Subscribe hangar.vehicle.modules]

[Publish ardusub.manual_control]
Type=manual_control
Rate=50
Actuator=yes
Inputs=ctl.wrench

[Publish ardusub.state]
Type=ardusub_state
Rate=10
# Each message names the safety.state it was judged against (Armed agreement).
Inputs=safety.state

[Publish ardusub.param]
Type=ardusub_param

[Actuator]
Command=ctl.wrench
Neutral=/usr/lib/hangar/hangar-mavlink --neutral /etc/hangar/mavlink.conf

[Serve arm]
Request=arm_req
Reply=arm_rep
Allow=safety

[Serve setup]
Request=empty
Reply=empty
Allow=admin topside:admin
```

plus the telemetry topics of its configuration ([Telemetry](#telemetry)). It is an
actuator module, so its methods admit only the callers listed. The manager adds the
request topics of its callers (`safety.req`, `hangar.vehicle.admin.req`, and
`subglob *.req` for the topside's `hangar.topside.admin.req`, since `mavlink.setup` is
in the bridge's `[Calls] Remote=`) and its reply topic `mavlink.rep` to its wiring
([04-calls](04-calls.md#the-two-topics),
[08-bridge](08-bridge.md#calls-across-the-tether)). Group
`hg-autopilot` reads the signing key; group `hangar-rec` writes into the recorder's
sessions ([06-recorder](06-recorder.md#sessions)).

The module waits on its UDP socket and its topics together through `hg_wait_fd`
([01-topics](01-topics.md#reading)), and polls the request topics its `subglob *.req`
matches on a 10 ms timer of its own, as every module waiting that way does. No handler
waits on ArduSub: its 50 Hz output never stalls.

### Commands to ArduSub

At 50 Hz, the module sends `MANUAL_CONTROL` built from the gate's command
([07-safety](07-safety.md#the-actuator-gate)), or neutral when the gate is closed:

| `wrench` axis (−1 to 1) | `MANUAL_CONTROL` field | Neutral |
|---|---|---|
| surge | `x` (−1000 to 1000) | 0 |
| sway | `y` | 0 |
| heave | `z` (0 to 1000) | 500 |
| yaw | `r` | 0 |
| roll, pitch | the MAVLink 2 extension fields, where the deployed ArduSub reads them | 0 |
| | `buttons`, `buttons2` | always 0: ArduSub acts on no button |

Each axis is checked for finiteness before anything else: a non-finite axis makes the
whole message neutral. The axes are then clamped to −1 to 1, and only then converted to
the integer fields. What the module sends is published as `ardusub.manual_control`,
the command as it left the gate. It is not the thrust applied: ArduSub's mixer scales
every axis down by the largest input, so the pulse widths actually sent are `thr.out`
([Telemetry](#telemetry)), which the adaptive depth hold labels with
([09-learning](09-learning.md#example-adaptive-depth-hold)).

**Units.** `wrench` holds normalized axis commands: fractions, from −1 to 1, of
ArduSub's full command at pilot gain 1. The mux's envelope is in the same units. The
parameter file pins the gain to 1, and no button can change it.

**In MANUAL, neutral is stop.** ArduSub starts in MANUAL, and hangar never asks for
another mode. `ardusub.state` reporting any other mode means ArduSub acted on its own
(a failsafe of its own, or a mode set from elsewhere), and in any other mode neutral
input is not stop: the module disarms ArduSub at the first such message and the
`autopilot` trigger fires ([Arming and failsafe](#arming-and-failsafe)). `mavlink.arm`
refuses while ArduSub is out of MANUAL; an ArduSub restart (`hangar restart ardusub`)
brings it back.

The `Neutral` command, which the manager runs if the module exits for any reason, sends
ArduSub neutral manual control and a disarm, signed. ArduSub also stops receiving
manual control, and its own failsafes act. The meaning of each axis in MANUAL is
verified against ArduSub's source for the version deployed; the table is configuration.

### Arming and failsafe

Arming is asynchronous: no call waits on ArduSub.

1. `safety.arm` runs the safety module's checklist and answers `accepted`.
2. The safety module publishes `safety.state` with state `arming` (`outputs_enabled`
   off) first, and only then calls `mavlink.arm` with `hg_call_start`
   ([07-safety](07-safety.md#vehicle-states)). The `arming` message is the window in
   which ArduSub may be armed ahead of hangar, so it must exist before the arm is sent.
3. `mavlink.arm` refuses at once, with the reason, unless the newest `safety.state` it
   reads says `arming`, ArduSub is disarmed and in MANUAL, and `ardusub.state.params`
   is 1. Otherwise it sends ArduSub `MAV_CMD_COMPONENT_ARM_DISARM` and answers
   `accepted`.
4. ArduSub answers with `COMMAND_ACK`, a bare `MAV_RESULT`, and gives its reasons as
   `STATUSTEXT` lines before or after it. The module collects the lines that start
   `PreArm:` or `Arm:` from the send until 500 ms after the ack, and publishes them in
   `ardusub.state`, or the `MAV_RESULT` name when there are none.
5. When `ardusub.state` reports ArduSub armed, the safety module enters `armed`; on a
   refusal, or after 2 s, it returns to `disarmed` with ArduSub's reason.

| `safety.state` | the module does |
|---|---|
| `arming` | arms ArduSub on `mavlink.arm` |
| `outputs_enabled` on: `armed`, failsafe `surface` or `hold` | sends the gate's command; the mux carries out the failsafe action |
| anything else: `disarmed`, failsafe `neutral`, stale | disarms ArduSub; neutral manual control |

ArduSub may be armed only while `safety.state` says `arming` or `outputs_enabled`. A
`safety.state` older than the gate's `SafetyTimeout` counts as neither.

**Armed agreement.** Whenever `ardusub.state` says armed outside that window, the
module disarms ArduSub and the safety module's `autopilot` trigger fires. `arming` is
the only window in which ArduSub may be armed before hangar is; there is no other
grace. This covers a lost disarm, an arm that arrives after `arming` has ended, and an
arm from anywhere but hangar.

A disarm that hangar asks for is not a disagreement. When `safety.state` leaves the
window, or goes stale, the module sends the disarm at once, and `ardusub.state` says
`disarming` (`armed` = 2) until ArduSub answers: `disarmed` when it accepts, `armed`
again when it refuses or has not answered three sends 100 ms apart. ArduSub answers
within milliseconds, so `disarming` is not a grace. Each `ardusub.state` message
references the `safety.state` message the module acted on (`Inputs=safety.state`), and
the safety module judges it against that message, not against a newer one the module
has not yet read.

`ardusub.state`, at 10 Hz, on every change, and after the module acts on each new
`safety.state`:

```
u8   armed            # 0 disarmed, 1 armed (HEARTBEAT base_mode), 2 disarming:
                      # a disarm hangar asked for, sent and not yet answered
u8   system_status    # HEARTBEAT system_status: CRITICAL or EMERGENCY in an ArduSub failsafe
u8   params           # 0 not read yet, 1 every parameter of the file reads as the
                      # file says, 2 a mismatch (the module's status line names it)
u8   reserved
u32  mode             # ArduSub's flight mode (HEARTBEAT custom_mode); 19 is MANUAL
u64  last_rx_ns       # when the last message of any kind arrived from ArduSub
u32  arm_attempt      # counts mavlink.arm requests
u8   arm_result       # MAV_RESULT of the last attempt; 255 while pending
u8   reserved2[3]
char arm_reason[100]  # the PreArm:/Arm: lines, or the MAV_RESULT name
char failsafe[50]     # ArduSub's last failsafe STATUSTEXT
```

The safety module's **`autopilot`** trigger ([07-safety](07-safety.md#failsafe)) fires,
from `ardusub.state`, when:

- ArduSub disarms while `safety.state` says `outputs_enabled`;
- `ardusub.state` says armed while the `safety.state` it references says neither
  `arming` nor `outputs_enabled` (Armed agreement, above), at the first such message;
- `mode` is not MANUAL ([Commands to ArduSub](#commands-to-ardusub));
- no message of any kind has come from ArduSub for 3 s. ArduSub sends `HEARTBEAT` at
  1 Hz and its telemetry at up to 50 Hz, so 3 s is never ordinary jitter.

ArduSub's own failsafe reports (`system_status`, the `failsafe` text) are recorded but
fire nothing by themselves: ArduSub acts on them, and a failsafe in which it disarms or
leaves MANUAL fires `autopilot` through that. Its leak failsafe is the backstop to the
`leak` module, which reads the probe itself ([Telemetry](#telemetry)).

## The topside's MAVLink module

`topside_mavlink` is what QGroundControl talks to. QGroundControl keeps its default
UDP auto-connect: the module sends to `127.0.0.1:14550` from its own port,
`127.0.0.1:14650`, and QGroundControl answers there. To QGroundControl the module is
the vehicle: system id 1, a `MAV_TYPE_SUBMARINE` with a generic autopilot, so that
QGroundControl expects no ArduPilot parameter set.

```ini
# /etc/hangar/modules/topside_mavlink.module   (topside)
[Module]
Exec=/usr/lib/hangar/hangar-topside-mavlink /etc/hangar/topside_mavlink.conf
User=hg-tmavlink

[Subscribe safety.state]
[Subscribe ardusub.state]
[Subscribe nav.attitude]
[Subscribe nav.depth]
[Subscribe power.battery]
[Subscribe hangar.vehicle.system]
[Subscribe bridge.topside.link]

[Publish pilot.wrench]
Type=wrench
Rate=50

[Publish pilot.buttons]
Type=buttons
Rate=50

# Methods of the other side carry the types the caller was built with.
[Call safety.arm]
Request=empty
Reply=empty

[Call safety.disarm]
Request=empty
Reply=empty
```

**From QGroundControl:**

| Message | Becomes |
|---|---|
| `MANUAL_CONTROL` axes | `pilot.wrench`, the joystick axes normalized to −1 to 1, at 50 Hz |
| `MANUAL_CONTROL` buttons | `pilot.buttons`, and the arm and disarm calls ([Buttons](#buttons)); one `pilot.buttons` message with every `pilot.wrench` |
| `MAV_CMD_COMPONENT_ARM_DISARM` (`COMMAND_LONG`, `COMMAND_INT`) | `safety.arm` or `safety.disarm`, with `hg_call_start`; `COMMAND_ACK` `MAV_RESULT_ACCEPTED` when the call is accepted, `MAV_RESULT_DENIED` otherwise, with the call's reason in a `STATUSTEXT`; the outcome shows in the module's `HEARTBEAT`, from `safety.state` |
| `SET_MODE`, `MAV_CMD_DO_SET_MODE` | `COMMAND_ACK` `MAV_RESULT_DENIED` and the `STATUSTEXT` `hangar owns modes` |
| `PARAM_REQUEST_LIST`, `PARAM_REQUEST_READ` | a small fixed parameter list, so that QGroundControl's connection completes; `PARAM_SET` is denied |
| `MISSION_REQUEST_LIST` | a count of 0 |
| `HEARTBEAT`, `REQUEST_DATA_STREAM`, `MAV_CMD_SET_MESSAGE_INTERVAL` | ignored: the module's rates are its own |
| anything else | `MAV_RESULT_DENIED` for a command; dropped otherwise |

**To QGroundControl**, from the topics the bridge mirrors up
([08-bridge](08-bridge.md#configuration)), only while `bridge.topside.link` is `up`:

| Message | From |
|---|---|
| `HEARTBEAT`, 1 Hz | `safety.state`: armed flag from `outputs_enabled`; custom mode MANUAL; `system_status` CRITICAL in `failsafe` |
| `ATTITUDE` | `nav.attitude` |
| `SCALED_PRESSURE2`, `VFR_HUD` | `nav.depth` |
| `SYS_STATUS`, `BATTERY_STATUS` | `power.battery` |
| `STATUSTEXT` | every change of `safety.state` with its `reason`, every refused call, `hangar.vehicle.system` leaving `nominal`, `ardusub.state.failsafe` |

With the link down the module stops its `HEARTBEAT`, and QGroundControl shows the
vehicle lost, which is true. What the HUD shows is as old as the mirrors. Which
parameter list and `HEARTBEAT` fields QGroundControl needs before it enables the
joystick and shows the HUD for a generic autopilot is confirmed against the version
deployed.

`pilot.wrench` and `pilot.buttons` are published on the topside, where the pilot is,
and mirrored down together at `command` priority. The calls ride on the module's
request topic, `topside_mavlink.req`, mirrored down, and the replies come back on
`safety.rep`, mirrored up ([08-bridge](08-bridge.md#calls-across-the-tether)). On the
vehicle the caller is `topside:topside_mavlink`, which the `Allow` lists of
`safety.arm` and `safety.disarm` name ([04-calls](04-calls.md#callers)). hangar accepts
MAVLink only on `127.0.0.1`, on either side; MAVLink reaching QGroundControl's listener
from the network goes to QGroundControl alone, never to the vehicle.

### Buttons

ArduSub acts on no button: every `BTNn_FUNCTION` and `BTNn_SFUNCTION` is none, and the
vehicle's module sends `buttons` and `buttons2` as 0. What a button does is decided on
the topside, in `topside_mavlink.conf`:

```ini
[Button 2]
Function=arm

[Button 3]
Function=disarm

[Button 6]
Function=pass
```

| `Function` | On a press |
|---|---|
| `arm`, `disarm` | the call, once per press |
| `pass` | the button's bit set in `pilot.buttons` while it is held |
| a button with no section | ignored |

`pilot.buttons` (type `buttons`): `u32 pressed`, bit *n* for QGroundControl's button
*n* among the buttons marked `pass`; `u32 reserved`. A hangar module on the vehicle
that acts on a button (lights or a camera tilt on a device of its own) reads it and
treats a message older than its own timeout as no button pressed. Nothing on the
Navigator's outputs is driven by it in phase 1.

## Telemetry

The vehicle's module turns ArduSub's messages into topics, by a table in its
configuration, and requests each message's rate from ArduSub
(`MAV_CMD_SET_MESSAGE_INTERVAL`) at start and after each ArduSub restart (a new
instance in `hangar.vehicle.modules`, or `time_boot_ms` going backwards). Nothing else
is on the channel, so nothing else changes the rates.

```ini
[Telemetry ATTITUDE]
Topic=nav.attitude
Rate=50

[Telemetry SERVO_OUTPUT_RAW]
Topic=thr.out
Rate=50
```

| Default | From | |
|---|---|---|
| `nav.attitude` | `ATTITUDE` | re-emitted to QGroundControl |
| `nav.imu` | `SCALED_IMU` (IMU instance 0) or `RAW_IMU` | 50 Hz by default. The Navigator has one IMU; its rate is that of MAVLink streaming, not the IMU's sample rate |
| `nav.depth` | `SCALED_PRESSURE2` (the depth sensor) and the configured fluid density | hangar's depth controller, the `depth` trigger, the HUD |
| `power.battery` | `SYS_STATUS`, `BATTERY_STATUS` | the safety module's `battery` trigger |
| `ardusub.state` | `HEARTBEAT`, `COMMAND_ACK`, `STATUSTEXT` | the `autopilot` trigger ([Arming and failsafe](#arming-and-failsafe)) |
| `ardusub.param` | `PARAM_VALUE` | [ArduSub's parameters](#ardusubs-parameters) |
| `thr.out` | `SERVO_OUTPUT_RAW` | the pulse widths actually sent to the thrusters, after ArduSub's mixing: what a dataset needs as the vehicle's true action |

**Leak.** ArduSub reports a leak only as a `STATUSTEXT` every 20 s and a `HEARTBEAT`
`system_status` shared with every other failsafe, so a rated leak topic cannot come
from it. `vehicle.leak` is published at 10 Hz by the `leak` module, which reads the
probe through a GPIO character device granted to it alone, never one of the `ardusub`
module's exclusive groups ([07-safety](07-safety.md#failsafe)). How the probe reaches
both (a probe line on a GPIO chip apart from the Navigator's, or a second probe) is
confirmed on the vehicle's hardware.

**Time.** ArduSub stamps its messages with its own boot clock (`time_boot_ms`,
`time_usec`), which starts at 0 with each ArduSub process. The module estimates the
offset to hangar's monotonic clock by a rule like the bridge's across the tether
([08-bridge](08-bridge.md#clocks)), from arrival times alone: over a sliding window of
10 s, it keeps the offset of the message that arrived soonest after its stamp. It
starts over when
`time_boot_ms` goes backwards or the `ardusub` module's instance changes, so stamps
never jump back after an ArduSub restart. Each topic message is stamped (`stamp_ns`)
in hangar's time; a message with no time field (`HEARTBEAT`, `SYS_STATUS`,
`BATTERY_STATUS`) with its arrival time.

**Raw MAVLink.** The module also writes every message exchanged with ArduSub, both
ways, as a `.tlog` into the current recorder session (following `recorder.session`;
the session directory is group `hangar-rec`, mode 2770), readable by QGroundControl
and pymavlink. The topics are what hangar uses; the log is everything.

## Phase 2

hangar drives the Navigator's PWM itself: a `thrusters` module, the actuator behind the
same gate, mixes `ctl.wrench` into pulse widths ([02-manifest](02-manifest.md#example)),
and the `ardusub` and `mavlink` modules are removed. Depth, IMU and the other Navigator
sensors become hangar modules on the buses ArduSub held. The hard part is what ArduSub
provides today: the IMU filtering and the attitude estimate that stabilization and
depth hold rest on. Phase 1 exists so that everything else in these docs runs, is
recorded and is learned from before that is written. `topside_mavlink` is unchanged:
it never spoke to ArduSub.

## Tests

ArduSub runs as SITL (software in the loop) on x86-64, so the whole path is tested in
CI without a vehicle: SITL as the `ardusub` module, both MAVLink modules, the bridge
between two network namespaces, and a scripted ground station (pymavlink) in place of
QGroundControl.

- Commands: joystick input reaches SITL only through the mux; gate closed, neutral; a
  non-finite axis, neutral; `buttons` and `buttons2` always 0.
- Mode: SITL put in STABILIZE from outside hangar while armed: disarmed at the first
  `ardusub.state` that says so, `autopilot` fired; `mavlink.arm` refused until SITL is
  restarted in MANUAL.
- Parameters: `mavlink.setup` sets the file and every `ardusub.param` reads back as the
  file says, `params` 1; refused while armed; a parameter changed in SITL gives
  `params` 2, `mavlink.arm` refused naming it, `hangar check` failing; read again after
  a SITL restart.
- Arming: `arming` published before `mavlink.arm` is called; SITL armed, then
  `safety.state` armed; a pre-arm refusal returned with the `PreArm:` text; no answer
  within 2 s returns to `disarmed`; `mavlink.arm` refused while `safety.state` is not
  `arming`; the 50 Hz output keeps its rate throughout.
- Agreement: SITL disarming itself puts hangar in failsafe; SITL armed behind hangar's
  back, or after `arming` timed out, is disarmed and `autopilot` fires at the first
  `ardusub.state` that says so; an ordinary disarm passes through `disarming` and fires
  nothing; a refused or unanswered disarm fires `autopilot`; SITL stays armed through a
  failsafe `surface` or `hold`, and the gate's command reaches it.
- Hardware: a module other than `ardusub` in `nav-spi`, `nav-i2c` or `nav-gpio`
  refused by the manager.
- Silence: SITL stopped; `autopilot` fires after 3 s, and not before.
- Signing: an unsigned or wrongly signed `MANUAL_CONTROL`, arm or
  `RC_CHANNELS_OVERRIDE` from another local process, sent to SITL's port: rejected.
- Topside: the ground station's `MANUAL_CONTROL` becomes `pilot.wrench` and
  `pilot.buttons` at 50 Hz; a button marked `arm` makes one call per press, one marked
  `pass` sets its bit while held, an unmarked one does nothing; an arm command answered
  `ACCEPTED` and shown armed in the `HEARTBEAT` once `safety.state` is; a mode request
  answered `DENIED` with `hangar owns modes`, SITL's mode unchanged; `PARAM_SET`
  denied; nothing from the ground station ever reaches SITL; `ATTITUDE`, depth and
  battery follow the mirrored topics; the `HEARTBEAT` stops within one period of the
  link going down and resumes with it.
- Rates kept at their declared values, and requested again after a SITL restart.
- Killing the vehicle's `mavlink` module: its `Neutral` command disarms SITL, and
  SITL's own failsafe fires `FS_GCS_TIMEOUT` later. Killing SITL: the `ardusub`
  module's `Neutral` command runs.
- Tether cut: hangar's `tether` failsafe; the module stops `MANUAL_CONTROL` at
  `tether_timeout`; SITL's ground-station failsafe acts behind it.
- Time: stamps against SITL's known clock, continuing forward across a SITL restart.
- Raw log: every message both ways in the session's `.tlog`, readable by pymavlink.
