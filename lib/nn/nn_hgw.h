// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
/*
 * The .hgw weights file (docs/09-learning.md, "The weights file") and its
 * loader with the numbered checks ("Loading weights").
 *
 *   [ header: 4096 bytes ][ section directory ][ sections ][ tensor data, 64-byte aligned ]
 *
 * Every integer is little-endian; every record is a fixed-size struct whose
 * layout is pinned below by _Static_asserts, so a reader maps the file and
 * reads it in place. The section directory starts at byte 4096 and holds
 * `section_count` entries of 32 bytes; each section starts at a 64-byte
 * aligned offset and is an array of its record type. A reader skips a
 * section whose name it does not know. Tensor data and probe blobs each
 * start 64-byte aligned.
 *
 * Header (4096 bytes; offsets in bytes):
 *
 *   off  size  field
 *     0     8  magic                "HGWEIGHT"
 *     8     4  format_version       1
 *    12     4  header_size          4096
 *    16     8  arch_hash            FNV-1a 64 of the canonical architecture (nn_hgw_arch_hash)
 *    24    32  name                 model name, NUL-padded
 *    56     8  tick_period_ns       the tick period the model was trained at
 *    64     4  warmup_ticks
 *    68     4  lineage_count        entries valid in sessions[] / ranges[] (<= 8)
 *    72     4  probe_rel_tol        f32
 *    76     4  probe_abs_tol        f32
 *    80     8  created_ns           unix time, ns
 *    88    32  trainer_program
 *   120    32  trainer_version
 *   152     8  trainer_settings_hash
 *   160    32  dataset              lineage: dataset name
 *   192     8  dataset_hash         lineage: hash of the dataset spec
 *   200     8  rows                 lineage: rows used
 *   208     8  steps                lineage: training steps
 *   216     8  parent_hash          lineage: BLAKE2s-256 (first 8 bytes) of the parent weights, 0 if none
 *   224    16  newest_row_boot      lineage: vehicle boot id of the newest row trained on
 *   240     8  newest_row_stamp_ns  lineage: its anchor_stamp_ns
 *   248     1  autopilot_mode       the mux mode the rows were recorded under (0 unknown)
 *   249     3  reserved0
 *   252     4  activation_bound     f32, 0 = no bound (check 6)
 *   256     8  file_size
 *   264     4  section_count
 *   268     4  section_dir_offset   4096
 *   272     8  tensor_data_offset   first byte of the tensor data region (64-aligned)
 *   280     8  reserved1
 *   288   384  sessions[8]          lineage summary: { u8 boot[16]; char session[32] } (session "live" for unrecorded rows)
 *   672   320  ranges[8]            lineage summary: { u64 first_seq, last_seq, first_stamp_ns, last_stamp_ns, rows }
 *   992  3104  zero
 *
 * Section directory entry (32 bytes): char name[16]; u64 offset; u64 size.
 *
 * Section records:
 *
 *   ops      192  nn_hgw_op       type[16], version, n_inputs, n_tensors, n_attrs, inputs[4] (buffer
 *                                 indices), output (buffer index), tensors[16] (tensor indices),
 *                                 attrs[8] (f64; meaning per op type, see nn_ops.h)
 *   tensors   80  nn_hgw_tensor   name[32], dtype, ndim, shape[4], offset (absolute, 64-aligned), size
 *   buffers   64  nn_hgw_buffer   name[32], dtype, ndim, shape[4]
 *   inputs    64  nn_hgw_binding  buffer, element, field[56]: element `element` of buffer `buffer`
 *                                 is the row field `field`; the model's inputs are the distinct
 *                                 buffers in order of first appearance, every element bound once
 *   outputs   64  nn_hgw_binding  as inputs, with the fields of the output type
 *   probes    32  nn_hgw_probe    steps, offset, size of a blob of `steps` records; a record is,
 *                                 for each model input in order, its raw bytes padded to 8, then
 *                                 for each model output in order, its f32 values padded to 8
 *   ranges    16  nn_hgw_range    lo, hi (f64), one per input binding, in order (optional)
 *   lineage   96  nn_hgw_lineage  boot[16], session[32], first_seq, last_seq, first_stamp_ns,
 *                                 last_stamp_ns, rows (optional; the header holds the first 8)
 *
 * The architecture hash is FNV-1a 64 over the canonical text that
 * nn_hgw_arch_canon() writes; see lib/nn/README.md for its exact form.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn_ops.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the .hgw reader reads the file in place and needs a little-endian host"
#endif

#define NN_HGW_MAGIC "HGWEIGHT"
#define NN_HGW_FORMAT_VERSION 1u
#define NN_HGW_HEADER_SIZE 4096u
#define NN_HGW_ALIGN 64u
#define NN_HGW_MAX_LINEAGE 8u
#define NN_HGW_NAME_LEN 32u
#define NN_HGW_FIELD_LEN 56u
#define NN_HGW_SECTION_NAME_LEN 16u

typedef struct nn_hgw_session {
  uint8_t boot[16];
  char session[32];
} nn_hgw_session;

typedef struct nn_hgw_range_summary {
  uint64_t first_seq, last_seq, first_stamp_ns, last_stamp_ns, rows;
} nn_hgw_range_summary;

typedef struct nn_hgw_header {
  char magic[8];
  uint32_t format_version;
  uint32_t header_size;
  uint64_t arch_hash;
  char name[NN_HGW_NAME_LEN];
  uint64_t tick_period_ns;
  uint32_t warmup_ticks;
  uint32_t lineage_count;
  float probe_rel_tol;
  float probe_abs_tol;
  uint64_t created_ns;
  char trainer_program[32];
  char trainer_version[32];
  uint64_t trainer_settings_hash;
  char dataset[32];
  uint64_t dataset_hash;
  uint64_t rows;
  uint64_t steps;
  uint64_t parent_hash;
  uint8_t newest_row_boot[16];
  uint64_t newest_row_stamp_ns;
  uint8_t autopilot_mode;
  uint8_t reserved0[3];
  float activation_bound;
  uint64_t file_size;
  uint32_t section_count;
  uint32_t section_dir_offset;
  uint64_t tensor_data_offset;
  uint64_t reserved1;
  nn_hgw_session sessions[NN_HGW_MAX_LINEAGE];
  nn_hgw_range_summary ranges[NN_HGW_MAX_LINEAGE];
  uint8_t zero[NN_HGW_HEADER_SIZE - 992];
} nn_hgw_header;

typedef struct nn_hgw_section {
  char name[NN_HGW_SECTION_NAME_LEN];
  uint64_t offset;
  uint64_t size;
} nn_hgw_section;

typedef struct nn_hgw_op {
  char type[16];
  uint32_t version;
  uint32_t n_inputs;
  uint32_t n_tensors;
  uint32_t n_attrs;
  uint32_t inputs[NN_MAX_OP_INPUTS];
  uint32_t output;
  uint32_t reserved0;
  uint32_t tensors[NN_MAX_OP_TENSORS];
  uint32_t reserved1[2];
  double attrs[NN_MAX_ATTRS];
} nn_hgw_op;

typedef struct nn_hgw_tensor {
  char name[NN_HGW_NAME_LEN];
  uint32_t dtype;
  uint32_t ndim;
  uint32_t shape[NN_MAX_DIMS];
  uint64_t offset;
  uint64_t size;
  uint64_t reserved;
} nn_hgw_tensor;

typedef struct nn_hgw_buffer {
  char name[NN_HGW_NAME_LEN];
  uint32_t dtype;
  uint32_t ndim;
  uint32_t shape[NN_MAX_DIMS];
  uint32_t reserved[2];
} nn_hgw_buffer;

typedef struct nn_hgw_binding {
  uint32_t buffer;
  uint32_t element;
  char field[NN_HGW_FIELD_LEN];
} nn_hgw_binding;

typedef struct nn_hgw_probe {
  uint32_t steps;
  uint32_t reserved0;
  uint64_t offset;
  uint64_t size;
  uint64_t reserved1;
} nn_hgw_probe;

typedef struct nn_hgw_range {
  double lo, hi;
} nn_hgw_range;

typedef struct nn_hgw_lineage {
  uint8_t boot[16];
  char session[32];
  uint64_t first_seq, last_seq, first_stamp_ns, last_stamp_ns, rows;
  uint64_t reserved;
} nn_hgw_lineage;

/* The layout, pinned. A change here is a new format version. */
_Static_assert(sizeof(nn_hgw_header) == NN_HGW_HEADER_SIZE, "header is 4096 bytes");
_Static_assert(offsetof(nn_hgw_header, magic) == 0, "layout");
_Static_assert(offsetof(nn_hgw_header, format_version) == 8, "layout");
_Static_assert(offsetof(nn_hgw_header, header_size) == 12, "layout");
_Static_assert(offsetof(nn_hgw_header, arch_hash) == 16, "layout");
_Static_assert(offsetof(nn_hgw_header, name) == 24, "layout");
_Static_assert(offsetof(nn_hgw_header, tick_period_ns) == 56, "layout");
_Static_assert(offsetof(nn_hgw_header, warmup_ticks) == 64, "layout");
_Static_assert(offsetof(nn_hgw_header, lineage_count) == 68, "layout");
_Static_assert(offsetof(nn_hgw_header, probe_rel_tol) == 72, "layout");
_Static_assert(offsetof(nn_hgw_header, probe_abs_tol) == 76, "layout");
_Static_assert(offsetof(nn_hgw_header, created_ns) == 80, "layout");
_Static_assert(offsetof(nn_hgw_header, trainer_program) == 88, "layout");
_Static_assert(offsetof(nn_hgw_header, trainer_version) == 120, "layout");
_Static_assert(offsetof(nn_hgw_header, trainer_settings_hash) == 152, "layout");
_Static_assert(offsetof(nn_hgw_header, dataset) == 160, "layout");
_Static_assert(offsetof(nn_hgw_header, dataset_hash) == 192, "layout");
_Static_assert(offsetof(nn_hgw_header, rows) == 200, "layout");
_Static_assert(offsetof(nn_hgw_header, steps) == 208, "layout");
_Static_assert(offsetof(nn_hgw_header, parent_hash) == 216, "layout");
_Static_assert(offsetof(nn_hgw_header, newest_row_boot) == 224, "layout");
_Static_assert(offsetof(nn_hgw_header, newest_row_stamp_ns) == 240, "layout");
_Static_assert(offsetof(nn_hgw_header, autopilot_mode) == 248, "layout");
_Static_assert(offsetof(nn_hgw_header, activation_bound) == 252, "layout");
_Static_assert(offsetof(nn_hgw_header, file_size) == 256, "layout");
_Static_assert(offsetof(nn_hgw_header, section_count) == 264, "layout");
_Static_assert(offsetof(nn_hgw_header, section_dir_offset) == 268, "layout");
_Static_assert(offsetof(nn_hgw_header, tensor_data_offset) == 272, "layout");
_Static_assert(offsetof(nn_hgw_header, sessions) == 288, "layout");
_Static_assert(sizeof(nn_hgw_session) == 48, "layout");
_Static_assert(offsetof(nn_hgw_header, ranges) == 672, "layout");
_Static_assert(sizeof(nn_hgw_range_summary) == 40, "layout");
_Static_assert(offsetof(nn_hgw_header, zero) == 992, "layout");
_Static_assert(sizeof(nn_hgw_section) == 32, "layout");
_Static_assert(offsetof(nn_hgw_section, offset) == 16, "layout");
_Static_assert(sizeof(nn_hgw_op) == 192, "layout");
_Static_assert(offsetof(nn_hgw_op, version) == 16, "layout");
_Static_assert(offsetof(nn_hgw_op, inputs) == 32, "layout");
_Static_assert(offsetof(nn_hgw_op, output) == 48, "layout");
_Static_assert(offsetof(nn_hgw_op, tensors) == 56, "layout");
_Static_assert(offsetof(nn_hgw_op, attrs) == 128, "layout");
_Static_assert(sizeof(nn_hgw_tensor) == 80, "layout");
_Static_assert(offsetof(nn_hgw_tensor, dtype) == 32, "layout");
_Static_assert(offsetof(nn_hgw_tensor, shape) == 40, "layout");
_Static_assert(offsetof(nn_hgw_tensor, offset) == 56, "layout");
_Static_assert(offsetof(nn_hgw_tensor, size) == 64, "layout");
_Static_assert(sizeof(nn_hgw_buffer) == 64, "layout");
_Static_assert(offsetof(nn_hgw_buffer, shape) == 40, "layout");
_Static_assert(sizeof(nn_hgw_binding) == 64, "layout");
_Static_assert(offsetof(nn_hgw_binding, field) == 8, "layout");
_Static_assert(sizeof(nn_hgw_probe) == 32, "layout");
_Static_assert(offsetof(nn_hgw_probe, offset) == 8, "layout");
_Static_assert(sizeof(nn_hgw_range) == 16, "layout");
_Static_assert(sizeof(nn_hgw_lineage) == 96, "layout");
_Static_assert(offsetof(nn_hgw_lineage, first_seq) == 48, "layout");

/* A parsed file: pointers into the mapping. Sections absent from the file
 * have a NULL pointer and a zero count. */
typedef struct nn_hgw_file {
  const uint8_t *data;
  size_t size;
  const nn_hgw_header *hdr;
  const nn_hgw_section *dir;
  const nn_hgw_op *ops;
  uint32_t n_ops;
  const nn_hgw_tensor *tensors;
  uint32_t n_tensors;
  const nn_hgw_buffer *buffers;
  uint32_t n_buffers;
  const nn_hgw_binding *inputs;
  uint32_t n_inputs;
  const nn_hgw_binding *outputs;
  uint32_t n_outputs;
  const nn_hgw_probe *probes;
  uint32_t n_probes;
  const nn_hgw_range *ranges;
  uint32_t n_ranges;
  const nn_hgw_lineage *lineage;
  uint32_t n_lineage;
} nn_hgw_file;

/* Structural parse: magic, format version, header size, directory and
 * section bounds, record-size divisibility, alignment, tensor offsets in
 * range. Nothing about the graph's meaning. 0, or -1 with the reason. */
int nn_hgw_parse(const void *data, size_t size, nn_hgw_file *f, char *reason,
                 size_t reason_len);

/* The canonical architecture text (README.md, "The architecture hash")
 * streamed to `sink`, and its FNV-1a 64 hash. */
typedef void (*nn_hgw_sink)(void *ctx, const char *bytes, size_t n);
void nn_hgw_arch_canon(const nn_hgw_file *f, nn_hgw_sink sink, void *ctx);
uint64_t nn_hgw_arch_hash(const nn_hgw_file *f);

/* FNV-1a 64 helpers, exposed for the tests. */
#define NN_FNV1A64_OFFSET 0xcbf29ce484222325ull
#define NN_FNV1A64_PRIME 0x100000001b3ull
uint64_t nn_fnv1a64(uint64_t h, const void *bytes, size_t n);

/* ---- The loader -------------------------------------------------------- */

struct nn_model;

/* What the caller (the policy module) knows and the file must agree with.
 * A NULL pointer or a zero value skips that comparison. */
typedef struct nn_load_req {
  const uint8_t *file_hash;    /* BLAKE2s-256 of the whole file (check 1) */
  uint64_t arch;               /* the policy's Arch (check 2) */
  uint64_t tick_period_ns;     /* the anchor's declared period (check 4) */
  const char *dataset;         /* the policy's Dataset (check 4) */
  uint64_t dataset_hash;       /* its spec hash (check 4) */
  const char *const *input_fields; /* fields the dataset's input parts offer (check 4) */
  size_t n_input_fields;
  const char *const *output_fields; /* fields of the output type (check 4) */
  size_t n_output_fields;
  double step_budget_ns;       /* the largest mean step time allowed (check 7) */
} nn_load_req;

#define NN_REPORT_REASON_LEN 128

typedef struct nn_report {
  int check;        /* 0: loaded; 1..7: the check that failed */
  char name[16];    /* "file hash", "format", "ops", "bindings", "tensors", "probes", "step time" */
  char reason[NN_REPORT_REASON_LEN];
  double step_ns;   /* mean step time measured on the probes (check 7), 0 if not reached */
  uint64_t probe_steps;
  uint64_t arch_hash; /* as computed from the file, once parsed */
} nn_report;

/* Checks 1 to 7 in order on a file in memory (`data` must stay mapped for
 * the model's life; the model reads tensors in place), or on a path, which
 * nn_load maps itself. On success *out is the model and report->check is 0;
 * on failure *out is NULL, nothing is left allocated, and the report names
 * the check and the reason. */
int nn_load_mem(const void *data, size_t size, const nn_load_req *req,
                struct nn_model **out, nn_report *report);
int nn_load(const char *path, const nn_load_req *req, struct nn_model **out,
            nn_report *report);

/* Map a whole file read-only (nn_load does this); unmap what nn_hgw_map
 * returned. */
const void *nn_hgw_map(const char *path, size_t *size, char *reason,
                       size_t reason_len);
void nn_hgw_unmap(const void *data, size_t size);
