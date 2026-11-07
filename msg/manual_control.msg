# msg/manual_control.msg: ardusub.manual_control, the MANUAL_CONTROL fields as
# sent, the command as it left the gate (docs/10-mavlink.md, Commands to ArduSub)
i16 x          # surge, -1000 to 1000
i16 y          # sway, -1000 to 1000
i16 z          # heave, 0 to 1000, 500 neutral
i16 r          # yaw, -1000 to 1000
u16 buttons    # always 0: ArduSub acts on no button
u16 buttons2   # always 0
i16 s          # roll, -1000 to 1000, MAVLink 2 extension
i16 t          # pitch, -1000 to 1000, MAVLink 2 extension
