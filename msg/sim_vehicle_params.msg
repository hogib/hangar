# msg/sim_vehicle_params.msg: the simulator's parameters (docs/14-simulation.md,
# "sim_vehicle"): the vehicle, the water, the thrusters, the battery and the
# sensors, every one live. Defaults are lib/sim's BlueROV2 heavy configuration
# and its typical sensors (lib/sim/README.md names each value's source).
f32 mass = 11.5              [1, 200]     # kg, the vehicle without payload; its weight is mass x 9.81
f32 buoyancy = 114.8         [0, 5000]    # N
f32 d_lin[6] = 4.03, 6.22, 5.18, 0.07, 0.07, 0.07       # linear damping per axis, N s/m and N m s/rad
f32 d_quad[6] = 18.18, 21.66, 36.99, 1.55, 1.55, 1.55   # quadratic damping per axis
f32 payload_mass = 0         [0, 100]     # kg, a rigid payload without buoyancy of its own
f32 payload_pos[3] = 0       [-2, 2]      # m, its position from the CG, body frame (x forward, y right, z down)
f32 current[3] = 0           [-5, 5]      # m/s, the water's velocity, world NED
f32 floor_depth = 0          [0, 10000]   # m, the sea floor; 0 none
f32 thruster_lag = 0.05      [0, 2]       # s, each thruster's first-order lag
f32 battery_ah = 18          [0.1, 200]   # Ah, the battery's capacity
f32 battery_full_v = 16.8    [1, 60]      # V at full charge
f32 battery_empty_v = 13.2   [1, 60]      # V empty
f32 amps_per_newton = 0.5    [0, 10]      # A drawn per N of thrust, summed over thrusters (an estimate)
f32 idle_current = 1         [0, 50]      # A with the thrusters stopped
f32 gyro_noise = 0.0007      [0, 1]       # rad/s, sd
f32 gyro_bias = 0.002        [-1, 1]      # rad/s
f32 accel_noise = 0.01       [0, 10]      # m/s^2, sd
f32 accel_bias = 0.02        [-10, 10]    # m/s^2
f32 imu_latency = 0.005      [0, 1]       # s
f32 depth_noise = 0.005      [0, 10]      # m, sd
f32 depth_bias = 0.02        [-10, 10]    # m
f32 depth_latency = 0.02     [0, 1]       # s
f32 attitude_noise = 0.0087  [0, 1]       # rad, sd
f32 attitude_bias = 0        [-1, 1]      # rad
f32 attitude_latency = 0.02  [0, 1]       # s
