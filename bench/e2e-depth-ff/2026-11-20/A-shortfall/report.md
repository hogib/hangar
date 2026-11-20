| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.95 / 1.95 | 1.86, 9.1 %, 6.3 s | 3.02, 19.7 %, 10.1 s |
| B2 | residual | M1 | nominal | 2.3 | -3.06 / 3.18 | 1.86, 9.4 %, 6.2 s | 3.02, 19.6 %, 10.3 s |
| B3 | residual | M1 | heavier | 0.0 | -1.93 / 1.93 | 2.72, 16.3 %, 8.6 s | 2.17, 14.0 %, 8.1 s |
| B4 | residual | M2 | heavier | 32.2 | -1.95 / 1.95 | 2.20, 12.0 %, 6.5 s | 2.18, 14.3 %, 8.0 s |
| B5 | assisted | - | heavier | 0.0 | -2.04 / 2.04 | 2.73, 16.1 %, 8.6 s | 2.20, 14.0 %, 8.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 33.0 | 1.97 | 8.2 | -2.00 / 2.01 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.54 | 10.4 | -2.05 / 2.05 |
| H1 | assisted to residual M1 | promotion | 16.2 | 0.53 | - | -0.82 / 4.99 |
| T2 | residual M1 | buoyancy -4 N | 32.8 | 1.97 | 8.2 | -1.97 / 1.97 |
| S2 | residual M1 to M2 | deploy and swap | 2.2 | 0.40 | 0.0 | -1.94 / 1.94 |
| H1b | assisted to residual M2 | promotion again, after a demotion | 2.2 | 0.39 | 0.0 | -1.97 / 1.98 |
| H2 | residual M2 to assisted | set_mode assisted | 2.2 | 0.41 | 0.0 | -2.00 / 2.01 |
| T3 | assisted | buoyancy +4 N | 36.3 | 2.54 | 10.2 | -1.97 / 1.97 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0000 | -0.0846 |
| B2 | -0.0818 | 0.0005 | -0.0844 |
| B3 | 0.0886 | 0.0002 | 0.0885 |
| B4 | 0.0888 | -0.0002 | 0.0885 |
| B5 | 0.0886 | -0.0002 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 7.8 | 22.2 |
| load: checking | 16.3 | 33.0 |
| load: warming | 20.3 | 40.0 |
| swapped (policy.active) | 1000.0 | 1020.8 |
| first policy.wrench of the model | 1020.3 | 1040.8 |
| first ctl.wrench carrying it | 151018.7 | 11038.9 |
| first thr.cmd carrying it | 151022.5 | 11042.9 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.02 / 20.07 | 0.147 | 0 | 0.020 / 0.057 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.04 | 0.147 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.06 | 0.159 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.248 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.147 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 59 | 19.00 / 20.34 | 0.140 | 0 | 0.020 / 0.034 |
| M1 | load | ctl.depth_hold | 60 | 19.03 / 20.25 | 0.131 | 0 | - |
| M1 | load | ctl.wrench | 60 | 19.02 / 20.07 | 0.181 | 0 | - |
| M1 | load | thr.cmd | 59 | 19.06 / 20.07 | 0.253 | 0 | - |
| M1 | load | nav.state | 59 | 19.01 / 20.33 | 0.138 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 18.98 / 20.07 | 0.144 | 0 | 0.025 / 0.075 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.02 / 20.32 | 0.154 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.07 | 0.154 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.248 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.02 / 20.05 | 0.143 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 60 | 19.01 / 20.06 | 0.130 | 0 | 0.020 / 0.051 |
| M2 | load | ctl.depth_hold | 60 | 19.01 / 20.06 | 0.130 | 0 | - |
| M2 | load | ctl.wrench | 60 | 19.02 / 20.06 | 0.181 | 0 | - |
| M2 | load | thr.cmd | 60 | 19.06 / 20.08 | 0.250 | 0 | - |
| M2 | load | nav.state | 60 | 19.02 / 20.03 | 0.129 | 0 | - |
