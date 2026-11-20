| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.04 / 2.04 | 1.89, 9.1 %, 6.4 s | 3.05, 19.8 %, 10.3 s |
| B2 | residual | M1 | nominal | 100.0 | -2.05 / 2.06 | 1.22, 0.8 %, 2.2 s | 1.91, 3.7 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -1.08 / 1.10 | 1.34, 0.0 %, 2.6 s | 1.34, 1.6 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.96 / 1.97 | 1.48, 0.0 %, 2.7 s | 1.60, 3.3 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -2.02 / 2.02 | 2.76, 16.3 %, 8.7 s | 2.21, 14.2 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.98 | 8.2 | -2.02 / 2.02 |
| T1:back | assisted | buoyancy +4 N | 36.2 | 2.52 | 10.0 | -2.00 / 2.00 |
| H1 | assisted to residual M1 | promotion | 2.6 | 0.42 | 0.0 | -2.07 / 2.07 |
| T2 | residual M1 | buoyancy -4 N | 2.7 | 0.26 | 0.0 | -0.90 / 0.92 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.32 | 0.0 | -1.73 / 1.74 |
| H2 | residual M2 to assisted | set_mode assisted | 3.4 | 0.46 | 0.0 | -2.04 / 2.04 |
| T3 | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.1 | -1.98 / 1.98 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0018 | -0.0850 |
| B2 | -0.0874 | 0.0021 | -0.0854 |
| B3 | 0.0087 | 0.0653 | 0.0778 |
| B4 | 0.0883 | -0.0002 | 0.0884 |
| B5 | 0.0886 | -0.0015 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 11.7 | 7.6 |
| load: checking | 24.6 | 22.0 |
| load: warming | 40.6 | 40.0 |
| swapped (policy.active) | 220.9 | 219.2 |
| first policy.wrench of the model | 240.0 | 239.3 |
| first ctl.wrench carrying it | 150279.2 | 258.3 |
| first thr.cmd carrying it | 150292.1 | 270.8 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 16.72 / 22.83 | 0.291 | 0 | 0.023 / 0.326 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 18.02 / 22.80 | 0.314 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 18.02 / 22.23 | 0.269 | 0 | - |
| M1 | before (10 s) | thr.cmd | 499 | 17.07 / 22.36 | 0.306 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 17.03 / 22.52 | 0.274 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 21 | 19.15 / 20.52 | 0.247 | 0 | 0.024 / 0.164 |
| M1 | load | ctl.depth_hold | 21 | 18.91 / 20.80 | 0.333 | 0 | - |
| M1 | load | ctl.wrench | 21 | 18.78 / 20.27 | 0.304 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.09 / 20.10 | 0.219 | 0 | - |
| M1 | load | nav.state | 21 | 19.12 / 20.53 | 0.244 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 17.01 / 22.79 | 0.373 | 0 | 0.026 / 2.795 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 17.03 / 22.70 | 0.315 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 18.02 / 22.25 | 0.270 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 17.07 / 22.36 | 0.311 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 17.03 / 22.42 | 0.281 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 20 | 18.03 / 22.44 | 0.772 | 0 | 0.031 / 0.040 |
| M2 | load | ctl.depth_hold | 21 | 18.03 / 21.57 | 0.567 | 0 | - |
| M2 | load | ctl.wrench | 21 | 19.03 / 20.58 | 0.259 | 0 | - |
| M2 | load | thr.cmd | 21 | 17.07 / 22.53 | 1.082 | 0 | - |
| M2 | load | nav.state | 20 | 18.04 / 22.43 | 0.770 | 0 | - |
