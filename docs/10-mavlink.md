# MAVLink and ArduSub

While ArduSub drives the thrusters, hangar sits between it and the pilot's
QGroundControl. The pilot keeps QGroundControl, its joystick setup and its views;
hangar's mux, safety module and recorder see, decide and record everything that
reaches the autopilot. ArduSub's Navigator Linux build runs as a hangar module;
BlueOS is not used.

## The rules

- **Nothing talks to ArduSub but hangar.** The vehicle's `mavlink` module owns
  ArduSub's MAVLink endpoint, and the link between them is signed (MAVLink 2
  signing). QGroundControl reaches ArduSub only through the module, which forwards
  what comes from QGroundControl by an allow-list ([The tunnel](#the-tunnel)).
- **The pilot's commands go through the mux**, and arming through the safety module.
  QGroundControl's joystick input becomes `pilot.wrench`; its arm and disarm requests
  become `safety.arm` and `safety.disarm`; its mode requests become a call to the
  vehicle's `mavlink` module. No MAVLink that moves the thrusters crosses the tether.
- **The control path is authenticated across the tether.** QGroundControl talks to
  `127.0.0.1` on the topside; MAVLink crosses the tether only inside the bridge; on
  the vehicle, ArduSub accepts only signed MAVLink. On the topside, local processes
  are trusted.
- **hangar and ArduSub never disagree about being armed**, either way. hangar arms
  ArduSub; ArduSub disarming itself is a failsafe in hangar; ArduSub armed while
  `safety.state` is neither `armed` nor `arming` is disarmed at once and is a failsafe
  too.
- **ArduSub alone drives the Navigator.** The `ardusub` module holds the Navigator's
  buses and GPIO exclusively; no other module can reach the hardware behind it.
- **ArduSub's own failsafes stay on**, behind hangar's
  ([07-safety](07-safety.md#phase-1-ardusub-drives-the-thrusters)). The ones hangar
  relies on are checked at every arm.
- **Whatever the autopilot reports is a topic**, stamped in hangar's time, and its raw
  MAVLink is recorded with every session.

## The path

```
 topside                                                vehicle
 ───────                                                ───────
 QGroundControl                                         ArduSub (module `ardusub`)
   ▲ udp 127.0.0.1:14550                                  ▲ udp 127.0.0.1:14660, signed
   ▼                                                      ▼
 topside_mavlink ═ pilot.wrench ═► mux ─ ctl.wrench ─►  mavlink
   │ ═══ pilot.buttons ═══════════════════════════════►   │
   │ ═══ safety.arm, safety.disarm ═► safety ─────────►   │ mavlink.arm
   │ ═══ mavlink.set_mode ════════════════════════════►   │
   │ ◄══ ardusub.buttons ══════════════════════════════   │
   │ ═══ tunnel: other MAVLink ═══════════════════════►   │ allow-list
   │ ◄══ tunnel: telemetry ════════════════════════════   │

 ═══ crosses the tether inside the bridge
```

Module names are unique across the vehicle and the topside together
([01-topics](01-topics.md#names)), so the two ends have different names: `mavlink` on
the vehicle, `topside_mavlink` on the topside.

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
module exits, as it does for an actuator: it puts the PWM outputs off. How it does that
on the Navigator (output-enable line, or the PWM controller's outputs written off) is
to be confirmed on the hardware. For a `Legacy` module, `LockMemory=yes` only leaves
`RLIMIT_MEMLOCK` raised; the program locks its own memory.

**The Navigator is ArduSub's.** A udev rule gives the Navigator's SPI and I2C buses and
its GPIO chip to the groups `nav-spi`, `nav-i2c` and `nav-gpio`, and the module lists
them in `[Module] ExclusiveGroups` ([02-manifest](02-manifest.md#module-required-once)):
the manager refuses any other module in them, as it does for an actuator's
`[Actuator] Groups`. No hangar module can therefore write the PWM controller, or read
a sensor on those buses, behind ArduSub's back. While ArduSub drives the thrusters,
the depth, IMU and other readings that live on the Navigator's buses come from
ArduSub's telemetry ([Telemetry](#telemetry)); a hangar module reads a sensor itself
only on a device granted to it apart from those groups (another bus, or another GPIO
chip), as the `leak` module does.

ArduPilot's Linux builds set the priorities of their own threads, so the module is
granted `CAP_SYS_NICE`. Exactly which devices, capabilities and options the deployed
ArduSub build needs, and whether it locks its own memory, is to be confirmed on the
vehicle, and the template written from that.

ArduSub is a UDP client to `127.0.0.1:14660`, which the `mavlink` module binds (and,
when the module is down, its `Neutral` command).

### Signing

The link between ArduSub and the `mavlink` module is MAVLink 2 signed. The key is
`/etc/hangar/mavlink.key`, readable only by group `hg-autopilot`, whose members are
`hg-mavlink` and `hg-ardusub`. ArduSub holds the same key and is set up to require
signing on that channel; how the key is installed in the deployed build is confirmed
on the vehicle with its other options. The module signs everything it sends to ArduSub,
messages forwarded from QGroundControl included, and so does its `Neutral` command,
which the manager runs as the module's user and groups. A message from any other local
process sent to ArduSub's port is rejected by ArduSub.

### The ground station's id

ArduSub accepts pilot input only from its ground station's system id (`SYSID_MYGCS`,
renamed `MAV_GCS_SYSID` in later ArduPilot versions; the module uses the name of the
deployed version). The vehicle's `mavlink` module sends `MANUAL_CONTROL` under that
id, as QGroundControl does (255 by default), but sends **no `HEARTBEAT`** under it.
QGroundControl's own heartbeats reach ArduSub unchanged through the tunnel, so
ArduSub's GCS failsafe follows QGroundControl across the tether: when the tether is
lost, the heartbeats stop, and ArduSub acts on its own behind hangar's `tether`
trigger. ArduSub also counts `MANUAL_CONTROL` from that id as contact with its ground
station, so once `bridge.vehicle.link` has reported no packet from the topside for the
safety module's `tether_timeout` (from `safety.params`), the module stops sending
`MANUAL_CONTROL` until the link is back. `FS_GCS_ENABLE` must be on; `mavlink.arm`
refuses otherwise.

## The vehicle's `mavlink` module

```ini
# /etc/hangar/modules/mavlink.module   (vehicle)
[Module]
Exec=/usr/lib/hangar/hangar-mavlink /etc/hangar/mavlink.conf
User=hg-mavlink
SupplementaryGroups=hg-autopilot hangar-rec

[Subscribe ctl.wrench]
[Subscribe pilot.buttons]
[Subscribe mux.state]
[Subscribe safety.state]
[Subscribe safety.params]
[Subscribe bridge.vehicle.link]
[Subscribe recorder.session]
[Subscribe hangar.vehicle.modules]
[Subscribe policy.*.active]

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

[Publish ardusub.buttons]
Type=ardusub_buttons

[Actuator]
Command=ctl.wrench
Neutral=/usr/lib/hangar/hangar-mavlink --neutral /etc/hangar/mavlink.conf

[Serve arm]
Request=arm_req
Reply=arm_rep
Allow=safety

[Serve set_mode]
Request=set_mode_req
Reply=set_mode_rep
Allow=topside:topside_mavlink admin topside:admin
```

plus the telemetry topics of its configuration ([Telemetry](#telemetry)). It is an
actuator module, so its methods admit only the callers listed. Group `hg-autopilot`
reads the signing key; group `hangar-rec` writes into the recorder's sessions
([06-recorder](06-recorder.md#sessions)).

The module waits on its UDP sockets and its topics together through `hg_wait_fd`
([01-topics](01-topics.md#reading)). It makes calls only with `hg_call_start`
([04-calls](04-calls.md#using-calls)), and no handler waits on ArduSub: its 50 Hz output
never stalls.

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
| buttons | `buttons` and `buttons2`, from `pilot.buttons` ([Buttons](#buttons)) | 0 |

Each axis is checked for finiteness before anything else: a non-finite axis makes the
whole message neutral. The axes are then clamped to −1 to 1, and only then converted to
the integer fields. What the module sends is published as `ardusub.manual_control`,
the command as it left the gate; a dataset uses it as the thrust command applied
([09-learning](09-learning.md#example-adaptive-depth-hold)).

**Units.** In phase 1, `wrench` holds normalized axis commands: fractions, from −1 to
1, of ArduSub's full command at pilot gain 1. The mux's envelope is in the same units.
ArduSub scales every axis by its pilot gain, which its gain buttons change at run
time; the module pins it. At start it sets `JS_GAIN_DEFAULT`, `JS_GAIN_MIN` and
`JS_GAIN_MAX` to 1 and `JS_THR_GAIN` to 1, and `hangar check` verifies them
([12-tools](12-tools.md#hangar-check)). The gain buttons are masked
([Buttons](#buttons)).

**Neutral is not stop in every mode.** In MANUAL, neutral input means no thrust. In
STABILIZE and DEPTH HOLD it means "hold attitude" or "hold depth", and the thrusters
keep acting. So, while `safety.state` is `armed` and ArduSub is armed in any mode other
than MANUAL, a gate closed for longer than `CommandTimeout` makes the module disarm
ArduSub. The failsafe actions `surface` and `hold`, which put ArduSub in modes of their
own ([Arming and failsafe](#arming-and-failsafe)), are the exception.

The `Neutral` command, which the manager runs if the module exits for any reason, sends
ArduSub neutral manual control and a disarm, signed. If the module dies, ArduSub also
stops receiving manual control, and its own pilot-input failsafe acts.

### Flight modes

What a `MANUAL_CONTROL` axis means depends on ArduSub's flight mode: direct thrust in
MANUAL, stabilized in STABILIZE, a climb rate on the vertical axis in DEPTH HOLD. The
module therefore sets ArduSub's mode from the mux's, by a table in its configuration:

| Mux mode | ArduSub mode | |
|---|---|---|
| `manual` | the mode the pilot chose in QGroundControl: MANUAL, STABILIZE or DEPTH HOLD | the pilot flies as today |
| `assisted`, `auto` | MANUAL | the wrench means thrust; hangar's controllers stabilize |
| `policy:<name>`, `residual:<name>` | the running model's `autopilot_mode`, from `policy.<name>.active` | the mode the model's labels were collected in ([09-learning](09-learning.md)) |

A failsafe `surface` or `hold` overrides the table while it runs
([Arming and failsafe](#arming-and-failsafe)).

`ardusub.state` carries ArduSub's mode, and the recorder keeps it, so a dataset can
keep only the rows collected in one mode and a weights file can record which
([09-learning](09-learning.md)).

**Mode requests.** QGroundControl's mode requests, and buttons mapped to a mode, reach
the module as `mavlink.set_mode` from the topside. The module passes a request to
ArduSub only while the mux is in `manual`, `safety.state` is not `failsafe`, and the
mode is MANUAL, STABILIZE or DEPTH HOLD. Otherwise it refuses, with the reason
(`mux in assisted mode`, `mode GUIDED not allowed`). The call answers at once; the mode
ArduSub actually took is in `ardusub.state`.

**To verify before use**: the exact meaning of each axis in each ArduSub mode, against
ArduSub's source for the version deployed. The table above is the design's intent; the
mapping is configuration, and is corrected to what the source says.

### Buttons

ArduSub gives each of its 32 buttons two functions: `BTNn_FUNCTION`, and
`BTNn_SFUNCTION`, used while the button whose function is `shift` is held. Some of
those functions arm, disarm or change mode; others change thrust outside the mux
(`input_hold_set` latches the current input as a trim that ArduSub adds to every later
command, so neutral input no longer stops the thrusters). Buttons are therefore
handled by an allow-list, per button and shift state:

| Function | Handling |
|---|---|
| lights, mount and camera tilt, relays, camera | **allowed**: passed in `pilot.buttons` |
| arm, disarm, arm toggle, mode changes | **call**: `safety.arm`, `safety.disarm` or `mavlink.set_mode`, made by the topside module |
| everything else: `input_hold_set`, all trims, gains, `roll_pitch_toggle`, `shift` itself, scripts | **masked** |

The vehicle module reads `BTNn_FUNCTION` and `BTNn_SFUNCTION` for all 32 buttons at
start and on every `PARAM_VALUE` of a `BTN` parameter, and publishes the table as
`ardusub.buttons`, an event topic mirrored up:

```
u8   function[64]    # ArduSub's function number: 0-31 plain, 32-63 shifted
u8   handling[64]    # 0 masked, 1 allowed, 2 call
u8   shift_button    # the button whose function is shift; 255 if none
u8   reserved[7]
```

The topside module applies it to QGroundControl's `MANUAL_CONTROL` buttons
(`buttons` and `buttons2`), tracking the shift button itself. A press with an allowed
function goes into `pilot.buttons` (32 bits pressed, 32 bits shifted); a press with a
call function makes the call, once per press; anything else is dropped. The vehicle
module checks `pilot.buttons` against its own table again, drops any bit not allowed in
its shift state, and sets the shift button's bit in `MANUAL_CONTROL` only together with
a shifted allowed press, never in the same message as an unshifted one. A
`pilot.buttons` message older than `CommandTimeout` counts as no button pressed.

**Buttons across a closed gate.** While the gate is closed, the module sends
`buttons` and `buttons2` as 0, whatever `pilot.buttons` says. When the gate reopens, a
button that is still held is not sent until `pilot.buttons` has shown it released
once; only a press that starts after the reopening reaches ArduSub. ArduSub acts on a
button's change from released to pressed, and a button held through the closed gate
would otherwise reach it as a press the pilot did not make at that moment. The same
holds after `pilot.buttons` has been stale.

`BTN` parameters cannot be set through the tunnel ([The tunnel](#the-tunnel)), so the
table changes only by an administrator's change on the vehicle.

### Arming and failsafe

Arming is asynchronous: no call waits on ArduSub.

1. `safety.arm` runs the safety module's checklist and answers `accepted`.
2. The safety module publishes `safety.state` with state `arming` (`outputs_enabled`
   off) first, and only then calls `mavlink.arm` with `hg_call_start`
   ([07-safety](07-safety.md#vehicle-states)). The order matters: the `arming` message
   is the window in which ArduSub may be armed ahead of hangar, so it must exist
   before the arm can be sent.
3. `mavlink.arm` refuses at once, with the reason, unless the newest `safety.state` it
   reads says `arming`, `FS_GCS_ENABLE` and `FS_LEAK_ENABLE` are on, and the gain
   parameters are pinned. Otherwise it sends ArduSub `MAV_CMD_COMPONENT_ARM_DISARM` and
   answers `accepted`.
4. ArduSub answers with `COMMAND_ACK`, which carries only a `MAV_RESULT`, and gives
   its reasons as `STATUSTEXT` lines, before or after the ack. The module collects the
   `STATUSTEXT` lines that start `PreArm:` or `Arm:` from the moment it sends the
   command until 500 ms after the ack, and publishes the result in `ardusub.state`:
   those lines, or the `MAV_RESULT` name when there are none.
5. When `ardusub.state` reports ArduSub armed, the safety module enters `armed`. On a
   refusal, or after 2 s, it returns to `disarmed`, with ArduSub's reason in
   `safety.state`.

| hangar | the module does |
|---|---|
| `arming` | arms ArduSub on `mavlink.arm` |
| `disarmed`, failsafe `neutral` | disarms ArduSub |
| failsafe `surface` | ArduSub mode SURFACE, with neutral input |
| failsafe `hold` | ArduSub mode DEPTH HOLD with neutral input, for `hold_time`, then disarms |

In phase 1, `surface` and `hold` are ArduSub's own modes, and the mux sends neutral
(`z` 500) while they run ([07-safety](07-safety.md#phase-1-ardusub-drives-the-thrusters)).
In SURFACE, ArduSub ignores the vertical input and climbs at its own rate, so the
pilot has no heave override during a phase-1 `surface`. When ArduSub reaches the
surface it switches itself to DEPTH HOLD (reason `SURFACE_COMPLETE`); the module
expects that and does not command SURFACE again. If ArduSub refuses SURFACE or DEPTH
HOLD, the module disarms it, as for `neutral`.

**Armed agreement.** Whenever `ardusub.state` says armed while `safety.state` is
neither `armed` nor `arming`, the module disarms ArduSub and the safety module's
`autopilot` trigger fires. `arming` is the only window in which ArduSub may be armed
before hangar is; there is no other grace. The one exception is a failsafe `surface`
or `hold`, which ArduSub carries out armed (above). A `safety.state` older than the
gate's `SafetyTimeout` counts as neither. This covers a lost disarm, an arm that
arrives after `arming` has ended, and an arm from anywhere but hangar.

A disarm that hangar asks for is not a disagreement. When `safety.state` leaves
`armed`, `arming` or a failsafe `surface` or `hold`, or goes stale, the module sends
ArduSub the disarm at once, and `ardusub.state` says `disarming` (`armed` = 2) until
ArduSub answers: `disarmed` when it accepts, `armed` again when it refuses or has not
answered three sends 100 ms apart. ArduSub answers within milliseconds, so `disarming`
is not a grace: it lasts as long as ArduSub takes to answer, and ends in a
disagreement if ArduSub does not. Each `ardusub.state` message references the
`safety.state` message the module had acted on when it published it
(`Inputs=safety.state`), and the safety module judges each one against that message,
not against a newer one the vehicle's module has not yet read.

`ardusub.state`, at 10 Hz, on every change, and after the module acts on each new
`safety.state`:

```
u8   armed            # 0 disarmed, 1 armed (HEARTBEAT base_mode), 2 disarming:
                      # a disarm hangar asked for, sent and not yet answered
u8   system_status    # HEARTBEAT system_status: CRITICAL or EMERGENCY in an ArduSub failsafe
u16  reserved
u32  mode             # ArduSub's flight mode (HEARTBEAT custom_mode)
u64  last_rx_ns       # when the last message of any kind arrived from ArduSub
u32  arm_attempt      # counts mavlink.arm requests
u8   arm_result       # MAV_RESULT of the last attempt; 255 while pending
u8   reserved2[3]
char arm_reason[100]  # the PreArm:/Arm: lines, or the MAV_RESULT name
char failsafe[50]     # ArduSub's last failsafe STATUSTEXT
```

The safety module's **`autopilot`** trigger ([07-safety](07-safety.md#failsafe)) fires,
from `ardusub.state`, when:

- ArduSub disarms while hangar is `armed`;
- `ardusub.state` says armed while the `safety.state` it references is neither `armed`
  nor `arming`, outside a failsafe `surface` or `hold` (Armed agreement, above), at the
  first such message;
- no message of any kind has come from ArduSub for 3 s. ArduSub sends `HEARTBEAT` at
  1 Hz and its telemetry at up to 50 Hz, so 3 s is never ordinary jitter.

ArduSub's own failsafe reports (`system_status`, the `failsafe` text) are recorded but
fire nothing by themselves: ArduSub acts on them, and a failsafe in which it disarms
fires `autopilot` through the disarm ([07-safety](07-safety.md#failsafe)).

ArduSub's leak failsafe stays on as the backstop to the `leak` module, which reads the
probe itself ([Telemetry](#telemetry)); `FS_LEAK_ENABLE` is checked at every arm.

## The tunnel

QGroundControl's other traffic crosses the tether through a bridge tunnel
([08-bridge](08-bridge.md#tunnels)): MAVLink datagrams, authenticated like everything
else, without added reliability, as MAVLink expects of UDP (its parameter and mission
protocols retry on their own). Both modules send their tunnel traffic to the bridge's
`Local`, `127.0.0.1:14670`, and the bridge delivers what comes from the other side to
the last local address that sent to it.

The vehicle's module is an **allow-list proxy** for everything coming from the tunnel.
What it does not forward is dropped, and a dropped command is answered with
`COMMAND_ACK` `MAV_RESULT_DENIED`; the module counts drops per message in its status
line.

| From QGroundControl | Forwarded to ArduSub |
|---|---|
| `HEARTBEAT` | unchanged, so ArduSub's GCS failsafe follows QGroundControl across the tether |
| `PARAM_REQUEST_LIST`, `PARAM_REQUEST_READ` | yes |
| `PARAM_SET` | only while `safety.state` is `disarmed` and ArduSub is disarmed, and never for a protected parameter (below) |
| mission download (`MISSION_REQUEST_LIST`, `MISSION_REQUEST_INT`, `MISSION_ACK`) | yes |
| log download (`LOG_REQUEST_LIST`, `LOG_REQUEST_DATA`, `LOG_REQUEST_END`) | yes |
| `COMMAND_LONG`, `COMMAND_INT` | only `MAV_CMD_REQUEST_MESSAGE` and the camera and mount commands |
| `RC_CHANNELS_OVERRIDE`, `MANUAL_CONTROL` | no: pilot input arrives only as `pilot.wrench` |
| `MAV_CMD_DO_MOTOR_TEST`, `MAV_CMD_DO_SET_SERVO`, `SET_POSITION_TARGET_*`, `SET_ATTITUDE_TARGET`, `MAV_CMD_MISSION_START`, `MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN` | no |
| `REQUEST_DATA_STREAM`, `MAV_CMD_SET_MESSAGE_INTERVAL` | no: the module owns the message rates ([Telemetry](#telemetry)) |
| arm, disarm and mode commands (`MAV_CMD_COMPONENT_ARM_DISARM`, `MAV_CMD_DO_SET_MODE`, `SET_MODE`) | no: the topside module turns them into calls before the tunnel |
| anything else | no |

**Protected parameters**, never set through the tunnel: `FS_*`, `SYSID_MYGCS` (or
`MAV_GCS_SYSID`), `BTN*`, `JS_*`, `MOT_*`, `ARMING_*`, `MAV*_OPTIONS`,
`SERVO*_FUNCTION`. They are changed on the vehicle by an administrator.

Everything from ArduSub to QGroundControl passes.

## The topside's MAVLink module

`topside_mavlink` is the topside's end. QGroundControl keeps its default UDP
auto-connect: the module sends to `127.0.0.1:14550` from its own port,
`127.0.0.1:14650`, and QGroundControl answers there. The module:

- turns `MANUAL_CONTROL` into `pilot.wrench` (the joystick axes, normalized) and
  `pilot.buttons` ([Buttons](#buttons)), both published at 50 Hz, one `pilot.buttons`
  message with every `pilot.wrench`, on the topside, where the pilot is, and mirrored
  down together at `command` priority ([08-bridge](08-bridge.md#configuration));
- turns arm and disarm commands, as `COMMAND_LONG` or `COMMAND_INT`, into
  `safety.arm` and `safety.disarm`, and mode requests (`MAV_CMD_DO_SET_MODE`,
  `SET_MODE`) into `mavlink.set_mode`, all made with `hg_call_start`. It answers
  QGroundControl with `COMMAND_ACK`: `MAV_RESULT_ACCEPTED` when the call is accepted,
  `MAV_RESULT_DENIED` otherwise, with the call's reason in a `STATUSTEXT`. The result
  of an arm shows in ArduSub's own `HEARTBEAT`, which passes through;
- passes every other message both ways through the tunnel, where the vehicle's module
  filters it ([The tunnel](#the-tunnel)).

Its manifest declares `[Call safety.arm]`, `[Call safety.disarm]` and
`[Call mavlink.set_mode]`, all three listed in the bridge's `[Calls] Remote=`
([08-bridge](08-bridge.md#calls-across-the-tether)). On the vehicle the caller is
`topside:topside_mavlink`, which the `Allow` lists of `safety.arm`, `safety.disarm`
and `mavlink.set_mode` name.

So QGroundControl and its joystick configuration are unchanged for the pilot. hangar
accepts MAVLink only on `127.0.0.1`, on either side. QGroundControl's own auto-connect
listener is on UDP 14550 of every interface; MAVLink reaching it from the network goes
to QGroundControl alone, never to the vehicle.

## Telemetry

The vehicle's module turns ArduSub's messages into topics, by a table in its
configuration, and requests each message's rate from ArduSub
(`MAV_CMD_SET_MESSAGE_INTERVAL`):

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
| `nav.attitude` | `ATTITUDE` | |
| `nav.imu` | `SCALED_IMU` (IMU instance 0) or `RAW_IMU` | 50 Hz by default. The Navigator has one IMU; its rate is that of MAVLink streaming, not the IMU's sample rate |
| `nav.depth` | `SCALED_PRESSURE2` (the depth sensor) and the configured fluid density | |
| `power.battery` | `SYS_STATUS`, `BATTERY_STATUS` | the safety module's `battery` trigger |
| `ardusub.state` | `HEARTBEAT`, `COMMAND_ACK`, `STATUSTEXT` | the `autopilot` trigger ([Arming and failsafe](#arming-and-failsafe)) |
| `ardusub.buttons` | `PARAM_VALUE` of `BTN*` | [Buttons](#buttons) |
| `thr.out` | `SERVO_OUTPUT_RAW` | the pulse widths actually sent to the thrusters, after ArduSub's mixing: what a dataset needs as the vehicle's true action |
| `ardusub.param` | `PARAM_VALUE` after a change | event topic: the autopilot's parameter changes, kept with the data |

**Rates are the module's.** ArduSub lets any ground station on a channel reset that
channel's message rates (`REQUEST_DATA_STREAM`, which QGroundControl sends on
connecting by default). The module drops those requests from the tunnel, sets the
`NOSTREAMOVERRIDE` bit of the channel's `MAVn_OPTIONS`, and requests every rate again
after each ArduSub restart (a new instance in `hangar.vehicle.modules`, or
`time_boot_ms` going backwards).

**Leak.** ArduSub reports a leak only as a `STATUSTEXT` every 20 s and a `HEARTBEAT`
`system_status` shared with every other failsafe, so a rated leak topic cannot come
from it. `vehicle.leak` is published at 10 Hz by the `leak` module, which reads the
probe through a GPIO character device granted to it alone, in a group of its own,
never one of the `ardusub` module's exclusive groups
([ArduSub as a module](#ardusub-as-a-module), [07-safety](07-safety.md#failsafe)).
ArduSub's own leak failsafe stays on behind it. How the probe reaches both (a probe
line on a GPIO chip apart from the Navigator's, or a second probe) is confirmed on the
vehicle's hardware.

**Time.** ArduSub stamps its messages with its own boot clock (`time_boot_ms`,
`time_usec`), which starts at 0 with each ArduSub process. The module estimates the
offset to hangar's monotonic clock as the bridge does across the tether
([08-bridge](08-bridge.md#clocks)): over a sliding window of 10 s, it keeps the offset
of the message that arrived soonest after its stamp. It discards the estimate and
starts over when `time_boot_ms` goes backwards or the `ardusub` module's instance
changes in `hangar.vehicle.modules`, so stamps never jump back after an ArduSub
restart. It stamps each topic message (`stamp_ns`) in hangar's time. A message with no
time field (`HEARTBEAT`, `SYS_STATUS`, `BATTERY_STATUS`) is stamped with its arrival
time.

**Raw MAVLink.** The module also writes every message exchanged with ArduSub, both
ways, as a `.tlog` into the current recorder session (following `recorder.session`;
the session directory is group `hangar-rec`, mode 2770), readable by QGroundControl
and pymavlink. The topics are what hangar uses; the log is everything.

## Testing with ArduSub SITL

ArduSub runs as SITL (software in the loop) on x86-64, so the whole path is tested in
CI without a vehicle: SITL as the `ardusub` module, both MAVLink modules, the bridge
between two network namespaces, and a scripted ground station (pymavlink) in place of
QGroundControl.

- Joystick input reaches ArduSub only through the mux; with the gate closed, ArduSub
  receives neutral. A non-finite command axis gives a neutral message.
- Gate closed for longer than `CommandTimeout` while armed in STABILIZE or DEPTH HOLD:
  ArduSub disarmed. In MANUAL: neutral, still armed.
- Arming: `safety.arm` answers `accepted`; `safety.state` `arming` is published before
  `mavlink.arm` is called; ArduSub armed, then `safety.state` armed;
  a pre-arm refusal comes back in `safety.state` with ArduSub's `PreArm:` text; no
  answer within 2 s returns to `disarmed`. `mavlink.arm` refused with `FS_GCS_ENABLE`
  or `FS_LEAK_ENABLE` off, or the gain not pinned, and refused while `safety.state` is
  not `arming`. The module's 50 Hz output keeps its rate throughout.
- Agreement: ArduSub disarming itself puts hangar in failsafe; ArduSub armed behind
  hangar's back (an arm sent to SITL directly), or arming after `arming` has timed
  out, is disarmed, and the `autopilot` trigger fires at the first `ardusub.state`
  that says so; an ordinary `safety.disarm`, and a failsafe `neutral`, pass through
  `disarming` and fire nothing; a disarm refused or unanswered fires `autopilot`.
- Hardware: a module other than `ardusub` in `nav-spi`, `nav-i2c` or `nav-gpio`
  refused by the manager.
- Silence: SITL stopped; the `autopilot` trigger fires after 3 s, and not before.
- Signing: an unsigned or wrongly signed `MANUAL_CONTROL`, arm or
  `RC_CHANNELS_OVERRIDE` from another local process, sent to ArduSub's port: rejected.
- Buttons: for every function, plain and shifted, on buttons 0 to 31: allowed ones
  pass, arm, disarm and mode ones become calls, the rest (`input_hold_set`, trims,
  gains, `shift`) never reach SITL. With the gate closed, buttons are 0; a button held
  while the gate closes and reopens never reaches SITL until it is released and
  pressed again; likewise across a stale `pilot.buttons`.
- Tunnel: each message in the "no" rows of the allow-list sent by the ground station
  never reaches SITL; `PARAM_SET` of a protected parameter refused, of another one
  accepted only while disarmed; parameter, mission and log download work.
- Mode requests passed in `manual`, refused otherwise and for any mode but MANUAL,
  STABILIZE and DEPTH HOLD; outside a failsafe `surface` or `hold`, ArduSub's mode
  follows the mux, and in `policy:<name>` the running model's `autopilot_mode`.
- Phase-1 failsafes: `surface` puts SITL in SURFACE, and its switch to DEPTH HOLD at
  the surface is left alone; `hold` holds depth for `hold_time`, then disarms; SURFACE
  refused by SITL: ArduSub disarmed.
- Rates: with the ground station requesting streams on connect, every telemetry topic
  keeps its declared rate; after SITL restarts, rates are requested again.
- Gain: `JS_GAIN_*` and `JS_THR_GAIN` set to 1 at start.
- Killing the vehicle's `mavlink` module: its `Neutral` command disarms ArduSub.
  Killing ArduSub: the `ardusub` module's `Neutral` command runs.
- Tether cut: hangar's `tether` failsafe; the module stops `MANUAL_CONTROL` at
  `tether_timeout`; ArduSub's GCS failsafe acts behind it once the ground station's
  heartbeats and the manual control have stopped.
- Time: stamps against SITL's known clock; after an ArduSub restart, stamps continue
  forward.
