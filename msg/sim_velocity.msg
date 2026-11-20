# msg/sim_velocity.msg: sim.velocity, the simulator's truth (docs/14-simulation.md)
f32 nu[6]         # body velocity: u v w (m/s, x forward, y right, z down), p q r (rad/s)
f32 vel_world[3]  # m/s, NED
f32 nu_dot[6]     # body acceleration at the step's start: m/s^2 and rad/s^2
