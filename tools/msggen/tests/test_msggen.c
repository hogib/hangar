// SPDX-License-Identifier: GPL-3.0-or-later
// The message-type generator: the depth.msg vector from lib/hg/hg_type_abi.md,
// layout (alignment, padding, rounding to 8), every refusal, canonical-form
// equivalence, and the generated msg/*.c linked in: each descriptor must
// agree with a fresh parse of the schema text it carries, and each struct
// with its descriptor's size. The generated files are compiled against
// tests/hg_type.h, the stand-in for lib/hg/hg_type.h.
#define MSGGEN_NO_MAIN
#include "msggen.c"
#include "test_harness.h"

#include "ardusub_state.h"
#include "attitude.h"
#include "battery.h"
#include "call_reply_envelope.h"
#include "call_request_envelope.h"
#include "depth.h"
#include "depth_hold_params.h"
#include "empty.h"
#include "imu.h"
#include "leak.h"
#include "manual_control.h"
#include "mux_state.h"
#include "nav_state.h"
#include "policy_active.h"
#include "policy_load.h"
#include "recorder_status.h"
#include "safety_state.h"
#include "thruster_out.h"
#include "wrench.h"

// The generator's kind order is the ABI's.
_Static_assert((int)MK_U8 == (int)HG_U8 && (int)MK_I8 == (int)HG_I8 && (int)MK_U16 == (int)HG_U16 &&
                   (int)MK_I16 == (int)HG_I16 && (int)MK_U32 == (int)HG_U32 &&
                   (int)MK_I32 == (int)HG_I32 && (int)MK_U64 == (int)HG_U64 &&
                   (int)MK_I64 == (int)HG_I64 && (int)MK_F32 == (int)HG_F32 &&
                   (int)MK_F64 == (int)HG_F64 && (int)MK_CHAR == (int)HG_CHAR,
               "kind order");

static struct msg_type T;
static char ERR[512];

static int parse(const char *text, int params) {
  msg_free(&T);
  snprintf(T.name, sizeof T.name, "t");
  ERR[0] = 0;
  return msg_parse("t.msg", text, strlen(text), params, &T, ERR, sizeof ERR);
}

// text is refused, with a message naming the file and line, holding `what`.
static void expect_refusal(const char *text, int params, const char *what, int line) {
  int rc = parse(text, params);
  CHECK(rc != 0);
  if (rc == 0) {
    printf("    accepted: %s\n", text);
    return;
  }
  char where[64];
  if (line > 0)
    snprintf(where, sizeof where, "t.msg:%d: ", line);
  else
    snprintf(where, sizeof where, "t.msg: ");
  if (strncmp(ERR, where, strlen(where)) != 0 || !strstr(ERR, what))
    printf("    FAIL %s:%d: for \"%s\" got \"%s\", want \"%s...%s\"\n", __FILE__, __LINE__, text, ERR,
           where, what);
  CHECK(strncmp(ERR, where, strlen(where)) == 0);
  CHECK(strstr(ERR, what) != NULL);
}

static uint64_t hash_of(const char *text) {
  int rc = parse(text, 1);
  CHECK(rc == 0);
  if (rc) printf("    %s\n", ERR);
  return T.hash;
}

static const char DEPTH_DOCS[] =
    "# msg/depth.msg\n"
    "f32 depth_m         # m, positive down\n"
    "f32 temp_c          # degrees Celsius\n"
    "f32 pressure_mbar   # mbar, absolute\n";

// The test vector lib/hg/hg_type_abi.md names; lib/hg's test checks the same.
static void test_depth_vector(void) {
  CHECK_EQ_INT(parse(DEPTH_DOCS, 0), 0);
  struct buf c = {0};
  msg_canonical(&T, &c);
  CHECK(strcmp(c.p, "f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n") == 0);
  buf_free(&c);
  CHECK(T.hash == 0xf397a321cd6643a1ull);
  CHECK(fnv1a64("f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n", 41) == 0xf397a321cd6643a1ull);
  CHECK_EQ_INT(T.payload_bytes, 16);
  CHECK_EQ_INT(T.n_fields, 3);
  CHECK_EQ_INT(T.fields[0].offset, 0);
  CHECK_EQ_INT(T.fields[1].offset, 4);
  CHECK_EQ_INT(T.fields[2].offset, 8);
  CHECK(strcmp(T.fields[0].comment, "m, positive down") == 0);
  CHECK(strcmp(T.fields[2].name, "pressure_mbar") == 0);
  CHECK_EQ_INT(T.fields[0].kind, MK_F32);
  CHECK_EQ_INT(T.fields[0].count, 1);
  CHECK_EQ_INT(T.fields[0].is_array, 0);
  // The generated descriptor, compiled from msg/depth.c, says the same.
  CHECK(DEPTH_TYPE.hash == 0xf397a321cd6643a1ull);
  CHECK_EQ_INT(DEPTH_TYPE.payload_bytes, 16);
  CHECK(strcmp(DEPTH_TYPE.name, "depth") == 0);
  CHECK_EQ_INT(DEPTH_TYPE.schema_bytes, strlen(DEPTH_DOCS));
  CHECK(memcmp(DEPTH_TYPE.schema, DEPTH_DOCS, DEPTH_TYPE.schema_bytes) == 0);
  CHECK_EQ_INT(sizeof(struct depth), 16);
}

static void test_alignment_and_padding(void) {
  CHECK_EQ_INT(parse("u8 a\nu64 b\n", 0), 0);
  CHECK_EQ_INT(T.fields[1].offset, 8);
  CHECK_EQ_INT(T.payload_bytes, 16);

  CHECK_EQ_INT(parse("char s[3]\nu16 x\n", 0), 0);
  CHECK_EQ_INT(T.fields[1].offset, 4);
  CHECK_EQ_INT(T.payload_bytes, 8);

  CHECK_EQ_INT(parse("u32 a\nf64 b\n", 0), 0);
  CHECK_EQ_INT(T.fields[1].offset, 8);
  CHECK_EQ_INT(T.payload_bytes, 16);

  CHECK_EQ_INT(parse("u8 a\nu16 b\nu8 c\nu32 d\n", 0), 0);
  CHECK_EQ_INT(T.fields[1].offset, 2);
  CHECK_EQ_INT(T.fields[2].offset, 4);
  CHECK_EQ_INT(T.fields[3].offset, 8);
  CHECK_EQ_INT(T.payload_bytes, 16);

  // Arrays align to their element, and their size is count * element.
  CHECK_EQ_INT(parse("u8 a\ni16 b[3]\nu8 c\n", 0), 0);
  CHECK_EQ_INT(T.fields[1].offset, 2);
  CHECK_EQ_INT(T.fields[2].offset, 8);
  CHECK_EQ_INT(T.payload_bytes, 16);
}

static void test_rounding_to_8(void) {
  CHECK_EQ_INT(parse("u8 a\n", 0), 0);
  CHECK_EQ_INT(T.payload_bytes, 8);
  CHECK_EQ_INT(parse("u8 a\nu8 b\nu8 c\n", 0), 0);
  CHECK_EQ_INT(T.payload_bytes, 8);
  CHECK_EQ_INT(parse("u32 a\nu32 b\nu32 c\n", 0), 0);
  CHECK_EQ_INT(T.payload_bytes, 16);
  CHECK_EQ_INT(parse("u16 a[3]\n", 0), 0);
  CHECK_EQ_INT(T.payload_bytes, 8);
  CHECK_EQ_INT(parse("f32 x[3]\n", 0), 0);
  CHECK_EQ_INT(T.payload_bytes, 16);
  CHECK_EQ_INT(parse("u64 a\n", 0), 0);
  CHECK_EQ_INT(T.payload_bytes, 8);
  CHECK_EQ_INT(parse("char s[9]\n", 0), 0);
  CHECK_EQ_INT(T.payload_bytes, 16);
}

static void test_refusals(void) {
  expect_refusal("f32 1abc\n", 0, "not a C identifier", 1);
  expect_refusal("f32 a-b\n", 0, "not a C identifier", 1);
  expect_refusal("f32 a\nu8 b\nf32 a\n", 0, "duplicate field 'a'", 3);
  expect_refusal("f32 nav.depth\n", 0, "dotted name", 1);
  expect_refusal("depth d\n", 0, "unknown type 'depth'", 1);
  expect_refusal("struct d\n", 0, "unknown type", 1);
  expect_refusal("f32 a[0]\n", 0, "at least 1", 1);
  expect_refusal("f32 a[]\n", 0, "array size", 1);
  expect_refusal("f32 a[x]\n", 0, "array size", 1);
  expect_refusal("f32 a[3\n", 0, "array size", 1);
  expect_refusal("char s\n", 0, "char needs a size", 1);
  expect_refusal("f32 _a\n", 0, "reserved", 1);
  expect_refusal("f32\n", 0, "missing field name", 1);
  expect_refusal("f32 a foo\n", 0, "unexpected 'foo'", 1);
  expect_refusal("", 0, "no fields", 0);
  expect_refusal("# only a comment\n\n", 0, "no fields", 0);
  // Attributes need --params.
  expect_refusal("f32 a = 1\n", 0, "pass --params", 1);
  expect_refusal("f32 a [0, 1]\n", 0, "pass --params", 1);
  expect_refusal("u32 a @restart\n", 0, "pass --params", 1);
  // Default of the wrong shape.
  expect_refusal("f32 a[3] = 1, 2\n", 1, "default has 2 values", 1);
  expect_refusal("f32 a = 1, 2\n", 1, "default has 2 values", 1);
  expect_refusal("u8 a = 300\n", 1, "does not fit u8", 1);
  expect_refusal("u8 a = -1\n", 1, "negative", 1);
  expect_refusal("i8 a = 200\n", 1, "does not fit i8", 1);
  expect_refusal("u8 a = \"x\"\n", 1, "not an integer", 1);
  expect_refusal("f32 a = abc\n", 1, "not a decimal number", 1);
  expect_refusal("f32 a = 0x10\n", 1, "not a decimal number", 1);
  expect_refusal("char s[4] = \"abcd\"\n", 1, "longer than s[4] holds", 1);
  expect_refusal("char s[4] = abc\n", 1, "quoted text", 1);
  expect_refusal("char s[4] = \"abc\n", 1, "unterminated string", 1);
  expect_refusal("f32 a =\n", 1, "missing value", 1);
  expect_refusal("f32 a = 1, 2, 3, 4\n", 1, "default has 4 values", 1);
  // Range of the wrong shape.
  expect_refusal("char s[4] [0, 1]\n", 1, "char field has no range", 1);
  expect_refusal("f32 a [5, 1]\n", 1, "min above max", 1);
  expect_refusal("f32 a [1]\n", 1, "expected [min, max]", 1);
  expect_refusal("f32 a [1, 2, 3]\n", 1, "expected [min, max]", 1);
  expect_refusal("f32 a [1, 2\n", 1, "unterminated range", 1);
  expect_refusal("u8 a [0, 256]\n", 1, "does not fit u8", 1);
  expect_refusal("f32 a = 11 [0, 10]\n", 1, "outside [0, 10]", 1);
  expect_refusal("f32 a [0, 10] = 1\n", 1, "default comes first", 1);
  expect_refusal("u32 a @restart [0, 1]\n", 1, "range comes before @restart", 1);
  expect_refusal("u32 a @restart @restart\n", 1, "given twice", 1);
  expect_refusal("u32 a @reboot\n", 1, "unexpected '@reboot'", 1);
  // Errors name the line the field is on, past comments and blanks.
  expect_refusal("# header\n\nf32 ok\n\n   u8 bad.name\n", 0, "dotted name", 5);

  // Over 3584 bytes is refused; exactly 3584 is not.
  static char big[4096];
  size_t n = 0;
  n += (size_t)snprintf(big + n, sizeof big - n, "u8 a\n");
  while (n < 3584 - 1) big[n++] = '#';
  big[n++] = '\n';
  big[n] = 0;
  CHECK_EQ_INT(n, 3584);
  CHECK_EQ_INT(parse(big, 0), 0);
  big[n - 1] = '#';
  big[n++] = '\n';
  big[n] = 0;
  expect_refusal(big, 0, "at most 3584", 0);

  // At most 128 fields, the bound lib/hg's run-time parser shares.
  static char many[4096];
  n = 0;
  for (int i = 0; i < 128; i++) n += (size_t)snprintf(many + n, sizeof many - n, "u8 f%d\n", i);
  CHECK_EQ_INT(parse(many, 0), 0);
  CHECK_EQ_INT(T.n_fields, 128);
  CHECK_EQ_INT(T.payload_bytes, 128);
  n += (size_t)snprintf(many + n, sizeof many - n, "u8 f128\n");
  expect_refusal(many, 0, "more than 128 fields", 129);

  // Names from paths.
  char name[MSG_MAX_NAME + 1];
  CHECK_EQ_INT(msg_name_from_path("msg/depth.msg", name, sizeof name, ERR, sizeof ERR), 0);
  CHECK(strcmp(name, "depth") == 0);
  CHECK_EQ_INT(msg_name_from_path("depth.msg", name, sizeof name, ERR, sizeof ERR), 0);
  CHECK(msg_name_from_path("msg/nav.state.msg", name, sizeof name, ERR, sizeof ERR) != 0);
  CHECK(strstr(ERR, "dotted type name") != NULL);
  CHECK(msg_name_from_path("msg/depth.txt", name, sizeof name, ERR, sizeof ERR) != 0);
  CHECK(msg_name_from_path("msg/.msg", name, sizeof name, ERR, sizeof ERR) != 0);
  CHECK(msg_name_from_path("msg/1depth.msg", name, sizeof name, ERR, sizeof ERR) != 0);
  CHECK(msg_name_from_path("msg/my-type.msg", name, sizeof name, ERR, sizeof ERR) != 0);
  CHECK(msg_name_from_path("msg/_x.msg", name, sizeof name, ERR, sizeof ERR) != 0);
}

static void test_canonical_equivalence(void) {
  uint64_t bare = hash_of("f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n");
  CHECK(bare == 0xf397a321cd6643a1ull);
  // Comments, blank lines, whitespace and parameter attributes do not count.
  CHECK(hash_of(DEPTH_DOCS) == bare);
  CHECK(hash_of("\n\nf32   depth_m\n\n\tf32 temp_c   # c\n f32  pressure_mbar\n# end") == bare);
  CHECK(hash_of("f32 depth_m = 1 [0, 10] @restart\nf32 temp_c = 2\nf32 pressure_mbar\n") == bare);
  CHECK(hash_of("f32 depth_m\r\nf32 temp_c\r\nf32 pressure_mbar\r\n") == bare);
  // Renaming, reordering or retyping a field does.
  CHECK(hash_of("f32 depth\nf32 temp_c\nf32 pressure_mbar\n") != bare);
  CHECK(hash_of("f32 temp_c\nf32 depth_m\nf32 pressure_mbar\n") != bare);
  CHECK(hash_of("f64 depth_m\nf32 temp_c\nf32 pressure_mbar\n") != bare);
  CHECK(hash_of("f32 depth_m\nf32 temp_c\n") != bare);
  CHECK(hash_of("f32 depth_m[1]\nf32 temp_c\nf32 pressure_mbar\n") != bare);
  // A scalar and a [1] array have different canonical lines.
  CHECK_EQ_INT(parse("u8 a[1]\n", 0), 0);
  struct buf c = {0};
  msg_canonical(&T, &c);
  CHECK(strcmp(c.p, "u8 a[1]\n") == 0);
  buf_free(&c);
}

static void test_attributes(void) {
  CHECK_EQ_INT(parse(DEPTH_HOLD_PARAMS_TYPE.schema, 1), 0);
  CHECK_EQ_INT(T.n_fields, 7);
  CHECK(strcmp(T.fields[0].dflt, "1.5") == 0);
  CHECK(strcmp(T.fields[0].min, "0") == 0);
  CHECK(strcmp(T.fields[0].max, "10") == 0);
  CHECK_EQ_INT(T.fields[0].restart, 0);
  CHECK(strcmp(T.fields[0].comment, "N/m") == 0);
  CHECK(strcmp(T.fields[3].dflt, "1, 1, 0.5") == 0);
  CHECK(strcmp(T.fields[3].min, "") == 0);
  CHECK(strcmp(T.fields[5].dflt, "\"ned\"") == 0);
  CHECK(strcmp(T.fields[6].dflt, "50") == 0);
  CHECK(strcmp(T.fields[6].min, "10") == 0);
  CHECK(strcmp(T.fields[6].max, "200") == 0);
  CHECK_EQ_INT(T.fields[6].restart, 1);
  // A '#' inside a quoted default is not a comment.
  CHECK_EQ_INT(parse("char s[8] = \"a#b\"   # the comment\n", 1), 0);
  CHECK(strcmp(T.fields[0].dflt, "\"a#b\"") == 0);
  CHECK(strcmp(T.fields[0].comment, "the comment") == 0);
  // One value for every element; a 15-byte string in char[16].
  CHECK_EQ_INT(parse("f32 g[3] = 2\nchar s[16] = \"123456789012345\"\n", 1), 0);
  // The generated table.
  CHECK(strcmp(DEPTH_HOLD_PARAMS_META[0].dflt, "1.5") == 0);
  CHECK(strcmp(DEPTH_HOLD_PARAMS_META[0].min, "0") == 0);
  CHECK(strcmp(DEPTH_HOLD_PARAMS_META[0].max, "10") == 0);
  CHECK_EQ_INT(DEPTH_HOLD_PARAMS_META[0].restart, 0);
  CHECK(strcmp(DEPTH_HOLD_PARAMS_META[3].dflt, "1, 1, 0.5") == 0);
  CHECK(strcmp(DEPTH_HOLD_PARAMS_META[5].dflt, "\"ned\"") == 0);
  CHECK_EQ_INT(DEPTH_HOLD_PARAMS_META[6].restart, 1);
}

// Every generated descriptor linked in agrees with a fresh parse of the
// schema text it carries: if msggen.c changed its layout or hash rules, this
// fails until `make msg` regenerates the files.
static const struct hg_type *const ALL_TYPES[] = {
    &ARDUSUB_STATE_TYPE,       &ATTITUDE_TYPE,     &BATTERY_TYPE,         &CALL_REPLY_ENVELOPE_TYPE,
    &CALL_REQUEST_ENVELOPE_TYPE, &DEPTH_TYPE,      &DEPTH_HOLD_PARAMS_TYPE, &EMPTY_TYPE,
    &IMU_TYPE,                 &LEAK_TYPE,         &MANUAL_CONTROL_TYPE,  &MUX_STATE_TYPE,
    &NAV_STATE_TYPE,           &POLICY_ACTIVE_TYPE, &POLICY_LOAD_TYPE,    &RECORDER_STATUS_TYPE,
    &SAFETY_STATE_TYPE,        &THRUSTER_OUT_TYPE, &WRENCH_TYPE,
};

static void test_generated_descriptors_agree(void) {
  size_t n = sizeof ALL_TYPES / sizeof ALL_TYPES[0];
  CHECK_EQ_INT(n, 19);
  for (size_t k = 0; k < n; k++) {
    const struct hg_type *g = ALL_TYPES[k];
    msg_free(&T);
    snprintf(T.name, sizeof T.name, "%s", g->name);
    int rc = msg_parse(g->name, g->schema, g->schema_bytes, 1, &T, ERR, sizeof ERR);
    CHECK_EQ_INT(rc, 0);
    if (rc) {
      printf("    %s: %s\n", g->name, ERR);
      continue;
    }
    CHECK(strlen(g->schema) == g->schema_bytes);
    CHECK(g->schema_bytes <= MSG_MAX_BYTES);
    CHECK(T.hash == g->hash);
    CHECK_EQ_INT(T.payload_bytes, g->payload_bytes);
    CHECK_EQ_INT(g->payload_bytes % 8, 0);
    CHECK_EQ_INT(T.n_fields, g->n_fields);
    if (T.hash != g->hash || T.n_fields != g->n_fields) printf("    %s disagrees\n", g->name);
    for (uint32_t i = 0; i < T.n_fields && i < g->n_fields; i++) {
      CHECK(strcmp(T.fields[i].name, g->fields[i].name) == 0);
      CHECK_EQ_INT((int)T.fields[i].kind, (int)g->fields[i].kind);
      CHECK_EQ_INT(T.fields[i].count, g->fields[i].count);
      CHECK_EQ_INT(T.fields[i].offset, g->fields[i].offset);
      CHECK(strcmp(T.fields[i].comment, g->fields[i].comment) == 0);
    }
    // Regenerating the header gives the asserts the compiler already passed.
    struct buf h = {0};
    char label[128];
    snprintf(label, sizeof label, "msg/%s.msg", g->name);
    gen_header(&T, label, &h);
    char want[160];
    snprintf(want, sizeof want, "_Static_assert(sizeof(struct %s) == %" PRIu32 ", \"layout\");", g->name,
             g->payload_bytes);
    CHECK(strstr(h.p, want) != NULL);
    CHECK(strstr(h.p, "// SPDX-License-Identifier: LGPL-3.0-or-later\n") != NULL);
    CHECK(strstr(h.p, "#include \"hg_type.h\"\n") != NULL);
    buf_free(&h);
  }
}

#define SZ(s, T) CHECK_EQ_INT(sizeof(struct s), T##_TYPE.payload_bytes)
static void test_struct_sizes(void) {
  SZ(ardusub_state, ARDUSUB_STATE);
  SZ(attitude, ATTITUDE);
  SZ(battery, BATTERY);
  SZ(call_reply_envelope, CALL_REPLY_ENVELOPE);
  SZ(call_request_envelope, CALL_REQUEST_ENVELOPE);
  SZ(depth, DEPTH);
  SZ(depth_hold_params, DEPTH_HOLD_PARAMS);
  SZ(empty, EMPTY);
  SZ(imu, IMU);
  SZ(leak, LEAK);
  SZ(manual_control, MANUAL_CONTROL);
  SZ(mux_state, MUX_STATE);
  SZ(nav_state, NAV_STATE);
  SZ(policy_active, POLICY_ACTIVE);
  SZ(policy_load, POLICY_LOAD);
  SZ(recorder_status, RECORDER_STATUS);
  SZ(safety_state, SAFETY_STATE);
  SZ(thruster_out, THRUSTER_OUT);
  SZ(wrench, WRENCH);
  CHECK_EQ_INT(sizeof(struct empty), 8);
  CHECK_EQ_INT(sizeof(depth_hold_params), 48);  // the typedef docs/05 uses
}

// The envelopes, as docs/04-calls.md gives them.
static void test_envelopes(void) {
  CHECK_EQ_INT(sizeof(struct call_request_envelope), 112);
  CHECK_EQ_INT(offsetof(struct call_request_envelope, call_id), 0);
  CHECK_EQ_INT(offsetof(struct call_request_envelope, deadline_ns), 8);
  CHECK_EQ_INT(offsetof(struct call_request_envelope, request_type), 16);
  CHECK_EQ_INT(offsetof(struct call_request_envelope, target_id), 24);
  CHECK_EQ_INT(offsetof(struct call_request_envelope, reserved), 28);
  CHECK_EQ_INT(offsetof(struct call_request_envelope, method), 32);
  CHECK_EQ_INT(sizeof ((struct call_request_envelope *)0)->method, 32);
  CHECK_EQ_INT(offsetof(struct call_request_envelope, caller), 64);
  CHECK_EQ_INT(sizeof ((struct call_request_envelope *)0)->caller, 48);

  CHECK_EQ_INT(sizeof(struct call_reply_envelope), 96);
  CHECK_EQ_INT(offsetof(struct call_reply_envelope, call_id), 0);
  CHECK_EQ_INT(offsetof(struct call_reply_envelope, caller_id), 8);
  CHECK_EQ_INT(offsetof(struct call_reply_envelope, status), 12);
  CHECK_EQ_INT(offsetof(struct call_reply_envelope, reply_type), 16);
  CHECK_EQ_INT(offsetof(struct call_reply_envelope, received_ns), 24);
  CHECK_EQ_INT(offsetof(struct call_reply_envelope, reason), 32);
  CHECK_EQ_INT(sizeof ((struct call_reply_envelope *)0)->reason, 64);

  // The layouts docs/07, docs/09 and docs/10 spell out.
  CHECK_EQ_INT(sizeof(struct safety_state), 128);
  CHECK_EQ_INT(offsetof(struct safety_state, triggers), 4);
  CHECK_EQ_INT(offsetof(struct safety_state, since_ns), 8);
  CHECK_EQ_INT(offsetof(struct safety_state, by), 16);
  CHECK_EQ_INT(offsetof(struct safety_state, reason), 64);
  CHECK_EQ_INT(sizeof(struct ardusub_state), 176);
  CHECK_EQ_INT(offsetof(struct ardusub_state, mode), 4);
  CHECK_EQ_INT(offsetof(struct ardusub_state, last_rx_ns), 8);
  CHECK_EQ_INT(offsetof(struct ardusub_state, arm_reason), 24);
  CHECK_EQ_INT(offsetof(struct ardusub_state, failsafe), 124);
  CHECK_EQ_INT(sizeof(struct policy_active), 104);
  CHECK_EQ_INT(offsetof(struct policy_active, hash), 8);
  CHECK_EQ_INT(offsetof(struct policy_active, dataset), 32);
  CHECK_EQ_INT(offsetof(struct policy_active, newest_row_boot), 72);
  CHECK_EQ_INT(offsetof(struct policy_active, rows), 96);
  CHECK_EQ_INT(sizeof(struct policy_load), 88);
  CHECK_EQ_INT(offsetof(struct policy_load, reason), 24);
  CHECK_EQ_INT(sizeof(struct manual_control), 16);
  CHECK_EQ_INT(offsetof(struct manual_control, buttons), 8);
}

static void test_source_generation(void) {
  CHECK_EQ_INT(parse("u8 a   # a \"quoted\" comment with a backslash \\ and ?\?= trigraph\n", 0), 0);
  struct buf c = {0};
  gen_source(&T, "msg/t.msg", &c);
  CHECK(strstr(c.p, "#include \"t.h\"") != NULL);
  CHECK(strstr(c.p, "static const struct hg_field t_fields[1]") != NULL);
  CHECK(strstr(c.p, "{\"a\", HG_U8, 1, 0, \"a \\\"quoted\\\" comment with a backslash \\\\ and \\?\\?= trigraph\"}") != NULL);
  CHECK(strstr(c.p, ".hash = 0x") != NULL);
  CHECK(strstr(c.p, ".payload_bytes = 8,") != NULL);
  CHECK(strstr(c.p, "hg_param_meta") == NULL);  // not a parameter type
  buf_free(&c);
  // Non-ASCII bytes in the schema text are octal escapes in the literal.
  CHECK_EQ_INT(parse("u8 a   # \xc2\xb5s\n", 0), 0);
  gen_source(&T, "msg/t.msg", &c);
  CHECK(strstr(c.p, "\\302\\265s") != NULL);
  buf_free(&c);
  struct buf h = {0};
  gen_header(&T, "msg/t.msg", &h);
  CHECK(strstr(h.p, "// Generated by hangar msg gen from msg/t.msg; do not edit.\n") != NULL);
  CHECK(strstr(h.p, "typedef struct t {\n") != NULL);
  CHECK(strstr(h.p, "uint8_t _pad0[7];") != NULL);
  CHECK(strstr(h.p, "extern const struct hg_type T_TYPE;   // defined in msg/t.c, also generated") != NULL);
  buf_free(&h);
  // The label is "<dir>/<file>" however the path was given.
  char label[256];
  msg_label("/home/x/hangar/msg/depth.msg", label, sizeof label);
  CHECK(strcmp(label, "msg/depth.msg") == 0);
  msg_label("msg/depth.msg", label, sizeof label);
  CHECK(strcmp(label, "msg/depth.msg") == 0);
  msg_label("depth.msg", label, sizeof label);
  CHECK(strcmp(label, "depth.msg") == 0);
}

int main(void) {
  RUN_TEST(test_depth_vector);
  RUN_TEST(test_alignment_and_padding);
  RUN_TEST(test_rounding_to_8);
  RUN_TEST(test_refusals);
  RUN_TEST(test_canonical_equivalence);
  RUN_TEST(test_attributes);
  RUN_TEST(test_generated_descriptors_agree);
  RUN_TEST(test_struct_sizes);
  RUN_TEST(test_envelopes);
  RUN_TEST(test_source_generation);
  msg_free(&T);
  TEST_MAIN_END();
}
