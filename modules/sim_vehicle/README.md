# sim_vehicle: the vehicle simulator

`hangar-sim-vehicle`, the module of docs/14-simulation.md "sim_vehicle": lib/sim's
6-DOF model (lib/sim/README.md) stepped at 200 Hz, with its thrusters, a battery that
drains with thrust, and its sensors, under one set of live parameters. It publishes the
**truth topics** and, depending on the mode, either is ArduSub SITL's physics or stands
in for ArduSub and the sensors altogether. C11 over lib/hg and lib/sim; static for musl.

## Layout

| File | |
|---|---|
| `sv.h`, `sv.c` | the core: model, thrusters, battery, sensors, SITL's frames. No I/O: the tests drive it |
| `sim_vehicle.c` | the module: parameters, topics, the loop, SITL's socket |
| `sim_vehicle.module` | the template for SITL mode (phase 1) |
| `../../profiles/sim-vehicle/sim_vehicle.module.off` | standalone mode, opt-in (phase 2) |
| `tests/test_sim_vehicle.c` | the suite (`make check-sim_vehicle`, about 17 s) |
| `msg/sim_pose.msg`, `sim_velocity.msg`, `sim_thrust_truth.msg` | the truth types |
| `msg/thruster_cmd.msg` | `thr.cmd`, phase 2's thruster command (docs/02's `thruster_cmd`) |
| `msg/sim_vehicle_params.msg` | the parameters |

## Two modes

```
hangar-sim-vehicle [--seed N] --sitl [host:]port    # ArduSub SITL's physics (sim_vehicle.module)
hangar-sim-vehicle [--seed N]                       # without ArduSub (sim_vehicle.module.off)
```

**SITL's physics (phase 1).** ArduSub SITL started with its JSON model sends its servo
outputs to the module's UDP port (9002 by default); the module steps once per packet
with `dt = 1 / frame_rate` and answers the sender. Servo outputs 1 to 8 are the
thrusters in ArduSub's numbering, which lib/sim's geometry uses (lib/sim/README.md,
"thruster directions"). A packet whose frame count repeats the last one gets the same
answer and no step (SITL resends when an answer is late); a frame count that goes back
means SITL restarted, and the vehicle starts again at rest. `nav.*`, `power.battery` and
`thr.out` are then the `mavlink` module's, from SITL's telemetry: the simulator never
writes them in this mode.

**Standalone (phase 2).** A 200 Hz timer: `thr.cmd` is read at each tick (the last
fresh command held between its messages; stale, absent or flagged stale, the thrusters
stop), and the module publishes `nav.imu` (200 Hz), `nav.attitude` and `nav.depth`
(50 Hz) and `power.battery` (1 Hz) itself. Simulated time keeps pace with the clock
(late ticks are stepped, up to a second). Take `mavlink.module` out of the root first:
one topic, one writer. Until a thruster module exists in the profile, whatever drives
the experiment publishes `thr.cmd`.

Either way the loop is one epoll over the timer, SITL's socket and the call topics'
eventfd (`set_params`): nothing is waited on that is not read (hangar#1).

## Topics

| Topic | Type | Rate | |
|---|---|---|---|
| `sim.pose` | `sim_pose` | 200 Hz | simulated time, position (world NED from the start), quaternion, roll, pitch, yaw: truth, no noise |
| `sim.velocity` | `sim_velocity` | 200 Hz | body velocity, world velocity, body acceleration |
| `sim.thrust` | `sim_thrust_truth` | 200 Hz | the pulse widths commanded, each thruster's thrust after the table and its lag, the body wrench, the supply voltage |
| `nav.imu` | `imu` | 200 Hz | standalone only: gyro and accelerometer with their noise, bias and latency |
| `nav.attitude` | `attitude` | 50 Hz | standalone only: attitude with its noise, bias and latency; rates from the gyro |
| `nav.depth` | `depth` | 50 Hz | standalone only: depth with its noise, bias and latency, and the pressure it means |
| `power.battery` | `battery` | 1 Hz | standalone only |

In SITL mode the truth topics are published at 200 Hz of simulated time, whatever
SITL's frame rate.

## Parameters

`msg/sim_vehicle_params.msg`, all live (`hangar param set sim_vehicle buoyancy=97.8`):
mass, buoyancy, linear and quadratic damping per axis, a payload's mass and position
(through `sim_params_add_payload`), the current, the floor, the thrusters' lag, the
battery (capacity, full and empty voltage, amps per newton of thrust, idle current), and
each sensor's noise, bias and latency. The defaults are lib/sim's BlueROV2 heavy
configuration and its typical sensors (the suite checks they are). The weight is
`mass × 9.81`, so the default net buoyancy is 1.99 N (lib/sim's own 112.8 N weight
gives 2.0 N). Changing buoyancy or payload during a run is how docs/09's adaptive
depth-hold experiment is driven.

## ArduPilot's JSON interface

Read from ArduPilot's `libraries/SITL/SIM_JSON.h` and `SIM_JSON.cpp` at the tag
`Sub-4.7.2`, the commit `tools/sim/Dockerfile` pins:

- SITL → physics, UDP to port 9002: `uint16 magic = 18458, uint16 frame_rate,
  uint32 frame_count, uint16 pwm[16]` (40 bytes, little-endian), or `magic = 29569` and
  32 channels (72 bytes);
- physics → SITL, to the address the packet came from: one JSON object, newline
  before and after (SITL turns newlines into NULs and parses the text between the last
  two). Required: `timestamp` (s), `imu.gyro` (rad/s, body), `imu.accel_body` (m/s²,
  specific force, body: `(0, 0, −g)` at rest and level), `velocity` (m/s, NED), and
  `attitude` or `quaternion`; the module also sends `position` (m, NED from the origin,
  which SITL turns into altitude and the barometer) and `battery.voltage`/`current`.

**Running it against the real SITL**, the one check not made here: start SITL with its
JSON model sending to this machine, e.g. in `tools/sim`'s container
`sim_vehicle.py -v ArduSub --model JSON:127.0.0.1 ...` (the exact options are
unverified), then `make check-sim_vehicle HANGAR_SITL=1`: the suite starts the
module on UDP 9002 under its manager, waits up to 60 s for SITL's frames, and checks
`sim.pose` runs. It checks the frames flow, not that ArduSub arms or holds depth.

## Tests

`make check-sim_vehicle`, about 17 s:

- **the physics:**
  - neutral buoyancy holds 5 m to 0.1 mm over 10 s;
  - the four verticals at 6 N each reach the terminal heave speed the damping gives (0.7388 against 0.7387 m/s), with their moments cancelling;
  - a 20° roll rights itself (−0.02° after 30 s);
  - energy decays with no thrust, never rising in 2000 steps;
  - the wrench of 500 random commands matches `sim_applied_wrench` to 1e-6 (the depth-hold label's conversion);
  - the same seed gives bit-identical readings and state, another seed different ones
    (the model and its sensors, stepped by the suite; a run under a manager is not a
    function of the seed: docs/14 "Determinism and replay");
  - 5 N less buoyancy sinks the vehicle, live, and so does a payload;
  - the battery drains with thrust;
  - a step costs 0.6 µs (8000× real time at 200 Hz, x86-64 `-O2`);
- **SITL's frames:** 16 and 32 channels, refused datagrams, the reply's framing and keys;
- **standalone, under a manager,** with the profile's shipped manifest:
  - `sim.pose` and `nav.imu` at 201 Hz, `nav.depth` at 51 Hz;
  - `thr.cmd` from a writer takes the vehicle down, and `sim.thrust` shows the commanded pulse widths;
  - the thrusters stop when `thr.cmd` goes stale;
  - `hangar param set` changes the buoyancy live, same pid;
  - CPU 0.0–0.5 % of a core at 200 Hz;
- **SITL mode, under a manager,** with the template, against a fake SITL in lockstep:
  - 400 frames at 400 Hz give a timestamp of exactly 1 s;
  - a resent frame gets the same answer;
  - thrust down through servos 5–8 dives the vehicle, and `sim.pose` follows;
  - a frame count from 1 again restarts the vehicle;
  - idle CPU 0.0 %;
- **`HANGAR_SITL=1`:** the real container, as above.

## Deviations from docs/14, and why

1. **Pressure, depth and attitude noise in SITL mode are SITL's own.** docs/14 has the
   simulator feed SITL its IMU, pressure and attitude with configurable noise, bias and
   latency. The JSON interface has no pressure or sensor-attitude field: SITL derives
   its barometer from the position and its attitude from the quaternion, and adds its
   own noise (its `SIM_BARO_RND` and the like). Only the IMU channels' noise, bias and
   latency apply to what SITL gets; the depth and attitude channels apply in standalone
   mode.
2. **The mode is a command-line option, with a manifest each.** A module's outputs are
   fixed by its manifest, and the two modes publish different topics.
3. **The battery is a simple model:** voltage linear in the charge left between the
   full and empty values, current `idle + amps_per_newton × Σ|thrust|` (0.5 A/N is an
   estimate from the T200's published curves). It feeds the thrust table's voltage, as
   docs/14 asks.
4. **`thr.cmd` is held between its messages while fresh** (by its topic's
   `StaleAfter`), since its writer's rate is below the simulator's.
5. **A restarted SITL resets the vehicle** to rest at the surface (the frame count going
   back), and the noise generator carries on rather than starting from the seed again.
6. **`sim_thrust_truth`, not `sim_thrust`, names the type of `sim.thrust`:**
   `sim_thrust.h` and `struct sim_thrusters` are lib/sim's.

## Left out, and found

- `sim_camera`, `sim_pilot`, `sim_faults`, `hangar sim up` and `hangar sim fit` are other
  parts of docs/14, not this module.
- **lib/sim's geometry pitches the vehicle hard under surge:** the horizontal thrusters
  sit 6 cm off the centre of gravity, so 22 N of surge makes about 1.9 N m of pitch
  moment, about what the 2 cm centre-of-buoyancy offset can hold (2.3 N m), and the
  vehicle tumbles instead of settling. A pure 20 N surge force also pitches it (the Munk
  moment of the unequal added masses), so forward speed is not surge damping's alone.
  Both are the model's physics as identified; whether the real vehicle does the first is
  for `hangar sim fit` against a recorded dive.
