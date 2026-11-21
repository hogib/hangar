| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.99 / 2.00 | 1.87, 9.5 %, 6.2 s | 3.03, 19.8 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.01 / 2.01 | 1.23, 0.9 %, 2.3 s | 1.92, 3.8 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -2.77 / 2.77 | 1.85, 2.5 %, 3.7 s | 2.73, 4.3 %, 4.1 s |
| P2 | residual | M2 | payload | 100.0 | -2.21 / 2.21 | 1.63, 0.5 %, 3.0 s | 2.57, 3.5 %, 4.0 s |
| N2 | residual | M2 | bare | 100.0 | -2.51 / 2.52 | 1.46, 7.0 %, 2.7 s | 1.75, 4.4 %, 2.2 s |
| P3 | residual | M1 again | payload | 100.0 | -2.15 / 2.16 | 1.82, 3.4 %, 3.5 s | 2.73, 4.3 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.03 / 2.03 | 2.94, 13.7 %, 10.8 s | 5.24, 27.4 %, 19.4 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.6 | 0.40 | 0.0 | -1.96 / 1.96 |
| C1 | residual M1 | payload on | 2.9 | 0.51 | 0.0 | -2.42 / 2.43 |
| S2 | residual M1 to M2 | deploy and swap | 3.1 | 0.48 | 0.0 | -2.23 / 2.23 |
| C2 | residual M2 | payload off | 3.4 | 0.55 | 0.0 | -2.68 / 2.69 |
| C3 | residual M2 | payload on | 2.8 | 0.49 | 0.0 | -2.38 / 2.39 |
| S3 | residual M2 to M1 | revert | 3.1 | 0.54 | 0.0 | -2.50 / 2.51 |
| H2 | residual M1 to assisted | set_mode assisted | 9.4 | 0.78 | 6.9 | -1.87 / 1.87 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0846 | -0.0011 | -0.0847 |
| A2 | -0.0852 | 0.0007 | -0.0843 |
| P1 | -0.0710 | -0.0115 | -0.0856 |
| P2 | -0.0750 | -0.0063 | -0.0822 |
| N2 | -0.0598 | -0.0205 | -0.0822 |
| P3 | -0.0830 | -0.0023 | -0.0857 |
| P4 | -0.0850 | -0.0011 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 4.0 | 6.6 | - |
| load: checking | 463.3 | 469.4 | - |
| load: warming | 480.3 | 480.4 | - |
| swapped (policy.active) | 660.6 | 659.9 | 661405.7 |
| first policy.wrench of the model | 680.6 | 679.9 | 661419.7 |
| first ctl.wrench carrying it | 70699.2 | 699.5 | 661438.7 |
| first thr.cmd carrying it | 70703.3 | 703.1 | 661443.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.53 | 0.208 | 0 | 0.027 / 0.254 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.41 | 0.170 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.43 | 0.176 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.55 | 0.294 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.53 | 0.206 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.03 / 20.38 | 0.210 | 0 | 0.019 / 0.038 |
| M1 | load | ctl.depth_hold | 43 | 19.02 / 20.91 | 0.279 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.03 / 20.31 | 0.161 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.27 | 0.263 | 0 | - |
| M1 | load | nav.state | 43 | 19.03 / 20.38 | 0.211 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.08 | 0.153 | 0 | 0.019 / 0.047 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.05 | 0.128 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.27 | 0.175 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.14 | 0.245 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.09 | 0.153 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 42 | 19.05 / 20.14 | 0.153 | 0 | 0.029 / 0.043 |
| M2 | load | ctl.depth_hold | 43 | 19.02 / 20.55 | 0.229 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.02 / 20.48 | 0.233 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.50 | 0.289 | 0 | - |
| M2 | load | nav.state | 42 | 19.05 / 20.14 | 0.152 | 0 | - |
