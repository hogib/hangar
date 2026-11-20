# msg/sim_pose.msg: sim.pose, the simulator's truth (docs/14-simulation.md,
# "sim_vehicle"): where the vehicle is, without noise or latency
f64 t         # s, simulated time since the start
f64 pos[3]    # m, world NED from the start point; pos[2] is depth, positive down
f32 q[4]      # unit quaternion w x y z, body to NED
f32 roll      # rad
f32 pitch     # rad
f32 yaw       # rad, from north
