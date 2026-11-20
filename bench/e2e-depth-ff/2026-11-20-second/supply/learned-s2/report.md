| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.00 / 2.01 | 1.85, 8.9 %, 6.4 s | 3.03, 19.9 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -2.03 / 2.03 | 1.21, 1.2 %, 2.2 s | 1.88, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -2.03 / 2.03 | 1.43, 6.0 %, 4.1 s | 2.21, 5.3 %, 5.0 s |
| B4 | residual | M2 | heavier | 100.0 | -2.06 / 2.06 | 1.46, 5.9 %, 4.1 s | 2.23, 5.3 %, 8.2 s |
| B5 | assisted | - | heavier | 0.0 | -1.99 / 1.99 | 1.98, 10.6 %, 6.6 s | 3.47, 22.4 %, 11.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 6.5 | 0.98 | 4.3 | -2.02 / 2.02 |
| T1:back | assisted | buoyancy +4 N | 2.2 | 0.72 | 0.0 | -2.02 / 2.03 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -1.99 / 2.00 |
| T2 | residual M1 | buoyancy -4 N | 4.1 | 1.12 | 0.0 | -2.19 / 2.20 |
| S2 | residual M1 to M2 | deploy and swap | 2.6 | 0.45 | 0.0 | -2.15 / 2.16 |
| H2 | residual M2 to assisted | set_mode assisted | 3.1 | 0.45 | 0.0 | -2.08 / 2.09 |
| T3 | assisted | buoyancy +4 N | 2.2 | 0.71 | 0.0 | -2.01 / 2.01 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0029 | -0.0848 |
| B2 | -0.0877 | 0.0021 | -0.0857 |
| B3 | -0.1048 | 0.0005 | -0.1046 |
| B4 | -0.1075 | 0.0039 | -0.1037 |
| B5 | -0.1027 | 0.0045 | -0.1025 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.1 |
| deploy called | 2.5 | -4.5 |
| load: checking | 997.6 | 1029.0 |
| load: warming | 998.6 | 1040.0 |
| swapped (policy.active) | 1179.1 | 1220.2 |
| first policy.wrench of the model | 1198.5 | 1242.3 |
| first ctl.wrench carrying it | 151274.1 | 1255.8 |
| first thr.cmd carrying it | 151281.8 | 1262.5 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 11.06 / 29.28 | 1.690 | 0 | 0.024 / 2.869 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 11.03 / 27.84 | 1.414 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 16.03 / 23.43 | 0.919 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 12.05 / 27.36 | 1.341 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 11.03 / 29.28 | 1.635 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 69 | 17.03 / 22.48 | 0.877 | 0 | 0.024 / 0.164 |
| M1 | load | ctl.depth_hold | 69 | 17.03 / 22.71 | 0.985 | 0 | - |
| M1 | load | ctl.wrench | 69 | 17.04 / 23.52 | 0.719 | 0 | - |
| M1 | load | thr.cmd | 68 | 18.08 / 21.64 | 0.475 | 0 | - |
| M1 | load | nav.state | 69 | 17.03 / 22.47 | 0.868 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 12.92 / 26.36 | 1.513 | 0 | 0.037 / 4.183 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 15.04 / 24.48 | 1.116 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 16.03 / 24.37 | 0.919 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 16.06 / 24.37 | 1.011 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 15.04 / 25.68 | 1.412 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 71 | 15.06 / 24.13 | 1.331 | 0 | 0.066 / 0.264 |
| M2 | load | ctl.depth_hold | 71 | 16.61 / 23.84 | 1.022 | 0 | - |
| M2 | load | ctl.wrench | 72 | 17.05 / 22.65 | 0.577 | 0 | - |
| M2 | load | thr.cmd | 71 | 17.08 / 22.40 | 0.614 | 0 | - |
| M2 | load | nav.state | 71 | 15.06 / 24.13 | 1.318 | 0 | - |
