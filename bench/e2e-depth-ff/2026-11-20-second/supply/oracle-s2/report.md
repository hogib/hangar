| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.01 / 2.01 | 1.86, 9.1 %, 6.3 s | 3.04, 19.6 %, 10.4 s |
| B2 | residual | M1 | nominal | 100.0 | -2.06 / 2.07 | 1.20, 0.8 %, 2.2 s | 1.88, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.96 / 1.96 | 1.45, 5.9 %, 4.1 s | 2.20, 5.5 %, 6.0 s |
| B4 | residual | M2 | heavier | 100.0 | -2.05 / 2.06 | 1.43, 6.0 %, 4.2 s | 2.19, 5.6 %, 5.3 s |
| B5 | assisted | - | heavier | 0.0 | -2.02 / 2.02 | 1.97, 10.5 %, 6.4 s | 3.48, 22.7 %, 11.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 6.4 | 0.98 | 4.2 | -2.05 / 2.05 |
| T1:back | assisted | buoyancy +4 N | 2.2 | 0.72 | 0.0 | -1.98 / 1.98 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -2.07 / 2.08 |
| T2 | residual M1 | buoyancy -4 N | 4.5 | 1.12 | 0.0 | -2.08 / 2.08 |
| S2 | residual M1 to M2 | deploy and swap | 2.6 | 0.42 | 0.0 | -2.07 / 2.07 |
| H2 | residual M2 to assisted | set_mode assisted | 3.2 | 0.45 | 0.0 | -1.93 / 1.94 |
| T3 | assisted | buoyancy +4 N | 2.2 | 0.72 | 0.0 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0024 | -0.0848 |
| B2 | -0.0873 | 0.0024 | -0.0850 |
| B3 | -0.1041 | 0.0004 | -0.1036 |
| B4 | -0.1039 | -0.0003 | -0.1044 |
| B5 | -0.1027 | -0.0001 | -0.1028 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 6.4 | 19.1 |
| load: checking | 19.1 | 48.7 |
| load: warming | 39.2 | 60.8 |
| swapped (policy.active) | 219.1 | 240.7 |
| first policy.wrench of the model | 239.1 | 260.2 |
| first ctl.wrench carrying it | 150278.0 | 278.9 |
| first thr.cmd carrying it | 150284.7 | 284.7 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.02 / 22.17 | 0.212 | 0 | 0.021 / 0.581 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 18.02 / 22.81 | 0.331 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 17.02 / 22.09 | 0.292 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 18.07 / 22.26 | 0.311 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 18.03 / 22.16 | 0.208 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 19.03 / 20.92 | 0.429 | 0 | 0.021 / 0.089 |
| M1 | load | ctl.depth_hold | 21 | 18.03 / 22.03 | 0.685 | 0 | - |
| M1 | load | ctl.wrench | 21 | 18.03 / 22.53 | 0.758 | 0 | - |
| M1 | load | thr.cmd | 20 | 18.05 / 22.15 | 0.977 | 0 | - |
| M1 | load | nav.state | 20 | 19.03 / 20.93 | 0.421 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 16.96 / 22.96 | 0.735 | 0 | 0.027 / 1.361 |
| M2 | before (10 s) | ctl.depth_hold | 499 | 17.03 / 22.88 | 0.645 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 499 | 17.03 / 22.91 | 0.666 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 15.07 / 23.07 | 0.894 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 17.03 / 22.85 | 0.714 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 17.03 / 23.48 | 1.330 | 0 | 0.036 / 0.408 |
| M2 | load | ctl.depth_hold | 21 | 18.12 / 22.14 | 0.719 | 0 | - |
| M2 | load | ctl.wrench | 21 | 19.03 / 22.13 | 0.662 | 0 | - |
| M2 | load | thr.cmd | 21 | 17.08 / 22.81 | 0.961 | 0 | - |
| M2 | load | nav.state | 21 | 17.03 / 23.48 | 1.310 | 0 | - |
