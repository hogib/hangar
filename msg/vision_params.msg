# msg/vision_params.msg: the vision module's parameters (docs/11-electric-eye.md
# "vision"): the pilot's picture. A preset names a whole chain (the five built in,
# or a [Preset] of vision.conf; modules/vision/README.md); "custom" runs the eight
# stages below as given. The stage fields are eeye's EffectStage, one array each.
char preset[16] = "normal"             # normal, auto, edges, sharpen, lowlight, a vision.conf preset, or custom
u8   auto_visibility = 0  [0, 1]       # custom: auto visibility ahead of the chain
u8   stage_count = 0      [0, 8]       # custom: how many of the stages run, in order
u8   effect[8] = 0        [0, 10]      # 0 none 1 grayscale 2 invert 3 threshold 4 tint 5 sobel 6 blur 7 contrast 8 light 9 log 10 canny
u8   threshold_value[8] = 128          # threshold: the cut
u8   tint_u[8] = 128                   # tint: the colour, U
u8   tint_v[8] = 128                   # tint: the colour, V
u8   tint_strength[8] = 0              # tint: 0 none, 255 full
u8   sobel_threshold[8] = 0            # sobel: gradients below it are 0
u8   blur_strength[8] = 1              # blur: passes of the 5-tap kernel
u8   light_level[8] = 128              # light: 128 neutral, below darker, above brighter and more saturated
u8   log_strength[8] = 1               # log: Gaussian passes before the Laplacian
u8   log_threshold[8] = 0              # log: the slope a zero-crossing needs
u8   canny_strength[8] = 1             # canny: Gaussian passes before the gradient
u8   canny_low[8] = 40                 # canny: hysteresis, low
u8   canny_high[8] = 90                # canny: hysteresis, high
u8   threads = 0          [0, 16] @restart  # the operators' thread pool; 0: one per core
