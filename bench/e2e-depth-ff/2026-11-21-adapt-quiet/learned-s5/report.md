| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.01 / 2.01 | 1.87, 8.7 %, 6.4 s | 3.03, 20.0 %, 10.4 s |
| A2 | residual | M1 | bare | 100.0 | -1.98 / 1.99 | 1.21, 0.7 %, 2.3 s | 1.82, 3.3 %, 2.4 s |
| P1 | residual | M1 | payload | 100.0 | -2.89 / 2.89 | 1.84, 1.5 %, 3.8 s | 2.59, 3.7 %, 4.1 s |
| P2 | residual | M2 | payload | 100.0 | -2.49 / 2.49 | 1.62, 0.5 %, 3.0 s | 2.63, 3.6 %, 4.0 s |
| N2 | residual | M2 | bare | 100.0 | -2.50 / 2.51 | 1.42, 10.6 %, 3.0 s | 1.84, 3.5 %, 2.3 s |
| P3 | residual | M1 again | payload | 100.0 | -2.02 / 2.02 | 1.86, 3.9 %, 3.5 s | 2.62, 3.6 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -1.99 / 1.99 | 2.93, 13.5 %, 10.8 s | 5.24, 27.6 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.3 | 0.40 | 0.0 | -1.99 / 1.99 |
| C1 | residual M1 | payload on | 3.2 | 0.54 | 0.0 | -2.51 / 2.51 |
| S2 | residual M1 to M2 | deploy and swap | 3.5 | 0.61 | 0.0 | -2.90 / 2.90 |
| C2 | residual M2 | payload off | 3.5 | 0.61 | 0.0 | -2.95 / 2.95 |
| C3 | residual M2 | payload on | 3.0 | 0.51 | 0.0 | -2.41 / 2.41 |
| S3 | residual M2 to M1 | revert | 2.5 | 0.44 | 0.0 | -2.08 / 2.08 |
| H2 | residual M1 to assisted | set_mode assisted | 11.5 | 0.89 | 7.8 | -1.74 / 1.75 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.94 / 1.94 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | -0.0007 | -0.0846 |
| A2 | -0.0840 | -0.0022 | -0.0864 |
| P1 | -0.0566 | -0.0218 | -0.0818 |
| P2 | -0.0761 | -0.0065 | -0.0843 |
| N2 | -0.0746 | -0.0089 | -0.0855 |
| P3 | -0.0838 | -0.0010 | -0.0850 |
| P4 | -0.0850 | -0.0003 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 5.0 | -0.6 | - |
| load: checking | 472.9 | 462.4 | - |
| load: warming | 479.9 | 480.4 | - |
| swapped (policy.active) | 660.1 | 660.7 | 661422.8 |
| first policy.wrench of the model | 680.8 | 680.7 | 661439.8 |
| first ctl.wrench carrying it | 70699.3 | 698.8 | 661458.8 |
| first thr.cmd carrying it | 70702.4 | 703.1 | 661463.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.37 | 0.221 | 0 | 0.023 / 0.050 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.50 | 0.253 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.51 | 0.252 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.36 | 0.287 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.36 | 0.219 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.02 / 20.75 | 0.199 | 0 | 0.012 / 0.034 |
| M1 | load | ctl.depth_hold | 43 | 19.04 / 20.62 | 0.183 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.74 / 20.03 | 0.055 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.07 | 0.260 | 0 | - |
| M1 | load | nav.state | 43 | 19.01 / 20.74 | 0.200 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.40 | 0.215 | 0 | 0.027 / 0.394 |
| M2 | before (10 s) | ctl.depth_hold | 501 | 19.03 / 20.49 | 0.244 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 501 | 19.01 / 20.56 | 0.276 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.56 | 0.293 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.38 | 0.211 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 44 | 19.03 / 20.55 | 0.185 | 0 | 0.019 / 0.037 |
| M2 | load | ctl.depth_hold | 44 | 19.01 / 20.84 | 0.257 | 0 | - |
| M2 | load | ctl.wrench | 44 | 19.01 / 20.56 | 0.229 | 0 | - |
| M2 | load | thr.cmd | 43 | 19.06 / 20.08 | 0.257 | 0 | - |
| M2 | load | nav.state | 44 | 19.02 / 20.56 | 0.187 | 0 | - |
