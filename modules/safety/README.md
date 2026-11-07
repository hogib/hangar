# safety: the vehicle's state

The module of `docs/07-safety.md` "Vehicle states" and "Failsafe": the one module that
arms, disarms, enters failsafe and publishes `safety.state`. Binary `hangar-safety`
(`modules/safety/safety.c`), manifest `safety.module`, parameters
`msg/safety_params.msg`, suite `tests/test_safety.c` (`make check-safety`), fake mux
`tests/fake_mux.c`. GPL-3.0-or-later, C11, libc and lib/hg only.

## What it does

- `safety.state` at 10 Hz and on every change: `disarmed`, `arming`, `armed`,
  `failsafe` with `outputs_enabled`, `action`, `triggers`, `since_ns`, `by`, `reason`
  as docs/07 writes them. The status line says the same.
- Calls, each with docs/07's `Allow` list: `arm` (the checklist, then `arming`
  published *before* `mavlink.arm` is started with `hg_call_start`), `disarm`
  (never refused; in failsafe it turns outputs off until `clear`), `estop` (failsafe,
  neutral, at once), `clear` (refused while a trigger is active; sends the manager
  `clear-failsafe` when `module` is the only one, and passes its refusal on), `check`
  (the checklist without arming: `check_rep.ok` and `detail`, what `hangar check`
  prints).
- The checklist: every module of `required` ready in `hangar.vehicle.modules`, no
  trigger active, `vehicle.leak` fresh and dry, `power.battery` fresh and above
  `battery_min`, `nav.depth` fresh, and, with an autopilot behind the actuators,
  `ardusub.state` fresh, disarmed, in MANUAL, with `params` 1. A refusal names the
  first check that failed; `check` names them all.
- Arming is asynchronous: `accepted` at once; `armed` when `ardusub.state` reports
  armed; `disarmed` with the reason on `mavlink.arm`'s refusal, on ArduSub's
  (`arm_result` and the `PreArm:`/`Arm:` texts from `ardusub.state`), or after
  `arm_timeout` (2 s). Without an autopilot (no routed `mavlink.arm` in the wiring),
  `arming` leads to `armed` at once.
- The triggers of docs/07's table, each with its action parameter: `module`,
  `tether`, `leak`, `battery` (below `battery_min` for `battery_time`), `depth` (beyond
  the mux's band, read from `mux.params`, by `depth_margin`), `autopilot` (the armed
  agreement judged against the `safety.state` each `ardusub.state` references, a
  self-disarm while armed, a mode other than MANUAL, silence for
  `autopilot_silence`), `estop`, `input` (a required input stale or absent). Several at
  once: `neutral` over `hold` over `surface`; `hold` falls to `neutral` when
  `depth_hold` is not ready and after `hold_time`.
- The latch: `<state dir>/failsafe`, written through a temporary name and a rename on
  entering failsafe and on every change of its triggers or action, removed by `clear`;
  a start with the file present starts in failsafe with its action and triggers,
  `by` `latch`. The file also records a disarm given in failsafe and an ended hold, so
  that a restart keeps the outputs off rather than surfacing or holding again.

## Deviations from docs/07, and why

1. **Which triggers count while disarmed.** docs/07 says the module "enters failsafe
   when a trigger fires" without restricting the state. Taken literally, every boot
   would start in failsafe (no sensor is up yet) and need a `clear`. Implemented:
   `module`, `estop` and the autopilot's armed-agreement disagreement count in every
   state; `tether`, `leak`, `battery`, `depth`, `input`, autopilot silence and
   autopilot mode count while `arming`, `armed` or in `failsafe`. The checklist still
   refuses `arm` while any of them is active.
2. **`estop` is an edge.** A trigger that stayed "active" forever could never be
   cleared; `estop` enters failsafe and stays in `triggers` and the latch, but does not
   block `clear`.
3. **The autopilot disagreement is latched** until `ardusub.state` says disarmed in
   MANUAL (the state in which the disagreement is over), then `clear` passes.
4. **`required` is `char[256]`, blank-separated**, not `char[8][32]`: the `.msg`
   grammar has no two-dimensional arrays. `required = "mux mavlink"` is the default.
5. **`leak_required`, `battery_required`, `depth_required`** are parameters (default
   on): a bench or a simulation without a `leak` module arms with
   `leak_required = 0` instead of pretending a probe. `profiles/sim-vehicle/safety.params`
   sets it; docs/07's behaviour is the default.
6. **The depth band comes from `mux.params`** (`depth_min`, `depth_max`), one more
   subscription than docs/07 lists; without the mux's parameters the `depth` trigger
   cannot be judged and does not fire.
7. **`hold`'s depth controller** is judged by `depth_hold` being ready in
   `hangar.vehicle.modules` (docs/07: "`neutral` at once if the depth controller is not
   ready"), since the safety module does not read `ctl.depth_hold`.
8. **A disarm in failsafe** (never refused) sets `outputs_enabled` off until `clear`,
   rather than leaving a `surface` running: making the vehicle safer is what the
   caller asked for. The disarm is latched with the failsafe, so a restart of the
   module does not bring the thrusters back.
9. **A system-failsafe message older than an accepted `clear-failsafe`** is not read
   as the trigger firing again: the manager publishes `hangar.vehicle.system` nominal
   within its next message, and a tick in between would otherwise re-latch.
10. **`safety.state` published from a handler references the request** through
    `hg_publish_with` (docs/01 "Calls"); a wiring without the request topic among the
    output's `Sources` (a hand-written one) falls back to an unreferenced publish.

## Until the bridge

- `tether`: there is no `bridge.vehicle.link`; `tether_enabled` is off by default and,
  on, fires at once with the reason "no bridge.vehicle.link (until the bridge)", so
  that enabling it is never silent. `tether_timeout` is kept for the module that will
  read the link.
- The checklist's "`bridge.vehicle.link` is `up`" is skipped.
- `topside:topside_mavlink` and the other topside tokens pass through the same-host
  shim (`lib/hg/NOTES.md`, "Phase 1"): a request read from the other root's `.req`
  file is a mirrored request to the library. With the bridge the same code reads the
  mirrored topic.

## Tests (`make check-safety`, 25 s)

A manager with the module, a fake mux (parameters only, named `mux`), the toy talker
as `depth_hold`, the toy crasher for a module given up with `OnGiveUp=failsafe`; the
suite itself is the vehicle's `mavlink` module (serving `arm`, publishing
`ardusub.state`, `vehicle.leak`, `power.battery`, `nav.depth`) and a raw caller for the
`Allow` lists. Every transition and refused transition of docs/07 "Tests": `arming`
before the call (the handler sees `arming`), armed on the report, disarmed on the
handler's refusal, on ArduSub's with its text, after the timeout (measured
≈1.0 s with `arm_timeout` 1); each trigger with its action and the combination rule
(leak + estop → neutral), the battery's time, the depth band by `depth_margin`, the
input's staleness (≈300 ms), hold to neutral with the controller stopped and after
`hold_time`; the latch across `hangar restart` with `arm` refused until `clear`; the
`module` trigger through a crasher, `clear` refused by the manager while it is failed
and accepted once it runs; `arm`/`clear`/`set_params` denied to an unlisted module,
`disarm`/`estop`/`check` open to any.
