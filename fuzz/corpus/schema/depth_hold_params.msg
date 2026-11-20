# msg/depth_hold_params.msg
f32  kp = 1.5          [0, 10]   # N/m
f32  ki = 0.1          [0, 2]    # N/(m s)
f32  max_thrust = 0.6  [0, 1]    # fraction of full thrust
f32  gains[3] = 1, 1, 0.5        # one value applies to every element
u8   enabled = 0       [0, 1]
char frame[16] = "ned"
u32  rate_hz = 50      [10, 200] @restart
