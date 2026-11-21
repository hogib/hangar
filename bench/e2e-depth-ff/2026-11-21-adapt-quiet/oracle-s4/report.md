| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.97 / 1.97 | 1.86, 9.3 %, 6.3 s | 3.01, 19.4 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.07 / 2.08 | 1.20, 0.7 %, 2.2 s | 1.92, 3.7 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.13 / 3.13 | 1.88, 2.2 %, 3.8 s | 2.55, 3.3 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.54 / 2.54 | 1.62, 0.6 %, 3.0 s | 2.58, 3.6 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.67 / 2.67 | 1.45, 10.6 %, 3.1 s | 1.85, 5.3 %, 3.0 s |
| P3 | residual | M1 again | payload | 100.0 | -1.99 / 2.00 | 1.88, 3.3 %, 3.6 s | 2.58, 3.5 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -2.02 / 2.02 | 2.94, 13.7 %, 10.8 s | 5.25, 27.6 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.6 | 0.42 | 0.0 | -2.03 / 2.03 |
| C1 | residual M1 | payload on | 3.0 | 0.51 | 0.0 | -2.35 / 2.36 |
| S2 | residual M1 to M2 | deploy and swap | 3.5 | 0.62 | 0.0 | -3.03 / 3.04 |
| C2 | residual M2 | payload off | 3.6 | 0.62 | 0.0 | -3.04 / 3.05 |
| C3 | residual M2 | payload on | 3.1 | 0.50 | 0.0 | -2.43 / 2.43 |
| S3 | residual M2 to M1 | revert | 2.5 | 0.45 | 0.0 | -2.16 / 2.16 |
| H2 | residual M1 to assisted | set_mode assisted | 12.6 | 0.96 | 7.9 | -1.70 / 1.72 |
| C4 | assisted | payload off | 2.1 | 0.38 | 0.0 | -1.99 / 2.00 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0847 | 0.0021 | -0.0849 |
| A2 | -0.0873 | 0.0021 | -0.0853 |
| P1 | -0.0675 | -0.0143 | -0.0862 |
| P2 | -0.0761 | -0.0075 | -0.0857 |
| N2 | -0.0741 | -0.0095 | -0.0860 |
| P3 | -0.0869 | 0.0011 | -0.0858 |
| P4 | -0.0852 | 0.0020 | -0.0849 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 15.8 | 5.2 | - |
| load: checking | 26.1 | 16.0 | - |
| load: warming | 40.2 | 20.0 | - |
| swapped (policy.active) | 220.5 | 200.1 | 660503.8 |
| first policy.wrench of the model | 240.5 | 220.2 | 660520.8 |
| first ctl.wrench carrying it | 70259.7 | 239.2 | 660539.8 |
| first thr.cmd carrying it | 70263.3 | 242.6 | 660542.9 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.45 | 0.172 | 0 | 0.013 / 0.443 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.54 | 0.297 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.51 | 0.213 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.59 | 0.325 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.32 | 0.170 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 21 | 19.03 / 20.34 | 0.235 | 0 | 0.024 / 0.033 |
| M1 | load | ctl.depth_hold | 21 | 19.03 / 20.48 | 0.306 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.03 / 20.46 | 0.243 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.08 / 20.08 | 0.222 | 0 | - |
| M1 | load | nav.state | 21 | 19.03 / 20.34 | 0.235 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 17.04 / 22.21 | 0.227 | 0 | 0.027 / 0.093 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.82 | 0.178 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.33 | 0.172 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.30 | 0.256 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 17.03 / 22.22 | 0.227 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 20.01 / 20.04 | 0.008 | 0 | 0.026 / 0.041 |
| M2 | load | ctl.depth_hold | 20 | 20.01 / 20.03 | 0.006 | 0 | - |
| M2 | load | ctl.wrench | 20 | 20.02 / 20.04 | 0.006 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.08 / 20.09 | 0.228 | 0 | - |
| M2 | load | nav.state | 19 | 20.01 / 20.03 | 0.005 | 0 | - |
