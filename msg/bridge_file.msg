# msg/bridge_file.msg: bridge.<role>.files, one message per file whose transfer
# ended, on the side that sent it and on the side that received it (docs/08-bridge.md
# "Files").
u64  id             # the transfer's id: bridge.send's reply on the sending side
u64  size           # bytes
u64  at_ns          # CLOCK_MONOTONIC of this machine, when the transfer ended
u8   state          # 1 sent: the other side has it, checked and visible; 2 received: here, checked and
                    # visible; 3 failed, sending; 4 failed, receiving: nothing of it is visible
u8   reserved[7]
char dest[33]       # the [Files <dest>] section
char name[65]       # the file's name
char blake2s[65]    # BLAKE2s-256 of the whole file, 64 hex digits; empty when it was never whole
char from[48]       # who called bridge.send, as the sending side names callers
char reason[61]     # with failed: why, one line
