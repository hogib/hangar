# Recorder, provenance and datasets

What the vehicle did, kept as data a network can learn from, both after the dive and
during it. Two modules and one file format:

- the **recorder** writes topics to disk, each topic a file of fixed-size records;
- the **pairer** joins outputs to the inputs they were computed from and publishes
  the result live, as a dataset topic that a trainer reads while the vehicle runs;
- a **dataset spec** says what a sample is, and produces the same rows live from
  topics and offline from recordings.

Training on the fly, and deploying what was trained back onto the vehicle, is the
learning loop ([09-learning](09-learning.md)); this doc provides its data.

## The rules

- **Nothing is recorded or paired at a producer's expense.** Both modules are readers;
  a writer never waits for them.
- **Losses are recorded as losses.** A message the recorder could not keep is a gap in
  the file; an input the pairer could not find is flagged missing in the row. A
  dataset never claims more than it has.
- **A sample is defined once.** The same dataset spec drives the live pairer, the
  offline build, and the inputs of a policy running on the vehicle: rows are
  byte-identical live and offline, and a model sees at run time the features it was
  trained on.
- **Every file is an array.** Records and rows are fixed-size, so a file opens with
  `numpy.memmap` and nothing is parsed.
- **A session describes itself**: the wiring graph, parameters, software versions and
  clocks travel with the data.

## The recorder

An ordinary module, `recorder`, which reads topics in order (`hg_next`) and appends
them to files.

### What it records

The recorder subscribes with wildcards, and takes topics as they appear:

```ini
# recorder.module
[Subscribe *]

[Record cam.*]
Policy=meta

[Record nav.imu]
Policy=full

[Record *]
Policy=full
```

`[Record <glob>]` sections are matched in order; the first that matches a topic sets
its policy.

| Policy | Records |
|---|---|
| `full` | every message |
| `every:N` | every Nth message |
| `rate:<Hz>` | at most that rate |
| `meta` | every message's header and provenance, without the payload |
| `off` | nothing |

Frame topics default to `meta`: at 1280×720, raw frames are 55 MB/s. The video itself
is recorded as H.264 by the encoder module, which publishes `video.index`, one message
per encoded frame: `{frame topic id, frame seq, file, pts}`. A dataset joins a frame to
anything else by its seq, and decodes it from the MP4 when it needs the pixels. A
network that needs frames in its rows reads a small topic made for it (`cam.nn`,
160×120 grey: 19 KB a frame), which is cheap enough to record and stream in full.

### Draining

The recorder wakes every 50 ms, drains every ring it records, and hands the records to
its own writer thread through a bounded buffer. A topic is drained losslessly if its
ring holds more than 50 ms of messages; at start, the recorder checks each recorded
topic's `Slots` against its declared rate and warns of those too shallow (`nav.imu: 32
slots at 1 kHz is 32 ms, under the 50 ms drain interval`). If the disk stalls and the
buffer fills, the recorder keeps draining and drops records, and the next record of
each affected topic carries the gap.

At the start of a session it also records the newest message of every event topic
(the parameters in force, the system state, the module states), so the state at the
start is known without waiting for a change.

### Sessions

A recording is a **session**, started and stopped by the recorder's calls or by its
`AutoStart` rule:

| Key in `[Recorder]` | Default | |
|---|---|---|
| `AutoStart` | `armed` | `boot`: always recording. `armed`: while the vehicle is armed ([07-safety](07-safety.md)). `manual`: only by `recorder.start` |
| `Dir` | `/var/lib/hangar/recordings` | |
| `MinFree` | `1 GB` | Stop a session below this much free space, and say so |
| `BlackBox` | `60s` | Length of the black box, 0 to turn it off |
| `BlackBoxMemory` | `32 MB` | Its largest size in RAM |

| Call | |
|---|---|
| `recorder.start` | start a session, with an optional note |
| `recorder.stop` | |
| `recorder.dump` | write the black box now |

`recorder.status` (event topic, and 1 Hz while recording) carries the state, the
session, bytes written, free space, and gaps so far. `recorder.session` carries the
session directory when one starts, so that other modules writing into it (the encoder)
follow it.

A session is a directory, `<Dir>/<YYYYmmdd-HHMMSS>-<host>/`:

| Entry | |
|---|---|
| `session.ini` | session id, boot id, host, software versions, start and stop times on both clocks, why it started and stopped, the note |
| `manifests/` | a copy of every enabled manifest: the wiring graph, and so the meaning of every provenance link |
| `msg/` | the message types in use |
| `<topic>.hgr` | one file per recorded topic |
| `hangar.clock.hgr` | once a second: the monotonic clock, the wall clock, and the time daemon's estimated offset |
| `video/` | the encoder's MP4 files |
| `blackbox-<time>/` | a black-box dump, in the same layout |

### The black box

Whether or not a session is running, the recorder keeps the last `BlackBox` of every
topic it would record, in RAM, sized from the declared rates and capped by
`BlackBoxMemory`. It writes it to disk, as a session of its own, when the system enters
failsafe, when a module exits without being asked to, and on `recorder.dump`. The
moments before an incident are kept even when nobody was recording.

## Recording files (`.hgr`)

```
[ header: 4096 bytes ][ record ][ record ] ...
record = [ record header: 128 bytes ][ payload ]   (payload absent under `meta`)
```

The **header** holds `magic` (`"HGREC001"`), the topic's name, topic id, type hash,
payload size, record size, the policy, the session id, and the topic's schema text, at
the same offsets as in a topic file's header ([01-topics](01-topics.md#header)) where
the field is the same.

The **record header** is the slot header of the message
([01-topics](01-topics.md#slot-header)), with two changes: offset 0 holds the message's
sequence number *n* itself (not the seqlock's 2*n*), and the reserved words carry a gap:

| Offset | Type | Field | |
|---|---|---|---|
| 0 | u64 | `seq` | |
| 8 | u64 | `stamp_ns` | |
| 16 | u64 | `publish_ns` | |
| 24 | u32 | `n_inputs` | |
| 28 | u32 | `flags` | the slot's flags (bit 0: computed from stale inputs), and bit 16: messages are missing before this one |
| 32 | u64 | `gap` | how many |
| 64 | {u32, u32, u64}[4] | `inputs` | as in the topic, with their `reused`, `stale` and `absent` flags |

Files are only appended to. A file whose length is not a whole number of records was
cut off mid-write; the reader ignores the partial record.

```python
rec = hangar.open_recording("session/nav.depth.hgr")   # numpy.memmap underneath
rec["seq"], rec["stamp_ns"], rec["depth_m"], rec["inputs"]
```

Offline converters write MCAP (for Foxglove) and Parquet: `hangar export mcap|parquet
<session>`. Nothing on the vehicle needs either.

## Datasets

### The spec

A dataset spec names one **anchor** topic, one row per anchor message, and the
**inputs** to join to it. Each part of a row has a **role**: `input` (what a model
sees), `label` (what it learns to produce) or `filter` (used by `Where` only). The
same spec builds a policy's inputs on the vehicle, so that a model sees at run time
exactly the features it was trained on ([09-learning](09-learning.md#features-at-run-time)).

Learning what a module computes: the anchor is the module's output, a label, and its
inputs are joined by provenance.

```ini
# /etc/hangar/datasets/depth_hold_clone.dataset
[Dataset]
Anchor=ctl.depth_hold
AnchorRole=label

# The depth message the controller computed this output from.
[Input nav.depth]
Join=provenance

# The 8 IMU messages up to the one it used.
[Input nav.imu]
Join=provenance
Window=8
Fields=gyro accel
```

Imitating the pilot: the anchor is the vehicle's state at each tick, an input; the
pilot's command is joined by time, as the label; rows are kept only while the pilot
was driving.

```ini
# /etc/hangar/datasets/pilot_imitation.dataset
[Dataset]
Anchor=nav.state
AnchorRole=input
Lateness=0
# source_surge: 1 is the pilot (msg/mux_state.msg)
Where=mux.state.source_surge == 1

# The small frame the pilot was looking at: 120 ms of display latency.
[Input cam.nn]
Join=asof:-120ms

[Input pilot.wrench]
Join=asof:0
Role=label

[Input mux.state]
Join=asof:0
Role=filter
```

| `[Dataset]` key | Default | |
|---|---|---|
| `Anchor` | — (required) | One row per message of this topic |
| `AnchorRole` | `label` | `label` or `input` |
| `Fields` | all | The anchor's fields to keep |
| `Rate` | `every:1` | `every:N` or `rate:<Hz>`, applied to anchor messages |
| `Where` | none | Keep only rows for which this holds ([Filters](#filters)) |
| `Missing` | `flag` | `flag`: keep the row, mark the input missing. `drop`: drop the row |
| `Lateness` | `100ms` | How long an `asof` join waits for late messages ([Joins](#joins)) |
| `MaxDelay` | `2s` | The longest any row may wait for its inputs |

| `[Input <part>]` key | Default | |
|---|---|---|
| `Topic` | the part's name | The topic joined. A part is named after its topic unless the same topic is joined twice: `[Input future_state]` with `Topic=nav.state` |
| `Join` | — (required) | `provenance` or `asof:<offset>` |
| `Window` | 1 | Join the K messages ending at the joined one |
| `Role` | `input` | `input`, `label` or `filter` |
| `Fields` | all | The input's fields to keep |

The spec is checked against the manifests: a `provenance` join to a topic that is not
in the anchor's declared `Inputs` is refused, as is a field that does not exist. A
topic may appear both as the anchor and as an input, for a window over the anchor's own
history.

### Joins

| Join | The input is | Deterministic because |
|---|---|---|
| `provenance` | the message the anchor names in its provenance | the seq is in the anchor |
| `asof:<offset>` | the newest message stamped at or before *anchor stamp + offset*, among those published by *anchor publish time + offset + Lateness* | both bounds are recorded times |

With `Window=K`, the input is the K messages ending at the joined one; `count` says how
many were available (fewer at the start of a stream or after a gap).

`provenance` pairs a module's inputs with its output: what the module computed. `asof`
pairs things no module connected: the pilot's command with the frame the pilot was
looking at; a later message with an earlier one, for prediction (positive offsets,
which make the row wait). Stamps are `stamp_ns`, or `publish_ns` for a message without
one.

The publish-time bound in `asof` is what makes the live and offline results equal: live,
the pairer cannot see a message that has not been published yet, and offline, the bound
excludes exactly those messages. A live row therefore waits up to `offset + Lateness`
after its anchor before it is emitted, and never more than `MaxDelay`.

**Specs a policy can run on.** A policy builds its inputs at the moment of the anchor
and cannot wait. A spec is usable by a policy only if every `input`-role `asof` join
has `offset + Lateness ≤ 0`; `label` and `filter` joins may look ahead, since a policy
does not build them. The pairer and `hangar dataset check` say whether a spec
qualifies.

### Filters

`Where` is a condition on the anchor's and the joined parts' fields:
`<part>.<field> <op> <number>` (the anchor's part is named after its topic), with `==`, `!=`, `<`, `<=`, `>`, `>=`, combined with
`and` and `or` and parentheses. A part flagged missing makes the condition false. The
live pairer and the offline build apply it identically. Rows filtered out leave gaps in
`anchor_seq`, from which a trainer knows a sequence was broken.

### The row

Fixed size, from the spec:

```
u64 anchor_seq
u64 anchor_stamp_ns
u32 missing          # bit i: part i missing
u32 reserved
<the anchor, its selected fields>
for each input, label and filter part:
  u64 seq            # of the (last) message joined
  u64 stamp_ns
  u32 count          # messages joined: 1, or up to K for a window
  u32 flags          # of the (last) message joined: bit 0 computed from stale
                     # inputs; for provenance joins, bit 1 reused, bit 2 stale
  <the part, its selected fields> × K
```

The row's schema names each field `<part>.<field>` and marks its role in a comment
(`# role=label`), so a trainer can split inputs from labels without the spec.

### Live: the pairer

The pairer module serves one or more specs. For each, it reads the anchor topic in
order and, for each anchor message, fetches the parts from their rings:
`hg_read_seq(sub, seq)` for provenance, `hg_find_stamp(sub, t)` for `asof`, and the
preceding seqs for windows ([01-topics](01-topics.md#reading)). It applies `Where`, and
publishes each row on `dataset.<name>`, an event topic of 256 slots by default.

A part that has already left its ring cannot be joined: the row marks it missing, and
the pairer counts it. Its status (`pairer.status`) gives, for each dataset and part,
the rows, the misses, and the part's measured latency (the anchor's publish time minus
the joined message's), from which it says how deep the part's ring must be
(`cam.raw: detector latency 180 ms, ring holds 133 ms: needs 8 slots`).

A trainer reads `dataset.<name>` like any topic: on the vehicle, or on the topside
GPU through the tether bridge, which carries the rows across ([08-bridge](08-bridge.md)).
A trainer slower than the rows skips some, and is told how many; for training, that is
sampling.

### Offline: the same rows from a recording

```sh
hangar dataset build pilot_imitation <session> -o pilot_imitation.hgr
hangar dataset pairs <session> depth_hold        # a spec made from the manifest:
                                                 # each output, its Inputs by provenance
hangar dataset check pilot_imitation             # valid; usable by a policy?
```

The build runs the same join and filter code over the session's files, with the same
bounds. For messages that both the live pairer and the recorder had, the rows are
byte-identical, which a test checks on every build. Rows that differ are those where
one of the two missed a message, and both flag it.

### Reading datasets

The same dtype, live or recorded:

```python
for batch in hangar.stream("dataset.pilot_imitation", batch=64):   # live topic
    ...
ds = hangar.open_dataset("pilot_imitation.hgr")                     # file, memmap
```

with a PyTorch `IterableDataset` over either, which can also yield runs of consecutive
rows for recurrent models, broken at gaps ([09-learning](09-learning.md#training)). The
Python package is for the topside and for training; nothing on the vehicle needs it.

## The topside

The topside may run its own recorder, in the same format, for what exists only there
(the pilot's view as displayed, the trainer's own topics). The pilot's joystick
commands cross the tether to the vehicle as a topic, so the vehicle's recorder and
pairer have them regardless. How sessions on the two ends are linked, and how times
are translated across the tether, is in [08-bridge](08-bridge.md).

## Tests

- Recorder: every message of a rated topic kept at its declared rate; a stalled disk
  produces gaps, recorded as gaps; a cut-off file reads up to its last whole record.
- Black box: dumped on a module crash and on failsafe, covering the period before it.
- Joins: each kind against hand-built message sequences, including late messages for
  `asof`, windows at the start of a stream, and inputs gone from their ring.
- Filters: `Where` live and offline on the same rows; a missing part makes it false.
- Live equals offline: run the pairer during a recorded session and build the same
  spec from the recording; rows identical wherever neither flagged a miss.
- Python: the dtypes of recordings and dataset topics read back what C wrote.
