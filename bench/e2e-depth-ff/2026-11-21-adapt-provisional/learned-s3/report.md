| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.00 / 2.00 | 1.85, 9.5 %, 6.2 s | 3.00, 19.7 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.06 / 2.06 | 1.23, 1.0 %, 2.3 s | 1.87, 3.5 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -2.85 / 2.85 | 1.88, 2.0 %, 3.8 s | 2.68, 4.0 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.35 / 2.36 | 1.63, 0.5 %, 3.0 s | 2.52, 3.2 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.65 / 2.65 | 1.44, 3.9 %, 1.9 s | 1.79, 4.1 %, 2.3 s |
| P3 | residual | M1 again | payload | 100.0 | -2.18 / 2.18 | 1.90, 3.6 %, 3.6 s | 2.68, 4.0 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.03 / 2.03 | 2.93, 13.6 %, 10.8 s | 5.24, 27.4 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.8 | 0.43 | 0.0 | -2.11 / 2.11 |
| C1 | residual M1 | payload on | 2.9 | 0.53 | 0.0 | -2.56 / 2.57 |
| S2 | residual M1 to M2 | deploy and swap | 3.1 | 0.52 | 0.0 | -2.45 / 2.45 |
| C2 | residual M2 | payload off | 3.4 | 0.56 | 0.0 | -2.79 / 2.79 |
| C3 | residual M2 | payload on | 2.8 | 0.50 | 0.0 | -2.42 / 2.43 |
| S3 | residual M2 to M1 | revert | 2.9 | 0.51 | 0.0 | -2.44 / 2.44 |
| H2 | residual M1 to assisted | set_mode assisted | 9.9 | 0.81 | 7.2 | -1.85 / 1.85 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.96 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0847 | 0.0050 | -0.0847 |
| A2 | -0.0904 | 0.0064 | -0.0840 |
| P1 | -0.0667 | -0.0146 | -0.0848 |
| P2 | -0.0765 | -0.0051 | -0.0831 |
| N2 | -0.0607 | -0.0194 | -0.0827 |
| P3 | -0.0883 | 0.0032 | -0.0856 |
| P4 | -0.0850 | 0.0053 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 0.2 | -1.3 | - |
| load: checking | 458.4 | 466.9 | - |
| load: warming | 460.4 | 479.9 | - |
| swapped (policy.active) | 639.7 | 660.0 | 661387.4 |
| first policy.wrench of the model | 660.0 | 679.1 | 661400.4 |
| first ctl.wrench carrying it | 70678.8 | 698.1 | 661419.4 |
| first thr.cmd carrying it | 70682.9 | 702.4 | 661422.9 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.06 | 0.165 | 0 | 0.024 / 0.044 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.05 | 0.171 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.05 | 0.153 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.13 | 0.262 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.06 | 0.165 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 41 | 19.03 / 20.32 | 0.163 | 0 | 0.020 / 0.039 |
| M1 | load | ctl.depth_hold | 42 | 19.34 / 20.15 | 0.107 | 0 | - |
| M1 | load | ctl.wrench | 42 | 19.02 / 20.45 | 0.236 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.08 | 0.261 | 0 | - |
| M1 | load | nav.state | 41 | 19.04 / 20.31 | 0.162 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.15 | 0.141 | 0 | 0.019 / 0.047 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.05 | 0.140 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 501 | 19.02 / 20.06 | 0.159 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.23 | 0.248 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.15 | 0.140 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 44 | 19.02 / 20.27 | 0.215 | 0 | 0.018 / 0.049 |
| M2 | load | ctl.depth_hold | 43 | 19.02 / 20.05 | 0.157 | 0 | - |
| M2 | load | ctl.wrench | 44 | 19.02 / 20.66 | 0.233 | 0 | - |
| M2 | load | thr.cmd | 43 | 19.06 / 20.10 | 0.257 | 0 | - |
| M2 | load | nav.state | 44 | 19.02 / 20.27 | 0.215 | 0 | - |
