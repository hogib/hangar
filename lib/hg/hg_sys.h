// SPDX-License-Identifier: LGPL-3.0-or-later
/* Kernel interfaces without a libc wrapper, declared by hand rather than taken
 * from the <linux/...> headers, which clash with the libc's own (musl's in
 * particular). Compiles against glibc and musl alike: nothing here depends on
 * a libc beyond <sys/syscall.h>'s SYS_* numbers, and the numbers of the
 * syscalls added since 5.x are the same on every architecture. */
#ifndef HG_SYS_H
#define HG_SYS_H

#include <stdint.h>
#include <sys/syscall.h>

/* futex(2) operations: the libc has no wrapper, and <linux/futex.h> is the
 * header that clashes. */
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
#define HG_FUTEX_PRIVATE_FLAG 128u

/* futex_waitv(2), Linux 5.16: one sleep on up to 128 futexes. Topic futexes
 * are shared between processes (the files are mapped by every module), so they
 * never carry FUTEX_PRIVATE_FLAG; the library's own words in process memory do. */
#define HG_FUTEX_32 2u
#define HG_FUTEX_32_PRIVATE (HG_FUTEX_32 | HG_FUTEX_PRIVATE_FLAG)
#define HG_FUTEX_WAITV_MAX 128

#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif

struct hg_futex_waitv {
	uint64_t val;
	uint64_t uaddr;
	uint32_t flags;
	uint32_t reserved;
};

/* memfd_create(2) flags, for the sealed wiring the manager hands a module and
 * for tests that stand in for it. The syscall number comes from the libc's
 * <sys/syscall.h> on every supported target (x86-64, AArch64, ARMv7). */
#define HG_MFD_CLOEXEC 1u
#define HG_MFD_ALLOW_SEALING 2u

/* fcntl(2) seals, in case the libc headers lack them (older musl). */
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 1u
#define F_SEAL_SHRINK 2u
#define F_SEAL_GROW 4u
#define F_SEAL_WRITE 8u
#endif

#endif
