| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.94 / 1.94 | 1.86, 9.2 %, 6.3 s | 3.03, 19.9 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -2.02 / 2.03 | 1.22, 0.9 %, 2.2 s | 1.91, 3.7 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.95 / 0.96 | 1.36, 0.0 %, 2.9 s | 1.34, 1.3 %, 2.8 s |
| B4 | residual | M2 | heavier | 100.0 | -1.90 / 1.91 | 1.46, 0.0 %, 2.7 s | 1.61, 3.5 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.01 / 2.01 | 2.72, 16.1 %, 8.6 s | 2.19, 14.7 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.97 | 8.2 | -2.03 / 2.03 |
| T1:back | assisted | buoyancy +4 N | 36.5 | 2.53 | 10.2 | -2.02 / 2.02 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.03 / 2.04 |
| T2 | residual M1 | buoyancy -4 N | 3.0 | 0.26 | 0.0 | -0.90 / 0.91 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.32 | 0.0 | -1.69 / 1.70 |
| H2 | residual M2 to assisted | set_mode assisted | 3.6 | 0.47 | 0.0 | -1.98 / 1.98 |
| T3 | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.1 | -1.95 / 1.95 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0015 | -0.0848 |
| B2 | -0.0873 | 0.0012 | -0.0861 |
| B3 | 0.0087 | 0.0673 | 0.0801 |
| B4 | 0.0899 | -0.0012 | 0.0890 |
| B5 | 0.0885 | -0.0028 | 0.0884 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 7.5 | 3.5 |
| load: checking | 15.0 | 15.3 |
| load: warming | 20.0 | 20.3 |
| swapped (policy.active) | 200.2 | 199.5 |
| first policy.wrench of the model | 220.1 | 219.6 |
| first ctl.wrench carrying it | 150239.4 | 238.6 |
| first thr.cmd carrying it | 150242.7 | 242.9 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.40 | 0.148 | 0 | 0.015 / 0.080 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.24 | 0.147 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.50 | 0.166 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.05 / 20.45 | 0.264 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.40 | 0.148 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 19.89 / 20.05 | 0.034 | 0 | 0.018 / 0.029 |
| M1 | load | ctl.depth_hold | 20 | 19.90 / 20.05 | 0.029 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.04 / 20.08 | 0.221 | 0 | - |
| M1 | load | thr.cmd | 19 | 19.06 / 20.08 | 0.313 | 0 | - |
| M1 | load | nav.state | 19 | 19.90 / 20.04 | 0.030 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.08 | 0.133 | 0 | 0.022 / 0.075 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.22 | 0.160 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.16 | 0.165 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.252 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.04 | 0.134 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 19.03 / 20.04 | 0.229 | 0 | 0.026 / 0.039 |
| M2 | load | ctl.depth_hold | 20 | 19.03 / 20.08 | 0.223 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.03 / 20.05 | 0.224 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.07 / 20.08 | 0.230 | 0 | - |
| M2 | load | nav.state | 19 | 19.03 / 20.04 | 0.229 | 0 | - |
