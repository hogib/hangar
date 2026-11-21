| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.00 / 2.00 | 1.87, 9.7 %, 6.2 s | 3.00, 19.6 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -1.99 / 2.00 | 1.22, 0.7 %, 2.4 s | 1.81, 3.4 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.05 / 1.07 | 1.34, 0.0 %, 2.8 s | 1.38, 1.5 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.89 / 1.89 | 1.44, 0.0 %, 2.9 s | 1.61, 3.5 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.01 / 2.01 | 2.72, 15.9 %, 8.6 s | 2.17, 14.1 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.7 | 1.97 | 8.2 | -1.97 / 1.97 |
| T1:back | assisted | buoyancy +4 N | 36.2 | 2.53 | 10.2 | -2.03 / 2.04 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.40 | 0.0 | -2.00 / 2.00 |
| T2 | residual M1 | buoyancy -4 N | 2.7 | 0.22 | 0.0 | -0.88 / 0.89 |
| S2 | residual M1 to M2 | deploy and swap | 1.9 | 0.30 | 0.0 | -1.58 / 1.58 |
| H2 | residual M2 to assisted | set_mode assisted | 4.1 | 0.49 | 0.0 | -2.01 / 2.01 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.54 | 10.1 | -2.03 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0019 | -0.0846 |
| B2 | -0.0854 | 0.0008 | -0.0846 |
| B3 | 0.0148 | 0.0560 | 0.0747 |
| B4 | 0.0920 | -0.0035 | 0.0889 |
| B5 | 0.0886 | -0.0055 | 0.0887 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 3.3 | 8.5 |
| load: checking | 461.8 | 471.0 |
| load: warming | 479.8 | 480.0 |
| swapped (policy.active) | 660.1 | 660.3 |
| first policy.wrench of the model | 680.1 | 680.3 |
| first ctl.wrench carrying it | 150698.6 | 698.6 |
| first thr.cmd carrying it | 150701.9 | 702.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.27 | 0.173 | 0 | 0.027 / 0.088 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.68 | 0.222 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.46 | 0.230 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.51 | 0.287 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.27 | 0.171 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.02 / 20.51 | 0.240 | 0 | 0.024 / 0.053 |
| M1 | load | ctl.depth_hold | 43 | 19.03 / 20.21 | 0.215 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.03 / 20.48 | 0.215 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.08 / 20.29 | 0.301 | 0 | - |
| M1 | load | nav.state | 43 | 19.02 / 20.52 | 0.240 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 18.94 / 20.53 | 0.217 | 0 | 0.028 / 0.130 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.52 | 0.225 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.49 | 0.222 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.60 | 0.320 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.03 / 20.50 | 0.215 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 43 | 19.03 / 20.45 | 0.224 | 0 | 0.018 / 0.037 |
| M2 | load | ctl.depth_hold | 43 | 19.02 / 20.27 | 0.158 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.03 / 20.25 | 0.162 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.09 | 0.261 | 0 | - |
| M2 | load | nav.state | 43 | 19.03 / 20.45 | 0.224 | 0 | - |
