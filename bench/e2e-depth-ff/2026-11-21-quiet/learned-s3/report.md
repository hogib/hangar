| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.01 / 2.01 | 1.86, 9.8 %, 6.2 s | 3.03, 19.9 %, 10.4 s |
| B2 | residual | M1 | nominal | 100.0 | -1.98 / 1.98 | 1.22, 0.5 %, 2.3 s | 1.83, 3.3 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.87 / 0.88 | 1.35, 0.0 %, 2.8 s | 1.44, 1.6 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.92 / 1.92 | 1.45, 0.0 %, 3.0 s | 1.62, 3.7 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -1.98 / 1.98 | 2.69, 16.1 %, 8.6 s | 2.20, 13.9 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.98 | 8.3 | -2.03 / 2.03 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.2 | -2.06 / 2.06 |
| H1 | assisted to residual M1 | promotion | 2.3 | 0.40 | 0.0 | -2.02 / 2.02 |
| T2 | residual M1 | buoyancy -4 N | 2.8 | 0.24 | 0.0 | -0.88 / 0.89 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.30 | 0.0 | -1.63 / 1.63 |
| H2 | residual M2 to assisted | set_mode assisted | 3.5 | 0.46 | 0.0 | -2.05 / 2.05 |
| T3 | assisted | buoyancy +4 N | 36.5 | 2.54 | 10.2 | -2.00 / 2.00 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | -0.0005 | -0.0849 |
| B2 | -0.0856 | 0.0021 | -0.0837 |
| B3 | 0.0122 | 0.0655 | 0.0823 |
| B4 | 0.0907 | -0.0029 | 0.0880 |
| B5 | 0.0885 | -0.0042 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 16.2 | 4.3 |
| load: checking | 473.4 | 465.4 |
| load: warming | 480.4 | 479.4 |
| swapped (policy.active) | 660.5 | 659.8 |
| first policy.wrench of the model | 680.9 | 679.8 |
| first ctl.wrench carrying it | 150698.9 | 697.9 |
| first thr.cmd carrying it | 150703.1 | 701.8 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.85 / 20.56 | 0.205 | 0 | 0.027 / 0.477 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.56 | 0.258 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.48 | 0.225 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.51 | 0.278 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.56 | 0.197 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.01 / 20.46 | 0.169 | 0 | 0.014 / 0.083 |
| M1 | load | ctl.depth_hold | 43 | 19.03 / 20.08 | 0.171 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.03 / 20.23 | 0.158 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.09 | 0.214 | 0 | - |
| M1 | load | nav.state | 43 | 19.01 / 20.46 | 0.169 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.51 | 0.196 | 0 | 0.031 / 0.148 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.53 | 0.245 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.52 | 0.230 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.56 | 0.282 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.52 | 0.196 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 43 | 19.04 / 20.52 | 0.186 | 0 | 0.029 / 0.051 |
| M2 | load | ctl.depth_hold | 43 | 19.09 / 20.29 | 0.169 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.02 / 20.61 | 0.306 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.09 | 0.214 | 0 | - |
| M2 | load | nav.state | 43 | 19.05 / 20.52 | 0.185 | 0 | - |
