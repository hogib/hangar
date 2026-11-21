| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.01 / 2.01 | 1.86, 8.8 %, 6.4 s | 3.04, 19.8 %, 10.4 s |
| B2 | residual | M1 | nominal | 100.0 | -2.02 / 2.03 | 1.26, 1.4 %, 2.3 s | 1.86, 3.4 %, 2.4 s |
| B3 | residual | M1 | heavier | 100.0 | -0.77 / 0.78 | 1.31, 0.0 %, 2.8 s | 1.41, 2.0 %, 2.5 s |
| B4 | residual | M2 | heavier | 100.0 | -1.89 / 1.89 | 1.58, 0.0 %, 3.0 s | 1.53, 2.5 %, 2.4 s |
| B5 | assisted | - | heavier | 0.0 | -1.97 / 1.97 | 2.73, 16.2 %, 8.7 s | 2.19, 14.4 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 33.0 | 1.97 | 8.2 | -1.98 / 1.98 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.1 | -1.98 / 1.98 |
| H1 | assisted to residual M1 | promotion | 2.6 | 0.42 | 0.0 | -2.09 / 2.10 |
| T2 | residual M1 | buoyancy -4 N | 2.8 | 0.16 | 0.0 | -0.47 / 0.49 |
| S2 | residual M1 to M2 | deploy and swap | 2.2 | 0.36 | 0.0 | -1.82 / 1.82 |
| H2 | residual M2 to assisted | set_mode assisted | 3.5 | 0.46 | 0.0 | -2.02 / 2.02 |
| T3 | assisted | buoyancy +4 N | 36.6 | 2.53 | 10.2 | -1.96 / 1.96 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0034 | -0.0846 |
| B2 | -0.0868 | 0.0026 | -0.0841 |
| B3 | 0.0217 | 0.0600 | 0.0865 |
| B4 | 0.0866 | -0.0031 | 0.0838 |
| B5 | 0.0886 | -0.0043 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 13.1 | 4.0 |
| load: checking | 474.9 | 471.5 |
| load: warming | 479.9 | 480.5 |
| swapped (policy.active) | 660.0 | 659.7 |
| first policy.wrench of the model | 680.0 | 679.7 |
| first ctl.wrench carrying it | 150698.0 | 698.8 |
| first thr.cmd carrying it | 150704.6 | 705.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.04 | 0.133 | 0 | 0.013 / 0.043 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.45 | 0.142 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.14 | 0.133 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.255 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.04 | 0.133 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.01 / 20.52 | 0.194 | 0 | 0.023 / 0.042 |
| M1 | load | ctl.depth_hold | 43 | 19.57 / 20.27 | 0.080 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.03 / 20.39 | 0.163 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.08 / 20.08 | 0.213 | 0 | - |
| M1 | load | nav.state | 43 | 19.02 / 20.51 | 0.192 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 18.13 / 20.95 | 0.176 | 0 | 0.027 / 0.991 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.17 | 0.166 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.15 | 0.182 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.36 | 0.256 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.09 | 0.154 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 42 | 19.01 / 20.91 | 0.261 | 0 | 0.025 / 0.069 |
| M2 | load | ctl.depth_hold | 43 | 19.52 / 20.04 | 0.077 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.05 / 20.27 | 0.155 | 0 | - |
| M2 | load | thr.cmd | 43 | 19.07 / 20.09 | 0.257 | 0 | - |
| M2 | load | nav.state | 42 | 19.02 / 20.91 | 0.260 | 0 | - |
