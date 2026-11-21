| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.96 / 1.97 | 1.86, 9.6 %, 6.1 s | 2.99, 19.4 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -2.02 / 2.03 | 1.22, 0.9 %, 2.2 s | 1.87, 3.5 %, 2.4 s |
| B3 | residual | M1 | heavier | 100.0 | -1.07 / 1.08 | 1.34, 0.0 %, 2.8 s | 1.33, 1.3 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.92 / 1.93 | 1.48, 0.0 %, 2.7 s | 1.60, 3.4 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.03 / 2.03 | 2.69, 15.9 %, 8.6 s | 2.19, 13.9 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 33.0 | 1.98 | 8.2 | -2.02 / 2.02 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.3 | -1.95 / 1.95 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.09 / 2.09 |
| T2 | residual M1 | buoyancy -4 N | 2.9 | 0.26 | 0.0 | -0.93 / 0.94 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.31 | 0.0 | -1.65 / 1.66 |
| H2 | residual M2 to assisted | set_mode assisted | 3.4 | 0.46 | 0.0 | -2.01 / 2.01 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.54 | 9.9 | -2.03 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0032 | -0.0847 |
| B2 | -0.0874 | 0.0029 | -0.0845 |
| B3 | 0.0088 | 0.0628 | 0.0752 |
| B4 | 0.0885 | -0.0017 | 0.0870 |
| B5 | 0.0885 | -0.0018 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.1 |
| deploy called | 11.3 | 17.2 |
| load: checking | 19.1 | 25.5 |
| load: warming | 20.1 | 40.6 |
| swapped (policy.active) | 199.3 | 220.8 |
| first policy.wrench of the model | 219.9 | 239.9 |
| first ctl.wrench carrying it | 150238.4 | 258.9 |
| first thr.cmd carrying it | 150242.4 | 263.5 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.39 | 0.176 | 0 | 0.017 / 0.074 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.26 | 0.166 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.32 | 0.185 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.32 | 0.263 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.39 | 0.176 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 19.03 / 20.64 | 0.275 | 0 | 0.022 / 0.031 |
| M1 | load | ctl.depth_hold | 20 | 19.66 / 20.28 | 0.102 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.02 / 20.65 | 0.315 | 0 | - |
| M1 | load | thr.cmd | 19 | 19.08 / 20.08 | 0.313 | 0 | - |
| M1 | load | nav.state | 19 | 19.03 / 20.65 | 0.277 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.25 | 0.148 | 0 | 0.022 / 0.259 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.36 | 0.154 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.43 | 0.168 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.40 | 0.259 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.04 | 0.146 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 19.02 / 20.09 | 0.221 | 0 | 0.030 / 0.083 |
| M2 | load | ctl.depth_hold | 21 | 20.02 / 20.05 | 0.007 | 0 | - |
| M2 | load | ctl.wrench | 21 | 19.02 / 20.10 | 0.223 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.08 / 20.09 | 0.306 | 0 | - |
| M2 | load | nav.state | 21 | 19.04 / 20.09 | 0.218 | 0 | - |
