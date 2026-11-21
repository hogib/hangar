| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.06 / 2.06 | 1.87, 9.2 %, 6.3 s | 3.01, 19.8 %, 10.1 s |
| A2 | residual | M1 | bare | 100.0 | -1.95 / 1.96 | 1.20, 0.7 %, 2.2 s | 1.85, 3.5 %, 2.4 s |
| P1 | residual | M1 | payload | 100.0 | -2.77 / 2.78 | 1.86, 2.1 %, 3.8 s | 2.68, 4.0 %, 4.1 s |
| P2 | residual | M2 | payload | 100.0 | -2.36 / 2.36 | 1.63, 0.7 %, 3.0 s | 2.57, 3.5 %, 4.0 s |
| N2 | residual | M2 | bare | 100.0 | -2.56 / 2.57 | 1.43, 8.1 %, 2.8 s | 1.83, 4.2 %, 2.3 s |
| P3 | residual | M1 again | payload | 100.0 | -2.11 / 2.12 | 1.86, 3.0 %, 3.6 s | 2.69, 3.9 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -1.99 / 1.99 | 2.94, 13.6 %, 10.8 s | 5.25, 27.5 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.40 | 0.0 | -2.01 / 2.02 |
| C1 | residual M1 | payload on | 2.9 | 0.50 | 0.0 | -2.42 / 2.42 |
| S2 | residual M1 to M2 | deploy and swap | 3.3 | 0.54 | 0.0 | -2.62 / 2.63 |
| C2 | residual M2 | payload off | 3.4 | 0.57 | 0.0 | -2.80 / 2.81 |
| C3 | residual M2 | payload on | 2.9 | 0.50 | 0.0 | -2.43 / 2.44 |
| S3 | residual M2 to M1 | revert | 2.7 | 0.46 | 0.0 | -2.21 / 2.21 |
| H2 | residual M1 to assisted | set_mode assisted | 10.3 | 0.82 | 7.1 | -1.84 / 1.85 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.99 / 1.99 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0847 | -0.0018 | -0.0848 |
| A2 | -0.0843 | -0.0012 | -0.0853 |
| P1 | -0.0649 | -0.0159 | -0.0840 |
| P2 | -0.0766 | -0.0047 | -0.0828 |
| N2 | -0.0674 | -0.0137 | -0.0833 |
| P3 | -0.0829 | -0.0022 | -0.0855 |
| P4 | -0.0849 | -0.0014 | -0.0851 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 5.9 | 7.9 | - |
| load: checking | 485.8 | 470.7 | - |
| load: warming | 500.9 | 479.7 | - |
| swapped (policy.active) | 680.1 | 660.0 | 661425.5 |
| first policy.wrench of the model | 700.1 | 680.0 | 661440.6 |
| first ctl.wrench carrying it | 70719.0 | 698.0 | 661459.6 |
| first thr.cmd carrying it | 70722.7 | 701.9 | 661462.8 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.95 / 20.24 | 0.167 | 0 | 0.027 / 0.169 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.15 | 0.171 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.03 / 20.07 | 0.186 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.05 / 20.11 | 0.259 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.12 | 0.165 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 44 | 19.02 / 20.76 | 0.273 | 0 | 0.025 / 0.039 |
| M1 | load | ctl.depth_hold | 44 | 19.02 / 20.77 | 0.262 | 0 | - |
| M1 | load | ctl.wrench | 44 | 19.04 / 20.07 | 0.162 | 0 | - |
| M1 | load | thr.cmd | 43 | 19.07 / 20.09 | 0.258 | 0 | - |
| M1 | load | nav.state | 44 | 19.03 / 20.76 | 0.272 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.05 | 0.134 | 0 | 0.013 / 0.048 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.04 | 0.147 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.47 | 0.148 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.248 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.134 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 43 | 19.03 / 20.36 | 0.161 | 0 | 0.026 / 0.041 |
| M2 | load | ctl.depth_hold | 43 | 20.01 / 20.11 | 0.016 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.05 / 20.04 | 0.148 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.07 / 20.08 | 0.259 | 0 | - |
| M2 | load | nav.state | 43 | 19.02 / 20.35 | 0.162 | 0 | - |
