# msg/call_request_envelope.msg: the request envelope, 112 bytes, then the
# request (docs/04-calls.md, The envelopes)
u64  call_id        # per caller, counting from 1
u64  deadline_ns    # CLOCK_MONOTONIC: when the caller stops waiting
u64  request_type   # type hash
u32  target_id      # topic id of the server's <server>.rep
u32  reserved       # 0
char method[32]
char caller[48]     # empty; set by the manager on hangar.<role>.admin.req only
