// SPDX-License-Identifier: GPL-3.0-or-later
/* Topic files on a module's behalf (docs/01 "Opening", docs/03 "Opening
 * topics"): a temporary file, its memory reserved, the header written, the
 * wake file created once per boot, bit 2 on the old file with a wake, the
 * rename, the directory word. The layout is written by offset, as
 * docs/01 "Header" gives it; lib/hg's reader validates what we wrote. */
#include "hangard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HEADER_BYTES 4096u
#define SLOT_HEADER_BYTES 128u
#define WAKE_BYTES 64u

#define OFF_MAGIC 0
#define OFF_VERSION 8
#define OFF_HEADER_BYTES 12
#define OFF_GENERATION 16
#define OFF_SLOT_COUNT 24
#define OFF_SLOT_HEADER_BYTES 28
#define OFF_PAYLOAD_BYTES 32
#define OFF_FLAGS 36
#define OFF_SLOT_STRIDE 40
#define OFF_SLOTS_OFFSET 48
#define OFF_TOPIC_ID 56
#define OFF_RATE_MHZ 60
#define OFF_TYPE_HASH 64
#define OFF_NAME 72
#define OFF_WRITER 120
#define OFF_WRITER_PID 152
#define OFF_SCHEMA_BYTES 156
#define OFF_CREATED 160
#define OFF_FIRST_SEQ 168
#define OFF_STALE_AFTER 176
#define OFF_FUTEX 192
#define OFF_WRITE_SEQ 200
#define OFF_LAST_PUBLISH 208
#define OFF_ORIGIN_BOOT 256
#define OFF_SCHEMA 512

static _Atomic uint32_t *at32(unsigned char *h, size_t off)
{
	return (_Atomic uint32_t *)(void *)(h + off);
}

static _Atomic uint64_t *at64(unsigned char *h, size_t off)
{
	return (_Atomic uint64_t *)(void *)(h + off);
}

uint64_t hdr_u64(const unsigned char *h, size_t off)
{
	return atomic_load_explicit((const _Atomic uint64_t *)(const void *)(h + off), memory_order_acquire);
}

uint32_t hdr_u32(const unsigned char *h, size_t off)
{
	return atomic_load_explicit((const _Atomic uint32_t *)(const void *)(h + off), memory_order_acquire);
}

static void put32(unsigned char *h, size_t off, uint32_t v)
{
	memcpy(h + off, &v, 4);
}

static void put64(unsigned char *h, size_t off, uint64_t v)
{
	memcpy(h + off, &v, 8);
}

static uint64_t stride_for(uint32_t payload)
{
	uint64_t p = ((uint64_t)payload + 7) & ~(uint64_t)7;
	return (SLOT_HEADER_BYTES + p + 63) & ~(uint64_t)63;
}

static int topic_path(char *out, size_t len, const char *prefix, const char *name, const char *suffix)
{
	int n = mf_fmt(out, len, "%s/%s%s%s", G.conf.topic_dir, prefix, name, suffix);
	if (n < 0 || (size_t)n >= len) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/* Gives a file to the hangar group with mode when we are root; otherwise
 * the mode alone (the group is ours). */
static void own_file(int fd, mode_t mode)
{
	if (G.is_root && G.have_gid_hangar)
		(void)fchown(fd, 0, G.gid_hangar);
	(void)fchmod(fd, mode);
}

/* A 64-byte word file, O_CREAT once per boot, never truncated. */
static int word_file(const char *path, mode_t mode)
{
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, mode);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st) == 0 && st.st_size < (off_t)WAKE_BYTES)
		(void)ftruncate(fd, WAKE_BYTES);
	own_file(fd, mode);
	return fd;
}

static void wake_word(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return;
	void *m = mmap(NULL, WAKE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return;
	atomic_fetch_add_explicit((_Atomic uint32_t *)m, 1, memory_order_seq_cst);
	hg_futex_wake_all(m);
	munmap(m, WAKE_BYTES);
}

int topic_dir_prepare(void)
{
	/* Parents too: under $HANGAR_ROOT the stand-in /dev/shm may not exist. */
	char buf[HD_PATH];
	mf_fmt(buf, sizeof buf, "%s", G.conf.topic_dir);
	for (char *p = buf + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(buf, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	if (mkdir(G.conf.topic_dir, 0750) < 0 && errno != EEXIST)
		return -1;
	if (G.is_root && G.have_gid_hangar)
		(void)chown(G.conf.topic_dir, 0, G.gid_hangar);
	(void)chmod(G.conf.topic_dir, 0750);
	char path[HD_PATH];
	if (topic_path(path, sizeof path, ".", "dir", "") < 0)
		return -1;
	int fd = word_file(path, 0660);
	if (fd < 0)
		return -1;
	close(fd);
	return 0;
}

void topic_dir_bump(void)
{
	char path[HD_PATH];
	if (topic_path(path, sizeof path, ".", "dir", "") == 0)
		wake_word(path);
}

static void announce(unsigned char *h)
{
	atomic_fetch_add_explicit(at32(h, OFF_FUTEX), 1, memory_order_seq_cst);
	hg_futex_wake_all(h + OFF_FUTEX);
}

/* Maps the header of an existing topic read-write; NULL if none or invalid. */
static unsigned char *map_old(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	unsigned char *h = NULL;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)HEADER_BYTES) {
		void *m = mmap(NULL, HEADER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m != MAP_FAILED) {
			if (memcmp(m, "HGTOPIC1", 8) == 0)
				h = m;
			else
				munmap(m, HEADER_BYTES);
		}
	}
	close(fd);
	return h;
}

static uint64_t real_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int topic_create(const struct topic_params *tp, char *err, size_t errlen)
{
	char path[HD_PATH], tmp[HD_PATH], wake[HD_PATH], suffix[32];
	mf_fmt(suffix, sizeof suffix, ".tmp.%ld", (long)getpid());
	if (topic_path(path, sizeof path, "", tp->name, "") < 0 ||
	    topic_path(tmp, sizeof tmp, ".", tp->name, suffix) < 0 ||
	    topic_path(wake, sizeof wake, ".", tp->name, ".wake") < 0) {
		mf_fmt(err, errlen, "name too long");
		return -1;
	}
	if (tp->slots < 2 || tp->slots > 65536 || (tp->slots & (tp->slots - 1)) || !tp->payload_bytes ||
	    tp->schema_bytes > HG_SCHEMA_MAX) {
		mf_fmt(err, errlen, "bad slots or payload");
		errno = EINVAL;
		return -1;
	}
	uint64_t stride = stride_for(tp->payload_bytes);
	size_t len = HEADER_BYTES + (size_t)tp->slots * stride;

	/* An existing file continues write_seq, unless it is a mirror of
	 * another boot of the sender. */
	unsigned char *old = map_old(path);
	uint64_t w = 0;
	if (tp->origin_boot)
		w = tp->first_seq ? tp->first_seq - 1 : 0;
	if (old) {
		int same = tp->origin_boot ? memcmp(old + OFF_ORIGIN_BOOT, tp->origin_boot, 16) == 0 : 1;
		if (same)
			w = hdr_u64(old, OFF_WRITE_SEQ);
	}

	int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (fd < 0 && errno == EEXIST && unlink(tmp) == 0)
		fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (fd < 0) {
		mf_fmt(err, errlen, "cannot create %s: %s", tmp, strerror(errno));
		goto fail;
	}
	int e = posix_fallocate(fd, 0, (off_t)len);
	if (e) {
		errno = e;
		mf_fmt(err, errlen, "cannot reserve %zu bytes: %s", len, strerror(e));
		goto fail_tmp;
	}
	own_file(fd, 0640);
	unsigned char *h = mmap(NULL, HEADER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (h == MAP_FAILED) {
		mf_fmt(err, errlen, "mmap: %s", strerror(errno));
		goto fail_tmp;
	}
	uint64_t gen;
	if (getrandom(&gen, sizeof gen, 0) != (ssize_t)sizeof gen)
		gen = hg_now_ns() ^ ((uint64_t)getpid() << 32);
	memcpy(h + OFF_MAGIC, "HGTOPIC1", 8);
	put32(h, OFF_VERSION, 1);
	put32(h, OFF_HEADER_BYTES, HEADER_BYTES);
	put64(h, OFF_GENERATION, gen);
	put32(h, OFF_SLOT_COUNT, tp->slots);
	put32(h, OFF_SLOT_HEADER_BYTES, SLOT_HEADER_BYTES);
	put32(h, OFF_PAYLOAD_BYTES, tp->payload_bytes);
	put32(h, OFF_FLAGS, 0);
	put64(h, OFF_SLOT_STRIDE, stride);
	put64(h, OFF_SLOTS_OFFSET, HEADER_BYTES);
	put32(h, OFF_TOPIC_ID, hg_topic_id(tp->name));
	put32(h, OFF_RATE_MHZ, tp->rate_mhz);
	put64(h, OFF_TYPE_HASH, tp->type_hash);
	memcpy(h + OFF_NAME, tp->name, strlen(tp->name));
	mf_fmt((char *)h + OFF_WRITER, 32, "%s", tp->writer);
	put32(h, OFF_WRITER_PID, (uint32_t)tp->writer_pid);
	put32(h, OFF_SCHEMA_BYTES, tp->schema_bytes);
	put64(h, OFF_CREATED, real_now_ns());
	put64(h, OFF_FIRST_SEQ, w);
	put64(h, OFF_STALE_AFTER, tp->stale_after_ns);
	atomic_store_explicit(at64(h, OFF_WRITE_SEQ), w, memory_order_relaxed);
	if (tp->origin_boot)
		memcpy(h + OFF_ORIGIN_BOOT, tp->origin_boot, 16);
	memcpy(h + OFF_SCHEMA, tp->schema, tp->schema_bytes);
	munmap(h, HEADER_BYTES);

	/* The wake file exists before the rename; it is never replaced. */
	int wfd = word_file(wake, 0660);
	if (wfd < 0) {
		mf_fmt(err, errlen, "cannot create %s: %s", wake, strerror(errno));
		goto fail_tmp;
	}
	close(wfd);

	uint32_t old_flags = 0;
	if (old) {
		old_flags = atomic_fetch_or_explicit(at32(old, OFF_FLAGS), HG_FLAG_REPLACED, memory_order_release);
		announce(old);
	}
	if (rename(tmp, path) < 0) {
		e = errno;
		mf_fmt(err, errlen, "rename: %s", strerror(e));
		if (old && !(old_flags & HG_FLAG_REPLACED)) {
			atomic_fetch_and_explicit(at32(old, OFF_FLAGS), ~HG_FLAG_REPLACED, memory_order_release);
			announce(old);
		}
		errno = e;
		goto fail_tmp;
	}
	topic_dir_bump();
	if (old)
		munmap(old, HEADER_BYTES);
	return fd;

fail_tmp:
	e = errno;
	close(fd);
	unlink(tmp);
	errno = e;
fail:
	e = errno;
	if (old)
		munmap(old, HEADER_BYTES);
	errno = e;
	return -1;
}

int topic_mark_died(const char *path)
{
	unsigned char *h = map_old(path);
	if (!h)
		return -1;
	atomic_fetch_or_explicit(at32(h, OFF_FLAGS), HG_FLAG_DIED, memory_order_release);
	announce(h);
	munmap(h, HEADER_BYTES);
	return 0;
}

unsigned char *topic_map_header(const char *path, ino_t *ino)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	unsigned char *h = NULL;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)HEADER_BYTES) {
		void *m = mmap(NULL, HEADER_BYTES, PROT_READ, MAP_SHARED, fd, 0);
		if (m != MAP_FAILED) {
			if (memcmp(m, "HGTOPIC1", 8) == 0) {
				h = m;
				if (ino)
					*ino = st.st_ino;
			} else {
				munmap(m, HEADER_BYTES);
				errno = EPROTO;
			}
		}
	} else if (st.st_size < (off_t)HEADER_BYTES) {
		errno = EPROTO;
	}
	int e = errno;
	close(fd);
	errno = e;
	return h;
}

void topics_mark_writer_died(const char *writer)
{
	DIR *d = opendir(G.conf.topic_dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[HD_PATH];
		mf_fmt(path, sizeof path, "%s/%s", G.conf.topic_dir, e->d_name);
		unsigned char *h = topic_map_header(path, NULL);
		if (!h)
			continue;
		char w[33];
		memcpy(w, h + OFF_WRITER, 32);
		w[32] = 0;
		int mine = strcmp(w, writer) == 0;
		munmap(h, HEADER_BYTES);
		if (mine && topic_mark_died(path) < 0)
			hd_log("%s: cannot flag %s: %s", writer, path, strerror(errno));
	}
	closedir(d);
}

int topic_other_writer(const char *name, pid_t asker, char *who, size_t wholen)
{
	char path[HD_PATH];
	if (topic_path(path, sizeof path, "", name, "") < 0)
		return 0;
	unsigned char *h = topic_map_header(path, NULL);
	if (!h)
		return 0;
	pid_t wp = (pid_t)hdr_u32(h, OFF_WRITER_PID);
	uint32_t flags = hdr_u32(h, OFF_FLAGS);
	char w[33];
	memcpy(w, h + OFF_WRITER, 32);
	w[32] = 0;
	munmap(h, HEADER_BYTES);
	/* Closed, dead or replaced: no writer holds it. Otherwise the process
	 * the header names holds it while it is alive: a module of ours, the
	 * manager itself, or any other live process (a module's helper that
	 * kept the descriptor, a stand-in in the tests). A dead writer is
	 * flagged by the manager at its exit or at start-up, so a reused pid
	 * is met only by a file that already carries bit 1. */
	if (flags & (HG_FLAG_CLOSED | HG_FLAG_DIED | HG_FLAG_REPLACED))
		return 0;
	if (wp <= 0 || wp == asker)
		return 0;
	if (wp == getpid()) {
		mf_fmt(who, wholen, "hangard (pid %d)", (int)wp);
		return 1;
	}
	if (kill(wp, 0) < 0 && errno == ESRCH)
		return 0;
	mf_fmt(who, wholen, "%s (pid %d)", w, (int)wp);
	return 1;
}
