# msg/thruster_cmd.msg: thr.cmd, the pulse widths for the thrusters when hangar
# drives them directly (phase 2, docs/07-safety.md "The actuator gate"); read by
# sim_vehicle in place of ArduSub's servos (docs/14-simulation.md)
u16 pwm[8]   # us, thrusters 1 to 8 in ArduSub's numbering; 1500 is stopped
