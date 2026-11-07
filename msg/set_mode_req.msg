# msg/set_mode_req.msg: mux.set_mode (docs/07-safety.md, The mux): "manual",
# "assisted" or "residual:<name>"; the hash of the model the operator checked
char mode[32]
u64  hash      # of the weights file, for residual:<name>; 0 otherwise
