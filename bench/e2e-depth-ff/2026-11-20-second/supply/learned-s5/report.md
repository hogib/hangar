| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.99 / 1.99 | 1.83, 9.0 %, 6.2 s | 3.01, 19.5 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -1.99 / 1.99 | 1.24, 1.7 %, 2.2 s | 1.91, 3.6 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -2.02 / 2.02 | 1.44, 5.7 %, 4.1 s | 2.20, 4.8 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -2.04 / 2.04 | 1.41, 6.2 %, 4.2 s | 2.21, 4.8 %, 2.8 s |
| B5 | assisted | - | heavier | 0.0 | -1.97 / 1.97 | 2.01, 10.6 %, 6.6 s | 3.46, 22.4 %, 10.9 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 6.4 | 0.98 | 4.2 | -1.92 / 1.93 |
| T1:back | assisted | buoyancy +4 N | 2.2 | 0.71 | 0.0 | -1.96 / 1.96 |
| H1 | assisted to residual M1 | promotion | 2.6 | 0.43 | 0.0 | -2.13 / 2.13 |
| T2 | residual M1 | buoyancy -4 N | 4.2 | 1.12 | 0.0 | -2.08 / 2.08 |
| S2 | residual M1 to M2 | deploy and swap | 2.6 | 0.43 | 0.0 | -2.04 / 2.04 |
| H2 | residual M2 to assisted | set_mode assisted | 3.4 | 0.47 | 0.0 | -2.00 / 2.00 |
| T3 | assisted | buoyancy +4 N | 2.2 | 0.71 | 0.0 | -1.98 / 1.99 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0048 | -0.0848 |
| B2 | -0.0891 | 0.0034 | -0.0858 |
| B3 | -0.1057 | 0.0019 | -0.1037 |
| B4 | -0.1062 | 0.0028 | -0.1035 |
| B5 | -0.1027 | 0.0035 | -0.1026 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.1 |
| deploy called | 15.5 | 10.2 |
| load: checking | 750.0 | 793.3 |
| load: warming | 760.0 | 800.3 |
| swapped (policy.active) | 940.1 | 979.7 |
| first policy.wrench of the model | 960.1 | 999.8 |
| first ctl.wrench carrying it | 150998.1 | 1018.1 |
| first thr.cmd carrying it | 151006.0 | 1025.3 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 17.00 / 22.49 | 0.362 | 0 | 0.021 / 1.236 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 18.02 / 21.48 | 0.217 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 17.03 / 22.66 | 0.303 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 18.07 / 22.07 | 0.304 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 17.03 / 22.48 | 0.343 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 56 | 17.99 / 20.34 | 0.305 | 0 | 0.024 / 0.058 |
| M1 | load | ctl.depth_hold | 57 | 19.02 / 20.75 | 0.242 | 0 | - |
| M1 | load | ctl.wrench | 57 | 19.02 / 20.93 | 0.228 | 0 | - |
| M1 | load | thr.cmd | 56 | 19.06 / 20.09 | 0.259 | 0 | - |
| M1 | load | nav.state | 56 | 18.03 / 20.33 | 0.301 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 17.04 / 22.64 | 0.465 | 0 | 0.032 / 2.436 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 17.04 / 22.75 | 0.306 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.04 / 22.78 | 0.277 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 17.07 / 22.71 | 0.404 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 17.03 / 22.64 | 0.396 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 58 | 18.52 / 21.58 | 0.371 | 0 | 0.038 / 1.556 |
| M2 | load | ctl.depth_hold | 59 | 19.03 / 20.31 | 0.189 | 0 | - |
| M2 | load | ctl.wrench | 59 | 19.03 / 20.42 | 0.237 | 0 | - |
| M2 | load | thr.cmd | 58 | 19.07 / 21.42 | 0.348 | 0 | - |
| M2 | load | nav.state | 58 | 19.03 / 20.55 | 0.235 | 0 | - |
