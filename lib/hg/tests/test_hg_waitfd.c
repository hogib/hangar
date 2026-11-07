// SPDX-License-Identifier: LGPL-3.0-or-later
// hg_wait_fd (docs/01 "Waiting together with file descriptors"): the
// eventfd is signalled on news, a message published while the module reads
// is never missed, the helper's set follows an absent topic that appears and
// a replaced file that is reopened, and the limits. The helper thread is
// the library's only thread; the suite forks its publisher before starting
// it, so no process ever forks with the thread alive.
#include "hg_test.h"

#include <poll.h>
#include <sys/eventfd.h>

static uint64_t buf[32];
static struct hg_info info;

static hg_pub *open_words(const char *name, uint32_t slots)
{
	struct hg_pub_opts o = { .slots = slots };
	hg_pub *p = hg_pub_open(name, WORDS_TYPE, &o);
	if (!p) {
		printf("    hg_pub_open(%s): %s\n", name, strerror(errno));
		exit(2);
	}
	return p;
}

static void pub_one(hg_pub *p)
{
	uint64_t seq = hg_pub_next_seq(p);
	fill(buf, seq);
	hg_publish(p, buf, sizeof buf, seq);
}

/* Waits for the eventfd with a timeout; 1 if it was signalled (and drains
 * it), 0 on timeout. */
static int signalled(int efd, int timeout_ms)
{
	struct pollfd pfd = { .fd = efd, .events = POLLIN };
	int r = poll(&pfd, 1, timeout_ms);
	if (r <= 0)
		return 0;
	uint64_t v;
	(void)read(efd, &v, sizeof v);
	return 1;
}

static void test_signalled_on_news(void)
{
	hg_pub *p = open_words("fd.basic", 16);
	hg_sub *s = hg_sub_open("fd.basic", WORDS_TYPE);
	int efd = hg_wait_fd(&s, 1);
	CHECK(efd >= 0);
	// The helper's first pass signals once (nothing is lost by a read that
	// finds nothing); after that, quiet.
	(void)signalled(efd, 500);
	CHECK_EQ_INT(signalled(efd, 100), 0);
	pub_one(p);
	CHECK_EQ_INT(signalled(efd, 1000), 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	CHECK_EQ_INT(signalled(efd, 100), 0);
	// A close is news too.
	hg_pub_close(p);
	CHECK_EQ_INT(signalled(efd, 1000), 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_CLOSED);
	hg_wait_fd_close();
	hg_sub_close(s);
}

struct pub_shared {
	atomic_int go;
	atomic_int done;
};

static void test_no_missed_message(void)
{
	// A publisher process runs 5000 messages at random short intervals
	// while the module reads on each signal; every message is read, with
	// no gap, and the last one arrives without a further publish to flush
	// it: a message published while the module was reading is seen by the
	// helper's next snapshot. The ring is deep (1024) and the gaps have a
	// 20 us floor: what is measured is the snapshot, not the slack of a
	// reader descheduled on a loaded machine.
	struct pub_shared *sh = shared_zeroed(sizeof *sh);
	hg_pub *p = open_words("fd.stream", 1024);
	const int N = 5000;
	pid_t pid = fork();
	if (pid == 0) {
		while (!atomic_load(&sh->go))
			sleep_ms(1);
		uint64_t x = 0x9e3779b97f4a7c15ull;
		for (int i = 0; i < N; i++) {
			pub_one(p);
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			struct timespec ts = { 0, 20000 + (long)(x % 100001) };
			nanosleep(&ts, NULL);
		}
		atomic_store(&sh->done, 1);
		_exit(0);
	}
	hg_sub *s = hg_sub_open("fd.stream", WORDS_TYPE);
	int efd = hg_wait_fd(&s, 1);
	CHECK(efd >= 0);
	atomic_store(&sh->go, 1);
	uint64_t got = 0, gaps = 0, torn = 0, last = 0;
	uint64_t t0 = hg_now_ns();
	while (last < (uint64_t)N && ms_since(t0) < 20000) {
		if (!signalled(efd, 2000)) {
			printf("    no signal for 2 s at %llu messages\n", (unsigned long long)got);
			break;
		}
		while (hg_next(s, buf, sizeof buf, &info) == HG_MSG) {
			got++;
			gaps += info.gap;
			torn += !intact(buf, &info);
			last = info.seq;
		}
	}
	CHECK_EQ_INT(wait_child(pid), 0);
	printf("    stream: %d sent, %llu read, gaps %llu, torn %llu\n", N, (unsigned long long)got,
	       (unsigned long long)gaps, (unsigned long long)torn);
	CHECK_EQ_INT(got, N);
	CHECK_EQ_INT(gaps, 0);
	CHECK_EQ_INT(torn, 0);
	CHECK_EQ_INT(last, N);
	hg_wait_fd_close();
	hg_sub_close(s);
	hg_pub_close(p);
	munmap(sh, sizeof *sh);
}

static void test_set_follows_attach_and_reopen(void)
{
	// An absent topic in the set: its creation wakes the helper through the
	// directory word; the module's read attaches it (the set changes under
	// the helper, which acknowledges before the module goes on); the next
	// publish signals. Then the file is replaced, with no other traffic,
	// and the next publish to the new file still signals.
	hg_sub *absent = hg_sub_open("fd.absent", WORDS_TYPE);
	hg_pub *other = open_words("fd.other", 16);
	hg_sub *os = hg_sub_open("fd.other", WORDS_TYPE);
	hg_sub *subs[2] = { absent, os };
	int efd = hg_wait_fd(subs, 2);
	CHECK(efd >= 0);
	(void)signalled(efd, 500);
	CHECK_EQ_INT(signalled(efd, 100), 0);
	hg_pub *p = open_words("fd.absent", 16);
	CHECK_EQ_INT(signalled(efd, 1000), 1); // the directory word
	CHECK_EQ_INT(hg_next(absent, buf, sizeof buf, &info), HG_NONE); // attached now
	(void)signalled(efd, 100); // the set change makes the helper take a new snapshot; it may signal
	CHECK_EQ_INT(signalled(efd, 100), 0);
	pub_one(p);
	CHECK_EQ_INT(signalled(efd, 1000), 1);
	CHECK_EQ_INT(hg_next(absent, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1);
	// Replacement.
	hg_pub *p2 = open_words("fd.absent", 16);
	CHECK_EQ_INT(signalled(efd, 1000), 1); // bit 2 on the old file
	CHECK_EQ_INT(hg_next(absent, buf, sizeof buf, &info), HG_NONE); // reopened
	CHECK_EQ_INT(hg_sub_reopens(absent), 1);
	(void)signalled(efd, 100); // again a set change
	CHECK_EQ_INT(signalled(efd, 100), 0);
	pub_one(p2);
	CHECK_EQ_INT(signalled(efd, 1000), 1);
	CHECK_EQ_INT(hg_next(absent, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 2);
	CHECK_EQ_INT(info.gap, 0);
	// The other topic still signals.
	pub_one(other);
	CHECK_EQ_INT(signalled(efd, 1000), 1);
	CHECK_EQ_INT(hg_next(os, buf, sizeof buf, &info), HG_MSG);
	// Closing a subscription in the set is a set change too.
	hg_sub_close(os);
	pub_one(p2);
	CHECK_EQ_INT(signalled(efd, 1000), 1);
	CHECK_EQ_INT(hg_next(absent, buf, sizeof buf, &info), HG_MSG);
	hg_wait_fd_close();
	hg_sub_close(absent);
	hg_pub_close(p);
	hg_pub_close(p2);
	hg_pub_close(other);
}

static void test_limits(void)
{
	static hg_sub *subs[127];
	char name[48];
	for (int i = 0; i < 127; i++) {
		snprintf(name, sizeof name, "fd.many%d", i);
		subs[i] = hg_sub_open(name, NULL);
	}
	CHECK_EQ_INT(hg_wait_fd(subs, 127), -1);
	CHECK_EQ_INT(errno, E2BIG);
	int efd = hg_wait_fd(subs, 126);
	CHECK(efd >= 0);
	CHECK_EQ_INT(hg_wait_fd(subs, 1), -1); // one helper per module
	CHECK_EQ_INT(errno, EBUSY);
	hg_wait_fd_close();
	for (int i = 0; i < 127; i++)
		hg_sub_close(subs[i]);
}

int main(void)
{
	hg_test_setup("waitfd");
	RUN_TEST(test_signalled_on_news);
	RUN_TEST(test_no_missed_message);
	RUN_TEST(test_set_follows_attach_and_reopen);
	RUN_TEST(test_limits);
	hg_test_teardown();
	TEST_MAIN_END();
}
