| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.99 / 2.00 | 1.86, 9.6 %, 6.2 s | 3.00, 19.6 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -1.98 / 1.98 | 1.21, 1.1 %, 2.2 s | 1.84, 3.5 %, 2.4 s |
| P1 | residual | M1 | payload | 100.0 | -2.88 / 2.88 | 1.89, 2.1 %, 3.9 s | 2.63, 4.0 %, 4.1 s |
| P2 | residual | M2 | payload | 100.0 | -2.44 / 2.44 | 1.71, 0.1 %, 3.1 s | 2.56, 3.3 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.50 / 2.51 | 1.46, 5.6 %, 2.6 s | 1.78, 3.9 %, 2.2 s |
| P3 | residual | M1 again | payload | 100.0 | -2.04 / 2.04 | 1.90, 3.6 %, 3.6 s | 2.64, 3.8 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.00 / 2.00 | 2.94, 13.6 %, 10.9 s | 5.25, 27.5 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.5 | 0.41 | 0.0 | -2.05 / 2.05 |
| C1 | residual M1 | payload on | 2.9 | 0.52 | 0.0 | -2.50 / 2.51 |
| S2 | residual M1 to M2 | deploy and swap | 3.2 | 0.54 | 0.0 | -2.57 / 2.57 |
| C2 | residual M2 | payload off | 3.2 | 0.56 | 0.0 | -2.75 / 2.76 |
| C3 | residual M2 | payload on | 2.9 | 0.50 | 0.0 | -2.44 / 2.45 |
| S3 | residual M2 to M1 | revert | 2.8 | 0.48 | 0.0 | -2.30 / 2.31 |
| H2 | residual M1 to assisted | set_mode assisted | 10.9 | 0.86 | 7.5 | -1.77 / 1.78 |
| C4 | assisted | payload off | 2.3 | 0.38 | 0.0 | -2.10 / 2.10 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0019 | -0.0847 |
| A2 | -0.0861 | 0.0002 | -0.0859 |
| P1 | -0.0622 | -0.0195 | -0.0851 |
| P2 | -0.0721 | -0.0057 | -0.0795 |
| N2 | -0.0656 | -0.0092 | -0.0769 |
| P3 | -0.0847 | -0.0005 | -0.0855 |
| P4 | -0.0851 | 0.0003 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 15.7 | 15.2 | - |
| load: checking | 476.3 | 479.2 | - |
| load: warming | 479.3 | 480.2 | - |
| swapped (policy.active) | 659.6 | 659.9 | 661396.7 |
| first policy.wrench of the model | 679.6 | 679.9 | 661399.7 |
| first ctl.wrench carrying it | 70698.9 | 699.0 | 661418.7 |
| first thr.cmd carrying it | 70702.3 | 702.9 | 661422.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.50 | 0.225 | 0 | 0.025 / 0.475 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.53 | 0.235 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.46 | 0.219 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.04 / 20.52 | 0.326 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.51 | 0.222 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 42 | 19.03 / 20.32 | 0.179 | 0 | 0.011 / 0.252 |
| M1 | load | ctl.depth_hold | 43 | 19.03 / 20.39 | 0.171 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.03 / 20.29 | 0.191 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.02 / 20.08 | 0.220 | 0 | - |
| M1 | load | nav.state | 42 | 19.03 / 20.32 | 0.172 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.49 | 0.209 | 0 | 0.019 / 0.063 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.55 | 0.213 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.49 | 0.223 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.62 | 0.292 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.48 | 0.208 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 42 | 19.05 / 20.31 | 0.161 | 0 | 0.027 / 0.055 |
| M2 | load | ctl.depth_hold | 43 | 19.05 / 20.73 | 0.280 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.02 / 20.25 | 0.225 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.11 | 0.251 | 0 | - |
| M2 | load | nav.state | 42 | 19.04 / 20.30 | 0.161 | 0 | - |
