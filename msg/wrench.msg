# msg/wrench.msg: a normalized command, -1 to 1 per axis: the fraction of
# ArduSub's full command at pilot gain 1 (docs/10-mavlink.md, Units)
f32 surge   # -1 to 1, forward positive
f32 sway    # -1 to 1, right positive
f32 heave   # -1 to 1, up positive
f32 roll    # -1 to 1
f32 pitch   # -1 to 1
f32 yaw     # -1 to 1, clockwise seen from above positive
