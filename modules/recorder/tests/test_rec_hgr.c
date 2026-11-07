// SPDX-License-Identifier: GPL-3.0-or-later
/* The .hgr format (docs/06 "Recording files"): the header and record
 * layouts to the byte, a writer/reader round trip of depth messages read
 * from a topic, gaps recorded as gaps and decimation as decimation, a
 * `meta` file without payloads, a cut-off file read up to its last whole
 * record, the searches by seq and by stamp, and a file that is not a
 * recording refused. With HGR_FIXTURE_DIR set, writes depth.hgr there for
 * python/tests/test_record.py. */
#include "rec_test.h"

#include "depth.h"
#include "hg_manager_stub.h"

static char dir[400];

static void path_of(char *out, size_t len, const char *name)
{
	rfmt(out, len, "%s/%s", dir, name);
}

static void test_layout(void)
{
	CHECK_EQ_INT(sizeof(struct hgr_header), 4096);
	CHECK_EQ_INT(sizeof(struct hgr_record), 128);
	CHECK_EQ_INT(offsetof(struct hgr_header, record_bytes), 16);
	CHECK_EQ_INT(offsetof(struct hgr_header, payload_bytes), 32);
	CHECK_EQ_INT(offsetof(struct hgr_header, topic_id), 56);
	CHECK_EQ_INT(offsetof(struct hgr_header, type_hash), 64);
	CHECK_EQ_INT(offsetof(struct hgr_header, name), 72);
	CHECK_EQ_INT(offsetof(struct hgr_header, schema_bytes), 156);
	CHECK_EQ_INT(offsetof(struct hgr_header, origin_boot), 256);
	CHECK_EQ_INT(offsetof(struct hgr_header, schema), 512);
	CHECK_EQ_INT(offsetof(struct hgr_record, stamp_ns), 8);
	CHECK_EQ_INT(offsetof(struct hgr_record, publish_ns), 16);
	CHECK_EQ_INT(offsetof(struct hgr_record, n_inputs), 24);
	CHECK_EQ_INT(offsetof(struct hgr_record, flags), 28);
	CHECK_EQ_INT(offsetof(struct hgr_record, gap), 32);
	CHECK_EQ_INT(offsetof(struct hgr_record, inputs), 64);
}

/* Publishes n depth messages on a topic and records them as the recorder
 * would: hg_next in order, one record each, with the gaps the reader saw. */
static void test_round_trip(void)
{
	struct hg_pub_opts o = { .slots = 16, .rate_mhz = 50000 };
	hg_pub *p = hg_pub_open("rt.depth", &DEPTH_TYPE, &o);
	hg_sub *s = hg_sub_open_from("rt.depth", NULL, HG_OLDEST);
	CHECK(p && s);
	for (int i = 1; i <= 10; i++) {
		struct depth d = { .depth_m = (float)i, .temp_c = 20, .pressure_mbar = 1000 + (float)i };
		struct hg_input ref = { .topic_id = 7, .flags = HG_REF_REUSED, .seq = (uint64_t)i * 3 };
		(void)ref;
		hg_publish(p, &d, sizeof d, 1000000ull * (uint64_t)i);
	}
	struct hg_topic_meta m;
	CHECK_EQ_INT(hg_sub_meta(s, &m), 0);
	uint32_t slen;
	const char *schema = hg_sub_schema(s, &slen);
	CHECK(schema && slen == DEPTH_TYPE.schema_bytes);
	struct hgr_header h;
	hgr_header_init(&h, &m, schema, slen, HGR_FULL, 1, "full", "20260101-000000-test", 0);
	CHECK_EQ_INT(h.record_bytes, 128 + 16);
	CHECK_EQ_INT(h.payload_bytes, 16);
	CHECK(h.type_hash == DEPTH_TYPE.hash);
	CHECK(strcmp(h.name, "rt.depth") == 0);
	CHECK_EQ_INT(h.rate_mhz, 50000);
	CHECK_EQ_INT(h.slot_count, 16);
	char path[600];
	path_of(path, sizeof path, "rt.depth.hgr");
	struct hgr_writer w;
	CHECK_EQ_INT(hgr_create(&w, path, &h), 0);
	CHECK(hgr_create(&(struct hgr_writer){ 0 }, path, &h) < 0 && errno == EEXIST);
	struct depth d;
	struct hg_info info;
	int n = 0;
	while (hg_next(s, &d, sizeof d, &info) == HG_MSG) {
		struct hgr_record r;
		/* The third record pretends two were lost before it, the sixth
		 * that a mirror skipped one on purpose. */
		info.flags |= n == 5 ? HG_SLOT_DECIMATED : 0;
		hgr_record_init(&r, &info, n == 2 ? 2 : 0, n == 5 ? 1 : 0);
		CHECK_EQ_INT(hgr_append(&w, &r, &d), 0);
		n++;
	}
	CHECK_EQ_INT(n, 10);
	CHECK_EQ_INT(w.records, 10);
	CHECK_EQ_INT(w.bytes, 4096 + 10 * 144);
	CHECK_EQ_INT(hgr_close(&w), 0);

	struct hgr_reader r;
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.count, 10);
	CHECK_EQ_INT(r.record_bytes, 144);
	CHECK(memcmp(r.h, &h, sizeof h) == 0);
	for (uint64_t i = 0; i < r.count; i++) {
		const struct hgr_record *rec = hgr_at(&r, i);
		const struct depth *pd = hgr_payload_at(&r, i);
		CHECK_EQ_INT(rec->seq, i + 1);
		CHECK_EQ_INT(rec->stamp_ns, 1000000ull * (i + 1));
		CHECK(pd->depth_m == (float)(i + 1));
		CHECK(pd->pressure_mbar == 1000 + (float)(i + 1));
		if (i == 2) {
			CHECK(rec->flags & HGR_REC_GAP);
			CHECK_EQ_INT(rec->gap, 2);
		} else if (i == 5) {
			CHECK(!(rec->flags & HGR_REC_GAP));
			CHECK(rec->flags & HG_SLOT_DECIMATED);
			CHECK_EQ_INT(rec->gap, 1);
		} else {
			CHECK_EQ_INT(rec->flags, 0);
			CHECK_EQ_INT(rec->gap, 0);
		}
		CHECK_EQ_INT(rec->n_inputs, 0);
	}
	CHECK_EQ_INT(hgr_find_seq(&r, 1), 0);
	CHECK_EQ_INT(hgr_find_seq(&r, 7), 6);
	CHECK_EQ_INT(hgr_find_seq(&r, 11), -1);
	CHECK_EQ_INT(hgr_find_seq(&r, 0), -1);
	CHECK_EQ_INT(hgr_find_stamp(&r, 999999), -1);
	CHECK_EQ_INT(hgr_find_stamp(&r, 1000000), 0);
	CHECK_EQ_INT(hgr_find_stamp(&r, 3500000), 2);
	CHECK_EQ_INT(hgr_find_stamp(&r, 99000000), 9);
	hgr_info(&r, 2, &info);
	CHECK_EQ_INT(info.gap, 2);
	CHECK_EQ_INT(info.seq, 3);
	hgr_info(&r, 5, &info);
	CHECK_EQ_INT(info.skipped, 1);
	CHECK_EQ_INT(info.gap, 0);
	CHECK(hgr_at(&r, 10) == NULL);
	hgr_reader_close(&r);

	/* The fixture for the Python reader: the same file. */
	const char *fx = getenv("HGR_FIXTURE_DIR");
	if (fx && *fx) {
		char to[700];
		rfmt(to, sizeof to, "%s/depth.hgr", fx);
		FILE *in = fopen(path, "rb"), *out = fopen(to, "wb");
		if (in && out) {
			char buf[4096];
			size_t k;
			while ((k = fread(buf, 1, sizeof buf, in)) > 0)
				fwrite(buf, 1, k, out);
		}
		if (in)
			fclose(in);
		if (out)
			fclose(out);
		printf("    wrote %s\n", to);
	}
	hg_sub_close(s);
	hg_pub_close(p);
}

static void test_meta_and_cut_off(void)
{
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	rfmt(m.name, sizeof m.name, "cam.raw");
	m.payload_bytes = 1024;
	m.slot_count = 4;
	m.topic_id = hg_topic_id("cam.raw");
	struct hgr_header h;
	hgr_header_init(&h, &m, "u8 data[1024]\n", 14, HGR_META, 1, "meta", "s", 0);
	CHECK_EQ_INT(h.record_bytes, 128);
	CHECK(h.flags & HGR_FLAG_META);
	char path[600];
	path_of(path, sizeof path, "cam.raw.hgr");
	struct hgr_writer w;
	CHECK_EQ_INT(hgr_create(&w, path, &h), 0);
	for (int i = 1; i <= 5; i++) {
		struct hg_info info = { .seq = (uint64_t)i, .stamp_ns = (uint64_t)i * 10, .publish_ns = (uint64_t)i * 10 + 1 };
		struct hgr_record r;
		hgr_record_init(&r, &info, 0, 0);
		CHECK_EQ_INT(hgr_append(&w, &r, NULL), 0);
	}
	hgr_close(&w);
	struct hgr_reader r;
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.count, 5);
	CHECK(r.meta);
	CHECK(hgr_payload_at(&r, 0) == NULL);
	CHECK_EQ_INT(hgr_at(&r, 4)->seq, 5);
	hgr_reader_close(&r);
	/* Cut off mid-record: the partial record is ignored. */
	CHECK_EQ_INT(truncate(path, 4096 + 4 * 128 + 40), 0);
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.count, 4);
	hgr_reader_close(&r);
	CHECK_EQ_INT(truncate(path, 4096), 0);
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(r.count, 0);
	CHECK_EQ_INT(hgr_find_seq(&r, 1), -1);
	CHECK_EQ_INT(hgr_find_stamp(&r, 100), -1);
	hgr_reader_close(&r);
	CHECK_EQ_INT(truncate(path, 100), 0);
	CHECK(hgr_open(&r, path) < 0 && errno == EPROTO);
	/* Not a recording at all. */
	path_of(path, sizeof path, "junk.hgr");
	char junk[5000];
	memset(junk, 'x', sizeof junk);
	int fd = open(path, O_WRONLY | O_CREAT, 0644);
	CHECK(write(fd, junk, sizeof junk) == (ssize_t)sizeof junk);
	close(fd);
	CHECK(hgr_open(&r, path) < 0 && errno == EPROTO);
	CHECK(hgr_open(&r, "/nonexistent/x.hgr") < 0 && errno == ENOENT);
}

/* A file written while a stamp is 0 is searched by publish time. */
static void test_find_stamp_uses_publish_without_stamp(void)
{
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	rfmt(m.name, sizeof m.name, "ev.t");
	m.payload_bytes = 8;
	struct hgr_header h;
	hgr_header_init(&h, &m, "u8 reserved\n", 12, HGR_FULL, 1, "full", "s", 0);
	char path[600];
	path_of(path, sizeof path, "ev.t.hgr");
	struct hgr_writer w;
	CHECK_EQ_INT(hgr_create(&w, path, &h), 0);
	uint64_t pad = 0;
	for (int i = 1; i <= 3; i++) {
		struct hg_info info = { .seq = (uint64_t)i, .stamp_ns = 0, .publish_ns = (uint64_t)i * 100 };
		struct hgr_record r;
		hgr_record_init(&r, &info, 0, 0);
		hgr_append(&w, &r, &pad);
	}
	hgr_close(&w);
	struct hgr_reader r;
	CHECK_EQ_INT(hgr_open(&r, path), 0);
	CHECK_EQ_INT(hgr_find_stamp(&r, 250), 1);
	CHECK_EQ_INT(hgr_find_stamp(&r, 99), -1);
	hgr_reader_close(&r);
}

int main(void)
{
	rec_setup("hgr");
	rfmt(dir, sizeof dir, "%s/files", R_root);
	mkdir_p(dir, 0755);
	RUN_TEST(test_layout);
	RUN_TEST(test_round_trip);
	RUN_TEST(test_meta_and_cut_off);
	RUN_TEST(test_find_stamp_uses_publish_without_stamp);
	rec_teardown();
	TEST_MAIN_END();
}
