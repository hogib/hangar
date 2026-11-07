# mux: the control source mux and envelope

The module of `docs/07-safety.md` "The mux": the one writer of `ctl.wrench`. Binary
`hangar-mux` (`modules/mux/mux.c`), manifest `mux.module`, parameters
`msg/mux_params.msg`, suite `tests/test_mux.c` (`make check-mux`). GPL-3.0-or-later,
C11, libc (libm) and lib/hg only.

## What it does

- Modes `manual`, `assisted`, `residual:<name>`; `auto` and `policy:<name>` refused
  as later. `set_mode` (`set_mode_req`: the mode and the hash of the model) is the
  promotion: refused unless the hash is the running model's, as the newest
  `policy.<name>.active` says, and refused for a policy the wiring does not carry.
- Per axis, at 50 Hz from its own clock: the pilot above `deadband` takes the axis and
  keeps it for `override_hold` (1 s); else in `manual` the pilot; in `assisted` the
  controller on the axes `controller_axes` names (bit 2, heave, by default) while
  `ctl.depth_hold` is fresh, the pilot elsewhere; in `residual:<name>` the running
  model's contribution added on the controller's axes, held within `residual_frac`
  (20 %) of the envelope per axis. A stale, flagged or non-finite pilot counts as zero
  and is not referenced.
- The envelope on every source after the choice: `max` per axis, `rate_max` per axis
  and second, and the depth band (`depth_min`, `depth_max`): a command pushing further
  out of it is zeroed and `nav.depth` referenced.
- Failsafe from `safety.state`: `surface` is heave `surface_thrust` up and nothing else,
  `hold` is heave from the depth controller while it is fresh, `neutral` is zero; the
  pilot's override holds in each.
- Demotion to `assisted`, with the reason in the status line and the log: the policy's
  output stale by its topic's own `stale_after` (the "skipped tick"), flagged as computed
  from stale inputs, not finite, or held at the residual envelope for more than
  `demote_saturation` of the last `demote_window`.
- Provenance: `hg_publish_with` names the sources that drove the message (the pilot,
  the controller, the policy, `nav.depth` when the band acted), at most 4, so the stale
  flag follows what drove and not what was ignored; a message with no source behind
  it (neutral) names nothing.
- `mux.state` on change and at 10 Hz: the mode, the source per axis (0 none, 1 pilot,
  2 controller, 4 policy), `residual_active`, the limit bits (0-5 envelope, 6-11 rate,
  12 depth, 13-18 residual).

## Deviations from docs/07, and why

1. **A message from a model other than the running one is no contribution and no
   demotion**: docs/07 says "no contribution for that tick", and a load swaps the
   model with a new `active` message that the next `wrench` references; the one tick
   between them must not demote. The hash is checked at promotion; afterwards
   whichever model the policy's `active` names drives ("Which model drives").
2. **A skipped tick is the topic's staleness** (three periods by default), not one
   period: one threshold for every reader, and a 50 Hz policy a few milliseconds late
   is not a fault.
3. **Which axes a controller holds is a parameter** (`controller_axes`, bits per axis),
   since a wrench carries no "I hold this axis" mark; docs/07's "depth, heading" is the
   heave bit for the depth controller, a heading controller would add yaw.
4. **The 4-reference limit is never reached**: with one pilot, one controller, one
   policy and `nav.depth` the mux has at most four sources per message, so the
   `HG_STALE`-for-a-fifth rule has nothing to do; it is written for a wiring with more
   policies (the mux takes up to 4 `policy.*.wrench` subscriptions).
5. **`surface_thrust` lives in `mux_params`**, since the mux carries the action out;
   docs/07 lists it among the actions without naming the module.
6. **The policy's hash is a `u64`** in `set_mode_req`, the `policy_active.hash` field's
   type; docs/07 says "the hash of the model" without a width.

## Tests (`make check-mux`, 20 s)

A manager with the module and the suite as every source (`tester`): the per-axis
choice in manual and assisted, the envelope (1.5 held at 1, the limit bit), the rate
limit (1 to −1 over ≈250 ms at 8/s), a stale pilot as zero and unreferenced, the
override and its hold (measured ≈1.0 s), the depth band with the `nav.depth`
reference, the residual envelope (0.5 held at 0.2), promotion refused without a model,
with a wrong hash and for an unknown policy, a message referencing an older `active`
as no contribution without demotion, a model swapped in driving in place, each
demotion rule (stale flag, skipped tick, non-finite, saturation ≈0.2 s of a 1 s
window), a policy's output never applied outside `residual`, `auto` and `policy:`
refused, the stale flag following the controller when it drives and not when it does
not, the failsafe actions.
