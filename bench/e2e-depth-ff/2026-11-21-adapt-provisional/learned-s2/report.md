| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.99 / 1.99 | 1.85, 9.3 %, 6.2 s | 3.02, 19.7 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -1.96 / 1.97 | 1.24, 0.9 %, 2.3 s | 1.86, 3.5 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -2.96 / 2.96 | 1.86, 2.2 %, 3.9 s | 2.63, 3.9 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.38 / 2.39 | 1.71, 0.0 %, 3.1 s | 2.62, 3.6 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.40 / 2.41 | 1.37, 4.7 %, 1.8 s | 1.78, 3.5 %, 2.2 s |
| P3 | residual | M1 again | payload | 100.0 | -2.06 / 2.06 | 1.90, 4.0 %, 3.6 s | 2.61, 3.7 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -1.99 / 1.99 | 2.94, 13.7 %, 10.8 s | 5.25, 27.5 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.4 | 0.40 | 0.0 | -2.02 / 2.03 |
| C1 | residual M1 | payload on | 3.0 | 0.51 | 0.0 | -2.46 / 2.47 |
| S2 | residual M1 to M2 | deploy and swap | 3.2 | 0.55 | 0.0 | -2.62 / 2.62 |
| C2 | residual M2 | payload off | 3.3 | 0.56 | 0.0 | -2.73 / 2.73 |
| C3 | residual M2 | payload on | 2.8 | 0.49 | 0.0 | -2.37 / 2.37 |
| S3 | residual M2 to M1 | revert | 2.9 | 0.48 | 0.0 | -2.31 / 2.31 |
| H2 | residual M1 to assisted | set_mode assisted | 11.4 | 0.89 | 7.7 | -1.72 / 1.73 |
| C4 | assisted | payload off | 2.3 | 0.38 | 0.0 | -2.06 / 2.07 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0006 | -0.0847 |
| A2 | -0.0847 | -0.0009 | -0.0857 |
| P1 | -0.0587 | -0.0217 | -0.0842 |
| P2 | -0.0699 | -0.0108 | -0.0822 |
| N2 | -0.0677 | -0.0100 | -0.0793 |
| P3 | -0.0829 | -0.0019 | -0.0852 |
| P4 | -0.0850 | -0.0009 | -0.0848 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 8.4 | 1.0 | - |
| load: checking | 459.6 | 474.0 | - |
| load: warming | 479.6 | 480.0 | - |
| swapped (policy.active) | 659.8 | 660.2 | 661402.3 |
| first policy.wrench of the model | 680.4 | 680.3 | 661420.3 |
| first ctl.wrench carrying it | 70698.2 | 698.3 | 661438.3 |
| first thr.cmd carrying it | 70703.1 | 703.6 | 661443.4 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.59 / 20.42 | 0.137 | 0 | 0.025 / 0.446 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.44 | 0.168 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.27 | 0.134 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.05 / 20.33 | 0.252 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.04 | 0.126 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.01 / 20.52 | 0.202 | 0 | 0.026 / 0.059 |
| M1 | load | ctl.depth_hold | 43 | 19.03 / 20.57 | 0.228 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.11 / 20.52 | 0.209 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.07 / 20.08 | 0.260 | 0 | - |
| M1 | load | nav.state | 43 | 19.02 / 20.52 | 0.205 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.08 | 0.148 | 0 | 0.027 / 0.060 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.38 | 0.160 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.34 | 0.166 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.05 / 20.10 | 0.252 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.148 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 43 | 19.02 / 20.36 | 0.220 | 0 | 0.026 / 0.061 |
| M2 | load | ctl.depth_hold | 43 | 19.02 / 20.04 | 0.153 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.23 / 20.04 | 0.123 | 0 | - |
| M2 | load | thr.cmd | 43 | 19.07 / 20.07 | 0.257 | 0 | - |
| M2 | load | nav.state | 43 | 19.02 / 20.36 | 0.220 | 0 | - |
