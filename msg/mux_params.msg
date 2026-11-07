# msg/mux_params.msg: the mux's parameters: the envelope, the residual
# envelope, the pilot's override and the demotion rules (docs/07-safety.md, The mux)
f32  max[6] = 1              [0, 1]       # envelope: the largest command per axis, surge sway heave roll pitch yaw
f32  rate_max[6] = 8         [0.1, 1000]  # the largest change per second, per axis
f32  depth_min = 0           [-10, 1000]  # m: the depth band; no command pushing out of it passes
f32  depth_max = 50          [0, 1000]    # m
f32  deadband = 0.05         [0, 0.5]     # pilot input above it takes the axis
f32  override_hold = 1       [0, 10]      # s the pilot keeps an axis after letting go
f32  residual_frac = 0.2     [0, 1]       # the residual envelope, as a fraction of the envelope per axis
f32  demote_saturation = 0.2 [0, 1]       # fraction of demote_window at a residual limit that demotes
f32  demote_window = 5       [0.5, 60]    # s
f32  surface_thrust = 0.5    [0, 1]       # heave up commanded in a failsafe surface
u8   controller_axes = 4     [0, 63]      # bits 0-5 surge..yaw: the axes ctl.depth_hold holds in assisted
