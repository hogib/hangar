| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.96 / 1.96 | 1.88, 9.7 %, 6.3 s | 3.03, 19.8 %, 10.4 s |
| B2 | residual | M1 | nominal | 100.0 | -2.02 / 2.02 | 1.23, 1.0 %, 2.2 s | 1.92, 3.7 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.00 / 1.01 | 1.34, 0.0 %, 2.9 s | 1.32, 1.3 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.93 / 1.93 | 1.46, 0.0 %, 2.7 s | 1.61, 3.6 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -1.98 / 1.98 | 2.75, 16.3 %, 8.7 s | 2.20, 14.2 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.7 | 1.97 | 8.2 | -1.99 / 1.99 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.1 | -2.03 / 2.03 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -2.10 / 2.10 |
| T2 | residual M1 | buoyancy -4 N | 2.8 | 0.27 | 0.0 | -0.91 / 0.92 |
| S2 | residual M1 to M2 | deploy and swap | 1.9 | 0.32 | 0.0 | -1.72 / 1.73 |
| H2 | residual M2 to assisted | set_mode assisted | 3.5 | 0.47 | 0.0 | -2.02 / 2.02 |
| T3 | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.3 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0016 | -0.0848 |
| B2 | -0.0872 | 0.0014 | -0.0858 |
| B3 | 0.0083 | 0.0595 | 0.0717 |
| B4 | 0.0898 | -0.0007 | 0.0894 |
| B5 | 0.0885 | -0.0013 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.1 | 0.1 |
| deploy called | 19.8 | 9.2 |
| load: checking | 32.1 | 22.6 |
| load: warming | 40.1 | 39.7 |
| swapped (policy.active) | 220.3 | 219.9 |
| first policy.wrench of the model | 240.3 | 240.1 |
| first ctl.wrench carrying it | 150277.1 | 256.0 |
| first thr.cmd carrying it | 150292.4 | 271.3 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 16.99 / 22.81 | 0.361 | 0 | 0.022 / 2.345 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 21.12 | 0.198 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.74 | 0.158 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.04 / 20.98 | 0.269 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 17.06 / 22.73 | 0.318 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 20.01 / 20.36 | 0.076 | 0 | 0.027 / 0.039 |
| M1 | load | ctl.depth_hold | 20 | 19.38 / 20.03 | 0.144 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.44 / 20.04 | 0.131 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.07 / 21.36 | 0.514 | 0 | - |
| M1 | load | nav.state | 20 | 20.02 / 20.38 | 0.080 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 16.51 / 22.53 | 0.359 | 0 | 0.025 / 2.533 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.88 | 0.160 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 17.03 / 22.46 | 0.229 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 18.07 / 21.34 | 0.270 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 18.02 / 21.99 | 0.250 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 19.03 / 20.20 | 0.248 | 0 | 0.031 / 0.457 |
| M2 | load | ctl.depth_hold | 21 | 19.02 / 20.38 | 0.269 | 0 | - |
| M2 | load | ctl.wrench | 21 | 19.02 / 21.17 | 0.455 | 0 | - |
| M2 | load | thr.cmd | 21 | 19.05 / 20.79 | 0.351 | 0 | - |
| M2 | load | nav.state | 21 | 19.03 / 20.06 | 0.219 | 0 | - |
