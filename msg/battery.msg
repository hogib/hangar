# msg/battery.msg: power.battery, from SYS_STATUS and BATTERY_STATUS (docs/10-mavlink.md)
f32 voltage_v   # V
f32 current_a   # A, discharge positive
f32 remaining   # fraction 0 to 1 of capacity left; -1 unknown
