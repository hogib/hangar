// SPDX-License-Identifier: LGPL-3.0-or-later
/* The manager's hand on topic files, for the tests. It knows the layout by
 * offset, as the manager does, and touches nothing the library's own writer
 * path would: the point is to exercise the library from outside. */
#define _GNU_SOURCE
#include "hg_manager_stub.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "hg_sys.h"
#include "hg_topic.h"

#define HEADER_BYTES 4096u
#define OFF_FLAGS 36
#define OFF_SLOT_COUNT 24
#define OFF_SLOT_STRIDE 40
#define OFF_FUTEX 192
#define OFF_WRITE_SEQ 200
#define OFF_LAST_PUBLISH 208

int hg_stub_fresh_dir(const char *base, const char *tag, char *out, size_t len)
{
	int n = snprintf(out, len, "%s/hg-%s-%ld", base, tag, (long)getpid());
	if (n < 0 || (size_t)n >= len) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (mkdir(base, 0700) < 0 && errno != EEXIST)
		return -1;
	if (mkdir(out, 0700) < 0 && errno != EEXIST)
		return -1;
	if (setenv("HANGAR_TOPIC_DIR", out, 1) < 0)
		return -1;
	return hg_stub_clear_dir();
}

int hg_stub_clear_dir(void)
{
	const char *dir = getenv("HANGAR_TOPIC_DIR");
	if (!dir) {
		errno = EINVAL;
		return -1;
	}
	DIR *d = opendir(dir);
	if (!d)
		return -1;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		unlink(p);
	}
	closedir(d);
	return 0;
}

/* Maps the header of a topic read-write. */
static void *map_header(const char *name)
{
	char path[PATH_MAX];
	if (hg_topic_path(path, sizeof path, "", name, "") < 0)
		return NULL;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	void *m = NULL;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)HEADER_BYTES) {
		void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p != MAP_FAILED && memcmp(p, "HGTOPIC1", 8) == 0)
			m = p;
		else if (p != MAP_FAILED) {
			munmap(p, (size_t)st.st_size);
			errno = EPROTO;
		}
	} else if (st.st_size < (off_t)HEADER_BYTES) {
		errno = EPROTO;
	}
	int e = errno;
	close(fd);
	errno = e;
	return m;
}

static size_t map_len(const char *name)
{
	char path[PATH_MAX];
	struct stat st;
	if (hg_topic_path(path, sizeof path, "", name, "") < 0 || stat(path, &st) < 0)
		return 0;
	return (size_t)st.st_size;
}

static _Atomic uint32_t *u32_at(void *m, size_t off)
{
	return (_Atomic uint32_t *)(void *)((unsigned char *)m + off);
}

static _Atomic uint64_t *u64_at(void *m, size_t off)
{
	return (_Atomic uint64_t *)(void *)((unsigned char *)m + off);
}

static int flag_op(const char *name, uint32_t set, uint32_t clear)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	if (set)
		atomic_fetch_or_explicit(u32_at(m, OFF_FLAGS), set, memory_order_release);
	if (clear)
		atomic_fetch_and_explicit(u32_at(m, OFF_FLAGS), ~clear, memory_order_release);
	atomic_fetch_add_explicit(u32_at(m, OFF_FUTEX), 1, memory_order_seq_cst);
	hg_futex_wake_all(u32_at(m, OFF_FUTEX));
	munmap(m, map_len(name));
	return 0;
}

int hg_stub_mark_died(const char *name)
{
	return flag_op(name, HG_FLAG_DIED, 0);
}

int hg_stub_mark_replaced(const char *name)
{
	return flag_op(name, HG_FLAG_REPLACED, 0);
}

int hg_stub_clear_replaced(const char *name)
{
	return flag_op(name, 0, HG_FLAG_REPLACED);
}

int hg_stub_bump_dir(void)
{
	char path[PATH_MAX];
	if (hg_topic_path(path, sizeof path, ".", "dir", "") < 0)
		return -1;
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0660);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st) == 0 && st.st_size < 64)
		(void)ftruncate(fd, 64);
	void *m = mmap(NULL, 64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return -1;
	atomic_fetch_add_explicit(u32_at(m, 0), 1, memory_order_seq_cst);
	hg_futex_wake_all(m);
	munmap(m, 64);
	return 0;
}

int hg_stub_read_header(const char *name, unsigned char out[4096])
{
	void *m = map_header(name);
	if (!m)
		return -1;
	memcpy(out, m, HEADER_BYTES);
	munmap(m, map_len(name));
	return 0;
}

int hg_stub_read_slot(const char *name, uint32_t slot, unsigned char *out, size_t len)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	uint32_t k = *(uint32_t *)(void *)((unsigned char *)m + OFF_SLOT_COUNT);
	uint64_t stride = *(uint64_t *)(void *)((unsigned char *)m + OFF_SLOT_STRIDE);
	size_t ml = map_len(name);
	if (slot >= k || len > stride) {
		munmap(m, ml);
		errno = EINVAL;
		return -1;
	}
	memcpy(out, (unsigned char *)m + HEADER_BYTES + slot * stride, len);
	munmap(m, ml);
	return 0;
}

int hg_stub_set_slot_seq(const char *name, uint32_t slot, uint64_t seq)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	uint32_t k = *(uint32_t *)(void *)((unsigned char *)m + OFF_SLOT_COUNT);
	uint64_t stride = *(uint64_t *)(void *)((unsigned char *)m + OFF_SLOT_STRIDE);
	size_t ml = map_len(name);
	if (slot >= k) {
		munmap(m, ml);
		errno = EINVAL;
		return -1;
	}
	atomic_store_explicit(u64_at(m, HEADER_BYTES + slot * stride), seq, memory_order_seq_cst);
	munmap(m, ml);
	return 0;
}

int hg_stub_set_write_seq(const char *name, uint64_t seq)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	atomic_store_explicit(u64_at(m, OFF_WRITE_SEQ), seq, memory_order_seq_cst);
	atomic_fetch_add_explicit(u32_at(m, OFF_FUTEX), 1, memory_order_seq_cst);
	hg_futex_wake_all(u32_at(m, OFF_FUTEX));
	munmap(m, map_len(name));
	return 0;
}

static int inode_of(const char *prefix, const char *name, const char *suffix, ino_t *out)
{
	char path[PATH_MAX];
	struct stat st;
	if (hg_topic_path(path, sizeof path, prefix, name, suffix) < 0 || stat(path, &st) < 0)
		return -1;
	*out = st.st_ino;
	return 0;
}

int hg_stub_wake_inode(const char *name, ino_t *out)
{
	return inode_of(".", name, ".wake", out);
}

int hg_stub_dir_inode(ino_t *out)
{
	return inode_of(".", "dir", "", out);
}

int hg_stub_topic_inode(const char *name, ino_t *out)
{
	return inode_of("", name, "", out);
}

int hg_stub_unlink_topic(const char *name)
{
	char path[PATH_MAX];
	if (hg_topic_path(path, sizeof path, "", name, "") < 0)
		return -1;
	return unlink(path);
}

int hg_stub_memfd(const char *text)
{
	int fd = (int)syscall(SYS_memfd_create, "hangar-wiring", HG_MFD_CLOEXEC | HG_MFD_ALLOW_SEALING);
	if (fd < 0)
		return -1;
	size_t len = strlen(text), off = 0;
	while (off < len) {
		ssize_t w = write(fd, text + off, len - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	/* Sealed as the manager seals it: nothing can change what the module
	 * reads, and the module's own code could not either. */
	if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ---- The publish protocol, step by step -------------------------------- */

static void *map_wake_file(const char *name)
{
	char path[PATH_MAX];
	if (hg_topic_path(path, sizeof path, ".", name, ".wake") < 0)
		return NULL;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	void *m = mmap(NULL, 64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return m == MAP_FAILED ? NULL : m;
}

int hg_stub_raw_body(const char *name)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	uint32_t k = *(uint32_t *)(void *)((unsigned char *)m + OFF_SLOT_COUNT);
	uint64_t stride = *(uint64_t *)(void *)((unsigned char *)m + OFF_SLOT_STRIDE);
	uint64_t n = atomic_load_explicit(u64_at(m, OFF_WRITE_SEQ), memory_order_acquire) + 1;
	size_t slot = HEADER_BYTES + ((n - 1) & (k - 1)) * stride;
	atomic_store_explicit(u64_at(m, slot), 2 * n - 1, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
	uint64_t now = hg_now_ns();
	atomic_store_explicit(u64_at(m, slot + 8), n, memory_order_relaxed);
	atomic_store_explicit(u64_at(m, slot + 16), now, memory_order_relaxed);
	atomic_store_explicit(u32_at(m, slot + 24), 0, memory_order_relaxed);
	atomic_store_explicit(u32_at(m, slot + 28), 0, memory_order_relaxed);
	for (size_t off = 128; off < stride; off += 8)
		atomic_store_explicit(u64_at(m, slot + off), 0, memory_order_relaxed);
	atomic_store_explicit(u64_at(m, slot), 2 * n, memory_order_release);
	atomic_store_explicit(u64_at(m, OFF_WRITE_SEQ), n, memory_order_seq_cst);
	atomic_store_explicit(u64_at(m, OFF_LAST_PUBLISH), now, memory_order_relaxed);
	munmap(m, map_len(name));
	return 0;
}

int hg_stub_raw_exchange_flag(const char *name, uint32_t *was)
{
	void *w = map_wake_file(name);
	if (!w)
		return -1;
	*was = atomic_exchange_explicit(u32_at(w, 0), 0, memory_order_seq_cst);
	munmap(w, 64);
	return 0;
}

int hg_stub_raw_increment(const char *name)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	atomic_fetch_add_explicit(u32_at(m, OFF_FUTEX), 1, memory_order_seq_cst);
	munmap(m, map_len(name));
	return 0;
}

int hg_stub_raw_wake(const char *name)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	hg_futex_wake_all(u32_at(m, OFF_FUTEX));
	munmap(m, map_len(name));
	return 0;
}

int hg_stub_raw_read_word(const char *name, uint32_t *out)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	*out = atomic_load_explicit(u32_at(m, OFF_FUTEX), memory_order_acquire);
	munmap(m, map_len(name));
	return 0;
}

int hg_stub_raw_set_flag(const char *name)
{
	void *w = map_wake_file(name);
	if (!w)
		return -1;
	atomic_store_explicit(u32_at(w, 0), 1, memory_order_seq_cst);
	munmap(w, 64);
	return 0;
}

int hg_stub_raw_waitv(const char *name, uint32_t val, int timeout_ms)
{
	void *m = map_header(name);
	if (!m)
		return -1;
	struct hg_futex_waitv wv = { .val = val, .uaddr = (uint64_t)(uintptr_t)u32_at(m, OFF_FUTEX),
				     .flags = HG_FUTEX_32 };
	uint64_t until = hg_now_ns() + (uint64_t)timeout_ms * 1000000u;
	struct timespec ts = { .tv_sec = (time_t)(until / 1000000000u), .tv_nsec = (long)(until % 1000000000u) };
	long r = syscall(SYS_futex_waitv, &wv, 1u, 0u, &ts, CLOCK_MONOTONIC);
	int rc = r < 0 ? errno : 0;
	munmap(m, map_len(name));
	return rc;
}
