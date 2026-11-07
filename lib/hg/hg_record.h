// SPDX-License-Identifier: LGPL-3.0-or-later
/* Recording files (`.hgr`, docs/06-recorder.md "Recording files"): one
 * topic's messages as fixed-size records after a 4096-byte header, only
 * ever appended to, so a file is an array (numpy.memmap underneath in
 * Python, a mapping here). The writer is the recorder's and the pairer's
 * file sink's; the reader is the offline dataset build's and the tests'.
 * Nothing here allocates: a writer is a descriptor and a few counters, a
 * reader a mapping.
 *
 * Header, 4096 bytes. Where a field is the topic header's (docs/01
 * "Header") it sits at the same offset with the same meaning; the magic
 * differs ("HGREC001"), and the fields a recording adds take the offsets
 * a topic uses for its live state (generation, slot_stride, futex_word,
 * write_seq), which a recording has no use for:
 *
 *   0    char[8]  magic           "HGREC001"
 *   8    u32      version         1
 *   12   u32      header_bytes    4096
 *   16   u64      record_bytes    128 + payload_bytes, or 128 under `meta`
 *   24   u32      slot_count      the topic's Slots, as recorded
 *   28   u32      record_header_bytes  128
 *   32   u32      payload_bytes   the message size, even under `meta`
 *   36   u32      flags           bit 0: payload absent (policy `meta`)
 *   40   u64      policy_n        N of `every:N` (and of `rate:<Hz>`); 1 for `full` and `meta`
 *   48   u64      records_offset  4096
 *   56   u32      topic_id
 *   60   u32      rate_mhz        declared rate in millihertz; 0 for an event topic
 *   64   u64      type_hash
 *   72   char[48] name            the topic
 *   120  char[32] writer          the writing module's name, from the topic
 *   152  u32      policy          0 full, 1 every, 2 rate, 3 meta (HGR_POLICY_*)
 *   156  u32      schema_bytes
 *   160  u64      created_real_ns wall clock when this file was created
 *   168  u64      first_seq       the topic's first_seq when the file was created
 *   176  u64      stale_after_ns
 *   184  u64      created_mono_ns CLOCK_MONOTONIC when this file was created
 *   192  char[64] session         the session id ("<YYYYmmdd-HHMMSS>-<host>")
 *   256  u8[16]   origin_boot     the boot that numbered the messages; zero for a local topic
 *   272  u32      epoch           k of "<topic>+<k>.hgr"; 0 for the first file
 *   276  u32      reserved        0
 *   280  char[32] policy_text     as written in recorder.conf ("every:4")
 *   312  u8[200]  reserved        0
 *   512  char[3584] schema        the .msg text from the topic's header
 *
 * Record header, 128 bytes: the slot header of docs/01 with seq itself
 * (not 2n) at 0 and the gap in the reserved words:
 *
 *   0    u64  seq
 *   8    u64  stamp_ns
 *   16   u64  publish_ns
 *   24   u32  n_inputs
 *   28   u32  flags        the slot's flags (bit 0 stale inputs, bit 1 decimated
 *                          before this one) and bit 16: messages are missing before this one
 *   32   u64  gap          how many: left out on purpose if bit 1 is set and bit 16 is not,
 *                          lost otherwise
 *   40   u8[24]            reserved, 0
 *   64   {u32,u32,u64}[4]  inputs, as in the topic
 *
 * A file whose length is not a whole number of records was cut off
 * mid-write; the reader ignores the partial record. */
#ifndef HG_RECORD_H
#define HG_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "hg_topic.h"

#define HGR_MAGIC "HGREC001"
#define HGR_HEADER_BYTES 4096u
#define HGR_RECORD_HEADER 128u
#define HGR_FLAG_META 1u          /* header flags: payload absent */
#define HGR_REC_GAP (1u << 16)    /* record flags: messages missing before this one */

enum hgr_policy { HGR_FULL = 0, HGR_EVERY = 1, HGR_RATE = 2, HGR_META = 3, HGR_OFF = 4 };

struct hgr_header {
	char magic[8];
	uint32_t version;
	uint32_t header_bytes;
	uint64_t record_bytes;
	uint32_t slot_count;
	uint32_t record_header_bytes;
	uint32_t payload_bytes;
	uint32_t flags;
	uint64_t policy_n;
	uint64_t records_offset;
	uint32_t topic_id;
	uint32_t rate_mhz;
	uint64_t type_hash;
	char name[48];
	char writer[32];
	uint32_t policy;
	uint32_t schema_bytes;
	uint64_t created_real_ns;
	uint64_t first_seq;
	uint64_t stale_after_ns;
	uint64_t created_mono_ns;
	char session[64];
	unsigned char origin_boot[16];
	uint32_t epoch;
	uint32_t reserved276;
	char policy_text[32];
	unsigned char reserved312[200];
	char schema[3584];
};

struct hgr_record {
	uint64_t seq;
	uint64_t stamp_ns;
	uint64_t publish_ns;
	uint32_t n_inputs;
	uint32_t flags;
	uint64_t gap;
	unsigned char reserved[24];
	struct hg_input inputs[HG_MAX_REFS];
};

/* Fills a header for a topic: its metadata and schema (as hg_sub_meta and
 * hg_sub_schema give them), the policy, the session id, the epoch. The
 * times are taken now. */
void hgr_header_init(struct hgr_header *h, const struct hg_topic_meta *m, const char *schema,
		     uint32_t schema_len, enum hgr_policy policy, uint64_t policy_n,
		     const char *policy_text, const char *session, uint32_t epoch);
/* The record header from what a read returned, with the gap: lost (bit 16)
 * or, when the slot carries bit 1 and nothing was lost, skipped. */
void hgr_record_init(struct hgr_record *r, const struct hg_info *info, uint64_t lost, uint64_t skipped);
/* 0 if the header is a recording's; -1 with errno EPROTO otherwise. */
int hgr_header_valid(const struct hgr_header *h, size_t file_len);

/* ---- Writer -------------------------------------------------------------- */

struct hgr_writer {
	int fd;
	uint64_t records;
	uint64_t bytes;
	uint32_t record_bytes;
	uint32_t payload_bytes;
	int meta;
};

/* Creates the file (O_EXCL, mode 0640) and writes the header. 0, or -1
 * with errno. */
int hgr_create(struct hgr_writer *w, const char *path, const struct hgr_header *h);
/* Appends one record: the header and, unless the file is `meta`, the
 * payload (payload_bytes of it). 0, or -1 with errno; a short write
 * leaves a partial record the reader ignores and is reported as EIO. */
int hgr_append(struct hgr_writer *w, const struct hgr_record *r, const void *payload);
int hgr_close(struct hgr_writer *w);

/* ---- Reader -------------------------------------------------------------- */

struct hgr_reader {
	int fd;
	const unsigned char *map;
	size_t len;
	uint64_t count;          /* whole records */
	uint32_t record_bytes;
	uint32_t payload_bytes;
	int meta;
	const struct hgr_header *h;   /* in the mapping */
};

/* Maps the file read-only and validates the header. 0, or -1 with errno
 * (EPROTO: not a recording). */
int hgr_open(struct hgr_reader *r, const char *path);
void hgr_reader_close(struct hgr_reader *r);
const struct hgr_record *hgr_at(const struct hgr_reader *r, uint64_t i);
/* NULL under `meta`. */
const void *hgr_payload_at(const struct hgr_reader *r, uint64_t i);
/* The index of the record with this seq, or -1: seqs rise with the index. */
int64_t hgr_find_seq(const struct hgr_reader *r, uint64_t seq);
/* The newest record stamped at or before t (stamp_ns, or publish_ns when
 * the stamp is 0, as docs/06 "Joins" reads stamps), or -1. */
int64_t hgr_find_stamp(const struct hgr_reader *r, uint64_t t_ns);
/* hg_info from a record, as hg_read_seq would fill it. */
void hgr_info(const struct hgr_reader *r, uint64_t i, struct hg_info *info);

#endif
