# msg/recorder_clock.msg: the records of hangar.clock.hgr, once a second in
# every session (docs/06-recorder.md, Sessions)
u64 monotonic_ns   # CLOCK_MONOTONIC
u64 realtime_ns    # CLOCK_REALTIME
i64 offset_ns      # the time daemon's estimated offset of the wall clock; 0 when none is known
