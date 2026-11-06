/* Kernel interfaces without a libc wrapper, declared here rather than taken
 * from <linux/...> headers, which clash with the libc's. */
#ifndef HG_SYS_H
#define HG_SYS_H

#include <stdint.h>
#include <sys/syscall.h>

#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
#define HG_FUTEX_PRIVATE_FLAG 128
#define HG_FUTEX_32 2u /* futex_waitv: 32-bit futex, shared (no FUTEX_PRIVATE_FLAG) */
#define HG_FUTEX_32_PRIVATE (HG_FUTEX_32 | HG_FUTEX_PRIVATE_FLAG)

#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

struct hg_futex_waitv {
	uint64_t val;
	uint64_t uaddr;
	uint32_t flags;
	uint32_t reserved;
};

struct hg_clone_args {
	uint64_t flags;
	uint64_t pidfd;
	uint64_t child_tid;
	uint64_t parent_tid;
	uint64_t exit_signal;
	uint64_t stack;
	uint64_t stack_size;
	uint64_t tls;
	uint64_t set_tid;
	uint64_t set_tid_size;
	uint64_t cgroup;
};

#define HG_CLONE_PIDFD 0x1000u
#define HG_CLOSE_RANGE_CLOEXEC 4u
#define HG_P_PIDFD 3

#endif
