# Simulation

The whole system on one computer with nothing real attached: two managers, the bridge
between them over a virtual tether, every production module, and three simulators that
stand in for the vehicle, the camera and the pilot. It is a profile, not a build: the
binaries are the ones that go on the vehicle.

## The rules

- **Production binaries only.** A simulator is an ordinary module; nothing in a
  production module knows whether its inputs are simulated.
- **Two machines on one.** The vehicle and the topside each run their own manager, in
  their own network namespace, joined by a virtual tether whose loss and delay are set
  from the command line.
- **Deterministic when asked.** A seeded simulation and a replayed pilot give the same
  topics twice.
- **Faults are scripted**, so the safety bench tests run unattended.

## Running it

```sh
hangar sim up [--loss 1% --rtt 20ms --rate 10Mbit] [--seed 7] [--pilot <session>|joystick|script.ini]
hangar sim down
```

`sim up` creates two network namespaces, `hg-vehicle` and `hg-topside`, a veth pair
between them with `netem` for loss, delay and rate, and starts one `hangard` in each with
`Role=` set and the `sim` profile's manifests, under separate topic directories
(`/dev/shm/hangar-sim-vehicle`, `-topside`) and state directories, so a simulation can
run beside an installed system. Everything else is the normal start-up: discovery over
the veth, the handshake, mirrors, the recorder. `hangar top --sim vehicle|topside`
attaches to either side.

## The simulators

### `sim_vehicle`

A rigid-body model of the vehicle, stepped at 200 Hz:

- six degrees of freedom; mass and inertia, added mass, linear and quadratic drag,
  buoyancy and the centre of buoyancy above the centre of mass (which gives the
  restoring moment);
- thrusters at the BlueROV2's positions and angles, each a first-order lag on commanded
  thrust and a thrust curve with a deadband;
- the sea floor and the surface as limits; a current, constant or slowly varying.

It publishes the **truth topics**, `sim.pose` (position and attitude), `sim.velocity`
(linear and angular) and `sim.thrust` (each thruster's thrust), at 200 Hz and without
noise: what a test asserts against, and what `sim_camera` warps footage by. One writer
per topic holds in the simulation as in production: a topic a production module
publishes is published by that module, and `sim_vehicle` never stands in for it.

**With ArduSub SITL in the loop** (phase 1), the `sim` profile's `ardusub` module is
SITL. `sim_vehicle` feeds SITL its sensor inputs (IMU, pressure, attitude, each with
configurable noise, bias and latency) over SITL's JSON interface and takes SITL's servo
outputs as the thruster command, so the `mavlink` module is tested against the real
autopilot code. `nav.*`, `power.battery` and `thr.out` are then published, as in
production, by the vehicle's `mavlink` module from SITL's telemetry
([10-mavlink](10-mavlink.md#telemetry)), and `vehicle.leak` by the `leak` module. In
the `sim` profile the `leak` module's `Exec` line names a line of a `gpio-sim` chip
(the kernel's GPIO simulator, which `sim up` sets up through configfs) instead of the
real probe, and `sim_faults` raises the leak by writing that line's `pull` attribute in
sysfs; the `leak` module reads a GPIO character device either way and does not know the
difference.

**Without ArduSub** (phase 2), `sim_vehicle` subscribes to `thr.cmd` and publishes
`nav.imu` (200 Hz), `nav.attitude` and `nav.depth` (50 Hz) and `power.battery` (1 Hz,
draining with thrust) itself, with the same noise, bias and latency parameters, in
place of the sensor modules that read the Navigator's buses.

Its parameters are live ([05-parameters](05-parameters.md)): mass, buoyancy, drag
coefficients, payload mass and position, current, and each sensor's noise, bias and
latency. Changing buoyancy or payload during a run is how the adaptive depth-hold
experiment of [09-learning](09-learning.md#example-adaptive-depth-hold) is driven.

### `sim_camera`

Publishes `cam.raw` and `cam.nn` from recorded footage, warped by `sim.pose`.
A **footage set** is a recorded session's video (the MP4 and `video.index`,
[06-recorder](06-recorder.md)) with the pose that was recorded beside it. For each
simulated frame the module takes the recorded frame nearest in pose, applies a
homography for the difference in attitude and a scale for the difference in altitude
over the floor, and the paper's water model for the difference in depth (attenuation
and veiling light per metre, [11-electric-eye](11-electric-eye.md)). It is not a
renderer: it produces plausible pictures that move with the vehicle, enough for the
vision chain, the dataset path and a policy's small frames, not for evaluating
perception. Without a footage set it publishes a synthetic texture.

### `sim_pilot`

Publishes `pilot.wrench` and `pilot.buttons` on the topside side from one of: a
recorded session (replayed by stamp, so a recorded dive can be re-driven through a
changed system); a script (`script.ini`: timed segments of axis values, for repeatable
manoeuvres); or a real joystick through `topside_mavlink` as in production. In the
third case the pilot flies the simulated vehicle from QGroundControl's video window,
fed by the simulated camera through the normal encoder and relay.

### `sim_faults`

A module that does what the bench tests of [07-safety](07-safety.md) need, on a
schedule from `faults.ini`: kill a module (through the manager's control socket, as
`hangar-admin`), stall a topic (ask `sim_vehicle` to freeze a sensor it feeds SITL, or,
without ArduSub, to stop publishing one), set the tether's loss to 100% for a time
(`netem`), raise the leak (the simulated GPIO line, [above](#sim_vehicle)), drain the
battery (in phase 1 SITL's own battery model, started with a small capacity so that it
runs down on schedule; in phase 2 a parameter of `sim_vehicle`), drive the vehicle past
the depth band, hang a core. Each fault and its time is published on
`sim.faults`, so the recording of a run says what was injected when, and a test asserts
the system's response against it.

## Determinism and replay

`--seed` seeds every simulator's noise. With `--pilot <session>` the pilot input is a
recording, so a run is a function of the seed, the session and the release: two runs
give the same topics, which the live-against-offline dataset test and the equivalence
tests of ported operators rely on. The recorder runs in the simulation as in production,
so a simulated dive is a session like any other and can itself be replayed.

## What the simulation is for

| Need | How |
|---|---|
| Developing a module | `hangar sim up`, edit, `hangar restart <module>` on the right side |
| The learning loop end to end | pairer, bridge, trainer, deploy, residual mode, all running; buoyancy changed live to make the controller adapt |
| Safety bench tests in CI | `sim_faults` with the bench schedule; assertions on time to neutral from the recording |
| Transport under a bad tether | `--loss`, `--rtt`, `--rate` |
| Upgrade over the tether | [13-deployment](13-deployment.md) tested between the two namespaces |
| Teaching a new pilot the console | joystick in, simulated vehicle out |

What it is not for: validating hydrodynamics, perception, or anything about the real
Navigator's hardware; those are the bench and wet tests of [07-safety](07-safety.md).

## Tests

- Two namespaces up and down cleanly beside an installed system; discovery over the veth.
- `sim_vehicle` against hand-computed cases: neutral buoyancy holds depth; a known thrust
  reaches the expected terminal velocity; restoring moment rights the vehicle.
- One writer per topic: with SITL in the loop, `nav.*`, `power.battery` and `thr.out`
  are written by `mavlink`, `vehicle.leak` by `leak`, and `sim_vehicle` writes `sim.*`
  alone; a leak raised on the simulated GPIO line reaches `vehicle.leak` and fires the
  `leak` trigger.
- Same seed, same session, same topics twice.
- Every bench test of 07 driven by `sim_faults` and asserted from the recording.
- `sim_camera` frames carry the pose-nearest recorded frame's sequence number in their
  provenance, so a dataset built in simulation can be traced to the footage it came from.
