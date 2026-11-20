# bridge: the tether between the vehicle and the topside

The `bridge` module of docs/08, stage 1: the two machines found and authenticated, the
topics `bridge.conf` lists carried up and down with their sequence numbers, boot ids and
stamps, and calls across the tether as their request and reply topics. With it the
topside role runs on the laptop and the vehicle role on the Pi, each under its own
manager, with no same-host shim (`tools/rov/README.md`, "The topside on the laptop").
The shim (`HANGAR_PEER_TOPIC_DIR`) stays, and stays the lab's default.

## Layout

| File | |
|---|---|
| `bridge.c` | the module: the link, the handshake, the two deliveries, the clock, `bridge.<role>.link` |
| `bridge_conf.h`, `bridge_conf.c` | `bridge.conf`'s reader and checks, and its hash; the manager links it too |
| `blake2s.h`, `blake2s.c` | BLAKE2s (RFC 7693), keyed; the manager links it too |
| `bridge.module`, `bridge.conf` | the manifest and the configuration templates (docs/08's example) |
| `bridge.mk` | `hangar-bridge` and `make check-bridge` |
| `tests/test_bridge.c` | the suite |
| `../../msg/bridge_link.msg` | `bridge.<role>.link` |

C11, libc and lib/hg only. The manager's side is in `hangard/` (`manifest.c`
`load_bridge`, `derive_bridge`; `wiring.c` mirror lines; `control.c` the mirrored open
and calls to the other side).

## What it does

- **The manager's side.** One module per machine has `[Bridge]`; without a readable
  `BridgeConf` it is refused, with `bridge.conf`'s line. The conf is read before the
  implicit topics: a call `[Calls] Remote=` names is remote, and its server gets
  `subglob *.req`. The bridge's wiring: `pub bridge.<role>.link` (10 Hz), a `sub` or
  `subglob` per topic it sends, a `mirror` line per topic it receives (the receiving
  file's `ExpectRate` and `StaleAfter`), and, with `Remote=`, the request topics of
  this machine's remote callers, the reply topics of its listed servers, its admin
  topic, and `mirror *.req`, `mirror *.rep`. A subscription the bridge brings is not
  warned about. The control socket's `open … boot=` is the bridge's alone, for a name a
  mirror line matches and no module here publishes, never `hangar.<role>.*` or
  `bridge.<role>.*` of this machine; `writer=` names the module that writes it on the
  other side (`hangar` only for the other side's `hangar.<role>.*`), `slots=` its ring.
- **The link.** One UDP socket (`Port`, 5614). Every datagram: a 24-byte header
  (`HGB1`, kind, priority, flags, link id, body length, packet sequence), at most 1 200
  bytes of body, and a 16-byte tag, keyed BLAKE2s-128 over header and body: under the
  shared key for a hello, else under the sender's direction key. A datagram whose tag
  does not check, or whose sequence is not new to a 64-wide replay window, is dropped
  and counted in `rejected`.
- **The handshake.** Each side's hello (92 bytes) carries its role, id, pair, a
  16-byte nonce, its boot id, the peer's nonce it answers and the id of the link whose
  keys it holds. Only a hello that echoes the nonce this side offers now is acted on:
  `K_up = BLAKE2s-256(key=K, "up"‖n_v‖n_t)`, `K_down` likewise, become the pending
  keys, a confirm under them goes at once, and the link is up on them when a datagram
  checks under them; the old keys stand until then. Any other hello is answered with
  this side's hello echoing its nonce, and changes nothing. A hello that names the
  link in force, after an outage, is answered with a confirm under the standing keys.
  The nonce is drawn again when keys made from it come into force and 5 s after it
  first went out, so a pair of nonces, and a session key, never comes twice: a
  recorded hello sets no key and a recorded session completes no handshake
  (docs/08 "What a recording is worth").
- **After the handshake** the two exchange the release (`HANGAR_VERSION`, `dev` when
  not set at build), the hash of `MsgDir`'s `.msg` files and the configuration's hash.
  A difference sets `config_match` false, says which in `detail` and the status line,
  and nothing is mirrored.
- **Two deliveries.** `latest`: every message read, each sent once as one datagram,
  `Rate=` skipping some (the next one sent marked `HG_SLOT_DECIMATED`). `reliable`
  (`Delivery=reliable`, and every request and reply topic): records on one ordered byte
  stream, acknowledged cumulatively and resent go-back-N after 4 × the smallest round
  trip (20 ms to 1 s); a record may span datagrams (an admin request is 16.5 KB). A
  topic's schema and writer go on the stream before its first message. Payloads go
  without their trailing zeros.
- **Mirrored topics.** Opened by the manager on the first message: the sender's
  schema, seq and boot id (`origin_boot`) and ring size, the writer the module there,
  the receiving file's rate and staleness. Seqs are kept (`hg_publish_seq`); a seq not
  above the last is dropped (`dropped_old`). A lost latest message is a gap a reader
  sees. The sending bridge keeps its own copy of every reliable message it has read,
  as the record to send, until the peer acknowledges it: at most as many per topic as
  the topic's ring has slots, the oldest going when one more comes (a gap the peer
  sees). After a reconnect a reliable topic resumes from the oldest one not
  acknowledged, so a request made while the link was down goes when it is back, and
  the server drops it if it is past its deadline. The copies are why a writer that
  restarts meanwhile loses nothing: its new file does not hold what the old one did.
- **Clocks.** Pings at 10 Hz on the up link; each pong gives an offset
  `((t2−t1)+(t3−t4))/2` and a round trip `(t4−t1)−(t3−t2)`; the estimate is the
  sample of least uncertainty `rtt/2 + age × 500 ppm` within 10 s. Every time on the
  wire is the sender's `CLOCK_MONOTONIC`; the receiver puts a message's stamp and
  publish time into its own clock, clamped to rise (`clamped`), drops a `command`
  message older than `MaxAge` with the uncertainty added (`dropped_late`), and
  rewrites a request's `deadline_ns` (shortened by the uncertainty) and a reply's
  `received_ns`, whatever the body after it. The estimate is of one link: it starts
  over at every handshake and ends when its last sample leaves the window. With no
  estimate nothing is translated: a `latest` message is dropped (`dropped_late`; a
  command is then not published here, the safe side), a `reliable` one waits on the
  stream behind the first message record, unacknowledged, and is delivered when the
  first pong has come (one round trip after the handshake: a ping goes with it).
  `detail` says `no clock estimate: nothing mirrored in` meanwhile.
- **Calls across.** A server reads the mirrored `<caller>.req` as any request topic;
  `origin_boot` makes its caller `<other role>:<caller>`, and the mirrored
  `hangar.<other role>.admin.req` (writer `hangar`) `<other role>:admin:<user>`. On the
  calling side, `hangar call` and the console's buttons reach a remote method through
  the manager's admin topic, with the types a module of that machine declares in its
  `[Call]` of it (`profiles/rov-topside/console.module` declares the console's).
- **Discovery.** `Peer=auto`: a beacon on `DiscoveryPort` (5613) each second,
  `HANGAR/1 role= pair= port= id= tag=`, the tag keyed BLAKE2s-128 under the shared
  key; the one candidate of the opposite role heard in the last 5 s is the peer, two
  is `ambiguous`. `Peer=<address>[:port]` fixes it (the lab: `192.168.2.10` and
  `192.168.2.1`). With `Peer=auto` the peer's address follows the datagrams that
  check under a session key and pass the replay window, and nothing else: a hello
  never moves it, and is answered to the peer while the link is up.
- **`bridge.<role>.link`** at 10 Hz: state, `config_match`, the peer's role, id, boot
  id and address, last heard, round trip (now and least), the counters (`loss_rx`,
  `rejected`, `retransmits`, `clamped`, `dropped_old`, `dropped_late`, messages each
  way, topics in), the clock's offset and uncertainty, `detail`. The status line says
  the same in short.
- **Waiting.** One `epoll`: the socket, the eventfd of `hg_wait_fd` over the named
  topics it sends, and a 10 ms timer (glob matches, pings, resends, the status topic).
  Every subscription in the wait set is read on every wake, so nothing is left unread. Idle with the link up: under 1 % of a core.

## Deviations from docs/08, with reasons

1. **The configuration hash leaves out `Key`, `MsgDir`, `Port`, `DiscoveryPort`,
   comments and blank lines**, as well as `Peer`. The key file's and the msg/ tree's
   paths are each machine's (the key is proved by the handshake, the tree by its own
   hash), and two bridges on one host, as in the suite, cannot share a port.
2. **The receiver translates every time**, the deadline included; docs/08 has the
   sending bridge rewrite the envelope. The sender knows no more than the receiver
   (the offset is symmetric), and one side doing all of it keeps the wire in one clock.
3. **`CLOCK_MONOTONIC`, not `CLOCK_MONOTONIC_RAW`, and no skew** (`clock_skew_ppm` 0):
   lib/hg stamps with `CLOCK_MONOTONIC`, and the 10 s window is short enough that
   500 ppm in the uncertainty covers the drift.
4. **Go-back-N, not selective acknowledgment**, with one stream for every reliable
   topic and every priority: simple and enough for a tether that loses little; under
   heavy loss a resend repeats what had arrived.
5. **A latest message larger than one datagram is not sent** (counted in
   `dropped_old`): there are no `fragment` datagrams. Nothing in `msg/` that is
   mirrored `latest` comes near 1 200 bytes once its trailing zeros are dropped.
6. **`MsgDir=`** in `[Bridge]` (default `/usr/share/hangar/msg`) and **`DiscoveryPort=`**
   (5613): the module is not handed `HANGAR_ROOT`, and a test needs the port.
7. **A mirrored topic's ring is the sender's** (`slots=` on the open), not 16: a
   burst after a resend fits as it did on the sending side. Up to 32 MB a topic
   (slots × (payload + 128)): a larger ring gets the most slots that fit, halved
   from the sender's, and says so in the log. Past 256 MB for all mirrored topics
   together a topic is not mirrored (the log says which). The peer names both
   numbers, and the manager reserves them in `/dev/shm`: unbounded, 4096 slots of
   1 MB each.
8. **The release string is `HANGAR_VERSION` at build** (`dev` without it), so two
   development builds of different trees match on the release and differ only by
   their msg/ hashes.

## Not in this stage (TODO)

- Priorities under the rate cap: `MaxRate` is read and not enforced, `Priority=` is
  carried in each datagram's header and orders nothing, and `bridge.<role>.budget` is
  not published.
- Files across the tether (`bridge.send`, docs/08 "Files").
- The video budget (docs/08 "Video").
- `hangar upgrade-vehicle`.
- `hangar check`'s report of `config_match` and of a module enabled on both machines;
  the name tables of docs/08 are not exchanged, so a collision is not detected.
- `fragment` datagrams for latest messages over 1 200 bytes.

## Running it

On each machine: `bridge.module` in `/etc/hangar/modules/`, the same `bridge.conf` in
`/etc/hangar/` but for `Peer=`, the same 32-byte key (`head -c 32 /dev/urandom`) as
`/etc/hangar/bridge.key`, readable by `hg-bridge` alone. `hangar status` shows the
bridge's line: `up <peer>, rtt …, offset …, N topics in, …`. On the lab's BlueROV2:
`BRIDGE=1 rov.sh up` on the Pi and `BRIDGE=1 tools/rov/rov.sh topside up` on the
laptop (`tools/rov/README.md`).

## Tests (`make check-bridge`, about 45 s)

Two managers, a vehicle and a topside, in two roots with two topic directories, each
with a bridge, and no `HANGAR_PEER_TOPIC_DIR`; between the bridges a UDP forwarder in
the test process that drops, replays, changes and forges datagrams on demand, holds
back one kind of datagram, and records what the topside sends so that a run can send
it again. Eight runs, each with managers of its own; `test_bridge --only <run>` runs
one (`main`, `mismatch`, `clocks`, `hold`, `replay`, `auto`, `writer`, `limits`).

`main`: the topside's bridge runs with a fake 5 s clock offset on the wire. Checked,
as one run printed them (2026-11-20, loopback, this laptop):

- the link up and `config_match` on both sides within 140 ms of the managers' start;
- `test.depth` mirrored up with the sender's seq (167 there, 167 here), writer
  `talker`, its boot id;
- `pilot.depth` down at 50 Hz: from the publisher's stamp to the vehicle's ring,
  median 0.10 ms, max 12 ms (the stamp translated through the 5 s offset); its
  `ExpectRate` and `StaleAfter` from `bridge.conf`, writer `pilot`;
- the offset measured +5000.000 ms on the vehicle and −5000.000 ms on the topside,
  uncertainty 0.02 ms, round trip 0.05 ms;
- a topic that falls silent goes stale on the receiving side and the link stays up;
- `server.secret` (vehicle) called from the topside's `tester`: `Allow=topside:tester`
  lets it in, 20 calls, round trip median 10.0 ms, max 10.2 ms (the server's `*.req`
  glob is polled every 10 ms, lib/hg's `GLOB_POLL_NS`; the bridge adds about 0.1 ms);
  `Allow=admin topside:admin` refuses it as `topside:tester`, and `hangar call` on the
  topside gets through as `topside:admin:<user>`;
- a replayed datagram, one with a byte changed, a hello under the wrong key, and a
  new datagram whose tag is wrong in its last byte only, or its first: each rejected
  and counted, the link up;
- 20 % loss each way for 3 s: the reliable `test.log` (200 Hz) arrives whole, 882
  messages with no gap, 67 resends; the latest `test.depth` shows 62 losses as gaps;
- CPU: 1.5 % and 1.0 % of a core with about 400 messages/s, 0.5 % and 0 % idle;
- the topside rebooted (its manager stopped, its topics gone, a new boot id):
  `pilot.depth` on the vehicle comes back on the new boot id, its readers' epoch 0 → 1,
  the seqs from 1 again;
- `mismatch`: a topside whose msg/ tree differs (one comment line): `config_match`
  false, `msg/ trees differ`, nothing mirrored.

The other runs, each written for a review finding and seen to fail before its fix:

- `clocks`: two clock bases. The topside's manager, its modules and its caller run
  in a time namespace three days ahead (skipped, and said so, where a user and time
  namespace cannot be made). The vehicle measures +259200.000 s; `pilot.depth`'s
  stamps come out in the vehicle's clock; a call whose OK reply is all zeros, as every
  `Reply=empty` method's is, is answered (0 of 5 before);
- `hold`: no clock estimate, nothing translated. The forwarder holds back the
  vehicle's pongs: for 1.2 s no `pilot.depth` is published there (62 counted in
  `dropped_late`) and a call's request is not handed to its server; with the pongs
  through, every stamp is right, none clamped, and the call is answered, 1.3 s after
  it was made. The same after the topside reboots onto another clock (before: 66
  messages stamped 5 s ahead, and 90 of the next 100 clamped);
- `replay`: someone with no key sends the topside's recorded datagrams again, from
  its address, hello first: into their own session (115 of 117 rejected, the link
  up), after the topside rebooted (the link and `pilot.depth` stay with the real
  topside, 76 messages in 1.5 s, none lost; before, the link went over to the
  recording), with the topside gone (no link; before, a link up with nobody there),
  and after the vehicle's bridge restarted (no link);
- `auto`: the vehicle with `Peer=auto`. A recorded hello sent 200 times in a second
  from another socket, the session's own and then an earlier one: nothing is sent to
  that socket and nothing of `test.depth` is lost on its way up (before: 312
  datagrams sent there, 102 of 131 messages lost). The topside's bridge restarted
  under 1 000 old hellos a second links again in 0.02 s;
- `writer`: a reliable topic at 20 Hz, the tether cut, its writer restarted twice,
  the tether mended: all 102 messages published meanwhile arrive, in order (before:
  28 lost);
- `limits`: a record of 1 MB + 4 KB on the reliable stream is passed over and the
  messages after it arrive (before: none did); under a 4 KB bound on a mirrored ring
  (the environment sets it; the real one is 32 MB) a topic of 256 slots is mirrored
  with 16.

The manifest suite (`check-hangard_manifest`) covers the manager's side: no
`BridgeConf`, a bad one, a second `[Bridge]`, the wiring derived from a conf, and the
shipped example on both roles.

## Unverified

- **The lab's tether.** Run on loopback only; not between the Pi and the laptop.
  `tools/rov/README.md` says how to try it.
- **`Peer=auto`'s discovery.** The beacons are written and parsed, but no run has
  two bridges find each other by them: broadcast on loopback does not reach a second
  socket. The `auto` run has `Peer=auto` on one side only, which learns its peer from
  the other's hello. A replayed beacon can still make a second candidate
  (`ambiguous`, no hello sent while the link is down): the candidate table is not
  aged or bounded by anything but its 8 entries.
- **A new link with reliable topics resuming** from the oldest unacknowledged
  message: the `writer` run takes the link down and back on the same keys, and the
  restarts of the other runs make new links, but no run checks a reliable topic
  across a new handshake message by message.
- **The clock on a real tether.** How long the wait for the first estimate is there
  (one round trip on loopback), and whether a modem that delays pongs behind other
  traffic ever lets the estimate lapse (10 s without a pong), are not measured.
- **A flood of datagrams with wrong tags.** The socket is read until empty on each
  wake and the watchdog is fed on the 100 ms tick: at a rate high enough to keep the
  socket readable the tick could starve. Not reproduced, not bounded.
- **The aarch64 static build** links (`make` with the release's cross toolchain);
  not run on the Pi.
