# msg/nav_params.msg: the nav module's estimator of vertical motion (modules/nav/README.md)
f32  accel_noise = 0.5   [0.001, 20]  # m/s^2: the vertical acceleration's noise, the filter's process noise
f32  bias_walk = 0.01    [0, 1]       # m/s^3: how fast the accelerometer's vertical bias may drift
f32  depth_noise = 0.02  [0.0001, 2]  # m: the depth sensor's noise
u8   use_imu = 1         [0, 1]       # 0: the vertical velocity from the depth alone (constant velocity)
