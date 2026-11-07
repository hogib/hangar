// SPDX-License-Identifier: LGPL-3.0-or-later
// Provenance and stale inputs (docs/01 "Provenance", "Stale inputs"): the
// references a publish records for its Inputs, reused when nothing new was
// read, hg_use overriding the most recent one, Sources named per message,
// refusals of references outside Inputs and Sources and of more than four,
// HG_STALE and HG_NO_REFS, a <role>:<topic> reference recorded as given, and
// slot flag bit 0 from a stale, dead, closed or absent input, transitive
// through a message that carries it, and cleared when the input recovers.
//
// One process plays every module: the wiring lists each input topic as both
// a sub and a pub line so this module can publish what it reads.
#include "hg_test.h"

static uint64_t buf[32];
static struct hg_info info;
static char wiring_path[600];

static void write_wiring(void)
{
	snprintf(wiring_path, sizeof wiring_path, "%s/wiring.txt", topic_dir);
	FILE *f = fopen(wiring_path, "w");
	if (!f) {
		perror("wiring");
		exit(2);
	}
	unsigned long long h = (unsigned long long)WORDS_TYPE->hash;
	fprintf(f, "hangar-wiring 1\nmodule prov\n");
	// a.in is rated (50 Hz, stale after 60 ms); b.in and the sources are events.
	fprintf(f, "sub a.in type=words hash=%016llx rate=50\n", h);
	fprintf(f, "sub b.in type=words hash=%016llx\n", h);
	fprintf(f, "sub c.src type=words hash=%016llx\n", h);
	fprintf(f, "sub d.src type=words hash=%016llx\n", h);
	fprintf(f, "sub x.mid type=words hash=%016llx\n", h);
	fprintf(f, "subglob *.req implicit\n");
	for (int i = 1; i <= 5; i++)
		fprintf(f, "sub out%d\n", i); /* this module reads its outputs back */
	fprintf(f, "pub a.in type=words hash=%016llx rate=50\n", h);
	fprintf(f, "pub b.in type=words hash=%016llx\n", h);
	fprintf(f, "pub c.src type=words hash=%016llx\n", h);
	fprintf(f, "pub d.src type=words hash=%016llx\n", h);
	fprintf(f, "pub x.mid type=words hash=%016llx\n", h);
	fprintf(f, "pub caller.req type=words hash=%016llx slots=16\n", h);
	fprintf(f, "pub out1 type=words hash=%016llx inputs=a.in,b.in sources=c.src,*.req\n", h);
	fprintf(f, "pub out2 type=words hash=%016llx inputs=out1\n", h);
	fprintf(f, "pub out3 type=words hash=%016llx inputs=x.mid\n", h);
	fprintf(f, "pub out4 type=words hash=%016llx inputs=vehicle:cam.pilot\n", h);
	fprintf(f, "pub out5 type=words hash=%016llx inputs=a.in,b.in,c.src,d.src sources=x.mid\n", h);
	fclose(f);
	setenv("HANGAR_WIRING_FILE", wiring_path, 1);
}

static hg_pub *open_words(const char *name)
{
	hg_pub *p = hg_pub_open(name, WORDS_TYPE, NULL);
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
	if (hg_publish(p, buf, sizeof buf, seq) < 0) {
		perror("hg_publish");
		exit(2);
	}
}

/* Reads what the output last published. */
static void last_of(hg_sub *s)
{
	enum hg_result r = hg_latest(s, buf, sizeof buf, &info);
	if (r != HG_MSG) {
		printf("    no message on %s: %d\n", hg_sub_name(s), (int)r);
		exit(2);
	}
}

static const struct hg_input *ref_to(const char *topic)
{
	uint32_t id = hg_topic_id(topic);
	for (uint32_t i = 0; i < info.n_inputs; i++)
		if (info.inputs[i].topic_id == id)
			return &info.inputs[i];
	return NULL;
}

static hg_pub *a_pub, *b_pub, *c_pub, *d_pub, *x_pub, *req_pub, *out1, *out2, *out3, *out4, *out5;
static hg_sub *a, *b, *c, *x, *req, *o1, *o2, *o3, *o4, *o5;

static void test_setup(void)
{
	CHECK_EQ_INT(hg_init(), 0);
	if (hg_init_error()[0])
		printf("    %s\n", hg_init_error());
	CHECK(strcmp(hg_module_name(), "prov") == 0);
	a_pub = open_words("a.in");
	c_pub = open_words("c.src");
	d_pub = open_words("d.src");
	x_pub = open_words("x.mid");
	req_pub = open_words("caller.req");
	a = hg_sub_open("a.in", WORDS_TYPE);
	b = hg_sub_open("b.in", WORDS_TYPE); // absent until b_pub opens
	c = hg_sub_open("c.src", WORDS_TYPE);
	x = hg_sub_open("x.mid", WORDS_TYPE);
	req = hg_sub_open("caller.req", WORDS_TYPE); // allowed by the subglob
	CHECK(a && b && c && x && req);
	out1 = open_words("out1");
	out2 = open_words("out2");
	out3 = open_words("out3");
	out4 = open_words("out4");
	out5 = open_words("out5");
	o1 = hg_sub_open("out1", NULL);
	o2 = hg_sub_open("out2", NULL);
	o3 = hg_sub_open("out3", NULL);
	o4 = hg_sub_open("out4", NULL);
	o5 = hg_sub_open("out5", NULL);
	CHECK(o1 && o2 && o3 && o4 && o5);
	// A topic the wiring lists nowhere is refused.
	CHECK(hg_sub_open("out9", NULL) == NULL);
	CHECK_EQ_INT(errno, EACCES);
}

static void test_inputs_recorded_and_reused(void)
{
	// b.in is absent: every out1 message carries bit 0 with an absent
	// reference until it appears.
	pub_one(a_pub);
	CHECK_EQ_INT(hg_next(a, buf, sizeof buf, &info), HG_MSG);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.n_inputs, 2);
	const struct hg_input *ra = ref_to("a.in"), *rb = ref_to("b.in");
	CHECK(ra && rb);
	if (!ra || !rb)
		return;
	CHECK_EQ_INT(ra->seq, 1);
	CHECK_EQ_INT(ra->flags, 0);
	CHECK_EQ_INT(rb->seq, 0);
	CHECK_EQ_INT(rb->flags, HG_REF_ABSENT);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	// b.in appears and is read: the output is fresh.
	b_pub = open_words("b.in");
	pub_one(b_pub);
	CHECK_EQ_INT(hg_next(b, buf, sizeof buf, &info), HG_MSG);
	pub_one(a_pub);
	CHECK_EQ_INT(hg_next(a, buf, sizeof buf, &info), HG_MSG);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.flags, 0);
	ra = ref_to("a.in");
	rb = ref_to("b.in");
	CHECK(ra && ra->seq == 2 && ra->flags == 0);
	CHECK(rb && rb->seq == 1 && rb->flags == 0);
	// Nothing new read since: both references reused.
	pub_one(out1);
	last_of(o1);
	ra = ref_to("a.in");
	rb = ref_to("b.in");
	CHECK(ra && ra->seq == 2 && ra->flags == HG_REF_REUSED);
	CHECK(rb && rb->seq == 1 && rb->flags == HG_REF_REUSED);
	CHECK_EQ_INT(info.flags, 0); // reuse alone is not staleness
	// A new a.in read: a fresh, b reused.
	pub_one(a_pub);
	CHECK_EQ_INT(hg_latest(a, buf, sizeof buf, &info), HG_MSG);
	pub_one(out1);
	last_of(o1);
	ra = ref_to("a.in");
	rb = ref_to("b.in");
	CHECK(ra && ra->seq == 3 && ra->flags == 0);
	CHECK(rb && rb->flags == HG_REF_REUSED);
}

static void test_hg_use_overrides(void)
{
	// A pipelined module: reads 4 and 5, says it computed from 4.
	pub_one(a_pub);
	pub_one(a_pub);
	CHECK_EQ_INT(hg_next(a, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 4);
	struct hg_info used = info;
	CHECK_EQ_INT(hg_next(a, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.seq, 5);
	struct hg_info five = info;
	hg_use(a, &used);
	pub_one(out1);
	last_of(o1);
	const struct hg_input *ra = ref_to("a.in");
	CHECK(ra && ra->seq == 4 && ra->flags == 0);
	// Until the next hg_use, that stays the reference: reused now.
	pub_one(out1);
	last_of(o1);
	ra = ref_to("a.in");
	CHECK(ra && ra->seq == 4 && ra->flags == HG_REF_REUSED);
	hg_use(a, &five); /* back to what was last returned: 5 */
	pub_one(out1);
	last_of(o1);
	ra = ref_to("a.in");
	CHECK(ra && ra->seq == 5 && ra->flags == 0);
}

static void test_sources_and_refusals(void)
{
	pub_one(c_pub);
	pub_one(d_pub);
	pub_one(req_pub);
	CHECK_EQ_INT(hg_next(c, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(hg_next(req, buf, sizeof buf, &info), HG_MSG);
	// A Sources reference, and the request topic matched by *.req.
	struct hg_input refs[5] = { { hg_topic_id("c.src"), 0, 1 }, { hg_topic_id("caller.req"), 0, 1 } };
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, 0, refs, 2), 0);
	last_of(o1);
	CHECK_EQ_INT(info.n_inputs, 4); // two Inputs and the two named Sources
	const struct hg_input *rc = ref_to("c.src"), *rq = ref_to("caller.req");
	CHECK(rc && rc->seq == 1 && rc->flags == 0);
	CHECK(rq && rq->seq == 1 && rq->flags == 0);
	CHECK(ref_to("a.in") != NULL);
	CHECK_EQ_INT(info.flags, 0);
	// A reference to a topic the module reads but has not declared as an
	// Inputs or Sources of this output: refused, nothing published.
	uint64_t before = hg_pub_next_seq(out1);
	struct hg_input bad = { hg_topic_id("d.src"), 0, 1 };
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, 0, &bad, 1), -1);
	CHECK_EQ_INT(errno, EINVAL);
	struct hg_input unknown = { hg_topic_id("nobody.knows"), 0, 1 };
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, 0, &unknown, 1), -1);
	// More than four in all: refused.
	struct hg_input five[5] = { { hg_topic_id("c.src"), 0, 1 }, { hg_topic_id("caller.req"), 0, 1 },
				    { hg_topic_id("a.in"), 0, 1 }, { hg_topic_id("b.in"), 0, 1 },
				    { hg_topic_id("c.src"), 0, 2 } };
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, 0, five, 5), -1);
	CHECK_EQ_INT(errno, EINVAL);
	// out5 has four Inputs: one Source more is one too many.
	struct hg_input xm = { hg_topic_id("x.mid"), 0, 1 };
	CHECK_EQ_INT(hg_publish_with(out5, buf, sizeof buf, 0, 0, &xm, 1), -1);
	CHECK_EQ_INT(errno, EINVAL);
	CHECK_EQ_INT(hg_pub_next_seq(out1), before);
	// Bad flags.
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, 8, NULL, 0), -1);
	// Overriding an Inputs reference: recorded as given, not the latest.
	struct hg_input over = { hg_topic_id("a.in"), 0, 2 };
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, 0, &over, 1), 0);
	last_of(o1);
	const struct hg_input *ra = ref_to("a.in");
	CHECK(ra && ra->seq == 2);
	CHECK_EQ_INT(info.n_inputs, 2);
	// HG_NO_REFS: nothing recorded, Inputs included, not stale.
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, HG_NO_REFS, NULL, 0), 0);
	last_of(o1);
	CHECK_EQ_INT(info.n_inputs, 0);
	CHECK_EQ_INT(info.flags, 0);
	// HG_STALE: bit 0 whatever the references say.
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, HG_STALE, NULL, 0), 0);
	last_of(o1);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	CHECK_EQ_INT(info.n_inputs, 2);
}

static void test_stale_input_flags_output(void)
{
	// a.in at 50 Hz is stale after 60 ms without a message.
	pub_one(a_pub);
	CHECK_EQ_INT(hg_latest(a, buf, sizeof buf, &info), HG_MSG);
	hg_use(a, &info);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.flags, 0);
	sleep_ms(80);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	const struct hg_input *ra = ref_to("a.in");
	CHECK(ra && (ra->flags & HG_REF_STALE));
	CHECK(ra && (ra->flags & HG_REF_REUSED));
	// Transitive: out2 is computed from out1, whose last message is flagged.
	pub_one(out2);
	last_of(o2);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	CHECK_EQ_INT(info.n_inputs, 1);
	CHECK(ref_to("out1") != NULL);
	// The input recovers: both clear.
	pub_one(a_pub);
	CHECK_EQ_INT(hg_latest(a, buf, sizeof buf, &info), HG_MSG);
	hg_use(a, &info);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.flags, 0);
	pub_one(out2);
	last_of(o2);
	CHECK_EQ_INT(info.flags, 0);
	// A dead input: flagged; the reference says stale.
	CHECK_EQ_INT(hg_stub_mark_died("b.in"), 0);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	const struct hg_input *rb = ref_to("b.in");
	CHECK(rb && (rb->flags & HG_REF_STALE));
	// b.in's writer restarts: a new file, no death flag.
	hg_pub_close(b_pub);
	b_pub = open_words("b.in");
	pub_one(b_pub);
	CHECK_EQ_INT(hg_next(b, buf, sizeof buf, &info), HG_MSG);
	pub_one(a_pub);
	CHECK_EQ_INT(hg_latest(a, buf, sizeof buf, &info), HG_MSG);
	hg_use(a, &info);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.flags, 0);
	// A Sources topic that is stale or absent and not referenced does not
	// flag the output: d.src is never read; c.src closed.
	hg_pub_close(c_pub);
	pub_one(out1);
	last_of(o1);
	CHECK_EQ_INT(info.flags, 0);
	// Referenced, a closed source does flag.
	struct hg_input rc = { hg_topic_id("c.src"), 0, 1 };
	CHECK_EQ_INT(hg_publish_with(out1, buf, sizeof buf, 0, 0, &rc, 1), 0);
	last_of(o1);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	const struct hg_input *r = ref_to("c.src");
	CHECK(r && (r->flags & HG_REF_STALE));
}

static void test_transitive_through_a_subscription(void)
{
	// x.mid published with bit 0 (as another module would); out3, computed
	// from the message read, carries it; a fresh x.mid clears it.
	CHECK_EQ_INT(hg_publish_with(x_pub, buf, sizeof buf, 0, HG_STALE, NULL, 0), 0);
	CHECK_EQ_INT(hg_next(x, buf, sizeof buf, &info), HG_MSG);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	pub_one(out3);
	last_of(o3);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
	const struct hg_input *rx = ref_to("x.mid");
	CHECK(rx && rx->seq == 1 && rx->flags == 0); // the input itself is live
	pub_one(x_pub);
	CHECK_EQ_INT(hg_next(x, buf, sizeof buf, &info), HG_MSG);
	pub_one(out3);
	last_of(o3);
	CHECK_EQ_INT(info.flags, 0);
	// An override to the flagged message, still in the ring, is flagged.
	struct hg_input old = { hg_topic_id("x.mid"), 0, 1 };
	CHECK_EQ_INT(hg_publish_with(out3, buf, sizeof buf, 0, 0, &old, 1), 0);
	last_of(o3);
	CHECK_EQ_INT(info.flags, HG_SLOT_STALE);
}

static void test_remote_reference(void)
{
	// <role>:<topic>: recorded as given, never checked (the topic is not on
	// this machine).
	pub_one(out4);
	last_of(o4);
	CHECK_EQ_INT(info.n_inputs, 1);
	CHECK_EQ_INT(info.inputs[0].topic_id, hg_topic_id("cam.pilot"));
	CHECK_EQ_INT(info.inputs[0].seq, 0);
	CHECK_EQ_INT(info.inputs[0].flags, 0);
	CHECK_EQ_INT(info.flags, 0);
	struct hg_input given = { hg_topic_id("cam.pilot"), 0, 77 };
	CHECK_EQ_INT(hg_publish_with(out4, buf, sizeof buf, 0, 0, &given, 1), 0);
	last_of(o4);
	CHECK_EQ_INT(info.inputs[0].seq, 77);
	CHECK_EQ_INT(info.flags, 0);
}

static void test_teardown(void)
{
	hg_sub_close(a);
	hg_sub_close(b);
	hg_sub_close(c);
	hg_sub_close(x);
	hg_sub_close(req);
	// A closed input's link must not dangle: publishing still works, and
	// the link finds this process's own a.in output instead.
	pub_one(out1);
	last_of(o1);
	CHECK(ref_to("a.in") != NULL);
	CHECK(ref_to("a.in") && ref_to("a.in")->seq == hg_pub_next_seq(a_pub) - 1);
	hg_sub_close(o1);
	hg_sub_close(o2);
	hg_sub_close(o3);
	hg_sub_close(o4);
	hg_sub_close(o5);
	hg_pub_close(a_pub);
	hg_pub_close(b_pub);
	hg_pub_close(d_pub);
	hg_pub_close(x_pub);
	hg_pub_close(req_pub);
	hg_pub_close(out1);
	hg_pub_close(out2);
	hg_pub_close(out3);
	hg_pub_close(out4);
	hg_pub_close(out5);
}

int main(void)
{
	hg_test_setup("prov");
	write_wiring();
	RUN_TEST(test_setup);
	RUN_TEST(test_inputs_recorded_and_reused);
	RUN_TEST(test_hg_use_overrides);
	RUN_TEST(test_sources_and_refusals);
	RUN_TEST(test_stale_input_flags_output);
	RUN_TEST(test_transitive_through_a_subscription);
	RUN_TEST(test_remote_reference);
	RUN_TEST(test_teardown);
	unlink(wiring_path);
	hg_test_teardown();
	TEST_MAIN_END();
}
