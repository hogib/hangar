/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A manifest's [Schedule] against what the kernel will let this process
 * give, and against what a running module has (docs/03 "Starting a
 * module", steps 3 and 4; "What is in force"). Nothing here depends on the
 * manager's state: the command and the suites use it as well. */
#ifndef HANGARD_SCHED_H
#define HANGARD_SCHED_H

#include <stddef.h>
#include <sys/types.h>

#include "manifest.h"

/* What the calling process may hand a child. */
struct hd_privs {
	int root;             /* effective uid 0 */
	int sys_nice;         /* CAP_SYS_NICE effective */
	int ipc_lock;         /* CAP_IPC_LOCK effective and permitted */
	long rtprio;          /* RLIMIT_RTPRIO, the highest priority allowed without the capability */
	long nice_floor;      /* the lowest nice RLIMIT_NICE allows (20 - limit) */
	int memlock_unlimited; /* RLIMIT_MEMLOCK is infinity */
};
void hd_privs_read(struct hd_privs *p);

/* Step 4: may policy, priority and nice be given? 0 with the reason in why. */
int hd_sched_reach(const struct hd_privs *p, int sched, int priority, int nice, char *why, size_t len);

/* LockMemory: how the module will be able to lock. */
enum hd_lock_by { HD_LOCK_NO, HD_LOCK_LIMIT, HD_LOCK_CAP };
enum hd_lock_by hd_lock_reach(const struct hd_privs *p, char *why, size_t len);

/* What a process has now, read from the kernel. */
struct hd_sched_now {
	int sched;            /* enum mf_sched, or -1: neither other, fifo nor rr */
	int priority;
	int nice;
	long locked_kb;       /* VmLck; -1 unknown */
};
int hd_sched_now(pid_t pid, struct hd_sched_now *o);

/* 1 when what was asked is not what the process has; out says both. */
int hd_sched_short(int sched, int priority, int nice, int lock_memory, const struct hd_sched_now *now, char *out,
		   size_t len);
const char *hd_sched_name(int sched);

#endif
