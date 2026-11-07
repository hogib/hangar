# msg/hangar_modules.msg: hangar.<role>.modules, one message per module on
# change and at 1 Hz (docs/03, System state and the manager's topics); index
# and count let a reader assemble one round
char name[32]
u8   state          # 0 starting, 1 ready, 2 unhealthy, 3 backoff, 4 failed, 5 stopped, 6 refused, 7 stopping
u8   index          # position in this round
u8   count          # modules in this round
u8   exit_known     # last_exit is meaningful
i32  pid            # 0 when not running
u32  failures       # since the last ResetAfter
i32  last_exit      # exit status, or -signal
u64  since_ns       # CLOCK_MONOTONIC, when the state was entered
u64  published_ns   # CLOCK_MONOTONIC, when the manager published this
char status[128]    # the module's status line (hg_status)
