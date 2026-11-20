| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.99 / 2.00 | 1.85, 9.3 %, 6.2 s | 3.00, 19.6 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -2.01 / 2.02 | 1.24, 1.0 %, 2.4 s | 1.86, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.84 / 0.86 | 1.32, 0.0 %, 2.8 s | 1.40, 1.8 %, 2.5 s |
| B4 | residual | M2 | heavier | 100.0 | -1.92 / 1.93 | 1.43, 0.0 %, 2.9 s | 1.63, 3.7 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.00 / 2.00 | 2.70, 16.0 %, 8.5 s | 2.19, 14.1 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.8 | 1.97 | 8.2 | -1.98 / 1.98 |
| T1:back | assisted | buoyancy +4 N | 36.7 | 2.53 | 10.2 | -2.05 / 2.05 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.41 | 0.0 | -2.00 / 2.00 |
| T2 | residual M1 | buoyancy -4 N | 2.5 | 0.20 | 0.0 | -0.73 / 0.74 |
| S2 | residual M1 to M2 | deploy and swap | 2.1 | 0.31 | 0.0 | -1.68 / 1.68 |
| H2 | residual M2 to assisted | set_mode assisted | 3.0 | 0.45 | 0.0 | -2.04 / 2.04 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.54 | 10.1 | -2.02 / 2.02 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0849 | 0.0035 | -0.0847 |
| B2 | -0.0868 | 0.0024 | -0.0846 |
| B3 | 0.0150 | 0.0599 | 0.0799 |
| B4 | 0.0879 | -0.0002 | 0.0879 |
| B5 | 0.0886 | 0.0004 | 0.0887 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.1 | 0.0 |
| deploy called | 12.5 | 7.6 |
| load: checking | 954.2 | 890.0 |
| load: warming | 960.3 | 900.1 |
| swapped (policy.active) | 1139.8 | 1079.9 |
| first policy.wrench of the model | 1159.9 | 1099.9 |
| first ctl.wrench carrying it | 151218.4 | 1119.5 |
| first thr.cmd carrying it | 151232.8 | 1133.3 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 16.02 / 24.36 | 1.200 | 0 | 0.022 / 4.369 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 16.61 / 24.33 | 1.097 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 15.03 / 24.99 | 1.162 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 15.08 / 23.72 | 1.198 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 16.03 / 23.90 | 1.113 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 66 | 14.67 / 25.32 | 1.249 | 0 | 0.022 / 2.401 |
| M1 | load | ctl.depth_hold | 67 | 17.04 / 23.18 | 0.705 | 0 | - |
| M1 | load | ctl.wrench | 66 | 18.05 / 22.16 | 0.425 | 0 | - |
| M1 | load | thr.cmd | 67 | 19.07 / 20.78 | 0.342 | 0 | - |
| M1 | load | nav.state | 66 | 17.03 / 22.95 | 0.964 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 10.92 / 28.88 | 1.564 | 0 | 0.027 / 6.158 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 15.03 / 25.43 | 1.031 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 14.03 / 24.52 | 1.144 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 15.07 / 24.57 | 1.131 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 14.03 / 26.00 | 1.359 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 63 | 17.93 / 22.34 | 0.734 | 0 | 0.027 / 1.233 |
| M2 | load | ctl.depth_hold | 64 | 17.03 / 22.19 | 0.661 | 0 | - |
| M2 | load | ctl.wrench | 64 | 13.03 / 26.35 | 1.372 | 0 | - |
| M2 | load | thr.cmd | 64 | 16.08 / 23.81 | 0.860 | 0 | - |
| M2 | load | nav.state | 63 | 18.04 / 22.23 | 0.605 | 0 | - |
