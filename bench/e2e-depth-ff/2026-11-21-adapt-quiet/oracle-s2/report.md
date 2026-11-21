| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.99 / 2.00 | 1.84, 9.1 %, 6.3 s | 3.03, 19.8 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.05 / 2.06 | 1.22, 0.8 %, 2.3 s | 1.89, 3.7 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.17 / 3.17 | 1.87, 1.8 %, 3.9 s | 2.56, 3.3 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.56 / 2.57 | 1.63, 0.6 %, 3.0 s | 2.58, 3.4 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.62 / 2.62 | 1.46, 10.3 %, 3.0 s | 1.86, 5.4 %, 3.0 s |
| P3 | residual | M1 again | payload | 100.0 | -2.01 / 2.01 | 1.85, 3.2 %, 3.6 s | 2.59, 3.4 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -1.99 / 1.99 | 2.94, 13.5 %, 10.9 s | 5.24, 27.4 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.09 / 2.10 |
| C1 | residual M1 | payload on | 3.1 | 0.52 | 0.0 | -2.41 / 2.41 |
| S2 | residual M1 to M2 | deploy and swap | 3.5 | 0.61 | 0.0 | -3.01 / 3.01 |
| C2 | residual M2 | payload off | 3.5 | 0.62 | 0.0 | -3.07 / 3.08 |
| C3 | residual M2 | payload on | 3.0 | 0.51 | 0.0 | -2.45 / 2.46 |
| S3 | residual M2 to M1 | revert | 2.6 | 0.45 | 0.0 | -2.20 / 2.21 |
| H2 | residual M1 to assisted | set_mode assisted | 12.1 | 0.93 | 8.1 | -1.74 / 1.76 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.96 / 1.96 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0016 | -0.0849 |
| A2 | -0.0871 | 0.0017 | -0.0855 |
| P1 | -0.0642 | -0.0168 | -0.0854 |
| P2 | -0.0753 | -0.0081 | -0.0855 |
| N2 | -0.0730 | -0.0103 | -0.0858 |
| P3 | -0.0868 | 0.0018 | -0.0852 |
| P4 | -0.0850 | 0.0027 | -0.0849 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 1.4 | 1.0 | - |
| load: checking | 11.8 | 11.4 | - |
| load: warming | 19.8 | 20.4 | - |
| swapped (policy.active) | 200.1 | 200.7 | 660475.3 |
| first policy.wrench of the model | 220.3 | 220.7 | 660480.3 |
| first ctl.wrench carrying it | 70238.7 | 238.6 | 660499.4 |
| first thr.cmd carrying it | 70243.2 | 242.8 | 660502.4 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.35 | 0.187 | 0 | 0.037 / 0.094 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.52 | 0.208 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.40 | 0.239 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.43 | 0.285 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.35 | 0.187 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 20.00 / 20.24 | 0.066 | 0 | 0.022 / 0.055 |
| M1 | load | ctl.depth_hold | 20 | 19.03 / 20.26 | 0.232 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.06 / 20.24 | 0.223 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.07 / 20.17 | 0.306 | 0 | - |
| M1 | load | nav.state | 19 | 20.01 / 20.23 | 0.063 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.35 | 0.223 | 0 | 0.030 / 0.356 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.72 | 0.236 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.54 | 0.236 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.41 | 0.282 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.34 | 0.219 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 20 | 19.03 / 20.03 | 0.223 | 0 | 0.013 / 0.018 |
| M2 | load | ctl.depth_hold | 20 | 19.03 / 20.03 | 0.223 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.01 / 20.03 | 0.224 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.06 / 20.06 | 0.223 | 0 | - |
| M2 | load | nav.state | 20 | 19.03 / 20.03 | 0.223 | 0 | - |
