| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | 3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |
|---|---|---|---|---|---|---|---|
| B1 | assisted | - | nominal | 0.0 | -2.05 / 2.05 | 1.86, 9.1 %, 6.2 s | 2.99, 19.6 %, 10.1 s |
| B2 | residual | M1 | nominal | 100.0 | -2.03 / 2.03 | 1.21, 0.7 %, 2.4 s | 1.90, 3.6 %, 2.5 s |
| B3 | residual | M1 | heavier | 100.0 | -0.28 / 0.32 | 1.25, 0.0 %, 2.6 s | 1.42, 1.9 %, 2.5 s |
| B4 | residual | M2 | heavier | 100.0 | -1.94 / 1.95 | 1.50, 0.0 %, 2.9 s | 1.64, 3.9 %, 2.3 s |
| B5 | assisted | - | heavier | 0.0 | -1.99 / 2.00 | 2.72, 16.2 %, 8.6 s | 2.19, 14.0 %, 8.0 s |

| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | End: mean / rms (cm) |
|---|---|---|---|---|---|---|
| T1 | assisted | buoyancy -4 N | 32.9 | 1.98 | 8.2 | -1.95 / 1.95 |
| T1:back | assisted | buoyancy +4 N | 36.5 | 2.53 | 10.1 | -2.03 / 2.03 |
| H1 | assisted to residual M1 | promotion | 2.4 | 0.41 | 0.0 | -1.93 / 1.94 |
| T2 | residual M1 | buoyancy -4 N | 2.7 | 0.17 | 0.0 | -0.49 / 0.51 |
| S2 | residual M1 to M2 | deploy and swap | 2.4 | 0.38 | 0.0 | -1.98 / 1.99 |
| H2 | residual M2 to assisted | set_mode assisted | 2.9 | 0.44 | 0.0 | -1.98 / 1.98 |
| T3 | assisted | buoyancy +4 N | 36.7 | 2.54 | 10.0 | -1.99 / 1.99 |

| Block | Integrator | Policy's heave | ctl.wrench heave |
|---|---|---|---|
| B1 | -0.0847 | 0.0002 | -0.0848 |
| B2 | -0.0857 | 0.0005 | -0.0853 |
| B3 | 0.0254 | 0.0546 | 0.0872 |
| B4 | 0.0893 | -0.0015 | 0.0880 |
| B5 | 0.0885 | -0.0001 | 0.0887 |

| After the newest row trained on (ms) | M1 (v1) | M2 (v2) |
|---|---|---|
| its row published | 0.0 | 0.0 |
| deploy called | 15.7 | 15.9 |
| load: checking | 474.6 | 475.9 |
| load: warming | 480.6 | 479.9 |
| swapped (policy.active) | 659.8 | 660.1 |
| first policy.wrench of the model | 679.8 | 680.1 |
| first ctl.wrench carrying it | 150699.0 | 699.1 |
| first thr.cmd carrying it | 150703.3 | 703.4 |

| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | Anchor to output median / max (ms) |
|---|---|---|---|---|---|---|---|
| M1 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.44 | 0.160 | 0 | 0.025 / 0.332 |
| M1 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.48 | 0.167 | 0 | - |
| M1 | before (10 s) | ctl.wrench | 500 | 19.01 / 20.28 | 0.141 | 0 | - |
| M1 | before (10 s) | thr.cmd | 500 | 19.06 / 20.36 | 0.268 | 0 | - |
| M1 | before (10 s) | nav.state | 500 | 19.01 / 20.44 | 0.157 | 0 | - |
| M1 | load | policy.depth_ff.wrench | 42 | 19.01 / 20.46 | 0.173 | 0 | 0.013 / 0.038 |
| M1 | load | ctl.depth_hold | 43 | 19.03 / 20.64 | 0.198 | 0 | - |
| M1 | load | ctl.wrench | 43 | 19.13 / 20.38 | 0.147 | 0 | - |
| M1 | load | thr.cmd | 42 | 19.06 / 20.09 | 0.215 | 0 | - |
| M1 | load | nav.state | 42 | 19.01 / 20.45 | 0.172 | 0 | - |
| M2 | before (10 s) | policy.depth_ff.wrench | 500 | 19.01 / 20.25 | 0.141 | 0 | 0.017 / 0.057 |
| M2 | before (10 s) | ctl.depth_hold | 500 | 19.01 / 20.50 | 0.185 | 0 | - |
| M2 | before (10 s) | ctl.wrench | 500 | 19.02 / 20.53 | 0.155 | 0 | - |
| M2 | before (10 s) | thr.cmd | 500 | 19.06 / 20.09 | 0.245 | 0 | - |
| M2 | before (10 s) | nav.state | 500 | 19.01 / 20.25 | 0.141 | 0 | - |
| M2 | load | policy.depth_ff.wrench | 42 | 19.51 / 20.05 | 0.080 | 0 | 0.014 / 0.043 |
| M2 | load | ctl.depth_hold | 43 | 19.02 / 20.19 | 0.157 | 0 | - |
| M2 | load | ctl.wrench | 43 | 19.03 / 20.04 | 0.152 | 0 | - |
| M2 | load | thr.cmd | 42 | 19.06 / 20.08 | 0.260 | 0 | - |
| M2 | load | nav.state | 42 | 19.48 / 20.04 | 0.084 | 0 | - |
