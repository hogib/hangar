# Topics

How data moves between modules on one machine. A **topic** is a named stream of
fixed-size messages of one type, written by exactly one module and read by any number.
Each topic is one shared-memory file holding a ring of slots. Crossing the tether is
the bridge's job ([08-bridge](08-bridge.md)); a topic never leaves its machine.

The design is electric-eye's frame bus (`EEYEBUS1`), generalized from video frames to
any message type, with the per-stream heartbeat thread removed and provenance added.

## The rules

- **The writer never waits for a reader.** It writes the next slot and moves on. A
  reader that falls behind skips ahead and is told how many messages it missed.
- **A message is whole or not returned.** Each slot is guarded by a sequence lock; a
  copy the writer overwrote midway is detected and retried.
- **One writer per topic**, named in its manifest. The manager refuses a second.
- **Sequence numbers count up by one per message and continue across writer
  restarts** for the life of the boot. `(boot id, topic id, seq)` identifies a message
  for good, the boot id (`/proc/sys/kernel/random/boot_id`) being that of the machine
  whose module published it. A topic mirrored across the tether keeps the sender's
  numbers and boot id, so there they rise with gaps; a gap is a message lost on the
  tether or one a rate-limited mirror skipped, and readers tell the two apart
  ([Mirrored topics](#mirrored-topics)).
- **Names are unique across the vehicle and the topside together**, so a mirrored
  topic never meets a local topic of the same name or id ([Names](#names)).
- **Readers sleep until a message arrives**, on a futex in the file. A writer only
  makes the wake system call when a reader is actually waiting.
- **Subscribing never blocks.** A topic that does not exist yet is `HG_ABSENT` until it
  appears; the module carries on, and its outputs say their inputs are missing.
- **A message computed from stale inputs says so**, and so does everything computed
  from it ([Stale inputs](#stale-inputs)).
- **Every message records the input messages it was computed from** (up to 4), filled
  in by the library ([Provenance](#provenance)).
- **The type travels with the topic.** The schema text is stored in the file, so any
  reader can decode it without generated code.

## Names

`[a-z0-9_]` segments joined by dots, at most 47 bytes: `nav.depth`, `cam.raw`,
`thr.cmd`. The file is `/dev/shm/hangar/<name>` (`$HANGAR_TOPIC_DIR` overrides the
directory; the manager sets it for every module). The prefix `hangar.` is reserved for
the manager's own topics, which any module or recorder reads like any other, and so
are the names `<module>.calls` and `<module>.params`
([02-manifest](02-manifest.md#how-the-manager-uses-manifests)). The manager's topics
carry its machine's role, set by `Role=vehicle|topside` in `hangard.conf`:
`hangar.vehicle.system` and `hangar.vehicle.modules` on the vehicle,
`hangar.topside.system` and `hangar.topside.modules` on the topside
([03-manager](03-manager.md#system-state-and-the-managers-topics)).

Topic names, like module names, are unique across the vehicle and the topside
together. Each topic also has a 32-bit **topic id**, the FNV-1a hash of its name, used
in provenance records. The manager refuses, when it loads the manifests, two topic
names whose ids collide. At the handshake the two bridges exchange their machines'
module and topic lists with the topic ids, and refuse to mirror a topic whose name or
id collides with one on the other side; `config_match` in the link status and
`hangar check` report it ([08-bridge](08-bridge.md#the-link)). A topic id therefore
names one topic across the pair, and provenance crosses the tether unchanged.

Per-machine infrastructure is the one exception, as for module names: the `bridge`
module runs on both machines under that name, its call record `bridge.calls` is never
mirrored, and its own topics, like the manager's, carry the role (`bridge.vehicle.link`,
`bridge.topside.files`; [08-bridge](08-bridge.md#the-bridge-module)).

## Message types

A type is a flat list of fixed-size fields, written in a `.msg` file:

```
# msg/depth.msg
f32 depth_m         # m, positive down
f32 temp_c          # degrees Celsius
f32 pressure_mbar   # mbar, absolute
```

| Field types | |
|---|---|
| `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64` | scalars |
| `<type> name[N]` | fixed array, N ≥ 1 |
| `char name[N]` | text of at most N−1 bytes, NUL-padded |

Parameter types may also give each field a default, a range and a `@restart` mark
([05-parameters](05-parameters.md#parameter-types)); those attributes are not part of
the type hash.

No nested types, no variable lengths, no pointers. Fields take their natural alignment;
the layout is the C layout of the struct with that field order, padding made explicit,
and the size rounded up to 8 bytes. Comments are kept: they carry units, which the
recorder copies into datasets.

The **type hash** is the 64-bit FNV-1a hash of the schema's canonical form: one field
per line as `type name[N]` (`[N]` only for arrays), single spaces, each line ended by a
newline, comments, blank lines and parameter attributes removed. A reader that opens a topic with an expected type gets an error, not data,
if the hashes differ; a reader that opens it without one (a dashboard, Python, the
recorder) parses the schema from the file.

A build-time generator turns `.msg` files into C headers (a struct and a type
descriptor holding the schema text and hash). It is not needed at run time, and
Python parses the schema text directly.

**Frames** are messages too. A frame type is built at run time for one size and
format (`hg_frame_type(1280, 720, HG_I422)`), its schema written with literal sizes:

```
u32 width
u32 height
u32 fourcc
u32 stride[3]
u32 offset[3]
u8  data[1843200]
```

so a reader that expects 720p cannot attach to a 480p stream by mistake. Frame topics
default to 4 slots.

**Dataset rows** are messages too. The manager generates a row type from the dataset
spec, and a manifest names it as `Type=dataset:<name>`, as it names a call record type
`calls:<module>` ([06-recorder](06-recorder.md#the-row)). Its field names carry dots
(`nav.state.depth_m`, `cam.nn._seq`), which only a generated row type may have.
Readers parse its schema from the file like any other.

## File layout

Native byte order, which on every supported target is little-endian.

```
[ header: 4096 bytes ][ slot 0 ][ slot 1 ] ... [ slot N-1 ]
slot = [ slot header: 128 bytes ][ payload ], padded to a multiple of 64 bytes
```

### Header

Written once at creation, except `flags` (set later by the writer and by the
manager) and the line from offset 192, which the writer updates on every publish (on
its own cache line, so readers of the metadata do not share a line with it). Every
field written while readers may be reading is accessed atomically (64-bit fields as
`_Atomic uint64_t`, so that on ARMv7 they are never torn into two 32-bit halves).

| Offset | Type | Field | |
|---|---|---|---|
| 0 | char[8] | `magic` | `"HGTOPIC1"` |
| 8 | u32 | `version` | 1 |
| 12 | u32 | `header_bytes` | 4096 |
| 16 | u64 | `generation` | random, differs for every writer run |
| 24 | u32 | `slot_count` | power of two, 2 to 65536 |
| 28 | u32 | `slot_header_bytes` | 128 |
| 32 | u32 | `payload_bytes` | message size |
| 36 | u32 | `flags` | bit 0: closed by its writer; bit 1: writer died (set by the manager); bit 2: replaced by a newer file (set by the manager) |
| 40 | u64 | `slot_stride` | bytes from one slot to the next |
| 48 | u64 | `slots_offset` | 4096 |
| 56 | u32 | `topic_id` | FNV-1a 32 of the name |
| 60 | u32 | `rate_mhz` | declared rate in millihertz; 0 for event topics; for a mirrored topic, the mirror's `ExpectRate` |
| 64 | u64 | `type_hash` | |
| 72 | char[48] | `name` | |
| 120 | char[32] | `writer` | the writing module's name |
| 152 | i32 | `writer_pid` | |
| 156 | u32 | `schema_bytes` | length of the schema text |
| 160 | u64 | `created_real_ns` | wall clock at creation |
| 168 | u64 | `first_seq` | `write_seq` when this file was created: messages up to it are not in this file |
| 176 | u64 | `stale_after_ns` | from the manifest's `StaleAfter`, or the mirror's for a mirrored topic; 0 for event topics |
| 192 | u32 | `futex_word` | changes on every publish and on close; wait on it |
| 196 | u32 | — | reserved, 0 |
| 200 | u64 | `write_seq` | messages published; message *n* is in slot *(n−1) mod slot_count* |
| 208 | u64 | `last_publish_ns` | `CLOCK_MONOTONIC` |
| 256 | u8[16] | `origin_boot` | boot id of the machine whose module published the messages: the sender's for a mirrored topic ([Mirrored topics](#mirrored-topics)); all zero for a local topic |
| 512 | char[3584] | `schema` | the `.msg` text, comments included |

### Slot header

| Offset | Type | Field | |
|---|---|---|---|
| 0 | u64 | `seq` | `2n` once message *n* is complete in this slot; odd while written |
| 8 | u64 | `stamp_ns` | when the data was true: sensor or capture time, `CLOCK_MONOTONIC`; 0 if unknown |
| 16 | u64 | `publish_ns` | `CLOCK_MONOTONIC` at publication |
| 24 | u32 | `n_inputs` | 0 to 4 |
| 28 | u32 | `flags` | bit 0: computed from stale inputs ([Stale inputs](#stale-inputs)); bit 1: decimated before this message, set by the bridge on a mirror with `Rate=` ([Mirrored topics](#mirrored-topics)) |
| 32 | — | — | reserved, 0 (32 bytes) |
| 64 | {u32, u32, u64}[4] | `inputs` | `{topic_id, flags, seq}` of each input message; `flags` bit 0: reused (nothing newer was read since the previous publish), bit 1: stale, bit 2: absent |

### Wake file

Beside each topic file the manager creates `/dev/shm/hangar/.<name>.wake`: 64 bytes,
group `hangar`, mode 0660, which every reader may write (the topic file itself they
can only read). Offset 0 holds a u32 `wait_flag`. A reader sets it before it sleeps;
the writer clears it as it publishes, and calls `FUTEX_WAKE` only if it was set
([Publishing](#writing), [Waiting](#reading)).

The flag repairs itself: a reader that dies asleep leaves it set, which costs the
writer one wake call on its next publish, and then it is clear. Since any module can
write it, a misbehaving one could clear it and delay another's wake-up until that
reader's timeout; it cannot corrupt data or make a reader miss a message.

A wake file, like the directory word's file `.dir` ([Absent topics](#reading)), keeps
its inode for the whole boot, because sleepers wait on it by inode. The manager creates
each with `O_CREAT` once per boot, never by a temporary file and a rename, never
truncates it, and never deletes or replaces it during a boot. A replaced topic keeps
its wake file, and a restarted or re-executed manager opens the existing files.

Times are `CLOCK_MONOTONIC` only. The recorder logs the offset between the monotonic
and the wall clock once a second, and the bridge translates times across the tether,
so the slot needs no wall-clock field.

## Writing

```c
hg_pub *p = hg_pub_open("nav.depth", &DEPTH_TYPE, NULL);   // slots, rate: the manifest
hg_publish(p, &msg, stamp_ns);
hg_pub_close(p);
```

**Opening** is done by the manager on the writer's behalf. The library sends the
request (topic, type hash, schema, payload size) over the control socket; the manager
checks it against the manifest (this module, a declared output, the declared type, no
other writer), creates the file under a temporary name, reserves its memory
(`posix_fallocate`, so a full `/dev/shm` fails here and not mid-run), writes the
header, creates the topic's wake file if it does not exist yet, renames the topic file
into place, then increments and wakes the directory word, and passes back a writable
descriptor (`SCM_RIGHTS`), which the library maps
([03-manager](03-manager.md#opening-topics)). If a file of that name already exists,
the new header continues its `write_seq` (and records it as `first_seq`), so sequence
numbers never repeat within a boot; a mirrored topic follows the sender's boot instead
([Mirrored topics](#mirrored-topics)). Before renaming the new file into place, the
manager sets bit 2 on the old file, increments its `futex_word` and wakes its readers,
which then reopen the path ([A replaced file](#reading)). The manager never deletes a
topic file during a boot.

A module run by hand without a manager (`hangar run`, tests) has the library create
the file itself, by the same steps, in a private topic directory
(`$HANGAR_TOPIC_DIR`, by default under the user's runtime directory): the vehicle's
`/dev/shm/hangar` is the manager's alone.

**Publishing** message *n* into slot *s*:

1. store `s.seq = 2n − 1`, then a release fence;
2. copy the slot header fields and the payload;
3. store-release `s.seq = 2n`, then store-release `write_seq = n`, and store
   `last_publish_ns` (atomic, relaxed);
4. increment `futex_word` (sequentially consistent), then atomically exchange the
   wake file's `wait_flag` with 0; only if it was set, `FUTEX_WAKE` all.

No system call when nobody waits; no lock; no allocation.

**Closing** sets flag bit 0, increments `futex_word` and wakes readers. A writer that
dies without closing is detected by the manager through its `pidfd`; the manager sets
flag bit 1 (atomic, release), increments `futex_word` (sequentially consistent) and
wakes the readers, exactly as a close does. No thread in the module keeps the topic
alive. A writer that died while the manager itself was down is flagged when the
manager starts again ([03-manager](03-manager.md#adoption)).

The library never shrinks a topic file. A writer that did (`ftruncate` on its
descriptor) would make its readers fault on their next access; readers check the file's
size against its header when they open it, and the failure is confined to a writer
bug.

## Reading

```c
hg_sub *s = hg_sub_open("nav.depth", &DEPTH_TYPE);   // never blocks; NULL type: introspect
hg_latest(s, &msg, &info);     // the newest message; older ones skipped
hg_next(s, &msg, &info);       // the next message in order; info.gap counts misses
unsigned r = hg_wait((hg_sub *[]){imu, depth}, 2, timeout_ms);  // ready set; 0: timed out
hg_read_seq(s, seq, &msg, &info);     // message `seq`, if still in the ring
hg_find_stamp(s, t_ns, &msg, &info);  // the newest message stamped at or before t
```

`hg_read_seq` and `hg_find_stamp` are random access into the ring, for the dataset
pairer ([06-recorder](06-recorder.md#live-the-pairer)): a message no longer in the
ring, or never in it (a seq a mirrored topic does not have), returns `HG_GONE`.
`hg_find_stamp` searches the ring from the newest seq back, checking each slot's `seq`
as `hg_next` does and passing over seqs not in the ring, assuming stamps rise with
sequence numbers, which a writer must keep.

`hg_wait` returns a **ready set**: every condition that holds when it returns, 0 if
none did before the timeout.

| Bit | Name | Meaning |
|---|---|---|
| 0 | `HG_READY_CALLS` | a request is pending for this module; `hg_serve` answers it ([04-calls](04-calls.md#using-calls)) |
| 1 | `HG_READY_REPLIES` | a reply to a call started with `hg_call_start` has arrived; `hg_call_poll` collects it |
| 2 | `HG_READY_TOPICS` | at least one of the topics has news: a message, a close, a replacement, or an absent topic that appeared |

Since all three are reported together, a loop that serves its calls and then reads its
topics on each pass lets neither starve the other.

`info` carries `seq`, `stamp_ns`, `publish_ns`, the slot `flags`, the inputs, `gap`
(messages lost), `skipped` (seqs a rate-limited mirror skipped on purpose), `epoch`
(changes when a mirrored topic's sender has rebooted), and the result:

| Result | Meaning |
|---|---|
| `HG_MSG` | a message was copied |
| `HG_NONE` | nothing new (`hg_latest`: nothing newer than the last one returned) |
| `HG_ABSENT` | the topic does not exist yet; the library attaches when it appears |
| `HG_CLOSED` | the writer closed the topic or died; the library reopens it when a new writer appears |
| `HG_BUSY` | the writer overtook the copy repeatedly (a reader far too slow for the ring) |
| `HG_GONE` | `hg_read_seq`, `hg_find_stamp`: the message is not in the ring |

**Newest message:**

```
n  = write_seq                          (acquire)
if n == the last n returned, or n <= first_seq: HG_NONE
s  = slot (n−1) mod slot_count
s1 = s.seq                              (acquire)
if s1 != 2n: the writer has moved on; start over
copy the slot header and payload
acquire fence; s2 = s.seq
if s2 != s1: overwritten while copying; start over
```

**In order** (`hg_next`), the reader wants *w = last + 1*, never below
`first_seq + 1`. With `W = write_seq` and `K = slot_count`:

- if `w > W`: `HG_NONE`;
- if `w ≤ W − K + 1`, the messages before `W − K + 2` are gone or about to be: they
  count in `gap`, and *w* moves to `W − K + 2` (not `W − K + 1`, whose slot is the one
  the writer fills next);
- otherwise read slot *(w − 1) mod K*: if its `seq` is `2w`, copy it with the
  before/after check; any other value (odd, greater or smaller than `2w`) means message
  *w* is not in the ring: count it in `gap` and go on to *w + 1*, never retrying the
  same target. Since *w ≤ W*, the writer has passed *w*, so there is nothing to wait
  for.

On a local topic the slot of a lost message holds an odd or a greater `seq`. On a
mirrored topic, whose numbers have gaps, it can also hold an older message or zeros:
*w* never reached this machine. The seqs stepped over on the way to a message whose
slot flag bit 1 is set (decimated before this message) were skipped on purpose; they
count in `skipped`, not in `gap`. The clamp above bounds the walk to fewer than *K*
slots per call.

A reader keeps up as long as fewer than `slot_count` messages are published between two
of its reads, that is, it reads at least once every `slot_count − 1` messages: 64 slots
at 1 kHz is 63 ms of slack, less the reader's own wake-up jitter.

**Waiting** reads `futex_word` (acquire), sets the wake file's `wait_flag`
(sequentially consistent), checks `write_seq` and `flags` again, and only then sleeps
on the value it read; it sets the flag again before every sleep. A publish between the
check and the sleep has changed `futex_word`, so the sleep returns at once; a publish
after the flag was set finds it set and wakes the reader. Several topics are waited on
with one `futex_waitv` call (Linux 5.16), whose limit is 128 futexes in all. Two are
kept for the library: the module's wake word, which a client increments for a request
and a server for a reply ([04-calls](04-calls.md#protocol)), and the directory word
below, so a module subscribes to at most 126 topics by name; the manager refuses a
wiring past it. Topics matched by a glob subscription (`[Subscribe nav.*]`,
[02-manifest](02-manifest.md#subscribe-topic-or-glob-one-per-input)) do not count: the module
reads them on a timer, and `hg_wait` does not wait on them. Waiting on a read-only
mapping is allowed. The futexes are shared, never `FUTEX_PRIVATE_FLAG`.

**Absent topics.** `hg_sub_open` returns at once, whether the topic exists or not. An
absent topic reads `HG_ABSENT`, and counts as stale for the module's outputs
([Stale inputs](#stale-inputs)). While any subscription is absent, `hg_wait` also
waits on the **directory word**, a u32 in `/dev/shm/hangar/.dir` that the manager
increments, and wakes, whenever it creates or replaces a topic, always after the
rename that puts the file in place. `hg_wait` reads the directory word (acquire) first,
then retries the open of every absent subscription, and sleeps on the value it read: a
topic created after the read has changed the word, so the sleep returns at once.
Nothing polls, and a missing topic upstream never stops a module from starting,
publishing, or becoming ready.

**Waiting together with file descriptors.** A futex cannot be waited on with `epoll`,
so a module that must also wait on sockets or devices (the bridge, the MAVLink module,
a camera) asks for `hg_wait_fd(subs, n)`: the library starts one helper thread that
waits on the futexes and signals an `eventfd`, which the module puts in its own `epoll`
set beside its descriptors, then reads its topics without blocking. The helper takes
its snapshot of the futex words *before* it signals, and waits on that snapshot, so a
message published while the module is reading is never missed. This is the library's
only thread, and only modules that ask for it have one. (`io_uring`, from Linux 6.7,
can wait on futexes and descriptors together and may replace the helper later.)

The helper's set of futexes changes on the module's thread: a topic attaches after
being absent, a replaced file is reopened, the directory word joins or leaves the set.
The library then writes the new set and increments a generation counter under a small
lock, increments the module's own wake word and wakes it (that word is always in the
helper's set, and a spurious wake costs nothing), and keeps any old mapping until the
helper has acknowledged that generation; only then does it unmap it. The helper reads
the set and its generation before every snapshot and acknowledges the generation it
snapshotted. So it never sleeps on a file no one publishes to any more and never reads
a mapping that is gone, and no futex beyond the two kept for the library is needed.

**A replaced file** (a restarted writer's new file renamed into place) is announced
in the old one: the manager sets bit 2 and wakes its readers before the rename. The
library first drains what it still wants from the old mapping, then reopens the path.
Readers never `stat` the path on the read path; the flag is the only signal. Because
sequence numbers continue across restarts, `hg_next` carries on where it was, and a gap
appears only if messages were really lost. A mirrored topic's new file can carry a
different `origin_boot`, when the sender has rebooted and its numbers start again
([Mirrored topics](#mirrored-topics)). The reader then starts a new **epoch**: `epoch`
in `info` changes, `hg_next` and `hg_latest` start from the new file's `first_seq + 1`,
and nothing is counted in `gap`.

**Liveness and staleness.** A reader can tell a dead writer (flag bit 1) from a
closed one (bit 0) from a live one with nothing new. A topic with a declared rate is
**stale** when `now − last_publish_ns` exceeds `stale_after_ns` (the manifest's
`StaleAfter`, by default three periods, or the mirror's for a mirrored topic), the same
threshold for every reader and the manager; `info` carries the age, and acting on
staleness is the manager's job ([03-manager](03-manager.md)). The bridge is the
exception: the topics it receives are left out of its health, which is judged by its
own watchdog and `bridge.<role>.link` ([08-bridge](08-bridge.md#the-bridge-module)).

## Provenance

Each published message can list up to four input messages, as `(topic_id, seq)`.
The library fills them in: a publisher is linked to the subscriptions it is computed
from, and on each publish it records the `seq` of the message most recently returned
by each of them. A link may also be to another of the module's own outputs, whose
newest published `seq` is recorded: a policy's output names the
`policy.<name>.active` message, published once per swap or revert, that put the
computing model in charge ([09-learning](09-learning.md#lineage)).

```c
hg_publish(p, &msg, stamp_ns);                          // a reference per Inputs topic
hg_publish_with(p, &msg, stamp_ns, flags, refs, n);     // references chosen per message
hg_publish_seq(p, seq, flags, &msg, stamp_ns, refs, n); // the bridge only: the sender's seq and flags
```

The links are declared in the manifest, never set by the module's code
([02-manifest](02-manifest.md#publish-topic-one-per-output)):

| Key | | |
|---|---|---|
| `Inputs` | at most 4 | referenced on every publish. An entry `<role>:<topic>` (`vehicle:cam.pilot`) names a topic of the other side, which the module does not subscribe to and gives the reference for itself with `hg_publish_with` |
| `Sources` | any number of the module's subscriptions | referenced only in a message whose module names them, with `hg_publish_with` |

`hg_publish_with` may give the reference for an `Inputs` topic itself, overriding the
default, and may add references to `Sources` topics, chosen per message: a mux names
the sources that drove this message ([07-safety](07-safety.md#the-mux)). Every
reference must be a message of an `Inputs` or `Sources` topic, and a message holds at
most 4 in all, its `Inputs` included; the library refuses a publish past that. Its
`flags`:

| Flag | |
|---|---|
| `HG_STALE` | set slot flag bit 0 whatever the references say ([Stale inputs](#stale-inputs)) |
| `HG_NO_REFS` | record no references at all, `Inputs` included: an output with no input behind it |

**What a reference means.** By default, the message most recently returned. That is
wrong in two cases, and both are recorded rather than guessed:

- **Reused input.** If nothing new was read from an input since the previous publish
  (a 100 Hz loop on a 50 Hz input, or an input that stopped), its reference carries
  the `reused` flag: the output was computed from a message already used before.
- **Pipelined module.** A module that reads message *n* but publishes the result after
  having read *n + 1* (a network working on one frame while the next arrives) says
  which message it used: `hg_use(sub, &info)` before publishing makes that message the
  reference, instead of the most recent one.

```c
hg_next(frames, &frame, &info);
hg_use(frames, &info);          // this frame is what the next publish was computed from
...
hg_publish(p, &detections, info.stamp_ns);
```

An output published with no input behind it at all (an actuator's neutral when its
gate is closed) is published with `HG_NO_REFS`
([07-safety](07-safety.md#the-actuator-gate)).

## Stale inputs

A message is only as fresh as what it was computed from. At each publish, the library
checks the rated topic of every reference the message records: each `Inputs` topic,
and each `Sources` topic the module named for this message. One that is stale (older
than its `stale_after_ns`), dead, closed or absent gets the `stale` (or `absent`) flag
in its reference, and the message gets slot flag bit 0, **computed from stale
inputs**. The flag is transitive: a message any of whose referenced messages carries
bit 0 carries it too, across the tether as well ([Mirrored topics](#mirrored-topics)).
So a depth controller that keeps running on a frozen depth publishes flagged commands,
the mux's output is flagged when it passes one on, and the actuator gate treats a
flagged command as no command ([07-safety](07-safety.md#the-actuator-gate)). Nothing in
the chain needs to know why; the flag says that something behind it is not current.

A topic the module may reference but did not reference in this message is not checked:
a mux whose unused source is absent or stale does not flag its output. Nor is a
reference to a topic of the other side (`Inputs=vehicle:cam.pilot`,
[11-electric-eye](11-electric-eye.md#video_relay)): that topic does not exist on this
machine. A module may also set bit 0 itself, with `HG_STALE`, when its output rests on
something stale that its references do not show: `hangar-policy` does when any `input`
part of its dataset is missing or stale, and when it has no model to run
([09-learning](09-learning.md#features-at-run-time)); the mux does when a source it
used but could not reference, past 4, carries the flag
([07-safety](07-safety.md#the-mux)). No module can clear the bit.

Module authors write nothing extra in the normal case, and the recorder gets exact
input→output pairs for every module without matching timestamps
([06-recorder](06-recorder.md)). A module that fuses more than four inputs records
the four that matter, or publishes an intermediate topic.

## Mirrored topics

A topic received over the tether is written on this machine by the bridge
([08-bridge](08-bridge.md#mirrored-topics)), the one module allowed `hg_publish_seq`.
It differs from a local topic in five ways.

- **The sender's numbers and boot.** Each message keeps the sender's `seq`, and the
  header's `origin_boot` holds the sender's boot id, so `(origin_boot, topic id, seq)`
  names the message on both machines. When the bridge opens a mirrored topic, it gives
  the manager the sender's boot id and the first seq it will publish; the manager
  creates the file with that `origin_boot` and with `first_seq` one below that seq, so
  the join point does not count as a gap. A file of that name with the same
  `origin_boot` (a restarted bridge) has its `write_seq` continued instead, as for a
  local topic. When the sender's boot id changes, which the bridge learns at the
  handshake, the manager creates a new file for each mirrored topic in that way and
  sets bit 2 on the old one; its readers reopen and start a new epoch
  ([A replaced file](#reading)).
- **Numbers only rise.** `hg_publish_seq` refuses a seq at or below `write_seq`.
- **Gaps are lost or skipped messages.** A seq missing on this side was lost on the
  tether, or skipped on purpose by a mirror with `Rate=`. The bridge sets slot flag
  bit 1 on the first message after a deliberate skip, so readers count those seqs in
  `skipped`, not in `gap`. A slot is chosen by seq, so a ring with gaps in its numbers
  holds fewer than `slot_count` messages.
- **Flags and provenance carried.** The sender's slot flags and references are
  published unchanged: a message computed from stale inputs on one side is flagged on
  the other, and its references name messages of the other side, whose topic ids are
  unique across the pair ([Names](#names)). A stamp of 0 stays 0.
- **Rate and staleness from the mirror.** `rate_mhz` and `stale_after_ns` come from
  the mirror's `ExpectRate=` and `StaleAfter=`, required for `safety` and `command`
  priority mirrors, not from a manifest. A received topic going stale flags its
  readers' outputs; it never counts against the bridge's health.

## Reading from Python

```python
import mmap, struct, numpy as np
m = mmap.mmap(open("/dev/shm/hangar/nav.depth", "rb").fileno(), 0, access=mmap.ACCESS_READ)
schema = m[512:512 + struct.unpack_from("<I", m, 156)[0]].decode()
dtype = hangar.dtype_from_schema(schema)    # ~40 lines, in tools/python
```

On x86-64 the newest message can be read with the algorithm above in pure Python
(`struct.unpack_from`), whose stores and loads the processor keeps in order. On ARM it
cannot: Python neither orders its loads nor reads 8 bytes in one access on ARMv7, so a
torn copy could pass the check. There the Python package reads through a small C
helper (`hg_read_latest`, by `ctypes`) that does the seqlock read properly; pure Python
is for x86-64 and for files no one is writing.

## Permissions

Topic files are created by the manager: owned by its user, group `hangar`, mode
0640. Every module (all are in `hangar`) can open any topic read-only by its path,
without asking the manager. Only the writer can write, through the descriptor the
manager handed it; and the manager, as the owner, can always set flags and wake
readers.

That holds between modules that share a user only because every module is
non-dumpable (`PR_SET_DUMPABLE` 0): otherwise a process of the same user could trace
another, or copy its descriptors (`pidfd_getfd`, `/proc/<pid>/fd`), and so write its
topics. A process is dumpable again after `exec`, so a module inherits nothing worth
taking across it but its sealed, read-only wiring `memfd`. `hg_init` first sets
`PR_SET_DUMPABLE` 0, then fetches its writable module page and its call socket from the
manager over the control socket (`attach`: the manager identifies it by `SO_PEERCRED`
and passes both with `SCM_RIGHTS`,
[03-manager](03-manager.md#starting-a-module)). The manager hands them only to a
module that no process is tracing (`TracerPid` 0 in `/proc/<pid>/status`), since a
tracer that attached before `hg_init` would keep its hold; once the module is
non-dumpable, no process of the same user can attach. Its topic descriptors come later
still, from `hg_pub_open`. Modules that must not trust each other at all run as
different users; an actuator module always does
([02-manifest](02-manifest.md#module-required-once)).

## Memory

A topic takes `4096 + slot_count × slot_stride` bytes: a 16-slot depth topic is 7 KB
(a 16-byte payload, 192-byte slots; 8 KB of pages);
a 4-slot 1280×720 I422 frame topic is 7.4 MB. All of it is reserved at open.

## electric-eye streams

hangar does not read or write `EEYEBUS1` files itself. An adapter in the library opens
an electric-eye stream (`/dev/shm/eeye.<name>`) as a read-only hangar topic of the
matching frame type, so that modules can read `eeye`'s `pilot` and `raw` before
electric-eye is ported ([11-electric-eye](11-electric-eye.md)).

## Tests

- Torn reads: a writer overtaking a reader mid-copy, at every slot count, under
  concurrent processes.
- Restarts: a writer killed and restarted continues `seq`; an `hg_next` reader sees no
  gap; a reader of the old file reopens.
- Wake-ups: no lost wake-up between check and sleep, over many iterations; no wake
  system call with no waiter (counted with `strace -c` or seccomp); a reader killed
  asleep costs one wake call, then none.
- Absent topics: `hg_sub_open` returns at once; the module attaches when the topic is
  created; the directory word wakes it; a topic created between the open attempt and
  the sleep is not missed. A manager restart and a topic replacement leave `.dir` and
  every wake file on the same inode.
- `hg_wait_fd`: messages published while the module reads are not missed. A writer's
  file replaced, and an absent topic created, with no other traffic: the next publish
  to the new file still signals the `eventfd`, and no mapping is unmapped while the
  helper uses it.
- `hg_wait`: calls pending and topic news reported in one ready set; a client sending
  requests back to back does not stop topic news being reported.
- `hg_next` over a topic written by `hg_publish_seq` with jumps both shorter and longer
  than `slot_count`: no `HG_NONE` while news is pending, `gap` equal to the missing
  seqs, seqs before a message with slot flag bit 1 counted in `skipped`;
  `hg_read_seq` of a missing seq returns `HG_GONE`.
- Slack: a reader reading every `slot_count − 1` messages loses nothing; one reading
  every `slot_count` messages loses one each time.
- Mirrored topics: `hg_publish_seq` refuses a seq at or below `write_seq` and keeps the
  slot flags it is given; a new sender boot id gives a new file with the new
  `origin_boot`, whose readers change `epoch` and count no gap; the first message of a
  new mirror counts no gap.
- Provenance: `reused` set exactly when nothing new was read; `hg_use` overrides;
  `hg_publish_with` refuses a reference outside `Inputs` and `Sources` and more than 4
  references; a `<role>:<topic>` reference is recorded as given and never checked.
- Stale inputs: an input going stale flags the output, transitively through two
  modules and across a mirror; an input recovering clears it. A `Sources` topic that is
  absent or stale and not referenced does not flag the output; `HG_STALE` sets bit 0.
- Permissions: a module holds neither its page nor its call socket before it is
  non-dumpable; `attach` is refused to a traced module.
- `futex_waitv` over many topics; a dead writer flagged by a stand-in manager; a
  replaced file announced by bit 2 and reopened.
- A freshly replaced file before its first publish: `HG_NONE`, no spinning.
- `hg_next` past a slot left odd by a writer that died mid-publish: counted as lost,
  no livelock.
- All of it on AArch64 and ARMv7 under full-system emulation (`qemu-system` with a
  5.16 or later kernel) or on a Pi, since QEMU's user-mode emulation implements neither
  `futex_waitv` nor `clone3`; and under ThreadSanitizer with the seqlock copy annotated.
- The budgets of [00-overview](00-overview.md#budgets), measured on the release build.
