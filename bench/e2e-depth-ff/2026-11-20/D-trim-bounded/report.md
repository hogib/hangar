| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.94 / 1.94 | 1.85, 8.8 %, 6.3 s | 3.01, 19.7 %, 10.1 s |
| B2 | residual | M1 | nominal | 100.0 | -3.84 / 30.50 | 2.33, 23.4 %, 12.2 s | 7.20, 63.5 %, - s |
| B3 | residual | M1 | heavier | 100.0 | -9.55 / 32.66 | 2.43, 44.5 %, 7.5 s | 7.67, 59.8 %, - s |
| B4 | residual | M2 | heavier | 100.0 | -4.68 / 34.31 | 2.29, 19.8 %, 12.5 s | 7.77, 42.4 %, - s |
| B5 | assisted | - | heavier | 0.0 | -1.99 / 1.99 | 2.72, 15.9 %, 8.6 s | 2.18, 14.2 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 33.0 | 1.97 | 8.2 | -1.99 / 1.99 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.54 | 10.5 | -2.05 / 2.05 |
| H1 | assisted to residual M1 | promotion | 56.1 | 4.35 | - | 11.86 / 20.50 |
| T2 | residual M1 | buoyancy -4 N | 57.2 | 12.09 | - | -13.75 / 38.38 |
| S2 | residual M1 to M2 | deploy and swap | 49.6 | 5.51 | - | 14.70 / 29.22 |
| H2 | residual M2 to assisted | set_mode assisted | 24.3 | 1.06 | 5.6 | -1.83 / 1.84 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.53 | 10.1 | -2.01 / 2.02 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | -0.0861 | -0.0845 |
| B2 | -0.0547 | -0.0308 | -0.0650 |
| B3 | 0.0370 | -0.0618 | -0.0114 |
| B4 | 0.0307 | -0.0127 | 0.0458 |
| B5 | 0.0887 | -0.0800 | 0.0887 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 15.7 | 2.6 |
| load: checking | 25.1 | 13.3 |
| load: warming | 40.1 | 20.3 |
| swapped (policy.active) | 1020.1 | 1000.5 |
| first policy.wrench of the model | 1040.2 | 1020.5 |
| first ctl.wrench carrying it | 151039.2 | 1039.0 |
| first thr.cmd carrying it | 151043.0 | 1043.6 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.07 | 0.153 | 0 | 0.021 / 0.056 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.05 | 0.153 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.05 | 0.165 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.252 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.153 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 60 | 19.03 / 20.06 | 0.131 | 0 | 0.014 / 0.047 |
| M1 | load | ctl.depth_hold | 61 | 19.01 / 20.25 | 0.135 | 0 | - |
| M1 | load | ctl.wrench | 61 | 19.02 / 20.81 | 0.209 | 0 | - |
| M1 | load | thr.cmd | 60 | 19.06 / 20.08 | 0.251 | 0 | - |
| M1 | load | nav.state | 60 | 19.03 / 20.05 | 0.131 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.36 | 0.147 | 0 | 0.025 / 0.055 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.14 | 0.147 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.07 | 0.166 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.16 | 0.248 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.36 | 0.147 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 59 | 19.04 / 20.23 | 0.186 | 0 | 0.034 / 0.065 |
| M2 | load | ctl.depth_hold | 60 | 19.02 / 20.20 | 0.183 | 0 | - |
| M2 | load | ctl.wrench | 60 | 19.04 / 20.27 | 0.207 | 0 | - |
| M2 | load | thr.cmd | 60 | 19.06 / 20.39 | 0.285 | 0 | - |
| M2 | load | nav.state | 59 | 19.05 / 20.23 | 0.184 | 0 | - |
