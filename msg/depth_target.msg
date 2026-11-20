# msg/depth_target.msg: ctl.depth_hold.target, the vertical motion the depth
# controller wants (docs/02, docs/09 "Example: adaptive depth hold")
f32  target_m   # m, positive down: the depth held
f32  depth_m    # m, positive down: nav.state's at this tick
f32  vel_z      # m/s, positive down: nav.state's at this tick
f32  accel_z    # m/s^2, positive down: the vertical acceleration the controller wants
f32  integral   # the integrator's share of the heave command, -1 to 1, up positive
u8   driving    # 1 while the mux gives the controller the heave axis (mux.state)
u8   latched    # 1 while the target follows the depth, the controller not driving
u8   reserved[2]
