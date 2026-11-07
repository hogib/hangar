# msg/ardusub_param.msg: ardusub.param, an event topic, one message per
# PARAM_VALUE received from ArduSub (docs/10-mavlink.md, ArduSub's parameters)
char id[17]      # the parameter's name
u8   type        # MAV_PARAM_TYPE
u8   reserved[2]
f32  value
u16  index
u16  count
