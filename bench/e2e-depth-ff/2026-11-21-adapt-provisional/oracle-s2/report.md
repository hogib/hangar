| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.96 / 1.96 | 1.85, 8.9 %, 6.3 s | 3.05, 20.1 %, 10.4 s |
| A2 | residual | M1 | bare | 100.0 | -2.07 / 2.08 | 1.23, 0.9 %, 2.3 s | 1.89, 3.7 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.15 / 3.15 | 1.87, 1.8 %, 3.9 s | 2.57, 3.3 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.55 / 2.55 | 1.61, 0.3 %, 3.0 s | 2.58, 3.4 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.61 / 2.61 | 1.46, 10.2 %, 3.0 s | 1.85, 5.5 %, 3.0 s |
| P3 | residual | M1 again | payload | 100.0 | -2.02 / 2.02 | 1.85, 3.3 %, 3.6 s | 2.59, 3.4 %, 4.3 s |
| P4 | assisted | - | payload | 0.0 | -2.03 / 2.03 | 2.93, 13.4 %, 10.8 s | 5.26, 27.7 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.42 | 0.0 | -2.07 / 2.07 |
| C1 | residual M1 | payload on | 3.2 | 0.52 | 0.0 | -2.43 / 2.44 |
| S2 | residual M1 to M2 | deploy and swap | 3.4 | 0.61 | 0.0 | -3.01 / 3.01 |
| C2 | residual M2 | payload off | 3.5 | 0.62 | 0.0 | -3.06 / 3.06 |
| C3 | residual M2 | payload on | 3.0 | 0.51 | 0.0 | -2.45 / 2.45 |
| S3 | residual M2 to M1 | revert | 2.5 | 0.45 | 0.0 | -2.18 / 2.18 |
| H2 | residual M1 to assisted | set_mode assisted | 12.1 | 0.93 | 8.0 | -1.75 / 1.77 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0847 | 0.0020 | -0.0847 |
| A2 | -0.0871 | 0.0015 | -0.0859 |
| P1 | -0.0635 | -0.0168 | -0.0847 |
| P2 | -0.0756 | -0.0079 | -0.0855 |
| N2 | -0.0729 | -0.0101 | -0.0855 |
| P3 | -0.0869 | 0.0007 | -0.0864 |
| P4 | -0.0850 | 0.0025 | -0.0849 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 12.4 | 19.8 | - |
| load: checking | 21.0 | 29.0 | - |
| load: warming | 40.0 | 40.1 | - |
| swapped (policy.active) | 220.1 | 220.4 | 660495.6 |
| first policy.wrench of the model | 240.1 | 240.4 | 660499.6 |
| first ctl.wrench carrying it | 70258.8 | 258.4 | 660518.7 |
| first thr.cmd carrying it | 70263.7 | 263.3 | 660523.6 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.44 | 0.157 | 0 | 0.013 / 0.253 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.41 | 0.173 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.41 | 0.161 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.55 | 0.272 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.44 | 0.156 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 19.94 / 20.31 | 0.070 | 0 | 0.013 / 0.034 |
| M1 | load | ctl.depth_hold | 21 | 19.96 / 20.30 | 0.064 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.02 / 20.30 | 0.228 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.08 / 20.08 | 0.221 | 0 | - |
| M1 | load | nav.state | 20 | 19.93 / 20.29 | 0.065 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.33 | 0.171 | 0 | 0.031 / 0.043 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.03 / 20.23 | 0.176 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.03 / 20.06 | 0.189 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.07 / 20.08 | 0.258 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.03 / 20.33 | 0.171 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 19.03 / 20.04 | 0.218 | 0 | 0.031 / 0.039 |
| M2 | load | ctl.depth_hold | 20 | 19.04 / 20.04 | 0.223 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.03 / 20.04 | 0.224 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.07 / 20.08 | 0.223 | 0 | - |
| M2 | load | nav.state | 21 | 19.03 / 20.05 | 0.218 | 0 | - |
