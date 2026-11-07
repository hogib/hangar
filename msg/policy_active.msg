# msg/policy_active.msg: policy.<name>.active, one message per swap or revert:
# the model now computing the output (docs/09-learning.md, Loading weights)
u32  version              # counts up per swap on this vehicle
u8   cause                # 0 start (current.hgw reloaded), 1 load, 2 revert
u8   reserved[3]
u64  hash                 # of the weights file
u64  arch                 # the architecture hash
u64  swapped_ns           # CLOCK_MONOTONIC
char dataset[32]          # the lineage's dataset
u64  dataset_hash         # of the spec
u8   newest_row_boot[16]  # vehicle boot id of the newest row trained on
u64  newest_row_stamp_ns  # its anchor_stamp_ns, on that boot's clock
u64  rows                 # rows trained on
