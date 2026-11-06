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
run in shadow before they drive, drive only within an envelope, and are demoted
automatically when they misbehave.

Two kinds of learning run on it, with one worked example each:

- **imitation**: a policy learns to do what the pilot did
  ([Example: imitating the pilot](#example-imitating-the-pilot));
- **online adaptation**: a network learns, during the dive, a correction to a
  classical controller as the vehicle's dynamics change
  ([Example: adaptive depth hold](#example-adaptive-depth-hold)).

## The rules

- **Only weights change during a dive, never code.** A policy's architecture is fixed
  by its manifest and installed like any module; the loop replaces its weights.
- **A model sees at run time the features it was trained on.** The policy builds its
  inputs with the same dataset spec, and the same join code, that built its training
  rows.
- **Weights are checked before they run**: hashes, architecture, finite values, the
  time a step takes, and probe outputs computed on the topside that the vehicle must
  reproduce.
- **A new model runs in shadow** and drives only when an operator promotes it; it is
  demoted automatically ([07-safety](07-safety.md#the-mux)).
- **The previous weights are kept**, and `revert` restores them at once.
- **Lineage is complete**: every action names the model that produced it, and every
  model names the rows it was trained on.
- **The runtime grows by adding ops**, without changing the file format or anything
  that already works.

## The loop, step by step

1. The **pairer** on the vehicle publishes `dataset.<name>`, one row per anchor message
   ([06-recorder](06-recorder.md#datasets)); the bridge mirrors it to the topside at
   `dataset` priority ([08-bridge](08-bridge.md#delivery)).
2. The **trainer** on the topside GPU reads the rows live and trains continuously.
3. From time to time it **exports** a weights file (`.hgw`), with the lineage of the
   rows it was trained on and a set of probes.
4. It **sends** the file down (`bridge.send`, at `bulk` priority) and calls
   `policy_<name>.load_weights`.
5. The **policy module** checks the file, warms the new model up beside the running one,
   and swaps it in between two ticks. It publishes what it loaded on
   `policy.<name>.model`.
6. The policy's output runs in **shadow**, compared live against whatever is driving
   (`policy.<name>.eval`), until an operator promotes it with `mux.set_mode`.
7. Its **actions** are recorded and paired, and become new samples.

## Policy modules

A policy is a module running the generic `hangar-policy` program with its own
manifest and policy spec. Each is its own module (`policy_pilot`, `policy_depth_ff`),
with its own process, restarts and parameters.

```ini
# /etc/hangar/modules/policy_pilot.module
[Module]
Description=Pilot imitation policy
Exec=/usr/lib/hangar/hangar-policy /etc/hangar/policies/pilot.policy

[Subscribe nav.state]
[Subscribe cam.nn]
[Subscribe ctl.wrench]
[Subscribe mux.state]

[Publish policy.pilot.wrench]
Type=wrench
Rate=50
Inputs=nav.state policy.pilot.model

[Publish policy.pilot.model]
Type=policy_model

[Publish policy.pilot.eval]
Type=policy_eval
Rate=1

[Serve load_weights]
Request=load_weights_req
Reply=load_weights_rep
Allow=topside:trainer topside:pilot_console

[Serve revert]
Request=empty
Reply=policy_version
Allow=topside:pilot_console

[Schedule]
CPUs=2
```

The policy spec says what the policy runs on. It is part of the installation, like the
manifest, and cannot be changed from the topside:

```ini
# /etc/hangar/policies/pilot.policy
[Policy]
Dataset=pilot_imitation
Arch=4be1c0d2a97f3e18
Output=policy.pilot.wrench
TickBudget=50%
ResetAfter=200ms
UpdateWhileActive=no
```

| `[Policy]` key | Default | |
|---|---|---|
| `Dataset` | — (required) | The spec that builds the inputs; it must be usable by a policy ([06-recorder](06-recorder.md#joins)) |
| `Arch` | — (required) | The architecture hash a weights file must have |
| `Output` | — (required) | The topic the policy publishes, one message per tick |
| `TickBudget` | `50%` | The largest share of the tick period one step may take, measured at load |
| `ResetAfter` | `200ms` | An input stale or interrupted this long resets recurrent state |
| `UpdateWhileActive` | `no` | Whether new weights may replace a model in `residual` mode without demotion ([Updates while active](#updates-while-active)) |

## Features at run time

The policy ticks on each message of the dataset's anchor. At each tick it builds the
row's `input` parts with the pairer's own join code: provenance by seq, `asof` by
stamp, windows by the preceding seqs. A spec qualifies only if no `input` join would
have to wait ([06-recorder](06-recorder.md#joins)), so the inputs are complete at the
moment of the anchor. `label` and `filter` parts are not built: the policy acts on
every tick, whoever is driving.

The input bytes a policy hands its model at a tick are therefore byte-identical to the
input part of the dataset row for the same anchor message, which a test checks. A
model never meets features at run time that differ from those it was trained on.

**One deliberate exception: substitution.** A model may be trained on something
measured afterwards and run on what is wanted instead: trained on the motion that
followed a thrust, run on the motion the controller wants. The policy spec names the
substitution explicitly:

```ini
[Substitute future_state]
Topic=ctl.depth_hold.target
Fields=accel_z
```

The part `future_state` is the dataset's `asof:+200ms` join of `nav.state`. Every
other `input` join must still qualify; a substituted part is exempt, since the policy
does not build it. The substitute must have the same fields and types as the part it
replaces, and
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

The semantics of PyTorch's `nn.LSTM`, so that the exporter maps a trained module
directly:

- gates in PyTorch's order (input, forget, cell, output), with `W_ih` (4H × I),
  `W_hh` (4H × H), and both biases, per layer; stacked layers feed each the hidden
  state of the one below;
- **one time step per tick**: the hidden and cell state of every layer are kept between
  ticks (`state_bytes` = 2 × layers × H floats), so the network's memory replaces an
  input window;
- **the step rate is part of the model**: the weights file records the tick period it
  was trained at, and a policy whose anchor's declared rate differs refuses it.

**State resets** (to zero) on:

- an input missing or stale beyond `ResetAfter`, or a gap in the anchor's sequence
  numbers: the conditions under which the trainer, reading the same row sequence
  numbers, also breaks its sequences ([Training](#training));
- `policy_<name>.reset_state`;
- `revert`.

**Warm-up on a new model.** A new model's recurrent state means nothing until it has
seen some inputs. After its checks pass, the new model runs beside the current one on
the live inputs for `warmup` ticks (from the weights file; by default the sequence
length it was trained on) while the current one keeps producing the output; then the
outputs switch. A policy with no model yet starts from zero state and publishes nothing
until its warm-up is done.

### Numerics

`f32` throughout, with the C library's `exp` and `tanh`. Results differ from the
topside's in the last bits, and across processors; probes are compared within the
tolerance in the weights file, never bit for bit.

## The weights file (`.hgw`)

Self-describing, mapped into memory and read in place:

```
[ header: 4096 bytes ][ section directory ][ sections ][ tensor data, 64-byte aligned ]
```

**Header**: `magic` (`"HGWEIGHT"`), format version, the architecture hash, the model's
name, the tick period it was trained at, `warmup` ticks, the probe tolerance (relative
and absolute), creation time, the trainer's description (program, version, a hash of
its settings), and the **lineage**:

| Lineage field | |
|---|---|
| `dataset` | name, and the hash of the spec |
| `sessions[8]` | the recorder sessions, or `live`, the rows came from |
| `ranges[8]` | per session: the first and last `anchor_seq` and stamp trained on |
| `rows` | rows used |
| `steps` | training steps |
| `parent` | hash of the weights this training started from, if any |

**Sections:**

| Section | Records |
|---|---|
| ops | per op: type, version, attributes, input buffers, output buffer, tensors |
| tensors | per tensor: name, element type (`f32`, `i8`), shape (up to 4 dimensions), offset, size |
| buffers | per buffer: name, shape |
| inputs | per model input: its buffer, and the row fields it is bound to, in order |
| outputs | the output buffer, and the fields of the output topic's type it fills, in order |
| probes | input sequences from zero state, and the expected output at every step |

The **architecture hash** covers the ops, their attributes, the shapes, and the input
and output bindings: everything but the tensor values and the lineage. Training changes
the weights and keeps the hash; changing the network changes it, and the policy refuses
the file.

Input normalization and output scaling are `affine` ops inside the model, so they
travel with the weights and cannot drift from them.

## Loading weights

`policy_<name>.load_weights {name, hash}`, naming a file the bridge received
([08-bridge](08-bridge.md#files)). The call answers at once (`accepted`, with the
version it will get); the work happens beside the tick path, on a worker thread of the
policy module, and its progress is on `policy.<name>.model`:

1. the file's hash matches the call's;
2. the format version, and the architecture hash equal to the policy's `Arch`;
3. every op and version is in this runtime, and every op's `check` passes;
4. the input bindings are fields of the dataset's `input` parts, and the output
   bindings fields of the output type;
5. every tensor value is finite;
6. the probes, from zero state, reproduce the expected outputs at every step within the
   tolerance;
7. one step takes under `TickBudget` of the tick period, measured on the probes;
8. warm-up beside the running model, then the swap, between two ticks.

A failed check stops the load: the running model is untouched, and
`policy.<name>.model` says which check failed and why. The previous model is kept;
`revert` swaps back to it at the next tick, with its recurrent state reset.

`policy.<name>.model`, an event topic:

```
u32  version          # counts up per load on this vehicle
u8   state            # 0 none, 1 checking, 2 warming, 3 running, 4 rejected, 5 reverted
u8   reserved[3]
u64  hash
u64  arch
u64  loaded_ns
char dataset[32]
u64  newest_row_stamp_ns   # the newest row it was trained on
char reason[64]
```

## Lineage

- Each `policy.<name>.wrench` message has, in its provenance, the anchor message it was
  computed at and the `policy.<name>.model` message of the model that computed it.
- That model message names the weights' hash, and the weights file names the dataset,
  sessions and row ranges it was trained on.
- Each row names, by `(topic, seq)`, the messages it was made from; each of those, its
  own inputs.

So the recording answers, for any thruster command: which model produced it, which rows
trained that model, and which sensor readings and pilot commands made those rows.
Nothing is reconstructed from timestamps; every link is an identifier.

**Loop latency.** For each model, the time from the newest row it was trained on
(`newest_row_stamp_ns`) to its first output while promoted: how long the loop takes to
turn an experience into an action. `hangar dataset loop-latency <session>` reports it
per model; `policy.<name>.model` gives the part up to the load live.

## Shadow evaluation

While the policy runs, it compares its output with what actually drove: the
`ctl.wrench` applied at the same tick, and from `mux.state` who produced it. It
publishes, once a second, `policy.<name>.eval`:

| Field | |
|---|---|
| `driver` | who drove during the window (pilot, controller, this policy) |
| `error[6]` | mean absolute difference per axis, over the last 10 s |
| `saturated` | the share of ticks at which the mux would have held the output at an envelope limit (from the mux's parameters on `mux.params`) |
| `substituted` | whether the policy runs on a substitution ([Features at run time](#features-at-run-time)) |
| `model_version` | |

The operator promotes on that evidence; the mux demotes on its own evidence
([07-safety](07-safety.md#the-mux)).

## Updates while active

In `policy:<name>` the network has the whole axis: a new model is always demoted and
promoted again. In `residual:<name>` the controller stays in the loop and the network's
contribution is held within the residual envelope; there, a policy with
`UpdateWhileActive=yes` keeps its mode across a load, so that online adaptation can go
on learning during the dive. Each load still passes every check and the warm-up, and is
recorded like any other.

## Training

The topside package, `hangar.learn`:

```python
from hangar import learn

stream = learn.stream("dataset.depth_ff", seq_len=50, batch=32)  # runs of rows, broken at gaps
model = DepthFF()                                                # your PyTorch module
for step, batch in enumerate(stream):
    loss = train_step(model, batch)                              # your training code
    if step % 2000 == 0:
        learn.deploy(model, policy="policy_depth_ff",
                     dataset="depth_ff", probes=stream.probes(8))
```

| Function | |
|---|---|
| `stream(topic, batch, seq_len=None)` | rows from the live topic; with `seq_len`, runs of consecutive rows (by `anchor_seq`) for recurrent models, broken where a row is missing or filtered out, as the vehicle resets its state |
| `open_dataset(file, seq_len=None)` | the same over a built dataset file |
| `export(model, dataset, probes)` | the `.hgw` file: graph, tensors, bindings, probes and lineage, checked against the vehicle's `runtime_info` |
| `deploy(model or file, policy)` | export, `bridge.send`, `load_weights`, and wait on `policy.<name>.model` |
| `revert(policy)`, `watch(policy)` | |

Training itself is the user's own PyTorch code; the package moves rows in and weights
out, and records the lineage.

## Example: imitating the pilot

**Data.** `pilot_imitation` ([06-recorder](06-recorder.md#the-spec)): anchored on
`nav.state` at 50 Hz; inputs the state and the `cam.nn` frame shown to the pilot
(`asof:-120ms`); label `pilot.wrench`; kept only while the pilot drove.

**Model.** A small CNN on `cam.nn` (three `conv2d`, `relu`, `maxpool2d` blocks, then
`global_avgpool` to 64 features), `concat` with the normalized state, a 2-layer `lstm`
of 64, and a `dense` to the six axes of the wrench, scaled by an `affine`.

**Training.** On the topside, from the live stream and from earlier dives' recordings
built with the same spec; sequences of 50 ticks (1 s).

**On the vehicle.** `policy_pilot` in shadow; `policy.pilot.eval` against the pilot,
dive after dive; promoted to `policy:pilot` when the operator judges it, with the pilot's
override on every axis and the envelope as always.

## Example: adaptive depth hold

**The idea.** The depth controller (`ctl.depth_hold`) is tuned for one buoyancy and one
drag; both change with payload, depth and tether. A network learns the heave thrust
needed for a wanted vertical motion, from what the thrusters did and how the vehicle
moved, and adds it as a feed-forward to the controller.

**Data.** `depth_ff`: anchored on `nav.state` at 50 Hz; inputs the state (depth,
vertical velocity, attitude) and the vertical acceleration 200 ms later
(the part `future_state`: `Topic=nav.state`, `Join=asof:+200ms`, `Role=input`, which
needs a substitution to run, below); label the heave thrust applied (`ctl.wrench`, `asof:0`).
Plain supervised learning of inverse dynamics.

**On the vehicle.** `policy_depth_ff`, with the measured future acceleration
substituted by the depth controller's wanted acceleration (`ctl.depth_hold.target`);
mode `residual:depth_ff`, the network's thrust within the residual envelope;
`UpdateWhileActive=yes`, so the trainer deploys a new model every few minutes during the
dive and the correction follows the vehicle as it changes.

## Tests

- Runtime: each op's golden vectors from PyTorch, for every kernel, on x86-64 and on
  ARM under emulation; LSTM over sequences, state kept between steps, reset.
- Unknown op or version: refused with the reason, nothing loaded.
- Loading: each check failing in turn leaves the running model untouched; warm-up and
  swap with no tick missed; `revert`.
- Features: the policy's input bytes equal the dataset row's input part for the same
  anchor, live; a spec with a waiting `input` join refused.
- Lineage: from a recorded thruster command to the model, the weights, the rows and the
  messages, by identifiers alone.
- The two examples end to end on recorded data, then on the bench with propellers off:
  stream, train, deploy, shadow, promote, demote.
