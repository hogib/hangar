| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.95 / 1.95 | 1.86, 8.8 %, 6.3 s | 3.01, 19.4 %, 10.1 s |
| B2 | residual | M1 | nominal | 49.1 | 2.90 / 7.94 | 6.26, 32.8 %, - s | 3.00, 21.4 %, 10.5 s |
| B3 | residual | M1 | heavier | 0.0 | -1.93 / 1.93 | 2.73, 16.2 %, 8.7 s | 2.20, 14.4 %, 8.2 s |
| B4 | residual | M2 | heavier | 34.3 | -0.38 / 33.78 | 3.04, 30.9 %, 7.5 s | 2.18, 14.4 %, 8.0 s |
| B5 | assisted | - | heavier | 0.0 | -2.04 / 2.04 | 2.72, 15.9 %, 8.6 s | 2.21, 14.1 %, 8.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 33.0 | 1.97 | 8.2 | -2.00 / 2.01 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.54 | 10.4 | -2.07 / 2.07 |
| H1 | assisted to residual M1 | promotion | 20.8 | 1.97 | - | 0.54 / 9.97 |
| T2 | residual M1 | buoyancy -4 N | 32.8 | 1.97 | 8.2 | -1.97 / 1.98 |
| S2 | residual M1 to M2 | deploy and swap | 37.2 | 1.35 | - | -1.24 / 16.17 |
| H1b | assisted to residual M2 | promotion again, after a demotion | 47.8 | 4.16 | - | 4.32 / 32.12 |
| H2 | residual M2 to assisted | set_mode assisted | 2.2 | 0.41 | 0.0 | -1.99 / 2.00 |
| T3 | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.1 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | -0.0823 | -0.0846 |
| B2 | -0.0122 | -0.0699 | -0.0586 |
| B3 | 0.0885 | -0.0825 | 0.0885 |
| B4 | 0.1012 | -0.0557 | 0.0283 |
| B5 | 0.0886 | 0.0882 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 21.4 | 19.6 |
| load: checking | 32.8 | 27.7 |
| load: warming | 39.8 | 40.7 |
| swapped (policy.active) | 1019.5 | 1020.5 |
| first policy.wrench of the model | 1039.5 | 1040.5 |
| first ctl.wrench carrying it | 151039.2 | 11039.1 |
| first thr.cmd carrying it | 151042.4 | 11043.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.05 | 0.146 | 0 | 0.019 / 0.057 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.04 | 0.140 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.06 | 0.165 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.10 | 0.252 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.147 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 59 | 19.02 / 20.06 | 0.130 | 0 | 0.020 / 0.041 |
| M1 | load | ctl.depth_hold | 60 | 19.02 / 20.03 | 0.129 | 0 | - |
| M1 | load | ctl.wrench | 60 | 19.02 / 20.03 | 0.130 | 0 | - |
| M1 | load | thr.cmd | 60 | 19.06 / 20.07 | 0.251 | 0 | - |
| M1 | load | nav.state | 59 | 19.02 / 20.04 | 0.130 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.07 | 0.141 | 0 | 0.024 / 0.059 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.05 | 0.148 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.06 | 0.160 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.05 / 20.11 | 0.245 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.04 | 0.141 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 60 | 19.02 / 20.68 | 0.157 | 0 | 0.024 / 0.054 |
| M2 | load | ctl.depth_hold | 61 | 19.03 / 20.04 | 0.129 | 0 | - |
| M2 | load | ctl.wrench | 60 | 19.02 / 20.04 | 0.181 | 0 | - |
| M2 | load | thr.cmd | 60 | 19.06 / 20.09 | 0.252 | 0 | - |
| M2 | load | nav.state | 60 | 19.02 / 20.68 | 0.156 | 0 | - |
