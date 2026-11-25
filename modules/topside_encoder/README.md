# topside_encoder: the pilot's picture to QGroundControl

Stage B4 of docs/11's phase B ("hangar's own"): the topside's encoder of placement A
(docs/11-electric-eye.md, "encoder" and "Placements"). It reads a frame topic (`cam.pilot`,
the effects of `vision`; `cam.decoded` from `video_relay` while `vision` does not run),
encodes it to H.264 **in process with libx264**, and sends it over RTP to
QGroundControl's video in (UDP 5600). No ffmpeg anywhere, as the user decided on
2026-11-20. Binary `hangar-topside-encoder`; GPL-3.0-or-later, C11 over `lib/hg` and
libx264 (GPL-2.0-or-later).

```
hangar-topside-encoder /etc/hangar/topside_encoder.conf
```

## What it does

- **Reads** the frame topic (`Source=`, an I422 full-range frame type, the header of
  `hg_frame_type`) on every wake. Of the frames waiting it encodes the newest: an older
  one behind it is skipped and counted, a jump in the RTP timestamps, as docs/11 has it
  for a frame dropped before encoding.
- **Does not copy the frame out of the ring.** A 1280×720 I422 frame is 1.8 MB and the
  only thing done with it is the conversion below, which writes x264's own picture. So
  the conversion reads the ring's slot in place (`hg_latest_view`, docs/01 "In place")
  and `hg_view_done` says afterwards whether the slot held that frame throughout; only
  then is the picture given to x264. A frame the writer overwrote meanwhile is dropped
  before the encoder has seen any of it (counted as "overwritten while read" in the
  stopping line), and after three in a row the newest is copied, as before, so a writer
  faster than the conversion cannot starve the encoder. Until that check the frame's
  header is not believed: it must describe the picture the encoder is already open
  for, and its strides and offsets must stay inside the payload. The first frame, a new
  size and a header that fails either test go by the copying path, which checks the same
  bounds on a whole message.
- **Converts** full-range 4:2:2 to limited-range 4:2:0 itself, with `hellion`'s fixed
  point (`legacy/hellion/sinks.c`, `to_i420`): Y' = 16 + Y·219/255, C' = 128 +
  (C−128)·224/255, each pair of chroma rows averaged.
- **Encodes** with hellion's settings: preset `ultrafast`, tune `zerolatency` (no
  B-frames, no lookahead: each frame comes out of the call it went in), a keyframe every
  `KeyInterval` frames and none other (scenecut off), SPS and PPS before each keyframe,
  limited range in the VUI. The rate control is an average bitrate held by a VBV of half
  a second, as hellion's `-maxrate`/`-bufsize`.
- **Sends** each frame as RTP H.264 with the vehicle's encoder's RFC 6184 packetizer
  (`modules/encoder/h264.c`, linked as it is: the encoder is unchanged): single NAL
  units and FU-A fragments within `MaxPacket`, the marker bit on the frame's last
  packet, payload type 96, a random SSRC. The RTP timestamp is the input frame's seq
  times 90000/`FrameRate`, so the frame's identity rides with the picture.
- **Indexes** each frame sent on `topside_video.index` (`msg/video_index.msg`: the frame
  topic's id, the frame's seq, the RTP timestamp, the bytes sent, keyframe, parameter
  sets added), referencing the frame it was made from (`Inputs=cam.pilot`).
- **The bitrate is a parameter**, applied live between two frames through
  `x264_encoder_reconfig`: `bitrate_kbps` (4000, hellion's figure) and its cap
  `max_bitrate_kbps` (8000); a target over the cap is refused with the reason
  (`msg/topside_encoder_params.msg`):

  ```
  hangar param set topside_encoder bitrate_kbps=2000
  ```

- **Waits** in one `hg_wait` over the frame topic and its call topics, each read on every
  wake (the lesson of hangar#1); no thread of its own beyond x264's sliced threads.

## `topside_encoder.conf`

| Key | Default | |
|---|---|---|
| `Source` | `cam.pilot` | the frame topic encoded |
| `Peer` | `127.0.0.1:5600` | QGroundControl's video in |
| `FrameRate` | 30 | the nominal rate: RTP timestamps and the rate control |
| `KeyInterval` | 30 | frames between keyframes |
| `MaxPacket` | 1200 | RTP header included |
| `PayloadType` | 96 | |
| `Threads` | 0 | x264's sliced threads; 0 lets it choose |
| `Index` | `topside_video.index` | the index topic |

## The library

Development and the tests use the system's libx264 (`pkg-config x264`; `X264_CFLAGS`,
`X264_LIBS`). The release and CI's cross builds link a static one built by
`tools/cross/x264.sh` with the target's `zig cc`: x264's library alone (no CLI, no
lavf/swscale/ffms/gpac/lsmash), 8-bit, 4:2:0, fetched by its git commit (the stable
branch, `b35605a…`, 2025-06-08), cached under the release's `--work`. On aarch64 it
has its NEON assembly; on x86-64 it has its assembly only where `nasm` is installed
(not on the development laptop: C alone there, and the script says so); on armv7 it is
C alone. Without libx264 the Makefile skips the module and its suite, and says so.

## Tests (`make check-topside_encoder`, about 10 s)

Under a manager, the test process playing `vision`: it publishes 1280×720 I422 frames
(a moving ramp with noise, so the rate control has work), receives the module's RTP on
a local socket, puts it back into frames with `video_relay`'s depacketizer and decodes
them with libavcodec. On the development laptop (x86-64, the system's libx264 with its
assembly):

- 60 frames published, 60 access units back, **60 decoded** at 1280×720, in order, each
  with the RTP timestamp its seq gives;
- **a keyframe at frames 1 and 31 and nowhere else**, SPS and PPS before each;
- 60 `topside_video.index` messages, each naming its frame, seq, timestamp and kind,
  with the `cam.pilot` frame as its provenance;
- **publish → the frame's last packet, 1280×720: median 6.4 ms, p90 6.9 ms, max about
  15 ms** (conversion, encode, packetizing, loopback). Reading in place took the median
  from 6.57 ms (6.50 to 6.61 over six runs) to 6.40 ms (6.28 to 6.49): the copy of one
  frame, about 0.17 ms of a path that x264 dominates;
- **a writer that laps the reader**: about 4800 flat frames of two grey levels in turn in
  1.2 s into the four slots; every picture that comes back shows one level (a span of
  luma of 0 or 1, where two frames in one picture would show about 110), 140 to 200
  frames were overwritten while being converted and dropped, and pictures kept coming;
- **a header that lies**: a last plane starting 16 bytes before the payload's end, and a
  stride of 2³¹: neither is converted, the module stays up, the next frame is encoded;
- **the bitrate**: 4036 kbit/s at the default 4000; after `hangar param set
  topside_encoder bitrate_kbps=1000`, 955 kbit/s; 9000 over the cap of 8000 refused;
- 0.0 % of a core with no frames; a clean stop (`encode 3.2 ms a frame on average`).

## Deviations from docs/11, and why

1. **The index topic is `topside_video.index`, not `video.index`**: names are unique
   across the vehicle and the topside, and the vehicle's encoder has `video.index`.
2. **No bitrate from `bridge.vehicle.budget`**: that budget is the tether's, which this
   stream does not cross (it goes to QGroundControl on the same machine). The bitrate
   is the parameter alone.
3. **No end-of-frame access unit delimiter.** QGroundControl's GStreamer receiver cuts
   frames at the RTP marker bit; the delimiter `lib/video`'s relay sends existed for an
   ffmpeg child's parser.
4. **No MP4 recording here**: the topside records the vehicle's stream in `video_relay`.
5. **The frame size follows the input**: a frame of another size reopens the encoder
   (a keyframe follows).
6. **A frame that is not I422 of even size is not encoded**, said once in the log.

## Unverified

- QGroundControl showing the stream: tested by decoding with libavcodec, not by QGC.
- On the Pi or the lab's laptop: measured on the development laptop only.
- The static x86-64 build has no assembly here (no `nasm`); its speed with assembly is
  the system library's above.
