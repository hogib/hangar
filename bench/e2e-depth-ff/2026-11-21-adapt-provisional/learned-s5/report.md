| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.02 / 2.03 | 1.86, 8.7 %, 6.4 s | 3.04, 19.8 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -1.97 / 1.98 | 1.22, 0.9 %, 2.3 s | 1.81, 3.2 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -2.90 / 2.91 | 1.88, 1.4 %, 3.9 s | 2.57, 3.4 %, 4.1 s |
| P2 | residual | M2 | payload | 100.0 | -2.52 / 2.52 | 1.70, 0.0 %, 3.3 s | 2.55, 3.2 %, 4.0 s |
| N2 | residual | M2 | bare | 100.0 | -2.59 / 2.60 | 1.43, 8.9 %, 2.8 s | 1.77, 3.4 %, 2.2 s |
| P3 | residual | M1 again | payload | 100.0 | -2.10 / 2.11 | 1.91, 3.6 %, 3.6 s | 2.58, 3.4 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.00 / 2.00 | 2.93, 13.5 %, 10.8 s | 5.26, 27.6 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.40 | 0.0 | -1.99 / 1.99 |
| C1 | residual M1 | payload on | 3.2 | 0.53 | 0.0 | -2.48 / 2.48 |
| S2 | residual M1 to M2 | deploy and swap | 3.3 | 0.56 | 0.0 | -2.76 / 2.76 |
| C2 | residual M2 | payload off | 3.2 | 0.56 | 0.0 | -2.82 / 2.82 |
| C3 | residual M2 | payload on | 2.9 | 0.51 | 0.0 | -2.47 / 2.48 |
| S3 | residual M2 to M1 | revert | 2.7 | 0.49 | 0.0 | -2.28 / 2.28 |
| H2 | residual M1 to assisted | set_mode assisted | 12.1 | 0.93 | 8.0 | -1.70 / 1.71 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.95 / 1.95 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0003 | -0.0846 |
| A2 | -0.0841 | -0.0017 | -0.0859 |
| P1 | -0.0538 | -0.0240 | -0.0812 |
| P2 | -0.0674 | -0.0111 | -0.0804 |
| N2 | -0.0616 | -0.0184 | -0.0823 |
| P3 | -0.0817 | -0.0014 | -0.0838 |
| P4 | -0.0850 | 0.0009 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 8.8 | 10.5 | - |
| load: checking | 466.9 | 469.4 | - |
| load: warming | 479.9 | 480.4 | - |
| swapped (policy.active) | 660.0 | 659.7 | 661399.2 |
| first policy.wrench of the model | 680.0 | 679.7 | 661400.2 |
| first ctl.wrench carrying it | 70699.1 | 698.6 | 661419.3 |
| first thr.cmd carrying it | 70703.1 | 702.9 | 661423.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.52 | 0.256 | 0 | 0.032 / 0.125 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.03 / 20.63 | 0.257 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.59 | 0.269 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.59 | 0.292 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.50 | 0.254 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.03 / 20.78 | 0.207 | 0 | 0.022 / 0.033 |
| M1 | load | ctl.depth_hold | 43 | 19.01 / 20.78 | 0.304 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.73 / 20.03 | 0.048 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.08 | 0.261 | 0 | - |
| M1 | load | nav.state | 43 | 19.02 / 20.77 | 0.208 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.43 | 0.163 | 0 | 0.019 / 0.089 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.57 | 0.206 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.39 | 0.186 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.54 | 0.270 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.44 | 0.162 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 42 | 19.04 / 20.06 | 0.152 | 0 | 0.015 / 0.026 |
| M2 | load | ctl.depth_hold | 43 | 20.01 / 20.14 | 0.019 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.16 / 20.04 | 0.132 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.07 | 0.260 | 0 | - |
| M2 | load | nav.state | 42 | 19.04 / 20.06 | 0.153 | 0 | - |
