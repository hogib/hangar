| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.03 / 2.03 | 1.87, 9.3 %, 6.3 s | 3.04, 19.8 %, 10.4 s |
| B2 | residual | M1 | nominal | 100.0 | -2.05 / 2.06 | 1.21, 0.7 %, 2.2 s | 1.91, 3.7 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.10 / 1.11 | 1.35, 0.0 %, 2.7 s | 1.35, 1.6 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.94 / 1.95 | 1.48, 0.0 %, 2.7 s | 1.59, 3.2 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.03 / 2.03 | 2.72, 16.3 %, 8.7 s | 2.20, 14.2 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.98 | 8.2 | -2.05 / 2.05 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.52 | 10.1 | -1.98 / 1.99 |
| H1 | assisted to residual M1 | promotion | 2.6 | 0.43 | 0.0 | -2.10 / 2.11 |
| T2 | residual M1 | buoyancy -4 N | 2.6 | 0.26 | 0.0 | -0.89 / 0.90 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.32 | 0.0 | -1.77 / 1.77 |
| H2 | residual M2 to assisted | set_mode assisted | 3.6 | 0.46 | 0.0 | -2.03 / 2.03 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.53 | 10.1 | -2.00 / 2.00 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0846 | 0.0021 | -0.0847 |
| B2 | -0.0874 | 0.0017 | -0.0859 |
| B3 | 0.0089 | 0.0631 | 0.0754 |
| B4 | 0.0882 | 0.0001 | 0.0888 |
| B5 | 0.0886 | -0.0007 | 0.0887 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 15.4 | 13.7 |
| load: checking | 27.6 | 27.2 |
| load: warming | 39.7 | 40.1 |
| swapped (policy.active) | 219.9 | 220.4 |
| first policy.wrench of the model | 239.9 | 240.4 |
| first ctl.wrench carrying it | 150279.0 | 259.4 |
| first thr.cmd carrying it | 150285.3 | 266.5 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 17.01 / 22.52 | 0.216 | 0 | 0.011 / 0.057 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 21.03 | 0.134 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 21.00 | 0.140 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 13.07 / 26.35 | 0.483 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 17.03 / 22.49 | 0.214 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 19.96 / 20.08 | 0.021 | 0 | 0.024 / 0.044 |
| M1 | load | ctl.depth_hold | 21 | 20.00 / 20.08 | 0.014 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.88 / 20.06 | 0.035 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.07 / 20.10 | 0.224 | 0 | - |
| M1 | load | nav.state | 20 | 19.99 / 20.08 | 0.016 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 17.95 / 22.95 | 0.440 | 0 | 0.018 / 1.054 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 17.02 / 22.62 | 0.364 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.02 / 22.83 | 0.458 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 15.07 / 24.98 | 0.508 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 18.02 / 22.80 | 0.430 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 19.01 / 20.61 | 0.261 | 0 | 0.024 / 0.040 |
| M2 | load | ctl.depth_hold | 21 | 20.01 / 20.15 | 0.034 | 0 | - |
| M2 | load | ctl.wrench | 21 | 20.02 / 20.16 | 0.030 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.06 / 20.09 | 0.225 | 0 | - |
| M2 | load | nav.state | 21 | 19.02 / 20.59 | 0.257 | 0 | - |
