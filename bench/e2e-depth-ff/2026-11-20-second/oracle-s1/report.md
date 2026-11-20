| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.06 / 2.06 | 1.87, 9.2 %, 6.4 s | 3.04, 19.9 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -2.01 / 2.02 | 1.23, 0.9 %, 2.3 s | 1.89, 3.6 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.12 / 1.13 | 1.35, 0.0 %, 2.8 s | 1.32, 1.3 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.90 / 1.90 | 1.45, 0.0 %, 2.7 s | 1.58, 3.2 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -1.99 / 2.00 | 2.73, 16.1 %, 8.6 s | 2.21, 13.9 %, 8.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.8 | 1.98 | 8.2 | -2.03 / 2.04 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.1 | -1.98 / 1.99 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.02 / 2.02 |
| T2 | residual M1 | buoyancy -4 N | 2.8 | 0.28 | 0.0 | -0.95 / 0.96 |
| S2 | residual M1 to M2 | deploy and swap | 1.9 | 0.31 | 0.0 | -1.65 / 1.66 |
| H2 | residual M2 to assisted | set_mode assisted | 3.3 | 0.46 | 0.0 | -1.99 / 2.00 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.54 | 10.0 | -2.00 / 2.00 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0043 | -0.0846 |
| B2 | -0.0874 | 0.0027 | -0.0846 |
| B3 | 0.0095 | 0.0602 | 0.0729 |
| B4 | 0.0885 | -0.0007 | 0.0881 |
| B5 | 0.0885 | -0.0016 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.1 |
| deploy called | 13.1 | 16.7 |
| load: checking | 25.2 | 42.5 |
| load: warming | 40.2 | 59.4 |
| swapped (policy.active) | 220.4 | 239.4 |
| first policy.wrench of the model | 240.8 | 259.4 |
| first ctl.wrench carrying it | 150278.0 | 277.8 |
| first thr.cmd carrying it | 150282.4 | 282.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.81 | 0.142 | 0 | 0.015 / 0.786 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.71 | 0.148 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.98 | 0.150 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 18.07 / 21.98 | 0.272 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.07 | 0.133 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 21 | 18.95 / 21.15 | 0.447 | 0 | 0.024 / 0.107 |
| M1 | load | ctl.depth_hold | 21 | 16.82 / 23.29 | 1.199 | 0 | - |
| M1 | load | ctl.wrench | 21 | 17.06 / 23.10 | 1.115 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.07 / 20.44 | 0.380 | 0 | - |
| M1 | load | nav.state | 21 | 19.03 / 21.14 | 0.440 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 17.04 / 22.87 | 0.294 | 0 | 0.022 / 2.859 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 18.03 / 21.77 | 0.227 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.03 / 22.26 | 0.289 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 17.07 / 22.66 | 0.362 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 18.02 / 21.55 | 0.206 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 18.02 / 21.52 | 0.794 | 0 | 0.033 / 0.121 |
| M2 | load | ctl.depth_hold | 22 | 16.23 / 23.78 | 1.343 | 0 | - |
| M2 | load | ctl.wrench | 22 | 16.30 / 23.77 | 1.348 | 0 | - |
| M2 | load | thr.cmd | 21 | 17.06 / 22.99 | 1.006 | 0 | - |
| M2 | load | nav.state | 22 | 18.03 / 21.51 | 0.769 | 0 | - |
