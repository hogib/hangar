# msg/topside_encoder_params.msg: topside_encoder's parameters (docs/11-electric-eye.md
# "encoder"; modules/topside_encoder/README.md). Both apply live, between two frames,
# through x264_encoder_reconfig: the target and the cap of the bitrate sent to
# QGroundControl. A target above the cap is refused.
u32  bitrate_kbps = 4000      [100, 50000]   # the target, kbit/s (hellion's 4000)
u32  max_bitrate_kbps = 8000  [100, 50000]   # the cap, kbit/s
