| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.95 / 1.95 | 1.85, 8.7 %, 6.3 s | 3.02, 19.8 %, 10.1 s |
| B2 | residual | M1 | nominal | 100.0 | -1.78 / 4.28 | 4.30, 27.3 %, 24.2 s | 3.94, 41.0 %, - s |
| B3 | residual | M1 | heavier | 100.0 | -2.21 / 3.00 | 4.97, 37.2 %, - s | 3.08, 34.7 %, 8.3 s |
| B4 | residual | M2 | heavier | 100.0 | -7.24 / 23.07 | 6.56, 57.3 %, - s | 5.60, 32.0 %, - s |
| B5 | assisted | - | heavier | 0.0 | -2.00 / 2.00 | 2.73, 15.9 %, 8.6 s | 2.18, 14.3 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.97 | 8.2 | -2.01 / 2.01 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.5 | -2.05 / 2.05 |
| H1 | assisted to residual M1 | promotion | 11.9 | 0.94 | - | -2.87 / 6.73 |
| T2 | residual M1 | buoyancy -4 N | 24.2 | 2.83 | 26.4 | -2.39 / 2.90 |
| S2 | residual M1 to M2 | deploy and swap | 38.4 | 3.77 | - | -11.04 / 18.35 |
| H2 | residual M2 to assisted | set_mode assisted | 23.9 | 1.70 | 10.3 | -1.15 / 1.23 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.54 | 10.1 | -2.03 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | -0.0848 | -0.0846 |
| B2 | -0.0313 | -0.0150 | -0.0442 |
| B3 | 0.0961 | -0.0359 | 0.0598 |
| B4 | 0.0228 | 0.0878 | 0.0721 |
| B5 | 0.0887 | -0.1379 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 8.4 | 19.3 |
| load: checking | 17.3 | 28.8 |
| load: warming | 20.3 | 39.8 |
| swapped (policy.active) | 1000.1 | 1019.5 |
| first policy.wrench of the model | 1020.2 | 1039.5 |
| first ctl.wrench carrying it | 151038.3 | 1058.4 |
| first thr.cmd carrying it | 151043.0 | 1062.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.07 | 0.159 | 0 | 0.023 / 0.065 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.06 | 0.159 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.07 | 0.177 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.04 / 20.10 | 0.255 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.159 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 60 | 19.01 / 20.08 | 0.181 | 0 | 0.015 / 0.030 |
| M1 | load | ctl.depth_hold | 60 | 19.03 / 20.68 | 0.237 | 0 | - |
| M1 | load | ctl.wrench | 60 | 19.02 / 20.04 | 0.130 | 0 | - |
| M1 | load | thr.cmd | 59 | 19.06 / 20.08 | 0.253 | 0 | - |
| M1 | load | nav.state | 60 | 19.01 / 20.08 | 0.181 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.28 | 0.154 | 0 | 0.034 / 0.059 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.35 | 0.167 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.55 | 0.177 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.02 / 20.10 | 0.256 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.28 | 0.153 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 60 | 19.02 / 20.32 | 0.235 | 0 | 0.027 / 0.075 |
| M2 | load | ctl.depth_hold | 60 | 19.03 / 20.43 | 0.256 | 0 | - |
| M2 | load | ctl.wrench | 60 | 19.02 / 20.33 | 0.271 | 0 | - |
| M2 | load | thr.cmd | 60 | 19.06 / 20.13 | 0.271 | 0 | - |
| M2 | load | nav.state | 60 | 19.02 / 20.32 | 0.232 | 0 | - |
