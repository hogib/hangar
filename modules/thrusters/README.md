# thrusters: ctl.wrench to pulse widths, without ArduSub

`hangar-thrusters`, the actuator of a vehicle hangar drives without ArduSub: the
simulator's standalone mode (modules/sim_vehicle, phase 2) and, later, ESCs driven
directly. C11 over lib/hg; static for musl.

- At 50 Hz: `ctl.wrench` through the actuator gate (`hg_gate`, docs/07), mixed as
  ArduSub's MANUAL mode mixes for the BlueROV2's vectored frame (ArduPilot's motor
  factors, lib/sim/README.md "thruster directions"), 1500 ± 400 µs, every output
  scaled down by the largest when one passes full scale. Gate closed: 1500 µs on all
  eight.
- Publishes `thr.cmd` (`thruster_cmd`, `Actuator=yes`), what sim_vehicle reads, and
  `thr.out` (`thruster_out`), the pulse widths sent, as the mavlink module publishes
  ArduSub's servo outputs, so a dataset's labels read the same topic either way.
- `--neutral` (the manifest's `Neutral=`) sends nothing: `thr.cmd`'s readers stop the
  thrusters when it goes stale. Stopping, the module publishes 1500 µs once.
- One writer per topic: not in a root with `mavlink.module`.
- It waits only on its tick; the gate's topics are read every tick (hangar#1).

`make check-thrusters`: the mixing against lib/sim's `sim_applied_wrench`, the
simulator's own command path: neutral and non-finite inputs give 1500 µs; each axis at
0.5 pushes the right way (heave up is −z, yaw clockwise is +Mz), with no force across;
heave is monotonic; a saturated mix is scaled to stay within 1100 to 1900 µs. The
module under a manager is exercised by `tools/sim/e2e-depth-ff.sh`.
