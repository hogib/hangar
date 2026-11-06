# The learning loop

Training a network on the fly from what the vehicle is doing, and putting it to work
on the vehicle while it runs:

```
 vehicle                                              topside GPU
 ───────                                              ───────────
 modules ──► pairer ──► dataset.<name> ══ bridge ══►  trainer (PyTorch)
    ▲                                                     │
    │                                                     ▼ export
   mux ◄── policy ◄── load_weights ◄═══ bridge.send ═══ weights file
    │        │
    └────────┴──► recorded, paired again ──► new samples
```

This loop is what hangar is for. Every link in it is recorded, every step can be
undone, and no step trusts the one before it: the weights are checked on the vehicle,
run in shadow until an operator promotes them, drive only within an envelope, and are
demoted automatically when they misbehave. The one exception to shadow is online
adaptation inside the residual envelope ([Updates while active](#updates-while-active)).

Two kinds of learning run on it, with one worked example each:

- **imitation**: a policy learns to do what the pilot did
  ([Example: imitating the pilot](#example-imitating-the-pilot));
- **online adaptation**: a network learns, during the dive, a correction to a
  classical controller as the vehicle's dynamics change
  ([Example: adaptive depth hold](#example-adaptive-depth-hold)).

## The rules

- **Only weights change during a dive, never code.** A policy's architecture is fixed
  by its policy spec (`Arch`) and installed like any module; the loop replaces its
  weights.
- **A model sees at run time the features it was trained on.** The policy builds its
  inputs with the same dataset spec, and the same join code, that built its training
  rows; a missing part is the same zeros on both sides. A recurrent model is trained
  over the same unbroken runs, with its state reset at the same rows, as it runs on the
  vehicle.
- **Weights are checked before they run**: hashes, architecture, finite values, the
  time a step takes, probe outputs computed on the topside that the vehicle must
  reproduce, and every intermediate value bounded on those probes.
- **Outputs are checked every tick.** A non-finite output is never published.
- **A new model starts in shadow** and drives only when an operator promotes it, naming
  the model; it is demoted automatically ([07-safety](07-safety.md#the-mux)). The one
  exception: a policy with `UpdateWhileActive=yes` that an operator has promoted to
  `residual:<name>` takes each new model after its checks and warm-up, within the
  residual envelope ([Updates while active](#updates-while-active)).
- **The previous weights are kept**, across restarts, and `revert` restores them at
  once.
- **Lineage is complete**: every action names the model that produced it, and every
  model names, by vehicle boot and row ranges, the rows it was trained on.
- **The runtime grows by adding ops**, without changing the file format or anything
  that already works.

## The loop, step by step

1. The **pairer** on the vehicle publishes `dataset.<name>`, one row per anchor message,
   each row carrying the vehicle's boot id ([06-recorder](06-recorder.md#datasets));
   the bridge mirrors it to the topside at `dataset` priority
   ([08-bridge](08-bridge.md#delivery)), `reliable` when its rows carry frames.
2. The **trainer**, a module on the topside GPU, reads the rows live and trains
   continuously.
3. From time to time it **exports** a weights file (`.hgw`), with the lineage of the
   rows it was trained on and a set of probes.
4. It **sends** the file down (`bridge.send`, at `bulk` priority) and calls
   `policy_<name>.load_weights`.
5. The **policy module** checks the file, warms the new model up beside the running one,
   and swaps it in between two ticks. It reports the load's progress on
   `policy.<name>.load` and announces the swap on `policy.<name>.active`.
6. The policy's output runs in **shadow**, compared live against whatever is driving
   (`policy.<name>.eval`), until an operator promotes that model with `mux.set_mode`;
   under `UpdateWhileActive=yes` in `residual:<name>`, the new model drives at the swap.
7. Its **actions** are recorded and paired, and become new samples.

The loop needs, in both copies of the bridge's configuration, `dataset.<name>` mirrored
up, and `policy.<name>.active`, `policy.<name>.load` (event topics, `reliable`) and
`policy.<name>.eval` mirrored up ([08-bridge](08-bridge.md#configuration)): the trainer
waits on the first two, and the operator promotes on the third. `deploy` gives up after
60 s without a `policy.<name>.load` message for its file, and says so.

## Policy modules

A policy is a module running the generic `hangar-policy` program with its own
manifest and policy spec. Each is its own module (`policy_pilot`, `policy_depth_ff`),
with its own process, restarts and parameters.

```ini
# /etc/hangar/modules/policy_pilot.module
[Module]
Description=Pilot imitation policy
Exec=/usr/lib/hangar/hangar-policy /etc/hangar/policies/pilot.policy
# writes the weights it swaps in into the recorder's session
SupplementaryGroups=hangar-rec

[Subscribe nav.state]
[Subscribe cam.nn]
[Subscribe ctl.wrench]
[Subscribe mux.state]
[Subscribe mux.params]
[Subscribe recorder.session]

[Publish policy.pilot.wrench]
Type=wrench
Rate=50
Inputs=nav.state policy.pilot.active

[Publish policy.pilot.active]
Type=policy_active

[Publish policy.pilot.load]
Type=policy_load

[Publish policy.pilot.eval]
Type=policy_eval
Rate=1

[Serve load_weights]
Request=load_weights_req
Reply=load_weights_rep
Allow=topside:trainer topside:pilot_console admin topside:admin

[Serve revert]
Request=empty
Reply=policy_version
Allow=topside:trainer topside:pilot_console admin topside:admin

[Serve reset_state]
Request=empty
Reply=empty
Allow=topside:pilot_console admin topside:admin

[Serve runtime_info]
Request=empty
Reply=runtime_info
Allow=topside:trainer topside:pilot_console admin topside:admin

[Schedule]
CPUs=2
```

The policy spec says what the policy runs on. It is part of the installation, like the
manifest, and cannot be changed from the topside:

```ini
# /etc/hangar/policies/pilot.policy
[Policy]
Name=pilot
Dataset=pilot_imitation
Arch=4be1c0d2a97f3e18
TickBudget=50%
UpdateWhileActive=no
```

| `[Policy]` key | Default | |
|---|---|---|
| `Name` | — (required) | The policy's name, `[a-z0-9_]`. The module is `policy_<name>`; it publishes `policy.<name>.wrench`, `.active`, `.load` and `.eval`; its mux modes are `policy:<name>` and `residual:<name>`. The manager refuses a module that runs `hangar-policy` under another name or without those four outputs |
| `Dataset` | — (required) | The spec that builds the inputs, and whose `ResetAfter` resets recurrent state; it must be usable by this policy, with its substitutions ([Features at run time](#features-at-run-time)) |
| `Arch` | — (required) | The architecture hash a weights file must have |
| `TickBudget` | `50%` | The largest share of the tick period the model steps of one tick may take together: the running model's step, plus during a warm-up the new model's ([Loading weights](#loading-weights)) |
| `UpdateWhileActive` | `no` | Whether a new model may replace a model in `residual` mode without demotion ([Updates while active](#updates-while-active)). Published in `policy.<name>.active`, so the mux can act on it |

A policy's topics:

| Topic | Kind | |
|---|---|---|
| `policy.<name>.wrench` | the anchor's rate | The output: one message per anchor message |
| `policy.<name>.active` | event | One message per swap or revert: the model now computing the output ([Loading weights](#loading-weights)) |
| `policy.<name>.load` | event | Progress of a load: checking, warming, done, rejected with the reason |
| `policy.<name>.eval` | 1 Hz | [Shadow evaluation](#shadow-evaluation) |

**With no running model** (a fresh installation, or a restart whose weights are
still being checked), the policy still publishes on every tick: a neutral wrench,
flagged as computed from stale inputs (slot flag bit 0), which the mux treats as no
contribution ([07-safety](07-safety.md#the-mux)), and an eval message with model
version 0. The module is therefore ready and fresh, and is never restarted for lack
of a model.

**At start** the policy reloads its weights from its state directory,
`/var/lib/hangar/policy_<name>/`: `current.hgw`, the model that was running, and
`previous.hgw`, kept for `revert`. Both go through checks 1 to 8; the current one is
warmed up and swapped in, and announced with cause `start`. A restarted policy always
comes back in shadow: the mux demoted it when its output went stale.

## Features at run time

The policy ticks on each message of the dataset's anchor. At each tick it builds the
row's `input` parts with the pairer's own join code: provenance by seq, `asof` by
stamp, windows by the preceding seqs. A spec qualifies only if no `input` join would
have to wait, unless the policy substitutes it (below)
([06-recorder](06-recorder.md#joins)), so the inputs are complete at the moment of the
anchor. The pairer, too, joins the `input` parts when it reads the anchor, and waits
only for `label` and `filter` parts. `label` and `filter` parts are not built by the
policy: it acts on every tick, whoever is driving.

A part that is missing (gone from its ring, or its topic absent) is filled with zeros
and its bit set in the row's `missing` field, identically in the row and in the
policy's inputs; a model may bind the missing bits and a part's `count` as inputs.
When any `input` part is missing or stale, the policy sets slot flag bit 0 on its
output itself, since the parts need not be among the output's `Inputs`
([01-topics](01-topics.md#stale-inputs)); the actuator gate and the mux then treat
that output as no command.

The input bytes a policy hands its model at a tick are therefore byte-identical to the
input part of the dataset row for the same anchor message, except where one side
missed a message the other had, the cases that
[06-recorder](06-recorder.md#offline-the-same-rows-from-a-recording) lists; a test
checks it. A model never meets features at run time that differ from those it was
trained on.

**One deliberate exception: substitution.** A model may be trained on something
measured afterwards and run on what is wanted instead: trained on the motion that
followed a thrust, run on the motion the controller wants. The policy spec names the
substitution explicitly:

```ini
[Substitute future_state]
Topic=ctl.depth_hold.target
Fields=accel_z
```

The part `future_state` is the dataset's `asof:+200ms` join of `nav.state`. A spec is
usable by a policy when every `input` join the policy does not substitute has
`offset + Lateness ≤ 0`; the policy checks this at start, and `hangar dataset check`
says which joins would need a substitution. The substitute topic is one of the policy's
subscriptions, joined as the newest message published at or before the anchor's
publish time, without waiting; its seq, stamp, count and flags fill the part's header,
and when it is missing or stale it counts as a missing part, for the `missing` bits
and for `ResetAfter`. The substitute must have the same fields and types as the part
it replaces. A substituted part is excluded from the byte-identity test, and
`policy.<name>.eval` reports the substitution, so nobody mistakes it for the general
case.

## The runtime

A small inference engine inside `hangar-policy`: no dependencies, all memory allocated
when a model is loaded, nothing allocated per tick. A model is a graph of **ops**, each
of a type and a version, reading and writing named buffers in a fixed order.

### Ops

Each op is one source file implementing one contract:

| Function | |
|---|---|
| `check(attrs, input shapes) → output shape, or the reason` | at load |
| `state_bytes(attrs, input shapes)` | memory kept between ticks (recurrent state) |
| `scratch_bytes(attrs, input shapes)` | memory used within a step |
| `run(attrs, tensors, inputs, output, state, scratch)` | one step |
| `reset(state)` | back to the initial state |

and one line in the registry: `{type, version, name, the functions}`. Adding an op is
that file, that line, golden test vectors computed with PyTorch, and its mapping in the
exporter. Nothing else changes: not the file format, not the other ops, not the policy
module.

A **kernel** is an implementation of an op version. Faster ones (NEON, int8) are added
beside the portable one, chosen at load by what the processor has, and must reproduce
the same golden vectors within the op's tolerance. The op, its attributes and its
results do not change.

**Version 1 ops:**

| Op | Attributes | |
|---|---|---|
| `dense` | in, out, bias | |
| `conv2d` | in channels, out channels, kernel, stride, padding, bias | CHW layout |
| `maxpool2d`, `avgpool2d` | kernel, stride | |
| `global_avgpool` | | |
| `relu`, `tanh`, `sigmoid` | | |
| `affine` | per-channel scale and shift | input normalization, folded batchnorm, output scaling |
| `add`, `concat` | axis | skip connections; merging inputs |
| `flatten`, `reshape` | shape | |
| `cast` | from u8/i8/u16/i16/u32/i32/f64 to f32, scale | raw row fields to f32 |
| `lstm` | input size, hidden size, layers | [LSTM](#lstm) |

A weights file using an op or version this runtime does not have is refused at load,
naming it (`op lstm v2 not supported`). `policy_<name>.runtime_info` lists the ops and
versions the vehicle has, and the exporter checks against it before anything is sent.

### LSTM

The semantics of PyTorch's `nn.LSTM`, unidirectional and without projection, so that
the exporter maps a trained module directly:

- gates in PyTorch's order (input, forget, cell, output); per layer *k*, `W_ih` is
  4H × I for layer 0 and 4H × H for layers 1 and up, `W_hh` is 4H × H, and `b_ih` and
  `b_hh` are 4H each; stacked layers feed each the hidden state of the one below. The
  exporter refuses `bidirectional=True` and `proj_size > 0`; dropout between layers has
  no effect at inference and is ignored;
- **one time step per tick**: the hidden and cell state of every layer are kept between
  ticks (`state_bytes` = 2 × layers × H floats), so the network's memory replaces an
  input window;
- **the step rate is part of the model**: the weights file records the tick period it
  was trained at, and a policy whose anchor's declared rate differs refuses it.

**State resets** (to zero) on:

- the rule the pairer applies to set `reset` in the row
  ([06-recorder](06-recorder.md#the-row)): a gap in the anchor's sequence numbers, a
  change of vehicle boot, or an `input` part missing or stale for the dataset's
  `ResetAfter`. The policy applies the same rule at its tick, so its state resets at
  exactly the rows where training starts a slot from zero state ([Training](#training));
- `policy_<name>.reset_state`, `revert`, a swap to a new model, and a restart. These
  leave no mark in the rows; each starts from zero state, as a run in training does
  after a `reset`.

A spec used by a recurrent policy (one with an `lstm` op) must have `Filter=mask`,
`Missing=flag` and `Rate=every:1`, so that the rows are every tick the policy steps
through; the policy refuses the weights otherwise (check 4), and `hangar dataset check`
says so.

**Warm-up on a new model.** A new model's recurrent state means nothing until it has
seen some inputs. After its checks pass, the new model runs beside the current one on
the live inputs for `warmup` ticks (from the weights file; by default the chunk length
it was trained on). On each of those ticks the tick thread steps the current model,
publishes its output, then steps the new one; then the outputs switch. A policy with no
running model warms the new one up the same way, publishing its neutral flagged output
meanwhile.

### Numerics

`f32` throughout, with the C library's `exp` and `tanh`. Results differ from the
topside's in the last bits, and across processors; probes are compared within the
tolerance in the weights file, never bit for bit.

Every tick, the policy checks its output and the recurrent state for finite values. A
tick with a non-finite value publishes nothing; the state is reset, the event is
counted in `policy.<name>.eval`, and the mux demotes the policy at once
([07-safety](07-safety.md#the-mux)).

## The weights file (`.hgw`)

Self-describing, mapped into memory and read in place:

```
[ header: 4096 bytes ][ section directory ][ sections ][ tensor data, 64-byte aligned ]
```

**Header**: `magic` (`"HGWEIGHT"`), format version, the architecture hash, the model's
name, the tick period it was trained at, `warmup` ticks, the probe tolerance (relative
and absolute), the **activation bound** (the largest absolute value any buffer may hold
on the probes and range samples, [Loading weights](#loading-weights)), `autopilot_mode` (the ArduSub flight mode its labels were collected in,
[Example: imitating the pilot](#example-imitating-the-pilot)), creation time, the
trainer's description (program, version, a hash of its settings), and the summary of
its **lineage**:

| Lineage field | |
|---|---|
| `dataset` | name, and the hash of the spec |
| `rows` | rows used |
| `steps` | training steps |
| `parent` | hash of the weights this training started from, if any |
| `newest_row` | vehicle boot id and `anchor_stamp_ns` (that boot's clock) of the newest row trained on |

**Sections:**

| Section | Records |
|---|---|
| ops | per op: type, version, attributes, input buffers, output buffer, tensors |
| tensors | per tensor: name, element type (`f32`, `i8`), shape (up to 4 dimensions), offset, size |
| buffers | per buffer: name, shape |
| inputs | per model input: its buffer, and the row fields it is bound to, in order (a part's fields, its `count`, its missing bit) |
| outputs | the output buffer, and the fields of the output topic's type it fills, in order |
| probes | input sequences from zero state, and the expected output at every step; one of them at least 20 × the chunk length, from an unbroken recorded run |
| ranges | optional: per input field, the smallest and largest value in the rows trained on |
| lineage | one entry per source of rows, any number: vehicle boot id (u8[16]), recorder session id or `live`, first and last `anchor_seq` and stamp, rows used from it |

The **architecture hash** covers the ops, their attributes, the shapes, and the input
and output bindings: everything but the tensor values and the lineage. Training changes
the weights and keeps the hash; changing the network changes it, and the policy refuses
the file.

Input normalization and output scaling are `affine` ops inside the model, so they
travel with the weights and cannot drift from them.

## Loading weights

`policy_<name>.load_weights {name, hash}`, naming a file the bridge received
([08-bridge](08-bridge.md#files)). The call answers at once (`accepted`, with the
version it will get, or refused while another load is in progress); the work happens
beside the tick path, on a worker thread of the policy module running at the lowest
priority (`SCHED_IDLE`), so that it never delays a tick. Its progress is on
`policy.<name>.load`:

1. the file's hash matches the call's;
2. the format version, and the architecture hash equal to the policy's `Arch`;
3. every op and version is in this runtime, and every op's `check` passes;
4. the lineage's dataset is the policy's `Dataset`, with the same spec hash; the input
   bindings are fields of the dataset's `input` parts, and the output bindings fields
   of the output type; for a recurrent model, the spec has `Filter=mask`,
   `Missing=flag` and `Rate=every:1`;
5. every tensor value is finite;
6. the probes, from zero state, reproduce the expected outputs at every step within the
   tolerance;
7. activations are bounded: running the probes, and, when the file carries `ranges`,
   samples spanning them (each input field at its smallest, largest and middle value,
   and random points between, from zero state over the chunk length), every
   intermediate buffer, recurrent state included, stays finite and within the
   activation bound of the header. A model that is finite on its probes but overflows
   on an input it may meet is refused here, not at its first bad tick;
8. the new model's step time, measured on the probes before warm-up, while the tick
   thread is waiting for its next anchor, plus the running model's, is under
   `TickBudget` of the tick period;
9. warm-up beside the running model, then the swap, between two ticks.

A failed check stops the load: the running model is untouched, and
`policy.<name>.load` says which check failed and why. A tick that overruns its period
during warm-up stops the load the same way (`warm-up overran`).

**The swap.** Between two ticks, the tick thread publishes `policy.<name>.active` for
the new model, then computes the next output with it. Every output therefore names, in
its provenance, the `active` message of the model that computed it, and an output
computed while another file is being checked or warmed up names the running model, not
that file. The worker then keeps the files: the old `current.hgw` becomes
`previous.hgw`, and the new file `current.hgw`. It also copies the new file into the
current recorder session, as `weights/<hash>.hgw` (following `recorder.session`, as
the encoder does), so that the recording holds every model that drove or ran in shadow.

The previous model is kept in memory; `revert` swaps back to it at the next tick, with
its recurrent state reset, publishes `active` with cause `revert`, and exchanges the
two files.

`policy.<name>.active`, an event topic:

```
u32  version              # counts up per swap on this vehicle
u32  autopilot_mode       # from the weights file: the ArduSub mode its labels were collected in
u8   cause                # 0 start (current.hgw reloaded), 1 load, 2 revert
u8   update_while_active  # from the policy spec
u8   reserved[6]
u64  hash
u64  arch
u64  swapped_ns
char dataset[32]
u64  dataset_hash         # of the spec
u8   newest_row_boot[16]  # vehicle boot id of the newest row trained on
u64  newest_row_stamp_ns  # its anchor_stamp_ns, on that boot's clock
u64  rows                 # rows trained on
```

`policy.<name>.load`, an event topic:

```
u32  version          # the version the load will get
u8   state            # 1 checking, 2 warming, 3 done, 4 rejected
u8   check            # with rejected: the check that failed, 1 to 9
u8   reserved[2]
u64  hash
u64  since_ns
char reason[64]
```

## Lineage

- Each `policy.<name>.wrench` message has, in its provenance, the anchor message it was
  computed at and the `policy.<name>.active` message of the model that computed it.
- That `active` message names the weights' hash; the weights file, kept in the
  recording, names the dataset, and per vehicle boot and session the row ranges it was
  trained on.
- Each row names its vehicle boot, and by `(topic, seq)` the messages it was made from;
  each of those, its own inputs. `(boot id, topic, seq)` identifies a message on either
  side of the tether ([08-bridge](08-bridge.md#mirrored-topics)).
- A thruster command names the `ctl.wrench` message it carried, and `ctl.wrench` the
  source messages the mux used for it ([07-safety](07-safety.md#the-mux)), among them
  the policy's.

So the recording answers, for any thruster command: which model produced it, which rows
trained that model, and which sensor readings and pilot commands made those rows.
Nothing is reconstructed from timestamps; every link is an identifier. A lineage entry
gives the span of rows and how many were used; rows inside the span that were masked,
lost on the tether or never sampled are not listed one by one. Rows streamed while no
session was recording name messages no recording holds; their entry still names the
boot and the span.

**Loop latency.** For each model, the time from the newest row it was trained on to its
first output while promoted: how long the loop takes to turn an experience into an
action. It is computed on the vehicle, as the `publish_ns` of the first promoted output
naming the model's `active` message minus `newest_row_stamp_ns`, and only when
`newest_row_boot` is the vehicle's current boot; a model trained on no rows of this
boot has no loop latency. `hangar dataset loop-latency <session>` reports it per model;
`policy.<name>.active` gives the part up to the swap live.

## Shadow evaluation

While the policy runs, it compares its output with what actually drove: the
`ctl.wrench` applied at the same tick, and from `mux.state` who produced each axis. It
publishes, once a second, `policy.<name>.eval`:

| Field | |
|---|---|
| `model_version`, `model_hash` | the model evaluated; version 0 while there is no running model |
| `driver[6]` | per axis, who drove it during most of the window (pilot, a controller, this policy) |
| `error[6]` | per axis, mean absolute difference from `ctl.wrench` over the last 10 s. On an axis the pilot drove, this is the imitation error; on an axis a controller drove, a comparison with the controller |
| `saturated` | the share of ticks at which an axis of the output was beyond the envelope's largest command (from the mux's parameters on `mux.params`). The rate limit and the depth band depend on the mux's own state and are not counted |
| `substituted` | whether the policy runs on a substitution ([Features at run time](#features-at-run-time)) |
| `nonfinite` | ticks with a non-finite output or state in the window |

The window starts over at every swap, so an eval message never mixes two models. The
operator promotes on that evidence, with `mux.set_mode` naming the `model_hash` they
evaluated; the mux refuses the call if that is not the running model's hash. The mux
demotes on its own evidence ([07-safety](07-safety.md#the-mux)).

## Updates while active

In `policy:<name>` the network has the whole axis: a new model is always demoted and
must be promoted again. In `residual:<name>` the controller stays in the loop and the
network's contribution is held within the residual envelope; there, a policy with
`UpdateWhileActive=yes` keeps its mode across a swap, so that online adaptation can go
on learning during the dive. The policy publishes the setting in each
`policy.<name>.active` message, from which the mux decides
([07-safety](07-safety.md#the-mux)). Each load still passes every check and the
warm-up, is recorded like any other, and stays under every other demotion rule.

## Training

The trainer runs on the topside as a module named `trainer`, started by the topside's
manager like any other, so that the policies' `Allow` lists name it `topside:trainer`:

```ini
# /etc/hangar/modules/trainer.module   (topside)
[Module]
Description=Trainer: depth feed-forward
Exec=/usr/bin/python3 /home/pilot/train/depth_ff.py
User=hg-trainer

[Subscribe dataset.depth_ff]
[Subscribe policy.depth_ff.active]
[Subscribe policy.depth_ff.load]

[Call bridge.send]
[Call policy_depth_ff.load_weights]
[Call policy_depth_ff.revert]
[Call policy_depth_ff.runtime_info]
```

`hangar.learn` takes the module's wiring and attaches to the manager as `hg_init`
does ([03-manager](03-manager.md#starting-a-module)), and makes its calls as `trainer`.
A script run from a shell has no module identity: it calls as `admin:<user>` if its
user is in `hangar-admin`, and reaches the vehicle as `topside:admin:<user>`, which
the policies' `Allow` lists name; any other user is refused
([12-tools](12-tools.md#the-python-package)).

```python
import time
from hangar import learn

stream = learn.stream("dataset.depth_ff", batch=32, seq_len=50)  # stateful chunks
model = DepthFF()                                    # your PyTorch module
h = None
deployed = time.monotonic()
for chunk in stream:
    h = learn.carry(h, chunk.fresh)                  # detached; zero where a slot starts a run
    out, h = model(chunk.inputs, h)
    loss = train_step(out, chunk.labels, chunk.mask) # your loss, masked
    if time.monotonic() - deployed >= 180:           # every 3 minutes
        learn.deploy(model, policy="depth_ff", dataset="depth_ff",
                     probes=stream.probes(8))
        deployed = time.monotonic()
```

| Function | |
|---|---|
| `stream(topic, batch, seq_len=None, replay="10min")` | rows from the live topic, through a replay buffer. With `seq_len`, stateful chunks for recurrent models (below) |
| `open_dataset(file, seq_len=None)` | the same over a built dataset file |
| `carry(state, fresh)` | the recurrent state, detached from the graph, zeroed for the slots marked fresh |
| `export(model, dataset, probes)` | the `.hgw` file: graph, tensors, bindings, probes, the input ranges and activation bound measured on the rows trained on, and lineage, checked against the vehicle's `runtime_info` |
| `deploy(model or file, policy)` | export, `bridge.send`, `load_weights`, and wait on `policy.<name>.load` and `policy.<name>.active` |
| `revert(policy)`, `watch(policy)` | |

**Stateful chunks.** With `seq_len`, each of the `batch` slots follows one run of rows
forward, chunk after chunk, and the user's code carries the recurrent state from one
chunk to the next, detached (truncated backpropagation through time). A chunk ends at a
row with `reset` set; the slot's next chunk starts from zero state (`chunk.fresh`), and
the rest of the short chunk is padding with no loss. This is the vehicle's own rule, so
the state a model meets in training is the state it carries on the vehicle. A row with
`masked` set (it failed the spec's `Where`) is stepped through like any other, and
carries no loss. `chunk.mask` is 0 on masked rows, on padding, and on label axes that
are not valid in the row ([06-recorder](06-recorder.md#the-spec)). A row lost on the
tether, or skipped by a trainer slower than the rows, also ends the chunk, as a fresh
start the vehicle did not make; `stream` counts them and reports the count.

**Replay.** The stream keeps the recent rows (by default the last 10 minutes) in a
buffer and feeds steps from it, so a step does not wait for 32 × 50 new rows. A slot
that reaches the newest row, or a `reset`, starts again at a point drawn from the
buffer, half of the draws from its last 2 minutes. A slot that starts inside a run,
rather than at a `reset`, runs its first chunk without loss, so that the state it
learns from has seen `seq_len` rows. Rows from earlier dives' recordings, through
`open_dataset`, may be mixed in; a change of vehicle boot is a `reset`.

Training itself is the user's own PyTorch code; the package moves rows in and weights
out, and records the lineage.

## Example: imitating the pilot

**Data.** `pilot_imitation` ([06-recorder](06-recorder.md#the-spec)): anchored on
`nav.state` at 50 Hz; inputs the state and the `cam.nn` frame shown to the pilot
(`asof:-120ms`); label `pilot.wrench`; filter parts `mux.state` and `ardusub.state`.
`Where` keeps the rows in which ArduSub was in MANUAL, and `Filter=mask`, since the
model is recurrent: other rows are stepped through without loss. Each axis of the label
counts only where the row's `mux.state.source_<axis>` is the pilot, so an axis a
controller held is never learned as the pilot's idle stick. The weights record
`autopilot_mode` MANUAL. Each row carries a 19 KB frame, about 17 datagrams, so the
mirror of `dataset.pilot_imitation` is `reliable` ([08-bridge](08-bridge.md#delivery)).

**Model.** A small CNN on `cam.nn` (three `conv2d`, `relu`, `maxpool2d` blocks, then
`global_avgpool` to 64 features), `concat` with the normalized state, a 2-layer `lstm`
of 64, and a `dense` to the six axes of the wrench, scaled by an `affine`.

**Training.** On the topside, from the live stream and from earlier dives' recordings
built with the same spec (the recorder keeps `cam.nn` in full); chunks of 50 ticks
(1 s), with the state carried across them.

**On the vehicle.** `policy_pilot` in shadow; `policy.pilot.eval` against the pilot,
axis by axis, dive after dive; promoted to `policy:pilot` with the evaluated model's
hash when the operator judges it. While it drives, the `mavlink` module keeps ArduSub in
the weights' `autopilot_mode` ([10-mavlink](10-mavlink.md#flight-modes)), with the
pilot's override on every axis and the envelope as always.

## Example: adaptive depth hold

**The idea.** The depth controller (`ctl.depth_hold`) is tuned for one buoyancy and one
drag; both change with payload, depth and tether. A network learns the heave thrust
needed for a wanted vertical motion, from what the thrusters were sent and how the
vehicle moved, and adds it as a feed-forward to the controller.

**Data.** Inputs the state (depth, vertical velocity, attitude) and the vertical
acceleration 200 ms later, which needs a substitution to run; label the heave that the
actuator gate sent to ArduSub, `ardusub.manual_control`'s `z`. In phase 1 the joystick
gain is pinned to 1 ([10-mavlink](10-mavlink.md)), and `Where` keeps the rows in which
outputs were enabled and ArduSub was in MANUAL, so the label is the thrust command that
reached the thrusters. Plain supervised learning of inverse dynamics.

```ini
# /etc/hangar/datasets/depth_ff.dataset
[Dataset]
Anchor=nav.state
AnchorRole=input
Lateness=0
Filter=mask
ResetAfter=200ms
# outputs enabled; ArduSub custom mode 19 is MANUAL
Where=safety.state.outputs_enabled == 1 and ardusub.state.mode == 19

# The vertical acceleration 200 ms later: substituted on the vehicle.
[Input future_state]
Topic=nav.state
Join=asof:+200ms
Fields=accel_z

# The heave sent to ArduSub: z from 0 to 1000, 500 neutral.
[Input ardusub.manual_control]
Join=asof:0
Role=label
Fields=z

[Input safety.state]
Join=asof:0
Role=filter

[Input ardusub.state]
Join=asof:0
Role=filter
```

The training code turns the label into the wrench's units, (z − 500) / 500. The spec
has no `mux.state` part: the label is the command the gate sent, whoever drove each
axis, so it is valid on every row `Where` keeps, and `hangar dataset check` passes it
without a warning ([12-tools](12-tools.md#datasets-and-analysis)). Its defaults,
`Missing=flag` and `Rate=every:1`, with `Filter=mask`, make it usable by the recurrent
`policy_depth_ff`, its one look-ahead join substituted.

**On the vehicle.** `policy_depth_ff`, with the measured future acceleration
substituted by the depth controller's wanted acceleration (`ctl.depth_hold.target`, a
topic the controller publishes beside `ctl.depth_hold`, which the policy subscribes to);
mode `residual:depth_ff`, the network's thrust within the residual envelope;
`UpdateWhileActive=yes`, so the trainer deploys a new model every 3 minutes during the
dive and the correction follows the vehicle as it changes.

## Tests

- Runtime: each op's golden vectors from PyTorch, for every kernel, on x86-64 and on
  ARM under emulation; LSTM of 1 and 2 layers over sequences, state kept between steps,
  reset; the exporter refusing a bidirectional LSTM and one with projection.
- Unknown op or version: refused with the reason, nothing loaded.
- Loading: each check failing in turn leaves the running model untouched; a model
  whose intermediate values exceed the header's activation bound, or turn non-finite,
  on its probes or on samples spanning its `ranges` refused at check 7; warm-up and
  swap with no tick missed, with both models at the edge of `TickBudget`; an overrun
  during warm-up stops the load; a second `load_weights` during a load refused;
  `revert`.
- Start: a policy with no weights is ready, publishes its flagged neutral output on
  every tick, and is never restarted; a restarted policy reloads `current.hgw` and
  `previous.hgw`, comes back in shadow, and `revert` still works.
- Non-finite: a model producing inf or NaN on one input: nothing published at that
  tick, state reset, counted on `eval`, the mux demotes.
- Features: the policy's input bytes equal the dataset row's input part for the same
  anchor, live, wherever neither side missed a message; a missing part is zeros with its
  bit, on both sides; a missing or stale input part flags the output; a spec with a
  waiting `input` join refused unless that part is substituted; substituted parts
  excluded from the comparison.
- Resets: on a recorded stream with gaps, stale parts, masked rows and a vehicle
  reboot, the rows' `reset` bits fall exactly at the ticks where the policy resets its
  state; `stream` ends chunks there and carries the state everywhere else.
- Lineage: from a recorded thruster command to the model, the weights (from the
  session), the rows and the messages, by identifiers alone, across a vehicle reboot;
  outputs computed during a load, and after a rejected one, name the running model.
- Names: a module running `hangar-policy` whose name is not `policy_<Name>`, or which
  lacks one of the four outputs, refused.
- Promotion: `mux.set_mode` with a hash other than the running model's refused; a swap
  in `policy:<name>` demotes; under `UpdateWhileActive=yes` in `residual:<name>`, it
  does not.
- The two examples end to end on recorded data, then on the bench with propellers off:
  stream, train, deploy, shadow, promote, demote.
