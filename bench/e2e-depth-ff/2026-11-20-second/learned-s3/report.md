| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.03 / 2.04 | 1.81, 8.8 %, 6.2 s | 3.04, 19.9 %, 10.2 s |
| B2 | residual | M1 | nominal | 100.0 | -1.98 / 1.98 | 1.22, 0.8 %, 2.3 s | 1.82, 3.3 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.73 / 0.74 | 1.34, 0.0 %, 2.6 s | 1.34, 1.5 %, 2.7 s |
| B4 | residual | M2 | heavier | 100.0 | -1.97 / 1.97 | 1.55, 0.0 %, 3.1 s | 1.55, 3.0 %, 2.2 s |
| B5 | assisted | - | heavier | 0.0 | -1.96 / 1.96 | 2.70, 15.9 %, 8.6 s | 2.19, 14.3 %, 8.2 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.6 | 1.97 | 8.2 | -2.04 / 2.04 |
| T1:back | assisted | buoyancy +4 N | 36.4 | 2.53 | 10.1 | -1.99 / 2.00 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.40 | 0.0 | -2.05 / 2.06 |
| T2 | residual M1 | buoyancy -4 N | 2.5 | 0.24 | 0.0 | -0.86 / 0.87 |
| S2 | residual M1 to M2 | deploy and swap | 2.4 | 0.38 | 0.0 | -1.98 / 1.99 |
| H2 | residual M2 to assisted | set_mode assisted | 2.4 | 0.41 | 0.0 | -2.01 / 2.01 |
| T3 | assisted | buoyancy +4 N | 36.3 | 2.54 | 10.2 | -2.02 / 2.02 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0846 | -0.0007 | -0.0847 |
| B2 | -0.0851 | 0.0017 | -0.0831 |
| B3 | 0.0132 | 0.0640 | 0.0825 |
| B4 | 0.0860 | 0.0032 | 0.0894 |
| B5 | 0.0885 | 0.0033 | 0.0888 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.1 |
| deploy called | 2.2 | 12.8 |
| load: checking | 901.9 | 873.7 |
| load: warming | 920.0 | 879.7 |
| swapped (policy.active) | 1100.0 | 1059.8 |
| first policy.wrench of the model | 1120.3 | 1079.8 |
| first ctl.wrench carrying it | 151198.1 | 1098.6 |
| first thr.cmd carrying it | 151204.3 | 1108.8 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 12.14 / 27.65 | 1.343 | 0 | 0.022 / 5.666 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 16.03 / 24.69 | 1.107 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 15.03 / 24.79 | 1.058 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 14.08 / 25.23 | 1.169 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 15.03 / 24.90 | 1.147 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 65 | 14.70 / 25.13 | 1.104 | 0 | 0.027 / 0.368 |
| M1 | load | ctl.depth_hold | 65 | 17.02 / 23.48 | 0.789 | 0 | - |
| M1 | load | ctl.wrench | 65 | 16.03 / 23.55 | 0.800 | 0 | - |
| M1 | load | thr.cmd | 65 | 17.44 / 24.82 | 0.801 | 0 | - |
| M1 | load | nav.state | 65 | 15.02 / 24.80 | 1.050 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 14.03 / 26.21 | 1.391 | 0 | 0.026 / 2.905 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 15.03 / 25.60 | 1.074 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 15.03 / 25.15 | 1.121 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 15.07 / 24.46 | 1.100 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 14.03 / 26.05 | 1.323 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 62 | 16.02 / 23.95 | 1.200 | 0 | 0.027 / 0.053 |
| M2 | load | ctl.depth_hold | 63 | 17.03 / 22.46 | 0.753 | 0 | - |
| M2 | load | ctl.wrench | 63 | 18.03 / 21.48 | 0.422 | 0 | - |
| M2 | load | thr.cmd | 62 | 15.07 / 24.80 | 1.172 | 0 | - |
| M2 | load | nav.state | 62 | 16.03 / 23.92 | 1.195 | 0 | - |
