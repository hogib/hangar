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

# Trained weights for a policy (docs/09): what the topside's trainer, or an
# operator, sends with bridge.send lands on the vehicle in
# /var/lib/hangar/bridge/files/policy_depth_ff/.
[Files policy_depth_ff]
Direction=down
MaxSize=16MB
Allow=trainer admin

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
| `FileRate` | a quarter of `MaxRate` | The rate files are held to ([Files](#files)); at most `MaxRate` |

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

| `[Files <dest>]` key | Default | |
|---|---|---|
| `Direction` | — (required) | `up` or `down`: the one way files of this destination go |
| `MaxSize` | `16MB` | The largest file, in bytes, `KB` or `MB`; at most `256MB`. Checked by the sender when it takes the file and by the receiver when one is begun |
| `MaxFiles` | `4` | Files of this destination the receiving side keeps: when one more has arrived, the oldest are removed |
| `Allow` | `admin` | Who may send to this destination: callers on the sending machine, as a method's `Allow` names them there (a module, `*`, `admin`) |

A `[Files]` section names a destination, `[a-z0-9_]`, at most 32 characters, and no
directory: where its files land is the receiving bridge's to decide ([Files](#files)).

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

[Serve send]
Request=bridge_send_req
Reply=bridge_send_rep

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
the manager, it is per-machine infrastructure. Its method `send` is never remote
(whatever `Remote=` matches, the bridge gets no `subglob *.req` line and so reads no
request topic of the other side), its reply topic `bridge.rep` is never mirrored, and
its own outputs carry the machine's role. The manager adds the outputs, as it adds
every module's implicit topics; the method is declared in the manifest, as any
module's is:

| Output | Rate | |
|---|---|---|
| `bridge.<role>.link` | 10 Hz, and on change | the link's state ([Link status](#link-status)), published whether the link is up or not |
| `bridge.<role>.budget` | 1 Hz, and on change | the rate `MaxRate` leaves for video ([Video and the tether](#video-and-the-tether)) |
| `bridge.<role>.files` | event | how each file sent or received ended ([Files](#files)); only with a `[Files]` section in the configuration |
| method `send` | | send a file ([Files](#files)); `Allow` is the default, `* admin`: every module on this machine and `admin`. Which of them may send to which destination is the `[Files]` section's `Allow` |

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
broadcast address, and listens there (IPv6's `ff02::1` and a beacon on every interface
that is up are not built: one IPv4 broadcast is sent, by the route the system gives it):

```
HANGAR/1 role=vehicle pair=default port=5614 id=1a2b3c4d tag=9f0c…
```

`tag` is the keyed hash of the rest of the line, under the shared key. `id` is random,
drawn when the bridge starts.

**A beacon decides nothing.** Its tag is under the shared key, which never changes,
and nothing in the line says when it was made: a recorded beacon checks for as long
as the key is in use, from whatever address it is sent. So a beacon whose tag checks,
of the opposite role and the same pair, makes a *candidate*: an `id` and the address
it came from (with the line's `port`), kept for 5 s after it was last heard. The same
`id` from two addresses is two candidates. At most 8 are kept; a ninth takes the
place of the one heard longest ago. A candidate is somewhere to send a hello, and no
more:

- a bridge with `Peer=auto` and no link sends its hello to a candidate when that
  candidate's beacon arrives, at most twice a second to each (a candidate that was
  replaced and is heard again is a new one: one hello for one beacon is the most);
- whoever is there and holds the key answers it, and the handshake goes on with the
  address that answered ([The link](#the-link)). The peer is who completes it: its
  address is set by the first datagram that checks under the session keys;
- no beacon sets the peer or its address, changes anything of a link that is up, or
  keeps a link from being made. A replayed one costs the hellos sent to it.

**Two peers.** That somebody is there now is shown by a hello that echoes the nonce
this side offers now ([The link](#the-link)), and by nothing in a beacon. A bridge
with `Peer=auto` and no link notes the `id` of every such hello. When two `id`s have
echoed the same nonce, two machines hold the key and answer: the bridge chooses
none, drops the handshake it had begun, and acts on no hello while they both answer
and for 5 s after they last did. The state is `ambiguous`, and `detail` says `two
peers hold the key and answer: none chosen`. Beacons of two `id`s are not that: one
may be a recording, or the peer's bridge before it restarted. But while candidates
of two `id`s are kept, the first hello that echoes the nonce is not acted on until
500 ms after it came, so that a second peer, if there is one, has answered before
keys are made with the first; the handshake goes on with the next such hello, which
the next beacon brings. A replayed beacon can thereby delay a link by about a
second, and cannot do more.

This holds for peers heard before the link is made. A second peer whose beacon first
arrives after the handshake with the first has completed is not looked for: beacons
change nothing of a link that is up.

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

**Handshake.** Each side sends a hello holding its role, its pair, a random nonce of
16 bytes, its machine's boot id (`/proc/sys/kernel/random/boot_id`), the peer's nonce
it answers (zeros in a first hello), and the id of the link whose keys it holds (0:
none). A hello that carries the receiver's own role or its own nonce is refused.

A hello is tagged under the shared key, which never changes, so a recorded one checks
for as long as that key is in use. What makes a hello count is the echo: a side acts
only on a hello that **echoes the nonce it is offering now**, which nobody could have
written before that nonce went out, nor without the key. A nonce is offered for one
handshake: it is drawn again as soon as keys made from it are in force, and 5 s after
it first went on the wire if no handshake completed. So three things can happen to a
hello whose tag checks:

- it names the link whose keys are in force here: the peer still holds them and lost
  the link in an outage. The keys stand; a confirm under them is the answer;
- it does not echo the nonce offered now (a first hello, or a recorded one sent
  again): nothing is kept of it and nothing changes. The answer is this side's hello,
  echoing the nonce received;
- it echoes the nonce offered now: it is fresh, and its keys become the pending ones.

No hello sets the peer's address. With `Peer=auto` the peer is where the last
datagram came from whose tag checked under a session key and whose `packet_seq` was
new; a hello, whatever its source, is answered to the peer while a link is up, and to
its source only when there is no link to keep.

**A fixed peer.** With `Peer=<address>` the peer is that address and port (`Port`
when the key names none; a host name is looked up once, when the bridge starts), and
nothing changes it. A datagram from any other address or port is dropped before
anything of it is read: no tag is computed, nothing is answered, and nothing moves
but the count of refused datagrams (`rejected`). So the other side has to send from
the address `Peer=` names. This is a rule about addresses, not a second
authentication: a datagram that carries the peer's address is judged by its tag and
its `packet_seq`, as every datagram is.

From the two nonces, `n_v` the vehicle's and `n_t` the topside's, each side derives
one key per direction:

```
K_up   = BLAKE2s-256(key = K, "up"   ‖ n_v ‖ n_t)    vehicle to topside
K_down = BLAKE2s-256(key = K, "down" ‖ n_v ‖ n_t)    topside to vehicle
```

A side tags what it sends with its own direction's key and checks what it receives
with the other: a datagram tagged with the receiver's own sending key (one of its own
datagrams sent back to it) fails the check. Each side then sends a confirm under its
new key. The link is up on a side once it has received a datagram that checks under
the peer's new key and carries a packet sequence. Until then, a link already up stays
up with its old keys, and a peer without the shared key cannot produce a valid tag.

**What a recording is worth.** Someone on the tether who holds no key and has
recorded earlier traffic can send it again. A recorded hello changes nothing on the
side that receives it: it is answered with one hello, sent to the peer of a link that
is up, and it sets no key. A recorded session cannot complete a handshake: its
datagrams are tagged under keys made from a nonce this side has since replaced, and
keys are never made twice from the same pair of nonces. The one recording that still
counts is of a handshake under way, sent again within the 5 s its nonce lives and
before that handshake completes: it is the peer's own, current hello, delivered a
second time, and the keys it leads to are those the peer holds. What such a person
can still do: delay or drop datagrams if they sit in the path (a link cut, which the
staleness of every mirrored topic shows), and make each side send one hello per
replayed hello: to its peer while the link is up, and, with `Peer=auto` and no link,
back to the address the replay came from. With `Peer=auto` and no link, a replayed
beacon is answered with a hello too, sent to the address it came from, and beacons
of two `id`s delay the link by about a second ([Discovery](#discovery)). With a fixed
`Peer=`, a replay is read only if it carries the peer's address and port. They cannot
bring a link up, keep one from being made, move one that
is up to other keys or to another address, or put a message on a topic. And a link made of replayed datagrams, were there one,
would get no clock estimate (its pongs answer no ping of this link), so nothing it
carried would be published ([Clocks](#clocks)).

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
topic's slot count drops the oldest and records the gap. The receiving side bounds
what the sender's numbers make it reserve: a mirrored topic's ring is at most 32 MB
(fewer slots than the sender's when that would be more), and the rings of all
mirrored topics together at most 256 MB, past which a topic is not mirrored. The
bridge holds itself to both, and the manager refuses an open that passes either
([03-manager](03-manager.md#opening-topics)). A record
on the reliable stream larger than a framed 1 MB message is passed over. The schema of a mirrored
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
that pair, and is counted in the uncertainty. With every new link (a handshake
completed: the peer rebooted, or its bridge restarted) the samples are discarded and
the estimate starts over; it also ends when its newest sample is older than the 10 s
window.

**Without an estimate nothing is translated.** A stamp or a deadline put into this
clock with no offset, or with the offset of the peer's previous boot, is wrong by the
difference of two uptimes: days, between a laptop and a Pi just powered. A command
stamped in the future passes `MaxAge`, a deadline in the future never expires, and a
future stamp, once published, has every later stamp of its topic clamped above it.
So, from the handshake until the first pong that answers a ping of this link (one
round trip: the first ping goes with the handshake's last step), and whenever the
estimate has ended:

- a `latest` message is dropped and counted in `dropped_late`. For a command this is
  the safe side: `pilot.wrench` is not published on the vehicle, and goes stale or
  stays absent there, as when the tether is cut;
- a `reliable` message waits. The receiver stops handling the stream at the first
  message record (the version and schema records before it are handled). What
  follows is kept in its buffer, 1 MB, so the usual wait of one round trip costs no
  resend; past that the sender keeps what is unacknowledged and resends it, each
  time after twice the pause, up to a second, while nothing is acknowledged.
  Everything is delivered in order once the estimate exists. A request made meanwhile is answered late, or is past its
  deadline by then and dropped by the server as expired; none is lost and none is
  executed on a deadline nobody could read.

`detail` in `bridge.<role>.link` says `no clock estimate: nothing mirrored in` while
it lasts, and `clock_uncertainty_ns` is 0.

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

A file crosses the tether when a `[Files <dest>]` section of the configuration
([Configuration](#configuration)) says files of that destination go its way. Neither
side is given a path:

- on the sending side a file is `<name>` in the bridge's outbox,
  `/var/lib/hangar/bridge/outbox/`;
- on the receiving side it becomes `/var/lib/hangar/bridge/files/<dest>/<name>`, mode
  0640, in the bridge's own state directory. Whoever uses the files of a destination
  reads that directory and needs no right to write in it: a policy names it as
  `Incoming=` in its spec ([09-learning](09-learning.md#policy-modules)).

A name is 1 to 64 of `A-Z a-z 0-9 . _ -` and does not begin with a `.`; both bridges
check it. There is no `/` in it and it is neither `.` nor `..`, so a name joined to a
directory stays in it.

**Sending.** `bridge.send {dest, name}` is a call to the bridge of the caller's own
machine. It is refused, with the reason, when the configuration has no `[Files <dest>]`
in this machine's sending direction; when the section's `Allow` does not name the
caller; when the name is not one; when `outbox/<name>` is missing, is not a regular
file with one link (a symbolic link is not followed), is empty or is over `MaxSize`;
and when 8 files are waiting already. Otherwise the bridge opens the file, removes its
name from the outbox (the file is the bridge's from there on) and replies at once with
the transfer's `id`. Files go one at a time, in the order asked.

**On the wire.** A file is records on the reliable stream ([Delivery](#delivery)), so
its datagrams are acknowledged, resent and authenticated as any are:

| Record | From | |
|---|---|---|
| begin | sender | `id` (random, 64 bits), size, `dest`, `name`, and the caller's token as the sending side names callers |
| status | receiver | `ready` at the offset the receiver has (0 for a new file), `done`, or `failed` with one line of reason |
| data | sender | `id`, offset, at most 1024 bytes |
| end | sender | `id` and the BLAKE2s-256 of the whole file, as the sender read it |
| abort | sender | `id`: the sender gave the file up |

The receiver holds a begin to its own copy of the configuration (a `[Files <dest>]`
toward this side, the name, the size within `MaxSize`), whatever the sender's copy
says, and answers `failed` otherwise. It writes the data to a hidden name
(`.part-<id>`) in the destination's directory, hashing as it writes, and takes data
only at the offset it is at. At the end, if the size is the one announced and its hash
of what it wrote is the sender's, it renames the file to `<name>` (replacing a file of
that name), removes the oldest files beyond `MaxFiles`, and answers `done`; otherwise
it removes what it wrote and answers `failed`. So nothing of a file is under its name
before the whole of it has arrived and checked. The tag on each datagram says who sent
it; the hash says the file is whole and the one the sender read, and catches a file
that changed under the sender while it was resent, or a mistake in going on after a
new link.

The file is not synced to storage before it is renamed: the bridge's loop also carries
the pilot's commands, and a wait for an SD card would hold them. While the machine
runs, a name holds a whole file or nothing. After a power cut within the kernel's
writeback delay it may hold less; the reader checks what it reads (a policy holds a
weights file to the hash `load_weights` names, [09-learning](09-learning.md#loading-weights)).

**Bulk.** Files are held to `FileRate`, and a chunk goes on the stream only while less
than 16 KB waits there unacknowledged, so a request or a reply published during a
transfer has at most that much file in front of it. This is what `bulk` priority is as
built: the bridge has one reliable stream and does not yet order what it sends by
priority, nor slow `dataset` and `bulk` by the round trip ([Delivery](#delivery)).
`latest` topics (commands, telemetry) are datagrams of their own and wait behind
nothing.

**When the link drops.** A link that comes back keeps its keys and its streams
([The link](#the-link)): the file goes on by the stream's own resends. A new link (one
bridge was restarted) has new streams: the sender begins the file again and goes on
from the offset the receiver answers, which is 0 when it was the receiving bridge that
restarted, since a restarted bridge removes what an earlier one left unfinished. A
sending bridge that restarts forgets its queue: those files are not sent, and no
message on its side reports them failed (a caller that waits for one gives up on its
own timeout). On either side a transfer in which nothing has moved
for 60 s (for a queued file, counted from its turn) is given up: the receiver removes
what it wrote, the sender reports the file
failed.

**How it ended.** `bridge.<role>.files`, an event topic, one message per transfer that
ended, on the side that sent and on the side that received:

| Field | |
|---|---|
| `id` | the transfer's id: `bridge.send`'s reply |
| `state` | 1 sent (the other side has it, checked and under its name), 2 received, 3 failed sending, 4 failed receiving (nothing of it is visible) |
| `dest`, `name`, `size` | |
| `blake2s` | BLAKE2s-256 of the whole file, 64 hex digits; empty when it was never whole |
| `from` | the caller of `bridge.send`: `trainer`, `admin:<user>`. On the receiving side it is the sender's word |
| `at_ns` | this machine's `CLOCK_MONOTONIC` when the transfer ended |
| `reason` | with a failure, one line |

The caller of `send` waits for the message with its `id`: state 1 says the file is on
the other side. A file's arrival starts nothing there. Loading weights is a call of its
own, `policy_<name>.load_weights`, with its own `Allow` and the policy's checks
([09-learning](09-learning.md#loading-weights)).

**Who can do what.** This path writes files on the vehicle, so:

- `send` is called on the sending machine only. The topside's requests travel down on
  its mirrored admin topic like any, and the vehicle's bridge does not read them.
- Two lists admit a caller: the method's `Allow` in the bridge's manifest (who may ask
  at all) and the section's `Allow` (who may send to this destination; by default
  operators only).
- A caller admitted to a destination can put files of at most `MaxSize`, under names
  of the characters above, into that destination's directory on the other side; it
  can replace a file there by sending one of the same name, and push older files out
  through `MaxFiles`. It cannot write anywhere else, cannot make the bridge read a
  file outside the outbox, and cannot make the receiving side hold more than `MaxFiles`
  files of `MaxSize` and one unfinished file.
- The section's `Allow` is checked by the sending bridge. Someone who holds the key,
  or has the other machine's bridge, is held only to what the receiving side checks:
  the destinations of its own configuration, the names, `MaxSize`, `MaxFiles`. Such a
  peer can also keep the receiver writing an unfinished file over and over, at the
  rate the link carries; the storage's wear from that is not bounded.
- The outbox is shared by everyone who may write in it. A file there can be replaced
  by another writer before `send` takes it; the caller compares the `blake2s` of the
  `sent` message with the hash of what it meant to send (`hangar.learn.deploy` does),
  and for weights the policy's first check does the same on the vehicle.
- Files travel as everything on the tether does: authenticated, not encrypted
  ([The link](#the-link)).

With modules under their own users ([13-deployment](13-deployment.md)), the bridge's
state directory is `hg-bridge`'s, mode 0750, the outbox 0770 and a destination's
directory 0750: a module that writes the outbox (the trainer) or reads a destination
(a policy) has the bridge's group among its `SupplementaryGroups`, and with it can read
every destination's files. The suites run every module as one user; the arrangement
with separate users is not tested.

## Link status

`bridge.<role>.link`, 10 Hz and on change, whether the link is up or not:

| Field | |
|---|---|
| `state` | `off`, `searching`, `connecting`, `up`, `ambiguous` (two peers answer, none chosen: [Discovery](#discovery)), `refused` |
| `peer` | address, name and `id` |
| `peer_boot_id` | the other machine's boot id, from the handshake |
| `last_rx_ns` | when the last datagram whose tag and `packet_seq` checked arrived (on the vehicle, the safety module's `tether` trigger) |
| `rtt_ns`, `rtt_min_ns` | the last round trip, and the minimum of the last 10 s |
| `loss_up`, `loss_down` | datagrams lost, from `packet_seq` |
| `rate[6]` | bytes per second sent, per priority |
| `low_rate` | the rate allowed now to `dataset` and `bulk` ([Delivery](#delivery)) |
| `clock_offset_ns`, `clock_skew_ppm`, `clock_uncertainty_ns` | the estimate ([Clocks](#clocks)) |
| `clamped` | translated stamps clamped to keep a mirrored topic's stamps monotone ([Clocks](#clocks)) |
| `dropped_old`, `dropped_late` | mirrored messages dropped: seq not above the last published; older than `MaxAge`, or a `latest` message received with no clock estimate ([Clocks](#clocks)) |
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
  peer reconnects. A whole recorded session, hello first, sent again from the peer's
  address: into its own session, after the peer rebooted, with the peer gone, and
  after this side's bridge restarted: no link comes up or changes, nothing is
  published. With `Peer=auto`, a recorded hello sent from another address: nothing
  is sent there and nothing sent to the peer is lost; a peer restarting under a
  stream of old hellos links again.
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
- Files (the bridge's suite, runs `files`, `filestall`, `filerekey`): a file of 150 KB
  down at `FileRate=800kbit`, no file under its name while it arrives, then the same
  bytes, mode 0640, the hash both sides name the file's own, the name gone from the
  outbox, in the time the rate gives; during it a call across answered and a 50 Hz
  `command` topic arriving whole. The tether cut midway for longer than the link's
  timeout and mended: the partial kept under its hidden name, then the whole file, no
  bridge restarted. The receiving bridge restarted midway: sent again from the start.
  Both bridges made to forget their keys when the link goes down: a new link, and the
  sender goes on from the byte the receiver had. A byte changed after the sender
  hashed it: failed on both sides, nothing visible, nothing hidden left. Two sends at
  once: both arrive, in order. `MaxFiles`: the oldest removed. Refused by `send`: a
  name with a path, a hidden name, a missing file, one over `MaxSize`, a symbolic
  link, a second hard link to the bridge's key, an unknown destination, one of the
  other direction, a caller the section's `Allow` does not name. The topside's
  requests arriving on the vehicle and its bridge answering none. A file up. The
  sending bridge restarted midway, and the tether cut for longer than the wait (1.5 s
  in the suite): given up on the side that waits, nothing left, and the next file
  goes through. What a peer may put in a begin (a path as name or destination, a
  hidden name, a size over `MaxSize`, data out of place or past the size) is in the
  fuzz harness's seeds, which checks after every input that nothing is on disk but
  under a destination's directory and a name a file may have. Not tested: separate
  users, a power cut before writeback, files over a megabyte, a real tether, the
  storage's speed on the Pi against the loop's latency.
- Dataset rows with a `cam.nn` frame under 1% datagram loss: every row arrives whole.
- Discovery, `Peer=auto` on both sides: with beacons of nine earlier `id`s of each
  side replayed from another address, with the beacons on the air sent again from
  another address, and with old hellos sent at both sides, a link is made, its peer
  the address that answered, and no state is `ambiguous`; two vehicles that hold the
  key and both answer: the topside links with neither and says `ambiguous`, and links
  once one is gone; the peer's bridge restarted, and the peer off for 3.5 s and back:
  linked again, never `ambiguous`; another pair, refused.
- A fixed `Peer=`: a recorded hello, and a datagram of the session itself with a
  `packet_seq` far ahead, each from another port: refused and counted, nothing sent,
  the replay window where it was, the link up.
