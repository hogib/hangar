// SPDX-License-Identifier: GPL-3.0-or-later
/* hgr_open where size_t is 32 bits and off_t 64 (a 32-bit build with
 * _FILE_OFFSET_BITS=64, the Makefile's default): a recording of 4 GiB or
 * more cannot be mapped, and must be refused with EFBIG. Its size used to
 * be cast to size_t, so the file was mapped with its length modulo 4 GiB
 * and read as a shorter recording, with no error.
 *
 * Built by recorder.mk's check-rec_hgr32 with -m32 from this file and the
 * two library files hg_record.c needs; skipped where the compiler has no
 * 32-bit libc. The file is sparse, on tmpfs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hg_record.h"
#include "test_harness.h"

uint64_t hg_now_ns(void) /* the one symbol hg_record.c takes from hg_topic.c */
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static char path[400];

static void test_recording_past_4gib(void)
{
	CHECK_EQ_INT(sizeof(size_t), 4);
	CHECK_EQ_INT(sizeof(off_t), 8);
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	snprintf(m.name, sizeof m.name, "big.t");
	m.payload_bytes = 896; /* 1024-byte records */
	struct hgr_header h;
	hgr_header_init(&h, &m, "u8 d[896]\n", 10, HGR_FULL, 1, "full", "s", 0);
	struct hgr_writer w;
	unlink(path);
	CHECK_EQ_INT(hgr_create(&w, path, &h), 0);
	hgr_close(&w);
	/* Three records: a file any build reads. */
	CHECK_EQ_INT(truncate(path, (off_t)HGR_HEADER_BYTES + 3 * 1024), 0);
	struct hgr_reader r;
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.count, 3);
	hgr_reader_close(&r);
	/* 4 GiB of records and three more: modulo 4 GiB it is the same file. */
	uint64_t records = (4ull << 30) / 1024 + 3;
	CHECK_EQ_INT(truncate(path, (off_t)(HGR_HEADER_BYTES + records * 1024)), 0);
	errno = 0;
	int rc = hgr_open(&r, path);
	int e = errno;
	printf("    %llu records, %llu bytes: hgr_open -> %d (%s)%s\n", (unsigned long long)records,
	       (unsigned long long)(HGR_HEADER_BYTES + records * 1024), rc, rc < 0 ? strerror(e) : "ok",
	       rc == 0 && r.count != records ? ", with the wrong count" : "");
	CHECK_EQ_INT(rc, -1);
	CHECK_EQ_INT(e, EFBIG);
	if (rc == 0)
		hgr_reader_close(&r);
	unlink(path);
}

int main(void)
{
	const char *base = getenv("HANGAR_TEST_BASE");
	char dir[300];
	if (base && *base)
		snprintf(dir, sizeof dir, "%s", base);
	else
		snprintf(dir, sizeof dir, "/dev/shm/hangar-test-%ld", (long)getuid());
	mkdir(dir, 0755);
	snprintf(path, sizeof path, "%s/rec-hgr32-%ld.hgr", dir, (long)getpid());
	RUN_TEST(test_recording_past_4gib);
	rmdir(dir); /* gone once the last suite is done; busy otherwise */
	TEST_MAIN_END();
}
