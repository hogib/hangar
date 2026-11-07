# msg/attitude.msg: nav.attitude, from ATTITUDE in phase 1 (docs/10-mavlink.md)
f32 q[4]        # unit quaternion w x y z, body to NED
f32 roll        # rad
f32 pitch       # rad
f32 yaw         # rad, from north
f32 rates[3]    # rad/s, body roll pitch yaw rates
