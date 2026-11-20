| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.97 / 1.97 | 1.86, 9.4 %, 6.3 s | 3.04, 19.9 %, 10.4 s |
| B2 | residual | M1 | nominal | 100.0 | -2.03 / 2.04 | 1.26, 1.7 %, 2.3 s | 1.88, 3.4 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.93 / 0.94 | 1.29, 0.0 %, 2.6 s | 1.34, 1.4 %, 2.5 s |
| B4 | residual | M2 | heavier | 100.0 | -1.97 / 1.98 | 1.51, 0.0 %, 2.8 s | 1.61, 3.5 %, 2.3 s |
| B5 | assisted | - | heavier | 0.0 | -1.95 / 1.96 | 2.75, 16.4 %, 8.7 s | 2.20, 13.7 %, 8.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.7 | 1.98 | 8.3 | -2.06 / 2.06 |
| T1:back | assisted | buoyancy +4 N | 36.2 | 2.53 | 10.1 | -1.98 / 1.98 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.45 | 0.0 | -2.16 / 2.17 |
| T2 | residual M1 | buoyancy -4 N | 2.9 | 0.21 | 0.0 | -0.69 / 0.71 |
| S2 | residual M1 to M2 | deploy and swap | 2.1 | 0.35 | 0.0 | -1.85 / 1.86 |
| H2 | residual M2 to assisted | set_mode assisted | 2.8 | 0.43 | 0.0 | -1.97 / 1.98 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.54 | 10.1 | -2.03 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0090 | -0.0847 |
| B2 | -0.0920 | 0.0072 | -0.0850 |
| B3 | 0.0127 | 0.0649 | 0.0822 |
| B4 | 0.0892 | -0.0045 | 0.0846 |
| B5 | 0.0885 | -0.0036 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 11.0 | 17.2 |
| load: checking | 750.4 | 752.5 |
| load: warming | 760.3 | 759.4 |
| swapped (policy.active) | 940.5 | 939.7 |
| first policy.wrench of the model | 960.5 | 959.8 |
| first ctl.wrench carrying it | 151015.4 | 974.6 |
| first thr.cmd carrying it | 151031.9 | 991.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 16.97 / 22.79 | 0.366 | 0 | 0.022 / 2.779 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 21.07 | 0.189 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 17.02 / 22.58 | 0.342 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 17.08 / 22.67 | 0.405 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 17.03 / 22.63 | 0.264 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 57 | 19.06 / 20.25 | 0.140 | 0 | 0.027 / 0.059 |
| M1 | load | ctl.depth_hold | 57 | 19.02 / 20.55 | 0.184 | 0 | - |
| M1 | load | ctl.wrench | 57 | 19.02 / 20.65 | 0.210 | 0 | - |
| M1 | load | thr.cmd | 57 | 19.06 / 20.10 | 0.259 | 0 | - |
| M1 | load | nav.state | 57 | 19.03 / 20.26 | 0.143 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 15.81 / 25.00 | 0.386 | 0 | 0.025 / 1.194 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 17.03 / 23.16 | 0.274 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.03 / 22.64 | 0.350 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 18.06 / 22.31 | 0.323 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 16.24 / 24.52 | 0.347 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 57 | 17.78 / 22.86 | 0.580 | 0 | 0.020 / 0.275 |
| M2 | load | ctl.depth_hold | 56 | 19.02 / 20.18 | 0.191 | 0 | - |
| M2 | load | ctl.wrench | 56 | 19.02 / 20.03 | 0.150 | 0 | - |
| M2 | load | thr.cmd | 56 | 19.06 / 20.08 | 0.227 | 0 | - |
| M2 | load | nav.state | 57 | 18.03 / 22.62 | 0.541 | 0 | - |
