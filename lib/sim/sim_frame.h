// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
 * sim_frame: a vehicle's thruster geometry as data.
 *
 * A frame file says where each thruster sits and which way a pulse width
 * above 1500 us pushes; the simulator builds its allocation matrix from it
 * and the `thrusters` module inverts the same matrix, so the two cannot
 * disagree about the vehicle (docs/14-simulation.md "Frames"). The text is
 * parsed from a buffer: no I/O here, the caller reads the file.
 *
 *   # comment
 *   [Frame]
 *   Name=bluerov2-heavy
 *
 *   [Thruster 1]
 *   Position=0.156 0.111 0.085     m, body frame (x forward, y right, z down), from the CG
 *   Direction=-1 1 0               which way a pulse width above 1500 us pushes; any length
 *   Reversed=no                    yes: installed the other way round (ArduSub's MOT_n_DIRECTION -1)
 *
 * Thrusters are numbered from 1 with no gaps, at most SIM_NTHRUSTERS; the
 * outputs of a frame with fewer stay at 1500 us and push nothing (their
 * columns of the allocation matrix are zero).
 */
#include "sim_thrust.h"
#include <stddef.h>

struct sim_frame {
  char name[32];
  int n; /* thrusters described, 1 to SIM_NTHRUSTERS */
  struct sim_thruster_geometry geometry;
};

/* Parses `len` bytes of frame text. 0, or -1 with a message in `err` (the
 * line number and what was wrong). Directions are normalized to unit length. */
int sim_frame_parse(const char *text, size_t len, struct sim_frame *out, char *err, size_t errlen);

/* The compiled-in BlueROV2 heavy configuration as a frame (name
 * "bluerov2-heavy", n 8): what a module uses when no file is given. */
void sim_frame_bluerov2_heavy(struct sim_frame *out);
