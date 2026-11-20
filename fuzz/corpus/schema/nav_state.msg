# msg/nav_state.msg: nav.state, the anchor of the depth-hold dataset (docs/06, docs/09)
f32 depth_m     # m, positive down
f32 vel_z       # m/s, positive down
f32 roll        # rad
f32 pitch       # rad
f32 yaw         # rad, from north
f32 rates[3]    # rad/s, body roll pitch yaw rates
f32 accel[3]    # m/s^2, body x y z, gravity removed
