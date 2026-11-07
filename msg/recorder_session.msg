# msg/recorder_session.msg: recorder.session, published when a session starts
# and when it stops, so that the modules writing into it follow it
# (docs/06-recorder.md, Sessions)
u8   active          # 1 while the session records
u8   reserved[7]
char id[64]          # "<YYYYmmdd-HHMMSS>-<host>"
char path[256]       # the session directory
u64  started_ns      # CLOCK_MONOTONIC, when the session started
