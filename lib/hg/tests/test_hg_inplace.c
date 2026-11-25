// SPDX-License-Identifier: LGPL-3.0-or-later
// Publishing and reading in place (docs/01 "In place"): hg_loan / hg_commit /
// hg_loan_abort and hg_latest_view / hg_next_view / hg_view_done.
//
// What must hold, and is checked here:
//  - a committed loan is a publish: the same seq, stamp, payload and padding
//    as hg_publish of those bytes, at 16 B, 12 B (a padded word), 256 B, 4 KB
//    and 1 MB;
//  - one loan at a time; a publish while one is out, a commit without one and
//    a commit of a wrong length are refused, and the last leaves the loan;
//  - an abort publishes nothing and the next message has the seq it would
//    have had; what a reader may still read while a loan is out;
//  - a view is the message hg_next or hg_latest would have copied, with the
//    same info and the same gap; hg_view_done is 1 once, -1 after;
//  - a view whose slot the writer came round to reads 0 at hg_view_done, and
//    the message is counted as lost by the next read in order;
//  - with a writer loaning flat out into a 4-slot ring (committing most,
//    aborting some after scribbling) and four reader processes mixing views
//    and copies: no view that hg_view_done called whole, and no copy, is ever
//    anything but one message's bytes. The count of torn ones must be 0.
#include "hg_test.h"

#define READERS 4
#define SECONDS 3

static struct hg_schema parsed[5];
static const struct {
	const char *schema;
	uint32_t c_bytes; /* the struct's own size: what a module hands to commit */
} kinds[] = {
	{ "u64 w[2]\n", 16 }, { "u32 a[3]\n", 12 }, { "u64 w[32]\n", 256 }, { "u64 w[512]\n", 4096 },
	{ "u64 w[131072]\n", 1048576 },
};
#define N_KINDS 5

static void fill_bytes(unsigned char *b, size_t n, uint64_t seq)
{
	for (size_t i = 0; i < n; i++)
		b[i] = (unsigned char)(seq * 131 + i * 7 + 1);
}

static void test_a_committed_loan_is_a_publish(void)
{
	for (int k = 0; k < N_KINDS; k++) {
		CHECK(hg_schema_parse(kinds[k].schema, strlen(kinds[k].schema), &parsed[k], NULL, 0) == 0);
		const struct hg_type *t = &parsed[k].type;
		size_t n = kinds[k].c_bytes, room = t->payload_bytes;
		CHECK(room >= n && room < n + 8);
		struct hg_pub_opts o = { .slots = 4 };
		char name[32]; /* a topic each: a file made again continues the old one's seqs */
		snprintf(name, sizeof name, "inplace.kind%d", k);
		hg_pub *p = hg_pub_open(name, t, &o);
		hg_sub *s = hg_sub_open_from(name, t, HG_OLDEST);
		CHECK(p != NULL && s != NULL);
		if (!p || !s)
			return;
		unsigned char *want = malloc(room), *got = malloc(room);
		struct hg_info info;
		for (uint64_t seq = 1; seq <= 9; seq++) { /* round the ring twice: every slot reused */
			CHECK_EQ_INT(hg_pub_next_seq(p), seq);
			unsigned char *slot = hg_loan(p);
			CHECK(slot != NULL);
			if (!slot)
				return;
			memset(slot, 0xee, room); /* the padding too: the commit must zero it */
			fill_bytes(slot, n, seq);
			CHECK_EQ_INT(hg_commit(p, n, 1000 + seq), 0);
			CHECK_EQ_INT(hg_pub_next_seq(p), seq + 1);
			memset(want, 0, room);
			fill_bytes(want, n, seq);
			memset(got, 0xaa, room);
			CHECK_EQ_INT(hg_next(s, got, room, &info), HG_MSG);
			CHECK_EQ_INT(info.seq, seq);
			CHECK_EQ_INT(info.stamp_ns, 1000 + seq);
			CHECK_EQ_INT(info.gap, 0);
			CHECK(info.publish_ns != 0);
			CHECK(memcmp(got, want, room) == 0);
		}
		/* And beside hg_publish on the same topic: the two interleave. */
		fill_bytes(want, n, 77);
		CHECK_EQ_INT(hg_publish(p, want, n, 5), 0);
		CHECK_EQ_INT(hg_next(s, got, room, &info), HG_MSG);
		CHECK_EQ_INT(info.seq, 10);
		free(want);
		free(got);
		hg_sub_close(s);
		hg_pub_close(p);
	}
}

static void test_the_rules_of_a_loan(void)
{
	static uint64_t buf[32], in[32];
	struct hg_pub_opts o = { .slots = 4 };
	hg_pub *p = hg_pub_open("inplace.rules", WORDS_TYPE, &o);
	hg_sub *s = hg_sub_open("inplace.rules", WORDS_TYPE);
	CHECK(p != NULL && s != NULL);
	if (!p || !s)
		return;
	struct hg_info info;
	for (uint64_t n = 1; n <= 6; n++) {
		fill(buf, n);
		CHECK_EQ_INT(hg_publish(p, buf, sizeof buf, n), 0);
	}
	CHECK_EQ_INT(hg_latest(s, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 6);

	errno = 0;
	CHECK_EQ_INT(hg_commit(p, sizeof buf, 0), -1); /* nothing loaned */
	CHECK_EQ_INT(errno, EINVAL);
	uint64_t *slot = hg_loan(p);
	CHECK(slot != NULL);
	if (!slot)
		return;
	errno = 0;
	CHECK(hg_loan(p) == NULL); /* one at a time */
	CHECK_EQ_INT(errno, EBUSY);
	errno = 0;
	CHECK_EQ_INT(hg_publish(p, buf, sizeof buf, 0), -1);
	CHECK_EQ_INT(errno, EBUSY);
	errno = 0;
	CHECK_EQ_INT(hg_publish_with(p, buf, sizeof buf, 0, HG_NO_REFS, NULL, 0), -1);
	CHECK_EQ_INT(errno, EBUSY);

	/* While it is out: nothing new for a reader, the newest still read by
	 * seq, and the oldest of the ring (message 3 of 3..6, whose slot is the
	 * one loaned) no longer. */
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_NONE);
	CHECK_EQ_INT(hg_read_seq(s, 6, in, sizeof in, &info), HG_MSG);
	CHECK(intact(in, &info));
	CHECK_EQ_INT(hg_read_seq(s, 4, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(hg_read_seq(s, 3, in, sizeof in, &info), HG_GONE);
	CHECK_EQ_INT(hg_pub_next_seq(p), 7);

	/* A length that is not the payload's: refused, and the loan stands. */
	fill(slot, 7);
	errno = 0;
	CHECK_EQ_INT(hg_commit(p, 8, 0), -1);
	CHECK_EQ_INT(errno, EINVAL);
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_NONE);
	CHECK_EQ_INT(hg_commit(p, sizeof buf, 7), 0);
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 7);
	CHECK(intact(in, &info));

	/* Given back after being scribbled on: nothing published, and the next
	 * message, by either way, is message 8. */
	slot = hg_loan(p);
	CHECK(slot != NULL);
	if (!slot)
		return;
	memset(slot, 0x5a, sizeof buf);
	hg_loan_abort(p);
	hg_loan_abort(p); /* twice is nothing */
	CHECK_EQ_INT(hg_pub_next_seq(p), 8);
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_NONE);
	CHECK_EQ_INT(hg_latest(s, in, sizeof in, &info), HG_NONE);
	CHECK_EQ_INT(hg_read_seq(s, 7, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(hg_read_seq(s, 8, in, sizeof in, &info), HG_GONE);
	fill(buf, 8);
	CHECK_EQ_INT(hg_publish(p, buf, sizeof buf, 8), 0);
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 8);
	CHECK_EQ_INT(info.gap, 0);
	CHECK(intact(in, &info));
	slot = hg_loan(p);
	CHECK(slot != NULL);
	hg_loan_abort(p);
	slot = hg_loan(p);
	CHECK(slot != NULL);
	if (slot) {
		fill(slot, 9);
		CHECK_EQ_INT(hg_commit(p, sizeof buf, 9), 0);
	}
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 9);
	CHECK(intact(in, &info));

	/* A publisher closed with a loan out: closed, and no message 10. */
	slot = hg_loan(p);
	CHECK(slot != NULL);
	hg_pub_close(p);
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_CLOSED);
	CHECK_EQ_INT(hg_read_seq(s, 10, in, sizeof in, &info), HG_GONE);
	hg_sub_close(s);

	/* A mirror publishes the sender's numbers: no loan. */
	unsigned char origin[16] = { 1, 2, 3 };
	hg_pub *m = hg_pub_open_mirror("inplace.mirror", WORDS_TYPE, origin, 0, &o);
	CHECK(m != NULL);
	if (m) {
		errno = 0;
		CHECK(hg_loan(m) == NULL);
		CHECK_EQ_INT(errno, EPERM);
		hg_pub_close(m);
	}
}

static void test_a_view_is_the_message(void)
{
	static uint64_t buf[32], in[32];
	struct hg_pub_opts o = { .slots = 8 };
	hg_pub *p = hg_pub_open("inplace.view", WORDS_TYPE, &o);
	hg_sub *a = hg_sub_open_from("inplace.view", WORDS_TYPE, HG_OLDEST); /* by view */
	hg_sub *b = hg_sub_open_from("inplace.view", WORDS_TYPE, HG_OLDEST); /* by copy */
	CHECK(p && a && b);
	if (!p || !a || !b)
		return;
	struct hg_info ia, ib;
	const void *v = (void *)1;
	CHECK_EQ_INT(hg_next_view(a, &v, &ia), HG_NONE);
	CHECK(v == NULL);
	errno = 0;
	CHECK_EQ_INT(hg_view_done(a), -1); /* none open */
	CHECK_EQ_INT(errno, EINVAL);
	for (uint64_t n = 1; n <= 5; n++) {
		fill(buf, n);
		hg_publish(p, buf, sizeof buf, 100 + n);
	}
	for (uint64_t n = 1; n <= 5; n++) {
		CHECK_EQ_INT(hg_next_view(a, &v, &ia), HG_MSG);
		CHECK_EQ_INT(hg_next(b, in, sizeof in, &ib), HG_MSG);
		CHECK(v != NULL);
		CHECK_EQ_INT(ia.seq, n);
		CHECK(ia.seq == ib.seq && ia.stamp_ns == ib.stamp_ns && ia.publish_ns == ib.publish_ns &&
		      ia.flags == ib.flags && ia.gap == ib.gap && ia.n_inputs == ib.n_inputs);
		CHECK(v && memcmp(v, in, sizeof in) == 0);
		CHECK_EQ_INT(hg_view_done(a), 1);
		CHECK_EQ_INT(hg_view_done(a), -1); /* once */
	}
	CHECK_EQ_INT(hg_next_view(a, &v, &ia), HG_NONE);
	CHECK(v == NULL);

	/* The gap of a reader that fell behind: the same by view as by copy. */
	for (uint64_t n = 6; n <= 30; n++) {
		fill(buf, n);
		hg_publish(p, buf, sizeof buf, 100 + n);
	}
	CHECK_EQ_INT(hg_next_view(a, &v, &ia), HG_MSG);
	CHECK_EQ_INT(hg_next(b, in, sizeof in, &ib), HG_MSG);
	CHECK(ia.seq == ib.seq && ia.gap == ib.gap && ia.gap > 0);
	CHECK(v && memcmp(v, in, sizeof in) == 0);
	CHECK_EQ_INT(hg_view_done(a), 1);

	/* The newest, in place. */
	CHECK_EQ_INT(hg_latest_view(a, &v, &ia), HG_MSG);
	CHECK_EQ_INT(ia.seq, 30);
	CHECK(v && ((const uint64_t *)v)[0] == 30 && ((const uint64_t *)v)[31] == 30);
	CHECK_EQ_INT(hg_view_done(a), 1);
	CHECK_EQ_INT(hg_latest_view(a, &v, &ia), HG_NONE);

	/* A view left open at the next call on the subscription is dropped. */
	fill(buf, 31);
	hg_publish(p, buf, sizeof buf, 131);
	CHECK_EQ_INT(hg_latest_view(a, &v, &ia), HG_MSG);
	CHECK_EQ_INT(hg_next(a, in, sizeof in, &ib), HG_NONE);
	CHECK_EQ_INT(hg_view_done(a), -1);

	/* A reader's view of a message published by a loan. */
	uint64_t *slot = hg_loan(p);
	CHECK(slot != NULL);
	if (slot) {
		fill(slot, 32);
		CHECK_EQ_INT(hg_commit(p, sizeof buf, 132), 0);
	}
	CHECK_EQ_INT(hg_next_view(a, &v, &ia), HG_MSG);
	CHECK_EQ_INT(ia.seq, 32);
	CHECK(v && ((const uint64_t *)v)[17] == 32);
	CHECK_EQ_INT(hg_view_done(a), 1);

	/* Without a buffer the copying reads are refused, not turned into views. */
	errno = 0;
	CHECK_EQ_INT(hg_next(a, NULL, sizeof in, &ia), HG_ERROR);
	CHECK_EQ_INT(errno, EINVAL);
	CHECK_EQ_INT(hg_latest(a, NULL, sizeof in, &ia), HG_ERROR);
	CHECK_EQ_INT(hg_read_seq(a, 32, NULL, sizeof in, &ia), HG_ERROR);
	hg_sub_close(a);
	hg_sub_close(b);
	hg_pub_close(p);
}

/* The writer comes round to a slot while a view of it is open: hg_view_done
 * says so, whether the writer finished the new message, is in the middle of
 * it (a loan), or gave the loan back; and the message is one the reader lost. */
static void test_a_view_overtaken(void)
{
	static uint64_t buf[32], in[32];
	struct hg_pub_opts o = { .slots = 4 };
	hg_pub *p = hg_pub_open("inplace.lapped", WORDS_TYPE, &o);
	hg_sub *s = hg_sub_open_from("inplace.lapped", WORDS_TYPE, HG_OLDEST);
	CHECK(p && s);
	if (!p || !s)
		return;
	struct hg_info info;
	const void *v;
	uint64_t n = 0;
#define PUB(k) \
	for (int i_ = 0; i_ < (k); i_++) { \
		fill(buf, ++n); \
		hg_publish(p, buf, sizeof buf, n); \
	}
	/* Overwritten by whole publishes. */
	PUB(2);
	CHECK_EQ_INT(hg_next_view(s, &v, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1);
	PUB(3); /* 3, 4, 5: message 5 is in message 1's slot */
	CHECK_EQ_INT(hg_view_done(s), 0);
	CHECK_EQ_INT(hg_view_done(s), -1);
	/* The next in order: message 1 lost by the view, and 2 gone from the
	 * ring since (it is in the slot the writer fills next). */
	CHECK_EQ_INT(hg_next(s, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 3);
	CHECK_EQ_INT(info.gap, 2);
	CHECK(intact(in, &info));

	/* Not overwritten yet: three more fit before the writer is back. */
	CHECK_EQ_INT(hg_next_view(s, &v, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 4);
	PUB(2); /* 6, 7 */
	CHECK(((const uint64_t *)v)[0] == 4);
	CHECK_EQ_INT(hg_view_done(s), 1);

	/* Overtaken by a loan that is still out: the slot holds no message. */
	CHECK_EQ_INT(hg_next_view(s, &v, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 5);
	PUB(1); /* 8 */
	uint64_t *slot = hg_loan(p); /* message 9's slot is message 5's */
	CHECK(slot != NULL);
	CHECK_EQ_INT(hg_view_done(s), 0);
	hg_loan_abort(p);
	/* And one given back: still no message there. */
	CHECK_EQ_INT(hg_next_view(s, &v, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 6);
	CHECK_EQ_INT(info.gap, 1); /* message 5, lost by the view */
	CHECK_EQ_INT(hg_view_done(s), 1);
	CHECK_EQ_INT(hg_latest_view(s, &v, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 8);
	PUB(4);
	CHECK_EQ_INT(hg_view_done(s), 0);
	/* A view from hg_latest_view that is lost is no gap: latest skips anyway. */
	CHECK_EQ_INT(hg_latest(s, in, sizeof in, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 12);
	CHECK_EQ_INT(info.gap, 0);
#undef PUB
	hg_sub_close(s);
	hg_pub_close(p);
}

/* ---- Under a concurrent writer ------------------------------------------ */

struct shared {
	atomic_int ready, stop;
	unsigned long long views[READERS], whole[READERS], discarded[READERS], copies[READERS], torn[READERS],
		gaps[READERS], busy[READERS];
};

#define BIG_WORDS 512 /* 4 KB: long enough to be overtaken in the middle */
static struct hg_schema big_parsed;

/* Reads every word through the view into memory of its own, as a user of a
 * view must before it knows whether the view was whole. */
static void take(uint64_t *dst, const void *v)
{
	memcpy(dst, v, BIG_WORDS * 8);
}

static int all_are(const uint64_t *b, uint64_t x)
{
	for (int i = 0; i < BIG_WORDS; i++)
		if (b[i] != x)
			return 0;
	return 1;
}

static void reader(struct shared *sh, int id)
{
	static uint64_t buf[BIG_WORDS];
	hg_sub *s = hg_sub_open("inplace.torn", &big_parsed.type);
	if (!s)
		_exit(1);
	unsigned long long views = 0, whole = 0, discarded = 0, copies = 0, torn = 0, gaps = 0, busy = 0;
	atomic_fetch_add(&sh->ready, 1);
	for (unsigned long i = 0; !atomic_load_explicit(&sh->stop, memory_order_relaxed); i++) {
		struct hg_info info;
		const void *v;
		enum hg_result r;
		int by_view = (i & 2) == 0;
		if (by_view)
			r = (i & 1) ? hg_next_view(s, &v, &info) : hg_latest_view(s, &v, &info);
		else
			r = (i & 1) ? hg_next(s, buf, sizeof buf, &info) : hg_latest(s, buf, sizeof buf, &info);
		if (r == HG_ERROR)
			_exit(3);
		if (r == HG_BUSY)
			busy++;
		if (r != HG_MSG)
			continue;
		gaps += info.gap;
		if (by_view) {
			views++;
			take(buf, v);
			int done = hg_view_done(s);
			if (done < 0)
				_exit(4);
			if (!done) {
				discarded++;
				continue;
			}
			whole++;
		} else {
			copies++;
		}
		/* A message, by either way: every word is its seq. The writer's
		 * aborted loans were filled with ~seq and must never be seen. */
		torn += !all_are(buf, info.seq);
	}
	sh->views[id] = views;
	sh->whole[id] = whole;
	sh->discarded[id] = discarded;
	sh->copies[id] = copies;
	sh->torn[id] = torn;
	sh->gaps[id] = gaps;
	sh->busy[id] = busy;
	hg_sub_close(s);
	_exit(0);
}

static void run_concurrent(uint32_t slots, int readers, int ms, int report)
{
	struct shared *sh = shared_zeroed(sizeof *sh);
	struct hg_pub_opts o = { .slots = slots };
	hg_pub *p = hg_pub_open("inplace.torn", &big_parsed.type, &o);
	CHECK(p != NULL);
	if (!p)
		return;
	pid_t pids[READERS];
	for (int i = 0; i < readers; i++) {
		pids[i] = fork();
		CHECK(pids[i] >= 0);
		if (pids[i] == 0)
			reader(sh, i);
	}
	uint64_t t0 = hg_now_ns();
	while (atomic_load(&sh->ready) < readers && ms_since(t0) < 5000)
		sleep_ms(1);
	CHECK_EQ_INT(atomic_load(&sh->ready), readers);
	static uint64_t buf[BIG_WORDS];
	uint64_t end = hg_now_ns() + (uint64_t)ms * 1000000ull;
	unsigned long long loans = 0, aborted = 0, copied = 0;
	uint64_t n = 0;
	do {
		for (int k = 0; k < 256; k++) {
			n = hg_pub_next_seq(p);
			if (k % 5 == 4) { /* the copying publish, beside the loans */
				for (int i = 0; i < BIG_WORDS; i++)
					buf[i] = n;
				hg_publish(p, buf, sizeof buf, n);
				copied++;
				continue;
			}
			uint64_t *slot = hg_loan(p);
			if (!slot) {
				CHECK(slot != NULL);
				break;
			}
			loans++;
			if (k % 7 == 3) { /* scribbled on, then given back */
				for (int i = 0; i < BIG_WORDS; i++)
					slot[i] = ~n;
				hg_loan_abort(p);
				aborted++;
				continue;
			}
			for (int i = 0; i < BIG_WORDS; i++)
				slot[i] = n;
			hg_commit(p, sizeof buf, n);
		}
	} while (hg_now_ns() < end);
	atomic_store(&sh->stop, 1);
	unsigned long long views = 0, whole = 0, discarded = 0, copies = 0, torn = 0, gaps = 0, busy = 0;
	for (int i = 0; i < readers; i++) {
		CHECK_EQ_INT(wait_child(pids[i]), 0);
		views += sh->views[i];
		whole += sh->whole[i];
		discarded += sh->discarded[i];
		copies += sh->copies[i];
		torn += sh->torn[i];
		gaps += sh->gaps[i];
		busy += sh->busy[i];
	}
	hg_pub_close(p);
	if (report)
		printf("    %d ms, %d readers, %u slots of 4 KB: %llu loans (%llu given back), %llu copying publishes; "
		       "views %llu (whole %llu, discarded %llu), copies %llu, busy %llu, gaps %llu, torn %llu\n",
		       ms, readers, slots, loans, aborted, copied, views, whole, discarded, copies, busy, gaps, torn);
	CHECK_EQ_INT(torn, 0);
	CHECK_EQ_INT(views, whole + discarded);
	CHECK(whole > 100);
	CHECK(copies > 100);
	CHECK(loans > 1000);
	munmap(sh, sizeof *sh);
}

static void test_no_torn_view_under_a_loaning_writer(void)
{
	static const char schema[] = "u64 w[512]\n";
	CHECK(hg_schema_parse(schema, strlen(schema), &big_parsed, NULL, 0) == 0);
	run_concurrent(4, READERS, SECONDS * 1000, 1);
}

static void test_every_slot_count(void)
{
	static const uint32_t slots[] = { 2, 8, 64 };
	for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++)
		run_concurrent(slots[i], 2, 400, 1);
}

int main(void)
{
	hg_test_setup("inplace");
	RUN_TEST(test_a_committed_loan_is_a_publish);
	RUN_TEST(test_the_rules_of_a_loan);
	RUN_TEST(test_a_view_is_the_message);
	RUN_TEST(test_a_view_overtaken);
	RUN_TEST(test_no_torn_view_under_a_loaning_writer);
	RUN_TEST(test_every_slot_count);
	hg_test_teardown();
	TEST_MAIN_END();
}
