# msg/video_index.msg: video.index, one message per frame the encoder sends
# (docs/06-recorder.md "What it records", docs/11-electric-eye.md "encoder"):
# {frame topic id, frame seq, file, pts}, with the frame's size and kind.
# The message's stamp is the frame's capture time.
u32  frame_topic_id   # the frame topic encoded (cam.pilot, cam.raw); 0 for the camera's
                      # own H.264 (Source=uvc:), which is no hangar topic
u32  rtp_timestamp    # the RTP timestamp sent: frame_seq times the nominal period, 90 kHz
u64  frame_seq        # the frame's number: the topic's seq, or the camera's own count
u64  capture_ns       # CLOCK_MONOTONIC, when the frame was captured (or read, for Source=file:)
u32  bytes            # the frame's H.264, Annex B, as sent (parameter sets added included)
u8   keyframe         # 1 for an IDR frame
u8   params_added     # 1 when the encoder put cached SPS and PPS in front of it
u8   reserved[2]
char file[64]         # the MP4 of the session it went into; empty while none is written
