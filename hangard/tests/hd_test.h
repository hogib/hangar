// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared by hangard's suites: a $HANGAR_ROOT of their own on tmpfs with
 * etc/hangar, run/hangar, var/lib/hangar, dev/shm/hangar and the message
 * types; manifests written from text; the manager started from
 * $HANGARD_BIN with its log in the root; the control socket; the status
 * reply parsed; topic headers by offset; the hangar command run with its
 * output captured. */
#ifndef HD_TEST_H
#define HD_TEST_H

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hg_control.h"
#include "hg_topic.h"
#include "test_harness.h"

static char T_root[256], T_modules[512], T_msg[512], T_control[512], T_topics[512], T_log[512];
static const char *T_hangard, *T_hangar, *T_toys;

/* snprintf through a va_list: bounded copies that may truncate are the
 * intent here, and this keeps the compiler from flagging each one. */
static inline int tfmt(char *dst, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static inline int tfmt(char *dst, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, len, fmt, ap);
	va_end(ap);
	return n;
}

static inline uint64_t now_ms(void)
{
	return hg_now_ns() / 1000000u;
}

static inline void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static inline int mkdir_p(const char *path)
{
	char buf[1024];
	tfmt(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	return mkdir(buf, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

static inline int write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	size_t len = strlen(text);
	ssize_t w = write(fd, text, len);
	close(fd);
	return w == (ssize_t)len ? 0 : -1;
}

/* Reads a whole file into a static buffer (the last 64 KB of it). */
static inline const char *read_file(const char *path)
{
	static char buf[65536];
	buf[0] = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return buf;
	struct stat st;
	off_t off = 0;
	if (fstat(fd, &st) == 0 && st.st_size > (off_t)sizeof buf - 1)
		off = st.st_size - (off_t)(sizeof buf - 1);
	ssize_t n = pread(fd, buf, sizeof buf - 1, off);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	return buf;
}

static inline int file_contains(const char *path, const char *needle)
{
	return strstr(read_file(path), needle) != NULL;
}

static inline int wait_file_contains(const char *path, const char *needle, long timeout_ms)
{
	uint64_t t0 = now_ms();
	while (now_ms() - t0 < (uint64_t)timeout_ms) {
		if (file_contains(path, needle))
			return 1;
		sleep_ms(20);
	}
	return file_contains(path, needle);
}

static inline int copy_file(const char *from, const char *to)
{
	const char *t = read_file(from);
	if (!*t)
		return -1;
	return write_file(to, t);
}

/* Removes every file in a directory tree. */
static inline void rm_tree(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		char p[1024];
		tfmt(p, sizeof p, "%s/%s", path, e->d_name);
		struct stat st;
		if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
			rm_tree(p);
		else
			unlink(p);
	}
	closedir(d);
	rmdir(path);
}

/* The root for this suite: fresh, with hangard.conf and the message types. */
static inline void hd_setup(const char *tag, const char *conf_extra)
{
	T_hangard = getenv("HANGARD_BIN");
	T_hangar = getenv("HANGAR_BIN");
	T_toys = getenv("HANGARD_TOYS");
	const char *base = getenv("HANGAR_TEST_BASE");
	char b[256];
	if (base && *base)
		tfmt(b, sizeof b, "%s", base);
	else
		tfmt(b, sizeof b, "/dev/shm/hangar-test-%ld", (long)getuid());
	tfmt(T_root, sizeof T_root, "%s/hangard-%s-%ld", b, tag, (long)getpid());
	rm_tree(T_root);
	tfmt(T_modules, sizeof T_modules, "%s/etc/hangar/modules", T_root);
	tfmt(T_msg, sizeof T_msg, "%s/usr/share/hangar/msg", T_root);
	tfmt(T_control, sizeof T_control, "%s/run/hangar/control", T_root);
	tfmt(T_topics, sizeof T_topics, "%s/dev/shm/hangar", T_root);
	tfmt(T_log, sizeof T_log, "%s/hangard.log", T_root);
	if (mkdir_p(T_modules) < 0 || mkdir_p(T_msg) < 0) {
		fprintf(stderr, "cannot create %s: %s\n", T_root, strerror(errno));
		exit(2);
	}
	char xdg[512];
	tfmt(xdg, sizeof xdg, "%s/xdg", T_root);
	mkdir_p(xdg);
	setenv("XDG_RUNTIME_DIR", xdg, 1);
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *types[] = { "depth", "safety_state", "wrench", "empty", "imu", NULL };
	for (int i = 0; types[i]; i++) {
		char from[1024], to[1024];
		tfmt(from, sizeof from, "%s/%s.msg", src, types[i]);
		tfmt(to, sizeof to, "%s/%s.msg", T_msg, types[i]);
		if (copy_file(from, to) < 0) {
			fprintf(stderr, "cannot copy %s (set HANGAR_MSG_SRC)\n", from);
			exit(2);
		}
	}
	char conf[1024], text[1024];
	tfmt(conf, sizeof conf, "%s/etc/hangar/hangard.conf", T_root);
	tfmt(text, sizeof text, "# test\nRole=vehicle\nHealthTick=50ms\n%s", conf_extra ? conf_extra : "");
	write_file(conf, text);
	setenv("HANGAR_ROOT", T_root, 1);
	setenv("HANGAR_TOPIC_DIR", T_topics, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("NOTIFY_SOCKET");
}

static inline void hd_teardown(void)
{
	rm_tree(T_root);
}

/* A manifest from printf-style text; the toys' directory is %s-substituted
 * by the caller. */
static inline void manifest(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void manifest(const char *name, const char *fmt, ...)
{
	char text[4096], path[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	tfmt(path, sizeof path, "%s/%s.module", T_modules, name);
	if (write_file(path, text) < 0) {
		fprintf(stderr, "cannot write %s\n", path);
		exit(2);
	}
}

static inline void manifest_remove(const char *name)
{
	char path[1024];
	tfmt(path, sizeof path, "%s/%s.module", T_modules, name);
	unlink(path);
}

/* ---- The manager ------------------------------------------------------------ */

static inline int control_ready(long timeout_ms)
{
	uint64_t t0 = now_ms();
	do {
		int s = hg_control_connect(T_control);
		if (s >= 0) {
			close(s);
			return 1;
		}
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

/* Starts hangard with its stderr appended to T_log; waits for the socket. */
static inline pid_t manager_start(void)
{
	if (!T_hangard) {
		fprintf(stderr, "HANGARD_BIN is not set\n");
		exit(2);
	}
	pid_t pid = fork();
	if (pid < 0)
		exit(2);
	if (pid == 0) {
		int fd = open(T_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		execl(T_hangard, T_hangard, (char *)NULL);
		_exit(127);
	}
	if (!control_ready(5000))
		printf("    (the control socket did not appear within 5 s)\n");
	return pid;
}

/* Waits for a child with a timeout; its exit status, -1 if still running. */
static inline int wait_pid_ms(pid_t pid, long timeout_ms)
{
	uint64_t t0 = now_ms();
	for (;;) {
		int st;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -2;
		if (r < 0)
			return -3;
		if (now_ms() - t0 > (uint64_t)timeout_ms)
			return -1;
		sleep_ms(10);
	}
}

static inline int manager_stop(pid_t pid)
{
	kill(pid, SIGTERM);
	int rc = wait_pid_ms(pid, 10000);
	if (rc == -1) {
		kill(pid, SIGKILL);
		wait_pid_ms(pid, 2000);
	}
	return rc;
}

static inline void manager_kill9(pid_t pid)
{
	kill(pid, SIGKILL);
	wait_pid_ms(pid, 2000);
}

/* One control request, as the test process (the manager's own user counts
 * as hangar-admin). The reply, NUL-terminated, in a static buffer. */
static inline const char *ctl(const char *req)
{
	static char reply[HG_CONTROL_MAX];
	if (hg_control_call(T_control, req, strlen(req), reply, sizeof reply, NULL, 0) < 0)
		tfmt(reply, sizeof reply, "err connect: %s", strerror(errno));
	return reply;
}

/* The status line of one module: state and pid; 0 if not listed. */
struct mstat {
	char state[32];
	int pid;
	unsigned failures;
	char last_exit[32];
	char status[160];
};

static inline int module_status(const char *name, struct mstat *out)
{
	const char *r = ctl("status");
	memset(out, 0, sizeof *out);
	char needle[64];
	tfmt(needle, sizeof needle, "\nmodule %s ", name);
	const char *l = strstr(r, needle);
	if (!l)
		return 0;
	l += 1 + 7 + strlen(name) + 1;
	unsigned long long since;
	int off = 0;
	if (sscanf(l, "%31s %d %u %31s %llu %n", out->state, &out->pid, &out->failures, out->last_exit, &since,
		   &off) < 5)
		return 0;
	const char *nl = strchr(l + off, '\n');
	tfmt(out->status, sizeof out->status, "%.*s", nl ? (int)(nl - (l + off)) : 0, l + off);
	return 1;
}

static inline const char *system_state(void)
{
	static char st[32];
	const char *r = ctl("status");
	const char *l = strstr(r, "\nsystem ");
	st[0] = 0;
	if (l)
		sscanf(l + 8, "%31s", st);
	return st;
}

/* Polls status until the module is in state; 1 if it got there. */
static inline int wait_state(const char *name, const char *state, long timeout_ms)
{
	uint64_t t0 = now_ms();
	struct mstat m;
	do {
		if (module_status(name, &m) && strcmp(m.state, state) == 0)
			return 1;
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

/* Polls until the module runs under a pid other than old, in state; 1 if so. */
static inline int wait_new_pid(const char *name, int old, const char *state, long timeout_ms)
{
	uint64_t t0 = now_ms();
	struct mstat m;
	do {
		if (module_status(name, &m) && m.pid > 0 && m.pid != old && strcmp(m.state, state) == 0)
			return 1;
		sleep_ms(20);
	} while (now_ms() - t0 < (uint64_t)timeout_ms);
	return 0;
}

/* Empties the manager's log (it holds the file in append mode). */
static inline void log_clear(void)
{
	(void)truncate(T_log, 0);
}

/* ---- Topic headers ------------------------------------------------------------- */

#define H_FLAGS 36
#define H_WRITER 120
#define H_WRITER_PID 152
#define H_FIRST_SEQ 168
#define H_WRITE_SEQ 200
#define H_LAST_PUBLISH 208

static inline int topic_header(const char *name, unsigned char out[4096])
{
	char path[1024];
	tfmt(path, sizeof path, "%s/%s", T_topics, name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = pread(fd, out, 4096, 0);
	close(fd);
	return n == 4096 && memcmp(out, "HGTOPIC1", 8) == 0 ? 0 : -1;
}

static inline uint64_t h64(const unsigned char *h, size_t off)
{
	uint64_t v;
	memcpy(&v, h + off, 8);
	return v;
}

static inline uint32_t h32(const unsigned char *h, size_t off)
{
	uint32_t v;
	memcpy(&v, h + off, 4);
	return v;
}

static inline uint64_t topic_seq(const char *name)
{
	unsigned char h[4096];
	return topic_header(name, h) == 0 ? h64(h, H_WRITE_SEQ) : 0;
}

/* The module page, read from the file. */
static inline int page_read(const char *name, uint64_t *instance, uint64_t *ready, uint64_t *alive, char *status)
{
	char path[1024];
	tfmt(path, sizeof path, "%s/run/hangar/modules/%s", T_root, name);
	unsigned char page[4096];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = pread(fd, page, sizeof page, 0);
	close(fd);
	if (n != 4096 || memcmp(page, "HGPAGE01", 8) != 0)
		return -1;
	memcpy(instance, page + 8, 8);
	memcpy(ready, page + 16, 8);
	memcpy(alive, page + 24, 8);
	if (status)
		tfmt(status, 128, "%.127s", (const char *)page + 64);
	return 0;
}

/* ---- The hangar command ------------------------------------------------------- */

/* Runs `hangar args...` with a timeout; stdout into out; the exit status. */
static inline int run_cli(char *out, size_t outlen, long timeout_ms, ...)
{
	char *argv[32];
	int argc = 0;
	argv[argc++] = (char *)T_hangar;
	va_list ap;
	va_start(ap, timeout_ms);
	char *a;
	while ((a = va_arg(ap, char *)) && argc < 31)
		argv[argc++] = a;
	va_end(ap);
	argv[argc] = NULL;
	int pfd[2];
	if (pipe(pfd) < 0)
		return -1;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(pfd[1], 1);
		close(pfd[0]);
		close(pfd[1]);
		execv(T_hangar, argv);
		_exit(127);
	}
	close(pfd[1]);
	size_t used = 0;
	out[0] = 0;
	uint64_t t0 = now_ms();
	for (;;) {
		struct timeval tv = { 0, 50000 };
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(pfd[0], &fds);
		int r = select(pfd[0] + 1, &fds, NULL, NULL, &tv);
		if (r > 0) {
			ssize_t n = read(pfd[0], out + used, outlen - 1 - used);
			if (n <= 0)
				break;
			used += (size_t)n;
			out[used] = 0;
			if (used == outlen - 1)
				break;
		}
		if (now_ms() - t0 > (uint64_t)timeout_ms) {
			kill(pid, SIGTERM);
			break;
		}
	}
	close(pfd[0]);
	int rc = wait_pid_ms(pid, 2000);
	if (rc == -1) {
		kill(pid, SIGKILL);
		wait_pid_ms(pid, 1000);
	}
	return rc;
}

#endif
