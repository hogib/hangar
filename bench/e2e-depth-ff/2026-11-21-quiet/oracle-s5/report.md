| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.98 / 1.99 | 1.88, 9.4 %, 6.2 s | 3.01, 19.5 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -2.06 / 2.06 | 1.22, 0.9 %, 2.3 s | 1.91, 3.6 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.04 / 1.06 | 1.34, 0.0 %, 2.6 s | 1.35, 1.3 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.92 / 1.93 | 1.46, 0.0 %, 2.7 s | 1.59, 3.3 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.04 / 2.04 | 2.73, 16.0 %, 8.7 s | 2.19, 14.1 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.8 | 1.97 | 8.2 | -2.00 / 2.01 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.52 | 10.1 | -1.97 / 1.97 |
| H1 | assisted to residual M1 | promotion | 2.6 | 0.42 | 0.0 | -2.04 / 2.04 |
| T2 | residual M1 | buoyancy -4 N | 3.3 | 0.26 | 0.0 | -0.93 / 0.94 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.32 | 0.0 | -1.72 / 1.72 |
| H2 | residual M2 to assisted | set_mode assisted | 3.3 | 0.46 | 0.0 | -2.03 / 2.03 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.1 | -2.03 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0018 | -0.0847 |
| B2 | -0.0874 | 0.0020 | -0.0856 |
| B3 | 0.0089 | 0.0659 | 0.0784 |
| B4 | 0.0881 | 0.0003 | 0.0890 |
| B5 | 0.0885 | -0.0006 | 0.0887 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 8.2 | 8.7 |
| load: checking | 16.2 | 17.6 |
| load: warming | 20.2 | 20.6 |
| swapped (policy.active) | 199.5 | 199.9 |
| first policy.wrench of the model | 219.5 | 219.9 |
| first ctl.wrench carrying it | 150239.1 | 239.1 |
| first thr.cmd carrying it | 150242.6 | 242.5 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.40 | 0.136 | 0 | 0.025 / 0.419 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.36 | 0.148 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.25 | 0.147 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.51 | 0.271 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.33 | 0.134 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 19.02 / 20.73 | 0.289 | 0 | 0.017 / 0.036 |
| M1 | load | ctl.depth_hold | 20 | 19.81 / 20.02 | 0.046 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.02 / 20.69 | 0.353 | 0 | - |
| M1 | load | thr.cmd | 19 | 19.06 / 20.08 | 0.230 | 0 | - |
| M1 | load | nav.state | 19 | 19.03 / 20.73 | 0.287 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 18.96 / 20.57 | 0.158 | 0 | 0.025 / 0.218 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.28 | 0.165 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.36 | 0.184 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.20 | 0.255 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.57 | 0.155 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 19.01 / 20.05 | 0.234 | 0 | 0.030 / 0.044 |
| M2 | load | ctl.depth_hold | 20 | 19.02 / 20.04 | 0.224 | 0 | - |
| M2 | load | ctl.wrench | 20 | 20.02 / 20.11 | 0.018 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.07 / 20.10 | 0.230 | 0 | - |
| M2 | load | nav.state | 19 | 19.02 / 20.04 | 0.230 | 0 | - |
