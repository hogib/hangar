| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.01 / 2.02 | 1.85, 9.2 %, 6.3 s | 3.02, 19.8 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -2.02 / 2.02 | 1.23, 1.0 %, 2.3 s | 1.89, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.99 / 1.99 | 1.47, 6.5 %, 4.3 s | 2.22, 5.2 %, 5.0 s |
| B4 | residual | M2 | heavier | 100.0 | -2.05 / 2.06 | 1.46, 6.7 %, 4.4 s | 2.21, 5.5 %, 4.8 s |
| B5 | assisted | - | heavier | 0.0 | -1.96 / 1.97 | 1.97, 10.3 %, 6.5 s | 3.48, 22.6 %, 11.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 6.5 | 0.98 | 4.2 | -2.05 / 2.05 |
| T1:back | assisted | buoyancy +4 N | 2.2 | 0.71 | 0.0 | -1.99 / 1.99 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.06 / 2.06 |
| T2 | residual M1 | buoyancy -4 N | 4.3 | 1.12 | 0.0 | -2.10 / 2.10 |
| S2 | residual M1 to M2 | deploy and swap | 2.6 | 0.43 | 0.0 | -2.06 / 2.06 |
| H2 | residual M2 to assisted | set_mode assisted | 3.4 | 0.47 | 0.0 | -2.01 / 2.01 |
| T3 | assisted | buoyancy +4 N | 2.2 | 0.73 | 0.0 | -2.05 / 2.06 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0027 | -0.0848 |
| B2 | -0.0875 | 0.0024 | -0.0851 |
| B3 | -0.1044 | 0.0008 | -0.1036 |
| B4 | -0.1047 | 0.0006 | -0.1042 |
| B5 | -0.1030 | 0.0015 | -0.1025 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 12.8 | 5.5 |
| load: checking | 24.3 | 27.4 |
| load: warming | 39.6 | 40.1 |
| swapped (policy.active) | 219.9 | 220.1 |
| first policy.wrench of the model | 241.5 | 240.1 |
| first ctl.wrench carrying it | 150278.9 | 241.5 |
| first thr.cmd carrying it | 150285.7 | 245.6 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.04 / 21.70 | 0.264 | 0 | 0.018 / 1.699 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 17.02 / 22.40 | 0.312 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 18.02 / 21.74 | 0.310 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.85 | 0.255 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 18.07 / 21.54 | 0.229 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 21 | 18.07 / 21.57 | 0.559 | 0 | 0.023 / 0.121 |
| M1 | load | ctl.depth_hold | 21 | 19.04 / 21.51 | 0.462 | 0 | - |
| M1 | load | ctl.wrench | 21 | 18.21 / 21.86 | 0.645 | 0 | - |
| M1 | load | thr.cmd | 20 | 17.07 / 22.88 | 1.315 | 0 | - |
| M1 | load | nav.state | 21 | 18.15 / 21.48 | 0.531 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 13.65 / 26.21 | 0.804 | 0 | 0.028 / 6.411 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 17.03 / 22.86 | 0.620 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.03 / 22.32 | 0.631 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 17.07 / 23.00 | 0.812 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 17.03 / 22.99 | 0.681 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 18.18 / 21.94 | 0.744 | 0 | 0.029 / 1.910 |
| M2 | load | ctl.depth_hold | 21 | 17.77 / 22.28 | 0.921 | 0 | - |
| M2 | load | ctl.wrench | 21 | 18.60 / 22.13 | 0.781 | 0 | - |
| M2 | load | thr.cmd | 21 | 17.08 / 23.05 | 1.290 | 0 | - |
| M2 | load | nav.state | 21 | 19.05 / 20.61 | 0.352 | 0 | - |
