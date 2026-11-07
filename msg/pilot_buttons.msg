# msg/pilot_buttons.msg: pilot.buttons, one message with every pilot.wrench
# (docs/10-mavlink.md, Buttons): bit n for QGroundControl's button n among
# the buttons marked pass in topside_mavlink.conf
u32 pressed    # bit n: button n is held
u32 reserved
