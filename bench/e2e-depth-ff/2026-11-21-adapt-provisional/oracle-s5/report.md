| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.01 / 2.01 | 1.85, 9.4 %, 6.2 s | 3.04, 20.0 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.04 / 2.04 | 1.22, 0.6 %, 2.3 s | 1.92, 3.8 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.23 / 3.23 | 1.87, 1.9 %, 3.9 s | 2.56, 3.5 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.56 / 2.56 | 1.62, 0.6 %, 3.0 s | 2.57, 3.4 %, 4.0 s |
| N2 | residual | M2 | bare | 100.0 | -2.67 / 2.68 | 1.47, 10.7 %, 3.1 s | 1.84, 5.1 %, 2.9 s |
| P3 | residual | M1 again | payload | 100.0 | -1.99 / 1.99 | 1.87, 3.1 %, 3.6 s | 2.56, 3.3 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.01 / 2.01 | 2.94, 13.8 %, 10.8 s | 5.25, 27.4 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.5 | 0.42 | 0.0 | -2.09 / 2.10 |
| C1 | residual M1 | payload on | 2.8 | 0.50 | 0.0 | -2.33 / 2.34 |
| S2 | residual M1 to M2 | deploy and swap | 3.5 | 0.62 | 0.0 | -3.00 / 3.00 |
| C2 | residual M2 | payload off | 3.5 | 0.63 | 0.0 | -3.09 / 3.10 |
| C3 | residual M2 | payload on | 2.9 | 0.51 | 0.0 | -2.45 / 2.45 |
| S3 | residual M2 to M1 | revert | 2.4 | 0.44 | 0.0 | -2.14 / 2.15 |
| H2 | residual M1 to assisted | set_mode assisted | 12.3 | 0.93 | 7.9 | -1.70 / 1.72 |
| C4 | assisted | payload off | 2.1 | 0.38 | 0.0 | -2.00 / 2.00 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0847 | 0.0031 | -0.0849 |
| A2 | -0.0872 | 0.0016 | -0.0858 |
| P1 | -0.0649 | -0.0162 | -0.0857 |
| P2 | -0.0761 | -0.0074 | -0.0856 |
| N2 | -0.0735 | -0.0092 | -0.0852 |
| P3 | -0.0874 | 0.0019 | -0.0855 |
| P4 | -0.0851 | 0.0023 | -0.0851 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.3 | 0.0 |
| deploy called | 1.1 | 19.6 | - |
| load: checking | 9.8 | 28.2 | - |
| load: warming | 19.8 | 40.2 | - |
| swapped (policy.active) | 200.1 | 220.5 | 660484.1 |
| first policy.wrench of the model | 220.2 | 239.5 | 660500.1 |
| first ctl.wrench carrying it | 70239.1 | 258.5 | 660519.1 |
| first thr.cmd carrying it | 70242.4 | 262.7 | 660522.4 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.48 | 0.226 | 0 | 0.018 / 0.456 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.52 | 0.232 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.54 | 0.260 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.04 / 20.42 | 0.293 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.46 | 0.224 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 19 | 20.00 / 20.13 | 0.025 | 0 | 0.017 / 0.044 |
| M1 | load | ctl.depth_hold | 20 | 19.59 / 20.26 | 0.128 | 0 | - |
| M1 | load | ctl.wrench | 20 | 19.03 / 20.28 | 0.231 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.06 / 20.08 | 0.307 | 0 | - |
| M1 | load | nav.state | 19 | 20.01 / 20.12 | 0.022 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.29 | 0.180 | 0 | 0.028 / 0.250 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.57 | 0.217 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.95 | 0.235 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.04 / 20.35 | 0.268 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.30 | 0.178 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 21 | 19.01 / 20.17 | 0.227 | 0 | 0.031 / 0.053 |
| M2 | load | ctl.depth_hold | 20 | 19.03 / 20.05 | 0.224 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.04 / 20.23 | 0.228 | 0 | - |
| M2 | load | thr.cmd | 20 | 19.09 / 20.09 | 0.222 | 0 | - |
| M2 | load | nav.state | 21 | 19.03 / 20.18 | 0.223 | 0 | - |
