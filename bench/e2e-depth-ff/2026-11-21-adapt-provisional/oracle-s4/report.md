| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.97 / 1.97 | 1.86, 9.2 %, 6.3 s | 3.02, 19.9 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.08 / 2.08 | 1.21, 0.8 %, 2.2 s | 1.91, 3.7 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.13 / 3.13 | 1.88, 2.2 %, 3.8 s | 2.56, 3.3 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.56 / 2.56 | 1.64, 0.6 %, 3.0 s | 2.59, 3.6 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.69 / 2.69 | 1.46, 10.4 %, 3.0 s | 1.86, 5.3 %, 3.0 s |
| P3 | residual | M1 again | payload | 100.0 | -1.99 / 1.99 | 1.88, 3.3 %, 3.6 s | 2.59, 3.5 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -2.02 / 2.02 | 2.94, 13.7 %, 10.8 s | 5.26, 27.6 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.6 | 0.42 | 0.0 | -2.03 / 2.03 |
| C1 | residual M1 | payload on | 3.0 | 0.51 | 0.0 | -2.34 / 2.35 |
| S2 | residual M1 to M2 | deploy and swap | 3.5 | 0.62 | 0.0 | -3.06 / 3.06 |
| C2 | residual M2 | payload off | 3.5 | 0.62 | 0.0 | -3.04 / 3.05 |
| C3 | residual M2 | payload on | 3.0 | 0.50 | 0.0 | -2.42 / 2.43 |
| S3 | residual M2 to M1 | revert | 2.5 | 0.45 | 0.0 | -2.15 / 2.15 |
| H2 | residual M1 to assisted | set_mode assisted | 12.4 | 0.94 | 7.9 | -1.72 / 1.73 |
| C4 | assisted | payload off | 2.1 | 0.38 | 0.0 | -1.98 / 1.98 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0847 | 0.0023 | -0.0848 |
| A2 | -0.0874 | 0.0018 | -0.0858 |
| P1 | -0.0676 | -0.0139 | -0.0858 |
| P2 | -0.0757 | -0.0080 | -0.0860 |
| N2 | -0.0739 | -0.0102 | -0.0868 |
| P3 | -0.0870 | 0.0014 | -0.0855 |
| P4 | -0.0851 | 0.0018 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 16.8 | 5.2 | - |
| load: checking | 25.6 | 14.0 | - |
| load: warming | 39.6 | 20.0 | - |
| swapped (policy.active) | 219.8 | 200.3 | 660497.2 |
| first policy.wrench of the model | 239.8 | 220.3 | 660499.2 |
| first ctl.wrench carrying it | 70258.0 | 239.2 | 660518.2 |
| first thr.cmd carrying it | 70262.6 | 242.9 | 660522.3 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.98 / 20.46 | 0.182 | 0 | 0.019 / 0.457 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.53 | 0.162 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.51 | 0.171 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.43 | 0.283 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.42 | 0.180 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 21 | 19.43 / 20.40 | 0.157 | 0 | 0.026 / 0.038 |
| M1 | load | ctl.depth_hold | 21 | 19.12 / 20.44 | 0.259 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.03 / 20.39 | 0.247 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.07 / 20.08 | 0.306 | 0 | - |
| M1 | load | nav.state | 21 | 19.43 / 20.38 | 0.155 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.18 | 0.141 | 0 | 0.019 / 0.044 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.39 | 0.154 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.05 | 0.141 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.237 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.18 | 0.140 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 20.01 / 20.04 | 0.008 | 0 | 0.014 / 0.024 |
| M2 | load | ctl.depth_hold | 20 | 19.01 / 20.03 | 0.224 | 0 | - |
| M2 | load | ctl.wrench | 20 | 20.02 / 20.04 | 0.009 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.06 / 20.07 | 0.229 | 0 | - |
| M2 | load | nav.state | 19 | 20.01 / 20.04 | 0.008 | 0 | - |
