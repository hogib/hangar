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
  `$HANGAR_ROOT/etc/hangar/datasets`; `Incoming`, the directory `load_weights` finds
  its files in, by default `incoming/` in the state directory) and the dataset spec; refuses a dataset an input
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
  session (`<session>/weights/<hash>.hgw`) when `recorder.session` says one is open,
  every model that swaps in during it.
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
143 us in 500 ms at rest; step 2.3 us; under 1 % of a core (below /proc's 10 ms
tick over 1 s). The features against the pairer's rows on the same spec, live: about 55
anchors per run, half with the input part stopped, bit for bit equal.

## The suite

A manager with the policy, the real mux and the real pairer (`Type=dataset:toy`, which
hangard now accepts, see below); the tester drives `toy.anchor` and `toy.target` at
50 Hz. Neutral and flagged with no model; runtime_info; a load with no output missed and
its active message referencing the request; checks 1, 2, 4, 5 and 6 failing in turn
with the running model untouched and named by the outputs, check 4 also for a model
trained at another tick period than the anchor's (`slow10hz`); a recurrent model
(one LSTM layer) refused by check 4 on a spec without `Filter=mask`, `Missing=flag`
and `Rate=every:1`, and loaded on the same spec with them (`test_recurrent_spec`, two
more policy modules); `current.hgw` overwritten in place and then truncated under the
running model, whose outputs do not change and whose process lives, and the next
load writing `previous.hgw` back as it was checked (`test_weights_are_private`); a second load refused; a
warm-up overrun (check 8); the features and the stale flag, then against the pairer's
rows; revert; a restart's reload of both files and revert after it; a non-finite output
unpublished and counted; Allow, and the manager's refusals of a misnamed policy and of
one lacking `.load`; `learn_live.py` (hangar.topic on the C writer's topic, deploy,
watch, revert, a deploy rejected by check 6); the mux's residual after set_mode names
the model, following a load and a revert, demoting on a stale input; idle CPU. 8 s.

## hangard: `Type=dataset:<name>`

Accepted like `frame`: no hash in the wiring, and when the writer opens the topic the
manager checks its schema is a dataset row (the six header fields first,
`hangard/policy_check.c`). The manager does not build the row type from the spec
itself: the pairer does, from the parts' types, and the policy's check 4 compares the
hash a model was trained on with the one it builds the same way.

## Where the weights file comes from

- `load_weights_req.name` names a file in the policy's incoming directory. By default
  that is `<state dir>/incoming/`, which the policy makes and owns: a trainer on the
  same machine writes the file there (`hangar.learn.deploy` with `via="local"`: the
  bench, the sim).
- Across the tether the file is sent with the bridge's `bridge.send` (docs/08 "Files")
  and arrives in the vehicle bridge's own directory,
  `/var/lib/hangar/bridge/files/policy_<name>/`. The policy's spec names that
  directory as `Incoming=`; the policy reads there and needs no right to write. The
  bridge does not write into the policy's state directory, and the policy's checks 1
  to 8 are what stands between a file that arrived and a model that runs. With
  `Incoming=` the policy does not make the directory, and its removal of the file
  after a load fails where it may not write: the bridge keeps the newest `MaxFiles`.
- Tested with `Incoming=` in `python/tests/test_deploy_tether.py` (two roots, two
  bridges, one user for every module); the suite here uses the default. Not tested:
  the policy and the bridge under different users.

## L2 and sim_vehicle

- The suite drives the toy dataset's topics itself; a policy on `nav.state` waits for
  the L2 estimator, and closed-loop runs for sim_vehicle.

## Deviations from docs/09

- The weights file is not mapped: it is read once into read-only memory of the
  process's own, and checks 1, 5 and 6 judge, and the ticks run on, exactly those
  bytes. `current.hgw` and `previous.hgw` are written from the two models' bytes at
  each load (docs/09 has the file "mapped into memory and read in place", and is
  corrected: a mapping, even a private one, shows later writes to the file and dies
  of a truncation). The incoming file is removed once `current.hgw` is written.

- "For a recurrent model, the spec has `Filter=mask`, `Missing=flag` and
  `Rate=every:1`" (check 4) is implemented as written, with "recurrent" meaning any
  op that keeps state between ticks (`state_bytes` > 0; today that is `lstm`). The
  other half of docs/09's sentence, "and `hangar dataset check` says so", is not:
  the command sees a spec, not the model that will run on it.
- The "spec hash" check 4 compares is the hash of the row type, which the three
  settings above are not part of: two specs that differ only in them have the same
  hash, and a recurrent model trained on one is told apart from the other only by
  the rule above, at the policy.

- Check 1 is skipped for `current.hgw` and `previous.hgw` at start: no request names
  their hash (the file was checked when it came).
- Every dataset part must be subscribed, not only the inputs: the row type, whose hash
  check 4 compares, is built over all the parts, as the pairer's. Only the inputs are
  read.
- A weights file's `warmup_ticks` is bounded: more than 30 s of ticks (1500 on an
  anchor without a rate) is refused as check 8. docs/09 sets no bound; without one a
  file could keep a load warming, and every later load and revert refused, for good.
- The warm-up's overrun is measured against the tick period, not `TickBudget`: the
  budget is what check 7 holds the new model's step to; a tick that overran its period
  is the one that delayed the running model's output.
- `revert` takes effect when the call is served, between two ticks, rather than "at the
  next tick boundary" with a wait: the loop serves calls between ticks. It is refused
  (`the last load's files are still being written: ask again`) while the worker is
  still writing the last load's files, as `load_weights` is: the worker has one job,
  and a revert's file exchange posted over it was lost, leaving `current.hgw` the
  model reverted from (`test_revert_while_files_are_written`).
- Not tested here: both models at the edge of `TickBudget` (the suite's overrun is
  forced), a request whose deadline passed during a restart, and the reset bits against
  a recorded stream with a reboot (the policy calls the pairer's `hg_ds_row_finish`, so
  the rule is the same code, but no recorded stream is replayed through both).
- `test-warm-delay-us` in the state directory, read at the start of each load,
  delays each warm-up step: the suite's overrun on demand. Nothing writes it in use.
