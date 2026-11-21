| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.01 / 2.01 | 1.85, 9.0 %, 6.3 s | 3.01, 19.5 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -1.99 / 1.99 | 1.23, 1.3 %, 2.3 s | 1.89, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.02 / 1.03 | 1.32, 0.0 %, 2.5 s | 1.35, 1.3 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.87 / 1.87 | 1.49, 0.0 %, 2.8 s | 1.59, 3.2 %, 2.3 s |
| B5 | assisted | - | heavier | 0.0 | -1.95 / 1.95 | 2.73, 16.1 %, 8.6 s | 2.18, 14.1 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.8 | 1.98 | 8.2 | -2.02 / 2.02 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.1 | -1.96 / 1.96 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.15 / 2.15 |
| T2 | residual M1 | buoyancy -4 N | 3.2 | 0.24 | 0.0 | -0.93 / 0.94 |
| S2 | residual M1 to M2 | deploy and swap | 1.9 | 0.31 | 0.0 | -1.69 / 1.70 |
| H2 | residual M2 to assisted | set_mode assisted | 3.4 | 0.46 | 0.0 | -1.96 / 1.96 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.54 | 10.1 | -2.01 / 2.02 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0007 | -0.0849 |
| B2 | -0.0869 | 0.0021 | -0.0849 |
| B3 | 0.0084 | 0.0656 | 0.0780 |
| B4 | 0.0886 | -0.0005 | 0.0886 |
| B5 | 0.0885 | -0.0021 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 13.7 | 0.9 |
| load: checking | 21.2 | 8.6 |
| load: warming | 40.2 | 20.6 |
| swapped (policy.active) | 220.5 | 199.8 |
| first policy.wrench of the model | 240.7 | 219.8 |
| first ctl.wrench carrying it | 150259.1 | 238.8 |
| first thr.cmd carrying it | 150262.8 | 242.4 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.99 / 20.43 | 0.191 | 0 | 0.013 / 0.250 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.03 / 20.45 | 0.227 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.41 | 0.173 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.59 | 0.296 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.42 | 0.188 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 21 | 19.02 / 20.28 | 0.235 | 0 | 0.029 / 0.038 |
| M1 | load | ctl.depth_hold | 21 | 19.04 / 20.26 | 0.231 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.38 / 20.25 | 0.150 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.06 / 20.08 | 0.223 | 0 | - |
| M1 | load | nav.state | 21 | 19.03 / 20.25 | 0.232 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.39 | 0.209 | 0 | 0.030 / 0.294 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.49 | 0.217 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.44 | 0.229 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.50 | 0.298 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.36 | 0.206 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 19.02 / 20.21 | 0.237 | 0 | 0.014 / 0.028 |
| M2 | load | ctl.depth_hold | 20 | 19.01 / 20.03 | 0.224 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.02 / 20.04 | 0.224 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.06 / 20.08 | 0.224 | 0 | - |
| M2 | load | nav.state | 19 | 19.02 / 20.21 | 0.237 | 0 | - |
