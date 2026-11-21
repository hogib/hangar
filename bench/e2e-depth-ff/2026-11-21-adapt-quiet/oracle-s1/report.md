| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.03 / 2.03 | 1.86, 9.2 %, 6.3 s | 3.01, 19.8 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.06 / 2.06 | 1.21, 0.7 %, 2.2 s | 1.87, 3.4 %, 2.4 s |
| P1 | residual | M1 | payload | 100.0 | -3.21 / 3.21 | 1.88, 1.5 %, 3.9 s | 2.58, 3.3 %, 4.3 s |
| P2 | residual | M2 | payload | 100.0 | -2.58 / 2.58 | 1.63, 0.5 %, 3.0 s | 2.56, 3.2 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.66 / 2.66 | 1.46, 9.8 %, 3.0 s | 1.83, 5.1 %, 2.9 s |
| P3 | residual | M1 again | payload | 100.0 | -2.07 / 2.08 | 1.88, 3.3 %, 3.6 s | 2.58, 3.3 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -2.01 / 2.01 | 2.94, 13.6 %, 10.8 s | 5.26, 27.5 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.08 / 2.08 |
| C1 | residual M1 | payload on | 3.0 | 0.50 | 0.0 | -2.34 / 2.35 |
| S2 | residual M1 to M2 | deploy and swap | 3.4 | 0.60 | 0.0 | -2.97 / 2.98 |
| C2 | residual M2 | payload off | 3.5 | 0.61 | 0.0 | -3.04 / 3.04 |
| C3 | residual M2 | payload on | 3.1 | 0.51 | 0.0 | -2.45 / 2.45 |
| S3 | residual M2 to M1 | revert | 2.6 | 0.46 | 0.0 | -2.23 / 2.23 |
| H2 | residual M1 to assisted | set_mode assisted | 12.7 | 0.96 | 8.2 | -1.74 / 1.76 |
| C4 | assisted | payload off | 2.2 | 0.39 | 0.0 | -2.02 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0042 | -0.0846 |
| A2 | -0.0876 | 0.0021 | -0.0857 |
| P1 | -0.0605 | -0.0205 | -0.0858 |
| P2 | -0.0733 | -0.0092 | -0.0847 |
| N2 | -0.0703 | -0.0120 | -0.0849 |
| P3 | -0.0872 | 0.0021 | -0.0853 |
| P4 | -0.0850 | 0.0021 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 4.3 | 4.5 | - |
| load: checking | 14.8 | 15.2 | - |
| load: warming | 19.8 | 19.2 | - |
| swapped (policy.active) | 199.6 | 199.3 | 660479.8 |
| first policy.wrench of the model | 219.6 | 219.3 | 660479.9 |
| first ctl.wrench carrying it | 70238.4 | 238.4 | 660498.9 |
| first thr.cmd carrying it | 70242.2 | 242.6 | 660503.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.46 | 0.155 | 0 | 0.025 / 0.185 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.29 | 0.167 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.45 | 0.193 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.56 | 0.326 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.46 | 0.154 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 19.04 / 20.55 | 0.271 | 0 | 0.020 / 0.047 |
| M1 | load | ctl.depth_hold | 20 | 19.86 / 20.19 | 0.054 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.01 / 20.68 | 0.326 | 0 | - |
| M1 | load | thr.cmd | 19 | 19.08 / 20.10 | 0.314 | 0 | - |
| M1 | load | nav.state | 19 | 19.05 / 20.52 | 0.266 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.03 / 20.47 | 0.208 | 0 | 0.028 / 0.074 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.64 | 0.206 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.48 | 0.222 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.50 | 0.298 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.03 / 20.46 | 0.207 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 20.01 / 20.06 | 0.017 | 0 | 0.018 / 0.049 |
| M2 | load | ctl.depth_hold | 20 | 19.03 / 20.06 | 0.225 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.06 / 20.24 | 0.230 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.28 / 20.38 | 0.198 | 0 | - |
| M2 | load | nav.state | 19 | 20.01 / 20.05 | 0.014 | 0 | - |
