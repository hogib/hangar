# Phase 1 as built: `safety`, `mux`, `mavlink`, `topside_mavlink`

The modules that put hangar between the pilot's QGroundControl and ArduSub, with ArduSub
held in MANUAL and driving the thrusters (`docs/07-safety.md`, `docs/10-mavlink.md`).
They are developed and tested against the Gazebo + ArduSub SITL container of
`tools/sim/` (`SETUP.md`, "Where hangar goes"), on one machine, with both hangar roles
run by two managers under two `$HANGAR_ROOT`s on the host.

## Ports on the development machine

| Who | Listens | Sends to |
|---|---|---|
| ArduSub SITL (container, `--net=host`) | MAVProxy's `--out 127.0.0.1:14550`; SITL's own MAVLink on TCP 5760 and UDP 14550 through MAVProxy | |
| `mavlink` (vehicle role, host) | a UDP port of its own, `14560` | SITL through MAVProxy's second output: `launch.sh` adds `--out 127.0.0.1:14560` when `HANGAR_SIM=1`, so the vehicle module and QGC can both be served while developing; in production ArduSub has one endpoint, the module's |
| `topside_mavlink` (topside role, host) | UDP `14550` for QGroundControl (QGC's autoconnect; SITL's `--out 14550` is then turned off in `launch.sh` under `HANGAR_SIM=1`) | QGC's address, learned from its first packet |
| bridge | not yet built: the two managers share the machine, so for now the "tether" is both roles reading each other's topic directory through a **same-host shim**: `HANGAR_PEER_TOPIC_DIR` makes `hg_sub_open` of a `<role>:`-prefixed name read the other root's topic files directly (read-only), the way mirrored topics will look once the bridge exists. Marked as a development-only shim in the library, refused when `HANGAR_CONTROL` is a real manager without the shim flag |

## Deliverables

1. `modules/safety/` (identifier `safety`): docs/07 "Vehicle states", "Failsafe", the
   `arm`/`arming`/`armed`/`disarming`/`disarmed`/`failsafe` machine with `safety.state`
   at 10 Hz, the calls `arm`, `disarm`, `estop`, `clear`, `check` with their Allow lists,
   the arming checklist (required modules ready from `hangar.<role>.modules`, inputs
   fresh), the triggers (`module`, `tether` (absent while there is no bridge: configurable
   off), `leak`, `battery`, `depth`, `autopilot`, `estop`, `input`), the actions as
   parameters (`safety_params.msg`: trigger actions, timeouts, required modules list as a
   fixed `char[8][32]`), latched failsafe in the state directory, `clear-failsafe` to the
   manager, asynchronous arming through `hg_call_start` to `mavlink.arm`.
2. `modules/mux/` (identifier `mux`): docs/07 "The mux": one writer of `ctl.wrench`,
   `Sources=` all control sources, modes `manual`, `assisted`, `residual:<name>` (v1; `policy:`
   refused as later), per-axis choice with pilot override and 1 s hold, the envelope and the
   residual envelope as parameters (`mux_params.msg`), `mux.state` on change and at 10 Hz,
   `set_mode` (Allow `topside:console admin topside:admin`), demotion rules, the stale-input
   transitivity (`hg_publish_with` naming the sources used), failsafe `surface` thrust and
   `hold` through `ctl.depth_hold` when present else neutral.
3. `modules/mavlink/` (identifier `mavlink`, the vehicle's; and the same program as
   `topside_mavlink` under a `--topside` flag or a second small program, your choice,
   justified): docs/10 in full as revised: MAVLink 2 framing and parsing written in C from
   the XML definitions (generate `modules/mavlink/mavlink_gen.h` from the pinned
   `ardupilotmega.xml`/`common.xml` of the ArduPilot tree at Sub-4.7.2 with a small Python
   generator `tools/mavgen/` of our own: only the messages docs/10 names (HEARTBEAT,
   MANUAL_CONTROL, COMMAND_LONG, COMMAND_ACK, STATUSTEXT, ATTITUDE, SCALED_IMU, SCALED_PRESSURE2,
   SYS_STATUS, BATTERY_STATUS, SERVO_OUTPUT_RAW, PARAM_SET, PARAM_VALUE, PARAM_REQUEST_READ,
   SET_MODE, MAV_CMD_SET_MESSAGE_INTERVAL, MAV_CMD_COMPONENT_ARM_DISARM, MAV_CMD_DO_SET_MODE,
   NAMED_VALUE_FLOAT), CRC extra per message, signing per the MAVLink 2 spec (key file,
   timestamp, SHA-256 in `lib/common` or the module), no pymavlink or libmavlink at run time
   (pymavlink's XML is the source, read by the generator, licence MIT noted);
   the vehicle module: SYSID_MYGCS presented, MANUAL held, `MANUAL_CONTROL` at 50 Hz from the
   actuator gate (`hg_gate` in lib/hg per docs/07: both `safety.state` fresh with
   `outputs_enabled`, and the command fresh and not stale-flagged), neutral otherwise,
   `ardusub.manual_control` published as the actuator output with `Actuator=yes` and
   `[Actuator] Command=ctl.wrench Neutral=<this program> --neutral`, telemetry → `nav.attitude`,
   `nav.imu`, `nav.depth`, `power.battery`, `thr.out`, `ardusub.state` with the clock
   estimator (reset on time_boot_ms going backwards), `arm` served for `safety` only with
   the STATUSTEXT reason collection, armed agreement both ways with the `disarming` state,
   `autopilot` trigger inputs, `hangar ardusub-setup` writing `/etc/hangar/ardusub.params`
   (JS gains pinned, BTN functions none, FS_GCS on, SYSID_MYGCS) through PARAM_SET and
   `hangar check`'s verification of them; the topside module: presents itself to QGC as the
   vehicle (HEARTBEAT, re-emitted telemetry from the mirrored `nav.*`/`ardusub.state` topics,
   STATUSTEXT), `MANUAL_CONTROL` → `pilot.wrench` (normalized −1..1, gain not applied: the
   envelope is the gain) and `pilot.buttons`, QGC's arm/disarm → `safety.arm`/`disarm` calls
   as `topside:topside_mavlink`, mode requests answered with a STATUSTEXT that hangar owns
   modes, no other MAVLink forwarded.
4. `lib/hg/hg_gate.h/.c`: the actuator gate of docs/07 as the library helper the docs name
   (`hg_gate_open`, `hg_gate_check` with reasons), tested once.
5. Message types: `safety_params.msg`, `mux_params.msg`, `pilot_buttons.msg` (u32 buttons,
   u32 buttons2… per docs/10), any missing telemetry type; regenerate with `make msg`.
6. Manifests under each module directory and a `profiles/sim-vehicle/` and
   `profiles/sim-topside/` set of enabled manifests + `hangard.conf` for the two roots, with
   a `tools/sim/hangar-up.sh` that starts both managers on the host under
   `$HANGAR_ROOT=/dev/shm/hangar-vehicle` and `/dev/shm/hangar-topside` with the shim, so the
   demo is: `tools/sim/run.sh up` (with `HANGAR_SIM=1`), `tools/sim/hangar-up.sh`, QGC on the
   host, Dawn Patrol on both roots (ports 8080 vehicle, 8081 topside).
7. Tests (suites in `check`, each under 30 s, as an ordinary user under `$HANGAR_ROOT`):
   safety's state machine and every trigger with toy inputs; the mux's per-axis choice,
   override hold, envelope, residual envelope, demotion, stale transitivity; the gate's
   three conditions and reasons; MAVLink framing/CRC/signing against known-good packets
   (take a few byte-exact vectors from pymavlink's test data or construct them with pymavlink
   in a one-off Python run during development, check them in; pymavlink is not a run-time or
   build dependency); the vehicle module against a **fake ArduSub** (a small C or Python
   program speaking just enough MAVLink: heartbeats, ACKs, telemetry, arming with a PreArm
   STATUSTEXT path) so CI needs no SITL: arming sequence incl. the `arming`/`disarming` states
   and the autopilot trigger, neutral when the gate closes, telemetry stamping; the topside
   module against a fake QGC; the whole chain in one process tree (two managers, both modules,
   fake QGC → `pilot.wrench` → mux → gate → fake ArduSub sees MANUAL_CONTROL; disarm → neutral;
   kill `safety` → neutral within SafetyTimeout + one period, measured). A separate
   `make check-sitl` target (not in `check`) runs the same chain against the real container
   when `HANGAR_SITL=1` and `tools/sim/run.sh` reports it up; document how.
8. READMEs per module with deviations; `docs/names.md` untouched (the user names programs).

## Rules

C11, `-std=c11 -Wall -Wextra -pedantic` clean under gcc, clang, zig cc; libc and the
kernel; SPDX GPL-3.0-or-later for modules, LGPL for lib/hg additions; no allocation on hot
paths; the docs' voice. One appended Makefile section per module (`modules/<m>/<m>.mk`
included with three lines each). Nothing else is being edited concurrently. Verify from
clean scratch dirs under the scratchpad: `make -j12 check` with gcc, clang, zig cc (41
suites + yours, 0 warnings); TSan/ASan on lib/hg incl. hg_gate; `make equivalence`; static
aarch64 links for all four modules and the gate. Where docs/07 or docs/10 is impossible,
contradictory, or depends on the bridge, implement the closest correct thing and list it
under deviations; never change behaviour silently. The shim and anything else that exists
only because the bridge does not yet is marked `/* until the bridge: */` in code and listed.
