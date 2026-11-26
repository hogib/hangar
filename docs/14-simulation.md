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
- **Deterministic when asked.** A seed fixes each simulator's noise, and the model
  gives the same state for the same inputs, bit for bit. A whole run under a manager is
  not yet a function of its seed ([Determinism and replay](#determinism-and-replay)).
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

A rigid-body model of the vehicle, stepped at 200 Hz, written in C as an ordinary
module with no dependencies, so that it ships in the release and runs in the vehicle's
own continuous integration ([Physics](#physics)):

- six degrees of freedom in the marine-vehicle form (Fossen): rigid-body and added
  mass, linear and quadratic damping, restoring forces from weight and buoyancy with
  the centre of buoyancy above the centre of mass;
- thrusters at the BlueROV2's positions and angles, each a first-order lag on commanded
  thrust and a tabulated thrust curve with a deadband;
- the sea floor and the surface as limits; a current. As built the module's current is
  a constant (`current[3]`, live): lib/sim also has a slow sinusoidal variation
  (`current_amp`, `current_period`), which no parameter of `sim_vehicle` sets yet.

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

#### Frames

Where the thrusters sit and which way each pushes is a file, not code: a frame file
(`lib/sim/sim_frame.h`), one `[Thruster N]` section each with its `Position`, its
`Direction` and whether it is `Reversed`. `sim_vehicle --frame FILE` builds its
allocation matrix from it, and the `thrusters` module, given the same file, inverts
that matrix ([07-safety](07-safety.md#the-actuator-gate)); a vehicle of six thrusters
is another file. Without the option both hold the compiled-in BlueROV2 heavy
configuration, which `profiles/sim-direct/bluerov2-heavy.frame` repeats as a file
(a test compares the two).

`profiles/sim-direct` is the whole chain without ArduSub: `safety`, the `mux`, `nav`,
`depth_hold` with its levelling and heading hold, `thrusters` and the simulator.
`make check-sim_direct` flies it: armed, down to 2 m and held, surge, sway, a full
turn, a disarm, the safety module killed. The profile sets the simulator's
translational damping to von Benzon's measured values, because with the published
defaults the model does not stay upright under surge
([Physics](#physics); lib/sim/README.md "Pitch under surge").

Its parameters are live ([05-parameters](05-parameters.md)): mass, buoyancy, drag
coefficients, payload mass and position, current, and each sensor's noise, bias and
latency. As built, a payload moves the centre of gravity for the restoring moment and
the inertia only: the thrusters' moment arms stay those of the bare vehicle (below). Changing buoyancy or payload during a run is how the adaptive depth-hold
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

## Physics

The simulator is deliberately the simple one, and the choice is settled rather than
open: a model hangar owns, calibrated from recordings, with a slot for a full simulator
when an experiment needs rendering.

**The model.** Fossen's rigid-body equation in the body frame,

```
(M_RB + M_A) ν̇ + (C_RB(ν) + C_A(ν)) ν + (D_lin + D_quad(ν)) ν + g(η) = τ_thrusters + τ_current
```

with the matrices diagonal, as the published identifications of the BlueROV2 take
them: `M_A` the added mass, `D_lin` and `D_quad` the linear and quadratic damping per
axis, `g(η)` the restoring vector from weight `W`, buoyancy `B` and the centre of
buoyancy's height above the centre of mass, and `τ_current` the damping terms evaluated
on the velocity relative to the water. Integration is fourth-order Runge–Kutta at
200 Hz with the quaternion renormalized each step. Nothing in it is novel; it is the
model used in the BlueROV2 identification and control literature, and the parameter
defaults are the published heavy-configuration values, each one named with its source
in `sim_vehicle`'s parameter file so that a replaced value can be traced.

**Thrust.** The T200's thrust against ESC command is tabulated, not fitted: a table of
thrust in newtons at each pulse width in 4 µs steps, forward and reverse, with the
deadband around 1500 µs, from the manufacturer's published measurements at the
battery's nominal voltage, scaled by a voltage factor from `power.battery`. The same
table, through the same allocation code as the vehicle's thruster mixing, turns
`thr.out` into applied thrust per axis for the depth-hold label
([09-learning](09-learning.md#example-adaptive-depth-hold)) and turns the simulated
command into force in `sim_vehicle`; simulation and vehicle cannot disagree about what a
pulse width means.

**Calibration.** The published coefficients are defaults, not truth. `hangar sim fit
<session>` fits buoyancy, the linear and quadratic damping of each axis and the
thruster lag to a recorded dive (least squares on `nav.*` and `thr.out` over the
session, with the fitted values and their residuals in a report), and writes them as a
parameter file the simulator loads. The simulator is thereby something to calibrate,
not to trust, and its error against the vehicle is a number reported per session.

**As built, and not on purpose** (`modules/sim_vehicle`, `lib/sim`; each is a
difference from the text above, to be closed or accepted):

- A payload off the centre of gravity changes the weight, the restoring moment (the
  centre of buoyancy moves the other way in the body frame) and the inertia. It does
  not change the thrusters' moment arms: the allocation is built once from the bare
  vehicle's geometry, so a payload forward of the CG gives no pitching moment under
  vertical thrust beyond the restoring one.
- The current is constant. With the default of zero, nothing disturbs the dynamics
  at all: sensor noise (white, with a constant bias) is the only randomness, and a
  depth hold measured in simulation is better than any current would leave it.
- Gravity is 9.81 m/s² in the simulator's weight and accelerometer (the value of the
  coefficients' source) and 9.80665 in `nav` and in the simulator's pressure:
  `nav.state.accel[2]` carries a constant −0.0034 m/s², which `nav`'s bias state
  absorbs.
- Standalone, a step that is late is made up, up to one second: the steps of one
  wake-up are published with one stamp and all use the newest `thr.cmd`; beyond 200
  late steps simulated time is dropped, with no log line and no counter.
- `d_lin` and `d_quad` have no range (`msg/sim_vehicle_params.msg`): a negative value
  is accepted, and the model is then unstable.

**What the model leaves out, on purpose**: hull and tether hydrodynamics beyond the
diagonal terms, wall and surface effects, current fields that vary in space, thruster
interaction. For version 1 of the learning loop and for the safety bench tests, what
matters is a vehicle whose buoyancy, drag and payload change on demand, deterministic
from a seed, in a process that costs nothing; fidelity is bought later, where an
experiment needs it.

**ArduSub SITL** is the autopilot in phase 1, with `sim_vehicle` as its external
physics over the JSON interface (`SIM_JSON`): each step sends the time, position,
attitude, velocities and accelerations, and the IMU and pressure readings, and reads
back the sixteen servo outputs. The real autopilot code (its MANUAL mixing, its
failsafes, its parameters as installed by `hangar ardusub-setup`) is thereby in the
loop on every run. SITL is built from the ArduPilot tree on the development machine;
nothing of it goes on the vehicle.

**A rendered stand-in for development.** On a development machine a Gazebo Harmonic
world with the `bluerov2_gz` BlueROV2 model, connected to SITL through the
`ardupilot_gazebo` plugin over the same JSON interface, can take `sim_vehicle`'s place
behind SITL and be flown from QGroundControl; `tools/sim/` is the recipe, a container
that leaves nothing on the host but Docker and the GPU runtime. It is a way to look at
the vehicle while the `mavlink` modules are written, not part of the `sim` profile:
its camera and pose would enter hangar through the `sim_bridge` module below, as
`cam.raw` and `sim.pose` over gz-transport, and nothing of it is built or tested with
hangar (the plugin and the model need no ROS).

**A full simulator later.** Stonefish (the closest fit for ROVs: hydrodynamics, camera
and sonar rendering, an existing BlueROV2 model) or another renderer plugs in through a
`sim_bridge` module that takes `sim.thrust` and publishes `sim.pose`, `sim.velocity`
and `cam.raw` in place of `sim_vehicle`'s physics and `sim_camera`. It is needed when
imitation from camera frames becomes the experiment and rendered pictures matter; until
then its dependency tree (C++, OpenGL, a GPU, non-deterministic frame times) buys
nothing the version-1 experiments use, and it can never run on the Pi or in the
release's own tests. Gazebo-based stacks are not considered: they bring the ROS
dependency tree the project exists to avoid.

## Determinism and replay

`--seed` seeds every simulator's noise. With `--pilot <session>` the pilot input is a
recording, so a run is meant to be a function of the seed, the session and the release:
two runs give the same topics, which the live-against-offline dataset test and the
equivalence tests of ported operators rely on. The recorder runs in the simulation as in
production, so a simulated dive is a session like any other and can itself be replayed.

**As built, the seed fixes the noise, not the run.** What is the same twice: lib/sim's
model and sensor channels for the same seed and the same commands at the same steps
(its suite, and `sim_vehicle`'s, compare them bit for bit). What is not: a standalone
run under a manager. `sim_vehicle` steps on a 200 Hz timer of the machine's clock and
applies, at each step, the newest `thr.cmd` that has arrived; `nav`, `depth_hold`, the
`mux` and `thrusters` each run a 50 Hz loop that starts when its process does and then
advances by its period. The phase of each loop against the simulator's steps, and
against the loop before it, is therefore set by when the manager happened to start the
processes. It does not drift during a run, so every run has its own delay from a depth
reading to the thrust it causes, constant for that run and unrelated to the seed: each
of the four hops waits between 0 and 20 ms. In the recorded runs of
`bench/e2e-depth-ff` the median from `nav.depth` to `thr.cmd` was between 34 and 60 ms
over 20 runs made on a loaded machine (37 and 39 ms for two runs with the same seed),
and 59.2 to 59.6 ms for all ten made on a quiet one (presumably the processes start at
the same intervals there; not tested). Consequences:

- two runs with the same seed give different topics, and differ in closed-loop numbers
  by as much as two seeds do;
- comparisons within one run (a baseline block against a learned block) share the
  delay and stand; comparisons between runs carry the difference in delay as well as
  the difference in noise;
- a model fitted with a fixed lag in ticks (`LAG` in `python/examples/depth_ff2.py`)
  assumes one delay for every run.

Each run's delay is measured from its recording (`control_latency` in the run's
`report.json`: every hop and the whole chain, through the messages' references), and
`e2e_depth_ff2.py --summarise` prints the whole chain's median per run beside the
seeds. Planned, not built: the simulator stepped from `thr.cmd` arrivals or a
fixed-phase start of the loops, which is what a run that is a function of its seed
needs; `--pilot <session>`.

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
  reaches the terminal velocity the damping coefficients give; the restoring moment
  rights the vehicle; energy decays monotonically with no thrust in still water.
- The thrust table: the same pulse width gives the same force in `sim_vehicle` and in the
  depth-hold label's conversion of `thr.out`, over the whole table.
- `hangar sim fit` on a session recorded in simulation with known parameters recovers
  them within the report's stated residuals.
- SITL in the loop: a MANUAL command through the vehicle `mavlink` module moves the
  simulated vehicle in the commanded direction, and SITL's GCS failsafe fires when the
  topside heartbeats stop.
- One writer per topic: with SITL in the loop, `nav.*`, `power.battery` and `thr.out`
  are written by `mavlink`, `vehicle.leak` by `leak`, and `sim_vehicle` writes `sim.*`
  alone; a leak raised on the simulated GPIO line reaches `vehicle.leak` and fires the
  `leak` trigger.
- Same seed, same session, same topics twice.
- Every bench test of 07 driven by `sim_faults` and asserted from the recording.
- `sim_camera` frames carry the pose-nearest recorded frame's sequence number in their
  provenance, so a dataset built in simulation can be traced to the footage it came from.
