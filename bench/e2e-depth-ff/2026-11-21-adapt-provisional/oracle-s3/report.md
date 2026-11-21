| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -2.04 / 2.05 | 1.86, 9.3 %, 6.3 s | 3.02, 19.8 %, 10.3 s |
| A2 | residual | M1 | bare | 100.0 | -2.04 / 2.04 | 1.22, 0.9 %, 2.3 s | 1.90, 3.5 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -3.26 / 3.27 | 1.90, 1.7 %, 4.0 s | 2.53, 3.1 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.77 / 2.77 | 1.67, 0.2 %, 3.0 s | 2.54, 3.5 %, 4.0 s |
| N2 | residual | M2 | bare | 100.0 | -2.67 / 2.67 | 1.47, 9.8 %, 3.0 s | 1.83, 5.2 %, 2.9 s |
| P3 | residual | M1 again | payload | 100.0 | -2.03 / 2.04 | 1.88, 3.6 %, 3.6 s | 2.54, 3.2 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.00 / 2.00 | 2.93, 13.4 %, 10.8 s | 5.27, 27.4 %, 19.5 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.5 | 0.41 | 0.0 | -2.04 / 2.05 |
| C1 | residual M1 | payload on | 3.2 | 0.52 | 0.0 | -2.35 / 2.35 |
| S2 | residual M1 to M2 | deploy and swap | 3.4 | 0.60 | 0.0 | -3.05 / 3.05 |
| C2 | residual M2 | payload off | 3.6 | 0.61 | 0.0 | -3.05 / 3.06 |
| C3 | residual M2 | payload on | 2.9 | 0.51 | 0.0 | -2.45 / 2.46 |
| S3 | residual M2 to M1 | revert | 2.8 | 0.46 | 0.0 | -2.14 / 2.14 |
| H2 | residual M1 to assisted | set_mode assisted | 13.5 | 0.99 | 8.3 | -1.63 / 1.65 |
| C4 | assisted | payload off | 2.1 | 0.38 | 0.0 | -1.98 / 1.99 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0849 | 0.0027 | -0.0849 |
| A2 | -0.0868 | 0.0011 | -0.0858 |
| P1 | -0.0571 | -0.0234 | -0.0855 |
| P2 | -0.0676 | -0.0151 | -0.0857 |
| N2 | -0.0693 | -0.0116 | -0.0835 |
| P3 | -0.0865 | 0.0012 | -0.0854 |
| P4 | -0.0850 | 0.0017 | -0.0850 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.0 | 0.0 | 0.0 |
| deploy called | 18.3 | 6.0 | - |
| load: checking | 27.2 | 14.6 | - |
| load: warming | 40.3 | 19.6 | - |
| swapped (policy.active) | 219.6 | 199.9 | 660499.8 |
| first policy.wrench of the model | 239.6 | 219.9 | 660519.9 |
| first ctl.wrench carrying it | 70258.2 | 238.9 | 660538.9 |
| first thr.cmd carrying it | 70263.1 | 242.8 | 660543.0 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.06 | 0.165 | 0 | 0.026 / 0.043 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.05 | 0.175 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.07 | 0.181 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.03 / 20.09 | 0.266 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.165 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 20 | 19.02 / 20.60 | 0.265 | 0 | 0.028 / 0.057 |
| M1 | load | ctl.depth_hold | 21 | 19.04 / 20.12 | 0.220 | 0 | - |
| M1 | load | ctl.wrench | 21 | 19.68 / 20.06 | 0.079 | 0 | - |
| M1 | load | thr.cmd | 20 | 19.04 / 20.07 | 0.230 | 0 | - |
| M1 | load | nav.state | 20 | 19.03 / 20.61 | 0.266 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.18 | 0.154 | 0 | 0.021 / 0.050 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.04 | 0.140 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.05 | 0.147 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.02 / 20.08 | 0.254 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.18 | 0.153 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 19 | 20.01 / 20.04 | 0.009 | 0 | 0.024 / 0.043 |
| M2 | load | ctl.depth_hold | 20 | 20.02 / 20.03 | 0.003 | 0 | - |
| M2 | load | ctl.wrench | 20 | 19.02 / 20.04 | 0.225 | 0 | - |
| M2 | load | thr.cmd | 19 | 19.05 / 20.07 | 0.233 | 0 | - |
| M2 | load | nav.state | 19 | 20.02 / 20.03 | 0.004 | 0 | - |
