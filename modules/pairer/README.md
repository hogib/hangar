# The pairer, datasets and the offline build

Module identifier `pairer`, binary `pairer` (unnamed as yet in
[docs/names.md](../../docs/names.md)): the live dataset pairer of
[docs/06-recorder.md](../../docs/06-recorder.md), "Datasets" and "Live: the pairer".
It serves the dataset specs its configuration lists: for each anchor message it
joins the parts from their rings and publishes one row on `dataset.<name>`, and
once a second its status, per dataset and part, on `pairer.status`. The same join
code, in `lib/hg/hg_dataset.c`, builds the same rows from a session's recordings in
`hangar dataset build`, and is what a policy module will run at its tick.

GPL-3.0-or-later, C11, libc and `lib/hg` only; static for musl. The dataset code is
LGPL in `lib/hg`, so a policy module and the offline build share it.

## Layout

| File | |
|---|---|
| `pairer.c` | the process: `pairer.conf`, the subscriptions, binding a spec to the topics' types, the anchor loop, the pending rows and their waits, publishing, the sink, `pairer.status` |
| `../../lib/hg/hg_dataset.[ch]` | the spec parser, the row type and its schema, `Where`, the join over a message source, the reset rule (LGPL) |
| `../../lib/hg/hg_record.[ch]` | the `.hgr` files the build reads and the sink writes (LGPL) |
| `../../hangar/hangar_dataset.c` | `hangar dataset build`, `check`, `schema`, `pairs` |
| `../../python/hangar/dataset.py` | the spec and the rows in Python |
| `../../msg/pairer_status.msg` | the status type |
| `../recorder/recorder.mk` | the make fragment, shared with the recorder |
| `../recorder/tests/test_rec_dataset.c`, `test_rec_pairer.c` | the suites |

## The library: `hg_dataset.h`

**The spec** (`hg_ds_parse`, `hg_ds_read_file`): docs/06's `[Dataset]` and `[Input
<part>]` sections with every key of its tables, parsed with the manifest syntax
(`hg_ini.h`) and as strictly; the defaults are the tables' (`AnchorRole=label`,
`Rate=every:1`, `Filter=drop`, `Missing=flag`, `ResetAfter=200ms`, `Lateness=100ms`,
`Window=1`, `Role=input`, `Topic` the part's name). `Where` is checked for syntax at
parse and for its fields at bind. `hg_ds_wait_ns` is the row's wait, the largest
`offset + Lateness` of the asof joins, at least 0; `hg_ds_policy_usable` names the
`input` asof joins that would have to wait; `hg_ds_check_manifests` checks each
provenance join against the anchor's `Inputs` and `Sources` in a directory of
manifests.

**The row type** (`hg_ds_bind`): from the spec and the types of the anchor and the
parts (as parsed from topic headers or recording headers), the row schema is written
and parsed back with `hg_schema_parse`, so the layout is the `.msg` layout of that
text, natural alignment and all, and its hash is a type hash like any other:

```
u64 anchor_seq # role=meta
u64 anchor_stamp_ns # role=meta
u8 boot_id[16] # role=meta the boot that numbered the anchor
u32 missing # role=meta bit i: part i missing
u32 flags # role=meta bit 0 reset, bit 1 masked, bit 2 late
u64 emitted_ns # role=meta when the live row's joins closed
f32 nav.state.depth_m # role=input m, positive down
...
u64 nav.imu._seq # role=input
u64 nav.imu._stamp_ns # role=input
u32 nav.imu._count # role=input
u32 nav.imu._flags # role=input
f32 nav.imu.0.gyro[3] # role=input rad/s, body x y z
f32 nav.imu.1.gyro[3] # role=input rad/s, body x y z
```

The header is docs/06's at offsets 0, 8, 16, 32, 36, 40; then the anchor's fields
named `<anchor>.<field>`, then per part `<part>._seq`, `_stamp_ns`, `_count`,
`_flags` and its fields, `<part>.<field>` or `<part>.<k>.<field>` over a window (k
from 0, the oldest, to K-1, the joined message). Every field's comment starts with
`role=` and continues with the source field's comment (the units); the source
comments are dropped when the text would pass 3584 bytes, and a schema still longer,
or with more than 128 fields, is refused. `hangar dataset schema <spec> <session>`
prints the text. A frame part takes the field `data[N]` of its open topic's size.

**The join** (`hg_ds_join`) reads a part through a `struct hg_ds_source`, two
functions, `read_seq` and `find_stamp`, which `hg_ds_source_sub` backs with an
`hg_sub` (`hg_read_seq`, `hg_find_stamp`) and `hg_ds_source_file` with a recording
(`hgr_find_seq`, `hgr_find_stamp`). A provenance join takes the seq the anchor names
for the part's topic id (the part is missing when the anchor names none, or names an
absent one); an asof join takes the newest message stamped at or before anchor time
plus offset, the anchor's stamp or, when 0, its publish time. A window of K reads the
K-1 seqs before the joined one and stops at the first that is not there; `_count`
says how many. A missing part is zeros with its bit set; its `_flags` still carry the
anchor's reference flags (bits 8-10) when it was a provenance join. The join reports
whether the part is stale: slot bit 0, reference bit stale, or, for an asof join to a
rated topic, a message older than the topic's `StaleAfter` before the join time.

**The row** (`hg_ds_row_begin`, `hg_ds_row_finish`): begin zeroes the row, fills the
header (`emitted_ns` = anchor `publish_ns` + the wait) and the anchor's fields;
finish applies `Missing`, evaluates `Where` and applies `Filter`, and sets `reset` by
docs/06's rule: the first row, an anchor gap the reader saw, a new epoch, and an
`input` part missing or stale for `ResetAfter` or longer measured on anchor stamps.
`hg_ds_rate_keeps` applies `Rate`. `late` is the build's to set: it compares each
joined message's `publish_ns` with the row's `emitted_ns`.

## The pairer

`pairer [<pairer.conf>]`. The configuration, manifest syntax, strict:

| `[Pairer]` key | Default | |
|---|---|---|
| `Datasets` | required | the specs, by name, separated by blanks |
| `DatasetDir` | `/etc/hangar/datasets` (under `$HANGAR_ROOT`) | where `<name>.dataset` is |
| `Sink` | none | until the bridge: a directory in which the rows of each dataset are also appended to `dataset.<name>.hgr` |
| `Pending` | 256 | rows waiting for their `emitted_ns`, per dataset |

For each dataset it subscribes to the anchor and the parts by name (all together at
most 127, one `hg_wait`), and binds the spec once every topic exists, exiting 78 if
the spec and the types do not agree (a field that does not exist, a `Where` field
that is an array). It then opens `dataset.<name>` with the row type and, with a
`Sink`, creates the sink file with the same header the build writes.

The loop waits on every subscription, 10 ms at most or until the oldest pending row's
`emitted_ns`. For each anchor message read in order it applies `Rate`, begins a row,
joins at once the parts that do not wait (provenance joins and asof joins with
`offset + Lateness <= 0`) and queues the row; rows whose `emitted_ns` has passed have
their other parts joined, are finished and published, in order, with the anchor's
stamp. When more anchors are waiting than `Pending` holds, the oldest row closes
early with what has arrived (counted, logged at exit). `pairer.status` carries, in a
round a second, one message per dataset (rows, dropped, masked) and one per part
(misses, the largest latency from a message's publication to its join, the part's
ring depth and the depth that latency needs, `(Slots - 1) / rate` above it, as a
power of two).

Under the manager the manifest must publish exactly the `dataset.<name>` topics the
configuration lists, or the pairer exits 78 saying which differs; unwired, the check
is skipped. The manifest's `Type=` for a dataset output is the row type: see
deviation 1.

## Offline: `hangar dataset`

```
hangar dataset build <spec> <session> [-o <out.hgr>]
hangar dataset check <spec> [<policy spec>] [<session>]
hangar dataset schema <spec> (<session> | --msgdir DIR --type <topic>=<msg> ... [--rate <topic>=<Hz>] ...)
hangar dataset pairs <session> <module>
```

`<spec>` is a name (`DatasetDir/<name>.dataset`, `DatasetDir` from `hangard.conf`
under `$HANGAR_ROOT`, else `/etc/hangar/datasets`) or a path ending in `.dataset`.
`hangar dataset <spec> <session>` alone is a `build`. `build` opens `<session>/<anchor>.hgr` and each part's file, refuses a part recorded
other than `full` (naming it) and a topic with more than one epoch in the session,
binds the row type from the files' headers, runs the join over every anchor record in
order with the recordings as sources, sets `late` where it joined a message published
after the row's `emitted_ns`, and writes the rows as a recording of `dataset.<name>`
(the row type's schema in the header; record seq counting from 1, stamp the anchor's,
publish time the row's `emitted_ns`). `check` prints one `OK`, `WARN` or `FAIL` line
per check: the spec; the manifests under `$HANGAR_ROOT/etc/hangar/modules` (a warning
when none publishes the anchor); whether a policy can run on it, naming the `input`
joins that would wait; the recurrent conditions; with a policy spec, that its
`Dataset=` is this spec; with a session, that every part was recorded `full` and the
row type binds. Exit status 0, 1 for a warning, 2 for a failure. `schema` prints the
row schema from a session's headers or from `.msg` files by name. `pairs` prints, from
`<session>/manifests/<module>.module`, one spec per output with `Inputs`: the output
as the anchor, each input a provenance join.

```sh
hangar dataset check depth_ff /var/lib/hangar/recordings/20261107-153000-rov
hangar dataset build depth_ff /var/lib/hangar/recordings/20261107-153000-rov -o depth_ff.hgr
python3 -c 'import hangar.dataset as d; ds = d.open_dataset("depth_ff.hgr"); print(len(ds), ds.fields("label"))'
```

## Live and offline

docs/06 promises the live and the offline rows identical except where a row says
otherwise, and the `rec_pairer` suite checks it on one run: the toy chain publishes,
the recorder records a session and the pairer writes its sink at the same time; the
build then runs over the session and every live row is compared with the offline row
of the same anchor. They are identical except: rows the build flags `late` (there
they may differ in the late part and in that part's missing bit); the first rows'
reset flags; a part one row alone marks missing (the pairer's, gone from its ring; the
build's, lost by the recorder), which moves the reset flag of the rows after it; and
the first rows of a recording, whose windows reach before its start and so hold fewer
messages than the live rows had (`_count` says so). `emitted_ns` is the same value in
both. The `rec_dataset` suite checks the same on a chain built in-process with
controlled stamps: a message published after its stamp joined by the build with
`late`, a part gone from a 4-slot ring missing live alone, windows at the start of a
stream, a stale range and a reused reference in `_flags`, the reset rule at a gap, a
new epoch and `ResetAfter`.

## Tests

In `rec_dataset`: docs/06's two example specs parsed field by field; 47 refused forms
(every table value out of range, every unknown or repeated key, `Where` syntax, parts
twice, a part without `Join`, keys before a section, a file not named `.dataset`);
the row type's offsets by hand for a spec with an anchor selection, a window, a filter
part and an asof part, the schema's role comments, the refusals at bind (a field that
does not exist, `rate:<Hz>` on an event anchor, a `Where` field outside the spec or an
array or dropped by `Fields=`, a row past 128 fields), the manifests check; `Where`
with every operator, `and`, `or`, parentheses and a missing part; the chain above
live and from recordings; `Rate=every:4` choosing the same anchors from two starting
points and `Filter=drop`; the reset rule alone; `Missing=drop`. With `HGR_FIXTURE_DIR`
set it writes `chain.dataset` and `dataset.chain.hgr` for `python/tests/test_dataset.py`.

In `rec_pairer`: the run above (about 250 common rows, about 90 late), the dataset
topic's slots and schema, a row referencing nothing unwired, `pairer.status` per part
with the misses of a part referenced every fourth message, `hangar dataset check`
(`WARN` for the manifests, `OK` for the session), `build`, `schema` from the session
and from `.msg` files hashing to the row type the pairer published, `check` and
`build` refusing a spec whose part the session does not hold, `pairs`; exit 78 for a
missing spec, a bad key, an empty list and a wiring whose dataset outputs differ.

## Deviations from docs/06, with reasons

1. **The manager does not generate `dataset:<name>` types**, and refuses a manifest
   that declares one (`hangard/README.md`, deviation 10). Until it does, a pairer
   manifest declares the row type as an ordinary type whose `.msg` file holds the row
   schema: `hangar dataset schema <spec> --msgdir ... --type <topic>=<msg> ...` (or
   from a session) prints the text; written to `MsgDir/dataset_<name>.msg`, a manifest
   says `Type=dataset_<name>` and the manager accepts the open, since the hash is the
   row type's (the generator refuses dotted names in `msg/`, so the file lives in the
   machine's `MsgDir` only; `hg_schema_parse` accepts them). The pairer's own check of
   the manifest against its configuration (exit 78) is by the `dataset.*` outputs'
   names. Untested under `hangard`; the suite wires the pairer by a file.
2. **`Sink=`: the rows also go to a file**, `<Sink>/dataset.<name>.hgr`, the same bytes
   as the topic and the same header the build writes (`/* until the bridge */` in the
   code): the trainer's transport is the bridge, which does not exist, and the live
   rows had to be compared with the build's somewhere.
3. **`Pending=`** bounds the rows waiting for their `emitted_ns`; docs/06 does not say
   what happens when anchors arrive faster than rows close. The oldest row closes
   early, with what has arrived, and the count is logged at exit.
4. **A row references its anchor message** when the wiring allows the reference (the
   anchor among the dataset output's `Inputs` or `Sources` in the manifest); unwired,
   or when it does not, the row is published without references. docs/06 does not
   say what a row references.
5. **Immediate joins happen when the anchor is read**, which is after its publication
   by the pairer's own latency; docs/06 says "at once, as a policy does". A message
   published in that interval is joined live and by the build alike, with `late` set
   by the build only if it was also published after `emitted_ns`.
6. **`Where` is false when any part it names is missing**, not comparison by
   comparison: docs/06's "a part flagged missing makes the condition false" read
   whole, so `or` does not revive a condition on a missing part.
7. **`Where` fields are scalars**; an array or `char` field is refused at bind. A
   window part's field without an index means the joined (last) message,
   `<part>.<k>.<field>` a particular one.
8. **The reset rule counts a stale or missing `input` part from the anchor stamp at
   which it became so**, and the row is flagged while the span reaches `ResetAfter`;
   with `ResetAfter=0` every row with such a part is flagged.
9. **`late` is the build's alone**, as docs/06 says, and the build also marks `late`
   on a row whose message the live row did join (one published in the pairer's read
   latency, deviation 5): then the two rows are identical and the flag is set.
10. **The build handles one epoch per topic**, refusing a session with `<topic>+1.hgr`
    for a topic the spec names: joining a part's seq across epochs needs the boot id
    per epoch, which the bridge will bring.
11. **`hangar dataset check` with a policy spec** compares `Dataset=` and says the
    weights are not checked: there is no policy module. `loop-latency` is not
    implemented (no weights, no lineage). `pairs` reads the session's copy of the
    manifest alone.
12. **The build's records take `emitted_ns` as their publish time** and count seqs
    from 1; the sink's take the publishing time and the topic's seqs. Neither is a
    row field, so the rows compare equal.
13. **Limits**: 16 parts, 32 selected fields per part (name them with `Fields=` when a
    type has more), a window of 64, 128 row fields in all (the schema's bound), 64
    terms in `Where`, a `Where` of 511 bytes, 8 datasets per pairer.
14. **Durations** take `ns`, `us`, `ms`, `s` and `min`; an asof offset may be negative
    (`asof:-50ms`).

## Until the bridge

- `Sink=` (deviation 2) stands in for the bridge's delivery of `dataset.<name>` to
  the topside; the topic itself is published as docs/06 says.
- Epochs in the build (deviation 10).
- `boot_id` is the anchor topic's `origin_boot` when set, this machine's otherwise;
  only the second case is exercised.
- The trainer's side (`hangar.stream`, the PyTorch dataset) is docs/09's and not here;
  `python/hangar/dataset.py` reads the rows and the spec.
