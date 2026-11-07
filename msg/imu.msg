# msg/imu.msg: nav.imu, from SCALED_IMU or RAW_IMU in phase 1 (docs/10-mavlink.md)
f32 gyro[3]    # rad/s, body x y z
f32 accel[3]   # m/s^2, body x y z, gravity included
f32 mag[3]     # gauss, body x y z
f32 temp_c     # degrees Celsius, the IMU's own
