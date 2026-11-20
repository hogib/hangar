# msg/runtime_info.msg: what a policy's runtime runs (docs/09-learning.md,
# "Ops"): the trainer's exporter checks a model against it before it sends.
u32  format_version   # of the .hgw files it reads
u32  n_ops            # entries in ops
u32  max_inputs       # model inputs and outputs a model may have
u32  max_outputs
u64  arch             # the policy's Arch: the architecture hash a weights file must have
char dataset[32]      # the policy's Dataset
u64  dataset_hash     # its spec's hash (the row type's)
u64  tick_period_ns   # the anchor's declared period
char ops[384]         # "<type>:<version>" separated by spaces, in the registry's order
