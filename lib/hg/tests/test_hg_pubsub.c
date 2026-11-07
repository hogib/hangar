// SPDX-License-Identifier: LGPL-3.0-or-later
// Publishing and reading in one process: hg_latest, hg_next and its gap
// rule, hg_read_seq, hg_find_stamp, the slack a reader has, late joiners at
// the newest message and HG_OLDEST at the oldest, a slot left odd by a
// writer that died mid-publish, closes, type checks, and mirrored topics
// through hg_publish_seq (gaps, skips, refusals).
#include "hg_test.h"

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

static void pub_n(hg_pub *p, int n)
{
	for (int i = 0; i < n; i++) {
		uint64_t seq = hg_pub_next_seq(p);
		fill(buf, seq);
		if (hg_publish(p, buf, sizeof buf, seq * 10) < 0) {
			perror("hg_publish");
			exit(2);
		}
	}
}

static void test_latest_and_next(void)
{
	hg_pub *p = open_words("ps.basic", 16);
	hg_sub *s = hg_sub_open("ps.basic", WORDS_TYPE);
	CHECK(s != NULL);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE); // an empty topic
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_NONE);
	CHECK_EQ_INT(info.result, HG_NONE);
	pub_n(p, 3);
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 3);
	CHECK_EQ_INT(info.stamp_ns, 30);
	CHECK(intact(buf, &info));
	CHECK(info.publish_ns != 0);
	CHECK_EQ_INT(info.n_inputs, 0);
	CHECK_EQ_INT(info.flags, 0);
	CHECK_EQ_INT(info.epoch, 0);
	// Nothing newer than the last one returned.
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_NONE);
	// hg_next continues after what hg_latest returned.
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	pub_n(p, 2);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 4);
	CHECK_EQ_INT(info.gap, 0);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 5);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	CHECK_EQ_INT(hg_pub_next_seq(p), 6);
	CHECK(info.age_ns < 1000000000ull);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	// A short buffer is an error, not a partial copy; so is a wrong length.
	// The payload is the type's size rounded up to 8, so a length within
	// the rounding (the C struct's own sizeof) is accepted on both sides.
	CHECK_EQ_INT(hg_latest(s, buf, 8, &info), HG_ERROR);
	CHECK_EQ_INT(errno, EMSGSIZE);
	CHECK_EQ_INT(hg_latest(s, buf, WORDS_BYTES - 8, &info), HG_ERROR);
	CHECK_EQ_INT(hg_publish(p, buf, 8, 0), -1);
	CHECK_EQ_INT(errno, EINVAL);
	CHECK_EQ_INT(hg_publish(p, buf, WORDS_BYTES - 8, 0), -1);
	CHECK_EQ_INT(hg_publish(p, buf, WORDS_BYTES + 1, 0), -1);
	CHECK_EQ_INT(hg_sub_payload_bytes(s), WORDS_BYTES);
	CHECK_EQ_INT(hg_sub_topic_id(s), hg_topic_id("ps.basic"));
	CHECK(strcmp(hg_sub_name(s), "ps.basic") == 0);
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_gap_rule(void)
{
	hg_pub *p = open_words("ps.gap", 16);
	hg_sub *s = hg_sub_open("ps.gap", WORDS_TYPE);
	pub_n(p, 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1);
	// W = 41, K = 16: w = 2 <= W-K+1 = 26, so 2..26 are gone or about to be;
	// the reader resumes at W-K+2 = 27 and counts 25 lost.
	pub_n(p, 40);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 27);
	CHECK_EQ_INT(info.gap, 25);
	CHECK(intact(buf, &info));
	for (uint64_t n = 28; n <= 41; n++) {
		CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
		CHECK_EQ_INT(info.seq, n);
		CHECK_EQ_INT(info.gap, 0);
	}
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_slack(void)
{
	// A reader that reads every K-1 messages loses nothing; every K, one
	// each time (docs/01 "In order").
	hg_pub *p = open_words("ps.slack", 16);
	hg_sub *s = hg_sub_open("ps.slack", WORDS_TYPE);
	uint64_t gap = 0, got = 0;
	for (int round = 0; round < 20; round++) {
		pub_n(p, 15);
		while (hg_next(s, buf, sizeof buf, &info) == HG_MSG) {
			gap += info.gap;
			got++;
		}
	}
	CHECK_EQ_INT(gap, 0);
	CHECK_EQ_INT(got, 300);
	for (int round = 0; round < 20; round++) {
		pub_n(p, 16);
		uint64_t g = 0;
		while (hg_next(s, buf, sizeof buf, &info) == HG_MSG)
			g += info.gap;
		CHECK_EQ_INT(g, 1);
	}
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_late_joiner_starts_at_newest(void)
{
	hg_pub *p = open_words("ps.late", 16);
	pub_n(p, 1000);
	// The newest message is returned once (a late joiner sees the current
	// value), with no gap for the 999 before it; then nothing until a publish.
	hg_sub *s = hg_sub_open("ps.late", WORDS_TYPE);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1000);
	CHECK_EQ_INT(info.gap, 0);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_NONE);
	hg_sub_close(s);
	s = hg_sub_open("ps.late", WORDS_TYPE);
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1000);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	pub_n(p, 1);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1001);
	CHECK_EQ_INT(info.gap, 0);
	hg_sub_close(s);
	// HG_OLDEST: the oldest message still safely in the ring, W-K+2.
	s = hg_sub_open_from("ps.late", WORDS_TYPE, HG_OLDEST);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1001 - 16 + 2);
	CHECK_EQ_INT(info.gap, 0);
	int n = 1;
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG) {
		CHECK_EQ_INT(info.gap, 0);
		n++;
	}
	CHECK_EQ_INT(n, 15);
	hg_sub_close(s);
	// A ring that never wrapped: HG_OLDEST starts at first_seq + 1.
	hg_pub_close(p);
	p = open_words("ps.late2", 16);
	pub_n(p, 5);
	s = hg_sub_open_from("ps.late2", WORDS_TYPE, HG_OLDEST);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 1);
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_read_seq_and_find_stamp(void)
{
	hg_pub *p = open_words("ps.rand", 16);
	pub_n(p, 40); // stamps 10, 20, ... 400; ring holds 25..40 (25 in the slot filled next)
	hg_sub *s = hg_sub_open("ps.rand", WORDS_TYPE);
	CHECK_EQ_INT(hg_read_seq(s, 30, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 30);
	CHECK_EQ_INT(info.stamp_ns, 300);
	CHECK(intact(buf, &info));
	CHECK_EQ_INT(hg_read_seq(s, 40, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(hg_read_seq(s, 25, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(hg_read_seq(s, 24, buf, sizeof buf, &info), HG_GONE);
	CHECK_EQ_INT(hg_read_seq(s, 41, buf, sizeof buf, &info), HG_GONE);
	CHECK_EQ_INT(hg_read_seq(s, 0, buf, sizeof buf, &info), HG_GONE);
	// Random access does not move hg_next: it still returns the newest
	// message a fresh subscription starts with, then nothing.
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 40);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);

	CHECK_EQ_INT(hg_find_stamp(s, 355, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 35);
	CHECK_EQ_INT(hg_find_stamp(s, 350, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 35);
	CHECK_EQ_INT(hg_find_stamp(s, 100000, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 40);
	CHECK_EQ_INT(hg_find_stamp(s, 250, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 25);
	CHECK_EQ_INT(hg_find_stamp(s, 249, buf, sizeof buf, &info), HG_GONE);
	CHECK_EQ_INT(hg_find_stamp(s, 0, buf, sizeof buf, &info), HG_GONE);
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_odd_seq_from_dead_writer(void)
{
	// A writer that died mid-publish left slot 10 (message 11) odd and, in
	// this scenario, write_seq already past it: the reader counts 11 as
	// lost and moves on without livelocking on the slot.
	hg_pub *p = open_words("ps.odd", 16);
	hg_sub *s = hg_sub_open("ps.odd", WORDS_TYPE);
	pub_n(p, 10);
	while (hg_next(s, buf, sizeof buf, &info) == HG_MSG)
		;
	CHECK_EQ_INT(hg_stub_set_slot_seq("ps.odd", 10, 2 * 11 - 1), 0);
	CHECK_EQ_INT(hg_stub_set_write_seq("ps.odd", 11), 0);
	uint64_t t0 = hg_now_ns();
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	CHECK_EQ_INT(info.gap, 1);
	CHECK(ms_since(t0) < 100);
	// Message 12 after it reads normally, with no further gap.
	CHECK_EQ_INT(hg_stub_set_write_seq("ps.odd", 10), 0);
	pub_n(p, 2); // the open pub still counts from 10: publishes 11 and 12
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 12);
	CHECK_EQ_INT(info.gap, 0);
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_closed(void)
{
	hg_pub *p = open_words("ps.closed", 16);
	hg_sub *s = hg_sub_open("ps.closed", WORDS_TYPE);
	pub_n(p, 2);
	hg_pub_close(p);
	// What was published is still read; then the close.
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_CLOSED);
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_CLOSED);
	// A death looks the same to a reader of the data, but is told apart in
	// the header flags.
	hg_sub_close(s);
	p = open_words("ps.died", 16);
	s = hg_sub_open("ps.died", WORDS_TYPE);
	CHECK_EQ_INT(hg_stub_mark_died("ps.died"), 0);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_CLOSED);
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_types_and_introspection(void)
{
	struct hg_pub_opts o = { .slots = 16, .rate_mhz = 50000 };
	hg_pub *p = hg_pub_open("ps.depth", DEPTH_TYPE, &o);
	CHECK(p != NULL);
	struct depth d = { 1.5f, 20.0f, 1013.25f };
	CHECK_EQ_INT(hg_publish(p, &d, sizeof d, 7), 0);
	// The wrong type: an error, not data.
	hg_sub *s = hg_sub_open("ps.depth", WORDS_TYPE);
	CHECK(s != NULL);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_ERROR);
	CHECK_EQ_INT(errno, EPROTOTYPE);
	hg_sub_close(s);
	// No type: introspection from the file.
	s = hg_sub_open("ps.depth", NULL);
	struct hg_topic_meta m;
	CHECK_EQ_INT(hg_sub_meta(s, &m), 0);
	CHECK(strcmp(m.name, "ps.depth") == 0);
	CHECK(m.type_hash == DEPTH_TYPE->hash);
	CHECK_EQ_INT(m.payload_bytes, 16);
	CHECK_EQ_INT(m.slot_count, 16);
	CHECK_EQ_INT(m.rate_mhz, 50000);
	CHECK_EQ_INT(m.stale_after_ns, 60000000); // three periods of 20 ms
	CHECK_EQ_INT(m.writer_pid, getpid());
	uint32_t len = 0;
	const char *text = hg_sub_schema(s, &len);
	CHECK(text != NULL);
	CHECK_EQ_INT(len, strlen(DEPTH_SCHEMA));
	CHECK(text && memcmp(text, DEPTH_SCHEMA, len) == 0);
	struct hg_schema parsed;
	CHECK_EQ_INT(hg_schema_parse(text, len, &parsed, NULL, 0), 0);
	CHECK(parsed.type.hash == m.type_hash);
	struct depth got;
	CHECK_EQ_INT(hg_latest(s, &got, sizeof got, &info), HG_MSG);
	CHECK(got.depth_m == 1.5f && got.pressure_mbar == 1013.25f);
	const struct hg_field *f = hg_type_field(&parsed.type, "temp_c");
	CHECK(f && f->offset == 4);
	float temp;
	memcpy(&temp, (unsigned char *)&got + (f ? f->offset : 0), 4);
	CHECK(temp == 20.0f);
	hg_sub_close(s);
	hg_pub_close(p);
	// hg_publish_seq is for mirrors only.
	p = open_words("ps.local", 16);
	CHECK_EQ_INT(hg_publish_seq(p, 1, 0, buf, sizeof buf, 0, NULL, 0), -1);
	CHECK_EQ_INT(errno, EPERM);
	hg_pub_close(p);
}

static void test_mirrored_topic(void)
{
	static const unsigned char boot[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	struct hg_pub_opts o = { .slots = 16 };
	hg_pub *p = hg_pub_open_mirror("ps.mirror", WORDS_TYPE, boot, 100, &o);
	CHECK(p != NULL);
	if (!p)
		return;
	CHECK_EQ_INT(hg_pub_next_seq(p), 100); // first_seq is 99: the join point is no gap
	hg_sub *s = hg_sub_open_from("ps.mirror", WORDS_TYPE, HG_OLDEST);
	struct hg_topic_meta m;
	CHECK_EQ_INT(hg_sub_meta(s, &m), 0);
	CHECK(memcmp(m.origin_boot, boot, 16) == 0);
	CHECK_EQ_INT(m.first_seq, 99);
	struct hg_input ref = { 0x1234, HG_REF_STALE, 42 };
	fill(buf, 100);
	CHECK_EQ_INT(hg_publish_seq(p, 100, 0, buf, sizeof buf, 1, &ref, 1), 0);
	fill(buf, 101);
	CHECK_EQ_INT(hg_publish_seq(p, 101, HG_SLOT_STALE, buf, sizeof buf, 2, NULL, 0), 0);
	fill(buf, 105);
	CHECK_EQ_INT(hg_publish_seq(p, 105, 0, buf, sizeof buf, 3, NULL, 0), 0);
	fill(buf, 110);
	CHECK_EQ_INT(hg_publish_seq(p, 110, HG_SLOT_DECIMATED, buf, sizeof buf, 4, NULL, 0), 0);
	// Numbers only rise.
	CHECK_EQ_INT(hg_publish_seq(p, 110, 0, buf, sizeof buf, 5, NULL, 0), -1);
	CHECK_EQ_INT(errno, ERANGE);
	CHECK_EQ_INT(hg_publish_seq(p, 50, 0, buf, sizeof buf, 5, NULL, 0), -1);

	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 100);
	CHECK_EQ_INT(info.gap, 0); // the first message of a new mirror counts no gap
	CHECK_EQ_INT(info.n_inputs, 1);
	CHECK_EQ_INT(info.inputs[0].topic_id, 0x1234);
	CHECK_EQ_INT(info.inputs[0].flags, HG_REF_STALE); // carried unchanged
	CHECK_EQ_INT(info.inputs[0].seq, 42);
	CHECK(intact(buf, &info));
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 101);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE); // the sender's flags, kept
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 105);
	CHECK_EQ_INT(info.gap, 3); // 102, 103, 104 lost on the tether
	CHECK_EQ_INT(info.skipped, 0);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 110);
	CHECK_EQ_INT(info.gap, 0);
	CHECK_EQ_INT(info.skipped, 4); // 106..109 skipped on purpose
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	// A missing seq is HG_GONE; a present one reads.
	CHECK_EQ_INT(hg_read_seq(s, 103, buf, sizeof buf, &info), HG_GONE);
	CHECK_EQ_INT(hg_read_seq(s, 105, buf, sizeof buf, &info), HG_MSG);
	// A jump longer than the ring: every seq between is a gap, no HG_NONE
	// while news is pending.
	fill(buf, 200);
	CHECK_EQ_INT(hg_publish_seq(p, 200, 0, buf, sizeof buf, 6, NULL, 0), 0);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 200);
	CHECK_EQ_INT(info.gap, 200 - 111);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_NONE);
	hg_sub_close(s);
	hg_pub_close(p);
	// A mirror of a new boot of the same sender: a new first_seq; of the same
	// boot: write_seq continued.
	p = hg_pub_open_mirror("ps.mirror", WORDS_TYPE, boot, 1000, &o);
	CHECK(p != NULL);
	CHECK_EQ_INT(hg_pub_next_seq(p), 201);
	hg_pub_close(p);
	static const unsigned char boot2[16] = { 9 };
	p = hg_pub_open_mirror("ps.mirror", WORDS_TYPE, boot2, 1000, &o);
	CHECK(p != NULL);
	CHECK_EQ_INT(hg_pub_next_seq(p), 1000);
	hg_pub_close(p);
	// An all-zero boot id is a local topic's: refused for a mirror.
	static const unsigned char zero[16];
	CHECK(hg_pub_open_mirror("ps.mirror2", WORDS_TYPE, zero, 1, &o) == NULL);
	CHECK_EQ_INT(errno, EINVAL);
}

static void test_bad_arguments(void)
{
	CHECK(hg_pub_open("Bad.Name", WORDS_TYPE, NULL) == NULL);
	CHECK_EQ_INT(errno, EINVAL);
	struct hg_pub_opts o = { .slots = 3 };
	CHECK(hg_pub_open("ps.odd_slots", WORDS_TYPE, &o) == NULL);
	CHECK_EQ_INT(errno, EINVAL);
	CHECK(hg_sub_open("..", NULL) == NULL);
	CHECK_EQ_INT(errno, EINVAL);
	// An absent topic reads HG_ABSENT, at once, without blocking.
	hg_sub *s = hg_sub_open("ps.never", NULL);
	CHECK(s != NULL);
	CHECK_EQ_INT(hg_next(s, buf, sizeof buf, &info), HG_ABSENT);
	CHECK_EQ_INT(hg_latest(s, buf, sizeof buf, &info), HG_ABSENT);
	CHECK_EQ_INT(hg_read_seq(s, 1, buf, sizeof buf, &info), HG_ABSENT);
	CHECK_EQ_INT(hg_sub_payload_bytes(s), 0);
	CHECK(hg_sub_schema(s, NULL) == NULL);
	hg_sub_close(s);
}

int main(void)
{
	hg_test_setup("pubsub");
	RUN_TEST(test_latest_and_next);
	RUN_TEST(test_gap_rule);
	RUN_TEST(test_slack);
	RUN_TEST(test_late_joiner_starts_at_newest);
	RUN_TEST(test_read_seq_and_find_stamp);
	RUN_TEST(test_odd_seq_from_dead_writer);
	RUN_TEST(test_closed);
	RUN_TEST(test_types_and_introspection);
	RUN_TEST(test_mirrored_topic);
	RUN_TEST(test_bad_arguments);
	hg_test_teardown();
	TEST_MAIN_END();
}
