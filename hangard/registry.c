// SPDX-License-Identifier: GPL-3.0-or-later
/* The registry (docs/03 "Adoption"): text, rewritten atomically on every
 * state change; /proc lookups; users and groups from /etc/passwd and
 * /etc/group read by hand (the release binaries are static). */
#include "hangard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const state_names[ST_COUNT] = { "starting", "ready",   "unhealthy", "backoff",
					    "failed",   "stopped", "refused",   "stopping" };
const char *const sys_state_names[3] = { "nominal", "degraded", "failsafe" };

int state_from_name(const char *s)
{
	for (int i = 0; i < ST_COUNT; i++)
		if (strcmp(state_names[i], s) == 0)
			return i;
	return -1;
}

static ssize_t read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return n;
}

unsigned long long proc_starttime(pid_t pid)
{
	char path[64], buf[2048];
	mf_fmt(path, sizeof path, "/proc/%d/stat", (int)pid);
	if (read_file(path, buf, sizeof buf) <= 0)
		return 0;
	char *p = strrchr(buf, ')'); /* the command name may hold spaces */
	if (!p)
		return 0;
	/* Fields 3..21 follow ") "; field 22 is the start time. */
	for (int field = 2; field < 22 && p; field++)
		p = strchr(p + 1, ' ');
	return p ? strtoull(p + 1, NULL, 10) : 0;
}

int proc_tracer_pid(pid_t pid)
{
	char path[64], buf[4096];
	mf_fmt(path, sizeof path, "/proc/%d/status", (int)pid);
	if (read_file(path, buf, sizeof buf) <= 0)
		return -1;
	const char *t = strstr(buf, "\nTracerPid:");
	if (!t)
		return -1;
	return (int)strtol(t + 11, NULL, 10);
}

void registry_write(void)
{
	FILE *f = fopen(G.paths.registry_tmp, "we");
	if (!f) {
		hd_log("cannot write %s: %s", G.paths.registry_tmp, strerror(errno));
		return;
	}
	fprintf(f, "hangar-registry 2\n");
	fprintf(f, "system: %s %llu %s\n", sys_state_names[G.sys], (unsigned long long)G.sys_since_ns,
		G.sys_reason);
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		fprintf(f, "%s %d %llu %s %u %g %016llx %016llx\n", m->name, (int)m->pid,
			m->pid ? m->starttime : 0ull, state_names[m->st], m->failures,
			(double)m->delay_ns / 1e9, (unsigned long long)m->manifest_hash,
			(unsigned long long)m->wiring_hash);
	}
	if (fclose(f) != 0 || chmod(G.paths.registry_tmp, 0600) != 0 ||
	    rename(G.paths.registry_tmp, G.paths.registry) != 0)
		hd_log("cannot write %s: %s", G.paths.registry, strerror(errno));
	G.registry_written_ns = hg_now_ns();
}

int registry_read(const char *path, struct reg_entry *out, int max, enum sys_state *sys,
		  uint64_t *since, char *reason, size_t reasonlen)
{
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	char line[1024];
	int n = 0, header = 0;
	if (reason && reasonlen)
		reason[0] = 0;
	while (fgets(line, sizeof line, f)) {
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = 0;
		if (!header) {
			header = 1;
			if (strncmp(line, "hangar-registry ", 16) != 0) {
				fclose(f);
				errno = EPROTO;
				return -1;
			}
			continue;
		}
		if (strncmp(line, "system:", 7) == 0) {
			char st[32] = "";
			unsigned long long s = 0;
			int off = 0;
			if (sscanf(line + 7, " %31s %llu %n", st, &s, &off) >= 2) {
				for (int i = 0; i < 3; i++)
					if (strcmp(sys_state_names[i], st) == 0 && sys)
						*sys = (enum sys_state)i;
				if (since)
					*since = s;
				if (reason && reasonlen && off > 0)
					mf_fmt(reason, reasonlen, "%s", line + 7 + off);
			}
			continue;
		}
		if (n == max)
			break;
		struct reg_entry *e = &out[n];
		memset(e, 0, sizeof *e);
		char st[32] = "";
		int pid = 0;
		unsigned long long start = 0, mh = 0, wh = 0;
		unsigned fails = 0;
		double delay = 0;
		int k = sscanf(line, "%31s %d %llu %31s %u %lf %llx %llx", e->name, &pid, &start, st, &fails,
			       &delay, &mh, &wh);
		if (k < 6)
			continue;
		int si = state_from_name(st);
		if (si < 0)
			continue;
		e->pid = (pid_t)pid;
		e->starttime = start;
		e->st = (enum state)si;
		e->failures = fails;
		e->delay_ns = (uint64_t)(delay * 1e9);
		e->manifest_hash = mh;
		e->wiring_hash = wh;
		n++;
	}
	fclose(f);
	return n;
}

/* Reads a colon-separated file and finds the line whose first field is
 * name; fields[] receive pointers into buf. */
static int find_line(const char *file, const char *name, char *buf, size_t len, char **fields, int nf)
{
	FILE *f = fopen(file, "re");
	if (!f)
		return -1;
	while (fgets(buf, (int)len, f)) {
		size_t l = strlen(buf);
		while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
			buf[--l] = 0;
		int k = 0;
		char *p = buf;
		fields[k++] = p;
		while (k < nf && (p = strchr(p, ':'))) {
			*p++ = 0;
			fields[k++] = p;
		}
		if (strcmp(fields[0], name) == 0) {
			fclose(f);
			for (; k < nf; k++)
				fields[k] = (char *)"";
			return 0;
		}
	}
	fclose(f);
	errno = ENOENT;
	return -1;
}

int lookup_user(const char *name, uid_t *uid, gid_t *gid)
{
	char buf[1024], *f[7];
	if (find_line("/etc/passwd", name, buf, sizeof buf, f, 7) < 0)
		return -1;
	*uid = (uid_t)strtoul(f[2], NULL, 10);
	*gid = (gid_t)strtoul(f[3], NULL, 10);
	return 0;
}

int lookup_group(const char *name, gid_t *gid)
{
	char buf[4096], *f[4];
	if (find_line("/etc/group", name, buf, sizeof buf, f, 4) < 0)
		return -1;
	*gid = (gid_t)strtoul(f[2], NULL, 10);
	return 0;
}
