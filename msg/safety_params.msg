# msg/safety_params.msg: the safety module's parameters (docs/07-safety.md:
# the trigger actions, the timeouts, the required modules). An action is
# 0 neutral, 1 surface, 2 hold; estop is always neutral and has none.
u8   action_module = 0     [0, 2]      # hangar.vehicle.system failsafe
u8   action_tether = 0     [0, 2]      # no packet from the topside for tether_timeout
u8   action_leak = 1       [0, 2]      # vehicle.leak wet
u8   action_battery = 1    [0, 2]      # power.battery below battery_min for battery_time
u8   action_depth = 1      [0, 2]      # nav.depth beyond the mux's band by depth_margin
u8   action_autopilot = 0  [0, 2]      # ArduSub disagreed, left MANUAL or fell silent
u8   action_input = 0      [0, 2]      # an input the safety module needs went stale
u8   tether_enabled = 0    [0, 1]      # until the bridge: there is no bridge.vehicle.link; on, the trigger fires at once
u8   leak_required = 1     [0, 1]      # arming needs vehicle.leak fresh and dry; its staleness is an input fault
u8   battery_required = 1  [0, 1]      # arming needs power.battery fresh and above battery_min
u8   depth_required = 1    [0, 1]      # nav.depth stale is an input fault
u8   pad0                           # (reserved is the params topic header's name)
f32  tether_timeout = 3    [0.5, 60]   # s
f32  battery_min = 11      [0, 60]     # V
f32  battery_time = 5      [0, 60]     # s below battery_min before the trigger fires
f32  depth_margin = 0.5    [0, 50]     # m beyond the mux's depth band
f32  hold_time = 10        [0, 600]    # s a failsafe hold lasts before it becomes neutral
f32  arm_timeout = 2       [0.5, 10]   # s for ArduSub to report armed before arming is given up
f32  autopilot_silence = 3 [0.5, 60]   # s without any message from ArduSub
char required[256] = "mux mavlink"     # modules that must be ready to arm, blank-separated
