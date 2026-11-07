// SPDX-License-Identifier: LGPL-3.0-or-later
// hg_wait (docs/01 "Waiting"): the publish-order guard, with the two orders
// of step 4 played out step by step; the spike's lost_wake run with the real
// library; liveness on a dead or closed topic; absent topics and the
// directory word; the interrupt word; ready sets over several topics; glob
// subscriptions polled at 10 ms; the 127 limit.
#include "hg_test.h"

#include <pthread.h>

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

/* ---- The publish-order guard ------------------------------------------- */

/* The reader's sleep, in a thread, so the writer's steps can be placed
 * around it: it reports what futex_waitv returned. */
struct sleeper {
	const char *name;
	uint32_t val;
	int timeout_ms;
	int result;
};

static void *sleeper_run(void *arg)
{
	struct sleeper *s = arg;
	s->result = hg_stub_raw_waitv(s->name, s->val, s->timeout_ms);
	return NULL;
}

/* One interleaving, with step 4 in the given order. The reader has just
 * returned message n; the writer has published n (steps 1 to 3) and does its
 * first half of step 4; the reader reads the word, sets the flag and finds
 * nothing new; the writer does the second half; the reader sleeps on the
 * word it read. Returns what that sleep returned. Then a further publish in
 * the same order, and whether the reader is awake after it. */
static void play(int swapped, int *first_sleep, int *woken_by_next)
{
	const char *name = swapped ? "wt.swapped" : "wt.ordered";
	struct hg_pub_opts o = { .slots = 16 };
	hg_pub *p = hg_pub_open(name, WORDS_TYPE, &o);
	hg_pub_close(p); /* only the files are wanted: the steps are played by hand */
	uint32_t was = 9;
	/* Steps 1 to 3 of message 1; the reader is taken to have read it. */
	CHECK_EQ_INT(hg_stub_raw_body(name), 0);
	if (swapped)
		CHECK_EQ_INT(hg_stub_raw_increment(name), 0);
	else
		CHECK_EQ_INT(hg_stub_raw_exchange_flag(name, &was), 0);
	/* The reader: word, flag, re-check (it has message 1: nothing new). */
	uint32_t v;
	CHECK_EQ_INT(hg_stub_raw_read_word(name, &v), 0);
	CHECK_EQ_INT(hg_stub_raw_set_flag(name), 0);
	/* The writer's second half. */
	if (swapped) {
		CHECK_EQ_INT(hg_stub_raw_exchange_flag(name, &was), 0);
		if (was)
			hg_stub_raw_wake(name); /* nobody is asleep yet */
	} else {
		CHECK_EQ_INT(hg_stub_raw_increment(name), 0);
		if (was)
			hg_stub_raw_wake(name);
	}
	/* The reader sleeps on the value it read. */
	struct sleeper sl = { name, v, 300, -1 };
	pthread_t t;
	pthread_create(&t, NULL, sleeper_run, &sl);
	pthread_join(t, NULL);
	*first_sleep = sl.result;
	if (sl.result == EAGAIN) {
		/* As hg_wait does: look again, nothing new, flag, sleep on the
		 * current word. */
		CHECK_EQ_INT(hg_stub_raw_read_word(name, &v), 0);
		CHECK_EQ_INT(hg_stub_raw_set_flag(name), 0);
	}
	/* Asleep now (on v in the ordered case; in the swapped case it slept
	 * through the timeout above, so it is put to sleep on the current
	 * word, which is what it would still be waiting on). */
	if (swapped)
		CHECK_EQ_INT(hg_stub_raw_read_word(name, &v), 0);
	sl = (struct sleeper){ name, v, 600, -1 };
	pthread_create(&t, NULL, sleeper_run, &sl);
	sleep_ms(50); /* let it queue */
	/* The next publish, in the same order. */
	CHECK_EQ_INT(hg_stub_raw_body(name), 0);
	if (swapped) {
		CHECK_EQ_INT(hg_stub_raw_increment(name), 0);
		CHECK_EQ_INT(hg_stub_raw_exchange_flag(name, &was), 0);
	} else {
		CHECK_EQ_INT(hg_stub_raw_exchange_flag(name, &was), 0);
		CHECK_EQ_INT(hg_stub_raw_increment(name), 0);
	}
	if (was)
		hg_stub_raw_wake(name);
	pthread_join(t, NULL);
	*woken_by_next = sl.result == 0;
}

static void test_publish_order_guard(void)
{
	int first, woken;
	/* The design's order: the reader's first sleep fails at once (the word
	 * moved after it read it), it re-checks, and the next publish finds its
	 * flag and wakes it. */
	play(0, &first, &woken);
	CHECK_EQ_INT(first, EAGAIN);
	CHECK_EQ_INT(woken, 1);
	/* The swapped order: the writer's exchange took the fresh flag while
	 * the reader was not yet asleep, the reader slept on the final value
	 * until its timeout, and the next publish found the flag clear and
	 * woke nobody. This is the test that fails with step 4 swapped. */
	play(1, &first, &woken);
	CHECK_EQ_INT(first, ETIMEDOUT);
	CHECK_EQ_INT(woken, 0);
}

/* ---- lost_wake with the real library ----------------------------------- */

struct lost_shared {
	atomic_int ready;
	unsigned long long msgs, gaps, timeouts, torn, wakes;
};

static void lost_reader(struct lost_shared *sh)
{
	hg_sub *s = hg_sub_open("wt.lost", WORDS_TYPE);
	if (!s)
		_exit(1);
	atomic_store(&sh->ready, 1);
	for (int done = 0; !done;) {
		unsigned r = hg_wait(&s, 1, 5000);
		if (r == (unsigned)-1)
			_exit(1);
		if (r == 0)
			sh->timeouts++;
		else
			sh->wakes++;
		for (;;) {
			enum hg_result res = hg_next(s, buf, sizeof buf, &info);
			sh->gaps += info.gap;
			if (res == HG_CLOSED)
				done = 1;
			if (res != HG_MSG)
				break;
			sh->msgs++;
			sh->torn += !intact(buf, &info);
		}
	}
	hg_sub_close(s);
	_exit(0);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return rng_state = x;
}

static void test_lost_wake(void)
{
	// What is measured is wake-ups, not slack: a 1024-slot ring and a 20 us
	// floor on the gaps give the reader 20 ms of slack against a loaded
	// machine, so a gap here would be a reader never woken, not one late.
	struct lost_shared *sh = shared_zeroed(sizeof *sh);
	hg_pub *p = open_words("wt.lost", 1024);
	pid_t pid = fork();
	if (pid == 0)
		lost_reader(sh);
	uint64_t t0 = hg_now_ns();
	while (!atomic_load(&sh->ready) && ms_since(t0) < 5000)
		sleep_ms(1);
	const unsigned long long N = 5000;
	for (unsigned long long i = 0; i < N; i++) {
		pub_one(p);
		long gap_ns = 20000 + (long)(rng() % 180001);
		struct timespec ts = { 0, gap_ns };
		while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
			;
	}
	hg_pub_close(p);
	CHECK_EQ_INT(wait_child(pid), 0);
	printf("    lost_wake: sent %llu, got %llu, gaps %llu, timeouts %llu, torn %llu, wake-ups %llu\n", N,
	       sh->msgs, sh->gaps, sh->timeouts, sh->torn, sh->wakes);
	CHECK_EQ_INT(sh->msgs, N);
	CHECK_EQ_INT(sh->gaps, 0);
	CHECK_EQ_INT(sh->timeouts, 0);
	CHECK_EQ_INT(sh->torn, 0);
	munmap(sh, sizeof *sh);
}

/* ---- Liveness ---------------------------------------------------------- */

static void test_liveness_on_dead_and_closed(void)
{
	hg_pub *p = open_words("wt.dead", 16);
	hg_sub *s = hg_sub_open("wt.dead", WORDS_TYPE);
	pub_one(p);
	CHECK_EQ_INT(hg_wait(&s, 1, 0), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 100), 0);
	CHECK(ms_since(t0) >= 100);
	CHECK_EQ_INT(hg_stub_mark_died("wt.dead"), 0);
	// Reported once...
	CHECK_EQ_INT(hg_wait(&s, 1, 1000), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_CLOSED);
	// ...then the reader sleeps for its whole timeout: no spinning.
	t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 200), 0);
	CHECK(ms_since(t0) >= 200);
	CHECK(ms_since(t0) < 400);
	hg_sub_close(s);
	hg_pub_close(p);
	// The same for a close.
	p = open_words("wt.closed", 16);
	s = hg_sub_open("wt.closed", WORDS_TYPE);
	hg_pub_close(p);
	CHECK_EQ_INT(hg_wait(&s, 1, 1000), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_CLOSED);
	t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 100), 0);
	CHECK(ms_since(t0) >= 100);
	hg_sub_close(s);
}

/* ---- Absent topics and the directory word ------------------------------ */

static void test_absent_topic_appears(void)
{
	hg_sub *s = hg_sub_open("wt.absent", WORDS_TYPE);
	CHECK(s != NULL);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_ABSENT);
	// Nothing to wake it: the whole timeout passes, with no polling.
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 100), 0);
	CHECK(ms_since(t0) >= 100);
	// A child creates the topic after 100 ms: the directory word wakes
	// the waiter long before its 3 s timeout, and the message is there.
	pid_t pid = fork();
	if (pid == 0) {
		sleep_ms(100);
		hg_pub *p = open_words("wt.absent", 16);
		pub_one(p);
		sleep_ms(100);
		hg_pub_close(p);
		_exit(0);
	}
	t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 3000), HG_READY_TOPICS);
	uint64_t took = ms_since(t0);
	CHECK(took >= 90 && took < 1000);
	// The topic that appeared is news whether or not the message was there
	// yet; the newest message is returned once.
	enum hg_result r = hg_next(s, buf, sizeof buf, &info);
	CHECK(r == HG_NONE || r == HG_MSG);
	if (r == HG_NONE) {
		CHECK_EQ_INT(hg_wait(&s, 1, 3000), HG_READY_TOPICS);
		CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	}
	CHECK_EQ_INT(info.seq, 1);
	// The close that follows is news.
	CHECK_EQ_INT(hg_wait(&s, 1, 3000), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_CLOSED);
	CHECK_EQ_INT(wait_child(pid), 0);
	hg_sub_close(s);
	// Several absent topics at once, one appearing.
	hg_sub *subs[3] = { hg_sub_open("wt.a1", NULL), hg_sub_open("wt.a2", NULL), hg_sub_open("wt.a3", NULL) };
	pid = fork();
	if (pid == 0) {
		sleep_ms(50);
		hg_pub *p = open_words("wt.a2", 16);
		pub_one(p);
		_exit(0);
	}
	unsigned char ready[3];
	CHECK_EQ_INT(hg_wait_ready(subs, 3, 3000, ready), HG_READY_TOPICS);
	CHECK_EQ_INT(ready[0], 0);
	CHECK_EQ_INT(ready[1], 1);
	CHECK_EQ_INT(ready[2], 0);
	CHECK_EQ_INT(wait_child(pid), 0);
	for (int i = 0; i < 3; i++)
		hg_sub_close(subs[i]);
}

static void test_dir_word_created_between_check_and_sleep(void)
{
	// The stub bumps the word as a manager would after a rename; a waiter
	// asleep on the old value returns and retries its opens.
	hg_sub *s = hg_sub_open("wt.bump", WORDS_TYPE);
	pid_t pid = fork();
	if (pid == 0) {
		sleep_ms(50);
		hg_stub_bump_dir();
		_exit(0);
	}
	uint64_t t0 = hg_now_ns();
	// Nothing appeared: the wake is spurious, and the wait goes back to
	// sleep until its timeout.
	CHECK_EQ_INT(hg_wait(&s, 1, 300), 0);
	CHECK(ms_since(t0) >= 300);
	CHECK_EQ_INT(wait_child(pid), 0);
	hg_sub_close(s);
}

/* ---- Interrupt --------------------------------------------------------- */

/* hg_wait_interrupt() is async-signal-safe (an atomic store and a futex
 * wake), so a thread stands in for the signal handler here: ThreadSanitizer
 * defers signal handlers to its safe points, which a raw futex_waitv is
 * not, so a handler-driven test would measure the sanitizer, not the
 * library. The path is the same: the word is set, the sleep is woken. */
static void *interrupter(void *arg)
{
	sleep_ms((long)(intptr_t)arg);
	hg_wait_interrupt();
	return NULL;
}

static void test_interrupt(void)
{
	hg_pub *p = open_words("wt.intr", 16);
	hg_sub *s = hg_sub_open("wt.intr", WORDS_TYPE);
	// Before the wait: the next wait returns at once.
	hg_wait_interrupt();
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 2000), HG_READY_INTERRUPT);
	CHECK(ms_since(t0) < 100);
	// Consumed: the following wait sleeps.
	t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 50), 0);
	CHECK(ms_since(t0) >= 50);
	// While asleep.
	pthread_t t;
	pthread_create(&t, NULL, interrupter, (void *)(intptr_t)50);
	t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 3000), HG_READY_INTERRUPT);
	CHECK(ms_since(t0) < 1000);
	pthread_join(t, NULL);
	// Between the caller's check and the sleep: the word is set before the
	// wait is entered, with news absent, and the wait does not sleep.
	hg_wait_interrupt();
	t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 3000), HG_READY_INTERRUPT);
	CHECK(ms_since(t0) < 100);
	// An interrupt does not hide news: both are reported in turn.
	pub_one(p);
	hg_wait_interrupt();
	CHECK_EQ_INT(hg_wait(&s, 1, 100), HG_READY_INTERRUPT);
	CHECK_EQ_INT(hg_wait(&s, 1, 100), HG_READY_TOPICS);
	hg_sub_close(s);
	hg_pub_close(p);
}

/* ---- Ready sets -------------------------------------------------------- */

static void test_ready_set_over_several_topics(void)
{
	// A request on a caller's .req topic and news on a data topic in one
	// pass; neither starves the other.
	hg_pub *data = open_words("wt.data", 64);
	hg_pub *req = open_words("caller.req", 16);
	hg_sub *subs[2] = { hg_sub_open("wt.data", WORDS_TYPE), hg_sub_open("caller.req", WORDS_TYPE) };
	unsigned char ready[2];
	CHECK_EQ_INT(hg_wait_ready(subs, 2, 0, ready), 0);
	pub_one(data);
	pub_one(req);
	CHECK_EQ_INT(hg_wait_ready(subs, 2, 100, ready), HG_READY_TOPICS);
	CHECK_EQ_INT(ready[0], 1);
	CHECK_EQ_INT(ready[1], 1);
	CHECK_EQ_INT(hg_next(subs[0], buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(hg_next(subs[1], buf, sizeof buf, &info), HG_MSG);
	// Requests back to back do not hide the data topic, nor the reverse.
	for (int i = 0; i < 50; i++)
		pub_one(req);
	pub_one(data);
	CHECK_EQ_INT(hg_wait_ready(subs, 2, 100, ready), HG_READY_TOPICS);
	CHECK_EQ_INT(ready[0], 1);
	CHECK_EQ_INT(ready[1], 1);
	int nreq = 0;
	while (hg_next(subs[1], buf, sizeof buf, &info) == HG_MSG)
		nreq++;
	CHECK_EQ_INT(nreq, 15); // a 16-slot ring: 15 safe
	CHECK_EQ_INT(hg_next(subs[0], buf, sizeof buf, &info), HG_MSG);
	// Only the one with news is ready.
	pub_one(data);
	CHECK_EQ_INT(hg_wait_ready(subs, 2, 100, ready), HG_READY_TOPICS);
	CHECK_EQ_INT(ready[0], 1);
	CHECK_EQ_INT(ready[1], 0);
	hg_sub_close(subs[0]);
	hg_sub_close(subs[1]);
	hg_pub_close(data);
	hg_pub_close(req);
}

static void test_limits(void)
{
	static hg_sub *subs[128];
	char name[48];
	for (int i = 0; i < 128; i++) {
		snprintf(name, sizeof name, "wt.many%d", i);
		subs[i] = hg_sub_open(name, NULL);
		CHECK(subs[i] != NULL);
	}
	// 127 by name accepted (all absent: they wait on the directory word).
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(subs, 127, 20), 0);
	CHECK(ms_since(t0) >= 20);
	CHECK_EQ_INT(hg_wait(subs, 128, 20), (unsigned)-1);
	CHECK_EQ_INT(errno, EINVAL);
	CHECK_EQ_INT(hg_wait(subs, 0, 20), (unsigned)-1);
	// 127 attached topics: 127 futexes plus the interrupt word fill the
	// 128; a message on the last one is seen.
	static hg_pub *pubs[127];
	for (int i = 0; i < 127; i++) {
		snprintf(name, sizeof name, "wt.many%d", i);
		pubs[i] = open_words(name, 2);
	}
	// Every one of them appeared: news once, then quiet.
	unsigned char ready[127];
	CHECK_EQ_INT(hg_wait_ready(subs, 127, 0, ready), HG_READY_TOPICS);
	int nready = 0;
	for (int i = 0; i < 127; i++) {
		nready += ready[i];
		CHECK_EQ_INT(hg_next(subs[i], buf, sizeof buf, &info), HG_NONE);
	}
	CHECK_EQ_INT(nready, 127);
	CHECK_EQ_INT(hg_wait(subs, 127, 0), 0);
	pub_one(pubs[126]);
	CHECK_EQ_INT(hg_wait_ready(subs, 127, 100, ready), HG_READY_TOPICS);
	CHECK_EQ_INT(ready[126], 1);
	CHECK_EQ_INT(ready[0], 0);
	for (int i = 0; i < 127; i++)
		hg_pub_close(pubs[i]);
	for (int i = 0; i < 128; i++)
		hg_sub_close(subs[i]);
}

/* ---- Glob subscriptions ------------------------------------------------ */

static void test_glob(void)
{
	hg_sub *g = hg_sub_open("gl.*", WORDS_TYPE);
	CHECK(g != NULL);
	CHECK_EQ_INT(hg_glob_count(g), 0);
	// With a glob, the wait is capped at 10 ms per sleep but still lasts
	// its whole timeout when nothing happens.
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&g, 1, 100), 0);
	CHECK(ms_since(t0) >= 100);
	// A matching topic created after the module started is found and read
	// within 10 ms of its first message.
	pid_t pid = fork();
	if (pid == 0) {
		sleep_ms(30);
		hg_pub *p = open_words("gl.one", 16);
		pub_one(p);
		sleep_ms(200);
		_exit(0);
	}
	CHECK_EQ_INT(hg_wait(&g, 1, 2000), HG_READY_TOPICS);
	CHECK(ms_since(t0) < 1000);
	CHECK_EQ_INT(hg_glob_count(g), 1);
	hg_sub *k = hg_glob_at(g, 0);
	CHECK(k != NULL);
	if (k) {
		CHECK(strcmp(hg_sub_name(k), "gl.one") == 0);
		enum hg_result r = hg_next(k, buf, sizeof buf, &info);
		CHECK(r == HG_MSG || r == HG_NONE); // attached before or after the publish
	}
	CHECK_EQ_INT(wait_child(pid), 0);
	// A non-matching topic is not a match.
	hg_pub *other = open_words("other.one", 16);
	pub_one(other);
	CHECK_EQ_INT(hg_glob_poll(g), 0);
	CHECK_EQ_INT(hg_glob_count(g), 1);
	hg_pub_close(other);
	// Reading the glob itself is an error.
	CHECK_EQ_INT(hg_next(g, buf, sizeof buf, &info), HG_ERROR);
	hg_sub_close(g);
}

int main(void)
{
	hg_test_setup("wait");
	RUN_TEST(test_publish_order_guard);
	RUN_TEST(test_lost_wake);
	RUN_TEST(test_liveness_on_dead_and_closed);
	RUN_TEST(test_absent_topic_appears);
	RUN_TEST(test_dir_word_created_between_check_and_sleep);
	RUN_TEST(test_interrupt);
	RUN_TEST(test_ready_set_over_several_topics);
	RUN_TEST(test_limits);
	RUN_TEST(test_glob);
	hg_test_teardown();
	TEST_MAIN_END();
}
