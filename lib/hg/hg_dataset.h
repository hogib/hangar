// SPDX-License-Identifier: LGPL-3.0-or-later
/* Datasets (docs/06-recorder.md "Datasets"): the spec, the row type built
 * from it and the types of the topics it names, and the one join code that
 * the live pairer, the offline build and a policy run. Nothing here reads a
 * topic or a file itself: a part's messages come through a `source`, which
 * the caller backs with an hg_sub (hg_read_seq, hg_find_stamp) or with a
 * recording (hgr_find_seq, hgr_find_stamp), so the same bytes come out of
 * both.
 *
 * The row (docs/06 "The row"), as a message type whose schema this file
 * generates (hg_ds_bind): the header fields, then the anchor's selected
 * fields named `<anchor>.<field>`, then for each part `<part>._seq`,
 * `<part>._stamp_ns`, `<part>._count`, `<part>._flags` and its selected
 * fields, `<part>.<field>` or, for a window, `<part>.<k>.<field>` with k
 * from 0 (the oldest) to K-1 (the joined message). Each field's comment
 * starts with `role=meta|input|label|filter`, then the .msg comment of the
 * field it came from. The layout is the .msg layout of that schema
 * (natural alignment, size rounded up to 8), so the header is
 *
 *   0   u64  anchor_seq
 *   8   u64  anchor_stamp_ns
 *   16  u8   boot_id[16]
 *   32  u32  missing          bit i: part i missing
 *   36  u32  flags            bit 0 reset, bit 1 masked, bit 2 late
 *   40  u64  emitted_ns
 *   48  ...  the anchor's fields, then the parts
 *
 * and a missing part, or a message of a window that `count` does not fill,
 * is zeros. */
#ifndef HG_DATASET_H
#define HG_DATASET_H

#include <stddef.h>
#include <stdint.h>

#include "hg_topic.h"
#include "hg_type.h"

#define HG_DS_MAX_PARTS 16
#define HG_DS_MAX_SELECT 32          /* fields kept per part (Fields=) */
#define HG_DS_MAX_WINDOW 64
#define HG_DS_MAX_MAP 128            /* row fields in all: the schema's bound */
#define HG_DS_WHERE_MAX 64           /* nodes of a Where expression */
#define HG_DS_NAME 32
#define HG_DS_WHERE_TEXT 512

/* Row flags. */
#define HG_DS_ROW_RESET 1u
#define HG_DS_ROW_MASKED 2u
#define HG_DS_ROW_LATE 4u
/* A part's _flags: bits 0-7 the slot flags of the joined message; for a
 * provenance join bits 8-10 the anchor's reference flags. */
#define HG_DS_PART_REUSED (HG_REF_REUSED << 8)
#define HG_DS_PART_STALE (HG_REF_STALE << 8)
#define HG_DS_PART_ABSENT (HG_REF_ABSENT << 8)

#define HG_DS_HDR_BYTES 48
#define HG_DS_OFF_ANCHOR_SEQ 0
#define HG_DS_OFF_ANCHOR_STAMP 8
#define HG_DS_OFF_BOOT 16
#define HG_DS_OFF_MISSING 32
#define HG_DS_OFF_FLAGS 36
#define HG_DS_OFF_EMITTED 40

enum hg_ds_role { HG_DS_INPUT = 0, HG_DS_LABEL = 1, HG_DS_FILTER = 2 };
enum hg_ds_join { HG_DS_PROVENANCE = 0, HG_DS_ASOF = 1 };
enum hg_ds_filter { HG_DS_DROP = 0, HG_DS_MASK = 1 };
enum hg_ds_rate { HG_DS_EVERY = 0, HG_DS_RATE_HZ = 1 };

const char *hg_ds_role_name(enum hg_ds_role r);

/* ---- The spec -------------------------------------------------------------- */

struct hg_ds_part_spec {
	char name[HG_NAME_MAX + 1];
	char topic[HG_NAME_MAX + 1];
	enum hg_ds_join join;
	int64_t offset_ns;            /* asof */
	uint32_t window;              /* 1.. */
	enum hg_ds_role role;
	int n_fields;                 /* 0: all */
	char fields[HG_DS_MAX_SELECT][HG_FIELD_NAME_MAX + 1];
	int line;
};

struct hg_ds_spec {
	char name[HG_DS_NAME];
	char anchor[HG_NAME_MAX + 1];
	enum hg_ds_role anchor_role;
	int n_anchor_fields;
	char anchor_fields[HG_DS_MAX_SELECT][HG_FIELD_NAME_MAX + 1];
	enum hg_ds_rate rate_kind;
	uint64_t rate_every;          /* N of every:N */
	uint32_t rate_mhz;            /* of rate:<Hz> */
	char where[HG_DS_WHERE_TEXT]; /* "" when none */
	enum hg_ds_filter filter;
	int missing_drop;             /* Missing=drop */
	int64_t reset_after_ns;
	int64_t lateness_ns;
	int n_parts;
	struct hg_ds_part_spec parts[HG_DS_MAX_PARTS];
};

/* Parses a spec's text (name: the file's stem). Strict as a manifest: an
 * unknown section or key, a repeated key, a bad value, a part twice, a
 * missing Anchor or Join are refused: -1, errno EINVAL, one line in err.
 * Where is checked for syntax; its fields are checked at bind. */
int hg_ds_parse(const char *text, size_t len, const char *name, struct hg_ds_spec *out, char *err, size_t errlen);
/* Reads <path> (a .dataset file; the stem is the name). */
int hg_ds_read_file(const char *path, struct hg_ds_spec *out, char *err, size_t errlen);
/* The wait after the anchor before a live row closes: the largest
 * offset + Lateness over the asof joins, at least 0. */
int64_t hg_ds_wait_ns(const struct hg_ds_spec *s);
/* 1 if every input-role asof join has offset + Lateness <= 0 (docs/06
 * "Specs a policy can run on"); otherwise 0 and, if out is given, the
 * names of the offending parts separated by spaces. */
int hg_ds_policy_usable(const struct hg_ds_spec *s, char *out, size_t outlen);
/* Checks the provenance joins against the manifests in dir (every
 * *.module): each must name a topic in the anchor's Inputs or Sources.
 * 0; -1 with EINVAL and the reason, or -1 with ENOENT when no manifest
 * publishes the anchor. */
int hg_ds_check_manifests(const struct hg_ds_spec *s, const char *dir, char *err, size_t errlen);

/* ---- The row type ------------------------------------------------------------ */

struct hg_ds_topic_info {
	const struct hg_type *type;   /* the topic's type, as parsed from its header */
	uint32_t topic_id;
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
};

struct hg_ds_field_map {
	uint32_t src_off;             /* in the message */
	uint32_t row_off;             /* in the row */
	uint32_t bytes;
	uint32_t k;                   /* message of the window, 0 the oldest */
	enum hg_field_kind kind;
};

struct hg_ds_part {
	uint32_t topic_id;
	uint32_t payload_bytes;
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
	uint32_t hdr_off;             /* _seq; _stamp_ns at +8, _count +16, _flags +20 */
	int first_map, n_map;         /* into row_type.map */
	int immediate;                /* joined when the anchor is read (docs/06 "Live") */
};

enum hg_ds_op { HG_DS_OP_CMP = 0, HG_DS_OP_AND = 1, HG_DS_OP_OR = 2 };
enum hg_ds_cmp { HG_DS_EQ, HG_DS_NE, HG_DS_LT, HG_DS_LE, HG_DS_GT, HG_DS_GE };

struct hg_ds_where_node {
	enum hg_ds_op op;
	enum hg_ds_cmp cmp;
	int part;                     /* -1: the anchor */
	uint32_t row_off;
	enum hg_field_kind kind;
	double value;
};

struct hg_ds_row_type {
	struct hg_ds_spec spec;
	struct hg_schema schema;      /* the row schema, parsed: .type is the row's hg_type */
	uint32_t row_bytes;
	uint32_t anchor_topic_id, anchor_payload_bytes, anchor_rate_mhz;
	uint64_t anchor_stale_after_ns;
	int n_anchor_map;
	struct hg_ds_field_map anchor_map[HG_DS_MAX_SELECT];
	int n_map;
	struct hg_ds_field_map map[HG_DS_MAX_MAP];
	struct hg_ds_part parts[HG_DS_MAX_PARTS];
	int64_t wait_ns;
	uint32_t scratch_bytes;       /* the largest part payload: what a join's buffer must hold */
	uint32_t where_parts;         /* bit i: part i is named in Where */
	int n_where;
	struct hg_ds_where_node where[HG_DS_WHERE_MAX];
};

/* Builds the row type from the spec and the topics' types (parts in the
 * spec's order). Refused (-1, EINVAL, err): a field that does not exist, a
 * Where field that does not exist or is an array, a rate:<Hz> Rate on an
 * event anchor, a row schema over 3584 bytes or 128 fields. The row type
 * is about 60 KB: give it static or heap storage. */
int hg_ds_bind(const struct hg_ds_spec *s, const struct hg_ds_topic_info *anchor,
	       const struct hg_ds_topic_info *parts, struct hg_ds_row_type *out, char *err, size_t errlen);
/* The row schema's text (what the manager would generate for
 * Type=dataset:<name>) is out->schema.text, out->schema.type.schema_bytes long. */

/* ---- Joining ------------------------------------------------------------------ */

/* Where a part's messages come from. Both return HG_MSG with info filled
 * (seq, stamp_ns, publish_ns, flags, n_inputs, inputs) and the payload
 * copied into buf, or HG_GONE / HG_NONE / HG_ABSENT. */
struct hg_ds_source {
	void *ctx;
	enum hg_result (*read_seq)(void *ctx, uint64_t seq, void *buf, size_t len, struct hg_info *info);
	enum hg_result (*find_stamp)(void *ctx, uint64_t t_ns, void *buf, size_t len, struct hg_info *info);
};
/* A source over an hg_sub (the pairer's), and over a recording (the
 * offline build's; struct hgr_reader of hg_record.h). */
struct hg_ds_source hg_ds_source_sub(hg_sub *s);
struct hgr_reader;
struct hg_ds_source hg_ds_source_file(const struct hgr_reader *r);

/* The anchor's time for joins: its stamp, or its publish time when the
 * stamp is 0. */
uint64_t hg_ds_anchor_time(const struct hg_info *anchor);

/* Starts a row: zeros it, fills the header (boot: the anchor topic's
 * origin_boot, or this machine's boot id when that is zero), copies the
 * anchor's fields, sets emitted_ns. */
void hg_ds_row_begin(const struct hg_ds_row_type *rt, unsigned char *row, const struct hg_info *anchor,
		     const void *anchor_payload, const unsigned char boot[16]);

struct hg_ds_joined {
	int missing;
	int stale;                    /* slot bit 0, reference bit stale, or older than StaleAfter (asof) */
	uint64_t publish_ns;          /* of the (last) joined message; 0 when missing */
	uint64_t latency_ns;          /* now - publish_ns at the join, for the pairer's status */
};
/* Joins part i for the anchor into row; scratch holds rt->scratch_bytes. */
void hg_ds_join(const struct hg_ds_row_type *rt, int i, unsigned char *row, const struct hg_info *anchor,
		const struct hg_ds_source *src, void *scratch, struct hg_ds_joined *out);

/* The reset rule's memory, one per dataset stream; zero it to start. */
struct hg_ds_state {
	int started;
	uint32_t epoch;
	int bad[HG_DS_MAX_PARTS];
	uint64_t bad_since[HG_DS_MAX_PARTS];
};
/* After every part is joined: applies Missing, Where and Filter, sets the
 * reset flag. anchor_gap: messages of the anchor topic were lost before
 * this one (a gap the reader saw, not one Rate made). Returns 1 to publish
 * the row, 0 to drop it; the state advances either way. */
int hg_ds_row_finish(const struct hg_ds_row_type *rt, struct hg_ds_state *st, unsigned char *row,
		     const struct hg_ds_joined *joined, int anchor_gap, uint32_t epoch);
/* 1 if the anchor message is one Rate keeps. */
int hg_ds_rate_keeps(const struct hg_ds_row_type *rt, uint64_t anchor_seq);
/* Evaluates Where on a row; 1 true, 0 false (also when a part it names is
 * missing); 1 when there is no Where. */
int hg_ds_where(const struct hg_ds_row_type *rt, const unsigned char *row);

/* This machine's boot id (/proc/sys/kernel/random/boot_id), 16 bytes; zeros
 * if it cannot be read. */
void hg_ds_boot_id(unsigned char out[16]);

#endif
