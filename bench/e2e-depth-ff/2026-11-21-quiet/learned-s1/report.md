| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.98 / 1.98 | 1.86, 9.4 %, 6.3 s | 3.03, 19.7 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -1.99 / 2.00 | 1.26, 1.4 %, 2.3 s | 1.88, 3.5 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.72 / 0.74 | 1.30, 0.0 %, 2.7 s | 1.44, 1.8 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.83 / 1.83 | 1.49, 0.0 %, 3.1 s | 1.57, 3.1 %, 2.3 s |
| B5 | assisted | - | heavier | 0.0 | -1.93 / 1.94 | 2.72, 16.1 %, 8.6 s | 2.17, 14.2 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.98 | 8.2 | -2.02 / 2.03 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.53 | 10.1 | -2.00 / 2.01 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.07 / 2.08 |
| T2 | residual M1 | buoyancy -4 N | 2.7 | 0.21 | 0.0 | -0.70 / 0.71 |
| S2 | residual M1 to M2 | deploy and swap | 2.0 | 0.31 | 0.0 | -1.62 / 1.64 |
| H2 | residual M2 to assisted | set_mode assisted | 3.4 | 0.45 | 0.0 | -1.99 / 1.99 |
| T3 | assisted | buoyancy +4 N | 36.6 | 2.54 | 10.1 | -2.03 / 2.03 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0848 | 0.0059 | -0.0847 |
| B2 | -0.0883 | 0.0034 | -0.0852 |
| B3 | 0.0134 | 0.0691 | 0.0879 |
| B4 | 0.0855 | 0.0016 | 0.0879 |
| B5 | 0.0885 | -0.0022 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 12.5 | 4.2 |
| load: checking | 470.3 | 466.9 |
| load: warming | 479.3 | 479.9 |
| swapped (policy.active) | 659.6 | 660.1 |
| first policy.wrench of the model | 679.4 | 680.2 |
| first ctl.wrench carrying it | 150699.1 | 698.1 |
| first thr.cmd carrying it | 150702.1 | 702.6 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.40 | 0.167 | 0 | 0.018 / 0.250 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.49 | 0.186 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.50 | 0.184 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.38 | 0.261 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.39 | 0.167 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 42 | 19.03 / 20.06 | 0.154 | 0 | 0.015 / 0.035 |
| M1 | load | ctl.depth_hold | 43 | 19.01 / 20.52 | 0.228 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.22 / 20.04 | 0.123 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.08 / 20.08 | 0.259 | 0 | - |
| M1 | load | nav.state | 43 | 19.03 / 20.04 | 0.152 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.25 | 0.160 | 0 | 0.022 / 0.060 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.03 / 20.59 | 0.178 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.25 | 0.166 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.04 / 20.37 | 0.272 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.26 | 0.160 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 43 | 19.02 / 20.56 | 0.233 | 0 | 0.019 / 0.037 |
| M2 | load | ctl.depth_hold | 43 | 19.03 / 20.19 | 0.154 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.02 / 20.72 | 0.242 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.09 | 0.261 | 0 | - |
| M2 | load | nav.state | 43 | 19.02 / 20.57 | 0.234 | 0 | - |
