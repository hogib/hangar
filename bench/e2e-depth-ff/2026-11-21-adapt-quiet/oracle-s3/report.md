| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.04 / 2.04 | 1.84, 8.9 %, 6.3 s | 2.99, 19.6 %, 10.2 s |
| A2 | residual | M1 | bare | 100.0 | -2.04 / 2.05 | 1.23, 1.3 %, 2.3 s | 1.90, 3.5 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.25 / 3.26 | 1.89, 1.7 %, 4.0 s | 2.51, 3.1 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.73 / 2.73 | 1.67, 0.3 %, 3.0 s | 2.57, 3.5 %, 4.0 s |
| N2 | residual | M2 | bare | 100.0 | -2.65 / 2.66 | 1.47, 9.9 %, 3.0 s | 1.84, 5.3 %, 3.0 s |
| P3 | residual | M1 again | payload | 100.0 | -2.02 / 2.02 | 1.86, 3.6 %, 3.6 s | 2.54, 3.2 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -2.02 / 2.02 | 2.93, 13.5 %, 10.8 s | 5.26, 27.4 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.5 | 0.41 | 0.0 | -2.06 / 2.07 |
| C1 | residual M1 | payload on | 3.4 | 0.52 | 0.0 | -2.34 / 2.34 |
| S2 | residual M1 to M2 | deploy and swap | 3.4 | 0.60 | 0.0 | -3.04 / 3.04 |
| C2 | residual M2 | payload off | 3.6 | 0.61 | 0.0 | -3.03 / 3.03 |
| C3 | residual M2 | payload on | 2.9 | 0.51 | 0.0 | -2.43 / 2.43 |
| S3 | residual M2 to M1 | revert | 2.7 | 0.46 | 0.0 | -2.14 / 2.14 |
| H2 | residual M1 to assisted | set_mode assisted | 13.3 | 0.99 | 8.2 | -1.62 / 1.65 |
| C4 | assisted | payload off | 2.1 | 0.38 | 0.0 | -2.00 / 2.00 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0849 | 0.0024 | -0.0850 |
| A2 | -0.0868 | 0.0013 | -0.0857 |
| P1 | -0.0569 | -0.0228 | -0.0846 |
| P2 | -0.0688 | -0.0134 | -0.0849 |
| N2 | -0.0700 | -0.0114 | -0.0837 |
| P3 | -0.0866 | 0.0014 | -0.0851 |
| P4 | -0.0850 | 0.0017 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 0.3 | 19.7 | - |
| load: checking | 10.9 | 30.2 | - |
| load: warming | 19.9 | 40.2 | - |
| swapped (policy.active) | 199.6 | 220.4 | 660477.0 |
| first policy.wrench of the model | 220.2 | 239.4 | 660480.0 |
| first ctl.wrench carrying it | 70238.6 | 259.0 | 660499.0 |
| first thr.cmd carrying it | 70242.9 | 262.7 | 660502.6 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.46 | 0.255 | 0 | 0.034 / 0.442 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.45 | 0.276 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.50 | 0.273 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.53 | 0.324 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.45 | 0.251 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 19.02 / 20.59 | 0.292 | 0 | 0.025 / 0.042 |
| M1 | load | ctl.depth_hold | 20 | 19.98 / 20.04 | 0.014 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.05 / 20.05 | 0.217 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.08 / 20.09 | 0.222 | 0 | - |
| M1 | load | nav.state | 19 | 19.02 / 20.57 | 0.289 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 18.80 / 20.48 | 0.189 | 0 | 0.029 / 0.412 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.41 | 0.185 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.55 | 0.181 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.63 | 0.281 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.47 | 0.183 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 19.02 / 20.13 | 0.221 | 0 | 0.027 / 0.041 |
| M2 | load | ctl.depth_hold | 20 | 19.02 / 20.24 | 0.232 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.02 / 20.27 | 0.298 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.06 / 20.08 | 0.305 | 0 | - |
| M2 | load | nav.state | 21 | 19.02 / 20.15 | 0.222 | 0 | - |
