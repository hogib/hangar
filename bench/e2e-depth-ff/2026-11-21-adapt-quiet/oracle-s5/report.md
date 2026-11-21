| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.02 / 2.02 | 1.88, 9.4 %, 6.3 s | 3.04, 19.8 %, 10.2 s |
| A2 | residual | M1 | bare | 100.0 | -2.04 / 2.05 | 1.24, 1.2 %, 2.2 s | 1.91, 3.7 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.22 / 3.22 | 1.86, 1.9 %, 3.8 s | 2.57, 3.5 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.56 / 2.56 | 1.63, 0.6 %, 3.0 s | 2.58, 3.4 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.66 / 2.67 | 1.47, 10.8 %, 3.1 s | 1.85, 5.2 %, 3.0 s |
| P3 | residual | M1 again | payload | 100.0 | -1.99 / 1.99 | 1.87, 3.2 %, 3.6 s | 2.58, 3.3 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -2.01 / 2.01 | 2.94, 13.8 %, 10.8 s | 5.25, 27.4 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -2.09 / 2.10 |
| C1 | residual M1 | payload on | 2.8 | 0.50 | 0.0 | -2.32 / 2.33 |
| S2 | residual M1 to M2 | deploy and swap | 3.5 | 0.62 | 0.0 | -3.00 / 3.01 |
| C2 | residual M2 | payload off | 3.5 | 0.62 | 0.0 | -3.08 / 3.09 |
| C3 | residual M2 | payload on | 2.9 | 0.51 | 0.0 | -2.45 / 2.45 |
| S3 | residual M2 to M1 | revert | 2.4 | 0.44 | 0.0 | -2.15 / 2.15 |
| H2 | residual M1 to assisted | set_mode assisted | 12.3 | 0.93 | 8.0 | -1.69 / 1.71 |
| C4 | assisted | payload off | 2.1 | 0.38 | 0.0 | -1.99 / 1.99 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0028 | -0.0849 |
| A2 | -0.0872 | 0.0014 | -0.0860 |
| P1 | -0.0652 | -0.0159 | -0.0857 |
| P2 | -0.0761 | -0.0075 | -0.0857 |
| N2 | -0.0739 | -0.0088 | -0.0853 |
| P3 | -0.0873 | 0.0020 | -0.0854 |
| P4 | -0.0851 | 0.0022 | -0.0851 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.1 | 0.0 |
| deploy called | 12.5 | 3.0 | - |
| load: checking | 23.0 | 13.6 | - |
| load: warming | 40.0 | 13.9 | - |
| swapped (policy.active) | 220.3 | 199.8 | 660498.1 |
| first policy.wrench of the model | 240.2 | 219.8 | 660500.1 |
| first ctl.wrench carrying it | 70259.6 | 238.9 | 660519.1 |
| first thr.cmd carrying it | 70262.9 | 242.8 | 660522.7 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.49 | 0.210 | 0 | 0.027 / 0.274 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.44 | 0.227 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.52 | 0.250 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.42 | 0.285 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.03 / 20.48 | 0.207 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 19.88 / 20.16 | 0.047 | 0 | 0.021 / 0.046 |
| M1 | load | ctl.depth_hold | 21 | 19.13 / 20.10 | 0.200 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.84 / 20.12 | 0.050 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.09 / 20.09 | 0.221 | 0 | - |
| M1 | load | nav.state | 20 | 19.90 / 20.14 | 0.041 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.49 | 0.195 | 0 | 0.026 / 0.471 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.33 | 0.175 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.78 | 0.185 | 0 | - |
| M2 | before (10 s) | thr.cmd | 501 | 19.06 / 20.65 | 0.275 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.51 | 0.192 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 19.00 / 20.21 | 0.240 | 0 | 0.028 / 0.047 |
| M2 | load | ctl.depth_hold | 20 | 19.03 / 20.26 | 0.231 | 0 | - |
| M2 | load | ctl.wrench | 20 | 20.02 / 20.06 | 0.011 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.06 / 20.08 | 0.230 | 0 | - |
| M2 | load | nav.state | 19 | 19.01 / 20.21 | 0.238 | 0 | - |
