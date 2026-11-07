// SPDX-License-Identifier: LGPL-3.0-or-later
// The spike's torn test as a suite: one writer publishing flat out into a
// 4-slot ring while four reader processes alternate hg_latest and hg_next,
// each copy checked word by word against its seq. The writer overtakes the
// readers mid-copy constantly at this ring depth; the seqlock must discard
// every such copy. The count of torn copies must be exactly 0.
#include "hg_test.h"

#define READERS 4
#define SECONDS 3

struct shared {
	atomic_int ready;
	atomic_int stop;
	atomic_int progress; /* reads so far, by the last reader to count one */
	unsigned long long read[READERS], torn[READERS], busy[READERS], gaps[READERS];
};

static void reader(struct shared *sh, int id)
{
	static uint64_t buf[32];
	hg_sub *s = hg_sub_open("torn.words", WORDS_TYPE);
	if (!s)
		_exit(1);
	unsigned long long nread = 0, torn = 0, busy = 0, gaps = 0;
	atomic_fetch_add(&sh->ready, 1);
	for (unsigned long i = 0; !atomic_load_explicit(&sh->stop, memory_order_relaxed); i++) {
		struct hg_info info;
		enum hg_result r = (i & 1) ? hg_next(s, buf, sizeof buf, &info)
					   : hg_latest(s, buf, sizeof buf, &info);
		if (r == HG_MSG) {
			nread++;
			atomic_store_explicit(&sh->progress, (int)nread, memory_order_relaxed);
			torn += !intact(buf, &info);
			gaps += info.gap;
		} else if (r == HG_BUSY) {
			busy++;
		} else if (r == HG_ERROR) {
			_exit(3);
		}
	}
	sh->read[id] = nread;
	sh->torn[id] = torn;
	sh->busy[id] = busy;
	sh->gaps[id] = gaps;
	hg_sub_close(s);
	_exit(0);
}

static void test_no_torn_reads(void)
{
	static uint64_t buf[32];
	struct shared *sh = shared_zeroed(sizeof *sh);
	struct hg_pub_opts o = { .slots = 4 };
	hg_pub *p = hg_pub_open("torn.words", WORDS_TYPE, &o);
	CHECK(p != NULL);
	if (!p)
		return;
	pid_t pids[READERS];
	for (int i = 0; i < READERS; i++) {
		pids[i] = fork();
		CHECK(pids[i] >= 0);
		if (pids[i] == 0)
			reader(sh, i);
	}
	uint64_t t0 = hg_now_ns();
	while (atomic_load(&sh->ready) < READERS && ms_since(t0) < 5000)
		sleep_ms(1);
	CHECK_EQ_INT(atomic_load(&sh->ready), READERS);
	uint64_t end = hg_now_ns() + SECONDS * 1000000000ull;
	uint64_t n = 0;
	do {
		for (int k = 0; k < 1024; k++) {
			n = hg_pub_next_seq(p);
			fill(buf, n);
			hg_publish(p, buf, sizeof buf, n);
		}
	} while (hg_now_ns() < end);
	atomic_store(&sh->stop, 1);
	unsigned long long read = 0, torn = 0, busy = 0, gaps = 0;
	for (int i = 0; i < READERS; i++) {
		CHECK_EQ_INT(wait_child(pids[i]), 0);
		read += sh->read[i];
		torn += sh->torn[i];
		busy += sh->busy[i];
		gaps += sh->gaps[i];
	}
	hg_pub_close(p);
	printf("    %d s, %d readers, 4 slots: published %llu, read %llu, torn %llu, busy %llu, gaps %llu\n",
	       SECONDS, READERS, (unsigned long long)n, read, torn, busy, gaps);
	CHECK_EQ_INT(torn, 0);
	CHECK(read > 1000);
	CHECK(n > 10000);
	munmap(sh, sizeof *sh);
}

static void test_every_slot_count(void)
{
	// Short runs at the other ring depths: a reader kept behind by a
	// writer that fills the ring repeatedly between its reads never gets
	// a mixed copy, whatever the depth.
	static uint64_t buf[32];
	static const uint32_t slots[] = { 2, 8, 64, 1024 };
	for (size_t si = 0; si < sizeof slots / sizeof slots[0]; si++) {
		struct shared *sh = shared_zeroed(sizeof *sh);
		struct hg_pub_opts o = { .slots = slots[si] };
		hg_pub *p = hg_pub_open("torn.words", WORDS_TYPE, &o);
		CHECK(p != NULL);
		if (!p)
			return;
		pid_t pid = fork();
		if (pid == 0)
			reader(sh, 0);
		uint64_t t0 = hg_now_ns();
		while (atomic_load(&sh->ready) < 1 && ms_since(t0) < 5000)
			sleep_ms(1);
		/* 300 ms of writing, and longer, up to 3 s, on a machine so loaded
		 * that the reader has not had 100 reads by then: the property is
		 * no torn read, the count only shows the reader ran. */
		uint64_t start = hg_now_ns(), end = start + 300000000ull;
		do {
			for (int k = 0; k < 256; k++) {
				uint64_t n = hg_pub_next_seq(p);
				fill(buf, n);
				hg_publish(p, buf, sizeof buf, n);
			}
		} while (hg_now_ns() < end ||
			 (atomic_load(&sh->progress) <= 100 && hg_now_ns() < start + 3000000000ull));
		atomic_store(&sh->stop, 1);
		CHECK_EQ_INT(wait_child(pid), 0);
		CHECK_EQ_INT(sh->torn[0], 0);
		CHECK(sh->read[0] > 100);
		hg_pub_close(p);
		munmap(sh, sizeof *sh);
	}
}

int main(void)
{
	hg_test_setup("torn");
	RUN_TEST(test_no_torn_reads);
	RUN_TEST(test_every_slot_count);
	hg_test_teardown();
	TEST_MAIN_END();
}
