# msg/thruster_out.msg: thr.out, from SERVO_OUTPUT_RAW: the pulse widths
# actually sent to the thrusters, after ArduSub's mixing (docs/10-mavlink.md)
u16 pwm[8]   # us, servo outputs 1 to 8; 1500 is stopped
