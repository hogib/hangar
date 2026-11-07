# msg/policy_load.msg: policy.<name>.load, the progress of a load (docs/09-learning.md)
u32  version          # the version the load will get
u8   state            # 1 checking, 2 warming, 3 done, 4 rejected
u8   check            # with rejected: the check that failed, 1 to 8
u8   reserved[2]
u64  hash             # of the weights file
u64  since_ns         # CLOCK_MONOTONIC, when this state was entered
char reason[64]       # with rejected: why
