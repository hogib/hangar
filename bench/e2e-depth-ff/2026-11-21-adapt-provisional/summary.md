# 2026-11-21-adapt-provisional

```
tools/sim/e2e-adapt.sh: seeds 1 2 3 4 5; 2 runs at a time; pinned: 1; extra: none
tree: ce24ed5
machine: Linux 7.3.4-arch1-2 x86_64; 12 threads; AMD Ryzen 5 5600H with Radeon Graphics
governor: performance
before learned 1 oracle 1: 2026-11-21T20:26:17+03:00; uptime: 20:26:17 up 1 day, 37 min,  1 user,  load average: 0.81, 0.88, 0.70
before learned 2 oracle 2: 2026-11-21T20:43:52+03:00; uptime: 20:43:52 up 1 day, 54 min,  1 user,  load average: 4.01, 3.47, 2.41
before learned 3 oracle 3: 2026-11-21T21:01:28+03:00; uptime: 21:01:28 up 1 day,  1:12,  1 user,  load average: 1.43, 1.39, 1.66
before learned 4 oracle 4: 2026-11-21T21:19:03+03:00; uptime: 21:19:03 up 1 day,  1:29,  1 user,  load average: 1.15, 1.51, 1.71
before learned 5 oracle 5: 2026-11-21T21:36:37+03:00; uptime: 21:36:37 up 1 day,  1:47,  1 user,  load average: 0.34, 0.42, 0.82
after the last run: 2026-11-21T21:54:11+03:00; uptime: 21:54:11 up 1 day,  2:05,  1 user,  load average: 0.44, 0.49, 0.58
```

## The runs

| Run | Load average, 1 min: at the start, highest, at the end (of 12 threads) | nav.depth to thr.cmd: median, p99 (ms) | 50 Hz intervals over 25 ms, over 30 ms | Recorder lost | Trainer: rows lost, steps | Demotions | Unfit |
|---|---|---|---|---|---|---|---|
| learned-s1 | 0.91, 5.33, 4.02 | 59.58, 60.217 | 5, 0 | 0 | 0, 59466 | 0 | no |
| learned-s2 | 4.09, 4.83, 1.39 | 58.187, 59.014 | 1, 0 | 0 | 0, 59550 | 0 | no |
| learned-s3 | 1.43, 3.75, 1.48 | 59.565, 60.281 | 3, 1 | 0 | 0, 59479 | 0 | no |
| learned-s4 | 1.14, 1.5, 0.44 | 59.395, 60.016 | 0, 0 | 0 | 0, 59575 | 0 | no |
| learned-s5 | 0.47, 1.44, 0.56 | 59.403, 60.107 | 3, 0 | 0 | 0, 59348 | 0 | no |
| oracle-s1 | 1.01, 5.33, 4.01 | 59.472, 60.094 | 3, 0 | 0 | 0, 0 | 0 | no |
| oracle-s2 | 4.42, 4.83, 1.39 | 59.24, 59.984 | 0, 0 | 0 | 0, 0 | 0 | no |
| oracle-s3 | 1.91, 3.75, 1.15 | 59.642, 60.32 | 0, 0 | 0 | 0, 0 | 0 | no |
| oracle-s4 | 1.18, 1.5, 0.34 | 59.671, 60.25 | 0, 0 | 0 | 0, 0 | 0 | no |
| oracle-s5 | 0.59, 1.44, 0.44 | 59.525, 60.181 | 1, 0 | 0 | 0, 0 | 0 | no |

## learned: 5 runs

| Both steps: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 4.89 | 3.01 | 3.15 | 8.18 | 4.45 | 4.32 | 4.44 |
| seed 2 | 4.87 | 3.09 | 3.15 | 8.19 | 4.49 | 4.32 | 4.51 |
| seed 3 | 4.86 | 3.10 | 3.23 | 8.16 | 4.56 | 4.15 | 4.58 |
| seed 4 | 4.88 | 3.06 | 3.26 | 8.18 | 4.54 | 4.20 | 4.55 |
| seed 5 | 4.90 | 3.03 | 3.20 | 8.19 | 4.45 | 4.25 | 4.48 |
| median (smallest to largest) | 4.88 (4.86 to 4.90) | 3.06 (3.01 to 3.10) | 3.20 (3.15 to 3.26) | 8.18 (8.16 to 8.19) | 4.49 (4.45 to 4.56) | 4.25 (4.15 to 4.32) | 4.51 (4.44 to 4.58) |

| Step 3 to 4 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 1.86 | 1.22 | 1.37 | 2.94 | 1.86 | 1.70 | 1.86 |
| seed 2 | 1.85 | 1.24 | 1.37 | 2.94 | 1.86 | 1.71 | 1.90 |
| seed 3 | 1.85 | 1.23 | 1.44 | 2.93 | 1.88 | 1.63 | 1.90 |
| seed 4 | 1.87 | 1.20 | 1.43 | 2.94 | 1.86 | 1.63 | 1.86 |
| seed 5 | 1.86 | 1.22 | 1.43 | 2.93 | 1.88 | 1.70 | 1.91 |
| median (smallest to largest) | 1.86 (1.85 to 1.87) | 1.22 (1.20 to 1.24) | 1.43 (1.37 to 1.44) | 2.94 (2.93 to 2.94) | 1.86 (1.86 to 1.88) | 1.70 (1.63 to 1.71) | 1.90 (1.86 to 1.91) |

| Step 4 to 3 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 3.02 | 1.79 | 1.78 | 5.24 | 2.59 | 2.62 | 2.58 |
| seed 2 | 3.02 | 1.86 | 1.78 | 5.25 | 2.63 | 2.62 | 2.61 |
| seed 3 | 3.00 | 1.87 | 1.79 | 5.24 | 2.68 | 2.52 | 2.68 |
| seed 4 | 3.01 | 1.85 | 1.83 | 5.25 | 2.68 | 2.57 | 2.69 |
| seed 5 | 3.04 | 1.81 | 1.77 | 5.26 | 2.57 | 2.55 | 2.58 |
| median (smallest to largest) | 3.02 (3.00 to 3.04) | 1.85 (1.79 to 1.87) | 1.78 (1.77 to 1.83) | 5.25 (5.24 to 5.26) | 2.63 (2.57 to 2.68) | 2.57 (2.52 to 2.62) | 2.61 (2.58 to 2.69) |

| Step 3 to 4 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 9.4 | 0.5 | 3.4 | 13.5 | 1.1 | 0.0 | 3.7 |
| seed 2 | 9.3 | 0.9 | 4.7 | 13.7 | 2.2 | 0.0 | 4.0 |
| seed 3 | 9.5 | 1.0 | 3.9 | 13.6 | 2.0 | 0.5 | 3.6 |
| seed 4 | 9.2 | 0.7 | 8.1 | 13.6 | 2.1 | 0.7 | 3.0 |
| seed 5 | 8.7 | 0.9 | 8.9 | 13.5 | 1.4 | 0.0 | 3.6 |
| median (smallest to largest) | 9.3 (8.7 to 9.5) | 0.9 (0.5 to 1.0) | 4.7 (3.4 to 8.9) | 13.6 (13.5 to 13.7) | 2.0 (1.1 to 2.2) | 0.0 (0.0 to 0.7) | 3.6 (3.0 to 4.0) |

| Step 4 to 3 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 19.6 | 3.3 | 4.3 | 27.5 | 3.6 | 3.7 | 3.5 |
| seed 2 | 19.7 | 3.5 | 3.5 | 27.5 | 3.9 | 3.6 | 3.7 |
| seed 3 | 19.7 | 3.5 | 4.1 | 27.4 | 4.0 | 3.2 | 4.0 |
| seed 4 | 19.8 | 3.5 | 4.2 | 27.5 | 4.0 | 3.5 | 3.9 |
| seed 5 | 19.8 | 3.2 | 3.4 | 27.6 | 3.4 | 3.2 | 3.4 |
| median (smallest to largest) | 19.7 (19.6 to 19.8) | 3.5 (3.2 to 3.5) | 4.1 (3.4 to 4.3) | 27.5 (27.4 to 27.6) | 3.9 (3.4 to 4.0) | 3.5 (3.2 to 3.7) | 3.7 (3.4 to 4.0) |

| Hold at 3 m: rms (cm) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 1.99 | 2.02 | 2.47 | 2.02 | 2.79 | 2.49 | 2.08 |
| seed 2 | 1.99 | 1.97 | 2.41 | 1.99 | 2.96 | 2.39 | 2.06 |
| seed 3 | 2.00 | 2.06 | 2.65 | 2.03 | 2.85 | 2.36 | 2.18 |
| seed 4 | 2.06 | 1.96 | 2.57 | 1.99 | 2.78 | 2.36 | 2.12 |
| seed 5 | 2.03 | 1.98 | 2.60 | 2.00 | 2.91 | 2.52 | 2.11 |
| median (smallest to largest) | 2.00 (1.99 to 2.06) | 1.98 (1.96 to 2.06) | 2.57 (2.41 to 2.65) | 2.00 (1.99 to 2.03) | 2.85 (2.78 to 2.96) | 2.39 (2.36 to 2.52) | 2.11 (2.06 to 2.18) |

| Heave command: reversals per second | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 0.03 | 0.31 | 1.17 | 0.03 | 0.37 | 0.69 | 0.53 |
| seed 2 | 0.03 | 0.26 | 0.54 | 0.03 | 0.31 | 0.37 | 0.29 |
| seed 3 | 0.03 | 0.20 | 0.86 | 0.03 | 0.09 | 0.74 | 0.14 |
| seed 4 | 0.03 | 0.20 | 0.60 | 0.06 | 0.17 | 0.49 | 0.06 |
| seed 5 | 0.06 | 0.20 | 0.97 | 0.03 | 0.71 | 0.94 | 0.31 |
| median (smallest to largest) | 0.03 (0.03 to 0.06) | 0.20 (0.20 to 0.31) | 0.86 (0.54 to 1.17) | 0.03 (0.03 to 0.06) | 0.31 (0.09 to 0.71) | 0.69 (0.37 to 0.94) | 0.29 (0.06 to 0.53) |

| Policy at its bound (% of ticks) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | 2.9 | 5.1 | - | 5.7 | 8.0 | 5.3 |
| seed 2 | - | 2.7 | 5.1 | - | 5.5 | 8.0 | 5.2 |
| seed 3 | - | 3.3 | 4.5 | - | 5.7 | 7.7 | 5.5 |
| seed 4 | - | 3.4 | 5.5 | - | 5.7 | 7.9 | 5.5 |
| seed 5 | - | 3.0 | 5.3 | - | 5.5 | 7.8 | 4.9 |
| median (smallest to largest) | - | 3.0 (2.7 to 3.4) | 5.1 (4.5 to 5.5) | - | 5.7 (5.5 to 5.7) | 7.9 (7.7 to 8.0) | 5.3 (4.9 to 5.5) |

| At the end of the block's hold: the integrator (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | -0.085 | -0.084 | -0.067 | -0.085 | -0.053 | -0.068 | -0.081 |
| seed 2 | -0.085 | -0.085 | -0.068 | -0.085 | -0.059 | -0.070 | -0.083 |
| seed 3 | -0.085 | -0.090 | -0.061 | -0.085 | -0.067 | -0.076 | -0.088 |
| seed 4 | -0.085 | -0.084 | -0.067 | -0.085 | -0.065 | -0.077 | -0.083 |
| seed 5 | -0.085 | -0.084 | -0.062 | -0.085 | -0.054 | -0.067 | -0.082 |
| median (smallest to largest) | -0.085 (-0.085 to -0.085) | -0.084 (-0.090 to -0.084) | -0.067 (-0.068 to -0.061) | -0.085 (-0.085 to -0.085) | -0.059 (-0.067 to -0.053) | -0.070 (-0.077 to -0.067) | -0.083 (-0.088 to -0.081) |

| At the end of the block's hold: the policy's output (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | -0.001 | -0.009 | - | -0.025 | -0.009 | -0.002 |
| seed 2 | - | -0.001 | -0.010 | - | -0.022 | -0.011 | -0.002 |
| seed 3 | - | 0.006 | -0.019 | - | -0.015 | -0.005 | 0.003 |
| seed 4 | - | -0.001 | -0.014 | - | -0.016 | -0.005 | -0.002 |
| seed 5 | - | -0.002 | -0.018 | - | -0.024 | -0.011 | -0.001 |
| median (smallest to largest) | - | -0.001 (-0.002 to 0.006) | -0.014 (-0.019 to -0.009) | - | -0.022 (-0.025 to -0.015) | -0.009 (-0.011 to -0.005) | -0.002 (-0.002 to 0.003) |

| Transient at the 3 m hold | Peak (cm) | IAE (m s) |
|---|---|---|
| H1: promotion, assisted to residual M1 | 2.39 (2.36 to 2.81) | 0.40 (0.39 to 0.43) |
| C1: payload on, residual M1 | 2.94 (2.89 to 3.16) | 0.51 (0.50 to 0.53) |
| S2: deploy and swap, residual M1 to M2 | 3.22 (3.12 to 3.34) | 0.55 (0.52 to 0.56) |
| C2: payload off, residual M2 | 3.26 (3.08 to 3.39) | 0.56 (0.55 to 0.57) |
| C3: payload on, residual M2 | 2.83 (2.79 to 2.92) | 0.50 (0.49 to 0.51) |
| S3: revert, residual M2 to M1 | 2.72 (2.62 to 2.89) | 0.48 (0.46 to 0.51) |
| H2: set_mode assisted, residual M1 to assisted | 11.37 (9.93 to 12.13) | 0.89 (0.81 to 0.93) |
| C4: payload off, assisted | 2.24 (2.21 to 2.27) | 0.38 (0.38 to 0.38) |

## oracle: 5 runs

M1 is the oracle of the vehicle without the payload, M2 the oracle of the vehicle with it.

| Both steps: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 4.88 | 3.09 | 3.31 | 8.21 | 4.45 | 4.19 | 4.46 |
| seed 2 | 4.91 | 3.12 | 3.30 | 8.20 | 4.44 | 4.20 | 4.45 |
| seed 3 | 4.89 | 3.11 | 3.30 | 8.20 | 4.43 | 4.20 | 4.42 |
| seed 4 | 4.88 | 3.12 | 3.31 | 8.19 | 4.44 | 4.23 | 4.47 |
| seed 5 | 4.90 | 3.14 | 3.31 | 8.19 | 4.43 | 4.20 | 4.42 |
| median (smallest to largest) | 4.89 (4.88 to 4.91) | 3.12 (3.09 to 3.14) | 3.31 (3.30 to 3.31) | 8.20 (8.19 to 8.21) | 4.44 (4.43 to 4.45) | 4.20 (4.19 to 4.23) | 4.45 (4.42 to 4.47) |

| Step 3 to 4 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 1.87 | 1.21 | 1.46 | 2.95 | 1.86 | 1.62 | 1.87 |
| seed 2 | 1.85 | 1.23 | 1.46 | 2.93 | 1.87 | 1.61 | 1.85 |
| seed 3 | 1.86 | 1.22 | 1.47 | 2.93 | 1.90 | 1.67 | 1.88 |
| seed 4 | 1.86 | 1.21 | 1.46 | 2.94 | 1.88 | 1.64 | 1.88 |
| seed 5 | 1.85 | 1.22 | 1.47 | 2.94 | 1.87 | 1.62 | 1.87 |
| median (smallest to largest) | 1.86 (1.85 to 1.87) | 1.22 (1.21 to 1.23) | 1.46 (1.46 to 1.47) | 2.94 (2.93 to 2.95) | 1.87 (1.86 to 1.90) | 1.62 (1.61 to 1.67) | 1.87 (1.85 to 1.88) |

| Step 4 to 3 m: IAE (m s) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 3.02 | 1.88 | 1.85 | 5.27 | 2.58 | 2.57 | 2.59 |
| seed 2 | 3.05 | 1.89 | 1.85 | 5.26 | 2.57 | 2.58 | 2.59 |
| seed 3 | 3.02 | 1.90 | 1.83 | 5.27 | 2.53 | 2.54 | 2.54 |
| seed 4 | 3.02 | 1.91 | 1.86 | 5.26 | 2.56 | 2.59 | 2.59 |
| seed 5 | 3.04 | 1.92 | 1.84 | 5.25 | 2.56 | 2.57 | 2.56 |
| median (smallest to largest) | 3.02 (3.02 to 3.05) | 1.90 (1.88 to 1.92) | 1.85 (1.83 to 1.86) | 5.26 (5.25 to 5.27) | 2.56 (2.53 to 2.58) | 2.57 (2.54 to 2.59) | 2.59 (2.54 to 2.59) |

| Step 3 to 4 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 9.1 | 0.7 | 9.9 | 13.5 | 1.6 | 0.5 | 3.3 |
| seed 2 | 8.9 | 0.9 | 10.2 | 13.4 | 1.8 | 0.3 | 3.3 |
| seed 3 | 9.3 | 0.9 | 9.8 | 13.4 | 1.7 | 0.2 | 3.6 |
| seed 4 | 9.2 | 0.8 | 10.4 | 13.7 | 2.2 | 0.6 | 3.3 |
| seed 5 | 9.4 | 0.6 | 10.7 | 13.8 | 1.9 | 0.6 | 3.1 |
| median (smallest to largest) | 9.2 (8.9 to 9.4) | 0.8 (0.6 to 0.9) | 10.2 (9.8 to 10.7) | 13.5 (13.4 to 13.8) | 1.8 (1.6 to 2.2) | 0.5 (0.2 to 0.6) | 3.3 (3.1 to 3.6) |

| Step 4 to 3 m: overshoot (%) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 19.8 | 3.5 | 5.1 | 27.4 | 3.3 | 3.2 | 3.3 |
| seed 2 | 20.1 | 3.7 | 5.5 | 27.7 | 3.3 | 3.4 | 3.4 |
| seed 3 | 19.8 | 3.5 | 5.2 | 27.4 | 3.1 | 3.5 | 3.2 |
| seed 4 | 19.9 | 3.7 | 5.3 | 27.6 | 3.3 | 3.6 | 3.5 |
| seed 5 | 20.0 | 3.8 | 5.1 | 27.4 | 3.5 | 3.4 | 3.3 |
| median (smallest to largest) | 19.9 (19.8 to 20.1) | 3.7 (3.5 to 3.8) | 5.2 (5.1 to 5.5) | 27.4 (27.4 to 27.7) | 3.3 (3.1 to 3.5) | 3.4 (3.2 to 3.6) | 3.3 (3.2 to 3.5) |

| Hold at 3 m: rms (cm) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 2.03 | 2.07 | 2.66 | 2.00 | 3.21 | 2.59 | 2.08 |
| seed 2 | 1.96 | 2.08 | 2.61 | 2.03 | 3.15 | 2.55 | 2.02 |
| seed 3 | 2.05 | 2.04 | 2.67 | 2.00 | 3.27 | 2.77 | 2.04 |
| seed 4 | 1.97 | 2.08 | 2.69 | 2.02 | 3.13 | 2.56 | 1.99 |
| seed 5 | 2.01 | 2.04 | 2.68 | 2.01 | 3.23 | 2.56 | 1.99 |
| median (smallest to largest) | 2.01 (1.96 to 2.05) | 2.07 (2.04 to 2.08) | 2.67 (2.61 to 2.69) | 2.01 (2.00 to 2.03) | 3.21 (3.13 to 3.27) | 2.56 (2.55 to 2.77) | 2.02 (1.99 to 2.08) |

| Heave command: reversals per second | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | 0.03 | 0.17 | 0.20 | 0.03 | 0.54 | 0.40 | 0.57 |
| seed 2 | 0.03 | 0.11 | 0.11 | 0.03 | 0.57 | 0.34 | 0.60 |
| seed 3 | 0.03 | 0.20 | 0.29 | 0.03 | 0.94 | 0.51 | 1.04 |
| seed 4 | 0.06 | 0.09 | 0.09 | 0.03 | 0.83 | 0.29 | 0.53 |
| seed 5 | 0.03 | 0.09 | 0.11 | 0.03 | 0.63 | 0.54 | 0.43 |
| median (smallest to largest) | 0.03 (0.03 to 0.06) | 0.11 (0.09 to 0.20) | 0.11 (0.09 to 0.29) | 0.03 (0.03 to 0.03) | 0.63 (0.54 to 0.94) | 0.40 (0.29 to 0.54) | 0.57 (0.43 to 1.04) |

| Policy at its bound (% of ticks) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | 2.3 | 6.1 | - | 3.8 | 7.4 | 3.4 |
| seed 2 | - | 2.1 | 6.1 | - | 3.7 | 7.6 | 3.8 |
| seed 3 | - | 2.1 | 6.1 | - | 3.7 | 7.5 | 3.6 |
| seed 4 | - | 2.0 | 6.2 | - | 3.5 | 7.6 | 3.5 |
| seed 5 | - | 2.5 | 6.2 | - | 3.7 | 7.6 | 3.6 |
| median (smallest to largest) | - | 2.1 (2.0 to 2.5) | 6.1 (6.1 to 6.2) | - | 3.7 (3.5 to 3.8) | 7.6 (7.4 to 7.6) | 3.6 (3.4 to 3.8) |

| At the end of the block's hold: the integrator (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | -0.085 | -0.088 | -0.070 | -0.085 | -0.061 | -0.073 | -0.087 |
| seed 2 | -0.085 | -0.087 | -0.073 | -0.085 | -0.064 | -0.076 | -0.087 |
| seed 3 | -0.085 | -0.087 | -0.069 | -0.085 | -0.057 | -0.068 | -0.086 |
| seed 4 | -0.085 | -0.087 | -0.074 | -0.085 | -0.068 | -0.076 | -0.087 |
| seed 5 | -0.085 | -0.087 | -0.073 | -0.085 | -0.065 | -0.076 | -0.087 |
| median (smallest to largest) | -0.085 (-0.085 to -0.085) | -0.087 (-0.088 to -0.087) | -0.073 (-0.074 to -0.069) | -0.085 (-0.085 to -0.085) | -0.064 (-0.068 to -0.057) | -0.076 (-0.076 to -0.068) | -0.087 (-0.087 to -0.086) |

| At the end of the block's hold: the policy's output (heave) | Without the payload: controller alone (A1) | M1 (A2) | M2 (N2) | With the payload: controller alone (P4) | M1 (P1) | M2 (P2) | M1 again (P3) |
|---|---|---|---|---|---|---|---|
| seed 1 | - | 0.002 | -0.012 | - | -0.020 | -0.009 | 0.002 |
| seed 2 | - | 0.002 | -0.010 | - | -0.017 | -0.008 | 0.001 |
| seed 3 | - | 0.001 | -0.012 | - | -0.023 | -0.015 | 0.001 |
| seed 4 | - | 0.002 | -0.010 | - | -0.014 | -0.008 | 0.001 |
| seed 5 | - | 0.002 | -0.009 | - | -0.016 | -0.007 | 0.002 |
| median (smallest to largest) | - | 0.002 (0.001 to 0.002) | -0.010 (-0.012 to -0.009) | - | -0.017 (-0.023 to -0.014) | -0.008 (-0.015 to -0.007) | 0.001 (0.001 to 0.002) |

| Transient at the 3 m hold | Peak (cm) | IAE (m s) |
|---|---|---|
| H1: promotion, assisted to residual M1 | 2.47 (2.40 to 2.64) | 0.42 (0.41 to 0.42) |
| C1: payload on, residual M1 | 3.02 (2.84 to 3.24) | 0.51 (0.50 to 0.52) |
| S2: deploy and swap, residual M1 to M2 | 3.44 (3.40 to 3.51) | 0.61 (0.60 to 0.62) |
| C2: payload off, residual M2 | 3.51 (3.46 to 3.58) | 0.62 (0.61 to 0.63) |
| C3: payload on, residual M2 | 3.01 (2.89 to 3.09) | 0.51 (0.50 to 0.52) |
| S3: revert, residual M2 to M1 | 2.54 (2.42 to 2.80) | 0.45 (0.44 to 0.46) |
| H2: set_mode assisted, residual M1 to assisted | 12.39 (12.12 to 13.47) | 0.94 (0.93 to 0.99) |
| C4: payload off, assisted | 2.14 (2.11 to 2.24) | 0.38 (0.38 to 0.39) |

## The criterion

Adaptation: **shown**.

- The oracle of the vehicle with the payload (P2): J 4.19 to 4.23 m s; the oracle of the vehicle without it (P1, P3): 4.42 to 4.47. Lower than before and after it in the same run in 5 of 5 runs; the ranges do not overlap. The run's own gain (the mean of before and after, less it): 0.23 (0.22 to 0.26) m s.
- M2 (P2): J 4.15 to 4.32 m s; M1 (P1, P3): 4.44 to 4.58. Lower than before and after it in the same run in 5 of 5 runs; the ranges do not overlap. The run's own gain (the mean of before and after, less it): 0.21 (0.13 to 0.41) m s.
- Runs with the machine's marks on them: learned-s3: 1 intervals over 30 ms.
