# msg/policy_version.msg: the reply to a policy's revert: the model now
# running, as policy.<name>.active will announce it (docs/09-learning.md).
u32  version        # the version announced
u8   reserved[4]
u64  hash           # of the weights file: the first 8 bytes of its BLAKE2s-256, little-endian
