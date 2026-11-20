| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.03 / 2.03 | 1.85, 9.1 %, 6.4 s | 3.02, 19.8 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -1.97 / 1.98 | 1.26, 1.1 %, 2.4 s | 1.92, 3.6 %, 2.7 s |
| B3 | residual | M1 | heavier | 100.0 | -2.01 / 2.01 | 1.44, 5.9 %, 4.2 s | 2.23, 4.8 %, 2.8 s |
| B4 | residual | M2 | heavier | 100.0 | -2.00 / 2.00 | 1.41, 5.6 %, 4.1 s | 2.26, 5.2 %, 6.9 s |
| B5 | assisted | - | heavier | 0.0 | -1.99 / 1.99 | 1.98, 10.3 %, 6.5 s | 3.46, 22.7 %, 11.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 6.5 | 0.97 | 4.3 | -2.00 / 2.00 |
| T1:back | assisted | buoyancy +4 N | 2.2 | 0.72 | 0.0 | -1.98 / 1.98 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.41 | 0.0 | -2.06 / 2.07 |
| T2 | residual M1 | buoyancy -4 N | 4.2 | 1.14 | 0.0 | -2.28 / 2.28 |
| S2 | residual M1 to M2 | deploy and swap | 3.1 | 0.46 | 0.0 | -2.08 / 2.08 |
| H2 | residual M2 to assisted | set_mode assisted | 2.7 | 0.42 | 0.0 | -1.96 / 1.96 |
| T3 | assisted | buoyancy +4 N | 2.3 | 0.73 | 0.0 | -2.02 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | -0.0015 | -0.0850 |
| B2 | -0.0853 | 0.0012 | -0.0841 |
| B3 | -0.1039 | 0.0000 | -0.1039 |
| B4 | -0.1059 | 0.0020 | -0.1037 |
| B5 | -0.1027 | 0.0015 | -0.1028 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.1 |
| deploy called | 0.1 | 5.0 |
| load: checking | 1058.3 | 970.5 |
| load: warming | 1075.3 | 979.5 |
| swapped (policy.active) | 1257.0 | 1160.0 |
| first policy.wrench of the model | 1275.0 | 1180.0 |
| first ctl.wrench carrying it | 151353.9 | 1197.7 |
| first thr.cmd carrying it | 151368.7 | 1214.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 14.04 / 28.87 | 1.536 | 0 | 0.024 / 3.158 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 13.18 / 26.85 | 1.314 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 14.03 / 26.63 | 1.226 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 15.07 / 24.36 | 1.300 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 14.03 / 25.79 | 1.441 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 73 | 17.02 / 22.90 | 1.263 | 0 | 0.023 / 0.530 |
| M1 | load | ctl.depth_hold | 73 | 17.02 / 22.70 | 0.808 | 0 | - |
| M1 | load | ctl.wrench | 73 | 15.03 / 23.25 | 1.031 | 0 | - |
| M1 | load | thr.cmd | 73 | 18.07 / 22.06 | 0.585 | 0 | - |
| M1 | load | nav.state | 73 | 17.04 / 22.88 | 1.251 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 14.17 / 25.58 | 1.477 | 0 | 0.038 / 2.917 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 15.03 / 24.97 | 1.102 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 14.04 / 25.78 | 1.014 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 14.08 / 25.11 | 1.075 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 15.04 / 25.20 | 1.409 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 67 | 17.82 / 23.64 | 0.928 | 0 | 0.049 / 0.975 |
| M2 | load | ctl.depth_hold | 68 | 15.04 / 23.61 | 0.939 | 0 | - |
| M2 | load | ctl.wrench | 68 | 16.05 / 22.43 | 0.792 | 0 | - |
| M2 | load | thr.cmd | 68 | 15.07 / 25.63 | 1.507 | 0 | - |
| M2 | load | nav.state | 67 | 18.04 / 22.72 | 0.832 | 0 | - |
