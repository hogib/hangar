# sim-depth-ff: the learning loop's experiment, in simulation

The root `tools/sim/e2e-depth-ff.sh` lays out and runs: docs/09's "Example: adaptive
depth hold" with no ArduSub and no Docker. Method and a run's results:
[bench/e2e-depth-ff/README.md](../../bench/e2e-depth-ff/README.md).

| Module | |
|---|---|
| `sim_vehicle` | standalone (`--seed 1`): writes `nav.imu`, `nav.attitude`, `nav.depth`, `power.battery`, reads `thr.cmd`; `buoyancy` is changed live |
| `thrusters` | `ctl.wrench` through the actuator gate to `thr.cmd` and `thr.out` |
| `nav`, `depth_hold`, `mux`, `safety` | as in `profiles/sim-vehicle`; the mux also subscribes to the policy's two topics |
| `pairer` | `datasets/depth_ff.dataset`: docs/09's spec with `nav_state`'s field names (`depth_m`) |
| `policy_depth_ff` | `hangar-policy` on `policies/depth_ff.policy`; the script rewrites `Arch` from the example's model |
| `recorder` | every topic in full, a session per arming |

Not the vehicle's profile, and not a starting point for one:

- `safety.params`: `leak_required = 0` (the simulator has no leak sensor) and
  `required = "mux thrusters"` (no mavlink module here).
- `depth_hold.params`: `accel_max = 4`, `i_band = 0.6`. With the defaults the
  controller does not hold this vehicle through a 4 N buoyancy change (the file says
  why); these two values are the second of four tries on this simulator, not a
  tuning.
- The manifests name `/usr/lib/hangar` and `/etc/hangar`; the script rewrites both to
  the build directory and its own root. Every module runs as the invoking user.
