# msg/depth_control_params.msg: the depth_hold module's controller (modules/depth_hold/README.md)
f32  kp = 4           [0, 20]     # 1/s^2: m/s^2 wanted per m of depth error
f32  kd = 4           [0, 20]     # 1/s: m/s^2 wanted per m/s of vertical velocity
f32  accel_max = 1    [0.01, 10]  # m/s^2: the largest vertical acceleration wanted
f32  ff = 0.1         [0, 2]      # heave per m/s^2 wanted: the feed-forward, mass over full thrust
f32  ki = 0.1         [0, 1]      # heave per m s of depth error: the integrator, for buoyancy and drag
f32  i_max = 0.3      [0, 1]      # the integrator's largest share of the heave command
f32  i_band = 0.3     [0, 100]    # m: the integrator learns only this close to the target, unsaturated
f32  max_thrust = 0.6 [0, 1]      # the largest heave command
f32  level_kp = 0     [0, 20]     # roll and pitch command per rad off level; 0 with level_kd 0: no levelling
f32  level_kd = 0     [0, 5]      # roll and pitch command per rad/s of roll or pitch rate
f32  level_max = 0.5  [0, 1]      # the largest roll, pitch or yaw command
f32  heading_kp = 0   [0, 20]     # yaw command per rad off the heading held; 0 with heading_kd 0: no heading hold
f32  heading_kd = 0   [0, 5]      # yaw command per rad/s of yaw rate
