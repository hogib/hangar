| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.99 / 2.00 | 1.86, 9.2 %, 6.4 s | 3.02, 19.9 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -2.03 / 2.03 | 1.23, 0.9 %, 2.3 s | 1.90, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.02 / 1.03 | 1.32, 0.0 %, 2.5 s | 1.36, 1.3 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.88 / 1.89 | 1.50, 0.0 %, 2.8 s | 1.58, 3.2 %, 2.3 s |
| B5 | assisted | - | heavier | 0.0 | -1.95 / 1.95 | 2.75, 16.1 %, 8.7 s | 2.20, 14.6 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.8 | 1.99 | 8.3 | -2.03 / 2.03 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.2 | -1.97 / 1.97 |
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -2.12 / 2.13 |
| T2 | residual M1 | buoyancy -4 N | 2.8 | 0.24 | 0.0 | -0.90 / 0.91 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.31 | 0.0 | -1.66 / 1.67 |
| H2 | residual M2 to assisted | set_mode assisted | 3.5 | 0.46 | 0.0 | -1.92 / 1.92 |
| T3 | assisted | buoyancy +4 N | 36.3 | 2.54 | 10.3 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0019 | -0.0847 |
| B2 | -0.0869 | 0.0019 | -0.0851 |
| B3 | 0.0089 | 0.0655 | 0.0783 |
| B4 | 0.0885 | -0.0006 | 0.0884 |
| B5 | 0.0886 | -0.0015 | 0.0886 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 5.7 | 19.7 |
| load: checking | 16.9 | 33.3 |
| load: warming | 19.9 | 40.5 |
| swapped (policy.active) | 200.1 | 220.9 |
| first policy.wrench of the model | 220.5 | 242.0 |
| first ctl.wrench carrying it | 150255.7 | 256.4 |
| first thr.cmd carrying it | 150264.3 | 265.2 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.44 | 0.149 | 0 | 0.016 / 0.103 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 18.02 / 22.33 | 0.234 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 21.63 | 0.173 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.02 / 20.34 | 0.246 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.45 | 0.149 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 19.50 / 20.41 | 0.153 | 0 | 0.015 / 0.049 |
| M1 | load | ctl.depth_hold | 20 | 19.64 / 20.02 | 0.091 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.02 / 20.35 | 0.238 | 0 | - |
| M1 | load | thr.cmd | 19 | 19.06 / 20.07 | 0.229 | 0 | - |
| M1 | load | nav.state | 19 | 19.52 / 20.39 | 0.147 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 16.94 / 22.77 | 0.273 | 0 | 0.021 / 0.195 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 18.03 / 22.08 | 0.205 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 21.49 | 0.187 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 18.04 / 22.34 | 0.361 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 17.04 / 22.67 | 0.263 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 17.98 / 21.11 | 0.526 | 0 | 0.026 / 2.117 |
| M2 | load | ctl.depth_hold | 20 | 19.02 / 20.60 | 0.323 | 0 | - |
| M2 | load | ctl.wrench | 20 | 18.03 / 21.68 | 0.596 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.06 / 20.14 | 0.293 | 0 | - |
| M2 | load | nav.state | 21 | 19.03 / 20.44 | 0.242 | 0 | - |
