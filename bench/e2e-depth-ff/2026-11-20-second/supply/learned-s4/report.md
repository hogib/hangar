| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.02 / 2.02 | 1.87, 9.5 %, 6.3 s | 3.04, 19.9 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -2.00 / 2.01 | 1.29, 1.3 %, 2.4 s | 1.88, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -2.05 / 2.06 | 1.48, 5.1 %, 3.9 s | 2.19, 4.9 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -2.04 / 2.05 | 1.37, 5.3 %, 3.9 s | 2.17, 4.5 %, 2.8 s |
| B5 | assisted | - | heavier | 0.0 | -2.06 / 2.06 | 1.98, 10.5 %, 6.5 s | 3.49, 22.5 %, 11.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 6.4 | 0.98 | 4.2 | -2.04 / 2.04 |
| T1:back | assisted | buoyancy +4 N | 2.3 | 0.72 | 0.0 | -2.01 / 2.01 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.39 | 0.0 | -1.95 / 1.95 |
| T2 | residual M1 | buoyancy -4 N | 4.2 | 1.13 | 0.0 | -2.21 / 2.22 |
| S2 | residual M1 to M2 | deploy and swap | 2.6 | 0.45 | 0.0 | -2.17 / 2.18 |
| H2 | residual M2 to assisted | set_mode assisted | 5.1 | 0.53 | 2.6 | -2.02 / 2.02 |
| T3 | assisted | buoyancy +4 N | 2.2 | 0.72 | 0.0 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | -0.0029 | -0.0848 |
| B2 | -0.0832 | -0.0019 | -0.0850 |
| B3 | -0.1015 | -0.0021 | -0.1035 |
| B4 | -0.1027 | 0.0001 | -0.1026 |
| B5 | -0.1029 | -0.0005 | -0.1027 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 6.5 | -8.7 |
| load: checking | 833.9 | 997.0 |
| load: warming | 840.1 | 1000.2 |
| swapped (policy.active) | 1020.7 | 1180.3 |
| first policy.wrench of the model | 1040.6 | 1199.4 |
| first ctl.wrench carrying it | 151098.8 | 1217.5 |
| first thr.cmd carrying it | 151109.1 | 1228.5 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 14.94 / 26.22 | 1.473 | 0 | 0.022 / 4.542 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 16.03 / 24.70 | 1.128 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 14.03 / 25.82 | 1.112 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 15.08 / 24.97 | 0.925 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 15.03 / 24.86 | 1.410 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 61 | 17.02 / 23.60 | 1.055 | 0 | 0.022 / 2.861 |
| M1 | load | ctl.depth_hold | 61 | 16.03 / 24.58 | 1.042 | 0 | - |
| M1 | load | ctl.wrench | 61 | 16.03 / 22.70 | 0.996 | 0 | - |
| M1 | load | thr.cmd | 61 | 18.07 / 22.14 | 0.738 | 0 | - |
| M1 | load | nav.state | 61 | 17.03 / 23.59 | 1.061 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 15.01 / 25.52 | 1.319 | 0 | 0.037 / 2.882 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 15.03 / 25.02 | 1.070 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 16.04 / 24.41 | 1.111 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 16.07 / 24.46 | 1.098 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 15.03 / 25.48 | 1.287 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 70 | 15.02 / 23.51 | 1.316 | 0 | 0.042 / 0.091 |
| M2 | load | ctl.depth_hold | 70 | 17.04 / 22.37 | 0.842 | 0 | - |
| M2 | load | ctl.wrench | 70 | 17.54 / 22.84 | 0.624 | 0 | - |
| M2 | load | thr.cmd | 69 | 17.08 / 22.72 | 0.900 | 0 | - |
| M2 | load | nav.state | 70 | 15.06 / 23.52 | 1.308 | 0 | - |
