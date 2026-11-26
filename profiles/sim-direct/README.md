# sim-direct: the vehicle flown without ArduSub, in simulation

hangar's own chain from the pilot's wrench to eight pulse widths, with no `mavlink`
module, no ArduSub and no Docker: docs/07's phase 2 as far as it is built. The
simulator stands in for the vehicle and its sensors. `make check-sim_direct` lays this
profile out under a manager and flies it.

| File | |
|---|---|
| `bluerov2-heavy.frame` | the eight thrusters' positions and directions (`lib/sim/sim_frame.h`); given to `thrusters` and to `sim_vehicle` |
| `thrusters.module` | `ctl.wrench` through the actuator gate, allocated through the frame at 16 V, to `thr.cmd` and `thr.out` |
| `sim_vehicle.module`, `sim_vehicle.params` | standalone (`--seed 1`), the same frame; surge, sway and heave damping as von Benzon et al. measured them |
| `nav.module` | `nav.state` from the simulator's `nav.depth`, `nav.attitude`, `nav.imu` |
| `depth_hold.module`, `depth_hold.params` | the depth controller, with levelling (0.5, 0.1) and heading hold (0.5, 0.1) |
| `mux.module`, `mux.params` | the pilot and the controller; in `assisted` the controller has heave, roll, pitch and yaw (`controller_axes = 60`) |
| `safety.module`, `safety.params` | as `profiles/sim-depth-ff`: no leak sensor, no tether, `required = "mux thrusters"` |
| `hangard.conf` | `Role=vehicle` |

The manifests name `/usr/lib/hangar` and `/etc/hangar`; whoever lays the profile out
rewrites both (the suite does, as `tools/sim/e2e-depth-ff.sh` does for its profile),
puts the frame file under `etc/hangar/` and each `<module>.params` at
`var/lib/hangar/<module>/params`. Every module runs as the invoking user.

## Flying it

Fly it in `assisted` (`hangar call mux.set_mode mode=assisted`). In `manual` nothing
levels the vehicle or holds its heading, and lib/sim's model keeps neither by itself
above half a metre a second (lib/sim/README.md "Pitch under surge").

Nothing in the profile publishes `pilot.wrench`: there is no topside and no bridge
here. The suite is the pilot. To fly it by hand, add whatever source the vehicle would
have (`joystick`, or `topside_mavlink` behind a bridge).

## What replaces ArduSub here, and what does not

docs/07 "Phase 2" has the table. In short: the mixing is `thrusters`', the armed state
is `safety`'s alone, levelling and heading are `depth_hold`'s, and depth, attitude, the
IMU and the battery are the simulator's. A leak has no source (`leak_required = 0`).
Nothing stands behind a dead hangar module the way ArduSub's own failsafes do.

## What `make check-sim_direct` checks (about 75 s)

The profile's own files, with this build's binaries, under a manager; the suite
publishes `pilot.wrench` and reads the simulator's truth, `thr.cmd` and `ctl.wrench`.

- Both modules log the frame they hold; disarmed, a full command moves nothing.
- Armed by `safety.arm` with no autopilot; `assisted`; `depth_hold.set depth_m=2`:
  within 0.1 m of 2 m after 6 s, then held between 1.98 and 2.01 m, roll within 3°
  and pitch within 1° (the attitude sensor's noise is in the loop here).
- Surge 0.5 for 8 s: 4.6 m forward, pitch within 1°, the depth within 3 cm, the
  heading within 1°.
- Every `thr.cmd` of that leg (401 of 401) names a `ctl.wrench` message; read back by
  its sequence number and allocated again through the frame file, that wrench gives
  exactly the pulse widths sent. Those wrenches name `pilot.wrench` and
  `ctl.depth_hold`.
- Sway 0.5 for 6 s: 3.0 m to the right. Yaw 0.1: a full turn clockwise in 4.7 s, and
  the heading it stops on is held.
- A disarm with the pilot still pushing: 1500 µs on all eight, naming no input, and
  the vehicle floats up. Armed again and the safety module killed: all neutral within
  about 300 ms, and still neutral when the manager has restarted it.

The numbers are one run's on an idle laptop; the suite's bounds are several times
wider, since the simulator keeps pace with the clock there. The motion itself is
judged at a fixed step in `make check-thrusters`, where it is the same on every run.

## What this is not

- Not a vehicle's profile: no hardware driver, no sensor modules, no `Neutral` command
  that reaches an ESC. Nothing here has been near the water or the lab's vehicle.
- Not a measured model: the 2 cm between the centres of gravity and buoyancy and the
  thrusters' 85 mm below the origin are published values nobody measured on a heavy.
  lib/sim/README.md says which three measurements would settle them.
- Not tuned: the levelling and heading gains were chosen on this simulator.
