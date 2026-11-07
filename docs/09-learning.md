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
drive only within an envelope, and are demoted automatically when they misbehave.

**Version 1** of the loop is online adaptation in the residual mode alone: a network
learns, during the dive, a bounded correction to a classical controller, and the mux
adds it to the controller's output within the residual envelope
([07-safety](07-safety.md#the-mux)). It has one worked example,
[adaptive depth hold](#example-adaptive-depth-hold). It covers: the policy module and
its spec; the runtime with the ops `dense`, `lstm`, `affine`, `cast`, `relu`, `tanh`
and `sigmoid`; the weights file; the load checks (hashes, architecture, finite values,
probes, step time) and the warm-up; a finiteness check on every tick; promotion by
hash and automatic demotion; `revert`; reload at start; lineage across vehicle boots;
the topics `policy.<name>.active` and `policy.<name>.load`; and the trainer with its
Python package. A network that takes a whole axis, shadow evaluation on the vehicle,
substitution, convolutions and the imitation of a pilot are listed under
[Later](#later) and are not part of version 1.

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
  time a step takes, and probe outputs computed on the topside that the vehicle must
  reproduce.
- **Outputs are checked every tick.** A non-finite output is never published.
- **A policy drives nothing until an operator promotes it**, naming the model. Its
  output is published and recorded from its first tick; the mux applies it only in
  `residual:<name>`, and only within the residual envelope. In that mode each later
  model the policy swaps in drives from its swap, after its checks and warm-up; the mux
  demotes the policy automatically when its output misbehaves
  ([07-safety](07-safety.md#the-mux)).
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
   `policy_<name>.load_weights`: a request on its own `trainer.req` topic, which the
   bridge mirrors down; the reply comes back on `policy_<name>.rep`
   ([04-calls](04-calls.md#the-rules)).
5. The **policy module** checks the file, warms the new model up beside the running one,
   and swaps it in between two ticks. It reports the load's progress on
   `policy.<name>.load` and announces the swap on `policy.<name>.active`.
6. The policy's output is published and recorded. With the mux in `residual:<name>`,
   set by an operator with `mux.set_mode` naming the running model's hash, the new
   model drives from the swap, within the residual envelope.
7. Its **actions** are recorded and paired, and become new samples.

The loop needs, in both copies of the bridge's configuration, `dataset.<name>` mirrored
up, `policy.<name>.active` and `policy.<name>.load` (event topics, `reliable`) mirrored
up, and `policy_*.*` in `[Calls] Remote=`, which has the bridges mirror `trainer.req`
down and `policy_<name>.rep` up, reliably, at `call` priority
([08-bridge](08-bridge.md#calls-across-the-tether)): the trainer waits on the first
two, and the operator promotes on the first. `deploy` gives up after 60 s without a
`policy.<name>.load` message for its file, and says so.

## Policy modules

A policy is a module running the generic `hangar-policy` program with its own
manifest and policy spec. Each is its own module (`policy_depth_ff`), with its own
process, restarts and parameters.

```ini
# /etc/hangar/modules/policy_depth_ff.module
[Module]
Description=Depth hold feed-forward policy
Exec=/usr/lib/hangar/hangar-policy /etc/hangar/policies/depth_ff.policy
# writes the weights it swaps in into the recorder's session
SupplementaryGroups=hangar-rec

[Subscribe nav.state]
[Subscribe ctl.depth_hold.target]
[Subscribe recorder.session]

[Publish policy.depth_ff.wrench]
Type=wrench
Rate=50
Inputs=nav.state ctl.depth_hold.target policy.depth_ff.active

[Publish policy.depth_ff.active]
Type=policy_active

[Publish policy.depth_ff.load]
Type=policy_load

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

The manager adds the module's reply topic, `policy_depth_ff.rep`, and the subscriptions
through which its callers' requests reach it: `hangar.vehicle.admin.req`, and
`subglob *.req` for the topside's mirrored `trainer.req` and `pilot_console.req`
([03-manager](03-manager.md#wiring)). A request is a message the policy reads in its
loop like any other ([04-calls](04-calls.md#the-rules)).

The policy spec says what the policy runs on. It is part of the installation, like the
manifest, and cannot be changed from the topside:

```ini
# /etc/hangar/policies/depth_ff.policy
[Policy]
Name=depth_ff
Dataset=depth_ff
Arch=4be1c0d2a97f3e18
TickBudget=50%
```

| `[Policy]` key | Default | |
|---|---|---|
| `Name` | — (required) | The policy's name, `[a-z0-9_]`. The module is `policy_<name>`; it publishes `policy.<name>.wrench`, `.active` and `.load`; its mux mode is `residual:<name>`. The manager refuses a module that runs `hangar-policy` under another name or without those three outputs |
| `Dataset` | — (required) | The spec that builds the inputs, and whose `ResetAfter` resets recurrent state; it must be usable by a policy ([Features at run time](#features-at-run-time)) |
| `Arch` | — (required) | The architecture hash a weights file must have |
| `TickBudget` | `50%` | The largest share of the tick period the model steps of one tick may take together: the running model's step, plus during a warm-up the new model's ([Loading weights](#loading-weights)) |

A policy's topics:

| Topic | Kind | |
|---|---|---|
| `policy.<name>.wrench` | the anchor's rate | The output: one message per anchor message |
| `policy.<name>.active` | event | One message per swap or revert: the model now computing the output ([Loading weights](#loading-weights)) |
| `policy.<name>.load` | event | Progress of a load: checking, warming, done, rejected with the reason |

**With no running model** (a fresh installation, or a restart whose weights are
still being checked), the policy still publishes on every tick: a neutral wrench,
flagged as computed from stale inputs (slot flag bit 0), which the mux treats as no
contribution ([07-safety](07-safety.md#the-mux)). The module is therefore ready and
fresh, and is never restarted for lack of a model.

**At start** the policy reloads its weights from its state directory,
`/var/lib/hangar/policy_<name>/`: `current.hgw`, the model that was running, and
`previous.hgw`, kept for `revert`. Both go through checks 1 to 7; the current one is
warmed up and swapped in, and announced with cause `start`. A restarted policy always
comes back unapplied: the mux fell back to `assisted` when its output went stale, and
an operator promotes it again.

## Features at run time

The policy ticks on each message of the dataset's anchor. At each tick it builds the
row's `input` parts with the pairer's own join code: provenance by seq, `asof` by
stamp, windows by the preceding seqs. A spec qualifies only if no `input` join would
have to wait: every `input`-role `asof` join has `offset + Lateness ≤ 0`
([06-recorder](06-recorder.md#joins)), so the inputs are complete at the moment of the
anchor. The policy checks this at start, and `hangar dataset check` names the `input`
joins that would have to wait. The pairer, too, joins the `input` parts when it reads
the anchor, and waits only for `label` and `filter` parts. `label` and `filter` parts
are not built by the policy: it acts on every tick, whoever is driving.

A part that is missing (gone from its ring, or its topic absent) is filled with zeros
and its bit set in the row's `missing` field, identically in the row and in the
policy's inputs; a model may bind the missing bits and a part's `count` as inputs.
When any `input` part is missing or stale, the policy sets slot flag bit 0 on its
output itself, since the parts need not be among the output's `Inputs`
([01-topics](01-topics.md#stale-inputs)); the actuator gate and the mux then treat
that output as no command.

The input bytes a policy hands its model at a tick are therefore the same as the
input part of the dataset row for the same anchor message, except where one of the two
read a ring at a moment the other did not: a message gone from the ring on one side,
or one published between the two reads. Both cases are recorded, not guessed: a
missing part by its bit, and a message the pairer joined only after the policy's tick
by the row's `late` flag ([06-recorder](06-recorder.md#joins)). A test checks the
rest. A model never meets features at run time that differ from those it was trained
on.

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
| `lstm` | input size, hidden size, layers | [LSTM](#lstm) |
| `affine` | per-channel scale and shift | input normalization, folded batchnorm, output scaling |
| `relu`, `tanh`, `sigmoid` | | |
| `cast` | from u8/i8/u16/i16/u32/i32/f64 to f32, scale | raw row fields to f32 |

Convolutions, pooling, `concat` and the other shape ops come later
([Later](#later)). A weights file using an op or version this runtime does not have is
refused at load, naming it (`op conv2d v1 not supported`). `policy_<name>.runtime_info`
lists the ops and versions the vehicle has, and the exporter checks against it before
anything is sent.

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
counted in the module's status line (`hangar top`), and the mux demotes the policy at
once, since its output skipped a tick ([07-safety](07-safety.md#the-mux)).

## The weights file (`.hgw`)

Self-describing, mapped into memory and read in place:

```
[ header: 4096 bytes ][ section directory ][ sections ][ tensor data, 64-byte aligned ]
```

**Header**: `magic` (`"HGWEIGHT"`), format version, the architecture hash, the model's
name, the tick period it was trained at, `warmup` ticks, the probe tolerance (relative
and absolute), creation time, the trainer's description (program, version, a hash of
its settings), and the summary of its **lineage**:

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
| lineage | one entry per source of rows, any number: vehicle boot id (u8[16]), recorder session id or `live`, first and last `anchor_seq` and stamp, rows used from it |

A reader skips a section whose name it does not know, so sections can be added
([Later](#later)) without a new format version.

The **architecture hash** covers the ops, their attributes, the shapes, and the input
and output bindings: everything but the tensor values and the lineage. Training changes
the weights and keeps the hash; changing the network changes it, and the policy refuses
the file.

Input normalization and output scaling are `affine` ops inside the model, so they
travel with the weights and cannot drift from them.

## Loading weights

`policy_<name>.load_weights {name, hash}`, naming a file the bridge received
([08-bridge](08-bridge.md#files)). The reply comes at once (`accepted`, with the
version it will get, or refused while another load is in progress); the work happens
beside the tick path, on a worker thread of the policy module running at the lowest
priority (`SCHED_IDLE`), so that it never delays a tick. Its progress is on
`policy.<name>.load`:

1. the file's hash matches the request's;
2. the format version, and the architecture hash equal to the policy's `Arch`;
3. every op and version is in this runtime, and every op's `check` passes;
4. the lineage's dataset is the policy's `Dataset`, with the same spec hash; the input
   bindings are fields of the dataset's `input` parts, and the output bindings fields
   of the output type; for a recurrent model, the spec has `Filter=mask`,
   `Missing=flag` and `Rate=every:1`;
5. every tensor value is finite;
6. the probes, from zero state, reproduce the expected outputs at every step within the
   tolerance;
7. the new model's step time, measured on the probes before warm-up, while the tick
   thread is waiting for its next anchor, plus the running model's, is under
   `TickBudget` of the tick period;
8. warm-up beside the running model, then the swap, between two ticks.

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
the encoder does), so that the recording holds every model that ran.

The previous model is kept in memory; `revert` swaps back to it at the next tick, with
its recurrent state reset, publishes `active` with cause `revert`, and exchanges the
two files.

`policy.<name>.active`, an event topic:

```
u32  version              # counts up per swap on this vehicle
u8   cause                # 0 start (current.hgw reloaded), 1 load, 2 revert
u8   reserved[3]
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
u8   check            # with rejected: the check that failed, 1 to 8
u8   reserved[2]
u64  hash
u64  since_ns
char reason[64]
```

## Promotion

The policy's output is published and recorded from its first tick, and the mux applies
it only in `residual:<name>`, which only `mux.set_mode` sets. That call is the
promotion: it carries the hash of the model the operator judged, and the mux refuses
it unless that is the hash of the policy's running model, as the newest
`policy.<name>.active` message announces it ([07-safety](07-safety.md#the-mux)).

In `residual:<name>` the controller stays in the loop and the network's contribution
is held within the residual envelope. There, the mode follows the policy's swaps: when
a `policy.<name>.wrench` message references a new `policy.<name>.active` message (a
load, or a revert), the mux takes that model as the promoted one, so that online
adaptation can keep learning during the dive. Every load has passed the checks and the
warm-up, is recorded like any other, and stays under every demotion rule: the mux
falls back to `assisted`, and says why, when the output goes stale, skips a tick, is
flagged as computed from stale inputs, or is held at an envelope limit for too long.
It never promotes on its own.

In version 1 the operator judges a model before promoting it from the trainer's own
validation and from the recorded output beside the controller's
(`hangar echo policy.<name>.wrench`); a comparison computed on the vehicle is
[Later](#later).

## Lineage

- Each `policy.<name>.wrench` message has, in its provenance, the anchor message it was
  computed at and the `policy.<name>.active` message of the model that computed it.
- That `active` message names the weights' hash; the weights file, kept in the
  recording, names the dataset, and per vehicle boot and session the row ranges it was
  trained on. For a load or a revert, the `active` message also references the request
  message that asked for it, on the caller's `.req` topic, and the policy's reply on
  `policy_<name>.rep` references the same request ([04-calls](04-calls.md#the-rules)).
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

# Methods of the other side carry the types the caller was built with.
[Call policy_depth_ff.load_weights]
Request=load_weights_req
Reply=load_weights_rep

[Call policy_depth_ff.revert]
Request=empty
Reply=policy_version

[Call policy_depth_ff.runtime_info]
Request=empty
Reply=runtime_info
```

The manager creates the trainer's request topic, `trainer.req`, and subscribes the
module to `bridge.rep` and to `policy_depth_ff.rep`, the mirrored reply topic of the
vehicle's policy ([04-calls](04-calls.md#the-rules)). `hangar.learn` reads the
module's wiring from `HANGAR_WIRING` as `hg_init` does
([03-manager](03-manager.md#starting-a-module)), opens `trainer.req` for writing
through the manager, and publishes its requests there, through the package's compiled
helper; the writer of that topic is the caller, so its calls are `trainer`'s and reach
the vehicle as `topside:trainer`. A script run from a shell has no request topic of its
own: the package hands its calls to the manager, as `hangar call` does, which publishes
them as `admin:<user>` if its user is in `hangar-admin`; they reach the vehicle as
`topside:admin:<user>`, which the policies' `Allow` lists name; any other user is
refused ([12-tools](12-tools.md#the-python-package)).

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
| `export(model, dataset, probes)` | the `.hgw` file: graph, tensors, bindings, probes and lineage, checked against the vehicle's `runtime_info` |
| `deploy(model or file, policy)` | export, `bridge.send`, `load_weights`, and wait on `policy.<name>.load` and `policy.<name>.active` |
| `revert(policy)`, `watch(policy)` | |

**Stateful chunks.** With `seq_len`, each of the `batch` slots follows one run of rows
forward, chunk after chunk, and the training script carries the recurrent state from one
chunk to the next, detached (truncated backpropagation through time). A chunk ends at a
row with `reset` set; the slot's next chunk starts from zero state (`chunk.fresh`), and
the rest of the short chunk is padding with no loss. This is the vehicle's own rule, so
the state a model meets in training is the state it carries on the vehicle. A row with
`masked` set (it failed the spec's `Where`) is stepped through like any other, and
carries no loss. `chunk.mask` is 0 on masked rows and on padding
([06-recorder](06-recorder.md#the-spec)). A row lost on the tether, or skipped by a
trainer slower than the rows, also ends the chunk, as a fresh start the vehicle did not
make; `stream` counts them and reports the count.

**Replay.** The stream keeps the recent rows (by default the last 10 minutes) in a
buffer and feeds steps from it, so a step does not wait for 32 × 50 new rows. A slot
that reaches the newest row, or a `reset`, starts again at a point drawn from the
buffer, half of the draws from its last 2 minutes. A slot that starts inside a run,
rather than at a `reset`, runs its first chunk without loss, so that the state it
learns from has seen `seq_len` rows. Rows from earlier dives' recordings, through
`open_dataset`, may be mixed in; a change of vehicle boot is a `reset`.

Training itself is the script's own PyTorch code; the package moves rows in and weights
out, and records the lineage.

## Example: adaptive depth hold

**The idea.** The depth controller (`ctl.depth_hold`) is tuned for one buoyancy and one
drag; both change with payload, depth and tether. And in phase 1 the heave it commands
is not the heave the thrusters apply: ArduSub's mixer scales every axis down by the
largest input, so a heave command sent beside a large surge or yaw arrives smaller
([10-mavlink](10-mavlink.md#commands-to-ardusub)). A network learns, during the dive,
the heave the controller's command was short by, at each state and for each vertical
acceleration the controller wanted, and the mux adds it to the controller's output as a
residual, within the residual envelope ([07-safety](07-safety.md#the-mux)). It learns
the correction, not the whole heave: a network that learned the whole heave
feed-forward and ran as a residual would sit at the residual envelope's cap.

**Data.** Inputs: the state (depth, vertical velocity, pitch, roll) and the vertical
acceleration the controller wants, `ctl.depth_hold.target`'s `accel_z`, which the
policy has at its tick as the recording has it, so nothing stands in for anything.
Labels: the controller's own heave command at that tick, `ctl.depth_hold`'s `heave`,
and the pulse widths the thrusters were actually sent, `thr.out` (from
`SERVO_OUTPUT_RAW`, after ArduSub's mixing, [10-mavlink](10-mavlink.md#telemetry)),
both joined at the anchor. The training code maps `thr.out` through the vehicle's
thrust curve and thruster allocation to the heave thrust applied, in the wrench's units,
and subtracts `ctl.depth_hold.heave`: the label is what had to be added to the
controller's command, which is what residual mode adds. `Where` keeps the rows in which
outputs were enabled and the vehicle was vertically still: on those rows what was
applied is what held it, whoever supplied the difference. Plain supervised learning; no
join looks ahead, so the spec is usable by a policy as it stands, and no substitution
is needed.

```ini
# /etc/hangar/datasets/depth_ff.dataset
[Dataset]
Anchor=nav.state
AnchorRole=input
Fields=depth vel_z pitch roll
Lateness=0
Filter=mask
ResetAfter=200ms
# Outputs enabled, and the vehicle vertically still (m/s): what was applied held it.
Where=safety.state.outputs_enabled == 1 and nav.state.vel_z > -0.05 and nav.state.vel_z < 0.05

# The vertical acceleration the controller wants: an input the policy has at its tick.
[Input ctl.depth_hold.target]
Join=asof:0
Fields=accel_z

# The controller's own heave command at this tick.
[Input ctl.depth_hold]
Join=asof:0
Role=label
Fields=heave

# The pulse widths the thrusters were sent, after ArduSub's mixing.
[Input thr.out]
Join=asof:0
Role=label

[Input safety.state]
Join=asof:0
Role=filter
Fields=outputs_enabled
```

The spec has no `mux.state` part: the labels are what the thrusters were sent and
what the controller commanded, whoever drove each axis, so they are valid on every row
`Where` keeps. Its defaults, `Missing=flag` and `Rate=every:1`, with `Filter=mask`,
make it usable by the recurrent `policy_depth_ff`: rows in which the vehicle was moving
are stepped through and carry no loss. In `assisted` the label is the pilot's trim and
the mixer's shortfall; in `residual:depth_ff` it includes the residual that was
applied, so a model that holds the vehicle reproduces its own correction, and one that
does not sees what was still added by hand.

**The residual envelope** is a parameter of the mux, by default 20 % of the envelope
on each axis ([07-safety](07-safety.md#the-mux)), and is set from recorded data: the
distribution of this label over a dive, the fraction of heave the controller was short
by, says how wide the envelope must be for the correction to fit. A policy sitting at
the cap shows in `mux.state`, which names the limits acting on each axis, and the mux
demotes it after `demote_saturation`; a share of saturated ticks per model
(`policy.<name>.eval`) comes later ([Later](#later)).

**Model.** An `affine` that normalizes the five inputs, a 1-layer `lstm` of 16, whose
memory carries what it saw at nearby depths across the masked ticks, a `dense` to one
output, and an `affine` that scales it; the output is bound to the heave field of the
wrench type, the other axes 0.

**Training.** On the topside, from the live stream and from earlier dives' recordings
built with the same spec; the label computed from the two label parts by the training
code, with the vehicle's thrust curve and allocation from its configuration; chunks of
50 ticks (1 s), with the state carried across them; a deploy every 3 minutes.

**On the vehicle.** `policy_depth_ff`, ticking on `nav.state`, with
`ctl.depth_hold.target` as its second input part; mode `residual:depth_ff`, set once by
the operator with the first model's hash, the network's heave within the residual
envelope; each model the trainer deploys after that drives from its swap, and the
correction follows the vehicle as it changes.

## Later

What version 1 leaves out, each a step that changes nothing already built:

- **`policy:<name>`**, the mux mode in which the network has a whole axis, with the
  pilot's override and the envelope as always. There a swap always demotes the policy,
  and the new model must be promoted again; an `UpdateWhileActive` key in the policy
  spec, published in `policy.<name>.active`, would say whether a swap keeps the mode.
  In version 1 the rule is fixed: `residual:<name>` follows every swap.
- **Shadow evaluation on the vehicle**: a 1 Hz topic `policy.<name>.eval` comparing the
  policy's output with the `ctl.wrench` applied at the same tick, per axis and by who
  drove it (from `mux.state`), with the share of saturated ticks and the non-finite
  count, the window starting over at every swap, mirrored up for the operator to
  promote on.
- **Substitution**: a `[Substitute <part>]` section in the policy spec, naming a topic
  the policy subscribes to that stands in, at run time, for an `input` join that looks
  ahead. A model is trained on something measured afterwards and run on what is wanted
  instead: an inverse-dynamics depth feed-forward trained on the vertical acceleration
  200 ms after the anchor (`asof:+200ms` of `nav.state`) and run on the acceleration
  the controller wants (`ctl.depth_hold.target`). The substitute must have the same
  fields and types as the part it replaces, is excluded from the identity test, and is
  reported, so nobody mistakes it for the general case.
- **Ops** `conv2d`, `maxpool2d`, `avgpool2d`, `global_avgpool`, `add`, `concat`,
  `flatten` and `reshape`, and with them **imitation of the pilot**: a dataset anchored
  on `nav.state` with the `cam.nn` frame the pilot was looking at (`asof:-120ms`) as an
  input, `pilot.wrench` as the label and `mux.state` as a filter part; a small CNN,
  `concat` with the state, an `lstm` and a `dense` to the six axes; rows carrying a
  frame mirrored `reliable`. Training uses every recorded session and the live stream,
  not a recent window, and the pilot's interventions, the ticks on which `mux.state`
  says the pilot took an axis from the policy, are the label (HG-DAgger).
- **Per-axis label masks**: `chunk.mask` 0 on the label axes `mux.state` says the pilot
  did not drive, so an axis a controller held is never learned as the pilot's idle
  stick; `hangar dataset check` warns of a wrench label without a `mux.state` part.
- **An activation bound**: a header field and a `ranges` section (per input field, the
  smallest and largest value in the rows trained on), and a load check that runs the
  probes and samples spanning the ranges and refuses a model whose intermediate values
  overflow on an input it may meet, rather than at its first bad tick.

## Tests

- Runtime: each op's golden vectors from PyTorch, for every kernel, on x86-64 and on
  ARM under emulation; LSTM of 1 and 2 layers over sequences, state kept between steps,
  reset; the exporter refusing a bidirectional LSTM and one with projection.
- Unknown op or version (`conv2d` among them): refused with the reason, nothing loaded;
  an unknown section skipped.
- Loading: each check failing in turn leaves the running model untouched; warm-up and
  swap with no tick missed, with both models at the edge of `TickBudget`; an overrun
  during warm-up stops the load; a second `load_weights` during a load refused;
  `revert`.
- Start: a policy with no weights is ready, publishes its flagged neutral output on
  every tick, and is never restarted; a restarted policy reloads `current.hgw` and
  `previous.hgw`, comes back unapplied, and `revert` still works.
- Non-finite: a model producing inf or NaN on one input: nothing published at that
  tick, state reset, counted in the status line, the mux demotes.
- Features: the policy's input bytes equal the dataset row's input part for the same
  anchor, live, wherever neither side missed a message and the row is not flagged
  `late`; a missing part is zeros with its bit, on both sides; a missing or stale input
  part flags the output; a spec with a waiting `input` join refused by the policy and
  named by `hangar dataset check`.
- Resets: on a recorded stream with gaps, stale parts, masked rows and a vehicle
  reboot, the rows' `reset` bits fall exactly at the ticks where the policy resets its
  state; `stream` ends chunks there and carries the state everywhere else.
- Lineage: from a recorded thruster command to the model, the weights (from the
  session), the rows and the messages, by identifiers alone, across a vehicle reboot;
  outputs computed during a load, and after a rejected one, name the running model;
  an `active` message with cause `load` and the reply on `policy_<name>.rep` both
  reference the `load_weights` request message.
- Calls: `load_weights` from the trainer arrives through the mirrored `trainer.req` as
  `topside:trainer`, from a shell as `topside:admin:<user>`, and from a module not in
  `Allow` is denied; a request whose deadline passed during a policy restart is dropped
  unexecuted; `deploy` reports a reply that never comes as a timeout and gives up after
  60 s without a `load` message.
- Names: a module running `hangar-policy` whose name is not `policy_<Name>`, or which
  lacks one of the three outputs, refused.
- Promotion: `mux.set_mode` with a hash other than the running model's refused; in
  `residual:<name>`, a load and a revert are followed without demotion; a stale,
  skipped or flagged output demotes.
- The example's label: on recorded rows with no residual applied and no other axis
  commanded, `thr.out` mapped through the thrust curve and allocation equals
  `ctl.depth_hold.heave` within the curve's tolerance, so the label is zero; with a
  surge command beside the heave, the label is the mixer's shortfall; with a residual
  applied, the label is that residual.
- The example end to end on recorded data, then on the bench with propellers off:
  stream, train, deploy, promote, a second deploy applied at its swap, revert, demote;
  the envelope set from the recorded label's distribution, and a model that would sit at
  the cap seen in `mux.state` and demoted.
