// SPDX-License-Identifier: GPL-3.0-or-later
/* What the harnesses share. A harness is one file, fuzz/fuzz_<name>.c, that
 * defines LLVMFuzzerTestOneInput; it is linked with libFuzzer (make fuzz) or
 * with fuzz/standalone.c (make check-fuzz-corpus). */
#ifndef HANGAR_FUZZ_H
#define HANGAR_FUZZ_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* A property a harness checks beyond "no crash": abort() is what both the
 * fuzzer and the regression runner report. */
#define FZ_CHECK(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "%s:%d: FZ_CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			abort(); \
		} \
	} while (0)

/* How far an input got, on stderr when FUZZ_TRACE is set in the environment:
 * for whoever writes a seed and wants to know it is accepted, e.g.
 * `FUZZ_TRACE=1 builddir/tests-debugoptimized/fuzzreg_wiring fuzz/corpus/wiring/seed-every-line`. */
static inline int fz_trace(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("FUZZ_TRACE") != NULL;
	return on;
}
#define FZ_NOTE(...) \
	do { \
		if (fz_trace()) { \
			fprintf(stderr, "trace: " __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

/* The input as a NUL-terminated copy of exactly size + 1 bytes, for parsers
 * that take text and a length: an overread past the length lands on the NUL
 * and then past the block. The caller frees it. */
static inline char *fz_text(const uint8_t *data, size_t size)
{
	char *t = malloc(size + 1);
	if (!t)
		abort();
	if (size)
		memcpy(t, data, size);
	t[size] = 0;
	return t;
}

/* The input as a file, for readers that take a path and cannot be handed a
 * buffer: an anonymous file (memfd), named by its /proc/self/fd path, so
 * nothing touches the disk. Returns the descriptor; the caller closes it. */
static inline int fz_file(const uint8_t *data, size_t size, char path[64])
{
	int fd = memfd_create("fuzz", 0);
	if (fd < 0)
		abort();
	size_t off = 0;
	while (off < size) {
		ssize_t k = write(fd, data + off, size - off);
		if (k <= 0)
			abort();
		off += (size_t)k;
	}
	snprintf(path, 64, "/proc/self/fd/%d", fd);
	return fd;
}

#endif
