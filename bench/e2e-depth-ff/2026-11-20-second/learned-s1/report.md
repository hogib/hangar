| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.97 / 1.98 | 1.88, 9.5 %, 6.3 s | 3.04, 19.9 %, 10.4 s |
| B2 | residual | M1 | nominal | 100.0 | -1.98 / 1.98 | 1.25, 0.9 %, 2.3 s | 1.89, 3.6 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.87 / 0.88 | 1.34, 0.0 %, 2.6 s | 1.40, 1.6 %, 2.6 s |
| B4 | residual | M2 | heavier | 100.0 | -1.87 / 1.88 | 1.49, 0.0 %, 2.9 s | 1.62, 3.3 %, 2.3 s |
| B5 | assisted | - | heavier | 0.0 | -1.97 / 1.98 | 2.76, 16.2 %, 8.7 s | 2.18, 13.8 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.6 | 1.98 | 8.3 | -2.07 / 2.07 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.2 | -2.00 / 2.00 |
| H1 | assisted to residual M1 | promotion | 2.3 | 0.41 | 0.0 | -2.00 / 2.01 |
| T2 | residual M1 | buoyancy -4 N | 2.9 | 0.25 | 0.0 | -0.92 / 0.93 |
| S2 | residual M1 to M2 | deploy and swap | 1.8 | 0.28 | 0.0 | -1.51 / 1.51 |
| H2 | residual M2 to assisted | set_mode assisted | 4.0 | 0.48 | 0.0 | -1.96 / 1.96 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.55 | 10.1 | -2.02 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0031 | -0.0847 |
| B2 | -0.0863 | 0.0019 | -0.0845 |
| B3 | 0.0109 | 0.0691 | 0.0849 |
| B4 | 0.0902 | -0.0036 | 0.0870 |
| B5 | 0.0886 | -0.0060 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.1 |
| deploy called | 18.9 | 9.3 |
| load: checking | 885.2 | 908.5 |
| load: warming | 900.2 | 918.6 |
| swapped (policy.active) | 1079.5 | 1099.0 |
| first policy.wrench of the model | 1100.1 | 1119.1 |
| first ctl.wrench carrying it | 151149.5 | 1125.8 |
| first thr.cmd carrying it | 151166.1 | 1144.9 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 15.03 / 24.50 | 1.133 | 0 | 0.022 / 2.790 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 16.16 / 24.65 | 1.005 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 15.03 / 25.21 | 1.111 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 14.07 / 25.59 | 1.100 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 15.03 / 24.47 | 1.082 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 63 | 18.00 / 26.05 | 1.168 | 0 | 0.022 / 2.985 |
| M1 | load | ctl.depth_hold | 63 | 15.03 / 24.27 | 1.083 | 0 | - |
| M1 | load | ctl.wrench | 63 | 18.03 / 22.16 | 0.437 | 0 | - |
| M1 | load | thr.cmd | 63 | 17.07 / 22.59 | 0.594 | 0 | - |
| M1 | load | nav.state | 63 | 18.04 / 23.09 | 0.842 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 13.28 / 27.43 | 1.143 | 0 | 0.027 / 3.924 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 13.03 / 27.24 | 1.257 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 14.03 / 25.13 | 1.246 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 15.07 / 25.05 | 1.163 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 15.03 / 25.01 | 1.031 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 65 | 17.27 / 23.94 | 1.024 | 0 | 0.026 / 2.521 |
| M2 | load | ctl.depth_hold | 64 | 17.30 / 22.95 | 0.733 | 0 | - |
| M2 | load | ctl.wrench | 64 | 16.79 / 23.25 | 0.795 | 0 | - |
| M2 | load | thr.cmd | 64 | 17.08 / 22.65 | 0.644 | 0 | - |
| M2 | load | nav.state | 65 | 17.29 / 23.92 | 0.836 | 0 | - |
