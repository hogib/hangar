# msg/call_reply_envelope.msg: the reply envelope, 96 bytes, then the reply,
# absent unless status is HG_OK (docs/04-calls.md, The envelopes)
u64  call_id
u32  caller_id      # topic id of the request topic the request was read from
i32  status         # HG_OK or an error (Status codes)
u64  reply_type     # type hash
u64  received_ns    # CLOCK_MONOTONIC: when the server read the request
char reason[64]     # one line, when not HG_OK
