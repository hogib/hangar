| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.02 / 2.03 | 1.86, 9.6 %, 6.3 s | 3.01, 19.7 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -2.03 / 2.03 | 1.28, 1.4 %, 2.4 s | 1.84, 3.3 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -2.13 / 2.13 | 1.47, 6.1 %, 4.3 s | 2.15, 4.3 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.98 / 1.98 | 1.40, 5.8 %, 4.0 s | 2.22, 5.8 %, 10.2 s |
| B5 | assisted | - | heavier | 0.0 | -1.99 / 1.99 | 2.00, 10.3 %, 6.5 s | 3.48, 22.7 %, 11.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 6.4 | 0.97 | 4.1 | -2.01 / 2.01 |
| T1:back | assisted | buoyancy +4 N | 2.2 | 0.71 | 0.0 | -2.01 / 2.01 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.43 | 0.0 | -2.10 / 2.10 |
| T2 | residual M1 | buoyancy -4 N | 3.8 | 1.17 | 0.0 | -2.34 / 2.34 |
| S2 | residual M1 to M2 | deploy and swap | 2.7 | 0.46 | 0.0 | -2.13 / 2.14 |
| H2 | residual M2 to assisted | set_mode assisted | 2.7 | 0.42 | 0.0 | -1.97 / 1.97 |
| T3 | assisted | buoyancy +4 N | 2.2 | 0.72 | 0.0 | -2.01 / 2.02 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0846 | 0.0060 | -0.0850 |
| B2 | -0.0895 | 0.0051 | -0.0847 |
| B3 | -0.1075 | 0.0032 | -0.1048 |
| B4 | -0.1078 | 0.0035 | -0.1044 |
| B5 | -0.1028 | 0.0075 | -0.1026 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 14.0 | -2.8 |
| load: checking | 1113.5 | 989.7 |
| load: warming | 1120.1 | 999.8 |
| swapped (policy.active) | 1300.3 | 1179.7 |
| first policy.wrench of the model | 1320.5 | 1199.9 |
| first ctl.wrench carrying it | 151378.0 | 1217.5 |
| first thr.cmd carrying it | 151385.5 | 1225.6 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 15.53 / 25.50 | 1.347 | 0 | 0.025 / 3.005 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 14.03 / 25.56 | 1.178 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 14.02 / 25.84 | 1.043 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 15.07 / 24.66 | 1.340 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 16.03 / 24.43 | 1.225 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 75 | 16.02 / 23.88 | 1.002 | 0 | 0.021 / 2.890 |
| M1 | load | ctl.depth_hold | 75 | 16.02 / 23.40 | 0.866 | 0 | - |
| M1 | load | ctl.wrench | 75 | 17.03 / 23.52 | 0.789 | 0 | - |
| M1 | load | thr.cmd | 74 | 18.08 / 21.61 | 0.462 | 0 | - |
| M1 | load | nav.state | 75 | 18.03 / 21.84 | 0.756 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 12.04 / 27.54 | 1.373 | 0 | 0.034 / 2.713 |
| M2 | before (10 s) | ctl.depth_hold | 501 | 15.03 / 24.82 | 1.045 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 15.04 / 24.36 | 1.050 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 14.08 / 26.06 | 1.188 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 12.04 / 27.52 | 1.328 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 69 | 16.14 / 23.75 | 1.176 | 0 | 0.040 / 2.617 |
| M2 | load | ctl.depth_hold | 69 | 17.03 / 23.45 | 0.892 | 0 | - |
| M2 | load | ctl.wrench | 70 | 17.08 / 22.58 | 0.752 | 0 | - |
| M2 | load | thr.cmd | 69 | 17.43 / 23.05 | 0.719 | 0 | - |
| M2 | load | nav.state | 69 | 17.03 / 22.86 | 0.932 | 0 | - |
