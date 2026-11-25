# msg/encoder_params.msg: the encoder's parameters: the camera's controls
# (modules/encoder/README.md "Camera controls"). The encoder holds the camera, so
# exposure, gain, white balance and the image adjustments are set through it; nothing
# else on the vehicle can. The units are lib/camera/camera_ctrl.h's, the same on any
# camera: each is converted to the device's own range, and a control the camera does
# not have is skipped and said once in the log.
#
# -1 in any field: leave that control as the camera has it. They apply live, between
# two frames, and are applied again at every start (a camera that was unplugged comes
# back with its own defaults). A manual value needs its automatic mode switched off in
# the same set: exposure_us with auto_exposure=0, white_balance_k with
# auto_white_balance=0; anything else is refused, because the camera would refuse it.
i32  auto_exposure = -1       [-1, 1]        # 1 automatic, 0 manual
i32  exposure_us = -1         [-1, 1000000]  # microseconds; needs auto_exposure=0
i32  gain = -1                [-1, 100]      # percent of the camera's range
i32  auto_white_balance = -1  [-1, 1]        # 1 automatic, 0 manual
i32  white_balance_k = -1     [-1, 10000]    # kelvin; needs auto_white_balance=0
i32  brightness = -1          [-1, 200]      # 100 neutral
i32  contrast = -1            [-1, 200]      # 100 neutral
i32  saturation = -1          [-1, 200]      # 100 neutral
i32  sharpness = -1           [-1, 200]      # 100 neutral
i32  keep_framerate = 1       [-1, 1]        # 1: automatic exposure may not lower the frame rate
