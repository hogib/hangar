# 2026-11-21-adapt-quiet

```
tools/sim/e2e-adapt.sh: seeds 1 2 3 4 5; 2 runs at a time; pinned: 1; extra: none
tree: 517dade
machine: Linux 7.3.4-arch1-2 x86_64; 12 threads; AMD Ryzen 5 5600H with Radeon Graphics
governor: performance
before learned 1 oracle 1: 2026-11-21T22:08:19+03:00; uptime: 22:08:19 up 1 day,  2:19,  1 user,  load average: 0.15, 0.84, 1.11
before learned 2 oracle 2: 2026-11-21T22:25:53+03:00; uptime: 22:25:53 up 1 day,  2:36,  1 user,  load average: 0.11, 0.26, 0.53
before learned 3 oracle 3: 2026-11-21T22:43:27+03:00; uptime: 22:43:27 up 1 day,  2:54,  1 user,  load average: 0.35, 0.40, 0.44
before learned 4 oracle 4: 2026-11-21T23:01:02+03:00; uptime: 23:01:02 up 1 day,  3:11,  1 user,  load average: 0.10, 0.20, 0.29
before learned 5 oracle 5: 2026-11-21T23:18:36+03:00; uptime: 23:18:36 up 1 day,  3:29,  1 user,  load average: 0.59, 0.46, 0.38
after the last run: 2026-11-21T23:36:11+03:00; uptime: 23:36:11 up 1 day,  3:47,  1 user,  load average: 0.55, 0.43, 0.38
```

## The runs

| Run | Load average, 1 min: at the start, highest, at the end (of 12 threads) | nav.depth to thr.cmd: median, p99 (ms) | 50 Hz intervals over 25 ms, over 30 ms | Recorder lost | Trainer: rows lost, steps | Demotions | Unfit |
|---|---|---|---|---|---|---|---|
| learned-s1 | 0.22, 0.55, 0.14 | 40.703, 61.043 | 0, 0 | 0 | 0, 59474 | 0 | no |
| learned-s2 | 0.18, 0.72, 0.36 | 59.158, 59.922 | 0, 0 | 0 | 0, 59451 | 0 | no |
| learned-s3 | 0.4, 0.53, 0.13 | 59.48, 60.195 | 0, 0 | 0 | 0, 59466 | 0 | no |
| learned-s4 | 0.33, 1.03, 0.55 | 59.651, 60.291 | 0, 0 | 0 | 0, 59446 | 0 | no |
| learned-s5 | 0.7, 0.77, 0.57 | 59.087, 59.841 | 0, 0 | 0 | 0, 59484 | 0 | no |
| oracle-s1 | 0.39, 0.55, 0.11 | 59.533, 60.226 | 0, 0 | 0 | 0, 0 | 0 | no |
| oracle-s2 | 0.44, 0.72, 0.35 | 59.451, 60.146 | 0, 0 | 0 | 0, 0 | 0 | no |
| oracle-s3 | 0.53, 0.53, 0.1 | 59.415, 60.054 | 0, 0 | 0 | 0, 0 | 0 | no |
| oracle-s4 | 0.62, 1.03, 0.59 | 59.286, 60.169 | 0, 0 | 0 | 0, 0 | 0 | no |
| oracle-s5 | 0.77, 0.77, 0.51 | 59.135, 59.933 | 0, 0 | 0 | 0, 0 | 0 | no |

## learned: 5 runs

| Both steps: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 4.89 | 3.10 | 3.19 | 8.19 | 4.54 | 4.31 | 4.56 |
| seed 2 | 4.86 | 3.04 | 3.23 | 8.20 | 4.52 | 4.26 | 4.54 |
| seed 3 | 4.90 | 3.14 | 3.21 | 8.18 | 4.57 | 4.19 | 4.55 |
| seed 4 | 4.86 | 3.10 | 3.23 | 8.20 | 4.57 | 4.19 | 4.58 |
| seed 5 | 4.90 | 3.04 | 3.27 | 8.17 | 4.44 | 4.25 | 4.48 |
| median (smallest to largest) | 4.89 (4.86 to 4.90) | 3.10 (3.04 to 3.14) | 3.23 (3.19 to 3.27) | 8.19 (8.17 to 8.20) | 4.54 (4.44 to 4.57) | 4.25 (4.19 to 4.31) | 4.55 (4.48 to 4.58) |

| Step 3 to 4 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 1.88 | 1.24 | 1.39 | 2.94 | 1.86 | 1.63 | 1.89 |
| seed 2 | 1.86 | 1.21 | 1.46 | 2.94 | 1.89 | 1.71 | 1.90 |
| seed 3 | 1.87 | 1.23 | 1.46 | 2.94 | 1.85 | 1.63 | 1.82 |
| seed 4 | 1.86 | 1.24 | 1.39 | 2.93 | 1.92 | 1.63 | 1.93 |
| seed 5 | 1.87 | 1.21 | 1.42 | 2.93 | 1.84 | 1.62 | 1.86 |
| median (smallest to largest) | 1.87 (1.86 to 1.88) | 1.23 (1.21 to 1.24) | 1.42 (1.39 to 1.46) | 2.94 (2.93 to 2.94) | 1.86 (1.84 to 1.92) | 1.63 (1.62 to 1.71) | 1.89 (1.82 to 1.93) |

| Step 4 to 3 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 3.02 | 1.86 | 1.80 | 5.25 | 2.68 | 2.68 | 2.67 |
| seed 2 | 3.00 | 1.84 | 1.78 | 5.25 | 2.63 | 2.56 | 2.64 |
| seed 3 | 3.03 | 1.92 | 1.75 | 5.24 | 2.73 | 2.57 | 2.73 |
| seed 4 | 3.01 | 1.86 | 1.84 | 5.27 | 2.65 | 2.57 | 2.65 |
| seed 5 | 3.03 | 1.82 | 1.84 | 5.24 | 2.59 | 2.63 | 2.62 |
| median (smallest to largest) | 3.02 (3.00 to 3.03) | 1.86 (1.82 to 1.92) | 1.80 (1.75 to 1.84) | 5.25 (5.24 to 5.27) | 2.65 (2.59 to 2.73) | 2.57 (2.56 to 2.68) | 2.65 (2.62 to 2.73) |

| Step 3 to 4 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 9.4 | 0.7 | 7.2 | 13.5 | 2.3 | 0.4 | 4.0 |
| seed 2 | 9.6 | 1.1 | 5.6 | 13.6 | 2.1 | 0.1 | 3.6 |
| seed 3 | 9.5 | 0.9 | 7.0 | 13.7 | 2.5 | 0.5 | 3.4 |
| seed 4 | 9.0 | 1.2 | 4.3 | 13.5 | 2.8 | 0.8 | 3.8 |
| seed 5 | 8.7 | 0.7 | 10.6 | 13.5 | 1.5 | 0.5 | 3.9 |
| median (smallest to largest) | 9.4 (8.7 to 9.6) | 0.9 (0.7 to 1.2) | 7.0 (4.3 to 10.6) | 13.5 (13.5 to 13.7) | 2.3 (1.5 to 2.8) | 0.5 (0.1 to 0.8) | 3.8 (3.4 to 4.0) |

| Step 4 to 3 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 19.8 | 3.6 | 4.2 | 27.5 | 4.0 | 3.9 | 3.9 |
| seed 2 | 19.6 | 3.5 | 3.9 | 27.5 | 4.0 | 3.3 | 3.8 |
| seed 3 | 19.8 | 3.8 | 4.4 | 27.4 | 4.3 | 3.5 | 4.3 |
| seed 4 | 19.8 | 3.7 | 4.3 | 27.5 | 3.9 | 3.4 | 3.8 |
| seed 5 | 20.0 | 3.3 | 3.5 | 27.6 | 3.7 | 3.6 | 3.6 |
| median (smallest to largest) | 19.8 (19.6 to 20.0) | 3.6 (3.3 to 3.8) | 4.2 (3.5 to 4.4) | 27.5 (27.4 to 27.6) | 4.0 (3.7 to 4.3) | 3.5 (3.3 to 3.9) | 3.8 (3.6 to 4.3) |

| Hold at 3 m: rms (cm) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 1.98 | 2.06 | 2.43 | 2.01 | 2.90 | 2.37 | 2.09 |
| seed 2 | 2.00 | 1.98 | 2.51 | 2.00 | 2.88 | 2.44 | 2.04 |
| seed 3 | 2.00 | 2.01 | 2.52 | 2.03 | 2.77 | 2.21 | 2.16 |
| seed 4 | 2.05 | 1.96 | 2.64 | 1.99 | 2.87 | 2.41 | 2.07 |
| seed 5 | 2.01 | 1.99 | 2.51 | 1.99 | 2.89 | 2.49 | 2.02 |
| median (smallest to largest) | 2.00 (1.98 to 2.05) | 1.99 (1.96 to 2.06) | 2.51 (2.43 to 2.64) | 2.00 (1.99 to 2.03) | 2.88 (2.77 to 2.90) | 2.41 (2.21 to 2.49) | 2.07 (2.02 to 2.16) |

| Heave command: reversals per second | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 0.03 | 0.06 | 0.80 | 0.03 | 0.09 | 0.49 | 0.11 |
| seed 2 | 0.03 | 0.23 | 1.37 | 0.03 | 0.23 | 0.91 | 0.14 |
| seed 3 | 0.03 | 0.17 | 1.00 | 0.03 | 0.03 | 0.66 | 0.11 |
| seed 4 | 0.03 | 0.17 | 0.40 | 0.03 | 0.31 | 0.54 | 0.23 |
| seed 5 | 0.03 | 0.20 | 0.23 | 0.03 | 0.37 | 0.26 | 0.14 |
| median (smallest to largest) | 0.03 (0.03 to 0.03) | 0.17 (0.06 to 0.23) | 0.80 (0.23 to 1.37) | 0.03 (0.03 to 0.03) | 0.23 (0.03 to 0.37) | 0.54 (0.26 to 0.91) | 0.14 (0.11 to 0.23) |

| Policy at its bound (% of ticks) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | 2.7 | 5.7 | - | 5.5 | 7.9 | 5.3 |
| seed 2 | - | 3.1 | 5.5 | - | 5.5 | 7.9 | 5.1 |
| seed 3 | - | 2.6 | 5.9 | - | 5.9 | 7.7 | 5.9 |
| seed 4 | - | 3.0 | 4.9 | - | 5.1 | 7.7 | 5.1 |
| seed 5 | - | 3.0 | 5.2 | - | 5.6 | 7.9 | 5.3 |
| median (smallest to largest) | - | 3.0 (2.6 to 3.1) | 5.5 (4.9 to 5.9) | - | 5.5 (5.1 to 5.9) | 7.9 (7.7 to 7.9) | 5.3 (5.1 to 5.9) |

| At the end of the block's hold: the integrator (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | -0.085 | -0.088 | -0.074 | -0.085 | -0.066 | -0.078 | -0.086 |
| seed 2 | -0.085 | -0.086 | -0.066 | -0.085 | -0.062 | -0.072 | -0.085 |
| seed 3 | -0.085 | -0.085 | -0.060 | -0.085 | -0.071 | -0.075 | -0.083 |
| seed 4 | -0.085 | -0.084 | -0.069 | -0.085 | -0.066 | -0.078 | -0.084 |
| seed 5 | -0.085 | -0.084 | -0.075 | -0.085 | -0.057 | -0.076 | -0.084 |
| median (smallest to largest) | -0.085 (-0.085 to -0.085) | -0.085 (-0.088 to -0.084) | -0.069 (-0.075 to -0.060) | -0.085 (-0.085 to -0.085) | -0.066 (-0.071 to -0.057) | -0.076 (-0.078 to -0.072) | -0.084 (-0.086 to -0.083) |

| At the end of the block's hold: the policy's output (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | 0.003 | -0.004 | - | -0.015 | -0.001 | 0.003 |
| seed 2 | - | 0.000 | -0.009 | - | -0.019 | -0.006 | -0.001 |
| seed 3 | - | 0.001 | -0.021 | - | -0.011 | -0.006 | -0.002 |
| seed 4 | - | -0.002 | -0.013 | - | -0.015 | -0.004 | -0.002 |
| seed 5 | - | -0.002 | -0.009 | - | -0.022 | -0.006 | -0.001 |
| median (smallest to largest) | - | 0.000 (-0.002 to 0.003) | -0.009 (-0.021 to -0.004) | - | -0.015 (-0.022 to -0.011) | -0.006 (-0.006 to -0.001) | -0.001 (-0.002 to 0.003) |

| Transient at the 3 m hold | Peak (cm) | IAE (m s) |
|---|---|---|
| H1: promotion, assisted to residual M1 | 2.46 (2.33 to 2.61) | 0.40 (0.40 to 0.42) |
| C1: payload on, residual M1 | 2.95 (2.86 to 3.18) | 0.51 (0.50 to 0.54) |
| S2: deploy and swap, residual M1 to M2 | 3.34 (3.15 to 3.54) | 0.55 (0.48 to 0.61) |
| C2: payload off, residual M2 | 3.41 (3.22 to 3.53) | 0.57 (0.55 to 0.61) |
| C3: payload on, residual M2 | 2.85 (2.80 to 3.00) | 0.50 (0.49 to 0.51) |
| S3: revert, residual M2 to M1 | 2.69 (2.45 to 3.15) | 0.47 (0.43 to 0.54) |
| H2: set_mode assisted, residual M1 to assisted | 10.93 (9.41 to 11.46) | 0.86 (0.78 to 0.89) |
| C4: payload off, assisted | 2.22 (2.19 to 2.30) | 0.38 (0.38 to 0.38) |

## oracle: 5 runs

M1 is the oracle of the vehicle without the payload, M2 the oracle of the vehicle with it.

| Both steps: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 4.87 | 3.08 | 3.29 | 8.19 | 4.46 | 4.19 | 4.46 |
| seed 2 | 4.88 | 3.11 | 3.32 | 8.18 | 4.43 | 4.22 | 4.45 |
| seed 3 | 4.83 | 3.13 | 3.32 | 8.19 | 4.40 | 4.23 | 4.41 |
| seed 4 | 4.88 | 3.12 | 3.29 | 8.19 | 4.44 | 4.21 | 4.46 |
| seed 5 | 4.92 | 3.15 | 3.33 | 8.20 | 4.43 | 4.21 | 4.45 |
| median (smallest to largest) | 4.88 (4.83 to 4.92) | 3.12 (3.08 to 3.15) | 3.32 (3.29 to 3.33) | 8.19 (8.18 to 8.20) | 4.43 (4.40 to 4.46) | 4.21 (4.19 to 4.23) | 4.45 (4.41 to 4.46) |

| Step 3 to 4 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 1.86 | 1.21 | 1.46 | 2.94 | 1.88 | 1.63 | 1.88 |
| seed 2 | 1.84 | 1.22 | 1.46 | 2.94 | 1.87 | 1.63 | 1.85 |
| seed 3 | 1.84 | 1.23 | 1.47 | 2.93 | 1.89 | 1.67 | 1.86 |
| seed 4 | 1.86 | 1.20 | 1.45 | 2.94 | 1.88 | 1.62 | 1.88 |
| seed 5 | 1.88 | 1.24 | 1.47 | 2.94 | 1.86 | 1.63 | 1.87 |
| median (smallest to largest) | 1.86 (1.84 to 1.88) | 1.22 (1.20 to 1.24) | 1.46 (1.45 to 1.47) | 2.94 (2.93 to 2.94) | 1.88 (1.86 to 1.89) | 1.63 (1.62 to 1.67) | 1.87 (1.85 to 1.88) |

| Step 4 to 3 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 3.01 | 1.87 | 1.83 | 5.26 | 2.58 | 2.56 | 2.58 |
| seed 2 | 3.03 | 1.89 | 1.86 | 5.24 | 2.56 | 2.58 | 2.59 |
| seed 3 | 2.99 | 1.90 | 1.84 | 5.26 | 2.51 | 2.57 | 2.54 |
| seed 4 | 3.01 | 1.92 | 1.85 | 5.25 | 2.55 | 2.58 | 2.58 |
| seed 5 | 3.04 | 1.91 | 1.85 | 5.25 | 2.57 | 2.58 | 2.58 |
| median (smallest to largest) | 3.01 (2.99 to 3.04) | 1.90 (1.87 to 1.92) | 1.85 (1.83 to 1.86) | 5.25 (5.24 to 5.26) | 2.56 (2.51 to 2.58) | 2.58 (2.56 to 2.58) | 2.58 (2.54 to 2.59) |

| Step 3 to 4 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 9.2 | 0.7 | 9.8 | 13.6 | 1.5 | 0.5 | 3.3 |
| seed 2 | 9.1 | 0.8 | 10.3 | 13.5 | 1.8 | 0.6 | 3.2 |
| seed 3 | 8.9 | 1.3 | 9.9 | 13.5 | 1.7 | 0.3 | 3.6 |
| seed 4 | 9.3 | 0.7 | 10.6 | 13.7 | 2.2 | 0.6 | 3.3 |
| seed 5 | 9.4 | 1.2 | 10.8 | 13.8 | 1.9 | 0.6 | 3.2 |
| median (smallest to largest) | 9.2 (8.9 to 9.4) | 0.8 (0.7 to 1.3) | 10.3 (9.8 to 10.8) | 13.6 (13.5 to 13.8) | 1.8 (1.5 to 2.2) | 0.6 (0.3 to 0.6) | 3.3 (3.2 to 3.6) |

| Step 4 to 3 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 19.8 | 3.4 | 5.1 | 27.5 | 3.3 | 3.2 | 3.3 |
| seed 2 | 19.8 | 3.7 | 5.4 | 27.4 | 3.3 | 3.4 | 3.4 |
| seed 3 | 19.6 | 3.5 | 5.3 | 27.4 | 3.1 | 3.5 | 3.2 |
| seed 4 | 19.4 | 3.7 | 5.3 | 27.6 | 3.3 | 3.6 | 3.5 |
| seed 5 | 19.8 | 3.7 | 5.2 | 27.4 | 3.5 | 3.4 | 3.3 |
| median (smallest to largest) | 19.8 (19.4 to 19.8) | 3.7 (3.4 to 3.7) | 5.3 (5.1 to 5.4) | 27.4 (27.4 to 27.6) | 3.3 (3.1 to 3.5) | 3.4 (3.2 to 3.6) | 3.3 (3.2 to 3.5) |

| Hold at 3 m: rms (cm) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 2.03 | 2.06 | 2.66 | 2.01 | 3.21 | 2.58 | 2.08 |
| seed 2 | 2.00 | 2.06 | 2.62 | 1.99 | 3.17 | 2.57 | 2.01 |
| seed 3 | 2.04 | 2.05 | 2.66 | 2.02 | 3.26 | 2.73 | 2.02 |
| seed 4 | 1.97 | 2.08 | 2.67 | 2.02 | 3.13 | 2.54 | 2.00 |
| seed 5 | 2.02 | 2.05 | 2.67 | 2.01 | 3.22 | 2.56 | 1.99 |
| median (smallest to largest) | 2.02 (1.97 to 2.04) | 2.06 (2.05 to 2.08) | 2.66 (2.62 to 2.67) | 2.01 (1.99 to 2.02) | 3.21 (3.13 to 3.26) | 2.57 (2.54 to 2.73) | 2.01 (1.99 to 2.08) |

| Heave command: reversals per second | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 0.03 | 0.06 | 0.23 | 0.03 | 0.49 | 0.49 | 0.60 |
| seed 2 | 0.03 | 0.11 | 0.11 | 0.03 | 0.51 | 0.40 | 0.54 |
| seed 3 | 0.03 | 0.20 | 0.26 | 0.06 | 0.79 | 0.54 | 1.06 |
| seed 4 | 0.03 | 0.06 | 0.09 | 0.03 | 0.66 | 0.31 | 0.57 |
| seed 5 | 0.03 | 0.17 | 0.17 | 0.03 | 0.69 | 0.51 | 0.49 |
| median (smallest to largest) | 0.03 (0.03 to 0.03) | 0.11 (0.06 to 0.20) | 0.17 (0.09 to 0.26) | 0.03 (0.03 to 0.06) | 0.66 (0.49 to 0.79) | 0.49 (0.31 to 0.54) | 0.57 (0.49 to 1.06) |

| Policy at its bound (% of ticks) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | 2.3 | 6.0 | - | 3.8 | 7.4 | 3.5 |
| seed 2 | - | 2.1 | 6.2 | - | 3.7 | 7.6 | 3.8 |
| seed 3 | - | 2.0 | 6.1 | - | 3.8 | 7.5 | 3.5 |
| seed 4 | - | 2.1 | 6.2 | - | 3.5 | 7.6 | 3.5 |
| seed 5 | - | 2.4 | 6.2 | - | 3.7 | 7.5 | 3.8 |
| median (smallest to largest) | - | 2.1 (2.0 to 2.4) | 6.2 (6.0 to 6.2) | - | 3.7 (3.5 to 3.8) | 7.5 (7.4 to 7.6) | 3.5 (3.5 to 3.8) |

| At the end of the block's hold: the integrator (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | -0.085 | -0.088 | -0.070 | -0.085 | -0.060 | -0.073 | -0.087 |
| seed 2 | -0.085 | -0.087 | -0.073 | -0.085 | -0.064 | -0.075 | -0.087 |
| seed 3 | -0.085 | -0.087 | -0.070 | -0.085 | -0.057 | -0.069 | -0.087 |
| seed 4 | -0.085 | -0.087 | -0.074 | -0.085 | -0.068 | -0.076 | -0.087 |
| seed 5 | -0.085 | -0.087 | -0.074 | -0.085 | -0.065 | -0.076 | -0.087 |
| median (smallest to largest) | -0.085 (-0.085 to -0.085) | -0.087 (-0.088 to -0.087) | -0.073 (-0.074 to -0.070) | -0.085 (-0.085 to -0.085) | -0.064 (-0.068 to -0.057) | -0.075 (-0.076 to -0.069) | -0.087 (-0.087 to -0.087) |

| At the end of the block's hold: the policy's output (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | 0.002 | -0.012 | - | -0.021 | -0.009 | 0.002 |
| seed 2 | - | 0.002 | -0.010 | - | -0.017 | -0.008 | 0.002 |
| seed 3 | - | 0.001 | -0.011 | - | -0.023 | -0.013 | 0.001 |
| seed 4 | - | 0.002 | -0.009 | - | -0.014 | -0.007 | 0.001 |
| seed 5 | - | 0.001 | -0.009 | - | -0.016 | -0.007 | 0.002 |
| median (smallest to largest) | - | 0.002 (0.001 to 0.002) | -0.010 (-0.012 to -0.009) | - | -0.017 (-0.023 to -0.014) | -0.008 (-0.013 to -0.007) | 0.002 (0.001 to 0.002) |

| Transient at the 3 m hold | Peak (cm) | IAE (m s) |
|---|---|---|
| H1: promotion, assisted to residual M1 | 2.49 (2.41 to 2.65) | 0.42 (0.41 to 0.42) |
| C1: payload on, residual M1 | 3.03 (2.84 to 3.35) | 0.51 (0.50 to 0.52) |
| S2: deploy and swap, residual M1 to M2 | 3.48 (3.38 to 3.52) | 0.61 (0.60 to 0.62) |
| C2: payload off, residual M2 | 3.51 (3.46 to 3.57) | 0.62 (0.61 to 0.62) |
| C3: payload on, residual M2 | 2.98 (2.88 to 3.08) | 0.51 (0.50 to 0.51) |
| S3: revert, residual M2 to M1 | 2.62 (2.43 to 2.73) | 0.45 (0.44 to 0.46) |
| H2: set_mode assisted, residual M1 to assisted | 12.60 (12.14 to 13.29) | 0.96 (0.93 to 0.99) |
| C4: payload off, assisted | 2.15 (2.11 to 2.23) | 0.38 (0.38 to 0.39) |

## The criterion

Adaptation: **shown**.

- The oracle of the vehicle with the payload (P2): J 4.19 to 4.23 m s; the oracle of the vehicle without it (P1, P3): 4.40 to 4.46. Lower than before and after it in the same run in 5 of 5 runs; the ranges do not overlap. The run's own gain (the mean of before and after, less it): 0.23 (0.17 to 0.27) m s.
- M2 (P2): J 4.19 to 4.31 m s; M1 (P1, P3): 4.44 to 4.58. Lower than before and after it in the same run in 5 of 5 runs; the ranges do not overlap. The run's own gain (the mean of before and after, less it): 0.27 (0.21 to 0.38) m s.
- Runs with the machine's marks on them: none.
