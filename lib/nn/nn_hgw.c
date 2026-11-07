// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * The .hgw file: structural parse, canonical architecture text and its
 * hash, mapping, and the loader with checks 1 to 7. See nn_hgw.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "nn_hgw.h"

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "blake2s.h"
#include "nn_graph.h"

static void set_reason(char *r, size_t rl, const char *fmt, ...) {
  va_list ap;
  if (!r || !rl)
    return;
  va_start(ap, fmt);
  vsnprintf(r, rl, fmt, ap);
  va_end(ap);
}

/* ---- FNV-1a 64 --------------------------------------------------------- */

uint64_t nn_fnv1a64(uint64_t h, const void *bytes, size_t n) {
  const uint8_t *p = (const uint8_t *)bytes;
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= p[i];
    h *= NN_FNV1A64_PRIME;
  }
  return h;
}

/* ---- parse ------------------------------------------------------------- */

static bool nul_within(const char *s, size_t n) {
  return memchr(s, 0, n) != NULL;
}

typedef struct section_kind {
  const char *name;
  size_t record;
} section_kind;

static const section_kind kinds[] = {
    {"ops", sizeof(nn_hgw_op)},         {"tensors", sizeof(nn_hgw_tensor)},
    {"buffers", sizeof(nn_hgw_buffer)}, {"inputs", sizeof(nn_hgw_binding)},
    {"outputs", sizeof(nn_hgw_binding)}, {"probes", sizeof(nn_hgw_probe)},
    {"ranges", sizeof(nn_hgw_range)},   {"lineage", sizeof(nn_hgw_lineage)},
};

int nn_hgw_parse(const void *data, size_t size, nn_hgw_file *f, char *r,
                 size_t rl) {
  const uint8_t *p = (const uint8_t *)data;
  const nn_hgw_header *h;
  size_t dir_end;
  uint32_t i, k;
  bool seen[sizeof kinds / sizeof kinds[0]] = {false};

  memset(f, 0, sizeof *f);
  if (size < NN_HGW_HEADER_SIZE) {
    set_reason(r, rl, "file is %zu bytes, shorter than the 4096-byte header", size);
    return -1;
  }
  if (((uintptr_t)p & (NN_HGW_ALIGN - 1)) != 0) {
    set_reason(r, rl, "file is not mapped 64-byte aligned");
    return -1;
  }
  h = (const nn_hgw_header *)p;
  if (memcmp(h->magic, NN_HGW_MAGIC, 8) != 0) {
    set_reason(r, rl, "magic is not HGWEIGHT");
    return -1;
  }
  if (h->format_version != NN_HGW_FORMAT_VERSION) {
    set_reason(r, rl, "format version %u, this runtime reads %u", h->format_version,
               NN_HGW_FORMAT_VERSION);
    return -1;
  }
  if (h->header_size != NN_HGW_HEADER_SIZE || h->section_dir_offset != NN_HGW_HEADER_SIZE) {
    set_reason(r, rl, "header size %u / directory offset %u, expected 4096",
               h->header_size, h->section_dir_offset);
    return -1;
  }
  if (h->file_size != size) {
    set_reason(r, rl, "header says %llu bytes, file has %zu",
               (unsigned long long)h->file_size, size);
    return -1;
  }
  if (!nul_within(h->name, sizeof h->name) || !nul_within(h->dataset, sizeof h->dataset) ||
      !nul_within(h->trainer_program, sizeof h->trainer_program) ||
      !nul_within(h->trainer_version, sizeof h->trainer_version)) {
    set_reason(r, rl, "a header string is not NUL-terminated");
    return -1;
  }
  if (h->lineage_count > NN_HGW_MAX_LINEAGE) {
    set_reason(r, rl, "lineage_count %u > 8", h->lineage_count);
    return -1;
  }
  if (!isfinite(h->probe_rel_tol) || !isfinite(h->probe_abs_tol) ||
      !isfinite(h->activation_bound) || h->probe_rel_tol < 0 || h->probe_abs_tol < 0 ||
      h->activation_bound < 0) {
    set_reason(r, rl, "probe tolerances or activation bound not finite and >= 0");
    return -1;
  }
  dir_end = NN_HGW_HEADER_SIZE + (size_t)h->section_count * sizeof(nn_hgw_section);
  if (h->section_count > 1024 || dir_end > size) {
    set_reason(r, rl, "section directory of %u entries does not fit", h->section_count);
    return -1;
  }
  if (h->tensor_data_offset > size || (h->tensor_data_offset % NN_HGW_ALIGN) != 0) {
    set_reason(r, rl, "tensor data offset %llu out of range or unaligned",
               (unsigned long long)h->tensor_data_offset);
    return -1;
  }
  f->data = p;
  f->size = size;
  f->hdr = h;
  f->dir = (const nn_hgw_section *)(p + NN_HGW_HEADER_SIZE);
  for (i = 0; i < h->section_count; i++) {
    const nn_hgw_section *s = &f->dir[i];
    if (!nul_within(s->name, sizeof s->name)) {
      set_reason(r, rl, "section %u: name not NUL-terminated", i);
      return -1;
    }
    if (s->offset % NN_HGW_ALIGN != 0 || s->offset < dir_end || s->offset > size ||
        s->size > size - s->offset) {
      set_reason(r, rl, "section %u (%s): offset %llu size %llu out of range", i,
                 s->name, (unsigned long long)s->offset, (unsigned long long)s->size);
      return -1;
    }
    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
      if (strcmp(s->name, kinds[k].name) == 0)
        break;
    if (k == sizeof kinds / sizeof kinds[0])
      continue; /* unknown section: skipped */
    if (seen[k]) {
      set_reason(r, rl, "section %s appears twice", s->name);
      return -1;
    }
    seen[k] = true;
    if (s->size % kinds[k].record != 0 || s->size / kinds[k].record > 0xFFFFFFFFu) {
      set_reason(r, rl, "section %s: %llu bytes is not a whole number of %zu-byte records",
                 s->name, (unsigned long long)s->size, kinds[k].record);
      return -1;
    }
    {
      const void *base = p + s->offset;
      uint32_t n = (uint32_t)(s->size / kinds[k].record);
      switch (k) {
      case 0:
        f->ops = base;
        f->n_ops = n;
        break;
      case 1:
        f->tensors = base;
        f->n_tensors = n;
        break;
      case 2:
        f->buffers = base;
        f->n_buffers = n;
        break;
      case 3:
        f->inputs = base;
        f->n_inputs = n;
        break;
      case 4:
        f->outputs = base;
        f->n_outputs = n;
        break;
      case 5:
        f->probes = base;
        f->n_probes = n;
        break;
      case 6:
        f->ranges = base;
        f->n_ranges = n;
        break;
      default:
        f->lineage = base;
        f->n_lineage = n;
        break;
      }
    }
  }
  if (!seen[0] || !seen[2] || !seen[3] || !seen[4]) {
    set_reason(r, rl, "a required section is missing (ops, buffers, inputs, outputs)");
    return -1;
  }
  for (i = 0; i < f->n_ops; i++)
    if (!nul_within(f->ops[i].type, sizeof f->ops[i].type)) {
      set_reason(r, rl, "op %u: type not NUL-terminated", i);
      return -1;
    }
  for (i = 0; i < f->n_tensors; i++) {
    const nn_hgw_tensor *t = &f->tensors[i];
    if (!nul_within(t->name, sizeof t->name)) {
      set_reason(r, rl, "tensor %u: name not NUL-terminated", i);
      return -1;
    }
    if (t->offset < h->tensor_data_offset || t->offset % NN_HGW_ALIGN != 0 ||
        t->offset > size || t->size > size - t->offset) {
      set_reason(r, rl, "tensor %u (%s): data at %llu+%llu out of range or unaligned", i,
                 t->name, (unsigned long long)t->offset, (unsigned long long)t->size);
      return -1;
    }
  }
  for (i = 0; i < f->n_buffers; i++)
    if (!nul_within(f->buffers[i].name, sizeof f->buffers[i].name)) {
      set_reason(r, rl, "buffer %u: name not NUL-terminated", i);
      return -1;
    }
  for (i = 0; i < f->n_inputs; i++)
    if (!nul_within(f->inputs[i].field, sizeof f->inputs[i].field)) {
      set_reason(r, rl, "input binding %u: field not NUL-terminated", i);
      return -1;
    }
  for (i = 0; i < f->n_outputs; i++)
    if (!nul_within(f->outputs[i].field, sizeof f->outputs[i].field)) {
      set_reason(r, rl, "output binding %u: field not NUL-terminated", i);
      return -1;
    }
  for (i = 0; i < f->n_probes; i++) {
    const nn_hgw_probe *pr = &f->probes[i];
    if (pr->offset < h->tensor_data_offset || pr->offset % NN_HGW_ALIGN != 0 ||
        pr->offset > size || pr->size > size - pr->offset || pr->steps == 0) {
      set_reason(r, rl, "probe %u: %u steps at %llu+%llu out of range or unaligned", i,
                 pr->steps, (unsigned long long)pr->offset, (unsigned long long)pr->size);
      return -1;
    }
  }
  for (i = 0; i < f->n_lineage; i++)
    if (!nul_within(f->lineage[i].session, sizeof f->lineage[i].session)) {
      set_reason(r, rl, "lineage %u: session not NUL-terminated", i);
      return -1;
    }
  return 0;
}

/* ---- canonical architecture text and hash ------------------------------ */

static void emit(nn_hgw_sink sink, void *ctx, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof buf)
    n = (int)sizeof buf - 1;
  sink(ctx, buf, (size_t)n);
}

static void emit_dims(nn_hgw_sink sink, void *ctx, uint32_t dtype, uint32_t ndim,
                      const uint32_t *dims) {
  uint32_t i;
  emit(sink, ctx, " %u %u", dtype, ndim);
  for (i = 0; i < ndim && i < NN_MAX_DIMS; i++)
    emit(sink, ctx, " %u", dims[i]);
  emit(sink, ctx, "\n");
}

void nn_hgw_arch_canon(const nn_hgw_file *f, nn_hgw_sink sink, void *ctx) {
  uint32_t i, k;
  emit(sink, ctx, "hgw-arch-1\n");
  emit(sink, ctx, "buffers %u\n", f->n_buffers);
  for (i = 0; i < f->n_buffers; i++) {
    emit(sink, ctx, "buffer %u", i);
    emit_dims(sink, ctx, f->buffers[i].dtype, f->buffers[i].ndim, f->buffers[i].shape);
  }
  emit(sink, ctx, "tensors %u\n", f->n_tensors);
  for (i = 0; i < f->n_tensors; i++) {
    emit(sink, ctx, "tensor %u", i);
    emit_dims(sink, ctx, f->tensors[i].dtype, f->tensors[i].ndim, f->tensors[i].shape);
  }
  emit(sink, ctx, "ops %u\n", f->n_ops);
  for (i = 0; i < f->n_ops; i++) {
    const nn_hgw_op *op = &f->ops[i];
    emit(sink, ctx, "op %u %s %u attrs %u", i, op->type, op->version, op->n_attrs);
    for (k = 0; k < op->n_attrs && k < NN_MAX_ATTRS; k++) {
      uint64_t bits;
      memcpy(&bits, &op->attrs[k], 8);
      emit(sink, ctx, " %016llx", (unsigned long long)bits);
    }
    emit(sink, ctx, " in %u", op->n_inputs);
    for (k = 0; k < op->n_inputs && k < NN_MAX_OP_INPUTS; k++)
      emit(sink, ctx, " %u", op->inputs[k]);
    emit(sink, ctx, " out %u tensors %u", op->output, op->n_tensors);
    for (k = 0; k < op->n_tensors && k < NN_MAX_OP_TENSORS; k++)
      emit(sink, ctx, " %u", op->tensors[k]);
    emit(sink, ctx, "\n");
  }
  emit(sink, ctx, "inputs %u\n", f->n_inputs);
  for (i = 0; i < f->n_inputs; i++)
    emit(sink, ctx, "input %u %u %s\n", f->inputs[i].buffer, f->inputs[i].element,
         f->inputs[i].field);
  emit(sink, ctx, "outputs %u\n", f->n_outputs);
  for (i = 0; i < f->n_outputs; i++)
    emit(sink, ctx, "output %u %u %s\n", f->outputs[i].buffer, f->outputs[i].element,
         f->outputs[i].field);
}

static void fnv_sink(void *ctx, const char *bytes, size_t n) {
  uint64_t *h = (uint64_t *)ctx;
  *h = nn_fnv1a64(*h, bytes, n);
}

uint64_t nn_hgw_arch_hash(const nn_hgw_file *f) {
  uint64_t h = NN_FNV1A64_OFFSET;
  nn_hgw_arch_canon(f, fnv_sink, &h);
  return h;
}

/* ---- mapping ----------------------------------------------------------- */

const void *nn_hgw_map(const char *path, size_t *size, char *r, size_t rl) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  struct stat st;
  void *p;
  if (fd < 0) {
    set_reason(r, rl, "cannot open %s", path);
    return NULL;
  }
  if (fstat(fd, &st) < 0 || st.st_size <= 0) {
    set_reason(r, rl, "cannot stat %s or it is empty", path);
    close(fd);
    return NULL;
  }
  p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    set_reason(r, rl, "cannot map %s", path);
    return NULL;
  }
  *size = (size_t)st.st_size;
  return p;
}

void nn_hgw_unmap(const void *data, size_t size) {
  if (data)
    munmap((void *)data, size);
}

/* ---- the loader -------------------------------------------------------- */

static const char *check_names[] = {"loaded",   "file hash", "format",   "ops",
                                    "bindings", "tensors",   "probes",   "step time"};

static int fail_check(nn_report *rep, int check, const char *fmt, ...) {
  va_list ap;
  rep->check = check;
  snprintf(rep->name, sizeof rep->name, "%s", check_names[check]);
  va_start(ap, fmt);
  vsnprintf(rep->reason, sizeof rep->reason, fmt, ap);
  va_end(ap);
  return -1;
}

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static bool in_list(const char *s, const char *const *list, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    if (strcmp(s, list[i]) == 0)
      return true;
  return false;
}

/* Every element of each bound buffer bound exactly once, and every field in
 * the caller's list when one is given. */
static int check_bindings(const nn_model *m, const nn_hgw_binding *b, uint32_t n,
                          const char *const *fields, size_t n_fields,
                          const char *what, nn_report *rep) {
  uint32_t i, j;
  for (i = 0; i < n; i++) {
    const nn_buffer *buf = &m->buffers[b[i].buffer];
    if (b[i].element >= nn_tdesc_numel(&buf->d))
      return fail_check(rep, 4, "%s binding %u (%s): element %u of a %zu-element buffer",
                        what, i, b[i].field, b[i].element, nn_tdesc_numel(&buf->d));
    for (j = 0; j < i; j++)
      if (b[j].buffer == b[i].buffer && b[j].element == b[i].element)
        return fail_check(rep, 4, "%s binding %u (%s): element bound twice", what, i,
                          b[i].field);
    if (fields && !in_list(b[i].field, fields, n_fields))
      return fail_check(rep, 4, "%s binding %u: no field %s", what, i, b[i].field);
  }
  return 0;
}

static int count_bound(const nn_hgw_binding *b, uint32_t n, uint32_t buffer) {
  uint32_t i, c = 0;
  for (i = 0; i < n; i++)
    if (b[i].buffer == buffer)
      c++;
  return (int)c;
}

static bool tensor_finite(const nn_hgw_file *f, const nn_hgw_tensor *t) {
  size_t i, n;
  const uint8_t *p = f->data + t->offset;
  if (t->dtype == NN_F32) {
    n = t->size / 4;
    for (i = 0; i < n; i++) {
      float v;
      memcpy(&v, p + 4 * i, 4);
      if (!isfinite(v))
        return false;
    }
  } else if (t->dtype == NN_F64) {
    n = t->size / 8;
    for (i = 0; i < n; i++) {
      double v;
      memcpy(&v, p + 8 * i, 8);
      if (!isfinite(v))
        return false;
    }
  }
  return true;
}

/* Writes value v as dtype into p (clamped and rounded for integers). */
static void write_elem(uint32_t dtype, uint8_t *p, double v) {
  switch (dtype) {
  case NN_F32: {
    float x = (float)v;
    memcpy(p, &x, 4);
    break;
  }
  case NN_F64:
    memcpy(p, &v, 8);
    break;
  case NN_U8: {
    double c = v < 0 ? 0 : v > 255 ? 255 : v;
    p[0] = (uint8_t)llround(c);
    break;
  }
  case NN_I8: {
    double c = v < -128 ? -128 : v > 127 ? 127 : v;
    int8_t x = (int8_t)llround(c);
    memcpy(p, &x, 1);
    break;
  }
  case NN_U16: {
    double c = v < 0 ? 0 : v > 65535 ? 65535 : v;
    uint16_t x = (uint16_t)llround(c);
    memcpy(p, &x, 2);
    break;
  }
  case NN_I16: {
    double c = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
    int16_t x = (int16_t)llround(c);
    memcpy(p, &x, 2);
    break;
  }
  case NN_U32: {
    double c = v < 0 ? 0 : v > 4294967295.0 ? 4294967295.0 : v;
    uint32_t x = (uint32_t)llround(c);
    memcpy(p, &x, 4);
    break;
  }
  case NN_I32: {
    double c = v < -2147483648.0 ? -2147483648.0 : v > 2147483647.0 ? 2147483647.0 : v;
    int32_t x = (int32_t)llround(c);
    memcpy(p, &x, 4);
    break;
  }
  default:
    break;
  }
}

/* Pointers to the per-input blocks and per-output blocks of one probe
 * record (or of the staging area), following the stride layout. */
static void split_record(const nn_model *m, uint8_t *rec, const void **in,
                         float **out) {
  size_t off = 0;
  uint32_t i;
  for (i = 0; i < m->n_inputs; i++) {
    in[i] = rec + off;
    off += (nn_input_bytes(m, i) + 7) / 8 * 8;
  }
  for (i = 0; i < m->n_outputs; i++) {
    out[i] = (float *)(rec + off);
    off += (nn_output_count(m, i) * 4 + 7) / 8 * 8;
  }
}

#define MAX_IO 64 /* model inputs or outputs the loader handles */

static int run_probes(nn_model *m, const nn_hgw_file *f, nn_report *rep) {
  const nn_hgw_header *h = f->hdr;
  size_t stride = nn_probe_stride(m);
  const void *in[MAX_IO];
  float *out[MAX_IO];
  const void *dummy_in[MAX_IO];
  float *exp_out[MAX_IO];
  uint32_t p, s, j;
  double t0, total_ns = 0;
  uint64_t total_steps = 0;
  float bound = h->activation_bound;

  if (m->n_inputs > MAX_IO || m->n_outputs > MAX_IO)
    return fail_check(rep, 6, "more than %d model inputs or outputs", MAX_IO);
  if (f->n_probes == 0)
    return fail_check(rep, 6, "the file has no probes");
  for (p = 0; p < f->n_probes; p++) {
    const nn_hgw_probe *pr = &f->probes[p];
    if (pr->size != (uint64_t)pr->steps * stride)
      return fail_check(rep, 6, "probe %u: %llu bytes for %u steps of %zu", p,
                        (unsigned long long)pr->size, pr->steps, stride);
  }
  /* The staging area receives this step's outputs; the inputs are read in
   * place from the file (every block is 8-byte aligned). */
  split_record(m, (uint8_t *)m->staging, dummy_in, out);
  for (p = 0; p < f->n_probes; p++) {
    const nn_hgw_probe *pr = &f->probes[p];
    nn_reset(m);
    t0 = now_ns();
    for (s = 0; s < pr->steps; s++) {
      uint8_t *rec = (uint8_t *)(f->data + pr->offset + (size_t)s * stride);
      uint32_t bad = 0;
      int rc;
      split_record(m, rec, in, exp_out);
      rc = nn_step_bounded(m, in, out, bound, &bad);
      if (rc == NN_ERR_BOUND)
        return fail_check(rep, 6, "probe %u step %u: op %u (%s) exceeds activation bound %g",
                          p, s, bad, m->ops[bad].def->type, (double)bound);
      if (rc != NN_OK)
        return fail_check(rep, 6, "probe %u step %u: non-finite value", p, s);
      for (j = 0; j < m->n_outputs; j++) {
        size_t n = nn_output_count(m, j), e;
        for (e = 0; e < n; e++) {
          float got = out[j][e], want = exp_out[j][e];
          if (!(fabsf(got - want) <= h->probe_abs_tol + h->probe_rel_tol * fabsf(want)))
            return fail_check(rep, 6, "probe %u step %u output %u[%zu]: got %g, expected %g",
                              p, s, j, e, (double)got, (double)want);
        }
      }
    }
    total_ns += now_ns() - t0;
    total_steps += pr->steps;
  }
  rep->step_ns = total_ns / (double)total_steps;
  rep->probe_steps = total_steps;

  /* The ranges section, when present: constant inputs at the corners and
   * the middle of the ranges, every element in turn at its extreme with the
   * rest in the middle, each held for 16 steps from zero state. */
  if (f->n_ranges > 0) {
    uint32_t n_samples = 3 + 2 * f->n_inputs, smp;
    uint8_t *rec = (uint8_t *)m->staging;
    if (f->n_ranges != f->n_inputs)
      return fail_check(rep, 6, "ranges section has %u entries for %u input bindings",
                        f->n_ranges, f->n_inputs);
    for (smp = 0; smp < n_samples; smp++) {
      uint32_t b;
      size_t off_in[MAX_IO];
      size_t off = 0;
      for (j = 0; j < m->n_inputs; j++) {
        off_in[j] = off;
        off += (nn_input_bytes(m, j) + 7) / 8 * 8;
      }
      memset(rec, 0, stride);
      for (b = 0; b < f->n_inputs; b++) {
        const nn_hgw_range *rg = &f->ranges[b];
        double mid = 0.5 * (rg->lo + rg->hi), v;
        uint32_t mi;
        const nn_tdesc *d;
        if (smp == 0)
          v = rg->lo;
        else if (smp == 1)
          v = rg->hi;
        else if (smp == 2)
          v = mid;
        else if ((smp - 3) / 2 == b)
          v = (smp - 3) % 2 == 0 ? rg->lo : rg->hi;
        else
          v = mid;
        for (mi = 0; mi < m->n_inputs; mi++)
          if (m->input_buf[mi] == f->inputs[b].buffer)
            break;
        d = nn_input_desc(m, mi);
        write_elem(d->dtype, rec + off_in[mi] + (size_t)f->inputs[b].element * nn_dtype_size(d->dtype), v);
      }
      split_record(m, rec, in, out);
      nn_reset(m);
      for (s = 0; s < 16; s++) {
        uint32_t bad = 0;
        int rc = nn_step_bounded(m, in, out, bound, &bad);
        if (rc == NN_ERR_BOUND)
          return fail_check(rep, 6, "range sample %u step %u: op %u (%s) exceeds activation bound %g",
                            smp, s, bad, m->ops[bad].def->type, (double)bound);
        if (rc != NN_OK)
          return fail_check(rep, 6, "range sample %u step %u: non-finite value", smp, s);
      }
    }
  }
  nn_reset(m);
  return 0;
}

static int load_impl(const void *data, size_t size, bool owns_map,
                     const nn_load_req *req, nn_model **out, nn_report *rep) {
  nn_hgw_file f;
  nn_model *m = NULL;
  char why[NN_REPORT_REASON_LEN];
  uint32_t i;
  static const nn_load_req none = {0};

  *out = NULL;
  memset(rep, 0, sizeof *rep);
  if (!req)
    req = &none;

  /* 1. the file's hash */
  if (req->file_hash) {
    uint8_t digest[NN_BLAKE2S_OUTBYTES];
    nn_blake2s(digest, sizeof digest, data, size, NULL, 0);
    if (memcmp(digest, req->file_hash, sizeof digest) != 0)
      return fail_check(rep, 1, "BLAKE2s-256 of the file differs from the request's");
  }
  /* 2. the format version and the architecture hash */
  if (nn_hgw_parse(data, size, &f, why, sizeof why) < 0)
    return fail_check(rep, 2, "%s", why);
  rep->arch_hash = nn_hgw_arch_hash(&f);
  if (f.hdr->arch_hash != rep->arch_hash)
    return fail_check(rep, 2, "header says architecture %016llx, the graph hashes to %016llx",
                      (unsigned long long)f.hdr->arch_hash,
                      (unsigned long long)rep->arch_hash);
  if (req->arch && req->arch != rep->arch_hash)
    return fail_check(rep, 2, "architecture %016llx, the policy's Arch is %016llx",
                      (unsigned long long)rep->arch_hash, (unsigned long long)req->arch);
  /* 3. every op known, every check passing */
  if (nn_graph_build(&f, data, size, owns_map, &m, why, sizeof why) < 0)
    return fail_check(rep, 3, "%s", why);
  m->owns_map = false; /* the caller unmaps until the load succeeds */
  /* 4. dataset, tick period and bindings */
  if (req->tick_period_ns && req->tick_period_ns != f.hdr->tick_period_ns) {
    fail_check(rep, 4, "trained at a %llu ns tick, the anchor's is %llu ns",
               (unsigned long long)f.hdr->tick_period_ns,
               (unsigned long long)req->tick_period_ns);
    goto fail;
  }
  if (req->dataset && strcmp(req->dataset, f.hdr->dataset) != 0) {
    fail_check(rep, 4, "trained on dataset %s, the policy's is %s", f.hdr->dataset,
               req->dataset);
    goto fail;
  }
  if (req->dataset_hash && req->dataset_hash != f.hdr->dataset_hash) {
    fail_check(rep, 4, "dataset spec hash %016llx, the policy's is %016llx",
               (unsigned long long)f.hdr->dataset_hash,
               (unsigned long long)req->dataset_hash);
    goto fail;
  }
  if (check_bindings(m, f.inputs, f.n_inputs, req->input_fields, req->n_input_fields,
                     "input", rep) < 0)
    goto fail;
  if (check_bindings(m, f.outputs, f.n_outputs, req->output_fields, req->n_output_fields,
                     "output", rep) < 0)
    goto fail;
  for (i = 0; i < m->n_inputs; i++) {
    const nn_buffer *b = &m->buffers[m->input_buf[i]];
    if ((size_t)count_bound(f.inputs, f.n_inputs, m->input_buf[i]) != nn_tdesc_numel(&b->d)) {
      fail_check(rep, 4, "input buffer %s: %d of %zu elements bound", b->name,
                 count_bound(f.inputs, f.n_inputs, m->input_buf[i]), nn_tdesc_numel(&b->d));
      goto fail;
    }
  }
  /* 5. every tensor value finite */
  for (i = 0; i < f.n_tensors; i++)
    if (!tensor_finite(&f, &f.tensors[i])) {
      fail_check(rep, 5, "tensor %u (%s) has a non-finite value", i, f.tensors[i].name);
      goto fail;
    }
  /* 6. the probes from zero state, and the activation bound */
  if (run_probes(m, &f, rep) < 0)
    goto fail;
  /* 7. the step time */
  if (req->step_budget_ns > 0 && rep->step_ns > req->step_budget_ns) {
    fail_check(rep, 7, "mean step %.0f ns over the budget of %.0f ns", rep->step_ns,
               req->step_budget_ns);
    goto fail;
  }
  m->owns_map = owns_map;
  m->steps = 0;
  m->nonfinite_count = 0;
  rep->check = 0;
  snprintf(rep->name, sizeof rep->name, "%s", check_names[0]);
  *out = m;
  return 0;
fail:
  nn_free(m);
  return -1;
}

int nn_load_mem(const void *data, size_t size, const nn_load_req *req,
                nn_model **out, nn_report *rep) {
  return load_impl(data, size, false, req, out, rep);
}

int nn_load(const char *path, const nn_load_req *req, nn_model **out,
            nn_report *rep) {
  size_t size = 0;
  char why[NN_REPORT_REASON_LEN];
  const void *data;
  int rc;
  *out = NULL;
  memset(rep, 0, sizeof *rep);
  data = nn_hgw_map(path, &size, why, sizeof why);
  if (!data)
    return fail_check(rep, 2, "%s", why);
  rc = load_impl(data, size, true, req, out, rep);
  if (rc < 0)
    nn_hgw_unmap(data, size);
  return rc;
}
