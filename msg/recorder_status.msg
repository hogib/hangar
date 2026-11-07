# msg/recorder_status.msg: recorder.status, on change and once a second while
# recording (docs/06-recorder.md, Sessions)
u8   state            # 0 idle, 1 recording, 2 dumping the black box
u8   reserved[7]
char session[64]      # the session directory's name, "<YYYYmmdd-HHMMSS>-<host>"; "" when idle
u64  bytes_written    # in this session
u64  free_bytes       # on the recording directory's filesystem
u32  gaps             # gaps so far in this session
u32  topics           # topics being recorded
u64  blackbox_ns      # how far back the black box reaches: the least over its topics
