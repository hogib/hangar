# msg/set_params_rep.msg: the reply of every module's set_params method
# (docs/05-parameters.md, Setting parameters)
u64  version      # the version now in force, or, with restarting, the one the new instance starts with
u8   restarting   # 1: a @restart field changed; the module restarts to apply it
