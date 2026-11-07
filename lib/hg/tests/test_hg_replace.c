// SPDX-License-Identifier: LGPL-3.0-or-later
// A replaced file (docs/01 "A replaced file"): bit 2 announced by the
// manager before the rename, the reader draining the old mapping and
// reopening the path, sequence numbers continuing across the restart with
// no gap, a fresh file before its first publish, the wake file and .dir
// keeping their inodes, and a mirrored topic's new origin_boot starting a
// new epoch.
#include "hg_test.h"

static uint64_t buf[32];
static struct hg_info info;

static hg_pub *open_words(const char *name)
{
	struct hg_pub_opts o = { .slots = 16 };
	hg_pub *p = hg_pub_open(name, WORDS_TYPE, &o);
	if (!p) {
		printf("    hg_pub_open(%s): %s\n", name, strerror(errno));
		exit(2);
	}
	return p;
}

static void pub_n(hg_pub *p, int n)
{
	for (int i = 0; i < n; i++) {
		uint64_t seq = hg_pub_next_seq(p);
		fill(buf, seq);
		hg_publish(p, buf, sizeof buf, seq);
	}
}

static void test_bit2_without_new_file(void)
{
	// The window between the manager's flagging and its rename: the reader
	// sees nothing new and does not spin; the flag cleared again (a
	// replacing writer that backed out) leaves the topic as it was.
	hg_pub *p = open_words("rp.pending");
	hg_sub *s = hg_sub_open("rp.pending", WORDS_TYPE);
	pub_n(p, 3);
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG)
		;
	CHECK_EQ_INT(hg_stub_mark_replaced("rp.pending"), 0);
	CHECK_EQ_INT(hg_wait(&s, 1, 0), HG_READY_TOPICS); // the announcement is news, once
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 100), 0);
	CHECK(ms_since(t0) >= 100); // slept the whole timeout: no spinning
	CHECK_EQ_INT(hg_sub_reopens(s), 0);
	CHECK_EQ_INT(hg_stub_clear_replaced("rp.pending"), 0);
	pub_n(p, 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 4);
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_restart_continues_seq_without_gap(void)
{
	ino_t wake0, dir0, topic0, wake1, dir1, topic1;
	hg_pub *a = open_words("rp.restart");
	hg_sub *s = hg_sub_open("rp.restart", WORDS_TYPE);
	pub_n(a, 50);
	uint64_t got = 0;
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG)
		got = info.seq;
	CHECK_EQ_INT(got, 50);
	CHECK_EQ_INT(hg_stub_wake_inode("rp.restart", &wake0), 0);
	CHECK_EQ_INT(hg_stub_dir_inode(&dir0), 0);
	CHECK_EQ_INT(hg_stub_topic_inode("rp.restart", &topic0), 0);
	// The writer "dies" (its mapping stays, as a dead process's file would)
	// and a new one opens the same name: the new file continues write_seq.
	hg_pub *b = open_words("rp.restart");
	CHECK_EQ_INT(hg_pub_next_seq(b), 51);
	CHECK_EQ_INT(hg_stub_wake_inode("rp.restart", &wake1), 0);
	CHECK_EQ_INT(hg_stub_dir_inode(&dir1), 0);
	CHECK_EQ_INT(hg_stub_topic_inode("rp.restart", &topic1), 0);
	CHECK(wake0 == wake1); // the wake file and .dir keep their inodes
	CHECK(dir0 == dir1);
	CHECK(topic0 != topic1); // the topic file is new
	// The reader: told by bit 2, reopens, and finds the fresh file empty.
	CHECK_EQ_INT(hg_wait(&s, 1, 0), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	CHECK_EQ_INT(hg_sub_reopens(s), 1);
	CHECK_EQ_INT(info.gap, 0);
	CHECK_EQ_INT(info.epoch, 0);
	// Nothing to spin on before the first publish.
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 50), 0);
	CHECK(ms_since(t0) >= 50);
	pub_n(b, 5);
	CHECK_EQ_INT(hg_wait(&s, 1, 0), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 51);
	CHECK_EQ_INT(info.gap, 0);
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG)
		got = info.seq;
	CHECK_EQ_INT(got, 55);
	// A message the reader did not drain before the switch is still read
	// from the old mapping first: the old file is drained, then reopened.
	pub_n(b, 3);
	hg_pub *c = open_words("rp.restart");
	CHECK_EQ_INT(hg_pub_next_seq(c), 59);
	pub_n(c, 2);
	uint64_t gap = 0;
	int n = 0;
	for (uint64_t want = 56; hg_next(s, buf, sizeof buf, &info) == HG_MSG; want++) {
		CHECK_EQ_INT(info.seq, want);
		gap += info.gap;
		n++;
	}
	CHECK_EQ_INT(n, 5);
	CHECK_EQ_INT(gap, 0);
	CHECK_EQ_INT(hg_sub_reopens(s), 2);
	// A close on the old mapping by the stale writer is not news for a
	// reader that has moved on.
	hg_pub_close(a);
	hg_pub_close(b);
	CHECK_EQ_INT(hg_wait(&s, 1, 0), 0);
	// A reader opening after the replacement sees the current file.
	hg_sub *late = hg_sub_open_from("rp.restart", WORDS_TYPE, HG_OLDEST);
	CHECK_EQ_INT(hg_next(late, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 59);
	CHECK_EQ_INT(info.gap, 0);
	hg_sub_close(late);
	hg_sub_close(s);
	hg_pub_close(c);
}

static void test_lost_messages_across_restart_are_a_gap(void)
{
	// Messages really lost across the restart (the reader fell behind the
	// old ring) count, nothing else does.
	hg_pub *a = open_words("rp.lost");
	hg_sub *s = hg_sub_open("rp.lost", WORDS_TYPE);
	pub_n(a, 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	pub_n(a, 30); // 2..16 are gone from a 16-slot ring
	hg_pub *b = open_words("rp.lost");
	pub_n(b, 1); // 32
	uint64_t gap = 0, first = 0, last = 0;
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG) {
		gap += info.gap;
		if (!first)
			first = info.seq;
		last = info.seq;
	}
	CHECK_EQ_INT(first, 17);
	CHECK_EQ_INT(last, 32);
	CHECK_EQ_INT(gap, 15);
	hg_sub_close(s);
	hg_pub_close(a);
	hg_pub_close(b);
}

static void test_mirror_new_boot_is_new_epoch(void)
{
	static const unsigned char boot1[16] = { 1 }, boot2[16] = { 2 };
	struct hg_pub_opts o = { .slots = 16 };
	hg_pub *a = hg_pub_open_mirror("rp.mirror", WORDS_TYPE, boot1, 500, &o);
	CHECK(a != NULL);
	if (!a)
		return;
	hg_sub *s = hg_sub_open_from("rp.mirror", WORDS_TYPE, HG_OLDEST);
	for (uint64_t n = 500; n < 510; n++) {
		fill(buf, n);
		CHECK_EQ_INT(hg_publish_seq(a, n, 0, buf, sizeof buf, n, NULL, 0), 0);
	}
	uint64_t got = 0;
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG)
		got = info.seq;
	CHECK_EQ_INT(got, 509);
	CHECK_EQ_INT(info.epoch, 0);
	// The sender rebooted: a new file with the new boot id, numbers from 1.
	hg_pub *b = hg_pub_open_mirror("rp.mirror", WORDS_TYPE, boot2, 1, &o);
	CHECK(b != NULL);
	CHECK_EQ_INT(hg_pub_next_seq(b), 1);
	CHECK_EQ_INT(hg_wait(&s, 1, 0), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	CHECK_EQ_INT(info.epoch, 1);
	CHECK_EQ_INT(info.gap, 0);
	fill(buf, 1);
	CHECK_EQ_INT(hg_publish_seq(b, 1, 0, buf, sizeof buf, 1, NULL, 0), 0);
	fill(buf, 2);
	CHECK_EQ_INT(hg_publish_seq(b, 2, 0, buf, sizeof buf, 2, NULL, 0), 0);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1);
	CHECK_EQ_INT(info.gap, 0);
	CHECK_EQ_INT(info.epoch, 1);
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 2);
	struct hg_topic_meta m;
	CHECK_EQ_INT(hg_sub_meta(s, &m), 0);
	CHECK(memcmp(m.origin_boot, boot2, 16) == 0);
	// The same boot again (a restarted bridge): no new epoch, numbers continue.
	hg_pub *c = hg_pub_open_mirror("rp.mirror", WORDS_TYPE, boot2, 100, &o);
	CHECK(c != NULL);
	CHECK_EQ_INT(hg_pub_next_seq(c), 3);
	fill(buf, 3);
	CHECK_EQ_INT(hg_publish_seq(c, 3, 0, buf, sizeof buf, 3, NULL, 0), 0);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 3);
	CHECK_EQ_INT(info.gap, 0);
	CHECK_EQ_INT(info.epoch, 1);
	CHECK_EQ_INT(hg_sub_reopens(s), 2);
	hg_sub_close(s);
	hg_pub_close(a);
	hg_pub_close(b);
	hg_pub_close(c);
}

static void test_dead_writer_then_replacement(void)
{
	// The manager flags a dead writer's file; a reader reports HG_CLOSED
	// once through hg_wait, sleeps, and is woken again by the replacement.
	hg_pub *a = open_words("rp.dead");
	hg_sub *s = hg_sub_open("rp.dead", WORDS_TYPE);
	pub_n(a, 2);
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG)
		;
	CHECK_EQ_INT(hg_stub_mark_died("rp.dead"), 0);
	CHECK_EQ_INT(hg_wait(&s, 1, 0), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_CLOSED);
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_wait(&s, 1, 100), 0);
	CHECK(ms_since(t0) >= 100);
	hg_pub *b = open_words("rp.dead");
	CHECK_EQ_INT(hg_wait(&s, 1, 0), HG_READY_TOPICS);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	pub_n(b, 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 3);
	CHECK_EQ_INT(info.gap, 0);
	hg_sub_close(s);
	hg_pub_close(a);
	hg_pub_close(b);
}

int main(void)
{
	hg_test_setup("replace");
	RUN_TEST(test_bit2_without_new_file);
	RUN_TEST(test_restart_continues_seq_without_gap);
	RUN_TEST(test_lost_messages_across_restart_are_a_gap);
	RUN_TEST(test_mirror_new_boot_is_new_epoch);
	RUN_TEST(test_dead_writer_then_replacement);
	hg_test_teardown();
	TEST_MAIN_END();
}
