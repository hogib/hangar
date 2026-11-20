| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.93 / 1.93 | 1.89, 9.5 %, 6.3 s | 3.04, 19.6 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -1.98 / 1.98 | 1.24, 1.0 %, 2.2 s | 1.93, 3.8 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.01 / 1.02 | 1.34, 0.0 %, 2.8 s | 1.32, 1.2 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.92 / 1.93 | 1.45, 0.0 %, 2.7 s | 1.60, 3.5 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -1.99 / 1.99 | 2.75, 16.3 %, 8.7 s | 2.20, 14.7 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.7 | 1.98 | 8.2 | -1.99 / 1.99 |
| T1:back | assisted | buoyancy +4 N | 36.2 | 2.53 | 10.2 | -2.05 / 2.05 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -2.11 / 2.11 |
| T2 | residual M1 | buoyancy -4 N | 2.7 | 0.27 | 0.0 | -0.87 / 0.88 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.32 | 0.0 | -1.70 / 1.71 |
| H2 | residual M2 to assisted | set_mode assisted | 3.7 | 0.47 | 0.0 | -2.05 / 2.05 |
| T3 | assisted | buoyancy +4 N | 36.2 | 2.53 | 10.2 | -1.98 / 1.98 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0027 | -0.0846 |
| B2 | -0.0872 | 0.0016 | -0.0855 |
| B3 | 0.0087 | 0.0612 | 0.0733 |
| B4 | 0.0897 | -0.0014 | 0.0885 |
| B5 | 0.0885 | -0.0021 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 7.8 | 16.4 |
| load: checking | 19.8 | 28.4 |
| load: warming | 41.8 | 40.4 |
| swapped (policy.active) | 219.9 | 220.6 |
| first policy.wrench of the model | 239.9 | 240.6 |
| first ctl.wrench carrying it | 150280.0 | 260.6 |
| first thr.cmd carrying it | 150291.5 | 271.8 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 16.99 / 22.88 | 0.225 | 0 | 0.011 / 0.087 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.26 | 0.118 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 17.02 / 22.78 | 0.222 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 18.07 / 21.61 | 0.258 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 17.03 / 22.80 | 0.222 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 17.97 / 21.90 | 0.638 | 0 | 0.023 / 0.077 |
| M1 | load | ctl.depth_hold | 21 | 19.02 / 20.69 | 0.377 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.86 / 20.03 | 0.036 | 0 | - |
| M1 | load | thr.cmd | 21 | 18.07 / 22.59 | 0.784 | 0 | - |
| M1 | load | nav.state | 20 | 18.02 / 21.91 | 0.632 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 16.78 / 22.95 | 0.526 | 0 | 0.018 / 2.010 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 18.03 / 21.88 | 0.367 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.02 / 22.79 | 0.415 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 17.08 / 22.85 | 0.443 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 17.02 / 22.83 | 0.499 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 20 | 19.73 / 20.04 | 0.065 | 0 | 0.019 / 0.044 |
| M2 | load | ctl.depth_hold | 20 | 19.02 / 20.02 | 0.223 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.62 / 20.04 | 0.090 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.08 / 20.07 | 0.221 | 0 | - |
| M2 | load | nav.state | 20 | 19.75 / 20.03 | 0.060 | 0 | - |
