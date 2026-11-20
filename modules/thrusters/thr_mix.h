// SPDX-License-Identifier: GPL-3.0-or-later
/* The thrusters' mixing: a normalized wrench to eight pulse widths, as
 * ArduSub's MANUAL mode mixes for the BlueROV2's vectored frame (heavy): each
 * output the sum of the axis inputs times ArduPilot's motor factors
 * (lib/sim/README.md, "thruster directions"), every output scaled down by the
 * largest when one passes full scale (a heave beside a large surge or yaw
 * arrives smaller), and 1500 +- 400 us. */
#ifndef THR_MIX_H
#define THR_MIX_H

#include <stdint.h>

#define THR_N 8

/* in: surge, sway, heave (up positive), roll, pitch, yaw (clockwise from
 * above positive), each -1 to 1 (clamped; non-finite is 0). pwm: outputs 1
 * to 8. Returns the scale applied, 1 when nothing passed full scale. */
float thr_mix(const float in[6], uint16_t pwm[THR_N]);

#endif
