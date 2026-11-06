# electric-eye in hangar

electric-eye (`eeye` and `hellion`, release v0.3.1) is the vehicle's camera, image
processing and video link today. It joins hangar in two phases: first as it is,
supervised and readable by hangar modules from day one; then ported into hangar
modules, each proven to produce the same output as the code it replaces.

electric-eye stays a project of its own, described by its paper as of v0.3.1. Its code
is MIT-licensed and is carried into hangar (GPL-3.0) with its copyright notices.

## Phase A: supervised as it is

### Legacy modules

`eeye` and `hellion` run unchanged, as modules of the manager instead of systemd
services of their own (their `eeye.service` and `hellion.service` are disabled, so that
no second copy fights for the camera and the ports):

```ini
# /etc/hangar/modules/eeye.module
[Module]
Description=electric-eye: camera, effects, frame bus
Exec=/usr/local/bin/eeye /var/lib/eeye/effects.json
User=eeye
Group=video
Legacy=yes
Environment=OMP_WAIT_POLICY=passive
```

```ini
# /etc/hangar/modules/hellion.module
[Module]
Description=electric-eye: video over the tether, page, recording
Exec=/usr/local/bin/hellion -c /etc/hellion/hellion.json
User=eeye
Group=video
Legacy=yes
```

`Legacy=yes` ([02-manifest](02-manifest.md#module-required-once)) is for a program
that does not use hangar's library: it declares no topics or calls, is ready while its
process runs, and is restarted under its `[Restart]` rules when it exits. Its health is
its own: `hellion check` and `hellion top` keep working as before. Legacy modules may
not have an actuator output.

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
| UDP 5600 | QGroundControl's video in | from `hellion` |
| UDP 5610 | `hellion` | video over the tether |
| UDP 5612 | `hellion` | its discovery |
| TCP 8080, 8091 | `hellion` | page, control |
| UDP 5613 | hangar's bridge | discovery ([08-bridge](08-bridge.md#discovery)) |
| UDP 5614 | hangar's bridge | data |
| UDP 14550 (localhost, topside) | hangar's `mavlink` | QGroundControl's MAVLink ([10-mavlink](10-mavlink.md)) |

## Phase B: ported into modules

| Module | Side | From | Publishes |
|---|---|---|---|
| `camera` | vehicle | `eeye`: `v4l2_in`, `rpicam_in`, `jpeg_decode`, `camera_ctrl` | `cam.raw` |
| `vision` | vehicle (placement B) | `eeye`: `effect_chain`, `conv`, `point_opps`, `auto_vis`, `img_stats` | `cam.pilot` |
| `cam_nn` | vehicle | new | `cam.nn` |
| `encoder` | vehicle | `hellion`: RTP sink, relay | H.264 over RTP; `video.index`; MP4 into the session |
| `video_relay` | topside | `hellion`: relay, decoder, loopback sink, recorder | to QGroundControl unchanged; `cam.pilot` (topside) |
| `pilot_console` | topside | `hellion`: page, HTTP server, preview hub | the operator's page |

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
luma when a chain's output chroma is all 128) is kept in the topic writer.

Its parameters are `effects.json` as a parameter type: the chain as a fixed array of 8
stages (which `eeye` already bounds it to), each stage's operator and parameters, and
the visibility correction on or off. `eeye`'s accept/refuse and roll-back of a new
configuration was the model for `set_params` ([05-parameters](05-parameters.md)), and
maps to it one to one.

### `cam_nn`

New: the frame a network sees. Reads `cam.raw`, reduces the luma plane by area
averaging to 160×120 (19 KB), and publishes `cam.nn` at the camera's rate, with
provenance to the `cam.raw` frame. Small enough to record in full and to carry in
dataset rows across the tether ([06-recorder](06-recorder.md#what-it-records)). Size and
rate are `@restart` parameters.

### `encoder`

Encodes `cam.pilot` (placement B) or `cam.raw` (placement A) to H.264 and sends it over
RTP to the topside, with the settings `hellion` uses (x264 preset `ultrafast`, tune
`zerolatency`, 4 000 kbit/s capped, a keyframe every 30 frames, parameter sets before
each), converting full-range 4:2:2 to limited-range 4:2:0 itself, as `hellion` does.

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
(`{frame topic id, frame seq, pts}` per encoded frame) and records the stream into the
recorder's session as fragmented MP4 by stream copy, through an `ffmpeg` child as
`hellion` does, so a file cut by a power loss plays to its last keyframe.

### `video_relay`

`hellion`'s topside half: owns the tether's video port, copies every packet unchanged to
QGroundControl (UDP 5600) and to its own decoder, with the end-of-frame delimiter that
took the paper's transport latency from 74.8 to 8.3 ms at 720p. It decodes with
libavcodec (slice threads, no reordering wait), which gives back each frame's
timestamp, and publishes `cam.pilot` on the topside with the **vehicle's sequence
numbers** (from the timestamp) and the capture stamp translated to the topside's clock
([08-bridge](08-bridge.md#clocks)). Joins across the tether then name frames exactly.
It also feeds the v4l2loopback webcam device and the topside's MP4 recording.

### `pilot_console`

`hellion`'s page, grown into the operator's console, and the `pilot_console` named in
the `Allow` lists of the safety, mux and policy modules:

- the live picture from any frame topic, as `hellion`'s shared preview encoder serves
  it;
- the effects and camera parameters, through `set_params` across the tether;
- arm, disarm, e-stop, clear ([07-safety](07-safety.md#vehicle-states));
- the mux's mode, and the promotion of a policy, with its live evaluation
  ([09-learning](09-learning.md#shadow-evaluation));
- the recorder's start, stop and black-box dump;
- the vehicle's and the link's state (`hangar.modules`, `safety.state`, `bridge.link`).

### Placements

Placement becomes a question of which modules are enabled where:

| | vehicle | topside |
|---|---|---|
| **B** (as electric-eye recommends) | `camera`, `vision`, `cam_nn`, `encoder` | `video_relay`, `pilot_console` |
| **A** | `camera`, `cam_nn`, `encoder` (of `cam.raw`) | `video_relay` (publishing `cam.raw`), `vision`, a second `encoder` to QGroundControl, `pilot_console` |

## Proving the port

Every ported operator is checked against electric-eye v0.3.1 with the paper's method:
the output of both hashed over fixed inputs (the three camera photographs and the
synthetic frames of edge-case sizes), every strength and threshold in steps, 1 to 7
threads, on x86-64 and, under full-system emulation, on ARMv7 and AArch64. Every
comparison must be identical. The visibility correction is checked the same way over
its synthetic sequences.

The video path is held to the paper's measurement: frame topic to frame topic through
`encoder` and `video_relay` over loopback, median latency no worse than electric-eye's
(3.8 ms at 640×480, 8.3 ms at 1280×720, 30 frames/s). That figure becomes a budget
([00-overview](00-overview.md#budgets)).

## Tests

- Phase A: `eeye` and `hellion` supervised and restarted; a hangar module reading
  `eeye:raw` through the adapter with `frame_no` as sequence numbers; both discoveries
  side by side.
- Phase B: the equivalence hashes above; the latency budget; frame sequence numbers
  identical on both sides of the tether through encoding, loss and keyframes;
  `video.index` matching the MP4's timestamps; parameters applied live and `@restart`
  ones by restart.
