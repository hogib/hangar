| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.79 / 2.80 | 1.59, 8.7 %, 6.3 s | 2.62, 19.1 %, - s |
| B2 | residual | M1 | nominal | 100.0 | -2.53 / 2.72 | 2.23, 15.9 %, 8.9 s | 3.79, 45.6 %, - s |
| B3 | residual | M1 | heavier | 100.0 | -1.23 / 1.53 | 3.31, 20.8 %, - s | 3.84, 44.5 %, - s |
| B4 | residual | M2 | heavier | 100.0 | 4.94 / 11.47 | 2.97, 28.1 %, 8.5 s | 2.94, 33.5 %, - s |
| B5 | assisted | - | heavier | 0.0 | -2.04 / 2.04 | 2.46, 15.8 %, 8.5 s | 1.87, 12.9 %, 8.1 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.1 | 1.47 | 8.2 | -0.70 / 0.93 |
| T1:back | assisted | buoyancy +4 N | 36.1 | 2.03 | 10.2 | -3.48 / 3.60 |
| H1 | assisted to residual M1 | promotion | 14.9 | 0.74 | 6.9 | 6.76 / 7.47 |
| T2 | residual M1 | buoyancy -4 N | 38.0 | 2.54 | 15.9 | -3.70 / 5.74 |
| S2 | residual M1 to M2 | deploy and swap | 38.3 | 1.81 | - | -23.39 / 27.39 |
| H2 | residual M2 to assisted | set_mode assisted | 31.2 | 0.59 | 5.2 | -3.48 / 3.64 |
| T3 | assisted | buoyancy +4 N | 36.8 | 2.07 | 10.2 | -3.48 / 3.58 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0826 | -0.0844 | -0.0850 |
| B2 | -0.0019 | -0.0851 | -0.0852 |
| B3 | 0.1764 | -0.0866 | 0.0891 |
| B4 | 0.0735 | -0.1118 | 0.0256 |
| B5 | 0.0886 | -0.0818 | 0.0887 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 10.6 | 9.4 |
| load: checking | 19.9 | 17.4 |
| load: warming | 40.0 | 20.4 |
| swapped (policy.active) | 1019.8 | 1000.5 |
| first policy.wrench of the model | 1039.7 | 1020.5 |
| first ctl.wrench carrying it | 61059.1 | 1038.7 |
| first thr.cmd carrying it | 61062.4 | 1042.6 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 21.06 | 0.177 | 0 | 0.023 / 0.081 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 21.22 | 0.179 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.67 | 0.194 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.07 / 20.11 | 0.257 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 21.02 | 0.176 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 60 | 19.02 / 20.05 | 0.130 | 0 | 0.019 / 0.059 |
| M1 | load | ctl.depth_hold | 61 | 19.02 / 20.03 | 0.142 | 0 | - |
| M1 | load | ctl.wrench | 61 | 19.02 / 20.52 | 0.154 | 0 | - |
| M1 | load | thr.cmd | 60 | 19.06 / 20.08 | 0.252 | 0 | - |
| M1 | load | nav.state | 60 | 19.02 / 20.03 | 0.131 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.62 | 0.190 | 0 | 0.027 / 0.512 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.51 | 0.174 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.68 | 0.211 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.55 | 0.266 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.59 | 0.180 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 60 | 19.02 / 20.40 | 0.302 | 0 | 0.030 / 0.052 |
| M2 | load | ctl.depth_hold | 60 | 19.03 / 20.52 | 0.298 | 0 | - |
| M2 | load | ctl.wrench | 60 | 19.04 / 20.51 | 0.321 | 0 | - |
| M2 | load | thr.cmd | 59 | 19.06 / 20.46 | 0.317 | 0 | - |
| M2 | load | nav.state | 60 | 19.02 / 20.38 | 0.301 | 0 | - |
