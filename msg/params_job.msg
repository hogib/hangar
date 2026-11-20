# msg/params_job.msg: ardusub.params_job, an event topic: the progress and the end of
# a parameter dump or load of the vehicle's mavlink module (mavlink.params_dump,
# mavlink.params_load), one message a change of state and at most 2 a second between
u32  job         # counts from 1 in each instance of the module; 0: none yet (the first message)
u32  total       # the autopilot's parameter count, 0 until known
u32  received    # parameters read so far
u32  differing   # load: parameters of the file whose value differs from the autopilot's
u32  set_ok      # load: set and read back as the file says
u32  set_failed  # load: not confirmed after the retries
u32  unknown     # load: names in the file the autopilot does not have
u8   kind        # 1 dump, 2 load
u8   state       # 0 none yet, 1 running, 2 done, 3 failed
u8   reserved[2]
char file[160]   # the file written (dump) or read (load), absolute; job 0: the directory
char reason[96]  # why it failed, or what it skipped
