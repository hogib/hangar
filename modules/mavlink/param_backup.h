// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/*
 * Every parameter of the autopilot read into a file, or a file's set written
 * back: the vehicle module's mavlink.params_dump and mavlink.params_load
 * (modules/mavlink/README.md, "Saving and restoring ArduSub's parameters").
 * What MAVROS's parameter node did for tools/rov/rov.sh, inside hangar, over
 * UDP or a serial line alike.
 *
 * A dump asks for the list (PARAM_REQUEST_LIST), files every PARAM_VALUE by
 * its index, asks again by index for what did not come (PARAM_REQUEST_READ,
 * one each 50 ms, three rounds), and writes `NAME VALUE TYPE` lines. A load
 * reads such a file, dumps first (the values in force, kept beside as
 * <name>.before), sets only the parameters that differ (PARAM_SET, one each
 * 50 ms, with the file's type) and counts each one confirmed when ArduSub's
 * PARAM_VALUE reads it back as the file says (three tries). ArduSub's own
 * bookkeeping (STAT_*, BARO*_GND_PRESS) is never set. One job at a time.
 *
 * The module drives it: pj_tick on every pass of its loop, pj_param_value for
 * every PARAM_VALUE received, the sending through the callbacks.
 */
#include <stddef.h>
#include <stdint.h>

#include "params_job.h" /* msg/params_job.msg */

struct pj_send {
	void (*list)(void);
	void (*read_index)(int index);
	void (*set)(const char *id, float value, uint8_t type);
};

/* dir: where the files are (created 0770 if missing). */
void pj_init(const char *dir, const struct pj_send *send);
const char *pj_dir(void);

/* 0, or -1 with the reason (an invalid name, a job under way, an unreadable
 * file). */
int pj_start_dump(const char *name, uint64_t now, char *reason, size_t len);
int pj_start_load(const char *name, uint64_t now, char *reason, size_t len);
int pj_running(void);
/* Ends a running job as failed (a load while ArduSub arms). */
void pj_abort(const char *why, uint64_t now);

void pj_param_value(const char *id, float value, uint8_t type, uint16_t index, uint16_t count, uint64_t now);
void pj_tick(uint64_t now);

/* The job's state as ardusub.params_job carries it (job 0: none yet, file
 * the directory). */
void pj_current(struct params_job *out);
/* The same, 1 when it changed since the last call (or 500 ms passed while
 * running), so the module publishes; else 0. */
int pj_status(struct params_job *out, uint64_t now);

/* ArduSub's own bookkeeping, never set nor compared: STAT_*, BARO*_GND_PRESS. */
int pj_self_kept(const char *id);
/* Two values the same for that type: exact for an integer type, else within
 * a millionth (floats travel as floats; a value printed with %.9g reads back
 * exactly). */
int pj_same(float a, float b, uint8_t type);
