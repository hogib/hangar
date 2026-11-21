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
  prints). An `arm` or a `clear` published before the module's own process started
  (it waited in its caller's ring through a restart, and the call library serves a
  restarted server what waited in time) is refused, "safety restarted after this arm
  was sent: send it again": docs/07 "A request from before a restart".
- The checklist: every module of `required` ready in `hangar.vehicle.modules`, no
  trigger active, `vehicle.leak` fresh and dry, `power.battery` fresh and above
  `battery_min`, `nav.depth` fresh, `bridge.vehicle.link` fresh and `up` with a packet
  from the topside within `tether_timeout` (with `tether_enabled`), and, with an
  autopilot behind the actuators,
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
  `autopilot_silence`), `estop`, `input` (a required input stale or absent). The
  tether, below. A
  message whose voltage or depth is not a number (NaN, an infinity) is a message that
  did not come: the last reading that was a number is judged while it is fresh by the
  topic's own rule, and after that the input is missing, named `power.battery voltage
  not a number` or `nav.depth depth not a number` in the checklist's refusal and in
  the `input` trigger's reason. `nav.depth` is a float off the wire, and the mavlink
  module publishes a NaN voltage when the autopilot reports that it measures none.
  With `battery_required = 0` such a battery is not judged, as an absent one is not.
  Several at
  once: `neutral` over `hold` over `surface`; `hold` falls to `neutral` when
  `depth_hold` is not ready and after `hold_time`.
- The latch: `<state dir>/failsafe`, written through a temporary name and a rename on
  entering failsafe and on every change of its triggers or action, removed by `clear`;
  a start with the file present starts in failsafe with its action and triggers,
  `by` `latch`. The file also records a disarm given in failsafe and an ended hold, so
  that a restart keeps the outputs off rather than surfacing or holding again. A
  latch file that is there and cannot be read as one (empty or cut short after a power
  cut, not this module's text, an open that fails for another reason than "no such
  file") is a failsafe too: neutral, `by` `latch`, reason `latch unreadable (...):
  failsafe assumed`, and a log line that names the file. The file exists only between
  entering failsafe and `clear`, so its presence is the fact. The write is not
  followed by an `fsync`: the docs want the latch renamed into place before the
  failsafe is published, and a storage flush in front of that message would delay
  the failsafe itself by as long as an SD card takes; the unreadable-latch rule is
  what makes the missing flush safe. The latch also carries `hold_since`, the
  `CLOCK_MONOTONIC` time at which a hold began: a hold lasts `hold_time` from then,
  across any number of restarts of the module, and a hold whose beginning is not
  known (a latch from before a reboot, whose time is of another clock; a latch
  without the line) is over, neutral.

## The tether

`bridge.vehicle.link` (`bridge.topside.link` under a topside role), every message of
it, not only the newest:

- **The last packet** is the newest `last_rx_ns` of a message whose `state` is `up`
  (3), cut to that message's publish time (a message cannot report a packet from
  after it was published; `last_rx_ns` is this machine's `CLOCK_MONOTONIC`). It is
  kept in the module, so a later message with `last_rx_ns` 0 (a restarted bridge) or
  no message at all (a dead one) changes nothing.
- **`tether`** (bit 1, `action_tether`, neutral by default) is active while `now -
  last packet >= tether_timeout` (3 s; 0.5 to 60), or while no packet is known at all
  (reason `tether: no packet from the topside yet`). The reason gives the time: `tether:
  no packet from the topside for 3.0 s`. A link down for less than `tether_timeout`
  fires nothing, and any number of such outages in a row fire nothing, since each
  packet starts the time again.
- **`input`** (bit 7) is active while the link topic is stale by its own rule (300 ms
  from the manager's wiring of the bridge) or absent: reason `input:
  bridge.vehicle.link stale`. A dead bridge therefore gives `input` after 300 ms and
  `tether` as well at `tether_timeout`.
- Both count while `arming`, `armed` or in failsafe (deviation 1) and block `clear`
  while they last. Disarmed they are the checklist's refusal, by name:
  `bridge.vehicle.link absent`, `... stale`, `... searching` (the link's state when it
  is not `up`), or the tether reason when the link says `up` with an old packet.
- **`tether_enabled`** (default 1) turns all of it off at 0: no checklist item, no
  `tether`, the link not counted by `input`. A machine without a bridge sets 0, as
  every profile under `profiles/` does (`rov.sh` sets 1 when it lays out the bridge
  with `BRIDGE=1`). With 1 and no bridge, `arm` is refused with `bridge.vehicle.link
  absent`; so is it when the manifest lacks `[Subscribe bridge.vehicle.link]` (logged
  at start).
- The link's `state` alone fires nothing: `connecting` with a recent packet is within
  the time. The bridge itself calls the link down 3 s after its last packet
  (`LINK_TIMEOUT_NS`), so with the default `tether_timeout` the two agree; a shorter
  `tether_timeout` fires before the bridge says down, a longer one after.

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

- The vehicle's `mavlink` module does not yet stop `MANUAL_CONTROL` at
  `tether_timeout` (docs/07 "ArduSub's own failsafes", docs/10): with `action_tether`
  `surface` or `hold`, ArduSub's GCS failsafe does not stand behind the trigger. With
  the default, `neutral`, hangar disarms ArduSub.
- `topside:topside_mavlink` and the other topside tokens pass through the same-host
  shim (`lib/hg/NOTES.md`, "Phase 1"): a request read from the other root's `.req`
  file is a mirrored request to the library. With the bridge the same code reads the
  mirrored topic.

## Tests (`make check-safety`, 45 s)

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
`disarm`/`estop`/`check` open to any. `test_tether`, with `tether_timeout` 1 s and the
suite publishing `bridge.vehicle.link`: enabled with no link topic, and with a link
`searching`, `check` and `arm` refuse by name; with the link up the vehicle arms and
stays armed; a 500 ms outage, and four 400 ms outages in a row, fire nothing; the link
down while armed is `tether`, neutral, 0.9 s after it went down, `clear` refused until
it is back; down for 2.5 s while disarmed is no failsafe and a refused `arm`; the
topic stopped while armed (a dead bridge) is `input` after 0.2 to 0.3 s and `tether`
with it at 0.9 s; with `action_tether` 1 the outputs stay on. Every other test of the
suite runs with `tether_enabled = 0` and no link topic, as a profile without a bridge
does. Before, the trigger did not read the link: off, a lost tether did nothing; on,
it fired at once. `test_not_a_number`: with NaN volts, then an
infinite depth, `check` and `arm` refuse by name; armed, each becomes the `input`
trigger (neutral) with `clear` refused while it lasts; forty depth messages of which
every other one is NaN fire nothing; with `battery_required = 0` NaN volts arm and
stay armed. Before the rule the NaN voltage passed the checklist and armed, and the
NaN depth left an armed vehicle armed with no trigger. `test_latch_unreadable`: in
failsafe by e-stop, the module is stopped, its latch file emptied (then, a second
time, filled with a cut header and garbage) and the module killed: the manager's
restart comes back in failsafe by `latch`, neutral, `latch unreadable` in the reason
and the log, `arm` refused until `clear`, which removes the file. It started
`disarmed`, reason `start`, before. `test_hold_across_restarts`: `hold_time` 6 s,
the module killed 2 s and 4 s into the hold with the inputs flowing: it comes back in
the hold each time and the hold ends 6.0 s after it began (10.2 s before: each start
began it again), and an ended hold stays ended across a third restart.
`test_request_from_before_start`: `hangar call safety.arm` sent while the module is
stopped, the module then killed: the instance the manager starts refuses it and stays
disarmed, with no `mavlink.arm` (it armed before); the same for `clear` in a latched
failsafe, which stays; `estop` sent the same way is served.
