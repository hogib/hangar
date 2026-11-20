// SPDX-License-Identifier: GPL-3.0-or-later
/* modules/mavlink/mavlink_main.c for the MAVLink harnesses: the link (the
 * serial and UDP receive loops, the transmit queue) as it is, its main() under
 * another name so that the fuzzer's or the regression runner's is the one
 * linked. */
#define main mavlink_prog_main
#include "mavlink_main.c"
