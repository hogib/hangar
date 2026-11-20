| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -1.95 / 1.96 | 1.84, 8.7 %, 6.3 s | 3.00, 19.8 %, 10.1 s |
| B2 | residual | M1 | nominal | 0.0 | -1.97 / 1.98 | 1.85, 9.4 %, 6.2 s | 3.02, 19.7 %, 10.3 s |
| B3 | residual | M1 | heavier | 0.0 | -1.92 / 1.92 | 2.72, 16.1 %, 8.6 s | 2.20, 14.1 %, 8.2 s |
| B4 | residual | M2 | heavier | 31.2 | -0.53 / 6.13 | 3.43, 33.2 %, 10.5 s | 2.21, 14.3 %, 8.1 s |
| B5 | assisted | - | heavier | 0.0 | -2.04 / 2.04 | 2.71, 15.9 %, 8.6 s | 2.19, 14.1 %, 8.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.97 | 8.2 | -2.00 / 2.00 |
| T1:back | assisted | buoyancy +4 N | 36.3 | 2.54 | 10.4 | -2.05 / 2.05 |
| H1 | assisted to residual M1 | promotion | 22.4 | 1.46 | 13.6 | -3.85 / 4.19 |
| T2 | residual M1 | buoyancy -4 N | 32.8 | 1.97 | 8.2 | -1.98 / 1.98 |
| S2 | residual M1 to M2 | deploy and swap | 29.4 | 1.18 | 17.4 | -8.99 / 15.23 |
| H1b | assisted to residual M2 | promotion again, after a demotion | 29.4 | 1.67 | 15.4 | -1.59 / 7.19 |
| H2 | residual M2 to assisted | set_mode assisted | 2.2 | 0.41 | 0.0 | -2.00 / 2.01 |
| T3 | assisted | buoyancy +4 N | 36.3 | 2.54 | 10.1 | -1.96 / 1.96 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | -0.0842 | -0.0846 |
| B2 | -0.0850 | -0.0837 | -0.0847 |
| B3 | 0.0885 | -0.0836 | 0.0885 |
| B4 | -0.0161 | 0.0804 | 0.0755 |
| B5 | 0.0886 | 0.0875 | 0.0885 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 12.3 | 5.4 |
| load: checking | 22.0 | 13.4 |
| load: warming | 40.0 | 19.4 |
| swapped (policy.active) | 1019.7 | 999.5 |
| first policy.wrench of the model | 1040.0 | 1019.5 |
| first ctl.wrench carrying it | 151058.4 | 11018.2 |
| first thr.cmd carrying it | 151062.4 | 11022.1 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.05 | 0.140 | 0 | 0.018 / 0.042 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.05 | 0.147 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.25 | 0.154 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.02 / 20.10 | 0.250 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.04 | 0.140 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 60 | 19.01 / 20.29 | 0.136 | 0 | 0.014 / 0.031 |
| M1 | load | ctl.depth_hold | 61 | 19.01 / 20.25 | 0.134 | 0 | - |
| M1 | load | ctl.wrench | 61 | 19.02 / 20.13 | 0.181 | 0 | - |
| M1 | load | thr.cmd | 60 | 19.05 / 20.08 | 0.253 | 0 | - |
| M1 | load | nav.state | 60 | 19.01 / 20.28 | 0.135 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 18.99 / 20.77 | 0.209 | 0 | 0.033 / 0.493 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.58 | 0.206 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.64 | 0.224 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.04 / 20.65 | 0.291 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.79 | 0.203 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 59 | 19.04 / 20.38 | 0.328 | 0 | 0.047 / 0.342 |
| M2 | load | ctl.depth_hold | 60 | 19.02 / 20.57 | 0.380 | 0 | - |
| M2 | load | ctl.wrench | 60 | 19.04 / 20.47 | 0.416 | 0 | - |
| M2 | load | thr.cmd | 59 | 19.05 / 20.49 | 0.394 | 0 | - |
| M2 | load | nav.state | 59 | 19.04 / 20.38 | 0.340 | 0 | - |
