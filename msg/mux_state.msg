# msg/mux_state.msg: mux.state, on every change and at 10 Hz (docs/07-safety.md, The mux)
char mode[24]         # "manual", "depth_hold", "residual:<name>"
u8   source_surge     # 0 none, 1 the pilot, 2 a controller, 3 an autonomy source, 4 a policy
u8   source_sway
u8   source_heave
u8   source_roll
u8   source_pitch
u8   source_yaw
u8   residual_active  # 1 while a policy's contribution is being added
u8   reserved
u32  limits           # bit per limit acting: 0-5 envelope surge..yaw, 6-11 rate surge..yaw, 12 depth band, 13-18 residual envelope surge..yaw
