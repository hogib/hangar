| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.98 / 1.99 | 1.86, 9.4 %, 6.2 s | 3.02, 19.6 %, 10.2 s |
| A2 | residual | M1 | bare | 100.0 | -2.01 / 2.02 | 1.22, 0.5 %, 2.4 s | 1.79, 3.3 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -2.79 / 2.79 | 1.86, 1.1 %, 4.0 s | 2.59, 3.6 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.48 / 2.49 | 1.70, 0.0 %, 3.0 s | 2.62, 3.7 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.46 / 2.47 | 1.37, 3.4 %, 1.8 s | 1.78, 4.3 %, 2.2 s |
| P3 | residual | M1 again | payload | 100.0 | -2.08 / 2.08 | 1.86, 3.7 %, 3.6 s | 2.58, 3.5 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.02 / 2.02 | 2.94, 13.5 %, 10.8 s | 5.24, 27.5 %, 19.5 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.39 | 0.0 | -1.94 / 1.94 |
| C1 | residual M1 | payload on | 2.9 | 0.51 | 0.0 | -2.45 / 2.45 |
| S2 | residual M1 to M2 | deploy and swap | 3.2 | 0.56 | 0.0 | -2.75 / 2.75 |
| C2 | residual M2 | payload off | 3.1 | 0.55 | 0.0 | -2.67 / 2.68 |
| C3 | residual M2 | payload on | 2.8 | 0.49 | 0.0 | -2.42 / 2.43 |
| S3 | residual M2 to M1 | revert | 2.6 | 0.46 | 0.0 | -2.23 / 2.24 |
| H2 | residual M1 to assisted | set_mode assisted | 12.1 | 0.92 | 7.8 | -1.74 / 1.76 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -2.02 / 2.02 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | -0.0006 | -0.0847 |
| A2 | -0.0836 | -0.0010 | -0.0846 |
| P1 | -0.0535 | -0.0251 | -0.0819 |
| P2 | -0.0680 | -0.0088 | -0.0787 |
| N2 | -0.0674 | -0.0089 | -0.0780 |
| P3 | -0.0811 | -0.0015 | -0.0829 |
| P4 | -0.0850 | -0.0022 | -0.0851 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 11.6 | 8.1 | - |
| load: checking | 466.1 | 510.8 | - |
| load: warming | 480.1 | 519.8 | - |
| swapped (policy.active) | 660.4 | 699.9 | 661437.1 |
| first policy.wrench of the model | 680.4 | 719.9 | 661440.1 |
| first ctl.wrench carrying it | 70698.5 | 739.0 | 661459.1 |
| first thr.cmd carrying it | 70703.4 | 743.1 | 661463.2 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.24 | 0.167 | 0 | 0.015 / 0.082 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.51 | 0.166 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.29 | 0.166 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.46 | 0.293 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.23 | 0.166 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.02 / 20.37 | 0.186 | 0 | 0.018 / 0.036 |
| M1 | load | ctl.depth_hold | 43 | 19.03 / 20.53 | 0.195 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.02 / 20.49 | 0.220 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.34 | 0.252 | 0 | - |
| M1 | load | nav.state | 43 | 19.02 / 20.36 | 0.185 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.07 | 0.126 | 0 | 0.015 / 0.051 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.52 | 0.131 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.61 | 0.149 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.08 | 0.238 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.04 | 0.126 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 44 | 19.01 / 20.41 | 0.164 | 0 | 0.015 / 0.038 |
| M2 | load | ctl.depth_hold | 45 | 19.74 / 20.03 | 0.041 | 0 | - |
| M2 | load | ctl.wrench | 45 | 19.03 / 20.32 | 0.213 | 0 | - |
| M2 | load | thr.cmd | 44 | 19.06 / 20.07 | 0.255 | 0 | - |
| M2 | load | nav.state | 44 | 19.01 / 20.41 | 0.164 | 0 | - |
