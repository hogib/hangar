| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.03 / 2.03 | 1.87, 9.1 %, 6.3 s | 3.02, 19.8 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.06 / 2.07 | 1.21, 0.7 %, 2.2 s | 1.88, 3.5 %, 2.4 s |
| P1 | residual | M1 | payload | 100.0 | -3.20 / 3.21 | 1.86, 1.6 %, 3.9 s | 2.58, 3.3 %, 4.3 s |
| P2 | residual | M2 | payload | 100.0 | -2.59 / 2.59 | 1.62, 0.5 %, 3.0 s | 2.57, 3.2 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.66 / 2.66 | 1.46, 9.9 %, 3.0 s | 1.85, 5.1 %, 2.9 s |
| P3 | residual | M1 again | payload | 100.0 | -2.07 / 2.08 | 1.87, 3.3 %, 3.6 s | 2.59, 3.3 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -2.00 / 2.00 | 2.95, 13.5 %, 10.8 s | 5.27, 27.4 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.07 / 2.07 |
| C1 | residual M1 | payload on | 3.0 | 0.51 | 0.0 | -2.34 / 2.35 |
| S2 | residual M1 to M2 | deploy and swap | 3.4 | 0.60 | 0.0 | -2.97 / 2.98 |
| C2 | residual M2 | payload off | 3.5 | 0.62 | 0.0 | -3.05 / 3.05 |
| C3 | residual M2 | payload on | 3.1 | 0.52 | 0.0 | -2.46 / 2.46 |
| S3 | residual M2 to M1 | revert | 2.6 | 0.46 | 0.0 | -2.23 / 2.23 |
| H2 | residual M1 to assisted | set_mode assisted | 12.7 | 0.96 | 8.2 | -1.75 / 1.77 |
| C4 | assisted | payload off | 2.2 | 0.39 | 0.0 | -2.02 / 2.02 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0042 | -0.0846 |
| A2 | -0.0876 | 0.0020 | -0.0858 |
| P1 | -0.0613 | -0.0203 | -0.0860 |
| P2 | -0.0732 | -0.0094 | -0.0849 |
| N2 | -0.0701 | -0.0119 | -0.0847 |
| P3 | -0.0871 | 0.0021 | -0.0852 |
| P4 | -0.0850 | 0.0022 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 15.3 | 3.3 | - |
| load: checking | 23.9 | 15.6 | - |
| load: warming | 39.9 | 19.6 | - |
| swapped (policy.active) | 219.2 | 199.7 | 660514.4 |
| first policy.wrench of the model | 239.4 | 219.8 | 660519.4 |
| first ctl.wrench carrying it | 70258.6 | 238.8 | 660538.4 |
| first thr.cmd carrying it | 70262.3 | 242.5 | 660542.2 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.37 | 0.165 | 0 | 0.015 / 0.110 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.23 | 0.164 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.43 | 0.179 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.27 | 0.256 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.37 | 0.164 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 19.05 / 20.34 | 0.243 | 0 | 0.026 / 0.041 |
| M1 | load | ctl.depth_hold | 21 | 19.31 / 20.34 | 0.175 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.33 / 20.30 | 0.167 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.08 / 20.09 | 0.304 | 0 | - |
| M1 | load | nav.state | 20 | 19.03 / 20.33 | 0.246 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.28 | 0.126 | 0 | 0.015 / 0.039 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.62 | 0.131 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.63 | 0.151 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.08 | 0.241 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.27 | 0.126 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 20.01 / 20.02 | 0.003 | 0 | 0.018 / 0.026 |
| M2 | load | ctl.depth_hold | 20 | 19.02 / 20.03 | 0.223 | 0 | - |
| M2 | load | ctl.wrench | 20 | 20.02 / 20.03 | 0.003 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.06 / 20.07 | 0.314 | 0 | - |
| M2 | load | nav.state | 19 | 20.01 / 20.02 | 0.002 | 0 | - |
