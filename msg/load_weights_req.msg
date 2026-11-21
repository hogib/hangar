# msg/load_weights_req.msg: policy_<name>.load_weights (docs/09-learning.md,
# "Loading weights"): the weights file to check, warm up and swap in.
char name[96]       # the file's name in the policy's incoming directory (no '/'): put there by
                    # the bridge (docs/08 "Files") or, on the policy's own machine, by the trainer
char blake2s[65]    # BLAKE2s-256 of the whole file, 64 hex digits (check 1)
