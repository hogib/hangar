# msg/sim_thrust_truth.msg: sim.thrust, the simulator's truth (docs/14-simulation.md):
# what the thrusters push, after the T200 table and their lag
u16 pwm[8]      # us, the pulse widths commanded (SITL's servos 1-8, or thr.cmd)
f32 thrust[8]   # N per thruster, positive pushing as the frame defines
f32 wrench[6]   # body frame: N (x forward, y right, z down) and N m
f32 voltage     # V, the supply the table was read at
