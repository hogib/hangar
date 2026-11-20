| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.99 / 1.99 | 1.85, 9.0 %, 6.3 s | 3.02, 19.5 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -1.95 / 1.96 | 1.25, 1.4 %, 2.4 s | 1.83, 3.3 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.52 / 0.54 | 1.35, 0.0 %, 2.9 s | 1.34, 1.5 %, 2.6 s |
| B4 | residual | M2 | heavier | 100.0 | -1.96 / 1.96 | 1.54, 0.0 %, 2.8 s | 1.55, 2.8 %, 2.4 s |
| B5 | assisted | - | heavier | 0.0 | -1.97 / 1.97 | 2.73, 16.3 %, 8.7 s | 2.20, 14.2 %, 8.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.6 | 1.96 | 8.2 | -1.93 / 1.93 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.1 | -1.96 / 1.96 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.40 | 0.0 | -2.00 / 2.01 |
| T2 | residual M1 | buoyancy -4 N | 2.7 | 0.23 | 0.0 | -0.80 / 0.81 |
| S2 | residual M1 to M2 | deploy and swap | 2.1 | 0.35 | 0.0 | -1.82 / 1.82 |
| H2 | residual M2 to assisted | set_mode assisted | 3.2 | 0.45 | 0.0 | -2.00 / 2.00 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.53 | 10.0 | -1.99 / 1.99 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | -0.0000 | -0.0849 |
| B2 | -0.0846 | -0.0001 | -0.0849 |
| B3 | 0.0158 | 0.0658 | 0.0877 |
| B4 | 0.0865 | -0.0017 | 0.0849 |
| B5 | 0.0886 | -0.0002 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 5.5 | 2.8 |
| load: checking | 726.3 | 724.7 |
| load: warming | 739.4 | 739.7 |
| swapped (policy.active) | 919.5 | 920.4 |
| first policy.wrench of the model | 939.6 | 939.4 |
| first ctl.wrench carrying it | 150978.2 | 957.9 |
| first thr.cmd carrying it | 150983.9 | 964.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 16.87 / 23.00 | 0.267 | 0 | 0.018 / 1.026 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 17.02 / 22.54 | 0.284 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 18.02 / 21.75 | 0.234 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 17.07 / 22.68 | 0.299 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 17.04 / 22.85 | 0.249 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 55 | 19.02 / 20.30 | 0.168 | 0 | 0.020 / 0.121 |
| M1 | load | ctl.depth_hold | 56 | 19.02 / 20.89 | 0.217 | 0 | - |
| M1 | load | ctl.wrench | 56 | 19.45 / 20.25 | 0.085 | 0 | - |
| M1 | load | thr.cmd | 55 | 19.06 / 20.08 | 0.262 | 0 | - |
| M1 | load | nav.state | 55 | 19.02 / 20.28 | 0.164 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 17.24 / 22.82 | 0.276 | 0 | 0.022 / 2.809 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 18.02 / 22.19 | 0.261 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 18.03 / 22.19 | 0.314 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 18.07 / 22.66 | 0.339 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 21.29 | 0.206 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 56 | 18.30 / 21.76 | 0.403 | 0 | 0.021 / 1.757 |
| M2 | load | ctl.depth_hold | 56 | 19.60 / 20.04 | 0.058 | 0 | - |
| M2 | load | ctl.wrench | 56 | 19.02 / 21.32 | 0.275 | 0 | - |
| M2 | load | thr.cmd | 56 | 19.06 / 20.10 | 0.259 | 0 | - |
| M2 | load | nav.state | 56 | 19.02 / 20.56 | 0.232 | 0 | - |
