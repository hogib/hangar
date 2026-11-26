// SPDX-License-Identifier: GPL-3.0-or-later
/* See hd_sched.h. */
#include "hangard.h"

#include "hd_sched.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CAP_IPC_LOCK_N 14
#define CAP_SYS_NICE_N 23

const char *hd_sched_name(int sched)
{
	return sched == MF_SCHED_FIFO ? "fifo" : sched == MF_SCHED_RR ? "rr" : sched == MF_SCHED_OTHER ? "other" : "?";
}

void hd_privs_read(struct hd_privs *p)
{
	memset(p, 0, sizeof *p);
	p->root = geteuid() == 0;
	struct hd_cap_header h = { HD_CAP_VERSION_3, 0 };
	struct hd_cap_data d[2];
	memset(d, 0, sizeof d);
	if (syscall(SYS_capget, &h, d) == 0) {
		p->sys_nice = (d[0].effective >> CAP_SYS_NICE_N) & 1;
		p->ipc_lock = (d[0].effective >> CAP_IPC_LOCK_N) & (d[0].permitted >> CAP_IPC_LOCK_N) & 1;
	}
	struct rlimit rl;
	if (getrlimit(RLIMIT_RTPRIO, &rl) == 0)
		p->rtprio = rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 99 ? 99 : (long)rl.rlim_cur;
	p->nice_floor = 0;
	if (getrlimit(RLIMIT_NICE, &rl) == 0)
		p->nice_floor = rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > 40 ? -20 : 20 - (long)rl.rlim_cur;
	if (p->nice_floor > 0)
		p->nice_floor = 0; /* raising nice is never refused */
	if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0)
		p->memlock_unlimited = rl.rlim_cur == RLIM_INFINITY;
}

int hd_sched_reach(const struct hd_privs *p, int sched, int priority, int nice, char *why, size_t len)
{
	if (len)
		why[0] = 0;
	if (p->root || p->sys_nice)
		return 1;
	if (sched != MF_SCHED_OTHER) {
		if (p->rtprio >= priority)
			return 1;
		snprintf(why, len, "no CAP_SYS_NICE, and RLIMIT_RTPRIO is %ld", p->rtprio);
		return 0;
	}
	if (nice < 0 && nice < p->nice_floor) {
		snprintf(why, len, "no CAP_SYS_NICE, and RLIMIT_NICE allows no nice below %ld", p->nice_floor);
		return 0;
	}
	return 1;
}

enum hd_lock_by hd_lock_reach(const struct hd_privs *p, char *why, size_t len)
{
	if (len)
		why[0] = 0;
	if (p->memlock_unlimited)
		return HD_LOCK_LIMIT;
	if (p->root)
		return HD_LOCK_LIMIT; /* the module's own Capabilities= or limit decides; the library says */
	if (p->ipc_lock)
		return HD_LOCK_CAP;
	snprintf(why, len, "no CAP_IPC_LOCK, and RLIMIT_MEMLOCK is not unlimited");
	return HD_LOCK_NO;
}

int hd_sched_now(pid_t pid, struct hd_sched_now *o)
{
	memset(o, 0, sizeof *o);
	o->locked_kb = -1;
	/* The system calls themselves: musl's sched_getscheduler and
	 * sched_getparam answer ENOSYS always (the kernel's are per thread,
	 * which is what is wanted here: the module's main thread). */
	int pol = (int)syscall(SYS_sched_getscheduler, pid);
	if (pol < 0)
		return -1;
	pol &= ~0x40000000; /* SCHED_RESET_ON_FORK */
	o->sched = pol == SCHED_OTHER ? MF_SCHED_OTHER : pol == SCHED_FIFO ? MF_SCHED_FIFO : pol == SCHED_RR ? MF_SCHED_RR : -1;
	struct sched_param sp = { 0 };
	if (syscall(SYS_sched_getparam, pid, &sp) == 0)
		o->priority = sp.sched_priority;
	errno = 0;
	int n = getpriority(PRIO_PROCESS, (id_t)pid);
	o->nice = errno ? 0 : n;
	char path[64], buf[4096];
	snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t r = read(fd, buf, sizeof buf - 1);
		close(fd);
		if (r > 0) {
			buf[r] = 0;
			const char *v = strstr(buf, "\nVmLck:");
			if (v)
				o->locked_kb = strtol(v + 7, NULL, 10);
		}
	}
	return 0;
}

int hd_sched_short(int sched, int priority, int nice, int lock_memory, const struct hd_sched_now *now, char *out,
		   size_t len)
{
	int s = 0, l = 0;
	if (sched != MF_SCHED_OTHER)
		s = now->sched != sched || now->priority != priority;
	else if (nice < 0)
		s = now->sched != MF_SCHED_OTHER || now->nice > nice;
	if (lock_memory)
		l = now->locked_kb <= 0;
	if (len)
		out[0] = 0;
	if (!s && !l)
		return 0;
	size_t n = 0;
	if (s) {
		char ask[32], has[32];
		if (sched != MF_SCHED_OTHER)
			snprintf(ask, sizeof ask, "%s %d", hd_sched_name(sched), priority);
		else
			snprintf(ask, sizeof ask, "nice %d", nice);
		if (now->sched != MF_SCHED_OTHER)
			snprintf(has, sizeof has, "%s %d", hd_sched_name(now->sched), now->priority);
		else
			snprintf(has, sizeof has, "other, nice %d", now->nice);
		int w = snprintf(out, len, "asks %s, runs %s", ask, has);
		n = w < 0 ? 0 : (size_t)w < len ? (size_t)w : len ? len - 1 : 0;
	}
	if (l && n < len) {
		if (now->locked_kb < 0)
			snprintf(out + n, len - n, "%sasks locked memory, and what is locked cannot be read", s ? "; " : "");
		else
			snprintf(out + n, len - n, "%sasks locked memory, has none locked", s ? "; " : "");
	}
	return 1;
}
