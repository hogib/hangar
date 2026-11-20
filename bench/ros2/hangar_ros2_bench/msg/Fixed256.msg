# 256 bytes, fixed: seq, the CLOCK_MONOTONIC time of the publish, and 30 words that each hold seq.
uint64 seq
uint64 stamp_ns
uint64[30] w
