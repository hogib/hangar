# policy: the generic policy program

The module of `docs/09-learning.md` "Policy modules", version 1: one module per policy,
`policy_<name>`, all running the one binary `hangar-policy` (`modules/policy/policy.c`)
on a policy spec. Example spec and manifest `toy.policy`, `policy_toy.module`; suite
`tests/test_policy.c` (`make check-policy`), with its weights files made by
`tests/make_models.py` from python/hangar's own writer and reference (no PyTorch).
GPL-3.0-or-later, C11, libc (libm, pthreads), lib/hg (with its dataset join code) and
lib/nn only.

## What it does

- Reads its spec (`Name`, `Dataset`, `Arch`, `TickBudget`; `DatasetDir`, by default
  `$HANGAR_ROOT/etc/hangar/datasets`) and the dataset spec; refuses a dataset an input
  join of which would have to wait (`hg_ds_policy_usable`), and a module name that is
  not `policy_<Name>`.
- Ticks on each anchor message: the row's `input` parts built with `hg_ds_join` and
  `hg_ds_row_finish`, the pairer's code, so the features and the reset bit are the
  pairer's for the same spec and topics; the model's input bindings resolved to row
  fields (`role=input` and `missing`), its output bindings to the wrench's fields.
- `policy.<name>.wrench`, one per anchor message, stamped with the anchor's stamp,
  referencing the anchor, the `policy.<name>.active` message of the model that computed
  it, and the input parts' seqs (4 at most), flagged stale when an input part was
  missing or stale. With no model: a neutral wrench, flagged, on every tick.
- `load_weights {name, blake2s}`: refused when a load is under way or the dataset is
  not bound yet; else checks 1 to 7 (`nn_load`, lib/nn) on a worker thread at
  `SCHED_IDLE`, step budget = `TickBudget` of the anchor's period less the running
  model's mean step; then the warm-up, beside the running model on the tick thread, for
  the file's `warmup_ticks`: a tick whose two steps took more than the tick period, or a
  non-finite warm-up output, rejects it (check 8). The swap is after a tick's output.
  `policy.<name>.load` carries checking, warming, done or rejected with the check and
  the reason; `policy.<name>.active` the new model, referencing the request.
- Keeping: after a swap the worker renames `current.hgw` to `previous.hgw` and the new
  file to `current.hgw` in the state directory, and copies it into the recorder's
  session (`<session>/weights/<hash>.hgw`) when `recorder.session` says one is open.
- `revert`: the previous model back at once, its state reset, announced with cause 2
  and referencing the request; the two files exchanged. `reset_state`: the running
  model's state (and a warming one's) reset. `runtime_info`: format version, ops and
  their versions, `Arch`, the dataset and its row type's hash, the tick period.
- At start, once the dataset's topics exist: `previous.hgw` checked and kept for revert,
  `current.hgw` through the worker and the warm-up, announced with cause 0.
- A non-finite output: nothing published at that tick, the state reset (lib/nn), counted
  in the status line; the mux demotes on the skipped tick.
- Its wait set is the anchor and its call topics, and both are read to their end at
  each wake (the lesson of hangar#1); with the dataset not bound yet the anchor is
  skipped. While a load is being checked the wait times out at 50 ms to take the
  worker's verdict, else at 500 ms.

## Measured (the suite, x86-64 laptop, debugoptimized)

A 2-to-2 dense model at 50 Hz: checks 1 to 7 about 20 ms, warm-up 40 ms (3 ticks),
request to swap 65 to 75 ms; no output missed across the load; the largest delay from
the anchor's stamp to the output read by the tester 31 us across the load and swap,
143 us in 500 ms at rest; step 2.3 us; under 1 % of a core.

## Until the bridge, L2 and sim_vehicle

- `load_weights_req.name` names a file in `<state dir>/incoming/`: until the bridge's
  `bridge.send` (docs/08) puts it there, the trainer writes it there itself
  (`hangar.learn.deploy`, marked `until the bridge`), which only works where the trainer
  and the policy share a file system (the bench, the sim).
- The suite drives the toy dataset's topics itself; a policy on `nav.state` waits for
  the L2 estimator, and closed-loop runs for sim_vehicle.

## Deviations from docs/09

- Check 1 is skipped for `current.hgw` and `previous.hgw` at start: no request names
  their hash (the file was checked when it came).
- Every dataset part must be subscribed, not only the inputs: the row type, whose hash
  check 4 compares, is built over all the parts, as the pairer's. Only the inputs are
  read.
- The warm-up's overrun is measured against the tick period, not `TickBudget`: the
  budget is what check 7 holds the new model's step to; a tick that overran its period
  is the one that delayed the running model's output.
- `revert` takes effect when the call is served, between two ticks, rather than "at the
  next tick boundary" with a wait: the loop serves calls between ticks.
- `test-warm-delay-us` in the state directory, read at the start of each load,
  delays each warm-up step: the suite's overrun on demand. Nothing writes it in use.
