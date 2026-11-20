# encoder: the vehicle's video to the topside

The `encoder` module of docs/11 ("encoder"), stage 1: **the camera's own H.264** over
RTP, nothing encoded on the vehicle. A UVC camera that encodes H.264 itself hands over
one access unit per V4L2 buffer; the module packetizes it (RFC 6184) and sends it to
the topside's video port, 5610. The libx264 path of docs/11, for frame topics
(`cam.pilot`, `cam.raw`) and cameras without H.264, is not built yet.

Why: the Pi 5 has no hardware H.264 encoder, and on the lab's BlueROV2 (2026-11-19)
electric-eye's software encode took 0.51 of a core (electric-eye#3). Until
`video_relay` and `vision` exist, the topside is electric-eye's own `hellion` and
`eeye` in placement A, which apply the effects there and feed QGroundControl
([tools/rov/README.md](../../tools/rov/README.md)).

## Layout

| File | |
|---|---|
| `h264.h`, `h264.c` | Annex B split into NAL units and access units; the RFC 6184 packetizer |
| `encoder.c` | the module: `encoder.conf`, the sources, the socket, `video.index` |
| `encoder.module`, `encoder.conf` | the manifest and configuration templates |
| `encoder.mk` | `hangar-encoder` and `make check-encoder` |
| `tests/test_encoder.c` | the suite |

C11, libc and lib/hg only: no ffmpeg, no libx264.

## What it does

- **Sources.** `Source=uvc:/dev/videoN` sets `V4L2_PIX_FMT_H264` at `Width`×`Height`
  and `FrameRate`, streams four mmap buffers, and sends each frame as the driver
  delivers it; its timestamp is the buffer's when the driver stamps with
  `CLOCK_MONOTONIC`, else the time it was dequeued. `Source=file:<path>` sends an
  Annex B file (at most 64 MB) one access unit per frame period, once or `Loop=yes`.
- **Packets** (`h264.c`): every NAL unit of a frame in order, as a single NAL unit when
  it fits `MaxPacket` (1 200 bytes, the 12-byte RTP header included), else FU-A
  fragments; the marker bit on the frame's last packet, which `hellion`'s relay follows
  with an access-unit delimiter for its decoder. Payload type 96, a random SSRC, the
  sequence number wrapping at 65 535.
- **Parameter sets.** The SPS and PPS of each frame are kept; an IDR frame without them
  gets the kept ones in front, so a receiver that starts at any keyframe decodes (the
  SDP `hellion` writes has no `sprop-parameter-sets`).
- **Frame identity.** The RTP timestamp is the frame number times 90 000 / `FrameRate`,
  the number being the seq of the `video.index` message that names the frame (one per
  frame sent, from one), so the topside's `video_relay` references `(vehicle:video.index,
  seq)` exactly. The camera's own count (the driver's sequence plus one) is in
  `video.index`'s `frame_seq`; a frame the camera drops is a jump there, not in the
  timestamps. (Until 2026-11-20 the timestamp used the camera's count, which made the
  two differ after the first dropped frame.)
- **Keyframes.** Asked of the camera at start and whenever none has come for
  `KeyframeInterval` (1 s), with `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`; a driver
  without that control is said once in the log, and then the camera's own interval is
  what a receiver waits for.
- **`video.index`** (`msg/video_index.msg`): one message per frame sent, stamped with
  the capture time: `frame_topic_id` 0 (the camera's stream is no hangar topic), the
  frame's number and RTP timestamp, its size as sent, whether it is a keyframe and
  whether parameter sets were added. `file` stays empty: no MP4 is written in this stage.
- **Waiting.** One epoll over the camera's descriptor (or the frame timer), a 100 ms
  timer for the keyframe check and the status line, and `hg_wait_fd` only if the wiring
  has calls (none in this stage). Nothing in the wait set goes unread (hangar#1).
- **Status line:** the source and peer, frames, keyframes, packets, kbit/s, send errors.
  A send to a topside that is not listening is refused by the kernel and only counted.

## Deviations from docs/11, with reasons

1. **A keyframe on an interval, not when a receiver joins.** The stream is one-way
   UDP: the vehicle cannot see a receiver join. `KeyframeInterval` stands in; a camera
   without `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` needs the UVC H.264 extension unit
   for this, which comes later.
2. **`video.index` has more than docs/06's four fields:** the RTP timestamp, the size,
   the keyframe and parameter-set flags, and the capture time beside the stamp, so that
   a frame on the topside is found by its RTP timestamp alone.
3. **No MP4 into the session yet.** docs/11 has the encoder record its stream by stream
   copy through an `ffmpeg` child; that is the next stage's, with the recorder's
   session (`video.index`'s `file`).
4. **At most 64 NAL units a frame** are sent (the rest dropped); cameras send a few.

## Running it

```sh
hangar-encoder /etc/hangar/encoder.conf
```

On the lab's BlueROV2 its manifest and configuration are in `profiles/rov-vehicle`,
opt-in; [tools/rov/README.md](../../tools/rov/README.md) has the session, with
`hellion` and `eeye` on the laptop in placement A.

## Tests (`make check-encoder`, about 16 s)

- The packetizer: NAL units and access units of a synthetic stream (3- and 4-byte start
  codes, trailing zeros, two slices in a frame); single NAL units and FU-A at the
  boundaries (a NAL unit that fills a packet exactly is one packet, one byte more is
  two), every packet within `MaxPacket`, the marker on each frame's last; sequence
  numbers across the 65 535 wrap; the cached SPS and PPS in front of an IDR without
  them, once.
- The module from a 40-frame Annex B file to a UDP socket: every frame reassembled and
  equal byte for byte to the input (the parameter sets added before each later IDR
  accounted for), RTP timestamps of n × 900 at 100 frames/s, sequence numbers in order;
  `video.index` read back, one message a frame with its fields; 0 % of a core once the
  file has ended; a bad `Source=` or a `uvc:` node that is no camera ends it with 78.
- ffmpeg, where it is installed with libx264: a real stream (three slices a frame)
  whose parameter sets are kept only at the first IDR, ffmpeg joining after that IDR
  has gone by, from the SDP `hellion` writes: it decodes from the IDR at frame 30, 23 or
  24 of frames 30 to 59 with ffmpeg 9.0 and 12 with 6.1 (Ubuntu 24.04, as in CI; the
  rest go while ffmpeg probes its input), and 0 with the insertion taken out, on both.
  The test asks for at least 6. One that listens from the start decodes 59 of 60
  whether the parameter sets are added or not, so it proves nothing about them.

## Unverified

- **That the lab's camera** (USB `32e4:9422`, "H264 USB Camera") **offers H.264 on one
  of its `/dev/video` nodes**, and on which: `v4l2-ctl -d /dev/videoN
  --list-formats-ext` on each, at the next visit. If none does, this stage has nothing
  to send on that vehicle.
- Whether that camera takes `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`, and whether it
  streams its H.264 and its MJPEG at once (for `camera` and `cam_nn` beside it).
- Nothing of `Source=uvc:` has run against a camera: the suite has none.
