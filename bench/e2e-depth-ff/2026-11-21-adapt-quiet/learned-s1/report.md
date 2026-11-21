| Block | Mode asked | Model | Vehicle | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| A1 | assisted | - | bare | 0.0 | -1.98 / 1.98 | 1.88, 9.4 %, 6.3 s | 3.02, 19.8 %, 10.2 s |
| A2 | residual | M1 | bare | 100.0 | -2.05 / 2.06 | 1.24, 0.7 %, 2.4 s | 1.86, 3.6 %, 2.5 s |
| P1 | residual | M1 | payload | 100.0 | -2.90 / 2.90 | 1.86, 2.3 %, 3.8 s | 2.68, 4.0 %, 4.2 s |
| P2 | residual | M2 | payload | 100.0 | -2.36 / 2.37 | 1.63, 0.4 %, 3.0 s | 2.68, 3.9 %, 4.1 s |
| N2 | residual | M2 | bare | 100.0 | -2.42 / 2.43 | 1.39, 7.2 %, 2.8 s | 1.80, 4.2 %, 2.2 s |
| P3 | residual | M1 again | payload | 100.0 | -2.08 / 2.09 | 1.89, 4.0 %, 3.6 s | 2.67, 3.9 %, 4.2 s |
| P4 | assisted | - | payload | 0.0 | -2.01 / 2.01 | 2.94, 13.5 %, 10.8 s | 5.25, 27.5 %, 19.5 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| H1 | assisted to residual M1 | promotion | 2.6 | 0.42 | 0.0 | -2.06 / 2.06 |
| C1 | residual M1 | payload on | 3.0 | 0.51 | 0.0 | -2.40 / 2.41 |
| S2 | residual M1 to M2 | deploy and swap | 3.3 | 0.55 | 0.0 | -2.65 / 2.65 |
| C2 | residual M2 | payload off | 3.2 | 0.57 | 0.0 | -2.72 / 2.72 |
| C3 | residual M2 | payload on | 2.8 | 0.50 | 0.0 | -2.43 / 2.44 |
| S3 | residual M2 to M1 | revert | 2.7 | 0.47 | 0.0 | -2.27 / 2.28 |
| H2 | residual M1 to assisted | set_mode assisted | 10.3 | 0.83 | 7.2 | -1.80 / 1.81 |
| C4 | assisted | payload off | 2.2 | 0.38 | 0.0 | -2.01 / 2.01 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| A1 | -0.0848 | 0.0046 | -0.0847 |
| A2 | -0.0878 | 0.0029 | -0.0850 |
| P1 | -0.0662 | -0.0152 | -0.0850 |
| P2 | -0.0778 | -0.0012 | -0.0804 |
| N2 | -0.0741 | -0.0040 | -0.0797 |
| P3 | -0.0862 | 0.0027 | -0.0837 |
| P4 | -0.0851 | 0.0034 | -0.0851 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) | ? (v3) |
|---|---|---|---|
| its row published | 0.1 | 0.0 | 0.1 |
| deploy called | 15.7 | 10.1 | - |
| load: checking | 475.5 | 473.4 | - |
| load: warming | 479.5 | 479.4 | - |
| swapped (policy.active) | 659.8 | 660.3 | 661421.7 |
| first policy.wrench of the model | 679.8 | 680.3 | 661439.7 |
| first ctl.wrench carrying it | 70697.4 | 697.5 | 661457.7 |
| first thr.cmd carrying it | 70704.7 | 704.2 | 661464.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 18.97 / 20.44 | 0.181 | 0 | 0.025 / 0.458 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.41 | 0.193 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.46 | 0.209 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.44 | 0.260 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.32 | 0.168 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 43 | 19.01 / 20.46 | 0.242 | 0 | 0.012 / 0.468 |
| M1 | load | ctl.depth_hold | 43 | 19.01 / 20.60 | 0.237 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.02 / 20.39 | 0.178 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.08 | 0.216 | 0 | - |
| M1 | load | nav.state | 43 | 19.02 / 20.30 | 0.220 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.00 / 20.51 | 0.189 | 0 | 0.029 / 0.527 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.60 | 0.239 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.48 | 0.206 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.36 | 0.268 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.37 | 0.176 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 43 | 19.02 / 20.25 | 0.221 | 0 | 0.016 / 0.061 |
| M2 | load | ctl.depth_hold | 43 | 19.03 / 20.31 | 0.226 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.07 / 20.28 | 0.204 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.08 / 20.10 | 0.260 | 0 | - |
| M2 | load | nav.state | 43 | 19.02 / 20.23 | 0.219 | 0 | - |
