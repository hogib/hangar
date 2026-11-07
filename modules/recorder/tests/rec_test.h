// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared by the recorder's and the pairer's suites: a scratch $HANGAR_ROOT
 * on tmpfs with a topic directory, wiring files for the suite's own
 * modules, children started from the built binaries, and small helpers. */
#ifndef REC_TEST_H
#define REC_TEST_H

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hg_dataset.h"
#include "hg_record.h"
#include "hg_topic.h"
#include "test_harness.h"

static char R_root[300], R_topics[400], R_rec[400];

static inline int rfmt(char *dst, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static inline int rfmt(char *dst, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, len, fmt, ap);
	va_end(ap);
	return n;
}

static inline void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static inline uint64_t now_ms(void)
{
	return hg_now_ns() / 1000000u;
}

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
		rfmt(p, sizeof p, "%s/%s", path, e->d_name);
		struct stat st;
		if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
			rm_tree(p);
		else
			unlink(p);
	}
	closedir(d);
	rmdir(path);
}

static inline int mkdir_p(const char *path, mode_t mode)
{
	char buf[1024];
	rfmt(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	if (mkdir(buf, mode) < 0 && errno != EEXIST)
		return -1;
	return chmod(buf, mode);
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

static inline const char *read_file(const char *path)
{
	static char buf[65536];
	buf[0] = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return buf;
	ssize_t n = read(fd, buf, sizeof buf - 1);
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

static inline int exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static inline int wait_exists(const char *path, long timeout_ms)
{
	uint64_t t0 = now_ms();
	while (now_ms() - t0 < (uint64_t)timeout_ms) {
		if (exists(path))
			return 1;
		sleep_ms(20);
	}
	return exists(path);
}

/* A fresh root: <base>/rec-<tag>-<pid> with dev/shm/hangar, etc/hangar,
 * var/lib/hangar/recordings (mode 2770), usr/share/hangar/msg with the types. */
static inline void rec_setup(const char *tag)
{
	const char *base = getenv("HANGAR_TEST_BASE");
	char b[256];
	if (base && *base)
		rfmt(b, sizeof b, "%s", base);
	else
		rfmt(b, sizeof b, "/dev/shm/hangar-test-%ld", (long)getuid());
	rfmt(R_root, sizeof R_root, "%s/rec-%s-%ld", b, tag, (long)getpid());
	rm_tree(R_root);
	rfmt(R_topics, sizeof R_topics, "%s/dev/shm/hangar", R_root);
	rfmt(R_rec, sizeof R_rec, "%s/var/lib/hangar/recordings", R_root);
	char p[600];
	rfmt(p, sizeof p, "%s/etc/hangar/modules", R_root);
	if (mkdir_p(R_topics, 0755) < 0 || mkdir_p(p, 0755) < 0 || mkdir_p(R_rec, 02770) < 0) {
		fprintf(stderr, "cannot create %s: %s\n", R_root, strerror(errno));
		exit(2);
	}
	rfmt(p, sizeof p, "%s/etc/hangar/datasets", R_root);
	mkdir_p(p, 0755);
	rfmt(p, sizeof p, "%s/usr/share/hangar/msg", R_root);
	mkdir_p(p, 0755);
	const char *src = getenv("HANGAR_MSG_SRC");
	if (!src || !*src)
		src = "msg";
	const char *types[] = { "depth", "imu", "wrench", "empty", "safety_state", "recorder_start", "recorder_status",
				"recorder_session", "pairer_status", NULL };
	for (int i = 0; types[i]; i++) {
		char from[700], to[700];
		rfmt(from, sizeof from, "%s/%s.msg", src, types[i]);
		rfmt(to, sizeof to, "%s/%s.msg", p, types[i]);
		write_file(to, read_file(from));
	}
	rfmt(p, sizeof p, "%s/etc/hangar/hangard.conf", R_root);
	write_file(p, "Role=vehicle\n");
	setenv("HANGAR_ROOT", R_root, 1);
	setenv("HANGAR_TOPIC_DIR", R_topics, 1);
	setvbuf(stdout, NULL, _IOLBF, 0); /* a crashed suite still leaves its lines in the log */
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
}

static inline void rec_teardown(void)
{
	if (getenv("REC_KEEP"))
		return;
	rm_tree(R_root);
}

/* Starts a program with its arguments (NULL-terminated), stderr appended
 * to <root>/<name>.log, HANGAR_WIRING_FILE set when wiring is given. */
static inline pid_t spawn(const char *name, const char *wiring, const char *bin, ...)
{
	char *argv[32];
	int argc = 0;
	argv[argc++] = (char *)bin;
	va_list ap;
	va_start(ap, bin);
	char *a;
	while ((a = va_arg(ap, char *)) && argc < 31)
		argv[argc++] = a;
	va_end(ap);
	argv[argc] = NULL;
	pid_t pid = fork();
	if (pid < 0)
		exit(2);
	if (pid == 0) {
		char log[700];
		rfmt(log, sizeof log, "%s/%s.log", R_root, name);
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 2);
			close(fd);
		}
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		if (wiring)
			setenv("HANGAR_WIRING_FILE", wiring, 1);
		else
			unsetenv("HANGAR_WIRING_FILE");
		setenv("HANGAR_MODULE", name, 1);
		execv(bin, argv);
		_exit(127);
	}
	return pid;
}

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

static inline int stop_child(pid_t pid)
{
	if (pid <= 0)
		return -1;
	kill(pid, SIGTERM);
	int rc = wait_pid_ms(pid, 5000);
	if (rc == -1) {
		kill(pid, SIGKILL);
		wait_pid_ms(pid, 2000);
	}
	return rc;
}

static inline const char *log_path(const char *name)
{
	static char p[700];
	rfmt(p, sizeof p, "%s/%s.log", R_root, name);
	return p;
}

/* The one session directory under the recordings dir whose name starts
 * with prefix (the newest by name), or "". */
static inline const char *find_session(const char *prefix)
{
	static char best[700];
	best[0] = 0;
	char name[128] = "";
	DIR *d = opendir(R_rec);
	if (!d)
		return best;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || strncmp(e->d_name, prefix, strlen(prefix)) != 0)
			continue;
		if (strcmp(e->d_name, name) > 0)
			rfmt(name, sizeof name, "%s", e->d_name);
	}
	closedir(d);
	if (name[0])
		rfmt(best, sizeof best, "%s/%s", R_rec, name);
	return best;
}

static inline int count_sessions(const char *prefix)
{
	int n = 0;
	DIR *d = opendir(R_rec);
	if (!d)
		return 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.' && strncmp(e->d_name, prefix, strlen(prefix)) == 0)
			n++;
	closedir(d);
	return n;
}

#endif
