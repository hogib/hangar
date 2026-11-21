# sim-depth-ff2: the learning loop's experiment, second design

The root `tools/sim/e2e-depth-ff2.sh` lays out and runs: docs/09's "Example: adaptive
depth hold" as it is written now, with no ArduSub and no Docker. Method and results:
[bench/e2e-depth-ff/README.md](../../bench/e2e-depth-ff/README.md), "Second design".
The first design's root is [profiles/sim-depth-ff](../sim-depth-ff/README.md).
`tools/sim/e2e-adapt.sh` runs the adaptation experiment on the same root (the same
page, "Adaptation").

The same modules and manifests as `profiles/sim-depth-ff`; what differs:

| | |
|---|---|
| `datasets/depth_ff.dataset` | three inputs the policy has at its tick (`accel_z`, `vel_z`, `integral`); as labels the acceleration `nav.state` reports and the last eight `thr.out` messages; no join looks ahead; rows with loss wherever the outputs are enabled and the controller drives |
| `policies/depth_ff.policy` | `Arch` of `python/examples/depth_ff2.py`'s model (no LSTM); the script rewrites it from the model, a test checks this copy |
| `sim_vehicle.module` | `--seed` is rewritten per run (`E2E_SEED`) |

Not the vehicle's profile, and not a starting point for one:

- `safety.params`: `leak_required = 0` (the simulator has no leak sensor) and
  `required = "mux thrusters"` (no mavlink module here).
- `depth_hold.params`: `accel_max = 4`, `i_band = 0.6`, as in the first design: with
  the defaults the controller does not hold this vehicle through a 4 N buoyancy
  change (the file says why). Both the baseline and the residual run with them.
- The manifests name `/usr/lib/hangar` and `/etc/hangar`; the script rewrites both to
  the build directory and its own root. Every module runs as the invoking user.
