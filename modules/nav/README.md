# nav: the vehicle's state, `nav.state`

Publishes `nav.state` (`msg/nav_state.msg`) at 50 Hz on its own clock: the depth and
vertical velocity, estimated; the attitude and its rates, as `nav.attitude` gives them;
the body acceleration with gravity removed. It is the anchor of docs/09's depth-hold
dataset and the input of the depth controller (`modules/depth_hold`).

## The estimate

A Kalman filter of three states, all positive down: depth, vertical velocity, and the
accelerometer's vertical bias (`nav_filter.c`). Each tick predicts with the vertical
acceleration the IMU measures (`nav.imu`'s specific force rotated into the vertical by
`nav.attitude`'s quaternion, gravity added back, the bias subtracted); each new
`nav.depth` corrects it. Without a fresh, finite attitude and IMU, or with `use_imu 0`,
the prediction is constant velocity, the process noise standing in for the unknown
acceleration. The first depth starts the filter there, at rest.

| Parameter (`msg/nav_params.msg`, live) | Default | |
|---|---|---|
| `accel_noise` | 0.5 m/s² | the vertical acceleration's noise, the filter's process noise |
| `bias_walk` | 0.01 m/s³ | how fast the accelerometer's vertical bias may drift |
| `depth_noise` | 0.02 m | the depth sensor's noise |
| `use_imu` | 1 | 0: the velocity from the depth alone |

Measured by the suite (`make check-nav`), against a sine of 0.5 m at 0.5 rad/s with the
vehicle tilted 10° and 5°, the accelerometer biased 0.2 m/s² and noisy (0.05 m/s²), the
depth noisy (0.02 m): velocity RMS error **0.019 m/s** with the IMU, 0.030 from the depth
alone; the bias found within 0.0004 m/s². A step from rest to 0.3 m/s is within 10 %
after **0.02 s** with the IMU, 0.54 s without. The module at 50 Hz: 0.0 % of a core.

## Provenance and staleness

`nav.depth` is the `Inputs` topic; `nav.attitude` and `nav.imu` are `Sources`,
referenced on a tick only when used. So a stale IMU the filter does without does not flag
`nav.state`, and a stale depth does (docs/01, stale inputs): a controller driving from it
then has its output flagged, and the gate goes neutral (docs/07). Before the first depth
`nav.state` is published as zeros flagged stale with no references, so the module is
ready and the manager's staleness check holds, and no reader takes it for a measurement.

## How it waits

On its call topics alone (`set_params`), with the timeout to its next tick: the inputs
are read at the tick, every new `nav.depth` message applied (`hg_next`), the newest
attitude and IMU taken. A topic in the wait set that is read only at the tick would be
news between two ticks and make `hg_wait` return at once (hangar#1).

## Deviations from the docs

1. **`nav_params` is new.** The docs give `nav.state` a type and no module; this module's
   parameters are its filter's noises and the IMU switch.
2. **Stamps are the tick's time,** not the depth's capture time: the filter is predicted
   to the tick, and every depth message since the last tick is applied at it (one 20 ms
   tick of skew at most).
3. **The yaw and rates are passed through** from `nav.attitude`, not estimated.

## Tests (`make check-nav`, about 8 s)

The filter alone: the vertical acceleration from a tilted, accelerating body; the sine
and step runs above. The module under a manager, the suite as the vehicle's sensors:
zeros flagged stale before the first depth; a 0.2 m/s descent found (0.198) within
3 s; 50 messages a second; every input referenced; the IMU stopped (the depth alone, not
flagged, the IMU no longer referenced) and back; the depth stopped (flagged) and back;
`use_imu 0` through `hangar param set`; the idle CPU.
