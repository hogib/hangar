// SPDX-License-Identifier: GPL-3.0-or-later
/* Starting a module (docs/03 "Starting a module"): clone3 with CLONE_PIDFD,
 * the registry entry and the Neutral record written before the child is
 * released, then in the child the ten steps in the documented order. Each
 * privileged step is attempted only when the manager runs as root; otherwise
 * it is skipped with a log line naming the step, so that the suites run as
 * an ordinary user under $HANGAR_ROOT. Also the module page, Neutral
 * commands and the signals to a module's group or cgroup. */
#include "hangard.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* What the child needs, decided in the parent so that the parent can log
 * what it skipped. */
struct child_ctx {
	const struct module *m;
	int release_fd;          /* read until a byte or EOF */
	int err_fd;              /* errno on exec failure */
	int do_cgroup, do_sched, do_user, do_caps, do_rlimit;
	char cgroup[HD_PATH];
	uid_t uid;
	gid_t gid;
	int n_groups;
	gid_t groups[MF_MAX_GROUPS + 1];
	char state_dir[HD_PATH];
	char log_file[HD_PATH];
	int use_journal;
	char *envp[MF_MAX_ENV + 8 + 256];
	char env_buf[MF_MAX_ENV * 256 + 4096];
	uint32_t caps;           /* bit per capability number */
};

static int cap_number(const char *name)
{
	static const struct { const char *n; int v; } caps[] = {
		{ "CAP_CHOWN", 0 },   { "CAP_FOWNER", 3 },    { "CAP_KILL", 5 },      { "CAP_SETGID", 6 },
		{ "CAP_SETUID", 7 },  { "CAP_SETPCAP", 8 },   { "CAP_IPC_LOCK", 14 }, { "CAP_SYS_NICE", 23 },
	};
	for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
		if (strcmp(caps[i].n, name) == 0)
			return caps[i].v;
	return -1;
}

int cgroup_for(const struct module *m, char *out, size_t len)
{
	if (!G.cgroup_base[0])
		return -1;
	mf_fmt(out, len, "%s/%s", G.cgroup_base, m->name);
	return 0;
}

/* ---- The module page ------------------------------------------------------ */

static int page_path(const struct module *m, char *out, size_t len)
{
	int n = mf_fmt(out, len, "%s/%s", G.paths.pages, m->name);
	return n < 0 || (size_t)n >= len ? -1 : 0;
}

static int page_map(struct module *m, int fd)
{
	void *p = mmap(NULL, HD_PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		return -1;
	if (m->page)
		munmap(m->page, HD_PAGE_BYTES);
	if (m->page_fd >= 0 && m->page_fd != fd)
		close(m->page_fd);
	m->page = p;
	m->page_fd = fd;
	return 0;
}

int page_prepare(struct module *m, uint64_t instance)
{
	char path[HD_PATH];
	if (page_path(m, path, sizeof path) < 0)
		return -1;
	int fd = m->page_fd >= 0 ? m->page_fd : open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0640);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st) < 0 || (st.st_size < HD_PAGE_BYTES && ftruncate(fd, HD_PAGE_BYTES) < 0)) {
		if (fd != m->page_fd)
			close(fd);
		return -1;
	}
	if (G.is_root && G.have_gid_hangar)
		(void)fchown(fd, 0, G.gid_hangar);
	(void)fchmod(fd, 0640);
	if (!m->page && page_map(m, fd) < 0) {
		close(fd);
		return -1;
	}
	/* The new instance, the rest cleared: a restarted module is not taken
	 * as ready on the strength of its predecessor. */
	memset(m->page, 0, HD_PAGE_BYTES);
	memcpy(m->page, "HGPAGE01", 8);
	memcpy(m->page + 8, &instance, 8);
	return 0;
}

int page_open_existing(struct module *m)
{
	char path[HD_PATH];
	if (page_path(m, path, sizeof path) < 0)
		return -1;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < HD_PAGE_BYTES || page_map(m, fd) < 0) {
		close(fd);
		return -1;
	}
	return 0;
}

/* ---- Users ---------------------------------------------------------------- */

/* Resolves the module's user, group and supplementary groups. Without
 * root the switch is skipped, so unknown names are not an error then. */
static int resolve_ids(const struct module *m, struct child_ctx *c)
{
	const struct mf_module *mf = m->mf;
	c->uid = getuid();
	c->gid = getgid();
	c->n_groups = 0;
	if (!G.is_root || mf->privileged)
		return 0;
	gid_t ugid;
	if (lookup_user(mf->user, &c->uid, &ugid) < 0) {
		hd_log("%s: no such user: %s", m->name, mf->user);
		return -1;
	}
	c->gid = ugid;
	if (mf->group_set && lookup_group(mf->group, &c->gid) < 0) {
		hd_log("%s: no such group: %s", m->name, mf->group);
		return -1;
	}
	/* Every module is also in hangar, which can read every topic. */
	if (G.have_gid_hangar && G.gid_hangar != c->gid)
		c->groups[c->n_groups++] = G.gid_hangar;
	for (int i = 0; i < mf->n_groups; i++) {
		gid_t g;
		if (lookup_group(mf->groups[i], &g) < 0) {
			hd_log("%s: no such group: %s", m->name, mf->groups[i]);
			return -1;
		}
		c->groups[c->n_groups++] = g;
	}
	return 0;
}

/* ---- Neutral records ------------------------------------------------------ */

static void neutral_path(const char *name, char *out, size_t len)
{
	mf_fmt(out, len, "%s/%s", G.paths.neutral, name);
}

int neutral_record_write(const struct module *m)
{
	const struct mf_module *mf = m->mf;
	if (!mf || !mf->has_neutral)
		return 0;
	const struct mf_exec *x = mf->has_actuator ? &mf->act_neutral : &mf->module_neutral;
	struct child_ctx ids;
	memset(&ids, 0, sizeof ids);
	if (resolve_ids(m, &ids) < 0)
		return -1;
	char path[HD_PATH], tmp[HD_PATH];
	neutral_path(m->name, path, sizeof path);
	mf_fmt(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "we");
	if (!f)
		return -1;
	fprintf(f, "hangar-neutral 1\nuid %lu\ngid %lu\ngroups", (unsigned long)ids.uid, (unsigned long)ids.gid);
	for (int i = 0; i < ids.n_groups; i++)
		fprintf(f, " %lu", (unsigned long)ids.groups[i]);
	fprintf(f, "\nargc %d\n", x->argc);
	for (int i = 0; i < x->argc; i++)
		fprintf(f, "%s\n", x->argv[i]);
	if (fclose(f) != 0 || chmod(tmp, 0600) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

void neutral_record_remove(const char *name)
{
	char path[HD_PATH];
	neutral_path(name, path, sizeof path);
	unlink(path);
}

struct neutral_record {
	uid_t uid;
	gid_t gid;
	int n_groups;
	gid_t groups[MF_MAX_GROUPS + 1];
	int argc;
	char *argv[MF_MAX_ARGS + 1];
	char buf[2048];
};

static int neutral_record_read(const char *name, struct neutral_record *r)
{
	char path[HD_PATH];
	neutral_path(name, path, sizeof path);
	memset(r, 0, sizeof *r);
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	char line[1024];
	size_t used = 0;
	int ok = 0;
	if (fgets(line, sizeof line, f) && strncmp(line, "hangar-neutral 1", 16) == 0 &&
	    fgets(line, sizeof line, f) && sscanf(line, "uid %lu", (unsigned long *)&r->uid) == 1 &&
	    fgets(line, sizeof line, f) && sscanf(line, "gid %lu", (unsigned long *)&r->gid) == 1 &&
	    fgets(line, sizeof line, f) && strncmp(line, "groups", 6) == 0) {
		char *p = line + 6;
		while (*p && r->n_groups <= MF_MAX_GROUPS) {
			char *e;
			unsigned long g = strtoul(p, &e, 10);
			if (e == p)
				break;
			r->groups[r->n_groups++] = (gid_t)g;
			p = e;
		}
		if (fgets(line, sizeof line, f) && sscanf(line, "argc %d", &r->argc) == 1 && r->argc > 0 &&
		    r->argc <= MF_MAX_ARGS) {
			ok = 1;
			for (int i = 0; i < r->argc; i++) {
				if (!fgets(line, sizeof line, f)) {
					ok = 0;
					break;
				}
				size_t n = strlen(line);
				while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
					line[--n] = 0;
				if (used + n + 1 > sizeof r->buf) {
					ok = 0;
					break;
				}
				memcpy(r->buf + used, line, n + 1);
				r->argv[i] = r->buf + used;
				used += n + 1;
			}
		}
	}
	fclose(f);
	if (!ok) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

/* Starts a Neutral command; returns its pid and pidfd. */
static pid_t neutral_start(const char *name, const struct neutral_record *r, int *pidfd)
{
	int pfd = -1;
	struct hd_clone_args ca;
	memset(&ca, 0, sizeof ca);
	ca.flags = HD_CLONE_PIDFD;
	ca.pidfd = (uint64_t)(uintptr_t)&pfd;
	ca.exit_signal = SIGCHLD;
	long pid = syscall(SYS_clone3, &ca, sizeof ca);
	if (pid < 0)
		return -1;
	if (pid == 0) {
		struct sigaction dfl = { .sa_handler = SIG_DFL };
		sigemptyset(&dfl.sa_mask);
		for (int sig = 1; sig < 65; sig++)
			sigaction(sig, &dfl, NULL);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		syscall(SYS_close_range, 3u, ~0u, HD_CLOSE_RANGE_CLOEXEC);
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0)
			dup2(devnull, 0);
		if (G.is_root) {
			if (setgroups((size_t)r->n_groups, r->groups) < 0 || setresgid(r->gid, r->gid, r->gid) < 0 ||
			    setresuid(r->uid, r->uid, r->uid) < 0)
				_exit(126);
		}
		char id[64];
		mf_fmt(id, sizeof id, "HANGAR_MODULE=%s", name);
		putenv(id);
		execv(r->argv[0], r->argv);
		_exit(127);
	}
	*pidfd = pfd;
	return (pid_t)pid;
}

/* Waits for a set of Neutral commands, each within 1 s of now_ns. */
static void neutral_wait(const char *const *names, pid_t *pids, int *pidfds, int n, uint64_t deadline)
{
	for (;;) {
		struct pollfd pf[HD_MAX_MODULES];
		int k = 0;
		for (int i = 0; i < n; i++)
			if (pids[i] > 0) {
				pf[k].fd = pidfds[i];
				pf[k].events = POLLIN;
				k++;
			}
		if (!k)
			return;
		uint64_t now = hg_now_ns();
		int timeout = deadline > now ? (int)((deadline - now + 999999) / 1000000) : 0;
		int r = poll(pf, (nfds_t)k, timeout);
		if (r < 0 && errno == EINTR)
			continue;
		for (int i = 0; i < n; i++) {
			if (pids[i] <= 0)
				continue;
			siginfo_t si;
			memset(&si, 0, sizeof si);
			if (syscall(SYS_waitid, HD_P_PIDFD, pidfds[i], &si, WEXITED | WNOHANG, NULL) == 0 &&
			    si.si_pid == pids[i]) {
				if (si.si_code == CLD_EXITED && si.si_status == 0) {
					hd_log("%s: Neutral command ran", names[i]);
				} else {
					hd_log("%s: Neutral command failed (%s %d): failsafe", names[i],
					       si.si_code == CLD_EXITED ? "exit" : "signal", si.si_status);
					set_sys_state(SYS_FAILSAFE, "Neutral command failed");
				}
				close(pidfds[i]);
				pids[i] = 0;
			}
		}
		if (r == 0 || hg_now_ns() >= deadline) {
			for (int i = 0; i < n; i++)
				if (pids[i] > 0) {
					hd_log("%s: Neutral command did not finish within 1 s: killed, failsafe", names[i]);
					syscall(SYS_pidfd_send_signal, pidfds[i], SIGKILL, NULL, 0u);
					siginfo_t si;
					while (syscall(SYS_waitid, HD_P_PIDFD, pidfds[i], &si, WEXITED, NULL) < 0 &&
					       errno == EINTR)
						;
					close(pidfds[i]);
					pids[i] = 0;
					set_sys_state(SYS_FAILSAFE, "Neutral command timed out");
				}
			return;
		}
	}
}

int neutral_run(const char *name)
{
	struct neutral_record r;
	if (neutral_record_read(name, &r) < 0) {
		if (errno == ENOENT)
			return 1;
		hd_log("%s: Neutral record unreadable: %s: failsafe", name, strerror(errno));
		set_sys_state(SYS_FAILSAFE, "Neutral record missing");
		return -1;
	}
	int pidfd;
	pid_t pid = neutral_start(name, &r, &pidfd);
	if (pid < 0) {
		hd_log("%s: cannot run Neutral command: %s: failsafe", name, strerror(errno));
		set_sys_state(SYS_FAILSAFE, "Neutral command cannot start");
		return -1;
	}
	const char *names[1] = { name };
	enum sys_state before = G.sys;
	neutral_wait(names, &pid, &pidfd, 1, hg_now_ns() + 1000000000ull);
	return G.sys == SYS_FAILSAFE && before != SYS_FAILSAFE ? -1 : 0;
}

void neutral_run_all(const struct reg_entry *dead, int n)
{
	const char *names[HD_MAX_MODULES];
	pid_t pids[HD_MAX_MODULES];
	int pidfds[HD_MAX_MODULES];
	int k = 0;
	for (int i = 0; i < n && k < HD_MAX_MODULES; i++) {
		struct neutral_record r;
		if (neutral_record_read(dead[i].name, &r) < 0) {
			if (errno != ENOENT) {
				hd_log("%s: Neutral record unreadable: %s: failsafe", dead[i].name, strerror(errno));
				set_sys_state(SYS_FAILSAFE, "Neutral record unreadable");
			}
			continue;
		}
		hd_log("%s: died at an unknown time after the registry was last written: running its Neutral command first",
		       dead[i].name);
		names[k] = dead[i].name;
		pids[k] = neutral_start(dead[i].name, &r, &pidfds[k]);
		if (pids[k] < 0) {
			hd_log("%s: cannot run Neutral command: %s: failsafe", dead[i].name, strerror(errno));
			set_sys_state(SYS_FAILSAFE, "Neutral command cannot start");
			continue;
		}
		k++;
	}
	if (k)
		neutral_wait(names, pids, pidfds, k, hg_now_ns() + 1000000000ull);
}

/* ---- Logs ------------------------------------------------------------------ */

/* Connects one stream to journald (the /run/systemd/journal/stdout
 * protocol: identifier, unit, priority, level prefix, three forwards). */
static int journal_stream(const char *identifier, int priority)
{
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return -1;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (strlen(G.paths.journal) >= sizeof sa.sun_path) {
		close(s);
		return -1;
	}
	strcpy(sa.sun_path, G.paths.journal);
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(s);
		return -1;
	}
	shutdown(s, SHUT_RD);
	char hdr[256];
	int n = mf_fmt(hdr, sizeof hdr, "%s\n\n%d\n1\n0\n0\n0\n", identifier, priority);
	ssize_t w = write(s, hdr, (size_t)n);
	if (w != n) {
		close(s);
		return -1;
	}
	return s;
}

/* Cuts the fallback log to its last cap bytes (docs/03 "Logs"): done before
 * the module starts, since the module holds it in append mode afterwards. */
static void cut_log(const char *path, uint64_t cap)
{
	struct stat st;
	if (stat(path, &st) < 0 || (uint64_t)st.st_size <= cap)
		return;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	char *buf = malloc((size_t)cap);
	if (!buf) {
		close(fd);
		return;
	}
	ssize_t n = pread(fd, buf, (size_t)cap, (off_t)((uint64_t)st.st_size - cap));
	close(fd);
	if (n > 0) {
		/* Start at a line boundary. */
		char *nl = memchr(buf, '\n', (size_t)n);
		size_t skip = nl ? (size_t)(nl - buf) + 1 : 0;
		char tmp[HD_PATH];
		mf_fmt(tmp, sizeof tmp, "%s.tmp", path);
		int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
		if (out >= 0) {
			ssize_t w = write(out, buf + skip, (size_t)n - skip);
			close(out);
			if (w >= 0)
				rename(tmp, path);
			else
				unlink(tmp);
		}
	}
	free(buf);
}

/* ---- The child ------------------------------------------------------------- */

static _Noreturn void fail_exec(int err_fd, int step, int e)
{
	int v[2] = { step, e };
	ssize_t w = write(err_fd, v, sizeof v);
	(void)w;
	_exit(127);
}

static _Noreturn void child_main(struct child_ctx *c)
{
	const struct module *m = c->m;
	const struct mf_module *mf = m->mf;
	setpgid(0, 0);
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigemptyset(&dfl.sa_mask);
	for (int sig = 1; sig < 65; sig++)
		sigaction(sig, &dfl, NULL);
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);

	/* 1. Wait until the registry entry and the Neutral record exist. */
	char byte;
	ssize_t r;
	while ((r = read(c->release_fd, &byte, 1)) < 0 && errno == EINTR)
		;
	if (r != 1)
		_exit(0); /* the manager died: no module runs without an entry */
	close(c->release_fd);

	/* 2. The cgroup. */
	if (c->do_cgroup) {
		char path[HD_PATH + 16];
		mf_fmt(path, sizeof path, "%s/cgroup.procs", c->cgroup);
		int fd = open(path, O_WRONLY | O_CLOEXEC);
		if (fd < 0 || write(fd, "0", 1) != 1)
			fail_exec(c->err_fd, 2, errno);
		close(fd);
	}
	/* 3. Resource limits: RLIMIT_MEMLOCK lowered to the default unless the
	 * module locks its memory. */
	if (c->do_rlimit) {
		struct rlimit rl;
		if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
			const rlim_t dflt = 8u << 20;
			if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > dflt)
				rl.rlim_cur = dflt;
			if (rl.rlim_max == RLIM_INFINITY || rl.rlim_max > dflt)
				rl.rlim_max = dflt;
			setrlimit(RLIMIT_MEMLOCK, &rl);
		}
	}
	/* 4. Scheduling, priority, nice, affinity. */
	if (c->do_sched && mf->sched != MF_SCHED_OTHER) {
		struct sched_param sp = { .sched_priority = mf->priority };
		if (sched_setscheduler(0, mf->sched == MF_SCHED_FIFO ? SCHED_FIFO : SCHED_RR, &sp) < 0)
			fail_exec(c->err_fd, 4, errno);
	}
	if (mf->sched == MF_SCHED_OTHER && mf->nice && (c->do_sched || mf->nice > 0))
		if (setpriority(PRIO_PROCESS, 0, mf->nice) < 0)
			fail_exec(c->err_fd, 4, errno);
	if (mf->cpus_set) {
		cpu_set_t set;
		CPU_ZERO(&set);
		for (int i = 0; i < 256; i++)
			if (mf->cpu_mask[i / 64] & ((uint64_t)1 << (i % 64)))
				CPU_SET(i, &set);
		if (sched_setaffinity(0, sizeof set, &set) < 0)
			fail_exec(c->err_fd, 4, errno);
	}
	/* 5. The state directory, as working directory. */
	if (mkdir(c->state_dir, 0750) < 0 && errno != EEXIST)
		fail_exec(c->err_fd, 5, errno);
	if (c->do_user)
		(void)chown(c->state_dir, c->uid, c->gid);
	if (chdir(c->state_dir) < 0)
		fail_exec(c->err_fd, 5, errno);
	/* 6. stdout and stderr: journald, or the fallback file. stdin: /dev/null. */
	int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (devnull >= 0)
		dup2(devnull, 0);
	int out = -1, err = -1;
	if (c->use_journal) {
		out = journal_stream(m->name, 6);
		err = journal_stream(m->name, 3);
	}
	if (out < 0 || err < 0) {
		int lf = open(c->log_file, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
		if (lf < 0)
			fail_exec(c->err_fd, 6, errno);
		if (c->do_user)
			(void)fchown(lf, c->uid, c->gid);
		out = err = lf;
	}
	dup2(out, 1);
	dup2(err, 2);
	/* 7. Capabilities kept across the user switch. */
	if (c->do_caps) {
		struct hd_cap_header h = { HD_CAP_VERSION_3, 0 };
		struct hd_cap_data d[2];
		if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0 || syscall(SYS_capget, &h, d) < 0)
			fail_exec(c->err_fd, 7, errno);
		d[0].inheritable |= c->caps;
		if (syscall(SYS_capset, &h, d) < 0)
			fail_exec(c->err_fd, 7, errno);
	}
	/* 8. Groups, group, user. */
	if (c->do_user) {
		if (setgroups((size_t)c->n_groups, c->groups) < 0 || setresgid(c->gid, c->gid, c->gid) < 0 ||
		    setresuid(c->uid, c->uid, c->uid) < 0)
			fail_exec(c->err_fd, 8, errno);
	}
	/* 9. Each listed capability raised as ambient. */
	if (c->do_caps) {
		for (int cap = 0; cap < 32; cap++)
			if ((c->caps & (1u << cap)) && prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) < 0)
				fail_exec(c->err_fd, 9, errno);
	}
	/* 10. exec, with only the listed descriptors: everything else is
	 * close-on-exec; the wiring is duplicated after the sweep so that it
	 * is not. */
	syscall(SYS_close_range, 3u, ~0u, HD_CLOSE_RANGE_CLOEXEC);
	if (!mf->legacy && m->wiring_fd >= 0) {
		int wfd = dup(m->wiring_fd);
		if (wfd < 0)
			fail_exec(c->err_fd, 10, errno);
		for (int i = 0; c->envp[i]; i++)
			if (strncmp(c->envp[i], "HANGAR_WIRING=", 14) == 0)
				mf_fmt(c->envp[i], 32, "HANGAR_WIRING=%d", wfd);
	}
	umask(0022);
	execve(mf->exec.argv[0], mf->exec.argv, c->envp);
	fail_exec(c->err_fd, 10, errno);
}

/* The child's environment: ours without HANGAR_*, then the module's. */
static void build_env(struct child_ctx *c, const struct module *m)
{
	const struct mf_module *mf = m->mf;
	int n = 0;
	size_t used = 0;
	char *buf = c->env_buf;
	size_t cap = sizeof c->env_buf;
#define ADD(...)                                                                                     \
	do {                                                                                         \
		int w_ = mf_fmt(buf + used, cap - used, __VA_ARGS__);                              \
		if (w_ > 0 && used + (size_t)w_ + 1 < cap && n < (int)(sizeof c->envp / sizeof c->envp[0]) - 1) { \
			c->envp[n++] = buf + used;                                                   \
			used += (size_t)w_ + 1;                                                      \
		}                                                                                    \
	} while (0)
	for (char **e = environ; *e; e++) {
		if (strncmp(*e, "HANGAR_", 7) == 0 || strncmp(*e, "NOTIFY_SOCKET=", 14) == 0 ||
		    strncmp(*e, "LISTEN_", 7) == 0)
			continue;
		ADD("%s", *e);
	}
	ADD("HANGAR_MODULE=%s", m->name);
	ADD("HANGAR_TOPIC_DIR=%s", G.conf.topic_dir);
	if (!mf->legacy) {
		ADD("HANGAR_CONTROL=%s", G.paths.control);
		/* Room for the number the child fills in after its dup. */
		ADD("HANGAR_WIRING=%-16d", 0);
	}
	if (G.paths.root[0])
		ADD("HANGAR_ROOT=%s", G.paths.root);
	for (int i = 0; i < mf->n_env; i++)
		ADD("%s", mf->env[i]);
#undef ADD
	c->envp[n] = NULL;
}

int module_spawn(struct module *m)
{
	const struct mf_module *mf = m->mf;
	static struct child_ctx c;
	memset(&c, 0, sizeof c);
	c.m = m;
	if (resolve_ids(m, &c) < 0)
		return -1;
	if (access(mf->exec.argv[0], X_OK) < 0) {
		hd_log("%s: cannot run %s: %s", m->name, mf->exec.argv[0], strerror(errno));
		return -1;
	}
	/* Which steps run: the privileged ones only as root. */
	if (cgroup_for(m, c.cgroup, sizeof c.cgroup) == 0 && G.is_root) {
		if (mkdir(c.cgroup, 0755) == 0 || errno == EEXIST)
			c.do_cgroup = 1;
		else
			hd_log("%s: skipping step 2 (cgroup %s): %s", m->name, c.cgroup, strerror(errno));
	} else {
		hd_log("%s: skipping step 2 (cgroup): %s", m->name,
		       G.is_root ? "no delegated cgroup" : "not running as root");
	}
	c.do_rlimit = !mf->lock_memory;
	if (G.is_root) {
		c.do_sched = 1;
	} else if (mf->sched != MF_SCHED_OTHER || mf->nice < 0) {
		hd_log("%s: skipping step 4 (scheduling policy %s, priority %d, nice %d): not running as root",
		       m->name, mf->sched == MF_SCHED_FIFO ? "fifo" : mf->sched == MF_SCHED_RR ? "rr" : "other",
		       mf->priority, mf->nice);
	}
	if (G.is_root && !mf->privileged) {
		c.do_user = 1;
		if (mf->n_caps) {
			c.do_caps = 1;
			for (int i = 0; i < mf->n_caps; i++) {
				int cap = cap_number(mf->caps[i]);
				if (cap >= 0)
					c.caps |= 1u << cap;
			}
		}
	} else if (!G.is_root) {
		hd_log("%s: skipping steps 7-9 (capabilities, user %s, group %s): not running as root", m->name,
		       mf->user, mf->group);
	}
	mf_fmt(c.state_dir, sizeof c.state_dir, "%s/%s", G.paths.var, m->name);
	mf_fmt(c.log_file, sizeof c.log_file, "%s/%s.log", G.conf.log_dir, m->name);
	c.use_journal = access(G.paths.journal, W_OK) == 0;
	if (!c.use_journal) {
		cut_log(c.log_file, G.conf.log_cap);
		hd_log("%s: no journald at %s: logging to %s", m->name, G.paths.journal, c.log_file);
	}
	build_env(&c, m);

	int release[2], errp[2];
	if (pipe2(release, O_CLOEXEC) < 0)
		return -1;
	if (pipe2(errp, O_CLOEXEC) < 0) {
		close(release[0]);
		close(release[1]);
		return -1;
	}
	c.release_fd = release[0];
	c.err_fd = errp[1];

	uint64_t instance = hg_now_ns();
	if (page_prepare(m, instance) < 0)
		hd_log("%s: cannot prepare the module page: %s", m->name, strerror(errno));

	int pidfd = -1;
	struct hd_clone_args ca;
	memset(&ca, 0, sizeof ca);
	ca.flags = HD_CLONE_PIDFD;
	ca.pidfd = (uint64_t)(uintptr_t)&pidfd;
	ca.exit_signal = SIGCHLD;
	long pid = syscall(SYS_clone3, &ca, sizeof ca);
	if (pid < 0) {
		hd_log("%s: clone3: %s", m->name, strerror(errno));
		close(release[0]);
		close(release[1]);
		close(errp[0]);
		close(errp[1]);
		return -1;
	}
	if (pid == 0)
		child_main(&c);
	close(release[0]);
	close(errp[1]);
	setpgid((pid_t)pid, (pid_t)pid);

	/* The entry is in the registry, and the Neutral record on disk, before
	 * the child may run (docs/03 step 1). */
	m->pid = (pid_t)pid;
	m->pidfd = pidfd;
	m->adopted = 0;
	m->started_ns = instance;
	m->starttime = proc_starttime(m->pid);
	m->exit_known = 0;
	registry_write();
	if (neutral_record_write(m) < 0)
		hd_log("%s: cannot write the Neutral record: %s", m->name, strerror(errno));
	ssize_t w;
	while ((w = write(release[1], "g", 1)) < 0 && errno == EINTR)
		;
	close(release[1]);

	/* End of file once execve succeeds; a step and an errno if not. */
	int v[2];
	ssize_t r;
	while ((r = read(errp[0], v, sizeof v)) < 0 && errno == EINTR)
		;
	close(errp[0]);
	if (r == (ssize_t)sizeof v) {
		hd_log("%s: cannot start: step %d: %s", m->name, v[0], strerror(v[1]));
		siginfo_t si;
		while (syscall(SYS_waitid, HD_P_PIDFD, pidfd, &si, WEXITED, NULL) < 0 && errno == EINTR)
			;
		close(pidfd);
		m->pid = 0;
		m->pidfd = -1;
		return -1;
	}
	if (epoll_add(pidfd, TAG_MODULE | (uint64_t)(m - G.modules)) < 0)
		hd_die("epoll_ctl: %s", strerror(errno));
	hd_log("%s: started, pid %d", m->name, (int)pid);
	return 0;
}

void module_signal_group(struct module *m, int sig)
{
	if (m->pid <= 0)
		return;
	if (kill(-m->pid, sig) < 0 && errno == ESRCH && m->pidfd >= 0)
		syscall(SYS_pidfd_send_signal, m->pidfd, sig, NULL, 0u);
}

void module_kill_group(struct module *m)
{
	char cg[HD_PATH];
	if (cgroup_for(m, cg, sizeof cg) == 0 && G.is_root) {
		char path[HD_PATH + 16];
		mf_fmt(path, sizeof path, "%s/cgroup.kill", cg);
		int fd = open(path, O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t w = write(fd, "1", 1);
			close(fd);
			if (w == 1)
				return;
		}
	}
	module_signal_group(m, SIGKILL);
}
