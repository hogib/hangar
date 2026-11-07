# msg/safety_state.msg: safety.state, at 10 Hz and on every change (docs/07-safety.md)
u8   state            # 0 disarmed, 1 armed, 2 failsafe, 3 arming
u8   outputs_enabled  # whether actuators may drive
u8   action           # in failsafe: 0 neutral, 1 surface, 2 hold
u8   reserved
u32  triggers         # failsafe: bit per trigger that fired (Failsafe)
u64  since_ns         # CLOCK_MONOTONIC, when this state was entered
char by[48]           # who armed ("topside:pilot_console", "admin:<user>",
                      # "topside:admin:<user>"), or what triggered
char reason[64]       # why: a refused arm, ArduSub's refusal, a trigger's cause
