// SPDX-License-Identifier: LGPL-3.0-or-later
/* Shared by lib/hg's suites: a private topic directory on tmpfs (futexes and
 * mappings behave there as they do under /dev/shm/hangar), the depth type of
 * docs/01, a 256-byte test type whose payload can be checked for tears, and
 * small helpers around time and child processes. */
#ifndef HG_TEST_H
#define HG_TEST_H

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hg_manager_stub.h"
#include "hg_topic.h"
#include "test_harness.h"

/* msg/depth.msg as docs/01 writes it. */
static const char DEPTH_SCHEMA[] =
	"# msg/depth.msg\n"
	"f32 depth_m         # m, positive down\n"
	"f32 temp_c          # degrees Celsius\n"
	"f32 pressure_mbar   # mbar, absolute\n";
static const char DEPTH_CANONICAL[] = "f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n";

struct depth {
	float depth_m;
	float temp_c;
	float pressure_mbar;
};

/* 32 words, each set to the message's seq by fill(): a torn copy shows. */
static const char WORDS_SCHEMA[] = "u64 w[32]   # test payload\n";
#define WORDS_BYTES 256

/* Descriptors built once per process from the texts above. */
static struct hg_schema depth_parsed, words_parsed;
static const struct hg_type *DEPTH_TYPE, *WORDS_TYPE;

static inline void fill(uint64_t *buf, uint64_t seq)
{
	for (int i = 0; i < WORDS_BYTES / 8; i++)
		buf[i] = seq;
}

static inline int intact(const uint64_t *buf, const struct hg_info *info)
{
	for (int i = 0; i < WORDS_BYTES / 8; i++)
		if (buf[i] != info->seq)
			return 0;
	return 1;
}

/* The base of the test topic directories: tmpfs. */
static inline const char *test_base(void)
{
	static char base[256];
	if (!base[0]) {
		const char *env = getenv("HANGAR_TEST_BASE");
		if (env && *env)
			snprintf(base, sizeof base, "%s", env);
		else
			snprintf(base, sizeof base, "/dev/shm/hangar-test-%ld", (long)getuid());
	}
	return base;
}

static char topic_dir[512];

/* Makes a fresh $HANGAR_TOPIC_DIR for this suite and parses the types. Exits
 * on failure: nothing else could run. */
static inline void hg_test_setup(const char *tag)
{
	if (hg_stub_fresh_dir(test_base(), tag, topic_dir, sizeof topic_dir) < 0) {
		fprintf(stderr, "cannot make a topic directory under %s: %s\n", test_base(),
			strerror(errno));
		exit(2);
	}
	if (hg_schema_parse(DEPTH_SCHEMA, strlen(DEPTH_SCHEMA), &depth_parsed, NULL, 0) < 0 ||
	    hg_schema_parse(WORDS_SCHEMA, strlen(WORDS_SCHEMA), &words_parsed, NULL, 0) < 0) {
		fprintf(stderr, "the test schemas do not parse\n");
		exit(2);
	}
	DEPTH_TYPE = &depth_parsed.type;
	WORDS_TYPE = &words_parsed.type;
}

static inline void hg_test_teardown(void)
{
	hg_stub_clear_dir();
	rmdir(topic_dir);
	rmdir(test_base()); /* gone once the last suite is done; busy otherwise */
}

static inline void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static inline uint64_t ms_since(uint64_t t0)
{
	return (hg_now_ns() - t0) / 1000000u;
}

/* Shared memory for a forked child's results. */
static inline void *shared_zeroed(size_t len)
{
	void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		perror("mmap");
		exit(2);
	}
	memset(m, 0, len);
	return m;
}

/* Waits for a child; its exit status, or -1. */
static inline int wait_child(pid_t pid)
{
	int st;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

#endif
