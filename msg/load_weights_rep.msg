# msg/load_weights_rep.msg: the reply to load_weights, sent at once: the work
# goes on beside the ticks and its progress is policy.<name>.load (docs/09).
u32  version        # the version the load will get when it is swapped in
u8   reserved[4]
