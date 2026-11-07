# msg/ardusub_state.msg: ardusub.state, at 10 Hz, on every change, and after the
# module acts on each new safety.state (docs/10-mavlink.md, Arming and failsafe)
u8   armed            # 0 disarmed, 1 armed (HEARTBEAT base_mode), 2 disarming:
                      # a disarm hangar asked for, sent and not yet answered
u8   system_status    # HEARTBEAT system_status: CRITICAL or EMERGENCY in an ArduSub failsafe
u8   params           # 0 not read yet, 1 every parameter of the file reads as the
                      # file says, 2 a mismatch (the module's status line names it)
u8   reserved
u32  mode             # ArduSub's flight mode (HEARTBEAT custom_mode); 19 is MANUAL
u64  last_rx_ns       # CLOCK_MONOTONIC, when the last message of any kind arrived from ArduSub
u32  arm_attempt      # counts mavlink.arm requests
u8   arm_result       # MAV_RESULT of the last attempt; 255 while pending
u8   reserved2[3]
char arm_reason[100]  # the PreArm:/Arm: lines, or the MAV_RESULT name
char failsafe[50]     # ArduSub's last failsafe STATUSTEXT
