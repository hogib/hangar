# electric-eye in hangar

electric-eye (`eeye` and `hellion`, release v0.3.1) is the vehicle's camera, image
processing and video link today. It joins hangar in two phases: first as it is,
supervised and readable by hangar modules from day one; then ported into hangar
modules, each proven to produce the same output as the code it replaces.

electric-eye stays a project of its own, described by its paper as of v0.3.1. Its code
is MIT-licensed and is carried into hangar (GPL-3.0) with its copyright notices.

## The source and the build

electric-eye's source lives in hangar's repository, in two places:

| Path | |
|---|---|
| `lib/<area>/` | the code hangar keeps: `vision` (the operators, the visibility correction, the effect chain), `camera` (V4L2, `rpicam`, JPEG decoding, camera controls), `proc` (child processes), `net` (discovery, relay), `video` (RTP, x264, the MP4 writer), `common` |
| `legacy/eeye/`, `legacy/hellion/` | the glue hangar replaces: the frame bus, the services, the status file, the page. Buildable as `eeye` and `hellion`, over `lib/`, until their replacements run |

Everything builds with `zig cc`, natively and as static aarch64-musl binaries with the
NEON path; `tools/cross/` holds the toolchain file and the recipe. Stripped, static with
musl and libjpeg-turbo, `eeye` is 1.09 MB and `hellion` 1.22 MB. The 17 test suites
pass natively and under `qemu-aarch64`.

OpenMP is not available in a static musl build, so the ported `vision` module runs the
operator loops on a thread pool of its own. The module may have threads: the module
*library* has none ([00-overview](00-overview.md#targets)), and that is a different
statement. The equivalence hashes guard the result whatever the thread count
([Proving the port](#proving-the-port)), and the paper's 4-thread figures depend on it.

## Phase A: supervised as it is

### Legacy modules

`eeye` and `hellion`, built from `legacy/eeye` and `legacy/hellion`
([The source and the build](#the-source-and-the-build)) and installed with the release
as module programs, run as they are, as modules of the manager instead of systemd
services of their own (their `eeye.service` and `hellion.service` are disabled, so that
no second copy fights for the camera and the ports); their own configuration files stay
where electric-eye keeps them:

```ini
# /etc/hangar/modules/eeye.module
[Module]
Description=electric-eye: camera, effects, frame bus
Exec=/usr/lib/hangar/eeye /var/lib/eeye/effects.json
User=eeye
Group=video
Legacy=yes
```

```ini
# /etc/hangar/modules/hellion.module
[Module]
Description=electric-eye: video over the tether, page, recording
Exec=/usr/lib/hangar/hellion -c /etc/hellion/hellion.json
User=eeye
Group=video
Legacy=yes
```

`Legacy=yes` ([02-manifest](02-manifest.md#module-required-once)) is for a program
that does not use hangar's library: it declares no topics or calls, is ready while its
process runs, and is restarted under its `[Restart]` rules when it exits. Its health is
its own: `hellion check` and `hellion top` keep working as before. Legacy modules may
not have an actuator output. One that drives hardware names a `[Module] Neutral=`
command, which the manager runs when it exits, as for an actuator (ArduSub,
[10-mavlink](10-mavlink.md#ardusub-as-a-module)); `eeye` and `hellion` drive no
actuator and have none.

`hellion` runs on both machines. Module names are unique across the vehicle and the
topside together ([00-overview](00-overview.md#three-planes)), so on the topside it is
the module `hellion_topside`, whose manifest is `hellion`'s with the topside's
`hellion.json`.

### Reading electric-eye's frames

hangar modules read `eeye`'s streams in place, through the library's adapter
([01-topics](01-topics.md#electric-eye-streams)): `hg_sub_open("eeye:raw", …)` maps
`/dev/shm/eeye.raw` read-only and presents it as a frame topic of the matching type,
with `frame_no` as the sequence number and `capture_mono_ns` as the stamp. Nothing is
copied: a 1280×720 frame is 1.8 MB, and copying every one into a hangar topic would cost
55 MB/s of memory bandwidth on the Pi. The reading module needs the `video` group
(`SupplementaryGroups=video`), which the frame bus files belong to.

So `cam_nn`, the pairer and a policy can use the camera before anything is ported, and
their provenance names `eeye`'s frame numbers.

### Ports

Both systems run side by side in phase A; their ports do not overlap:

| Port | Owner | |
|---|---|---|
| UDP 5600 | QGroundControl's video in | from `hellion_topside` |
| UDP 5610 | `hellion`, `hellion_topside` | video over the tether |
| UDP 5612 | `hellion`, `hellion_topside` | their discovery |
| TCP 8080, 8091 | `hellion` | page, control |
| UDP 5613 | hangar's bridge | discovery ([08-bridge](08-bridge.md#discovery)) |
| UDP 5614 | hangar's bridge | data |
| UDP 14550 (topside) | QGroundControl | its MAVLink listener, unchanged from its default; it receives from `topside_mavlink`, which it sees as the vehicle ([10-mavlink](10-mavlink.md#the-topsides-mavlink-module)). Nothing from ArduSub reaches it |
| UDP 14650 (127.0.0.1, topside) | `topside_mavlink` | its own port, from which it sends to 127.0.0.1:14550 |
| UDP 14660 (127.0.0.1, vehicle) | the vehicle's `mavlink` | ArduSub's signed MAVLink; the module is ArduSub's only peer |

No MAVLink crosses the tether: QGroundControl's joystick becomes `pilot.wrench` and
`pilot.buttons` on the topside, and its arm and disarm requests become calls
([10-mavlink](10-mavlink.md#the-topsides-mavlink-module)).

## Phase B: ported into modules

Each module below is a hangar module over `lib/`: the code it needs is already in the
repository, and the port is the module's manifest, its topics and its parameters in
place of the glue in `legacy/`. Once a module runs, the legacy binary it replaces is no
longer built.

| Module | Side | From | Publishes |
|---|---|---|---|
| `camera` | vehicle | `eeye`: `v4l2_in`, `rpicam_in`, `jpeg_decode`, `camera_ctrl` | `cam.raw` |
| `vision` | vehicle (placement B) | `eeye`: `effect_chain`, `conv`, `point_opps`, `auto_vis`, `img_stats` | `cam.pilot` |
| `cam_nn` | vehicle | new | `cam.nn` |
| `encoder` | vehicle | `hellion`: RTP sink, relay | H.264 over RTP; `video.index`; MP4 into the session |
| `video_relay` | topside | `hellion`: relay, decoder, loopback sink, recorder | to QGroundControl unchanged; `cam.decoded` |
| `pilot_console` | topside | `hellion`: page, HTTP server, preview hub | the operator's page |

Module and topic names are unique across the two machines, so no module above runs
on both, and the topside's decoded picture is a topic of its own, `cam.decoded`.

`hellion top` and `hellion check` become `hangar top` and `hangar check`
([12-tools](12-tools.md)); `hellion`'s discovery and status exchange become the bridge
([08-bridge](08-bridge.md)).

### `camera`

Captures as `eeye` does (MJPEG or YUYV over V4L2, or the Pi camera through
`rpicam-vid`; the newest buffer taken and older ones returned, so one slow frame never
leaves the picture behind) and publishes `cam.raw`: I422 full range, 4 slots, stamped
with the capture time, numbered one per captured frame.

Its parameters ([05-parameters](05-parameters.md)) are the camera controls of `eeye`
(exposure, gain, white balance, brightness, contrast, saturation, sharpness, in
device-independent units), applied live; and the device, resolution, format and frame
rate, marked `@restart`, since all frame buffers are sized at start.

### `vision`

`eeye`'s effects thread as a module: reads `cam.raw`, runs the visibility correction
and the operator chain, and publishes `cam.pilot`, whose provenance names the
`cam.raw` frame it was made from. The frame bus's neutral-chroma shortcut (copying only
luma when a chain's output chroma is all 128) is kept in the topic writer. The operator
loops run on the module's own thread pool, in place of OpenMP, with the thread count a
`@restart` parameter; the pool's threads touch no topic, and the main thread alone
reads and publishes ([The source and the build](#the-source-and-the-build)).

Its parameters are `effects.json` as a parameter type: the chain as a fixed array of 8
stages (which `eeye` already bounds it to), each stage's operator and parameters, and
the visibility correction on or off. `eeye`'s accept/refuse and roll-back of a new
configuration was the model for `set_params` ([05-parameters](05-parameters.md)), and
maps to it one to one.

### `cam_nn`

New: the frame a network sees. Reads `cam.raw`, reduces the luma plane by area
averaging to 160×120 (19 KB), and publishes `cam.nn` at the camera's rate, with
provenance to the `cam.raw` frame. Size and rate are `@restart` parameters.

No version-1 policy reads it: the runtime has no convolution ops, and a policy on
frames is the later pilot-imitation example ([09-learning](09-learning.md#later)). It
is recorded and streamed for the trainer from the first version on. It is small enough
to record in full and to carry in dataset rows across the tether, and both are
configured, not assumed:

- the recorder keeps its payload: `[Record cam.nn] Policy=full` comes before the
  `[Record cam.*] Policy=meta` rule for the large frame topics in
  `/etc/hangar/recorder.conf` ([06-recorder](06-recorder.md#what-it-records)). A
  dataset spec with a part recorded `meta` cannot be built offline, and
  `hangar dataset check` says so;
- a dataset mirror whose rows carry it uses `Delivery=reliable`
  ([08-bridge](08-bridge.md#delivery)): a row of about 19.4 KB is 17 datagrams, and
  under `latest` one lost datagram loses the row.

### `encoder`

Encodes `cam.pilot` (placement B) or `cam.raw` (placement A) to H.264 and sends it over
RTP to the topside, with the settings `hellion` uses (x264 preset `ultrafast`, tune
`zerolatency`, a keyframe every 30 frames, parameter sets before each), converting
full-range 4:2:2 to limited-range 4:2:0 itself, as `hellion` does.

**The bitrate follows the tether.** The stream does not pass through the bridge but
shares the tether with it, and the bridge's `MaxRate` is the total for both
([08-bridge](08-bridge.md#video-and-the-tether)). The encoder subscribes to
`bridge.vehicle.budget` and sets x264's bitrate from its `video_bps` between two
frames, capped by its `max_bitrate` parameter (4 000 kbit/s, `hellion`'s figure) and
never below its `min_bitrate` floor (500 kbit/s), so that the pilot keeps a picture
when the bridge is busy; both are live parameters. A stale or absent budget leaves the
bitrate where it was.

**Frame identity crosses the tether.** The encoder uses libx264 through its API, not an
`ffmpeg` child, and sets each picture's timestamp to its frame's sequence number times
the nominal frame period. The RTP timestamp therefore carries the vehicle's frame
number; QGroundControl ignores it, and the topside decoder recovers it exactly. A frame
dropped before encoding is a jump in the timestamps, as in `hellion`'s nominal-rate
stamping, which the paper found the rate control handles. On a Pi 4, the hardware
encoder (through V4L2, which carries each buffer's timestamp) can replace libx264; the
Pi 5 has no hardware H.264 encoder.

It packetizes RTP itself (RFC 6184: single NAL units and FU-A fragments of at most
1 200 bytes, the marker bit on each frame's last packet), publishes `video.index`
(`{frame topic id, frame seq, file, pts}` per encoded frame) and records the stream into the
recorder's session as fragmented MP4 by stream copy, through an `ffmpeg` child as
`hellion` does, so a file cut by a power loss plays to its last keyframe. Session
directories belong to the group `hangar-rec` (mode 2770,
[06-recorder](06-recorder.md#sessions)), so the encoder has
`SupplementaryGroups=hangar-rec`.

### `video_relay`

`hellion`'s topside half: owns the tether's video port, copies every packet unchanged to
QGroundControl (UDP 5600) and to its own decoder, with the end-of-frame delimiter that
took the paper's transport latency from 74.8 to 8.3 ms at 720p. It decodes with
libavcodec (slice threads, no reordering wait), which gives back each frame's
timestamp, and publishes `cam.decoded`, one message per decoded frame, with:

- its own sequence numbers, counting up by one, as for any topic of an ordinary module
  (only the bridge publishes with the other machine's numbers,
  [01-topics](01-topics.md#provenance));
- the capture stamp translated to the topside's clock ([08-bridge](08-bridge.md#clocks));
- one provenance reference, **the vehicle's frame**: `(cam.pilot, seq)`, the seq
  recovered from the timestamp.

The manifest declares that reference as a topic of the other side,
`Inputs=vehicle:cam.pilot`: the module fills it with `hg_publish_with`, and the library
does not check that topic's staleness on this side, where it does not exist. A join
across the tether follows the reference and names the frame exactly. A frame lost on
the tether has no `cam.decoded` message; `cam.decoded` itself has no gap.

It also feeds the v4l2loopback webcam device and the topside's MP4 recording, which
goes into the session of the topside's recorder (`topside_recorder`, following
`topside_recorder.session`); it has `SupplementaryGroups=hangar-rec` for that.

### `pilot_console`

`hellion`'s page, grown into the operator's console. Its calls are messages on its
request topic, `pilot_console.req`, which the manager creates for it from its `[Call]`
sections ([04-calls](04-calls.md#the-two-topics)). It runs on the topside, so the
vehicle's modules name it `topside:pilot_console` in their `Allow` lists
([04-calls](04-calls.md#allow-tokens)): those of the safety, mux and policy modules, and
the `[Params] Allow` of `camera` and `vision`. The bridge mirrors `pilot_console.req`
down and the reply topics of the modules it calls up, as `[Calls] Remote=` in
`bridge.conf` arranges ([08-bridge](08-bridge.md#calls-across-the-tether)); on the
vehicle the request arrives on a mirrored topic, so the caller is
`topside:pilot_console` by the topic it came on, asserted by nobody. It offers:

- the live picture from any frame topic, as `hellion`'s shared preview encoder serves
  it;
- the effects and camera parameters, through `set_params`: across the tether to a
  module on the vehicle, or locally to `vision` in placement A;
- arm, disarm, e-stop, clear ([07-safety](07-safety.md#vehicle-states)). `safety.arm`
  answers `accepted` once its checklist passes; the console shows the outcome from
  `safety.state`: `arming`, then `armed`, or `disarmed` with ArduSub's reason;
- the mux's mode (`manual`, `assisted`, `residual:<name>`), and the promotion of a
  policy from its load progress (`policy.<name>.load`) and the running model
  (`policy.<name>.active`). `mux.set_mode` carries the hash of the model the operator
  promotes, and the mux refuses it if another model is running
  ([07-safety](07-safety.md#the-mux)); `revert` is a call to the policy. A live
  comparison of a model against the pilot (`policy.<name>.eval`) comes later
  ([09-learning](09-learning.md#later));
- the recorder's start, stop and black-box dump (`recorder.start`, `recorder.stop`,
  `recorder.dump` on the vehicle);
- the vehicle's and the link's state (`hangar.vehicle.system`,
  `hangar.vehicle.modules`, `safety.state`, `bridge.vehicle.link`,
  `bridge.topside.link`).

The vehicle's topics it shows reach it through the bridge's up mirrors, the policy
topics (`policy.*.active`, `policy.*.load`) and the reply topics of the modules it
calls included ([08-bridge](08-bridge.md#configuration)).

### Placements

Placement becomes a question of which modules are enabled where:

| | vehicle | topside |
|---|---|---|
| **B** (as electric-eye recommends) | `camera`, `vision`, `cam_nn`, `encoder` | `video_relay` (`cam.decoded` from `cam.pilot`), `pilot_console` |
| **A** | `camera`, `cam_nn`, `encoder` (of `cam.raw`) | `video_relay` (`cam.decoded` from `cam.raw`, `Inputs=vehicle:cam.raw`, nothing to QGroundControl), `vision` (reading `cam.decoded`), `topside_encoder` to QGroundControl, `pilot_console` |

`vision` runs on one machine or the other, never both; the topside's encoder in
placement A is a module of another name, `topside_encoder`.

## Proving the port

Every ported operator is checked against electric-eye v0.3.1 with the paper's method,
as the `make` target `equivalence` (`tools/equivalence/`): the output of every operator
is hashed over a fixed set of 20 615 cases, synthetic frames at seven sizes (1280×720,
640×480 and the awkward 65×48, 79×33, 31×9, 2×2, 1×1) and five content patterns
(seeded random, flat, all-max, checkerboard, ramp), every strength and threshold in
steps, the point operators through one-stage chains, nine multi-stage chains, and the
visibility correction over 60-frame sequences. The hashes were recorded once from
v0.3.1 and are checked in (`hashes-v0.3.1.txt`); the target computes them from
`lib/vision` at 1, 3 and 4 threads where OpenMP is compiled in, and compares. It runs
natively on x86-64 and as the static AArch64 build under `qemu-aarch64`, which executes
the NEON paths; every comparison must be identical. The paper's camera photographs are
not part of the set, so that the target is self-contained and the same everywhere;
the synthetic sizes cover the vectorization tails the photographs did not. ARMv7 is
checked when a full-system emulator or a board is available, not in the default target.

The video path is held to the paper's measurement: frame topic to frame topic through
`encoder` and `video_relay` over loopback, median latency no worse than electric-eye's
(3.8 ms at 640×480, 8.3 ms at 1280×720, 30 frames/s). That figure is one of the
targets `hangar bench` reports against ([00-overview](00-overview.md#targets)).

## Tests

- Phase A: `eeye`, `hellion` and `hellion_topside` supervised and restarted; a hangar
  module reading `eeye:raw` through the adapter with `frame_no` as sequence numbers;
  both discoveries side by side; QGroundControl, left at its defaults, receiving
  MAVLink from `topside_mavlink` on 14550 and nothing from ArduSub, and video on 5600.
- Build: `eeye`, `hellion` and every phase B module built natively and as static
  aarch64-musl binaries; the 17 suites and the `equivalence` target passing natively and
  under `qemu-aarch64`; the `vision` module's hashes equal at 1 and at 4 threads.
- Phase B: the equivalence hashes above; the latency target; every `cam.decoded`
  message referencing the vehicle frame it shows, through encoding, loss and
  keyframes, and `cam.decoded` counting up by one; `video.index` matching the MP4's
  timestamps; parameters applied live and `@restart` ones by restart; the encoder's
  bitrate following `bridge.vehicle.budget` within a second, between its floor and its
  cap, and unchanged while the budget is stale.
- `cam.nn` recorded in full, and a dataset with a `cam.nn` part built offline from a
  session equal to the live rows except where a row's `late` flag is set or one row
  alone marks a part `missing`
  ([06-recorder](06-recorder.md#offline-the-same-rows-from-a-recording)); a spec whose
  part was recorded `meta` reported by `hangar dataset check`.
- `pilot_console`: its requests, arriving on the vehicle on the mirrored
  `pilot_console.req`, allowed as `topside:pilot_console`; the same request on any
  other `.req` topic denied where only that token is listed; its replies read from the
  mirrored `.rep` topics; `set_params` reaching `camera` across the tether; arming
  reported through `safety.state`; a promotion naming a model other than the running
  one refused.
- Names: the encoder and `video_relay` writing into their sessions as members of
  `hangar-rec`; a module named `encoder` enabled on both machines reported by the
  bridges and by `hangar check`.
