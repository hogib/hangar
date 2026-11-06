# Tether bridge and discovery

How topics, calls and files cross the tether. One module on each side, `bridge`,
finds the other, authenticates it, and carries what its configuration lists: sensor
and state topics up, pilot commands down, dataset rows up to the trainer on the
topside GPU, trained weights down ([09-learning](09-learning.md)). Video does not go
through the bridge: it stays H.264 over RTP, as in electric-eye.

The bridge generalizes electric-eye's `hellion`: its discovery, its relay and its link
statistics, with authentication added.

## The rules

- **A mirrored topic is the same topic on both sides**: same name, same type, same
  sequence numbers. `(topic, seq)` means the same message on either end of the tether,
  so provenance and dataset joins need no translation.
- **Loss is visible.** A message lost on the tether is a gap in the mirrored topic's
  sequence numbers.
- **Commands are never starved.** Traffic is sent in strict priority order under a
  total rate cap: safety and commands first, files last.
- **Nothing unauthenticated is accepted.** Every datagram, discovery beacons included,
  carries a tag computed with a key shared by the two ends.
- **Times are translated.** The bridge measures the offset between the two monotonic
  clocks and stamps mirrored messages in local time; no time daemon is needed on the
  tether.
- **A call across the tether is still a call**: the same deadlines, the same refusal
  of expired requests, and the caller identified as a module of the other side.

## Configuration

One file, `/etc/hangar/bridge.conf`, identical on both sides; `up` is from the vehicle
to the topside.

```ini
[Bridge]
Role=vehicle
Pair=default
Key=/etc/hangar/bridge.key
Port=5614
MaxRate=20Mbit

[Mirror safety.state hangar.system hangar.modules bridge.link]
Direction=up
Priority=safety

[Mirror pilot.wrench]
Direction=down
Priority=command

[Mirror nav.* mux.state recorder.status *.params]
Direction=up
Rate=10

[Mirror dataset.*]
Direction=up
Priority=dataset

[Calls]
Remote=safety.* mux.* recorder.* *.set_params policy_*.*
```

| `[Bridge]` key | Default | |
|---|---|---|
| `Role` | — (required) | `vehicle` or `topside`; the only key that differs between the two copies |
| `Pair` | `default` | Only a bridge with the same pair name is accepted. A name, not a secret |
| `Key` | `/etc/hangar/bridge.key` | 32 random bytes, made at installation and copied to the other side |
| `Port` | `5614` | Data. Discovery is on 5613 (not `hellion`'s 5612, so both can run side by side: [11-electric-eye](11-electric-eye.md#ports)) |
| `Peer` | `auto` | `auto` (discovery) or a fixed `address` |
| `MaxRate` | `20Mbit` | Total sending rate, all priorities |

| `[Mirror <topic or glob> ...]` key | Default | |
|---|---|---|
| `Direction` | — (required) | `up` or `down` |
| `Delivery` | `latest` | `latest` or `reliable` ([Delivery](#delivery)) |
| `Priority` | `telemetry` | `safety`, `command`, `call`, `telemetry`, `dataset`, `bulk` |
| `Rate` | every message | The most messages per second sent, newest first |

`[Calls] Remote=` lists the methods of the other side that this side's modules may
call through the bridge ([Calls](#calls-across-the-tether)). `[Tunnel <name>]` sections
carry datagrams of another protocol across ([Tunnels](#tunnels)).

Each side sends what its configuration says goes its way, and accepts only what its
configuration says comes to it. The two bridges compare a hash of their
configurations when they connect; a difference is reported in `bridge.link` and by
`hangar check`, and each side still accepts only what its own copy allows.

## The bridge module

The manifest of the module gives it what no other module has:

```ini
# bridge.module
[Module]
Exec=/usr/lib/hangar/bridge /etc/hangar/bridge.conf
User=hg-bridge

[Bridge]
```

A `[Bridge]` section ([02-manifest](02-manifest.md)) allows the module to publish the
topics its configuration receives, of any type (the schema comes from the other side),
provided no local module publishes them; to publish with the sender's sequence numbers
(`hg_publish_seq`); to serve the stand-in sockets of remote methods; and to name the
remote caller of a call it forwards. At most one module per machine has it.

The bridge waits on its UDP socket and its topics together, through `hg_wait_fd`
([01-topics](01-topics.md#reading)).

## Discovery

From `hellion`: every bridge announces itself once a second on UDP 5613, to the IPv4
broadcast address and IPv6 `ff02::1` of every interface that is up, and listens there:

```
HANGAR/1 role=vehicle name=rov-1 pair=default port=5614 id=1a2b3c4d tag=9f0c…
```

`tag` is the keyed hash of the rest of the line. A bridge with `Peer=auto` connects to
the one bridge of the opposite role and the same pair heard in the last 5 s, and with
two or more it chooses none and says so. A beacon only starts a connection attempt;
whether the other side holds the key is settled by the handshake, so a replayed beacon
gains nothing.

## The link

UDP, one socket each side. Every datagram:

| Offset | Type | Field | |
|---|---|---|---|
| 0 | char[4] | `magic` | `"HGB1"` |
| 4 | u8 | `kind` | hello, ping, pong, schema, message, fragment, data, ack |
| 5 | u8 | `priority` | |
| 6 | u16 | `flags` | |
| 8 | u32 | `link_id` | chosen in the handshake |
| 12 | u32 | `length` | of the body |
| 16 | u64 | `packet_seq` | per direction, rising by one |
| 24 | | body | at most 1200 bytes |
| end | u8[16] | `tag` | keyed BLAKE2s-128 of header and body, with the link key |

**Handshake.** Each side sends a hello with a fresh random nonce; the link key is the
keyed hash of both nonces under the shared key. A hello repeated from an earlier
connection yields a different link key, and a peer without the shared key cannot
produce a valid tag.

**Replays.** `packet_seq` must be new: a window of the last 64 received numbers, as in
IPsec, admits reordering and drops duplicates and replays.

**No encryption.** The tether is a cable; what needs protecting is that nobody else
can be the topside or inject commands, which the tag gives. BLAKE2s is a small, single
file in either language.

## Delivery

| Delivery | How | For |
|---|---|---|
| `latest` | sent once, never retransmitted; a message larger than one datagram is fragmented, and a lost fragment loses the message | commands, telemetry, dataset rows |
| `reliable` | acknowledged (cumulative and selective), retransmitted, delivered in order | calls, files, and topics that must arrive whole |

Mirrored messages are at most 1 MB; a `reliable` mirror whose backlog passes its
topic's slot count drops the oldest and records the gap. The schema of a mirrored
topic is sent reliably before its first message.

**Priority.** A strict order: `safety`, `command`, `call`, `telemetry`, `dataset`,
`bulk`; within one priority, topics take turns. `MaxRate` caps the total. Dataset rows
and files therefore use what is left after everything else, and can never delay a
pilot command.

## Mirrored topics

The receiving bridge creates each mirrored topic through the manager, with the
sender's schema, and publishes every message with:

- the sender's sequence number (`hg_publish_seq`): numbers rise, and a jump is a loss;
- `stamp_ns` translated into local monotonic time ([Clocks](#clocks));
- `publish_ns` the local publication time;
- the provenance unchanged: its `(topic, seq)` pairs refer to messages of the other
  side, which mean the same message wherever that topic is mirrored or recorded.

So the dataset pairer runs on the vehicle, where the inputs and their rings are, and
the rows it publishes reach the trainer through a `dataset.*` mirror.

## Clocks

Each bridge pings the other ten times a second. From each exchange (sent `t1`,
received there `t2`, answered there `t3`, received back `t4`) it computes the round
trip `(t4 − t1) − (t3 − t2)` and the offset `((t2 − t1) + (t3 − t4)) / 2`, and keeps
the offset of the exchange with the shortest round trip in the last 10 s. The
uncertainty is half that round trip. Both are published in `bridge.link` and recorded,
so an offline tool can refine the alignment of two recordings afterwards.

## Calls across the tether

A module that calls a method of the other side declares it as usual (`[Call
safety.arm]`). If `[Calls] Remote=` lists it, the manager creates the method's socket
on this side and hands it to the bridge, which serves it as a stand-in: it forwards
each request reliably, the other bridge makes the call locally, and the reply comes
back.

- **The caller is named.** The far bridge makes the call naming the original caller as
  `topside:pilot_console` (or `topside:admin:<user>`), in the request envelope's
  `on_behalf_of` field, which servers honour only from the bridge
  ([04-calls](04-calls.md#callers)). `Allow` lists name remote callers that way.
- **Deadlines are translated**, and shortened by the clock's uncertainty, so a request
  that expired on the way is dropped unexecuted.
- **No link, no call.** With the link down, the stand-in answers `HG_E_NOLINK` at once:
  nothing ran. A link lost after the request was sent gives `HG_E_LOST`: the outcome is
  unknown, and the far side's call record says what happened.

## Files

`bridge.send` (a call on the sending side) sends a file reliably, at `bulk` priority,
in chunks, resuming after a disconnection, and checks a BLAKE2s-256 hash of the whole
file before it is made visible. The receiver puts it in
`/var/lib/hangar/bridge/files/<name>` (group `hangar`, mode 0640) and publishes
`bridge.files`: `{name, size, hash, from, received_ns}`. Trained weights go down this
way ([09-learning](09-learning.md)), and recordings come up after a dive.

## Tunnels

A datagram channel for a protocol that is not hangar's, MAVLink above all
([10-mavlink](10-mavlink.md#the-topsides-mavlink-module)):

```ini
[Tunnel mavlink]
Local=127.0.0.1:14670
Priority=telemetry
MaxRate=2Mbit
```

Each datagram a local program sends to `Local` arrives at the other side's `Local`
address, authenticated like everything else, and the reverse. Nothing is added: no
retransmission and no ordering, as the protocol inside expects of UDP. Only programs
on the machine itself can reach `Local`.

## Link status

`bridge.link`, 10 Hz and on change:

| Field | |
|---|---|
| `state` | `off`, `searching`, `connecting`, `up`, `ambiguous`, `refused` |
| `peer` | address and name |
| `last_rx_ns` | when the last valid datagram arrived (the safety module's `tether` trigger) |
| `rtt_ns`, `loss_up`, `loss_down` | |
| `rate[6]` | bytes per second sent, per priority |
| `clock_offset_ns`, `clock_uncertainty_ns` | |
| `config_match` | whether the two configurations agree |
| `detail` | one line: why, or what |

## Recordings on both sides

When a recorder starts a session, its id is sent across, and each side's
`session.ini` names the other's: the two recordings of a dive point at each other, and
`bridge.link`'s recorded clock offsets align them.

## Tests

- Loss, reordering and duplication (`netem`): `latest` loses whole messages and shows
  the gaps; `reliable` delivers everything in order; duplicates and replays are dropped.
- A wrong key, a modified datagram, a replayed hello: rejected, and counted.
- Priorities: with dataset rows and a file saturating `MaxRate`, the latency of
  `pilot.wrench` stays at its unloaded value.
- Clock: offset within its stated uncertainty against a known offset, under load.
- Calls: deadlines across the link; `HG_E_NOLINK` and `HG_E_LOST`; `on_behalf_of`
  ignored from anything but the bridge.
- Files: resume after the link drops; a corrupted file never becomes visible.
- Discovery: two candidates, no choice; another pair, refused.
