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
- **A camera that goes away.** Unplugged, a V4L2 node answers every request with
  `ENODEV` and reports an error on its descriptor at every poll. The module ends then,
  with exit status 75, and the manager starts it again under the manifest's
  `[Restart]` (`Backoff=1s..10s`, a burst limit the retries never reach): each start
  opens the camera afresh, and one that is still missing ends with 75 again, so a
  camera plugged back in is streaming within 10 s. (It used to go round its loop on
  the error at 99 % of a core, sending nothing, until restarted by hand.) A node that
  exists and is no H.264 camera is still 78, a configuration the manager gives up on.
  A camera that stops delivering without any error is the watchdog's: the module
  calls `hg_alive()` only while frames are being sent, and the manifests set
  `[Health] Watchdog=5s`, since `video.index` is an event topic with no rate for the
  manager to judge.
- **Camera controls** (`msg/encoder_params.msg`): exposure, gain, white balance,
  brightness, contrast, saturation and sharpness are the module's parameters, because
  the module holds the camera and nothing else on the vehicle can set them. The units
  are `lib/camera/camera_ctrl.h`'s, the same on any camera (percent with 100 neutral,
  microseconds, kelvin); `camera_ctrl_apply` puts each into the device's own range,
  the automatic modes first, and a control the camera does not have is skipped and said
  once in the log. `-1` leaves a control as the camera has it, which is every field's
  default but `keep_framerate`. They apply live, between two frames, and again at every
  start, since a camera that was unplugged comes back with its own defaults. A manual
  exposure or white balance is refused unless the same set switches its automatic mode
  off, because the camera would refuse it. With `Source=file:` they are accepted and
  the log says that nothing takes them.

  ```sh
  hangar param set encoder auto_exposure=0 exposure_us=8000 gain=30
  hangar param set encoder brightness=120          # 100 is neutral
  hangar param reset encoder                       # back to the camera's own
  ```

  From the topside the same commands work across the bridge (the rov profiles carry
  `encoder.params` and `encoder.set_params`; docs/04 "Declaring calls").
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
  file has ended; a bad `Source=` or a `uvc:` node that is no camera ends it with 78,
  a `uvc:` node that does not exist with 75.
- The camera unplugged (`test_camera_unplugged`): `tests/fake_uvc.c`, an `LD_PRELOAD`
  library that is a V4L2 H.264 node inside the encoder's process, streams for 1.5 s and
  is then "unplugged" (`ENODEV` from every request, a hang-up at every poll): the
  encoder ends by itself with 75 and says the camera is gone. Skipped, and said so,
  where the toolchain makes no shared objects or the encoder is static.
- The camera's controls (`tests/test_encoder_controls.c`, the module under a manager on
  the same fake camera, which has a webcam's controls and writes every set to the log):
  with the defaults one control is touched (`keep_framerate`); `brightness=150 gain=50
  contrast=100` arrive as 32, 50 and 32 in the camera's ranges; `auto_exposure=0
  exposure_us=8000` as the mode first and then 80 (100 µs units); a white balance
  without its automatic mode off is refused with the reason and nothing reaches the
  camera; a control the camera lacks (sharpness) is said once and the rest of the set
  still applied; after a restart the saved values are put to the camera again. Skipped
  where the fake camera cannot be loaded.
- ffmpeg, where it is installed with libx264: a real stream (three slices a frame)
  whose parameter sets are kept only at the first IDR, ffmpeg joining after that IDR
  has gone by, from the SDP `hellion` writes: it decodes from the IDR at frame 30, 23 or
  24 of frames 30 to 59 with ffmpeg 9.0 and 12 with 6.1 (Ubuntu 24.04, as in CI; the
  rest go while ffmpeg probes its input), and 0 with the insertion taken out, on both.
  The test asks for at least 6. One that listens from the start decodes 59 of 60
  whether the parameter sets are added or not, so it proves nothing about them.

## Unverified

- **The camera controls on a camera.** They have run on the suite's fake camera and,
  with `Source=file:`, across the bridge in a rehearsal; `camera_ctrl_apply` itself is
  eeye's, with its own suite (`lib/camera/tests`). Not yet tried on a real camera
  through this module, and in particular: whether the lab's camera takes the controls on its H.264 node
  (`/dev/video2`), the one this module has open, or only on `/dev/video0`. `v4l2-ctl -d /dev/video2 --list-ctrls` on the Pi answers it.

- **A real unplug.** What the kernel does to a UVC node whose device is pulled
  (`ENODEV` from the ioctls, `EPOLLERR | EPOLLHUP` from poll) is emulated by the
  suite's fake camera from a reading of `v4l2-dev.c`, not observed on a camera. The
  watchdog path (a camera that hangs without an error) was run once by hand under a
  manager with the fake's `FAKE_UVC_STALL_S`, not in the suite.

- **That the lab's camera** (USB `32e4:9422`, "H264 USB Camera") **offers H.264 on one
  of its `/dev/video` nodes**, and on which: `v4l2-ctl -d /dev/videoN
  --list-formats-ext` on each, at the next visit. If none does, this stage has nothing
  to send on that vehicle.
- Whether that camera takes `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`, and whether it
  streams its H.264 and its MJPEG at once (for `camera` and `cam_nn` beside it).
- Nothing of `Source=uvc:` has run against a camera: the suite has none.
