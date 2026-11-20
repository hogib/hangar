# depth_hold: the classical depth controller

Reads `nav.state` (`modules/nav`) and publishes, at 50 Hz, `ctl.depth_hold` (a wrench,
heave only) and `ctl.depth_hold.target` (`msg/depth_target.msg`: the depth held, the
state, **`accel_z`**, the vertical acceleration it wants, which docs/09's dataset joins
and its policy reads, the integrator, whether it drives). The controller of docs/07's
`assisted` mode and failsafe `hold`, and of docs/09's adaptive depth hold.

## The controller (`dh_ctl.c`)

All positive down except the heave command, which is up positive:

    accel_z = clamp(kp (target - depth) - kd vel_z, ±accel_max)   the acceleration wanted
    heave   = clamp(-ff accel_z + I, ±max_thrust)                   the command
    I      -= ki (target - depth) dt, clamped to ±i_max             driving, within i_band, unclamped

| Parameter (`msg/depth_control_params.msg`, live) | Default | |
|---|---|---|
| `kp` | 4 /s² | m/s² wanted per m of depth error |
| `kd` | 4 /s | m/s² wanted per m/s (critically damped with `kp`) |
| `accel_max` | 1 m/s² | the largest acceleration wanted |
| `ff` | 0.1 | heave per m/s² wanted: mass over full vertical thrust |
| `ki` | 0.1 | heave per m s of error: the integrator, for buoyancy and drag |
| `i_max` | 0.3 | the integrator's largest share of the command |
| `i_band` | 0.3 m | the integrator learns only this close to the target, unsaturated |
| `max_thrust` | 0.6 | the largest heave command |

The integrator learns only within `i_band` of the target and while `accel_z` is not
clamped: integrating on the approach wound it up (27 % overshoot on a 1 m step at
`kp` 1). `kp` 4 keeps a buoyancy change's error inside the band (7 N gives about 0.09 m)
so that the integrator does take it out.

**Measured** by the suite against a simulated vehicle (16.5 kg with added mass, 160 N
of vertical thrust at heave 1 behind a 0.1 s lag, linear and quadratic drag): a 1 m step
rises (10–90 %) in **2.22 s**, overshoots **4.3 %**, settles within 5 cm after
**3.18 s**; a 7 N buoyancy step moves it **0.086 m** at worst, back within 5 cm after
**4.70 s**. The defaults are tuned against that model, not the lab's vehicle: on the
water, start with them, and `hangar param set depth_hold kp=… kd=…`.

## Who drives

It always publishes; whether its heave drives is the mux's choice (`modules/mux`):
`assisted` while the pilot leaves the heave stick alone, `residual:<name>`, and a
failsafe `hold`. It reads that choice from `mux.state`: `source_heave` 2, a controller,
or 4, a policy, which in residual mode is the controller's heave with the policy's
bounded residual added, so the controller is still the one holding. While it does not drive, its **target
follows the depth** and its integrator is held, so a handover (the stick released, a
failsafe hold) **holds the depth it happens at**, with the trim it had learnt. A target
from `set` holds until the controller, having driven with it, stops driving.

Calls: **`set`** (`depth_set_req`: a depth, refused outside −1 to 1000 m) and **`hold`**
(the depth it is at), `Allow` as the mux's `set_mode`.

`nav.state` is its `Inputs` topic: a stale depth flags its output, and a flagged
command that drives makes the gate go neutral (docs/07, bench test 3). A controller that
does not drive is never used by the mux, whatever it publishes, so it never flags
anything (docs/07, "an idle controller"). Before the first `nav.state` its outputs are
zeros flagged stale.

## Deviations from the docs

1. **Its parameter type is `depth_control_params`, not `depth_hold_params`.**
   `msg/depth_hold_params.msg` is docs/05's example and the fixture of the hangard and
   msggen suites (its field set and its 48 bytes are checked there); the controller's
   gains are another set.
2. **`depth_target` is defined here** (docs/02 names the type, docs/09 its `accel_z`):
   the depth held, the state, `accel_z`, the integrator, `driving`, `latched`.
3. **It always publishes, and does not keep an "engaged" state.** A controller that
   stopped publishing when disengaged would be restarted by the manager's staleness
   check (docs/02), and one publishing a flagged zero would make the gate go neutral
   whenever the mux used it; whether it drives is the mux's choice, read back from
   `mux.state`.
4. **Its `Inputs` is `nav.state`,** not docs/02's example's `nav.depth nav.imu`: docs/09's
   dataset anchors on `nav.state`, and the controller and the estimate are separate
   modules.
5. **The integrator's band** (`i_band`) is the module's own anti-windup.

## Tests (`make check-depth_hold`, about 40 s)

The controller alone against the simulated vehicle: the step and the buoyancy step
above; not driving, the target follows a 0.3 m/s descent and the integrator learns
nothing; handed the axis while sinking, it brakes and holds where it was handed it; a
set target kept until the controller, having driven with it, stops; `hold`; no
integration with the outputs disabled. Then the chain under a manager: `nav`,
`depth_hold` and the mux, the suite as the vehicle (it turns `ctl.wrench` into motion,
and the motion into `nav.depth`, `nav.attitude` and `nav.imu`), the pilot and safety:

- manual: the pilot has heave, `depth_hold` idle, its target following;
- `assisted`, 3 N light: handed the axis at 1.88 m, holds 1.84 m, trimmed to within
  5 cm;
- the pilot pushes down for 1.5 s and lets go: the new depth held (4.30 m for 4.32 m);
- `set` 1 m deeper: held (5.35 m for 5.32 m); a depth out of range refused;
- a failsafe `hold` in manual mode: the mux gives it heave, the depth kept (5.23 m for
  5.21 m);
- the depth stopped while it drives: its command flagged, and so the mux's;
- the idle CPU of both modules (0.0 %).
