
### learned: 5 runs (seeds 1, 2, 3, 4, 5)

| | assisted, nominal (B1) | M1, nominal (B2) | M1, heavier (B3) | M2, heavier (B4) | assisted, heavier (B5) |
|---|---|---|---|---|---|
| residual in the command (% of ticks) | 0.00 (0.00 to 0.00) | 100.00 (100.00 to 100.00) | 100.00 (100.00 to 100.00) | 100.00 (100.00 to 100.00) | 0.00 (0.00 to 0.00) |
| hold at 3 m: rms (cm) | 2.01 (1.98 to 2.05) | 2.00 (1.98 to 2.03) | 0.78 (0.32 to 1.07) | 1.89 (1.83 to 1.95) | 1.98 (1.94 to 2.01) |
| step 3 to 4 m: IAE (m s) | 1.86 (1.86 to 1.87) | 1.22 (1.21 to 1.26) | 1.31 (1.25 to 1.35) | 1.49 (1.44 to 1.58) | 2.72 (2.69 to 2.73) |
| step 3 to 4 m: overshoot (%) | 9.41 (8.82 to 9.81) | 0.73 (0.53 to 1.40) | 0.00 (0.00 to 0.02) | 0.00 (0.00 to 0.00) | 16.13 (15.86 to 16.21) |
| step 4 to 3 m: IAE (m s) | 3.03 (2.99 to 3.04) | 1.86 (1.81 to 1.90) | 1.42 (1.38 to 1.44) | 1.61 (1.53 to 1.64) | 2.19 (2.17 to 2.20) |
| step 4 to 3 m: overshoot (%) | 19.69 (19.61 to 19.86) | 3.39 (3.33 to 3.61) | 1.84 (1.53 to 1.97) | 3.48 (2.54 to 3.85) | 14.11 (13.87 to 14.37) |
| policy at its bound (% of ticks) | 9.40 (8.88 to 9.77) | 3.00 (2.26 to 3.69) | 2.60 (2.03 to 2.83) | 2.83 (2.71 to 2.89) | 10.57 (9.89 to 10.94) |
| heave command: reversals per second | 0.03 (0.03 to 0.03) | 0.23 (0.09 to 0.34) | 1.11 (0.43 to 1.66) | 0.37 (0.26 to 1.51) | 0.03 (0.03 to 0.06) |
| at the hold's end: the integrator | -0.085 (-0.085 to -0.085) | -0.086 (-0.088 to -0.085) | 0.015 (0.012 to 0.025) | 0.089 (0.086 to 0.092) | 0.088 (0.088 to 0.089) |
| at the hold's end: the policy's output | 0.002 (-0.001 to 0.006) | 0.002 (0.001 to 0.003) | 0.060 (0.055 to 0.069) | -0.003 (-0.004 to 0.002) | -0.004 (-0.005 to -0.000) |

| Transient at the 3 m hold | Peak (cm) | IAE (m s) | Within 5 cm after (s) | Runs still outside at the end |
|---|---|---|---|---|
| T1: buoyancy -4 N, assisted | 32.88 (32.74 to 32.98) | 1.98 (1.97 to 1.98) | 8.24 (8.20 to 8.28) | 0 of 5 |
| T1:back: buoyancy +4 N, assisted | 36.34 (36.18 to 36.52) | 2.53 (2.53 to 2.53) | 10.13 (10.05 to 10.20) | 0 of 5 |
| H1: promotion, assisted to residual M1 | 2.39 (2.33 to 2.56) | 0.41 (0.40 to 0.42) | 0.00 (0.00 to 0.00) | 0 of 5 |
| T2: buoyancy -4 N, residual M1 | 2.71 (2.67 to 2.84) | 0.21 (0.16 to 0.24) | 0.00 (0.00 to 0.00) | 0 of 5 |
| S2: deploy and swap, residual M1 to M2 | 1.99 (1.94 to 2.39) | 0.31 (0.30 to 0.38) | 0.00 (0.00 to 0.00) | 0 of 5 |
| H2: set_mode assisted, residual M2 to assisted | 3.50 (2.91 to 4.10) | 0.46 (0.44 to 0.49) | 0.00 (0.00 to 0.00) | 0 of 5 |
| T3: buoyancy +4 N, assisted | 36.57 (36.49 to 36.74) | 2.54 (2.53 to 2.54) | 10.10 (10.03 to 10.25) | 0 of 5 |

Demotions per run: 0, 0, 0, 0, 0. Rows lost per run: 0, 0, 0, 0, 0. Intervals over 30 ms per run (five 50 Hz topics): 0, 0, 0, 0, 0.
M1, newest row trained on to deploy called (ms): 13.1 (3.3 to 16.2).
M1, newest row trained on to swapped (policy.active) (ms): 660.0 (659.6 to 660.5).
M1, newest row trained on to first thr.cmd carrying it (ms): 150703.1 (150701.9 to 150704.6).
M2, newest row trained on to deploy called (ms): 4.3 (4.0 to 15.9).
M2, newest row trained on to swapped (policy.active) (ms): 660.1 (659.7 to 660.3).
M2, newest row trained on to first thr.cmd carrying it (ms): 702.6 (701.8 to 705.0).
Train/serve: 187843 anchors with an output and a row, 0 with another ctl.depth_hold.target message; outputs against the reference on the rows, largest difference per run: 1.4e-07, 1.6e-07, 1.3e-07, 1.4e-07, 9.7e-08.
Lineage complete per run: True, True, True, True, True; thr.cmd carrying a residual / leading back to a policy.active: 15034/15034, 15034/15034, 15034/15034, 15034/15034, 15034/15034.
Recorder gaps per run: 0, 0, 0, 0, 0. Anchors without a row: 0, 0, 0, 0, 0. Trainer steps: 42874, 42798, 42838, 42882, 42859.

| The run's own difference, residual less assisted | Median (smallest to largest) | Runs better / worse |
|---|---|---|
| M1 nominal, step 3 to 4 m: IAE | -0.64 (-0.65 to -0.60) | 5 / 0 |
| M1 nominal, step 4 to 3 m: IAE | -1.18 (-1.20 to -1.10) | 5 / 0 |
| M1 nominal, hold rms (cm) | 0.00 (-0.03 to 0.02) | 2 / 2 |
| M1 heavier, step 3 to 4 m: IAE | -1.42 (-1.47 to -1.34) | 5 / 0 |
| M1 heavier, step 4 to 3 m: IAE | -0.78 (-0.79 to -0.73) | 5 / 0 |
| M2 heavier, step 3 to 4 m: IAE | -1.23 (-1.28 to -1.15) | 5 / 0 |
| M2 heavier, step 4 to 3 m: IAE | -0.58 (-0.66 to -0.55) | 5 / 0 |
| M2 heavier, hold rms (cm) | -0.08 (-0.12 to -0.05) | 5 / 0 |
| buoyancy -4 N, peak (cm): with M1 (T2) less assisted (T1) | -30.17 (-30.20 to -30.06) | 5 / 0 |
| buoyancy -4 N, IAE (m s): with M1 (T2) less assisted (T1) | -1.77 (-1.81 to -1.74) | 5 / 0 |

### oracle: 5 runs (seeds 1, 2, 3, 4, 5)

| | assisted, nominal (B1) | M1, nominal (B2) | M1, heavier (B3) | M2, heavier (B4) | assisted, heavier (B5) |
|---|---|---|---|---|---|
| residual in the command (% of ticks) | 0.00 (0.00 to 0.00) | 100.00 (100.00 to 100.00) | 100.00 (100.00 to 100.00) | 100.00 (100.00 to 100.00) | 0.00 (0.00 to 0.00) |
| hold at 3 m: rms (cm) | 1.99 (1.94 to 2.01) | 2.03 (1.99 to 2.06) | 1.03 (0.96 to 1.08) | 1.93 (1.87 to 1.98) | 2.03 (1.95 to 2.04) |
| step 3 to 4 m: IAE (m s) | 1.86 (1.85 to 1.88) | 1.22 (1.21 to 1.23) | 1.34 (1.32 to 1.36) | 1.48 (1.46 to 1.49) | 2.72 (2.69 to 2.73) |
| step 3 to 4 m: overshoot (%) | 9.23 (9.04 to 9.63) | 0.89 (0.59 to 1.26) | 0.00 (0.00 to 0.00) | 0.00 (0.00 to 0.00) | 15.97 (15.84 to 16.15) |
| step 4 to 3 m: IAE (m s) | 3.01 (2.99 to 3.03) | 1.89 (1.87 to 1.91) | 1.34 (1.33 to 1.35) | 1.59 (1.56 to 1.61) | 2.19 (2.17 to 2.19) |
| step 4 to 3 m: overshoot (%) | 19.51 (19.41 to 19.86) | 3.54 (3.45 to 3.71) | 1.33 (1.27 to 1.49) | 3.28 (3.12 to 3.53) | 14.10 (13.92 to 14.67) |
| policy at its bound (% of ticks) | 7.06 (5.77 to 7.37) | 2.06 (2.00 to 2.40) | 2.34 (2.29 to 2.57) | 2.71 (2.63 to 2.74) | 8.83 (7.83 to 8.91) |
| heave command: reversals per second | 0.03 (0.03 to 0.06) | 0.11 (0.06 to 0.20) | 1.89 (1.40 to 2.14) | 0.20 (0.11 to 0.26) | 0.03 (0.03 to 0.09) |
| at the hold's end: the integrator | -0.085 (-0.085 to -0.085) | -0.087 (-0.087 to -0.087) | 0.009 (0.008 to 0.013) | 0.088 (0.088 to 0.090) | 0.088 (0.088 to 0.089) |
| at the hold's end: the policy's output | 0.002 (0.001 to 0.003) | 0.002 (0.001 to 0.003) | 0.066 (0.060 to 0.067) | -0.001 (-0.002 to 0.000) | -0.002 (-0.003 to -0.001) |

| Transient at the 3 m hold | Peak (cm) | IAE (m s) | Within 5 cm after (s) | Runs still outside at the end |
|---|---|---|---|---|
| T1: buoyancy -4 N, assisted | 32.86 (32.76 to 32.96) | 1.97 (1.97 to 1.98) | 8.21 (8.19 to 8.22) | 0 of 5 |
| T1:back: buoyancy +4 N, assisted | 36.39 (36.28 to 36.48) | 2.53 (2.52 to 2.53) | 10.14 (10.06 to 10.31) | 0 of 5 |
| H1: promotion, assisted to residual M1 | 2.44 (2.40 to 2.62) | 0.42 (0.42 to 0.42) | 0.00 (0.00 to 0.00) | 0 of 5 |
| T2: buoyancy -4 N, residual M1 | 3.00 (2.76 to 3.28) | 0.26 (0.24 to 0.26) | 0.00 (0.00 to 0.00) | 0 of 5 |
| S2: deploy and swap, residual M1 to M2 | 2.02 (1.88 to 2.10) | 0.32 (0.31 to 0.34) | 0.00 (0.00 to 0.00) | 0 of 5 |
| H2: set_mode assisted, residual M2 to assisted | 3.38 (3.33 to 3.63) | 0.46 (0.46 to 0.47) | 0.00 (0.00 to 0.00) | 0 of 5 |
| T3: buoyancy +4 N, assisted | 36.42 (36.29 to 36.44) | 2.54 (2.53 to 2.54) | 10.09 (9.93 to 10.19) | 0 of 5 |

Demotions per run: 0, 0, 0, 0, 0. Rows lost per run: 0, 0, 0, 0, 0. Intervals over 30 ms per run (five 50 Hz topics): 0, 0, 0, 0, 0.
M1, newest row trained on to deploy called (ms): 8.2 (4.3 to 13.7).
M1, newest row trained on to swapped (policy.active) (ms): 200.0 (199.3 to 220.5).
M1, newest row trained on to first thr.cmd carrying it (ms): 150242.7 (150242.4 to 150262.8).
M2, newest row trained on to deploy called (ms): 3.5 (0.6 to 17.2).
M2, newest row trained on to swapped (policy.active) (ms): 199.9 (199.5 to 220.8).
M2, newest row trained on to first thr.cmd carrying it (ms): 242.9 (242.4 to 263.5).
Train/serve: 187612 anchors with an output and a row, 1 with another ctl.depth_hold.target message; outputs against the reference on the rows, largest difference per run: 5.6e-08, 7.7e-08, 9.2e-08, 6.0e-08, 4.1e-08.
Lineage complete per run: True, True, True, True, True; thr.cmd carrying a residual / leading back to a policy.active: 15011/15011, 15011/15011, 15011/15011, 15011/15011, 15012/15012.
Recorder gaps per run: 0, 0, 0, 0, 0. Anchors without a row: 0, 0, 0, 0, 0. Trainer steps: 0, 0, 0, 0, 0.

| The run's own difference, residual less assisted | Median (smallest to largest) | Runs better / worse |
|---|---|---|
| M1 nominal, step 3 to 4 m: IAE | -0.64 (-0.65 to -0.62) | 5 / 0 |
| M1 nominal, step 4 to 3 m: IAE | -1.12 (-1.13 to -1.10) | 5 / 0 |
| M1 nominal, hold rms (cm) | 0.06 (-0.02 to 0.09) | 1 / 4 |
| M1 heavier, step 3 to 4 m: IAE | -1.37 (-1.41 to -1.36) | 5 / 0 |
| M1 heavier, step 4 to 3 m: IAE | -0.84 (-0.86 to -0.83) | 5 / 0 |
| M2 heavier, step 3 to 4 m: IAE | -1.24 (-1.27 to -1.21) | 5 / 0 |
| M2 heavier, step 4 to 3 m: IAE | -0.59 (-0.61 to -0.58) | 5 / 0 |
| M2 heavier, hold rms (cm) | -0.10 (-0.11 to -0.06) | 5 / 0 |
| buoyancy -4 N, peak (cm): with M1 (T2) less assisted (T1) | -29.92 (-30.10 to -29.48) | 5 / 0 |
| buoyancy -4 N, IAE (m s): with M1 (T2) less assisted (T1) | -1.72 (-1.74 to -1.71) | 5 / 0 |
