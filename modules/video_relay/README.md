# video_relay: the topside's end of the vehicle's video

Phase B stage B2 of [docs/11](../../docs/11-electric-eye.md) ("video_relay", and the
stage table under "hangar's own"). The vehicle's `encoder` sends H.264 over RTP to the
topside's video port. This module owns that port and does three things with every
packet:

1. **Relays it unchanged** to the configured targets: QGroundControl's video in
   (`127.0.0.1:5600`), or anything else that wants the stream.
2. **Puts the frames back together and decodes them in process** with libavcodec
   (slice threads, no reordering wait), publishing `cam.decoded`: one message per
   decoded frame, I422 full range as `lib/vision` expects, referencing the vehicle's
   frame.
3. **Records the stream as fragmented MP4**, stream copy, into the topside recorder's
   session (`video/` in it) or into a directory of its own.

There is no ffmpeg child and no thread of its own. One `epoll` covers the socket, a
1-second timer for the status line, and the call and session subscriptions, each of
which is read whenever the loop wakes (hangar#1).

```
hangar-video-relay /etc/hangar/video_relay.conf
```

| File | |
|---|---|
| `video_relay.c` | the module: the socket, the relay, the decoder, `cam.decoded`, the recording |
| `depack.c/.h` | RTP H.264 (RFC 6184: single NAL units, STAP-A, FU-A) back into access units |
| `mp4.c/.h` | the fragmented MP4 writer, and the SPS's picture size |
| `video_relay.module`, `video_relay.conf` | the manifest and the configuration templates |
| `tests/test_video_relay.c`, `tests/testsrc-320x240-60.h264` | the suite and its H.264 |

## Frame identity

The encoder's RTP timestamp is the frame's number times 90 000 / `FrameRate`. The
number is the seq of the encoder's `video.index` message for that frame
(`modules/encoder/README.md`, "Frame identity"). The module recovers it (the timestamp
unwrapped to 64 bits, times `FrameRate` / 90 000, rounded) and publishes each decoded
frame with one provenance reference, `(vehicle:video.index, number)`: `Frames=` in the
configuration and `Inputs=` in the manifest. A join from a decoded frame back to the
vehicle therefore finds the frame's `video.index` message, and through it the capture
time, the size and whether it was a keyframe.

## Loss and restarts

A gap in the RTP sequence numbers marks the frame under way as broken. That frame is
not decoded, and neither is any later one until a whole keyframe arrives: a decoder fed
a frame whose reference is missing shows garbage until then. So the frames are skipped
and counted, and the vehicle's numbers for them have no `cam.decoded` message.

A **new stream**, meaning another SSRC or sequence numbers far behind the last (more
than 64 packets: the encoder restarted), starts over at its first keyframe. A packet a
little behind the newest is late and dropped.

## Recording

`Session=` names a recorder session topic (`topside_recorder.session` by default, also
declared in the manifest). A session starting means a recording into
`<session>/video/<Name>-<id>.mp4`; a session stopping ends it. `RecordDir=` records
from the module's start into `<RecordDir>/<Name>-<time>.mp4` instead, for the bench and
the tests. Files are mode 0640; a missing `video/` is made 2770 (docs/06, "Sessions").

The MP4 is fragmented, one fragment per frame:
- the header (`ftyp`, then `moov` with an empty sample table and `mvex`) is written at
  the first keyframe, from its SPS and PPS;
- each frame is then a `moof` + `mdat`, its NAL units with 4-byte lengths;
- timing is the RTP clock (90 kHz).

A file cut by a crash or a power loss plays up to its last whole fragment (the suite
cuts one at two thirds).

## `video_relay.conf`

| Key | Default | |
|---|---|---|
| `Bind` | `5610` | the port, or `host:port` |
| `Relay` | none | blank-separated `host:port` targets, every packet unchanged |
| `FrameRate` | `30` | the encoder's: the timestamp's period |
| `PayloadType` | `96` | the encoder's; other packets are counted and dropped |
| `Frames` | `vehicle:video.index` | the vehicle topic the frame numbers are seqs of |
| `Decode` | `yes` | `cam.decoded` on or off |
| `Threads` | `2` | the decoder's slice threads (1–16) |
| `Session` | `topside_recorder.session` | the recorder session topic followed; empty for none |
| `RecordDir` | none | record from the start into this directory |
| `Name` | `video_relay` | the MP4 files' prefix |

The status line says frames per second, kbit/s, frames, decoded, packets lost, frames
skipped, and whether it is recording.

## Building

libavcodec and libavutil come from `pkg-config` for development and the tests (the
system's: `libavcodec-dev` on Debian and Ubuntu, `ffmpeg` on Arch). `AVCODEC_CFLAGS`
and `AVCODEC_LIBS` override them, as with libturbojpeg. Without them the module and its
suite are skipped, and say so.

The release links a static build of FFmpeg's H.264 decoder and parser alone
(`tools/cross/avcodec.sh`, pinned FFmpeg 7.1.1, LGPL 2.1 or later; `LICENSING.md` has
its configure flags). It's built for each target with zig cc and cached under the
release's `--work`, like libjpeg-turbo.

## Tests (`make check-video_relay`, about 6 s)

All on `tests/testsrc-320x240-60.h264`: 60 frames of ffmpeg's test pattern, a keyframe
every 30, parameter sets repeated. It was made with:

```
ffmpeg -f lavfi -i testsrc=size=320x240:rate=30 -frames:v 60 -c:v libx264 -preset ultrafast \
  -tune zerolatency -g 30 -bf 0 -crf 30 -pix_fmt yuv420p -x264-params repeat-headers=1 -f h264 out.h264
```

The RTP is made by the encoder's own packetizer (`modules/encoder/h264.c`).

- **The depacketizer:** a round trip at 1200- and 300-byte packets (FU-A everywhere),
  every NAL unit equal and every timestamp right; a lost packet of frame 10 skipping
  10–29 and nothing else; bad packets counted.
- **The SPS:** 320×240.
- **The MP4:** 60 fragments, `ftyp` first; ffprobe reads 60 frames, and 30 from the
  file cut at two thirds (skipped, and said, without ffprobe).
- **The module over loopback, in lockstep:**
  - 60 frames, each decoded, its reference `(video.index, n)`, every packet relayed
    byte for byte, the picture I422 320×240 full range;
  - then a new stream (a restarted packetizer) with a packet of frame 70 lost:
    61–69 decoded, 70–90 not, 91–120 decoded;
  - 99 fragments in the MP4, file mode 0640;
  - at rest 0.0% of a core;
  - a bad configuration refused with exit status 78.
- **Latency on this laptop** (x86-64, 320×240), from the frame's last packet sent to
  `cam.decoded` published (depacketizing, decoding, the I422 conversion, the publish):
  **0.10 ms median, about 1.2 ms worst**.

## Deviations from docs/11, and why

1. **`lib/video`'s `udp_relay` is not used.** Its forwarding thread and the access unit
   delimiter it inserts exist for an ffmpeg *child*, whose parser only knows a frame
   has ended when the next one arrives. In process, frames are cut at the marker bit
   and handed to libavcodec whole, so neither is needed, and the module stays
   single-threaded.
2. **`lib/video`'s `mp4_record` is not used either:** it is built around an ffmpeg
   child (the user's decision, 2026-11-20: no ffmpeg child anywhere). The fragmented
   MP4 is written in process (`mp4.c`), one fragment per frame.
3. **The reference is `vehicle:video.index`**, not docs/11's `vehicle:cam.pilot`. With
   the camera's own H.264 (encoder stage 1) there is no frame topic on the vehicle;
   the frame's `video.index` message is its identity. The libx264 path, when it
   exists, sets `Frames=vehicle:cam.pilot`.
4. **`cam.decoded`'s stamp is the topside's clock at the frame's last packet,** not the
   vehicle's capture time translated (docs/11): translation needs the bridge's clock
   estimate. The capture time is in the referenced `video.index` message.
5. **`cam.decoded` is an event topic** (no `Rate=`): with no video coming in, the
   module is ready all the same and not restarted.
6. **Its size is fixed by the first frame decoded.** A frame of another size is dropped
   and said once; the topic's file is sized at its open (docs/01).
7. **A lost packet skips the frames up to the next keyframe** rather than concealing
   the loss.
8. **Not here yet:** the v4l2loopback webcam device that hellion fed (docs/11 lists
   it); and `topside_recorder`, whose session the recording follows, does not exist yet
   (`RecordDir=` stands in).
9. **Pixel formats:** 4:2:0 and 4:2:2, limited or full range (the camera's H.264 is
   4:2:0 limited); anything else is dropped and said once.
