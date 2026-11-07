// SPDX-License-Identifier: LGPL-3.0-or-later
// The file layout to the byte (docs/01-topics.md "File layout"), read back
// from a real topic file with the stub's raw access rather than through the
// library's own structs, so a wrong offset in the library shows here; the
// canonical schema form, its hash and the topic id against known vectors;
// the frame type; the schema parser's layout rules.
#include "hg_test.h"

static uint32_t rd32(const unsigned char *b, size_t off)
{
	uint32_t v;
	memcpy(&v, b + off, 4);
	return v;
}

static uint64_t rd64(const unsigned char *b, size_t off)
{
	uint64_t v;
	memcpy(&v, b + off, 8);
	return v;
}

static void test_header_offsets(void)
{
	struct hg_pub_opts o = { .slots = 16, .rate_mhz = 50000, .stale_after_ns = 60000000 };
	hg_pub *p = hg_pub_open("lay.depth", DEPTH_TYPE, &o);
	CHECK(p != NULL);
	if (!p)
		return;
	unsigned char h[4096];
	CHECK_EQ_INT(hg_stub_read_header("lay.depth", h), 0);
	CHECK(memcmp(h, "HGTOPIC1", 8) == 0);
	CHECK_EQ_INT(rd32(h, 8), 1);                       // version
	CHECK_EQ_INT(rd32(h, 12), 4096);                   // header_bytes
	CHECK(rd64(h, 16) != 0);                           // generation: random
	CHECK_EQ_INT(rd32(h, 24), 16);                     // slot_count
	CHECK_EQ_INT(rd32(h, 28), 128);                    // slot_header_bytes
	CHECK_EQ_INT(rd32(h, 32), 16);                     // payload_bytes: 3 f32 rounded to 8
	CHECK_EQ_INT(rd32(h, 36), 0);                      // flags
	CHECK_EQ_INT(rd64(h, 40), 192);                    // slot_stride: docs/01 "Memory"
	CHECK_EQ_INT(rd64(h, 48), 4096);                   // slots_offset
	CHECK_EQ_INT(rd32(h, 56), hg_topic_id("lay.depth")); // topic_id
	CHECK_EQ_INT(rd32(h, 60), 50000);                  // rate_mhz
	CHECK(rd64(h, 64) == DEPTH_TYPE->hash);            // type_hash
	CHECK(strcmp((const char *)h + 72, "lay.depth") == 0);
	CHECK(strcmp((const char *)h + 120, hg_module_name()) == 0);
	CHECK_EQ_INT((int32_t)rd32(h, 152), (int32_t)getpid());
	CHECK_EQ_INT(rd32(h, 156), strlen(DEPTH_SCHEMA));  // schema_bytes
	CHECK(rd64(h, 160) != 0);                          // created_real_ns
	CHECK_EQ_INT(rd64(h, 168), 0);                     // first_seq
	CHECK_EQ_INT(rd64(h, 176), 60000000);              // stale_after_ns
	CHECK_EQ_INT(rd32(h, 192), 0);                     // futex_word
	CHECK_EQ_INT(rd32(h, 196), 0);                     // reserved
	CHECK_EQ_INT(rd64(h, 200), 0);                     // write_seq
	CHECK_EQ_INT(rd64(h, 208), 0);                     // last_publish_ns
	static const unsigned char zero[16];
	CHECK(memcmp(h + 256, zero, 16) == 0);             // origin_boot: local
	CHECK(memcmp(h + 512, DEPTH_SCHEMA, strlen(DEPTH_SCHEMA)) == 0);
	// The file is 4096 + slot_count * slot_stride, all reserved.
	char path[600];
	hg_topic_path(path, sizeof path, "", "lay.depth", "");
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (f) {
		fseek(f, 0, SEEK_END);
		CHECK_EQ_INT(ftell(f), 4096 + 16 * 192);
		fclose(f);
	}
	hg_pub_close(p);
	CHECK_EQ_INT(hg_stub_read_header("lay.depth", h), 0);
	CHECK_EQ_INT(rd32(h, 36), HG_FLAG_CLOSED);
	CHECK_EQ_INT(rd32(h, 192), 1);                     // one announcement
}

static void test_slot_offsets(void)
{
	// Wired, so the output has Inputs to record.
	struct hg_pub_opts o = { .slots = 4 };
	hg_pub *p = hg_pub_open("lay.words", WORDS_TYPE, &o);
	CHECK(p != NULL);
	if (!p)
		return;
	uint64_t buf[32];
	fill(buf, 1);
	struct hg_input refs[1] = { { 0, 0, 0 } };
	(void)refs;
	CHECK_EQ_INT(hg_publish(p, buf, sizeof buf, 123456789), 0);
	fill(buf, 2);
	CHECK_EQ_INT(hg_publish_with(p, buf, sizeof buf, 99, HG_STALE, NULL, 0), 0);
	unsigned char s[384];
	CHECK_EQ_INT(hg_stub_read_slot("lay.words", 0, s, sizeof s), 0);
	CHECK_EQ_INT(rd64(s, 0), 2);                       // seq = 2n, n = 1
	CHECK_EQ_INT(rd64(s, 8), 123456789);               // stamp_ns
	CHECK(rd64(s, 16) != 0);                           // publish_ns
	CHECK_EQ_INT(rd32(s, 24), 0);                      // n_inputs
	CHECK_EQ_INT(rd32(s, 28), 0);                      // flags
	for (int i = 32; i < 64; i++)
		CHECK_EQ_INT(s[i], 0);                     // reserved
	CHECK_EQ_INT(rd64(s, 128), 1);                     // payload word 0
	CHECK_EQ_INT(rd64(s, 128 + 248), 1);               // payload word 31
	CHECK_EQ_INT(hg_stub_read_slot("lay.words", 1, s, sizeof s), 0);
	CHECK_EQ_INT(rd64(s, 0), 4);
	CHECK_EQ_INT(rd64(s, 8), 99);
	CHECK_EQ_INT(rd32(s, 28), HG_SLOT_STALE);
	unsigned char h[4096];
	CHECK_EQ_INT(hg_stub_read_header("lay.words", h), 0);
	CHECK_EQ_INT(rd64(h, 40), 384);                    // 128 + 256, a multiple of 64
	CHECK_EQ_INT(rd64(h, 200), 2);                     // write_seq
	CHECK_EQ_INT(rd32(h, 192), 2);                     // futex_word: one per publish
	CHECK(rd64(h, 208) != 0);
	hg_pub_close(p);
}

static void test_input_record(void)
{
	// {u32 topic_id, u32 flags, u64 seq} at 64 + 16 i, as the slot stores it.
	CHECK_EQ_INT(sizeof(struct hg_input), 16);
	CHECK_EQ_INT(offsetof(struct hg_input, topic_id), 0);
	CHECK_EQ_INT(offsetof(struct hg_input, flags), 4);
	CHECK_EQ_INT(offsetof(struct hg_input, seq), 8);
}

static void test_fnv_vectors(void)
{
	// The published FNV-1a test vectors.
	CHECK(hg_fnv1a64("", 0) == 0xcbf29ce484222325ull);
	CHECK(hg_fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
	CHECK(hg_fnv1a64("foobar", 6) == 0x85944171f73967e8ull);
	CHECK_EQ_INT(hg_fnv1a32(""), 0x811c9dc5u);
	CHECK_EQ_INT(hg_fnv1a32("a"), 0xe40c292cu);
	CHECK_EQ_INT(hg_fnv1a32("foobar"), 0xbf9cf968u);
	CHECK_EQ_INT(hg_topic_id("nav.depth"), hg_fnv1a32("nav.depth"));
}

static void test_depth_canonical_and_hash(void)
{
	char canon[256];
	size_t n = hg_schema_canonical(DEPTH_SCHEMA, strlen(DEPTH_SCHEMA), canon, sizeof canon);
	CHECK_EQ_INT(n, strlen(DEPTH_CANONICAL));
	CHECK(strcmp(canon, DEPTH_CANONICAL) == 0);
	// The hash is FNV-1a 64 of the canonical bytes, and nothing else.
	uint64_t ref = hg_fnv1a64(DEPTH_CANONICAL, strlen(DEPTH_CANONICAL));
	CHECK(DEPTH_TYPE->hash == ref);
	// hg_type_abi.md: both implementations print this for msg/depth.msg.
	printf("    depth.msg canonical hash: %016llx\n", (unsigned long long)DEPTH_TYPE->hash);
	CHECK_EQ_INT(DEPTH_TYPE->payload_bytes, 16);
	CHECK_EQ_INT(DEPTH_TYPE->schema_bytes, strlen(DEPTH_SCHEMA));
	CHECK_EQ_INT(DEPTH_TYPE->n_fields, 3);
	CHECK(strcmp(DEPTH_TYPE->fields[0].name, "depth_m") == 0);
	CHECK(strcmp(DEPTH_TYPE->fields[0].comment, "m, positive down") == 0);
	CHECK_EQ_INT(DEPTH_TYPE->fields[1].offset, 4);
	CHECK_EQ_INT(DEPTH_TYPE->fields[2].offset, 8);
	CHECK(strcmp(DEPTH_TYPE->fields[2].comment, "mbar, absolute") == 0);
	// Comments and spacing do not change the hash; a field does.
	struct hg_schema s;
	CHECK_EQ_INT(hg_schema_parse("f32 depth_m\nf32   temp_c\n\n\nf32 pressure_mbar # x\n", 48, &s,
				     NULL, 0), 0);
	CHECK(s.type.hash == ref);
	CHECK_EQ_INT(hg_schema_parse("f32 depth_m\nf32 temp_c\n", 23, &s, NULL, 0), 0);
	CHECK(s.type.hash != ref);
}

static void test_parameter_attributes_ignored(void)
{
	// docs/05 "Parameter types": defaults, ranges and @restart are not part
	// of the type hash.
	const char *params =
		"# msg/depth_hold_params.msg\n"
		"f32  kp = 1.5          [0, 10]   # N/m\n"
		"f32  ki = 0.1          [0, 2]    # N/(m s)\n"
		"f32  max_thrust = 0.6  [0, 1]    # fraction of full thrust\n"
		"f32  gains[3] = 1, 1, 0.5        # one value applies to every element\n"
		"u8   enabled = 0       [0, 1]\n"
		"char frame[16] = \"ned\"\n"
		"u32  rate_hz = 50      [10, 200] @restart\n";
	const char *plain = "f32 kp\nf32 ki\nf32 max_thrust\nf32 gains[3]\nu8 enabled\nchar frame[16]\nu32 rate_hz\n";
	char canon[512];
	size_t n = hg_schema_canonical(params, strlen(params), canon, sizeof canon);
	CHECK_EQ_INT(n, strlen(plain));
	CHECK(strcmp(canon, plain) == 0);
	struct hg_schema s;
	CHECK_EQ_INT(hg_schema_parse(params, strlen(params), &s, NULL, 0), 0);
	CHECK(s.type.hash == hg_fnv1a64(plain, strlen(plain)));
	// Layout: kp 0, ki 4, max_thrust 8, gains 12..24, enabled 24, frame 25..41, rate_hz 44; size 48.
	CHECK_EQ_INT(s.fields[3].offset, 12);
	CHECK_EQ_INT(s.fields[4].offset, 24);
	CHECK_EQ_INT(s.fields[5].offset, 25);
	CHECK_EQ_INT(s.fields[6].offset, 44);
	CHECK_EQ_INT(s.type.payload_bytes, 48);
	CHECK(strcmp(s.fields[0].comment, "N/m") == 0);
	CHECK(strcmp(s.fields[4].comment, "") == 0);
}

static void test_layout_rules(void)
{
	struct hg_schema s;
	char err[128];
	// Natural alignment with explicit padding; size rounded up to 8.
	CHECK_EQ_INT(hg_schema_parse("u8 a\nu64 b\nu16 c\n", 17, &s, NULL, 0), 0);
	CHECK_EQ_INT(s.fields[0].offset, 0);
	CHECK_EQ_INT(s.fields[1].offset, 8);
	CHECK_EQ_INT(s.fields[2].offset, 16);
	CHECK_EQ_INT(s.type.payload_bytes, 24);
	CHECK_EQ_INT(hg_schema_parse("u8 a\n", 5, &s, NULL, 0), 0);
	CHECK_EQ_INT(s.type.payload_bytes, 8);
	CHECK_EQ_INT(hg_schema_parse("i16 a[3]\nf64 b\n", 15, &s, NULL, 0), 0);
	CHECK_EQ_INT(s.fields[0].count, 3);
	CHECK_EQ_INT(s.fields[1].offset, 8);
	CHECK_EQ_INT(hg_field_elem_size(HG_F64), 8);
	CHECK_EQ_INT(hg_field_elem_size(HG_CHAR), 1);
	// Dotted names: dataset rows.
	CHECK_EQ_INT(hg_schema_parse("f32 nav.state.depth_m\nu64 nav.state._seq\n", 41, &s, NULL, 0), 0);
	CHECK(hg_type_field(&s.type, "nav.state._seq") != NULL);
	CHECK(hg_type_field(&s.type, "nope") == NULL);
	// Refusals.
	CHECK_EQ_INT(hg_schema_parse("char name\n", 10, &s, err, sizeof err), -1);
	CHECK(strstr(err, "char") != NULL);
	CHECK_EQ_INT(hg_schema_parse("f32 a\nf32 a\n", 12, &s, err, sizeof err), -1);
	CHECK(strstr(err, "duplicate") != NULL);
	CHECK_EQ_INT(hg_schema_parse("f128 a\n", 7, &s, err, sizeof err), -1);
	CHECK_EQ_INT(hg_schema_parse("u8 a[0]\n", 8, &s, err, sizeof err), -1);
	CHECK_EQ_INT(hg_schema_parse("u8 1a\n", 6, &s, err, sizeof err), -1);
	CHECK_EQ_INT(hg_schema_parse("# only a comment\n", 17, &s, err, sizeof err), -1);
	CHECK_EQ_INT(hg_schema_canonical("f128 a\n", 7, NULL, 0), (size_t)-1);
}

static void test_frame_type(void)
{
	struct hg_frame_type ft;
	CHECK_EQ_INT(hg_frame_type(1280, 720, HG_I422, &ft), 0);
	const char *want = "u32 width\nu32 height\nu32 fourcc\nu32 stride[3]\nu32 offset[3]\nu8 data[1843200]\n";
	CHECK(strcmp(ft.type.schema, want) == 0);
	CHECK_EQ_INT(ft.type.schema_bytes, strlen(want));
	CHECK(ft.type.hash == hg_fnv1a64(want, strlen(want)));
	CHECK_EQ_INT(ft.type.payload_bytes, 1843240); // 36 + 1843200, rounded up to 8
	CHECK_EQ_INT(ft.type.n_fields, 6);
	CHECK_EQ_INT(ft.fields[5].offset, 36);
	CHECK_EQ_INT(ft.fields[5].count, 1843200);
	CHECK_EQ_INT(ft.stride[1], 640);
	CHECK_EQ_INT(ft.offset[2], 1280 * 720 + 640 * 720);
	// The same text parsed from a header gives the same hash and layout.
	struct hg_schema s;
	CHECK_EQ_INT(hg_schema_parse(want, strlen(want), &s, NULL, 0), 0);
	CHECK(s.type.hash == ft.type.hash);
	CHECK_EQ_INT(s.type.payload_bytes, ft.type.payload_bytes);
	CHECK_EQ_INT(s.fields[5].offset, 36);
	// Another size is another type.
	struct hg_frame_type ft2;
	CHECK_EQ_INT(hg_frame_type(640, 480, HG_I422, &ft2), 0);
	CHECK(ft2.type.hash != ft.type.hash);
	CHECK_EQ_INT(hg_frame_type(640, 480, HG_I420, &ft2), 0);
	CHECK_EQ_INT(ft2.data_bytes, 640 * 480 * 3 / 2);
	CHECK_EQ_INT(hg_frame_type(641, 480, HG_I420, &ft2), -1);
	CHECK_EQ_INT(hg_frame_type(0, 480, HG_I422, &ft2), -1);
	CHECK_EQ_INT(hg_frame_type(64, 48, HG_FOURCC('M', 'J', 'P', 'G'), &ft2), -1);
	CHECK_EQ_INT(sizeof(struct hg_frame_header), 36);
}

static void test_frame_topic_defaults_to_4_slots(void)
{
	struct hg_frame_type ft;
	CHECK_EQ_INT(hg_frame_type(64, 48, HG_GREY, &ft), 0);
	hg_pub *p = hg_pub_open("lay.frame", &ft.type, NULL);
	CHECK(p != NULL);
	if (!p)
		return;
	unsigned char h[4096];
	CHECK_EQ_INT(hg_stub_read_header("lay.frame", h), 0);
	CHECK_EQ_INT(rd32(h, 24), 4);
	CHECK_EQ_INT(rd32(h, 32), (36 + 64 * 48 + 7) & ~7u);
	hg_pub_close(p);
}

static void test_names(void)
{
	CHECK(hg_valid_name("nav.depth"));
	CHECK(hg_valid_name("a"));
	CHECK(hg_valid_name("hangar.vehicle.admin.req"));
	CHECK(!hg_valid_name(""));
	CHECK(!hg_valid_name(".a"));
	CHECK(!hg_valid_name("a."));
	CHECK(!hg_valid_name("a..b"));
	CHECK(!hg_valid_name("A"));
	CHECK(!hg_valid_name("a/b"));
	CHECK(!hg_valid_name("a-b"));
	CHECK(hg_valid_name("abcdefghijabcdefghijabcdefghijabcdefghijabcdefg"));   // 47
	CHECK(!hg_valid_name("abcdefghijabcdefghijabcdefghijabcdefghijabcdefgh")); // 48
}

int main(void)
{
	hg_test_setup("layout");
	RUN_TEST(test_header_offsets);
	RUN_TEST(test_slot_offsets);
	RUN_TEST(test_input_record);
	RUN_TEST(test_fnv_vectors);
	RUN_TEST(test_depth_canonical_and_hash);
	RUN_TEST(test_parameter_attributes_ignored);
	RUN_TEST(test_layout_rules);
	RUN_TEST(test_frame_type);
	RUN_TEST(test_frame_topic_defaults_to_4_slots);
	RUN_TEST(test_names);
	hg_test_teardown();
	TEST_MAIN_END();
}
