# Tether bridge and discovery

How topics, calls and files cross the tether. One module on each side, `bridge`,
finds the other, authenticates it, and carries what its configuration lists: sensor
and state topics up, pilot commands down, dataset rows up to the trainer on the
topside GPU, trained weights down ([09-learning](09-learning.md)). Video does not go
through the bridge: it stays H.264 over RTP, as in electric-eye. No MAVLink crosses the
tether either: the topside's `topside_mavlink` speaks to QGroundControl on the topside
alone, from the topics mirrored up
([10-mavlink](10-mavlink.md#the-topsides-mavlink-module)).

The bridge generalizes electric-eye's `hellion`: its discovery, its relay and its link
statistics, with authentication added.

## The rules

- **A mirrored topic is the same topic on both sides**: same name, same type, and,
  within one boot of the sender, the same sequence numbers. `(boot id, topic id, seq)`
  names one message on either end of the tether for good, so provenance and dataset
  joins need no translation.
- **Names are unique across the pair.** Module and topic names, and topic ids, are
  unique across the vehicle and the topside together. The two bridges compare their
  machines' lists at the handshake and mirror no name that both sides have.
- **One release on both sides.** The exchange after the handshake carries the release
  version and the hash of the installed `msg/` tree; if either differs, `config_match`
  is false and nothing is mirrored
  ([13-deployment](13-deployment.md#version-lock-step-across-the-tether)).
- **Loss is visible.** A message lost on the tether is a gap in the mirrored topic's
  sequence numbers. A gap a mirror makes on purpose (`Rate=`) is marked as such.
- **Commands are never delayed by the bridge's own queues.** Traffic is sent in strict
  priority order under a total rate cap: safety and commands first, files last. Since
  the two directions share one medium on the tether, `dataset` and `bulk` traffic
  backs off when the link's delay rises.
- **Nothing unauthenticated is accepted.** Every datagram, discovery beacons included,
  carries a tag computed with a key shared by the two ends, and each direction has its
  own key. Authentication covers the tether; on each machine, the manager vouches for
  which module writes which topic.
- **Times are translated.** The bridge measures the offset between the two machines'
  clocks and stamps mirrored messages in local time; no time daemon is needed on the
  tether.
- **A call across the tether is two mirrored topics.** The caller's request topic goes
  over and the server's reply topic comes back, reliably, with the same deadlines, the
  same refusal of expired requests, and the caller named as a module of the other side
  ([Calls](#calls-across-the-tether)).
- **A silent link is the link's state, not the bridge's failure.** The bridge is
  judged by its own watchdog and its own rated outputs, `bridge.<role>.link` and
  `bridge.<role>.budget`, never by the staleness of the topics it receives.

## Configuration

One file, `/etc/hangar/bridge.conf`, identical on both sides; `up` is from the vehicle
to the topside. A bridge's role is its machine's: `Role=vehicle` or `Role=topside` in
`hangard.conf` ([03-manager](03-manager.md#running-it)).

```ini
[Bridge]
Pair=default
Key=/etc/hangar/bridge.key
Port=5614
MaxRate=20Mbit

[Mirror safety.state bridge.vehicle.link]
Direction=up
Priority=safety
ExpectRate=10
StaleAfter=300ms

[Mirror hangar.vehicle.system hangar.vehicle.modules]
Direction=up
Priority=safety
ExpectRate=1
StaleAfter=3s

# pilot.buttons is published with every pilot.wrench.
[Mirror pilot.wrench pilot.buttons]
Direction=down
Priority=command
ExpectRate=50
StaleAfter=100ms
MaxAge=100ms

# What the topside shows the pilot and the operator, and re-emits to QGroundControl.
[Mirror nav.* power.battery vehicle.leak mux.state ardusub.state recorder.status *.params]
Direction=up
Rate=10

# Event topics whose every message matters.
[Mirror ardusub.param policy.*.active policy.*.load]
Direction=up
Delivery=reliable

# A row that carries a cam.nn frame is 17 datagrams.
[Mirror dataset.*]
Direction=up
Priority=dataset
Delivery=reliable

[Calls]
Remote=safety.* mux.* recorder.* mavlink.setup installer.* *.set_params policy_*.*
```

| `[Bridge]` key | Default | |
|---|---|---|
| `Pair` | `default` | Only a bridge with the same pair name is accepted. A name, not a secret |
| `Key` | `/etc/hangar/bridge.key` | 32 random bytes, made at installation and copied to the other side |
| `Port` | `5614` | Data. Discovery is on 5613 (not `hellion`'s 5612, so both can run side by side: [11-electric-eye](11-electric-eye.md#ports)) |
| `Peer` | `auto` | `auto` (discovery) or the other side's fixed `address`. The one key that may differ between the two copies; the configuration hash leaves it out |
| `MaxRate` | `20Mbit` | Total sending rate of this side, all priorities ([Priority](#delivery)), the video stream outside the bridge included ([Video and the tether](#video-and-the-tether)) |

| `[Mirror <topic or glob> ...]` key | Default | |
|---|---|---|
| `Direction` | — (required) | `up` or `down` |
| `Delivery` | `latest` | `latest` or `reliable` ([Delivery](#delivery)) |
| `Priority` | `telemetry` | `safety`, `command`, `call`, `telemetry`, `dataset`, `bulk` |
| `Rate` | every message | The most messages per second sent, newest first. The receiver marks what it skipped ([Mirrored topics](#mirrored-topics)) |
| `ExpectRate` | `0` (event topic) | The rate, in Hz, the receiving side declares for the topic: its file's `rate_mhz`. Required for `safety` and `command` priority |
| `StaleAfter` | 3 periods of `ExpectRate` | When the received topic is stale for its readers: its file's `stale_after_ns`. Only with `ExpectRate`. Required for `safety` and `command` priority |
| `MaxAge` | none | `command` priority only: a message older than this on arrival is dropped ([Mirrored topics](#mirrored-topics)) |

A `safety` or `command` mirror names its topics; a glob there is refused. The
receiving side's readers act on `ExpectRate` and `StaleAfter`: on the vehicle,
`pilot.wrench` goes stale 100 ms after the last command arrived, and the mux counts it
as zero ([07-safety](07-safety.md#the-mux)).

`[Calls] Remote=` lists the methods of the other side that this side's modules may
call through the bridge. It is shorthand: each manager expands it into `[Mirror]`
sections for request and reply topics, at `call` priority, from its own manifests
([Calls](#calls-across-the-tether)).

Each side sends what its configuration says goes its way, and accepts only what its
configuration says comes to it. A bridge sends only topics written on its own machine,
never one it receives. The two bridges compare a hash of their configurations when
they connect; a difference is reported in `bridge.<role>.link` and by `hangar check`,
and each side still accepts only what its own copy allows.

## The bridge module

The manifest of the module gives it what no other module has:

```ini
# bridge.module
[Module]
Exec=/usr/lib/hangar/bridge /etc/hangar/bridge.conf
User=hg-bridge

[Bridge]

[Health]
Watchdog=1s
```

A `[Bridge]` section ([02-manifest](02-manifest.md#bridge)) allows the module:

- to read the topics its configuration sends, without declaring them;
- to publish the topics its configuration receives, of any type (the schema comes from
  the other side), provided no local module publishes them, with the sender's sequence
  numbers and slot flags (`hg_publish_seq`).

At most one module per machine has it. The manager reads the bridge's configuration
(`BridgeConf` in `hangard.conf`, `/etc/hangar/bridge.conf` by default) and builds the
module's wiring from its `[Mirror]` sections and the expansion of `[Calls]`
([03-manager](03-manager.md#wiring)).

The module is named `bridge` on both machines, the one module name the two share: like
the manager, it is per-machine infrastructure. Its method `send` is never remote, its
reply topic `bridge.rep` is never mirrored, and its own outputs carry the machine's
role. The manager adds them, as it adds every module's implicit topics:

| Output | Rate | |
|---|---|---|
| `bridge.<role>.link` | 10 Hz, and on change | the link's state ([Link status](#link-status)), published whether the link is up or not |
| `bridge.<role>.budget` | 1 Hz, and on change | the rate `MaxRate` leaves for video ([Video and the tether](#video-and-the-tether)) |
| `bridge.<role>.files` | event | files received ([Files](#files)) |
| method `send` | | send a file ([Files](#files)); `Allow` is the default, `* admin`: every module on this machine and `admin` |

The topics the bridge receives count neither for its readiness nor against its health:
they go silent whenever the link does, and a tether outage must not restart the
bridge. The manager judges the bridge by its rated outputs, `bridge.<role>.link` and
`bridge.<role>.budget`, and by `Watchdog`.

The bridge waits on its UDP socket and its topics together, through `hg_wait_fd`
([01-topics](01-topics.md#reading)). It waits by futex on the named topics of its
`safety`, `command` and `call` mirrors, and polls the topics its globs match on a 10 ms
timer of its own, the cadence the library keeps for any module's glob subscriptions
([01-topics](01-topics.md#reading)). It makes no calls of its own.

## Discovery

From `hellion`: every bridge announces itself once a second on UDP 5613, to the IPv4
broadcast address and IPv6 `ff02::1` of every interface that is up, and listens there:

```
HANGAR/1 role=vehicle name=rov-1 pair=default port=5614 id=1a2b3c4d tag=9f0c…
```

`tag` is the keyed hash of the rest of the line, under the shared key. `id` is random,
drawn when the bridge starts. Beacons with the same `id` are one candidate, from
whatever address they come. A bridge with `Peer=auto` connects to the one candidate of
the opposite role and the same pair heard in the last 5 s: it tries the handshake at
each address it heard that `id` from, and keeps the first that completes. With two or
more candidates it chooses none and says so (`ambiguous`).

A beacon only starts a connection attempt; whether the other side holds the key is
settled by the handshake, so a replayed beacon gains nothing, and a beacon never
changes the peer of a link that is up.

## The link

UDP, one socket each side. Every datagram:

| Offset | Type | Field | |
|---|---|---|---|
| 0 | char[4] | `magic` | `"HGB1"` |
| 4 | u8 | `kind` | hello, confirm, ping, pong, schema, message, fragment, data, ack |
| 5 | u8 | `priority` | |
| 6 | u16 | `flags` | |
| 8 | u32 | `link_id` | chosen in the handshake |
| 12 | u32 | `length` | of the body |
| 16 | u64 | `packet_seq` | per direction, rising by one |
| 24 | | body | at most 1200 bytes |
| end | u8[16] | `tag` | keyed BLAKE2s-128 of header and body, with the sender's direction key; for a hello, with the shared key |

**Handshake.** Each side sends a hello holding its role, its pair, a fresh random
nonce of 16 bytes, and its machine's boot id (`/proc/sys/kernel/random/boot_id`). A
hello that carries the receiver's own role or its own nonce is refused. From the two
nonces, `n_v` the vehicle's and `n_t` the topside's, each side derives one key per
direction:

```
K_up   = BLAKE2s-256(key = K, "up"   ‖ n_v ‖ n_t)    vehicle to topside
K_down = BLAKE2s-256(key = K, "down" ‖ n_v ‖ n_t)    topside to vehicle
```

A side tags what it sends with its own direction's key and checks what it receives
with the other: a datagram tagged with the receiver's own sending key (one of its own
datagrams sent back to it) fails the check. Each side then sends a confirm under its
new key. The link is up on a side once it has received a datagram that checks under
the peer's new key. Until then, a link already up stays up with its old keys: a hello
replayed from an earlier connection starts at most an attempt that never completes,
and a peer without the shared key cannot produce a valid tag.

Once the link is up, the two bridges exchange, reliably, their release version and the
hash of their installed `msg/` tree, their configuration hash, and their machines'
module lists and topic tables (name and id of every topic their managers know, mirrored
or not). Mirroring starts when all of it is checked. A release version or a `msg/` hash
that differs sets `config_match` false and nothing is mirrored: there is no
compatibility mode between versions, and `detail` names the two versions
([13-deployment](13-deployment.md#version-lock-step-across-the-tether)). A module or
topic name that both sides have, or a topic id that two names share, is not mirrored;
`config_match` and `detail` in `bridge.<role>.link`, and `hangar check`, name it.

**Replays.** `packet_seq` must be new: a window of the last 64 numbers received in
each direction, as in IPsec, admits reordering and drops duplicates and replays. The
window, and `last_rx_ns` in the link status, are updated only by a datagram whose tag
checks.

**No encryption.** The tether is a cable; what needs protecting is that nobody else
can be the topside or inject commands, which the tag gives. BLAKE2s is a small, single
file in either language.

## Delivery

| Delivery | How | For |
|---|---|---|
| `latest` | sent once, never retransmitted; a message larger than one datagram is fragmented, and a lost fragment loses the message | commands, telemetry |
| `reliable` | acknowledged (cumulative and selective), retransmitted, delivered in order | request and reply topics, files, event topics whose every message matters, dataset rows (a row with a frame spans many datagrams) |

Mirrored messages are at most 1 MB; a `reliable` mirror whose backlog passes its
topic's slot count drops the oldest and records the gap. The schema of a mirrored
topic is sent reliably before its first message.

**Priority.** A strict order: `safety`, `command`, `call`, `telemetry`, `dataset`,
`bulk`; within one priority, topics take turns. `MaxRate` caps the total each side
sends. Inside the bridge, a command never waits behind a lower priority.

The link itself is shared. Tether modems (HomePlug-based) are half-duplex: rows going
up and commands going down use the same medium, along with the video outside the
bridge, and when the modems' capacity falls below what is sent, packets queue in the
modems' own buffers, commands behind rows. Each side therefore limits its `dataset`
and `bulk` traffic, retransmissions included, by the round trip it measures
([Clocks](#clocks)):

- when a ping's round trip exceeds the minimum of the last 10 s by more than 5 ms, the
  rate allowed to the two classes is halved, but not below 1% of `MaxRate`;
- each ping that does not raises it by 1/20 of `MaxRate`, up to what `MaxRate` leaves.

`safety`, `command`, `call` and `telemetry` are never slowed; they are small. The
vehicle's upload thereby yields to the topside's commands, and the reverse.

## Video and the tether

The H.264 stream from the encoder does not pass through the bridge
([11-electric-eye](11-electric-eye.md#encoder)), but it shares the tether, and on the
vehicle's side it is most of what goes up. `MaxRate` is therefore the total for bridge
traffic *and* video: the bridge publishes `bridge.<role>.budget`, at 1 Hz and on
change, with the rate left for video, and the encoder follows it.

```
u64 max_rate_bps     # MaxRate
u64 bridge_bps       # what the bridge is sending now, all priorities,
                     # retransmissions included (the sum of link's rate[6])
u64 video_bps        # max_rate_bps - bridge_bps, 0 when nothing is left
```

The vehicle's `encoder` reads `bridge.vehicle.budget` and sets x264's bitrate from
`video_bps`, within its own cap and above its own floor
([11-electric-eye](11-electric-eye.md#encoder)); in placement A, `topside_encoder`
follows `bridge.topside.budget` the same way. The bridge does not read the encoder's
rate: when video and bridge traffic together exceed the modems' capacity, the round trip
rises, and the bridge's `dataset` and `bulk` classes back off ([Delivery](#delivery)),
which raises the budget again. `safety`, `command`, `call` and `telemetry` are never
slowed by the video, and the video's floor is never taken by the bridge: the encoder
keeps its floor, and the bridge's back-off makes room for it.

## Mirrored topics

The receiving bridge has the manager create each mirrored topic, with the sender's
schema, the mirror's `ExpectRate` and `StaleAfter`, and the sender's boot id as the
file's `origin_boot` ([01-topics](01-topics.md#mirrored-topics)). It publishes every
message with:

- the sender's sequence number (`hg_publish_seq`). Numbers only rise: a `latest`
  message whose seq is not above the last one published on the topic (a duplicate, or
  one overtaken on the way) is dropped and counted (`dropped_old`), and
  `hg_publish_seq` refuses a seq that does not rise;
- `stamp_ns` translated into local monotonic time ([Clocks](#clocks)); a stamp of 0
  (unknown) stays 0;
- `publish_ns` the local publication time;
- the sender's slot flags: bit 0, computed from stale inputs, as the sender had it;
  bit 1 set when the sending bridge skipped messages on purpose before this one
  (`Rate=`), so readers count those seqs as skipped, not lost;
- the provenance unchanged, with each reference's flags: its `(topic id, seq)` pairs
  refer to messages of the other side, which mean the same message wherever that topic
  is mirrored or recorded;
- the payload unchanged, except the two time fields of a call envelope, which are
  translated ([Calls](#calls-across-the-tether)).

A gap without bit 1 is a message lost on the tether, or one sent while the link was
down. The link's own loss is counted from `packet_seq` (`loss_up`, `loss_down`).

**Sender boots.** A mirrored topic's numbers are the sender's for one boot of the
sender. When a mirrored topic is first created, its `first_seq` is one below the first
seq received, so the join point is not a gap. When the handshake brings a new boot id
for the peer (the other machine rebooted), the manager creates a new file for each
mirrored topic, with the new `origin_boot` and `first_seq` one below the new sender's
first seq, and sets bit 2 on the old file. Readers reopen it and start a new epoch
(`info.epoch` changes), which is not counted as a gap. A link that drops and comes
back with the same boot ids continues the same files.

**Age of commands.** A message can wait in a tether modem's buffer for seconds and
still arrive. For a `command` mirror with `MaxAge`, the receiver computes the
message's age from its translated `stamp_ns` (the sender's translated `publish_ns` if
the stamp is 0), adds the clock's uncertainty, and drops the message, counted in
`dropped_late`, when the sum exceeds `MaxAge`. A pilot command dropped this way leaves
`pilot.wrench` to go stale on the vehicle, as a lost one does.

So the dataset pairer runs on the vehicle, where the inputs and their rings are, and
the rows it publishes reach the trainer through a `dataset.*` mirror.

## Clocks

Each bridge pings the other ten times a second. Pings and pongs carry
`CLOCK_MONOTONIC_RAW` times, which no time daemon slews (the topside laptop usually
runs one). From each exchange (sent `t1`, received there `t2`, answered there `t3`,
received back `t4`) it computes the round trip `(t4 − t1) − (t3 − t2)` and the offset
`((t2 − t1) + (t3 − t4)) / 2`. A pong that answers no ping this side sent, or gives a
round trip that is not positive, is discarded.

Each sample has an **uncertainty**: half its round trip plus its age × 500 ppm. The
two clocks drift apart while the sample ages, crystals by tens of ppm each, and the
bound is set well above that. Over a window of the last 10 s the bridge keeps the
sample with the smallest uncertainty, not merely the shortest round trip: a sample with
a short round trip taken 9 s ago can be worse than a fresh one with a longer round trip,
and the shortest round trip alone would keep the old one. Over the window's samples it
also estimates the **skew** between the two clocks, by a least-squares fit of offset
against local time, and applies the skew when it translates a stamp, so that the
estimate does not step each time the kept sample changes.

Translated stamps are **monotone** within each mirrored topic: a stamp that would go
backwards (the estimate moved between two messages) is clamped to the topic's last
translated stamp plus one nanosecond, and each clamp is counted in
`bridge.<role>.link` (`clamped`). Readers of a topic may therefore assume that stamps
rise with sequence numbers, as `hg_find_stamp` does
([01-topics](01-topics.md#reading)).

Stamps and deadlines are in `CLOCK_MONOTONIC`. Each side reads its `CLOCK_MONOTONIC`
and `CLOCK_MONOTONIC_RAW` together at every ping and converts between the two with
the newest pair; the conversion's error grows by at most 500 ppm of the time since
that pair, and is counted in the uncertainty. When the peer's boot id changes, the
samples are discarded and the estimate starts over.

The offset, the skew and the uncertainty are published in `bridge.<role>.link` and
recorded, so an offline tool can refine the alignment of two recordings afterwards.

## Calls across the tether

A call is two messages on two topics ([04-calls](04-calls.md#the-two-topics)): the
request on the caller's `<caller>.req`, the reply on the server's `<server>.rep`. A
call crosses the tether when those two topics are mirrored, the request topic toward
the server's side and the reply topic back, both `reliable` at `call` priority. The
server reads the mirrored request topic as it reads a local caller's, and the caller
reads the mirrored reply topic as it reads a local server's; nothing in either module
knows the call went over the tether.

A module that calls a method of the other side declares it with the types it was built
with (`[Call safety.arm]` with `Request=` and `Reply=` in the topside's `pilot_console`,
[04-calls](04-calls.md#declaring-calls)). `[Calls] Remote=` names the methods
that may be called across. Each manager expands it, when it builds the bridge's wiring,
from the manifests of its own machine:

| This machine | Sends toward the other side | Receives from it |
|---|---|---|
| has a module `C` with `[Call S.M]`, `S.M` listed, `S` not enabled here | `[Mirror C.req]` | |
| has the module `S`, serving a method the list names | `[Mirror S.rep]` | |
| has any entry in `Remote=` | `[Mirror hangar.<role>.admin.req]` | `[Mirror *.req *.rep]` |

every one of them `Priority=call Delivery=reliable`, with `Direction` set by the
machine's role. With the same file on both sides, `safety.*` sends `pilot_console.req`
and `topside_mavlink.req` down from the topside and `safety.rep` up from the vehicle;
`*.set_params` sends each caller's request topic toward the module it sets, on either
side. The receiving side takes request and reply topics by glob, since it does not
know the other side's callers before the handshake; a module serving a method the list
names gets a `subglob *.req` line in its wiring beside its named `sub <caller>.req`
lines ([03-manager](03-manager.md#wiring)), so it opens a mirrored request topic the
moment the bridge creates it and reads it within 10 ms, the cap `hg_wait` keeps in a
module with that line ([01-topics](01-topics.md#reading)), passing over requests
addressed to another module. A caller's `sub <server>.rep` line names the mirrored
reply topic, which is `HG_ABSENT` until the first reply arrives. A module enabled on
both machines is found at the handshake and reported by `hangar check`; with module
names unique across the pair, that is a configuration error.

- **The caller is named by the topic.** A request topic has one writer, which the
  manager of its machine guarantees ([04-calls](04-calls.md#allow-tokens)). Mirrored,
  it keeps its name and carries the sender's boot id in `origin_boot`, so a server
  reading `pilot_console.req` with `origin_boot` set knows the caller is
  `pilot_console` of the other side: `topside:pilot_console`, the role being the peer's.
  Nothing in the message asserts the caller, and neither bridge writes a name. `Allow`
  lists name remote callers by the tokens `topside:<module>`, `topside:*` (every
  module of the topside) and `topside:admin`. The default `Allow` admits nobody from
  the other side, so a method meant to be called across the tether names its callers.
- **Operators.** `hangar call` on the topside asks the topside's manager, the writer of
  `hangar.topside.admin.req`, which puts `admin:<user>` in the envelope's `caller`
  field. That topic is mirrored down by name, so a server trusts the field on it and on
  no other topic, and reads the caller as `topside:admin:<user>`.
- **Deadlines are translated.** The sending bridge rewrites `deadline_ns` in a request
  envelope into the other side's time, shortened by the clock's uncertainty, and
  `received_ns` in a reply envelope; these are the only payload bytes a bridge ever
  changes. A request whose deadline has passed on arrival is dropped unexecuted by the
  server with `HG_E_EXPIRED`, as any expired request; a reply that comes back after the
  caller's deadline is discarded by the caller.
- **Nothing is lost on the link.** Request and reply mirrors are `reliable`: while the
  link is up, every request and every reply arrives, in order. With the link down, a
  request waits in the caller's ring and the sending bridge's backlog; if the link is
  back before the deadline, it goes through, otherwise the caller has `HG_E_TIMEOUT`,
  and the server, receiving it later, drops it expired. At most 15 requests are in
  flight per caller, one below the request topic's 16 slots
  ([04-calls](04-calls.md#the-two-topics), [Delivery](#delivery)).
- **No link state in a status.** A `[Call S.M]` to a module enabled on neither side,
  or to one of the other side that `Remote=` does not name, is `HG_E_NOROUTE` from the
  library at once, from the wiring ([04-calls](04-calls.md#status-codes)). A call to
  the other side with the link down is `HG_E_TIMEOUT` when its deadline passes; the
  server's `.rep` topic, recorded on its side, says what happened: an answer, an
  `HG_E_EXPIRED`, or nothing, when the request never arrived.
- **Nothing waits.** Many calls may be in flight; neither bridge's loop, nor any
  module's, waits for one.

## Files

`bridge.send` (a call on the sending side) sends a file reliably, at `bulk` priority,
in chunks, resuming after a disconnection, and checks a BLAKE2s-256 hash of the whole
file before it is made visible. The receiver puts it in
`/var/lib/hangar/bridge/files/<name>` (group `hangar`, mode 0640) and publishes
`bridge.<role>.files`: `{name, size, hash, from, received_ns}`. Trained weights go
down this way, sent by the topside's `trainer` module
([09-learning](09-learning.md)), and recordings come up after a dive.

## Link status

`bridge.<role>.link`, 10 Hz and on change, whether the link is up or not:

| Field | |
|---|---|
| `state` | `off`, `searching`, `connecting`, `up`, `ambiguous`, `refused` |
| `peer` | address, name and `id` |
| `peer_boot_id` | the other machine's boot id, from the handshake |
| `last_rx_ns` | when the last datagram whose tag and `packet_seq` checked arrived (on the vehicle, the safety module's `tether` trigger) |
| `rtt_ns`, `rtt_min_ns` | the last round trip, and the minimum of the last 10 s |
| `loss_up`, `loss_down` | datagrams lost, from `packet_seq` |
| `rate[6]` | bytes per second sent, per priority |
| `low_rate` | the rate allowed now to `dataset` and `bulk` ([Delivery](#delivery)) |
| `clock_offset_ns`, `clock_skew_ppm`, `clock_uncertainty_ns` | the estimate ([Clocks](#clocks)) |
| `clamped` | translated stamps clamped to keep a mirrored topic's stamps monotone ([Clocks](#clocks)) |
| `dropped_old`, `dropped_late` | mirrored messages dropped: seq not above the last published; older than `MaxAge` |
| `config_match` | whether the two sides run the same release and `msg/` tree, the two configurations agree, and no module or topic name or id collides |
| `detail` | one line: why, or what; the two release versions when they differ |

## Recordings on both sides

When a recorder starts a session, its id is sent across, and each side's
`session.ini` names the other's session and the other machine's boot id: the two
recordings of a dive point at each other, `(boot id, topic id, seq)` names each
mirrored message in both, and the recorded clock offsets of `bridge.<role>.link` align
them. The topside's recorder is a module of another name than the vehicle's
(`topside_recorder`), as every module is.

## Tests

- Loss, reordering and duplication (`netem`): `latest` loses whole messages and shows
  the gaps; `reliable` delivers everything in order; duplicates and replays are
  dropped; a `latest` message overtaken by a newer one is dropped and counted, never
  published.
- A wrong key, a modified datagram, a replayed hello, a datagram or a hello reflected
  back to its sender: rejected, counted, and none of them moves `last_rx_ns` or the
  replay window.
- Handshake: a replayed hello leaves a live link up and never completes; a restarted
  peer reconnects.
- Priorities, over a link narrower than `MaxRate` and shared by both directions
  (`netem` and `tbf` at 5 Mbit for both together, with video): dataset rows going up
  and a file going down; once the rates settle, the median latency of `pilot.wrench`
  stays within 5 ms of its unloaded value, and `low_rate` falls and recovers.
- `Rate=`: a mirror at 10 Hz of a 200 Hz topic; readers on the other side count the
  skipped seqs as skipped and none as lost; `loss_up` counts only datagrams lost.
- Staleness: tether pulled with a non-zero stick; `pilot.wrench` on the vehicle stale
  within its `StaleAfter`; the bridge neither unready nor restarted through a 60 s
  outage.
- `MaxAge`: commands delayed 300 ms in a queue (`netem delay`) are dropped and counted.
- Boot epochs: the topside rebooted mid-run, then the vehicle; on the other side each
  mirrored topic gets a new file and a new epoch, no gap is counted, and commands and
  dataset rows resume. A first connection counts no gap.
- Clock: offset within its stated uncertainty against a known offset, under load, and
  with the topside's `CLOCK_MONOTONIC` slewed at 500 ppm; pongs with no ping and
  negative round trips discarded; a short-round-trip sample aged past a fresh one is
  replaced by it; `clock_skew_ppm` within 50 ppm of a known skew; no translated stamp
  on a mirrored topic below the one before it while the estimate moves, and every clamp
  counted.
- Versions: a peer at another release, or with another `msg/` hash: `config_match`
  false, nothing mirrored, both versions in `detail`, `hangar check` failing on both
  sides; the same release and `msg/` tree with another `bridge.conf`: mirrored, with
  `config_match` false and the difference in `detail`.
- Budget: `bridge.vehicle.budget` at `MaxRate` minus what the bridge sends, under load
  and idle; the encoder's bitrate follows it within one second, down to its floor and up
  to its cap, and the bridge's `low_rate` falls when the video alone fills the link.
- Flags and stamps: a message flagged as computed from stale inputs arrives flagged; a
  stamp of 0 arrives as 0.
- Names: a module or topic name, or a topic id, present on both sides: not mirrored,
  and reported in `config_match` and by `hangar check`; `bridge.rep` on neither side's
  mirrors.
- `Remote=` expansion: with the example file, the topside sends `pilot_console.req`,
  `topside_mavlink.req`, `trainer.req` and `hangar.topside.admin.req` down and
  receives `*.req *.rep`; the vehicle sends `safety.rep`, `mux.rep`, `recorder.rep`,
  `mavlink.rep`, `installer.rep` and each policy's `.rep` up; a server with a listed
  method has `subglob *.req` in its wiring and reads a mirrored request topic within
  10 ms of its creation.
- Calls: a request's `deadline_ns` and a reply's `received_ns` translated, the deadline
  shortened by the uncertainty, every other payload byte unchanged; a request expired
  on the way answered `HG_E_EXPIRED` and never executed; a reply after the caller's
  deadline discarded; `HG_E_NOROUTE` at once for a method no side serves and for one
  not in `Remote=`; with the link down, a request goes through when the link returns
  within its deadline, and is `HG_E_TIMEOUT` otherwise with nothing on the server's
  `.rep`; a remote call held for its whole deadline while `pilot.wrench` keeps its rate
  and pings keep their period.
- Remote callers: a server reads a mirrored `pilot_console.req` as
  `topside:pilot_console`, admitted only where `Allow` names it and refused by the
  default list; `hangar.topside.admin.req` read as `topside:admin:<user>`, and a
  `caller` field on any other topic ignored.
- Files: resume after the link drops; a corrupted file never becomes visible.
- Dataset rows with a `cam.nn` frame under 1% datagram loss: every row arrives whole.
- Discovery: one `id` heard from two addresses (a replayed beacon): the handshake picks
  the address that completes; two candidates, no choice; another pair, refused.
