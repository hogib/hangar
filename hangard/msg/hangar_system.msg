# msg/hangar_system.msg: hangar.<role>.system, on change and at 1 Hz (docs/03,
# System state and the manager's topics)
u8   state          # 0 nominal, 1 degraded, 2 failsafe
u8   reserved[7]
u64  since_ns       # CLOCK_MONOTONIC, when this state was entered
u64  manager_ns     # CLOCK_MONOTONIC, when the manager published this
u32  modules        # modules the manager knows
u32  manager_pid
char reason[64]     # why: the module given up, the Neutral command that failed
