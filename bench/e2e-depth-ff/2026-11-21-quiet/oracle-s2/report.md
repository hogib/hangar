| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.00 / 2.00 | 1.85, 9.2 %, 6.3 s | 3.01, 19.4 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -2.03 / 2.04 | 1.21, 0.6 %, 2.3 s | 1.89, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.02 / 1.03 | 1.32, 0.0 %, 2.6 s | 1.33, 1.5 %, 2.6 s |
| B4 | residual | M2 | heavier | 100.0 | -1.97 / 1.98 | 1.49, 0.0 %, 2.8 s | 1.56, 3.1 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.04 / 2.04 | 2.70, 15.8 %, 8.5 s | 2.17, 14.2 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.97 | 8.2 | -2.06 / 2.06 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.1 | -2.00 / 2.00 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -2.06 / 2.06 |
| T2 | residual M1 | buoyancy -4 N | 2.8 | 0.24 | 0.0 | -0.77 / 0.79 |
| S2 | residual M1 to M2 | deploy and swap | 2.1 | 0.34 | 0.0 | -1.75 / 1.76 |
| H2 | residual M2 to assisted | set_mode assisted | 3.4 | 0.46 | 0.0 | -1.96 / 1.97 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.54 | 10.2 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0026 | -0.0848 |
| B2 | -0.0871 | 0.0021 | -0.0850 |
| B3 | 0.0129 | 0.0600 | 0.0768 |
| B4 | 0.0881 | -0.0005 | 0.0878 |
| B5 | 0.0886 | -0.0010 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 4.3 | 0.6 |
| load: checking | 11.8 | 8.5 |
| load: warming | 19.8 | 20.5 |
| swapped (policy.active) | 200.0 | 200.7 |
| first policy.wrench of the model | 220.0 | 220.7 |
| first ctl.wrench carrying it | 150238.8 | 239.0 |
| first thr.cmd carrying it | 150242.9 | 243.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.54 | 0.209 | 0 | 0.027 / 0.073 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.49 | 0.215 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.51 | 0.226 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.04 / 20.39 | 0.276 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.52 | 0.208 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 19.99 / 20.03 | 0.009 | 0 | 0.019 / 0.031 |
| M1 | load | ctl.depth_hold | 20 | 19.94 / 20.04 | 0.020 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.03 / 20.91 | 0.378 | 0 | - |
| M1 | load | thr.cmd | 19 | 19.05 / 20.07 | 0.234 | 0 | - |
| M1 | load | nav.state | 19 | 19.99 / 20.03 | 0.008 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.61 | 0.194 | 0 | 0.028 / 0.104 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.38 | 0.192 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.32 | 0.198 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.03 / 20.47 | 0.276 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.58 | 0.192 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 20.00 / 20.04 | 0.009 | 0 | 0.020 / 0.035 |
| M2 | load | ctl.depth_hold | 20 | 20.01 / 20.04 | 0.007 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.04 / 20.27 | 0.231 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.08 / 20.08 | 0.223 | 0 | - |
| M2 | load | nav.state | 19 | 20.02 / 20.03 | 0.007 | 0 | - |
