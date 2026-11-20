| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.98 / 1.98 | 1.86, 9.1 %, 6.3 s | 3.05, 19.7 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -2.04 / 2.05 | 1.22, 1.2 %, 2.3 s | 1.90, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.03 / 1.04 | 1.32, 0.0 %, 2.6 s | 1.35, 1.4 %, 2.6 s |
| B4 | residual | M2 | heavier | 100.0 | -1.90 / 1.91 | 1.51, 0.0 %, 2.8 s | 1.60, 3.3 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.02 / 2.02 | 2.72, 16.1 %, 8.6 s | 2.19, 14.6 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.97 | 8.3 | -1.99 / 1.99 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.1 | -2.03 / 2.03 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.41 | 0.0 | -2.07 / 2.07 |
| T2 | residual M1 | buoyancy -4 N | 2.9 | 0.24 | 0.0 | -0.79 / 0.81 |
| S2 | residual M1 to M2 | deploy and swap | 2.1 | 0.34 | 0.0 | -1.77 / 1.78 |
| H2 | residual M2 to assisted | set_mode assisted | 3.2 | 0.45 | 0.0 | -1.96 / 1.96 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.54 | 10.2 | -2.00 / 2.01 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0846 | 0.0014 | -0.0848 |
| B2 | -0.0873 | 0.0025 | -0.0847 |
| B3 | 0.0128 | 0.0628 | 0.0797 |
| B4 | 0.0882 | -0.0006 | 0.0879 |
| B5 | 0.0886 | -0.0013 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 6.3 | 11.9 |
| load: checking | 25.6 | 35.4 |
| load: warming | 39.6 | 36.4 |
| swapped (policy.active) | 219.8 | 216.6 |
| first policy.wrench of the model | 240.2 | 237.3 |
| first ctl.wrench carrying it | 150277.3 | 254.5 |
| first thr.cmd carrying it | 150288.7 | 265.3 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 17.45 / 22.59 | 0.216 | 0 | 0.015 / 2.590 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 21.65 | 0.159 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 18.05 / 21.20 | 0.169 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.51 | 0.246 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.43 | 0.141 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 21 | 19.02 / 20.47 | 0.260 | 0 | 0.025 / 0.044 |
| M1 | load | ctl.depth_hold | 21 | 19.02 / 20.45 | 0.299 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.12 / 20.49 | 0.288 | 0 | - |
| M1 | load | thr.cmd | 21 | 18.07 / 21.99 | 0.694 | 0 | - |
| M1 | load | nav.state | 21 | 19.03 / 20.45 | 0.255 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 17.68 / 22.38 | 0.339 | 0 | 0.021 / 2.374 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 17.02 / 22.76 | 0.331 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.04 / 22.55 | 0.293 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 18.07 / 22.12 | 0.333 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 18.02 / 22.07 | 0.249 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 17.01 / 22.46 | 0.975 | 0 | 0.037 / 0.164 |
| M2 | load | ctl.depth_hold | 21 | 17.03 / 21.81 | 0.886 | 0 | - |
| M2 | load | ctl.wrench | 21 | 17.05 / 22.24 | 0.969 | 0 | - |
| M2 | load | thr.cmd | 20 | 18.07 / 21.51 | 0.828 | 0 | - |
| M2 | load | nav.state | 21 | 17.04 / 22.46 | 0.954 | 0 | - |
