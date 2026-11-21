| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.05 / 2.05 | 1.86, 9.0 %, 6.3 s | 3.01, 19.8 %, 10.1 s |
| A2 | residual | M1 | bare | 100.0 | -1.96 / 1.96 | 1.24, 1.2 %, 2.3 s | 1.86, 3.7 %, 2.4 s |
| P1 | residual | M1 | payload | 100.0 | -2.87 / 2.87 | 1.92, 2.8 %, 3.8 s | 2.65, 3.9 %, 4.1 s |
| P2 | residual | M2 | payload | 100.0 | -2.40 / 2.41 | 1.63, 0.8 %, 3.1 s | 2.57, 3.4 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.64 / 2.64 | 1.39, 4.3 %, 1.8 s | 1.84, 4.3 %, 2.3 s |
| P3 | residual | M1 again | payload | 100.0 | -2.06 / 2.07 | 1.93, 3.8 %, 3.6 s | 2.65, 3.8 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -1.99 / 1.99 | 2.93, 13.5 %, 10.8 s | 5.27, 27.5 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.40 | 0.0 | -2.00 / 2.01 |
| C1 | residual M1 | payload on | 3.0 | 0.50 | 0.0 | -2.40 / 2.40 |
| S2 | residual M1 to M2 | deploy and swap | 3.5 | 0.57 | 0.0 | -2.73 / 2.74 |
| C2 | residual M2 | payload off | 3.5 | 0.59 | 0.0 | -2.88 / 2.89 |
| C3 | residual M2 | payload on | 2.9 | 0.50 | 0.0 | -2.39 / 2.39 |
| S3 | residual M2 to M1 | revert | 2.6 | 0.43 | 0.0 | -2.09 / 2.10 |
| H2 | residual M1 to assisted | set_mode assisted | 11.2 | 0.87 | 7.5 | -1.81 / 1.82 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.96 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | -0.0018 | -0.0848 |
| A2 | -0.0840 | -0.0015 | -0.0854 |
| P1 | -0.0658 | -0.0149 | -0.0840 |
| P2 | -0.0779 | -0.0036 | -0.0831 |
| N2 | -0.0689 | -0.0126 | -0.0840 |
| P3 | -0.0835 | -0.0017 | -0.0855 |
| P4 | -0.0850 | -0.0015 | -0.0851 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 15.9 | 4.6 | - |
| load: checking | 474.6 | 467.0 | - |
| load: warming | 480.7 | 480.0 | - |
| swapped (policy.active) | 660.1 | 659.3 | 661406.2 |
| first policy.wrench of the model | 680.1 | 679.3 | 661420.2 |
| first ctl.wrench carrying it | 70699.4 | 698.3 | 661439.2 |
| first thr.cmd carrying it | 70702.8 | 702.8 | 661442.9 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.98 / 20.48 | 0.254 | 0 | 0.034 / 0.233 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.48 | 0.266 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.65 | 0.269 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.50 | 0.301 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.03 / 20.44 | 0.251 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 42 | 19.03 / 20.45 | 0.172 | 0 | 0.012 / 0.032 |
| M1 | load | ctl.depth_hold | 43 | 19.71 / 20.03 | 0.052 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.02 / 20.64 | 0.199 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.07 / 20.08 | 0.260 | 0 | - |
| M1 | load | nav.state | 42 | 19.03 / 20.45 | 0.172 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.43 | 0.241 | 0 | 0.030 / 0.111 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.50 | 0.264 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.45 | 0.259 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.53 | 0.322 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.42 | 0.240 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 42 | 19.01 / 20.44 | 0.248 | 0 | 0.018 / 0.059 |
| M2 | load | ctl.depth_hold | 43 | 19.02 / 20.24 | 0.220 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.03 / 20.24 | 0.217 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.44 | 0.272 | 0 | - |
| M2 | load | nav.state | 43 | 19.01 / 20.43 | 0.285 | 0 | - |
