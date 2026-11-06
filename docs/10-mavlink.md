# MAVLink and ArduSub

While ArduSub drives the thrusters, hangar sits between it and the pilot's
QGroundControl. The pilot keeps QGroundControl, its joystick setup and its views;
hangar's mux, safety module and recorder see, decide and record everything that
reaches the autopilot. ArduSub's Navigator Linux build runs as a hangar module;
BlueOS is not used.

## The rules

- **Nothing talks to ArduSub but hangar.** The vehicle's `mavlink` module owns
  ArduSub's MAVLink endpoint; QGroundControl reaches ArduSub only through it.
- **The pilot's commands go through the mux**, and arming through the safety module.
  QGroundControl's joystick input becomes `pilot.wrench`; its arm and disarm requests
  become `safety.arm` and `safety.disarm`.
- **The control path is authenticated end to end.** QGroundControl connects to
  `localhost` on the topside; MAVLink crosses the tether only inside the bridge.
- **hangar and ArduSub never disagree about being armed.** Each follows the other:
  hangar arms ArduSub, and ArduSub disarming itself is a failsafe in hangar.
- **ArduSub's own failsafes stay on**, behind hangar's ([07-safety](07-safety.md#phase-1-ardusub-drives-the-thrusters)).
- **Whatever the autopilot reports is a topic**, stamped in hangar's time, and its raw
  MAVLink is recorded with every session.

## The path

```
 topside                                   vehicle
 ───────                                   ───────
 QGroundControl                            ArduSub (module `ardusub`)
   │ udp localhost:14550                     ▲ udp localhost:14660
   ▼                                         │
 mavlink (topside) ─ pilot.wrench ═══ bridge ═══► mux ─ ctl.wrench ─► mavlink (vehicle)
   │                 pilot.buttons ══ bridge ═══════════════════════►   │
   │                 safety.* calls ═ bridge ═► safety ─ mavlink.arm ─►   │
   └──── other MAVLink ════ bridge tunnel ═══════════════════════════►   │
   ◄──── telemetry ════════ bridge tunnel ═══════════════════════════════┘
```

## ArduSub as a module

```ini
# /etc/hangar/modules/ardusub.module
[Module]
Description=ArduSub autopilot (Navigator)
Exec=/usr/lib/hangar/ardusub <serial and board options: a UDP endpoint on localhost:14660>
User=hg-ardusub
SupplementaryGroups=spi i2c gpio
Capabilities=CAP_SYS_NICE

[Restart]
Policy=always
OnGiveUp=failsafe

[Schedule]
LockMemory=yes
```

ArduSub publishes no topics, so its health is judged by the `mavlink` module through
its heartbeats ([Failsafe](#arming-and-failsafe)). ArduPilot's Linux builds set the
priorities of their own threads, so the module is granted `CAP_SYS_NICE`
([02-manifest](02-manifest.md#module-required-once)); exactly which devices,
capabilities and options the deployed ArduSub build needs is to be confirmed on the
vehicle, and the template written from that.

ArduSub accepts pilot input only from its ground station's system id
(`SYSID_MYGCS`). The vehicle's `mavlink` module presents itself with that id; since
nothing else talks to ArduSub, there is no ambiguity.

## The vehicle's `mavlink` module

```ini
# /etc/hangar/modules/mavlink.module   (vehicle)
[Module]
Exec=/usr/lib/hangar/hangar-mavlink /etc/hangar/mavlink.conf
User=hg-mavlink

[Subscribe ctl.wrench]
[Subscribe pilot.buttons]
[Subscribe mux.state]
[Subscribe safety.state]

[Publish ardusub.manual_control]
Type=manual_control
Rate=50
Actuator=yes
Inputs=ctl.wrench

[Actuator]
Command=ctl.wrench
Neutral=/usr/lib/hangar/hangar-mavlink --neutral /etc/hangar/mavlink.conf

[Serve arm]
Request=arm_req
Reply=arm_rep
Allow=safety
Deadline=1s
```

plus the telemetry topics of its configuration ([Telemetry](#telemetry)).

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
| buttons | `buttons`, from `pilot.buttons`, masked ([Buttons](#buttons)) | 0 |

The module waits on its UDP socket and its topics together through `hg_wait_fd`
([01-topics](01-topics.md#reading)). Its `Neutral` command, which the manager runs if
the module exits for any reason, sends ArduSub neutral manual control and a disarm.

In phase 1, `wrench` holds normalized axis commands, fractions of full command from −1
to 1, and the mux's envelope is in the same units. If this module dies, ArduSub stops
receiving manual control and its own pilot-input failsafe acts.

### Flight modes

What a `MANUAL_CONTROL` axis means depends on ArduSub's flight mode: direct thrust in
MANUAL, stabilized in STABILIZE, a climb rate on the vertical axis in DEPTH HOLD. The
module therefore sets ArduSub's mode from the mux's, by a table in its configuration:

| Mux mode | ArduSub mode | |
|---|---|---|
| `manual` | the mode the pilot chose in QGroundControl (MANUAL, STABILIZE, DEPTH HOLD) | the pilot flies as today |
| `assisted`, `auto`, `policy:<name>`, `residual:<name>` | MANUAL | the wrench means thrust; hangar's controllers stabilize |

QGroundControl's mode requests are passed to ArduSub while the mux is in `manual`, and
refused otherwise, with a status text saying why (`mux in assisted mode`).

**To verify before use**: the exact meaning of each axis in each ArduSub mode, against
ArduSub's source for the version deployed. The table above is the design's intent; the
mapping is configuration, and is corrected to what the source says.

### Buttons

ArduSub maps joystick buttons to functions (its `BTNn_FUNCTION` parameters), and some
of those arm, disarm or change mode. The module reads those parameters at start and
whenever they change, and **masks** every button whose function is arming, disarming or
a mode change: such a press becomes the corresponding call (`safety.arm`,
`safety.disarm`, a mode request) instead of reaching ArduSub. Every other button
(lights, camera tilt, gain) passes through.

### Arming and failsafe

| hangar | the module does |
|---|---|
| `safety.arm` | `safety` calls `mavlink.arm`; the module arms ArduSub and answers with ArduSub's result. A refusal (a pre-arm check failed) carries ArduSub's own reason, and `safety` stays disarmed |
| `disarm`, failsafe `neutral` | disarm ArduSub |
| failsafe `surface` | ArduSub mode SURFACE |
| failsafe `hold` | ArduSub mode DEPTH HOLD with neutral input, for `hold_time`, then disarm |

The other way, the module publishes `ardusub.state` (armed, mode, ArduSub's failsafe
flags, last heartbeat) and the safety module's **`autopilot`** trigger fires
([07-safety](07-safety.md#failsafe)) when ArduSub disarms while hangar is armed, when
ArduSub reports a failsafe of its own, or when its heartbeat is missing for 1 s.

## The topside's `mavlink` module

QGroundControl is pointed at `localhost:14550`, where the topside instance listens. It:

- turns `MANUAL_CONTROL` into `pilot.wrench` (the joystick axes, normalized) and
  `pilot.buttons`, published on the topside, where the pilot is, and mirrored down at
  `command` priority ([08-bridge](08-bridge.md#delivery));
- turns arm and disarm commands into `safety.arm` and `safety.disarm`, and mode
  requests into a call to the vehicle's `mavlink` module, answering QGroundControl with
  the result (`COMMAND_ACK`);
- passes every other message both ways through a bridge tunnel.

So QGroundControl and its joystick configuration are unchanged for the pilot, and no
MAVLink is accepted from the network on either side.

The **tunnel** is a datagram channel of the bridge ([08-bridge](08-bridge.md#tunnels)):
MAVLink datagrams, authenticated like everything else, without added reliability, as
MAVLink expects of UDP (its parameter and mission protocols retry on their own).

## Telemetry

The vehicle's module turns ArduSub's messages into topics, by a table in its
configuration, and requests each message's rate from ArduSub (`SET_MESSAGE_INTERVAL`):

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
| `nav.imu` | `SCALED_IMU2` | |
| `nav.depth` | `SCALED_PRESSURE2` (the depth sensor) and the configured fluid density | |
| `power.battery` | `SYS_STATUS`, `BATTERY_STATUS` | the safety module's `battery` trigger |
| `vehicle.leak` | ArduSub's leak report | the `leak` trigger |
| `ardusub.state` | `HEARTBEAT`, failsafe status | the `autopilot` trigger |
| `thr.out` | `SERVO_OUTPUT_RAW` | the pulse widths actually sent to the thrusters, after ArduSub's mixing: what a dataset needs as the vehicle's true action |
| `ardusub.param` | `PARAM_VALUE` after a change | event topic: the autopilot's parameter changes, kept with the data |

**Time.** ArduSub stamps its messages with its own boot clock. The module estimates the
offset to hangar's monotonic clock as the bridge does across the tether, keeping the
offset of the messages that arrived soonest after their stamp, and stamps each topic
message (`stamp_ns`) in hangar's time.

**Raw MAVLink.** The module also writes ArduSub's complete MAVLink stream as a `.tlog`
into the current recorder session (following `recorder.session`), readable by
QGroundControl and pymavlink. The topics are what hangar uses; the log is everything.

## Testing with ArduSub SITL

ArduSub runs as SITL (software in the loop) on x86-64, so the whole path is tested in
CI without a vehicle: SITL as the `ardusub` module, both `mavlink` modules, the bridge
between two network namespaces, and a scripted ground station (pymavlink) in place of
QGroundControl.

- Joystick input reaches ArduSub only through the mux; with the gate closed, ArduSub
  receives neutral.
- Arming: hangar arms ArduSub; a pre-arm refusal comes back with ArduSub's reason;
  ArduSub disarming itself puts hangar in failsafe.
- Masked buttons become calls; the others pass.
- QGroundControl's mode requests passed in `manual`, refused otherwise; ArduSub's mode
  follows the mux.
- Parameter and mission traffic through the tunnel; nothing accepted from the network.
- Killing the vehicle's `mavlink` module: ArduSub's pilot-input failsafe acts.
- Telemetry stamps against SITL's known clock.
