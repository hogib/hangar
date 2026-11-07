# Recorder, provenance and datasets

What the vehicle did, kept as data a network can learn from, both after the dive and
during it. Two modules and one file format:

- the **recorder** writes topics to disk, each topic a file of fixed-size records;
- the **pairer** joins outputs to the inputs they were computed from and publishes
  the result live, as a dataset topic that a trainer reads while the vehicle runs;
- a **dataset spec** says what a sample is, and one join code produces its rows live
  from topics and offline from recordings.

Training on the fly, and deploying what was trained back onto the vehicle, is the
learning loop ([09-learning](09-learning.md)); this doc provides its data.

## The rules

- **Nothing is recorded or paired at a producer's expense.** Both modules are readers;
  a writer never waits for them.
- **Losses are recorded as losses.** A message the recorder could not keep is a gap in
  the file; an input the pairer could not find is flagged missing in the row. A
  dataset never claims more than it has.
- **A sample is defined once.** The same dataset spec drives the live pairer, the
  offline build, and the inputs of a policy running on the vehicle. Live and offline
  rows are identical except where a row says otherwise: the offline row's `late` flag,
  when a message it would have joined, by its stamp, was published after the live row
  was emitted; a part `missing` in one row alone, when it had left its ring before the
  live pairer read it, or the recorder lost it ([Joins](#joins)). A model sees at run
  time the features it was trained on.
- **Every file is an array.** Records and rows are fixed-size, so a file opens with
  `numpy.memmap` and nothing is parsed.
- **A session describes itself**: the wiring graph, parameters, software versions and
  clocks travel with the data.

## The recorder

An ordinary module, `recorder`, which reads topics in order (`hg_next`) and appends
them to files. Its settings are in its own file, `/etc/hangar/recorder.conf`, named on
its `Exec` line, as the bridge's are in `bridge.conf`; its manifest holds only what the
manager checks.

### What it records

The recorder subscribes with a glob, and takes topics as they appear:

```ini
# /etc/hangar/modules/recorder.module
[Module]
Description=Recorder
Exec=/usr/lib/hangar/recorder /etc/hangar/recorder.conf
User=hg-recorder
SupplementaryGroups=hangar-rec

[Subscribe *]

[Publish recorder.status]
Type=recorder_status

[Publish recorder.session]
Type=recorder_session

[Serve start]
Request=recorder_start
Reply=empty
Allow=topside:pilot_console admin topside:admin

[Serve stop]
Request=empty
Reply=empty
Allow=topside:pilot_console admin topside:admin

[Serve dump]
Request=empty
Reply=empty
Allow=topside:pilot_console admin topside:admin
```

A glob subscription ([02-manifest](02-manifest.md#sections-and-keys)) lets the module
open any topic whose name matches, including topics created after it started
(mirrored topics, modules enabled by a reload). `hg_wait` polls them, every 10 ms at
most, instead of waiting on them, so the limit of 127 subscriptions does not apply to
them ([01-topics](01-topics.md#reading)).

Calls and parameters are topics too: a request is a message on the caller's
`<caller>.req`, its reply a message on the server's `<server>.rep`, whose provenance
names the request it answers, and a module's values in force are on `<module>.params`
([04-calls](04-calls.md#the-two-topics), [05-parameters](05-parameters.md#the-parameter-topic)).
The glob takes them in like any other topic, so every command, every refusal and every
parameter change sits in the recording beside the data it acted on, in the same time
base.

```ini
# /etc/hangar/recorder.conf
[Recorder]
AutoStart=armed
BlackBoxMemory=32MB

# The small frame made for networks, in full; the large frames as headers only.
[Record cam.nn]
Policy=full

[Record cam.*]
Policy=meta

[Record *]
Policy=full
```

The file has the manifest syntax and is parsed as strictly
([02-manifest](02-manifest.md#syntax)). A size is a number followed by `KB`, `MB` or
`GB`, in powers of 1024: `32MB`. `[Record <glob>]` sections are matched in order; the
first that matches a topic sets its keys, and a topic that no section matches takes
the defaults.

| `[Record <glob>]` key | Default | |
|---|---|---|
| `Policy` | `full`; `meta` for frame topics | What is kept (below) |
| `BlackBox` | `off` for `dataset.*`, `low` for frame topics, `high` for event topics, `normal` otherwise | The topic's class in the black box ([The black box](#the-black-box)) |

| Policy | Records |
|---|---|
| `full` | every message |
| `every:N` | the messages whose `seq` is a multiple of N |
| `rate:<Hz>` | `every:N`, with N the topic's declared rate divided by the given rate, rounded, at least 1. An event topic is recorded `full` |
| `meta` | every message's header and provenance, without the payload |
| `off` | nothing |

`every:N` and `rate:<Hz>` depend only on each message's own `seq`, so two recordings of
one topic keep the same messages, whenever each started.

Frame topics default to `meta`: at 1280×720, raw frames are 55 MB/s. The video itself
is recorded as H.264 by the encoder module, which publishes `video.index`, one message
per encoded frame: `{frame topic id, frame seq, file, pts}`. An analysis joins a frame
to anything else by its seq, and decodes it from the MP4 when it needs the pixels.
Decoded pixels are not the bytes the vehicle's modules read, so no dataset is built
from them. A network that needs frames in its rows reads a small topic made for it
(`cam.nn`, 160×120 grey: 19 KB a frame), which is cheap enough to record and stream in
full, and is listed `full` before the `cam.*` section. A dataset is built offline only
from topics recorded `full` ([Offline](#offline-the-same-rows-from-a-recording)).

### Draining

The recorder's `hg_wait` returns within 10 ms, as in every module with a glob
subscription ([01-topics](01-topics.md#reading)); it serves its calls on each return,
and drains every 50 ms: it checks the directory word for new topics that match its
glob, drains every ring it records, and hands the records to its own writer thread
through a bounded buffer.

A reader in order can count on `Slots − 1` messages of a ring, since the oldest slot is
the one the writer fills next ([01-topics](01-topics.md#reading)). A topic is therefore
drained losslessly if `(Slots − 1) / rate` exceeds the drain interval plus 10 ms for
wake-up jitter: 60 ms. At start, and for each topic that appears later, the recorder
checks the topic's `Slots` against its declared rate and warns of a ring too shallow
(`nav.imu: 2 slots at 50 Hz hold 20 ms, under the 60 ms the recorder needs`). If the
disk stalls and the buffer fills, the recorder keeps draining and drops records, and
the next record of each affected topic carries the gap.

A rate-limited mirror leaves messages out on purpose, and marks the next message it
sends with slot flag bit 1 ([08-bridge](08-bridge.md#mirrored-topics)). The recorder
keeps the flag, so a reader of the file tells those messages from losses
([Recording files](#recording-files-hgr)).

A mirrored topic whose sender rebooted starts a new **epoch**
([08-bridge](08-bridge.md#mirrored-topics)): a new topic file, with sequence numbers
that start again and the sender's new boot id in `origin_boot`. The recorder closes
that topic's recording and continues in `<topic>+<k>.hgr` (k = 1, 2, …), so that one
file never holds two numberings. A new epoch is not a gap.

At the start of a session it also records the newest message of every event topic
(the parameters in force, the system state, the module states, the last request and
reply of each module), so the state at the start is known without waiting for a
change.

### Sessions

A recording is a **session**, started and stopped by the recorder's calls or by its
`AutoStart` rule:

| Key in `[Recorder]` | Default | |
|---|---|---|
| `AutoStart` | `armed` | `boot`: always recording. `armed`: while the vehicle is armed ([07-safety](07-safety.md)). `manual`: only by `recorder.start` |
| `Dir` | `/var/lib/hangar/recordings` | Where sessions are made (below) |
| `MinFree` | `1GB` | Stop a session below this much free space, and say so |
| `BlackBox` | `60s` | Length of the black box, 0 to turn it off |
| `BlackBoxMemory` | `32MB` | Its largest size in RAM |

| Call | |
|---|---|
| `recorder.start` | start a session, with an optional note |
| `recorder.stop` | |
| `recorder.dump` | write the black box now |

`recorder.status` (an event topic, published on change and once a second while recording) carries the state, the
session, bytes written, free space, gaps so far, and, for each topic in the black box,
how far back its buffer reaches. `recorder.session` carries the session directory when
one starts, so that other modules writing into it (the encoder's MP4 files, the
`mavlink` module's `.tlog`, the policies' weights files) follow it.

A session is a directory, `<Dir>/<YYYYmmdd-HHMMSS>-<host>/`. The manager makes the
default `Dir` at start (`RecordingDir` in `hangard.conf`,
[03-manager](03-manager.md#start-up)): owner root, group `hangar-rec`, mode 2770
(setgid). Another `Dir` is made the same way at installation. The recorder refuses to
start (exit status 78) if its `Dir` is not group `hangar-rec` with mode 2770. Each
session directory, and its `video/` and `weights/`, is made the same way. The modules
that write into sessions have `hangar-rec` in their `SupplementaryGroups` and create
their files with mode 0640: on the vehicle `encoder`, `mavlink` and the policy modules
([09-learning](09-learning.md#policy-modules)), on the topside `video_relay`, into
`topside_recorder`'s sessions. No other module is in the group.

| Entry | |
|---|---|
| `session.ini` | session id, boot id, host, software versions, start and stop times on both clocks, why it started and stopped, the note; the other side's session id and boot id ([08-bridge](08-bridge.md#recordings-on-both-sides)) |
| `manifests/` | a copy of every enabled manifest: the wiring graph, and so the meaning of every provenance link |
| `msg/` | the message types in use |
| `<topic>.hgr` | one file per recorded topic; `<topic>+<k>.hgr` for each later epoch of a mirrored topic |
| `hangar.clock.hgr` | once a second: the monotonic clock, the wall clock, and the time daemon's estimated offset |
| `video/` | the encoder's MP4 files |
| `weights/` | every weights file a policy swapped in during the session, as `<hash>.hgw` ([09-learning](09-learning.md#loading-weights)) |
| `blackbox-<time>/` | a black-box dump, in the same layout |

### The black box

Whether or not a session is running, the recorder keeps the recent past of every topic
in the black box, in RAM, within `BlackBoxMemory`. Each topic asks for a budget:

- a rated topic, `BlackBox` of messages at its declared rate;
- an event topic, its newest `Slots` messages, however old, so that the last requests
  and replies (`<module>.req`, `<module>.rep`), parameter changes and state changes are
  kept even when they happened long before.

The recorder grants the budgets class by class: `high`, then `normal`, then `low`. The
first class that does not fit whole shares what is left in proportion to its budgets,
so each of its topics keeps the same fraction of its history; the classes after it get
nothing. `dataset.*` is `off` by default, since its rows can be built again from their
parts. For scale: `cam.nn` recorded `full`, 160×120 at 30 Hz, needs 35 MB for 60 s by
itself, so as a frame topic, in class `low`, it gets what the others leave. How far
back each topic actually reaches is in `recorder.status` and in the dump's
`session.ini`.

The recorder writes the black box to disk, as a session of its own, when the system
enters failsafe, when a module exits without being asked to, and on `recorder.dump`.
The moments before an incident are kept even when nobody was recording.

## Recording files (`.hgr`)

```
[ header: 4096 bytes ][ record ][ record ] ...
record = [ record header: 128 bytes ][ payload ]   (payload absent under `meta`)
```

The **header** holds `magic` (`"HGREC001"`), the topic's name, topic id, type hash,
payload size, record size, the policy, the session id, `origin_boot` (the boot id of
the machine that numbered the messages, for a mirrored topic; all zero for a local
one), and the topic's schema text, at the same offsets as in a topic file's header
([01-topics](01-topics.md#header)) where the field is the same.

The **record header** is the slot header of the message
([01-topics](01-topics.md#slot-header)), with two changes: offset 0 holds the message's
sequence number *n* itself (not the seqlock's 2*n*), and the reserved words carry a gap:

| Offset | Type | Field | |
|---|---|---|---|
| 0 | u64 | `seq` | |
| 8 | u64 | `stamp_ns` | |
| 16 | u64 | `publish_ns` | |
| 24 | u32 | `n_inputs` | |
| 28 | u32 | `flags` | the slot's flags (bit 0: computed from stale inputs; bit 1: messages before this one were left out by a rate-limited mirror), and bit 16: messages are missing before this one |
| 32 | u64 | `gap` | how many: left out on purpose if bit 1 is set, lost otherwise |
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

A dataset spec, `/etc/hangar/datasets/<name>.dataset`, names one **anchor** topic, one
row per anchor message, and the **inputs** to join to it. Each part of a row has a
**role**: `input` (what a model sees), `label` (what it learns to produce) or `filter`
(used by `Where`, and kept in the row for the trainer). The same spec builds a
policy's inputs on the vehicle, so that a model sees at run time the features it was
trained on ([09-learning](09-learning.md#features-at-run-time)).

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

Learning a correction to a controller: the anchor is the vehicle's state at each tick,
an input, and so is the vertical acceleration the controller wants, joined by time; the
labels, joined by time too, are the controller's own heave command and the pulse widths
the thrusters were actually sent (`thr.out`, from `SERVO_OUTPUT_RAW` after ArduSub's
mixing, [10-mavlink](10-mavlink.md#telemetry)). The training code maps the pulse widths
through the vehicle's thrust curve and allocation to the heave thrust applied, and
subtracts the controller's command: the label is the correction the controller lacked,
which is what the mux adds in residual mode. `Where` keeps the rows in which outputs
were enabled and the vehicle was vertically still, on which what was applied is what
held it; the other rows are kept and masked, since the policy is recurrent. This spec
feeds the adaptive depth hold of
[09-learning](09-learning.md#example-adaptive-depth-hold), and is the same file.

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

No join of this spec looks ahead, so a policy can build the same inputs at its tick
without waiting ([Specs a policy can run on](#joins)).

| `[Dataset]` key | Default | |
|---|---|---|
| `Anchor` | — (required) | One row per message of this topic |
| `AnchorRole` | `label` | `label` or `input` |
| `Fields` | all | The anchor's fields to keep |
| `Rate` | `every:1` | Applied to anchor messages before the joins. `every:N` keeps the anchors whose `seq` is a multiple of N. `rate:<Hz>` is `every:N` with N the anchor's declared rate divided by the given rate, rounded, at least 1; refused for an event-topic anchor |
| `Where` | none | The condition a row must meet ([Filters](#filters)) |
| `Filter` | `drop` | What happens to a row that fails `Where`: `drop` removes it; `mask` publishes it with the `masked` flag ([Filters](#filters)) |
| `Missing` | `flag` | `flag`: keep the row, mark the input missing. `drop`: drop the row |
| `ResetAfter` | `200ms` | An `input` part missing or stale this long sets the row's `reset` flag ([The row](#the-row)). A policy built on this spec resets its state by the same rule |
| `Lateness` | `100ms` | How long a live `asof` join waits, past *anchor publish time + offset*, for messages stamped in time but published later ([Joins](#joins)). The one live knob |

| `[Input <part>]` key | Default | |
|---|---|---|
| `Topic` | the part's name | The topic joined. A part is named after its topic unless the same topic is joined twice: `[Input future_state]` with `Topic=nav.state` |
| `Join` | — (required) | `provenance` or `asof:<offset>` |
| `Window` | 1 | Join the K messages ending at the joined one |
| `Role` | `input` | `input`, `label` or `filter` |
| `Fields` | all | The input's fields to keep |

The spec is checked against the manifests. A `provenance` join to a topic that is
neither in the anchor's declared `Inputs` nor in its `Sources`
([02-manifest](02-manifest.md#sections-and-keys)) is refused, as is a field that does
not exist. A `Sources` topic is referenced only by the messages that used it: on an
anchor message that does not reference it, the part is missing. A topic may appear
both as the anchor and as an input, for a window over the anchor's own history.

### Joins

| Join | The input is | Deterministic because |
|---|---|---|
| `provenance` | the message the anchor names in its provenance | the seq is in the anchor |
| `asof:<offset>` | the newest message stamped at or before *anchor stamp + offset* | stamps are recorded times; a message published too late for the live row sets its `late` flag (below) |

With `Window=K`, the input is the K messages ending at the joined one; `count` says how
many were available (fewer at the start of a stream or after a gap).

`provenance` pairs a module's inputs with its output: what the module computed. `asof`
pairs things no module connected: the command the gate sent with the state the vehicle
was in; a later message with an earlier one, for prediction (positive offsets, which
make the row wait). Stamps are `stamp_ns`, or `publish_ns` for a message without one.

**Lateness.** A message is published some time after it is stamped, and the pairer
reads it later still. Live, the pairer joins an `asof` part once *anchor publish time +
offset + Lateness* has passed, so that a message stamped in time but published a little
later is still joined. `Lateness` is the only live knob. A live row therefore waits,
after its anchor, for the largest `offset + Lateness` of its joins, at least 0, and
that moment is the row's `emitted_ns` ([The row](#the-row)): anchor `publish_ns` plus
that wait, computed from recorded times alone, so the offline build knows it too.

**Late messages.** A message stamped in time for a join but published after the row's
`emitted_ns` was not there when the live row was joined: the live row holds the message
before it, or marks the part missing. The offline build, which has every recorded
message, joins it by its stamp and sets the row's `late` flag (bit 2). The live row
never carries the flag: at `emitted_ns` nothing is known about what is published
later. Live and offline rows are therefore identical except where `late` is set, and
where a part had left its ring before the live pairer read it, which the live row
marks `missing` and the build, with every recorded message, joins
([Live: the pairer](#live-the-pairer), [Offline](#offline-the-same-rows-from-a-recording)).

**Specs a policy can run on.** A policy builds its inputs at the moment of the anchor
and cannot wait. A spec is usable by a policy only if every `input`-role `asof` join
has `offset + Lateness ≤ 0` ([09-learning](09-learning.md#features-at-run-time));
`label` and `filter` joins may look ahead, since a policy does not build them. A
recurrent policy (one with an `lstm` op) takes one step per anchor message, so its
spec must also have `Rate=every:1`, `Missing=flag` and `Filter=mask`: a row for every
tick. `hangar dataset check` says whether a spec qualifies and names the `input` joins
that disqualify it; given a policy spec, it also checks that the spec is the policy's
`Dataset`, and the recurrent conditions against the policy's current weights when it
has any ([12-tools](12-tools.md#datasets-and-analysis)).

### Filters

`Where` is a condition on the anchor's and the joined parts' fields:
`<part>.<field> <op> <number>` (the anchor's part is named after its topic), with
`==`, `!=`, `<`, `<=`, `>`, `>=`, combined with `and` and `or` and parentheses. A part
flagged missing makes the condition false. The live pairer and the offline build apply
it identically.

`Filter` decides what happens to a row that fails it:

- `drop`, the default, removes the row. Its `anchor_seq` is then absent from the
  dataset, which is not a loss.
- `mask` publishes the row with flag bit 1, `masked`. A trainer steps a recurrent model
  through it, as the vehicle's policy acts on every tick whoever is driving, and takes
  no loss from it ([09-learning](09-learning.md#training)).

A row lost after it was published (on the tether, or skipped by a slow trainer) is a
gap in the `dataset.<name>` topic's own sequence numbers, never only in `anchor_seq`.

### The row

Fixed size, from the spec:

```
u64 anchor_seq
u64 anchor_stamp_ns
u8  boot_id[16]      # the boot that numbered the anchor: its topic's origin_boot,
                     # or this machine's boot id for a local anchor
u32 missing          # bit i: part i missing
u32 flags            # bit 0 reset: recurrent state starts over before this row;
                     # bit 1 masked: the row failed Where;
                     # bit 2 late: set by the offline build when it joined a message
                     # published after emitted_ns
u64 emitted_ns       # when the live row's joins closed: anchor publish_ns plus the
                     # largest offset + Lateness of its asof joins, at least 0
<the anchor, its selected fields>
for each input, label and filter part:
  u64 seq            # of the (last) message joined; 0 if missing
  u64 stamp_ns       # 0 if missing
  u32 count          # messages joined: 1, or up to K for a window; 0 if missing
  u32 flags          # bits 0-7: the slot flags of the (last) message joined
                     # (bit 0 computed from stale inputs); for provenance joins,
                     # bits 8-10: the anchor's reference flags (8 reused, 9 stale,
                     # 10 absent)
  <the part, its selected fields> × K
```

A missing part is zeros, with its `missing` bit set, and so are the messages of a
window that `count` does not fill. A policy hands its model the same bytes for a
missing input ([09-learning](09-learning.md#features-at-run-time)).

`boot_id` and `anchor_seq`, with the anchor's topic id, identify the anchor message
across reboots and across the tether ([08-bridge](08-bridge.md#mirrored-topics)). A
part's `seq` is in its own topic's numbering, whose boot is that topic's
`origin_boot` in the recording. `emitted_ns` is the same value live and offline, since
it follows from the anchor's `publish_ns` and the spec; it is what the offline build
compares a joined message's `publish_ns` against to set `late` ([Joins](#joins)).

**The reset flag.** The pairer sets bit 0 by the rule a policy applies to its own
state ([09-learning](09-learning.md#lstm)), from what the anchor and its parts carry:

- on the first row; on the first row after an anchor message the pairer never saw (a
  gap in the anchor topic itself, not one made by `Rate` or `Where`); and on the first
  row of a new epoch of the anchor;
- on every row at which an `input` part has been missing or stale for `ResetAfter` or
  longer, measured on anchor stamps. A part is stale when its flags carry bit 0 or
  bit 9, or, for an `asof` join, when its (last) message is stamped more than its
  topic's `StaleAfter` before *anchor stamp + offset* (event topics are never stale by
  age).

The row's schema names each field `<part>.<field>`, message *k* of a window (0 the
oldest) `<part>.<k>.<field>`, and a part's header fields `<part>._seq`,
`<part>._stamp_ns`, `<part>._count` and `<part>._flags`. It marks each field's role in
a comment (`# role=label`), so a trainer can split inputs from labels without the
spec.

### Live: the pairer

The pairer module serves the specs listed in its own file, `/etc/hangar/pairer.conf`.
Each is also an output of its manifest, of type `dataset:<name>`:

```ini
# /etc/hangar/pairer.conf
[Pairer]
Datasets=depth_ff
```

```ini
# /etc/hangar/modules/pairer.module
[Module]
Description=Dataset pairer
Exec=/usr/lib/hangar/pairer /etc/hangar/pairer.conf

[Subscribe nav.state]
[Subscribe ctl.depth_hold.target]
[Subscribe ctl.depth_hold]
[Subscribe thr.out]
[Subscribe safety.state]

[Publish dataset.depth_ff]
Type=dataset:depth_ff
Slots=256

[Publish pairer.status]
Type=pairer_status
```

The pairer refuses to start (exit status 78) if the two lists differ.
`Type=dataset:<name>` ([02-manifest](02-manifest.md#sections-and-keys)) is a type the
manager generates, as it does the types of the call topics `<module>.req` and
`<module>.rep` ([04-calls](04-calls.md#the-two-topics)). When the pairer opens
`dataset.<name>`, the manager builds the row schema from
`/etc/hangar/datasets/<name>.dataset` and the schemas of the topics it names (a frame
part takes the size of its open topic), and refuses an open whose schema hashes to
anything else ([03-manager](03-manager.md#opening-topics)). The pairer therefore opens
a dataset topic once every topic its spec names exists. A row schema longer than a
topic header's 3584 bytes of schema text is refused.

For each spec, the pairer reads the anchor topic in order and, for each anchor
message, fetches the parts from their rings: `hg_read_seq(sub, seq)` for provenance,
`hg_find_stamp(sub, t)` for `asof`, and the preceding seqs for windows
([01-topics](01-topics.md#reading)). It joins each part as soon as its wait is over:
`provenance` joins, and `asof` joins with `offset + Lateness ≤ 0`, at once, as a
policy does; the others at the row's `emitted_ns` ([Joins](#joins)). It applies
`Where` and `Filter`, sets the row's flags, and publishes each row on
`dataset.<name>`, an event topic.

A part that has already left its ring cannot be joined: the row marks it missing, and
the pairer counts it. Its status (`pairer.status`) gives, for each dataset and part,
the rows, the misses, and the part's measured latency: from the joined message's
publication to the moment the pairer read it, its own wait included. From that it says
how deep the part's ring must be, since `(Slots − 1) / rate` must exceed the latency
(`cam.raw: detector latency 180 ms, ring holds 100 ms: needs 8 slots`).

A trainer reads `dataset.<name>` like any topic: on the vehicle, or on the topside
GPU through the tether bridge, which carries the rows across
([08-bridge](08-bridge.md)). A dataset whose rows carry frames is mirrored `reliable`,
since a lost fragment loses the whole row. A trainer slower than the rows skips some,
and is told how many: a gap in the topic's own sequence numbers, which a recurrent
trainer treats as a break ([09-learning](09-learning.md#training)).

### Offline: the same rows from a recording

```sh
hangar dataset build depth_ff <session> -o depth_ff.hgr
hangar dataset pairs <session> depth_hold        # a spec made from the manifest:
                                                 # each output, its Inputs by provenance
hangar dataset check depth_ff                    # valid; usable by a policy?
hangar dataset check depth_ff /etc/hangar/policies/depth_ff.policy
                                                 # usable by this policy?
hangar dataset check depth_ff <session>          # can it be built from this session?
```

The build runs the same join, filter and flag code over the session's files. Every
part's topic must have been recorded `full` in the session: a part recorded `meta`,
`every:N`, `rate:<Hz>` or not at all would join other messages than the pairer did
live, so `hangar dataset check <spec> <session>` names it and `build` refuses. The
build computes each row's `emitted_ns` as the pairer did, and sets `late` on a row in
which it joined a message published after it ([Joins](#joins)). Live and offline rows
are then identical except where `late` is set, where one of the two missed a message,
which that row alone marks `missing` (the pairer, one that had left its ring; the
build, one the recorder lost), and in the build's first row, whose `reset` flag is
always set; a test checks this on every build.

### Reading datasets

The same dtype, live or recorded:

```python
for batch in hangar.stream("dataset.depth_ff", batch=64):   # live topic
    ...
ds = hangar.open_dataset("depth_ff.hgr")                     # file, memmap
```

with a PyTorch `IterableDataset` over either, which steps a recurrent model through
the rows in order: its state starts over where `reset` is set, and `masked` rows carry
no loss ([09-learning](09-learning.md#training)). The Python package is for the
topside and for training; nothing on the vehicle needs it.

## The topside

The topside may run its own recorder, in the same format, for what exists only there
(the pilot's view as displayed, the trainer's own topics). Module and topic names are
unique across the vehicle and the topside together
([08-bridge](08-bridge.md#configuration)), so it is a module of another name,
`topside_recorder`, with its own settings file, and its topics and calls are named
after it: `topside_recorder.status`, `topside_recorder.session`,
`topside_recorder.start`. The pilot's joystick commands cross the tether to the
vehicle as a topic, so the vehicle's recorder and pairer have them regardless. How
sessions on the two ends are linked, and how times are translated across the tether,
is in [08-bridge](08-bridge.md).

## Tests

- Recorder: every message of a rated topic kept at its declared rate when
  `(Slots − 1) / rate` passes the drain check, and a warning for a ring that does not;
  a stalled disk produces gaps, recorded as gaps; messages left out by a rate-limited
  mirror recorded as such, not as losses; a mirrored topic's new epoch continued in a
  new file; a cut-off file reads up to its last whole record.
- Globs: a topic created after the recorder started is recorded; `[Record]` sections
  match first to last (`cam.nn` full, `cam.raw` meta); `every:N` and `rate:<Hz>` keep
  the same messages in two recordings started at different times.
- Sessions: a `Dir` with the wrong group or mode stops the recorder with status 78; the
  encoder, `mavlink` and a policy module write into a session, and a module outside
  `hangar-rec` cannot.
- Black box: dumped on a module crash and on failsafe, covering the period before it;
  with more than `BlackBoxMemory` asked for, classes granted in order and each topic's
  reach reported; an event topic keeps its newest `Slots` messages; `dataset.*` left
  out.
- Joins: each kind against hand-built message sequences, including messages published
  after their stamp for `asof`, windows at the start of a stream, and inputs gone from
  their ring; a message published within `Lateness` of its stamp joined live; one
  published after the row's `emitted_ns` absent from the live row, joined by the build
  with `late` set; `emitted_ns` equal live and offline.
- `Rate`: the same anchors selected live by a pairer running since boot and offline
  from a session started later.
- Filters: `Where` live and offline on the same rows; a missing part makes it false;
  `mask` publishes the row flagged `masked`, `drop` removes it.
- Reset flag: set at an anchor gap, at a new epoch, and while an `input` part has been
  missing or stale for `ResetAfter`; not set at a change of `Where`; on the same ticks
  as the resets of a policy fed the same messages.
- Row type: the manager's generated schema equals the pairer's; an open with another
  schema refused; the pairer stops with status 78 when `pairer.conf` and its manifest
  differ.
- Offline: a spec with a part recorded `meta` refused by `check` and `build`, which
  name the part.
- Live and offline: run the pairer during a recorded session and build the same spec
  from the recording; rows identical wherever the build set no `late` flag and neither
  flagged a miss, apart from the build's first `reset` flag; every row in which the
  two differ otherwise carries `late`.
- Calls recorded: a `<caller>.req` and a `<server>.rep` topic taken in by the glob,
  the reply's provenance naming its request, and the newest of each kept at the start
  of a session.
- Python: the dtypes of recordings and dataset topics read back what C wrote.
