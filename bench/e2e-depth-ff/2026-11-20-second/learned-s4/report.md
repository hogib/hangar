| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.05 / 2.05 | 1.84, 9.2 %, 6.3 s | 3.00, 19.8 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -2.02 / 2.03 | 1.24, 1.2 %, 2.3 s | 1.90, 3.6 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.00 / 1.02 | 1.32, 0.0 %, 2.8 s | 1.43, 1.8 %, 2.6 s |
| B4 | residual | M2 | heavier | 100.0 | -1.80 / 1.81 | 1.46, 0.0 %, 2.9 s | 1.61, 3.2 %, 2.3 s |
| B5 | assisted | - | heavier | 0.0 | -2.08 / 2.08 | 2.75, 16.3 %, 8.7 s | 2.18, 14.3 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.98 | 8.2 | -2.00 / 2.00 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.2 | -2.02 / 2.02 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.43 | 0.0 | -2.05 / 2.06 |
| T2 | residual M1 | buoyancy -4 N | 2.9 | 0.21 | 0.0 | -0.75 / 0.78 |
| S2 | residual M1 to M2 | deploy and swap | 1.9 | 0.29 | 0.0 | -1.52 / 1.53 |
| H2 | residual M2 to assisted | set_mode assisted | 2.9 | 0.43 | 0.0 | -2.02 / 2.02 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.54 | 10.2 | -2.01 / 2.01 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0043 | -0.0847 |
| B2 | -0.0886 | 0.0029 | -0.0853 |
| B3 | 0.0103 | 0.0636 | 0.0781 |
| B4 | 0.0804 | 0.0038 | 0.0850 |
| B5 | 0.0885 | 0.0004 | 0.0882 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.1 | 0.0 |
| deploy called | 14.5 | 9.4 |
| load: checking | 940.6 | 892.3 |
| load: warming | 959.8 | 896.4 |
| swapped (policy.active) | 1140.1 | 1076.6 |
| first policy.wrench of the model | 1160.1 | 1096.7 |
| first ctl.wrench carrying it | 151218.4 | 1115.1 |
| first thr.cmd carrying it | 151222.6 | 1119.7 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 14.09 / 26.43 | 1.242 | 0 | 0.022 / 2.872 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 16.15 / 24.12 | 1.008 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 16.03 / 24.14 | 1.042 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 14.07 / 25.67 | 1.144 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 15.03 / 25.49 | 1.115 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 67 | 17.10 / 22.81 | 0.645 | 0 | 0.022 / 0.099 |
| M1 | load | ctl.depth_hold | 67 | 17.04 / 24.27 | 1.018 | 0 | - |
| M1 | load | ctl.wrench | 67 | 17.03 / 24.29 | 1.033 | 0 | - |
| M1 | load | thr.cmd | 66 | 16.08 / 24.21 | 0.878 | 0 | - |
| M1 | load | nav.state | 67 | 17.03 / 22.82 | 0.650 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 11.73 / 29.43 | 1.492 | 0 | 0.027 / 5.987 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 15.03 / 24.92 | 1.089 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 15.03 / 25.12 | 1.098 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 15.07 / 25.15 | 1.203 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 14.03 / 25.36 | 1.240 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 64 | 16.92 / 23.05 | 0.813 | 0 | 0.026 / 2.992 |
| M2 | load | ctl.depth_hold | 64 | 18.03 / 21.61 | 0.566 | 0 | - |
| M2 | load | ctl.wrench | 64 | 18.03 / 21.76 | 0.517 | 0 | - |
| M2 | load | thr.cmd | 63 | 18.06 / 22.71 | 0.697 | 0 | - |
| M2 | load | nav.state | 64 | 17.03 / 22.94 | 0.796 | 0 | - |
