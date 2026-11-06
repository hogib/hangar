/* spike-manager <config>: starts modules, restarts them with back-off, flags
 * the topics of a dead writer, and stops everything on SIGTERM/SIGINT.
 * Config: one module per line, "<name> <topic|-> <program> [args...]". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hg_sys.h"
#include "hg_topic.h"

extern char **environ;

#define MS 1000000ull
#define BACKOFF_FIRST (100 * MS)
#define BACKOFF_MAX (2000 * MS)
#define RESET_AFTER (5000 * MS)
#define STOP_TIMEOUT (500 * MS)

enum state { ST_RUNNING, ST_BACKOFF, ST_STOPPING, ST_STOPPED };
static const char *const state_names[] = { "running", "backoff", "stopping", "stopped" };

struct module {
	char *name;
	char *topic;          /* NULL: writes no topic */
	char *exe;            /* resolved before spawning */
	char **argv;
	enum state st;
	pid_t pid;            /* 0 when not running */
	int pidfd;
	unsigned long long starttime;
	uint64_t started_ns;
	uint64_t restart_at;
	uint64_t delay_ns;    /* current back-off */
	unsigned failures;
};

static struct module *mods;
static size_t nmods;
static int epfd, sigfd, timfd;
static const char *topic_dir;
static char registry_path[PATH_MAX], registry_tmp[PATH_MAX];
static int shutting_down;
static uint64_t stop_deadline;

#define TAG_SIGNAL 0
#define TAG_TIMER 1
#define TAG_MODULE 2

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("spike-manager: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void die(const char *what)
{
	logf_("%s: %s", what, strerror(errno));
	exit(1);
}

/* ---- Config ------------------------------------------------------------ */

static char *resolve(const char *prog)
{
	if (strchr(prog, '/'))
		return strdup(prog);
	const char *path = getenv("PATH");
	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	char buf[PATH_MAX];
	for (const char *p = path;; p++) {
		const char *e = strchr(p, ':');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len && snprintf(buf, sizeof buf, "%.*s/%s", (int)len, p, prog) < (int)sizeof buf &&
		    access(buf, X_OK) == 0)
			return strdup(buf);
		if (!e)
			break;
		p = e;
	}
	return strdup(prog); /* not found: execve reports it */
}

static void load_config(const char *file)
{
	FILE *f = fopen(file, "re");
	if (!f)
		die(file);
	char *line = NULL;
	size_t cap = 0;
	unsigned lineno = 0;
	while (getline(&line, &cap, f) >= 0) {
		lineno++;
		char *save, *tok[256];
		int n = 0;
		for (char *t = strtok_r(line, " \t\r\n", &save); t;
		     t = strtok_r(NULL, " \t\r\n", &save)) {
			if (n == 255) {
				logf_("%s:%u: more than 255 words", file, lineno);
				exit(1);
			}
			tok[n++] = t;
		}
		if (n == 0 || tok[0][0] == '#')
			continue;
		if (n < 3) {
			logf_("%s:%u: want <name> <topic|-> <program> [args...]", file, lineno);
			exit(1);
		}
		const char *topic = strcmp(tok[1], "-") ? tok[1] : NULL;
		if (topic && !hg_valid_name(topic)) {
			logf_("%s:%u: invalid topic name %s", file, lineno, topic);
			exit(1);
		}
		for (size_t i = 0; topic && i < nmods; i++)
			if (mods[i].topic && !strcmp(mods[i].topic, topic)) {
				logf_("%s:%u: topic %s is already written by %s", file, lineno, topic,
				      mods[i].name);
				exit(1);
			}
		struct module *nm = realloc(mods, (nmods + 1) * sizeof *mods);
		if (!nm)
			die("realloc");
		mods = nm;
		struct module *m = &mods[nmods++];
		memset(m, 0, sizeof *m);
		m->pidfd = -1;
		m->name = strdup(tok[0]);
		m->topic = topic ? strdup(topic) : NULL;
		m->exe = resolve(tok[2]);
		m->argv = calloc((size_t)n - 1, sizeof *m->argv);
		if (!m->name || (topic && !m->topic) || !m->exe || !m->argv)
			die("strdup");
		for (int i = 2; i < n; i++)
			if (!(m->argv[i - 2] = strdup(tok[i])))
				die("strdup");
	}
	free(line);
	fclose(f);
}

/* ---- Registry ---------------------------------------------------------- */

static unsigned long long read_starttime(pid_t pid)
{
	char path[64], buf[1024];
	snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	char *p = strrchr(buf, ')'); /* the command name may hold spaces */
	if (!p)
		return 0;
	/* Fields 3..21 follow ") "; field 22 is the start time. */
	for (int field = 2; field < 22 && p; field++)
		p = strchr(p + 1, ' ');
	return p ? strtoull(p + 1, NULL, 10) : 0;
}

static void write_registry(void)
{
	FILE *f = fopen(registry_tmp, "we");
	if (!f) {
		logf_("cannot write %s: %s", registry_tmp, strerror(errno));
		return;
	}
	fprintf(f, "hangar-registry 1\n");
	for (size_t i = 0; i < nmods; i++) {
		struct module *m = &mods[i];
		fprintf(f, "%s %d %llu %s %u %g\n", m->name, (int)m->pid,
			m->pid ? m->starttime : 0ull, state_names[m->st], m->failures,
			(double)m->delay_ns / 1e9);
	}
	if (fclose(f) != 0 || rename(registry_tmp, registry_path) != 0)
		logf_("cannot write %s: %s", registry_path, strerror(errno));
}

/* ---- Timer ------------------------------------------------------------- */

static void arm_timer(void)
{
	uint64_t at = UINT64_MAX;
	if (shutting_down)
		at = stop_deadline;
	else
		for (size_t i = 0; i < nmods; i++)
			if (mods[i].st == ST_BACKOFF && mods[i].restart_at < at)
				at = mods[i].restart_at;
	struct itimerspec its = { 0 };
	if (at != UINT64_MAX) {
		if (at == 0)
			at = 1;
		its.it_value.tv_sec = (time_t)(at / 1000000000u);
		its.it_value.tv_nsec = (long)(at % 1000000000u);
	}
	if (timerfd_settime(timfd, TFD_TIMER_ABSTIME, &its, NULL) < 0)
		die("timerfd_settime");
}

/* ---- Spawning ---------------------------------------------------------- */

/* Back-off: 100 ms, doubling, at most 2 s; reset after 5 s running. */
static void schedule_restart(struct module *m)
{
	uint64_t now = hg_now_ns();
	if (m->started_ns && now - m->started_ns >= RESET_AFTER) {
		m->failures = 0;
		m->delay_ns = 0;
	}
	m->delay_ns = m->delay_ns ? m->delay_ns * 2 : BACKOFF_FIRST;
	if (m->delay_ns > BACKOFF_MAX)
		m->delay_ns = BACKOFF_MAX;
	m->failures++;
	m->restart_at = now + m->delay_ns;
	m->st = ST_BACKOFF;
	logf_("%s: restarting in %g s", m->name, (double)m->delay_ns / 1e9);
	write_registry();
	arm_timer();
}

static _Noreturn void child(const struct module *m, int errfd)
{
	setpgid(0, 0);
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigemptyset(&dfl.sa_mask);
	for (int sig = 1; sig < 65; sig++)
		sigaction(sig, &dfl, NULL); /* SIGKILL, SIGSTOP and libc-internal ones fail */
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	/* Every descriptor above 2 closes at exec; errfd until then. */
	syscall(SYS_close_range, 3u, ~0u, HG_CLOSE_RANGE_CLOEXEC);
	execve(m->exe, m->argv, environ);
	int e = errno;
	ssize_t r = write(errfd, &e, sizeof e);
	(void)r;
	_exit(127);
}

static void spawn(struct module *m)
{
	int pfd[2];
	if (pipe2(pfd, O_CLOEXEC) < 0) {
		logf_("%s: pipe2: %s", m->name, strerror(errno));
		schedule_restart(m);
		return;
	}
	int pidfd = -1;
	struct hg_clone_args ca = { 0 };
	ca.flags = HG_CLONE_PIDFD;
	ca.pidfd = (uint64_t)(uintptr_t)&pidfd;
	ca.exit_signal = SIGCHLD;
	long pid = syscall(SYS_clone3, &ca, sizeof ca);
	if (pid < 0) {
		/* No fallback to fork: without CLONE_PIDFD there is no safe handle. */
		logf_("%s: clone3: %s", m->name, strerror(errno));
		close(pfd[0]);
		close(pfd[1]);
		schedule_restart(m);
		return;
	}
	if (pid == 0)
		child(m, pfd[1]);
	close(pfd[1]);
	setpgid((pid_t)pid, (pid_t)pid); /* also here: no window before the child's */

	/* End of file once execve succeeds (or the child died before it). */
	int err;
	ssize_t r;
	while ((r = read(pfd[0], &err, sizeof err)) < 0 && errno == EINTR)
		;
	close(pfd[0]);
	if (r == (ssize_t)sizeof err) {
		/* The child exits at once and has started nothing: reap it here. */
		logf_("%s: cannot run %s: %s", m->name, m->exe, strerror(err));
		siginfo_t si;
		while (syscall(SYS_waitid, HG_P_PIDFD, pidfd, &si, WEXITED, NULL) < 0 &&
		       errno == EINTR)
			;
		close(pidfd);
		m->started_ns = 0; /* it never ran: no back-off reset */
		schedule_restart(m);
		return;
	}

	m->pid = (pid_t)pid;
	m->pidfd = pidfd;
	m->started_ns = hg_now_ns();
	m->starttime = read_starttime(m->pid);
	m->st = ST_RUNNING;
	write_registry();
	logf_("%s: started, pid %d", m->name, (int)pid);

	struct epoll_event ev = { .events = EPOLLIN,
				  .data.u64 = TAG_MODULE + (uint64_t)(m - mods) };
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, pidfd, &ev) < 0)
		die("epoll_ctl");
}

static void signal_group(struct module *m, int sig)
{
	if (kill(-m->pid, sig) < 0 && errno == ESRCH)
		syscall(SYS_pidfd_send_signal, m->pidfd, sig, NULL, 0u);
}

static void on_exit_(struct module *m)
{
	siginfo_t si;
	memset(&si, 0, sizeof si);
	int reaped = 0; /* by someone else: no status, and the group id is free */
	if (syscall(SYS_waitid, HG_P_PIDFD, m->pidfd, &si, WEXITED | WNOWAIT, NULL) < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		logf_("%s: waitid: %s", m->name, strerror(errno));
		reaped = 1;
	}
	if (m->topic) {
		char path[PATH_MAX];
		snprintf(path, sizeof path, "%s/%s", topic_dir, m->topic);
		if (hg_topic_mark_died(path) < 0 && errno != ENOENT)
			logf_("%s: cannot flag %s: %s", m->name, path, strerror(errno));
	}
	if (!reaped) {
		/* The unreaped leader keeps the group id from being reused. */
		if (kill(-m->pid, SIGKILL) < 0 && errno != ESRCH)
			logf_("%s: kill: %s", m->name, strerror(errno));
		siginfo_t si2;
		while (syscall(SYS_waitid, HG_P_PIDFD, m->pidfd, &si2, WEXITED, NULL) < 0 &&
		       errno == EINTR)
			;
	}
	if (reaped)
		logf_("%s: pid %d exited, status unknown", m->name, (int)m->pid);
	else if (si.si_code == CLD_EXITED)
		logf_("%s: pid %d exited with status %d", m->name, (int)m->pid, si.si_status);
	else
		logf_("%s: pid %d killed by signal %d (%s)", m->name, (int)m->pid, si.si_status,
		      strsignal(si.si_status));

	epoll_ctl(epfd, EPOLL_CTL_DEL, m->pidfd, NULL);
	close(m->pidfd);
	m->pidfd = -1;
	m->pid = 0;

	if (shutting_down) {
		m->st = ST_STOPPED;
		write_registry();
		arm_timer();
	} else {
		schedule_restart(m);
	}
}

static void on_timer(void)
{
	uint64_t ticks;
	while (read(timfd, &ticks, sizeof ticks) < 0 && errno == EINTR)
		;
	uint64_t now = hg_now_ns();
	if (shutting_down) {
		if (now >= stop_deadline) {
			for (size_t i = 0; i < nmods; i++)
				if (mods[i].pid) {
					logf_("%s: still running after %llu ms, killing", mods[i].name,
					      STOP_TIMEOUT / MS);
					signal_group(&mods[i], SIGKILL);
				}
			return; /* reaped as their pidfds become readable */
		}
	} else {
		for (size_t i = 0; i < nmods; i++)
			if (mods[i].st == ST_BACKOFF && mods[i].restart_at <= now)
				spawn(&mods[i]);
	}
	arm_timer();
}

static void begin_shutdown(void)
{
	shutting_down = 1;
	stop_deadline = hg_now_ns() + STOP_TIMEOUT;
	for (size_t i = 0; i < nmods; i++) {
		struct module *m = &mods[i];
		if (m->pid) {
			m->st = ST_STOPPING;
			signal_group(m, SIGTERM);
		} else {
			m->st = ST_STOPPED;
		}
	}
	write_registry();
	arm_timer();
}

static int running(void)
{
	for (size_t i = 0; i < nmods; i++)
		if (mods[i].pid)
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: spike-manager <config>\n");
		return 2;
	}
	topic_dir = getenv("HANGAR_TOPIC_DIR");
	if (!topic_dir || !*topic_dir) {
		logf_("HANGAR_TOPIC_DIR is not set");
		return 1;
	}
	if (mkdir(topic_dir, 0750) < 0 && errno != EEXIST)
		die(topic_dir);
	snprintf(registry_path, sizeof registry_path, "%s/../registry", topic_dir);
	snprintf(registry_tmp, sizeof registry_tmp, "%s/../.registry.tmp", topic_dir);
	load_config(argv[1]);

	/* An inherited SIG_IGN or SA_NOCLDWAIT would have the kernel reap every
	 * module at exit: no status, and its group id free for reuse. */
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	sigemptyset(&dfl.sa_mask);
	if (sigaction(SIGCHLD, &dfl, NULL) < 0)
		die("sigaction");
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		die("sigprocmask");
	if ((sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)) < 0)
		die("signalfd");
	if ((timfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) < 0)
		die("timerfd_create");
	if ((epfd = epoll_create1(EPOLL_CLOEXEC)) < 0)
		die("epoll_create1");
	struct epoll_event ev = { .events = EPOLLIN, .data.u64 = TAG_SIGNAL };
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &ev) < 0)
		die("epoll_ctl");
	ev.data.u64 = TAG_TIMER;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, timfd, &ev) < 0)
		die("epoll_ctl");

	for (size_t i = 0; i < nmods; i++)
		spawn(&mods[i]);

	while (!shutting_down || running()) {
		struct epoll_event evs[16];
		int n = epoll_wait(epfd, evs, 16, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("epoll_wait");
		}
		for (int i = 0; i < n; i++) {
			uint64_t tag = evs[i].data.u64;
			if (tag == TAG_SIGNAL) {
				struct signalfd_siginfo ssi;
				while (read(sigfd, &ssi, sizeof ssi) == (ssize_t)sizeof ssi)
					if ((ssi.ssi_signo == SIGTERM || ssi.ssi_signo == SIGINT) &&
					    !shutting_down) {
						logf_("signal %u: stopping modules", ssi.ssi_signo);
						begin_shutdown();
					}
				/* SIGCHLD carries nothing the pidfds do not. */
			} else if (tag == TAG_TIMER) {
				on_timer();
			} else if (mods[tag - TAG_MODULE].pid) {
				on_exit_(&mods[tag - TAG_MODULE]);
			}
		}
	}
	unlink(registry_path);
	logf_("stopped");
	return 0;
}
