// SPDX-License-Identifier: LGPL-3.0-or-later
/* The model graph: build from a parsed file, step, reset. See nn_graph.h. */
#include "nn_graph.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN 64u

static size_t round_up(size_t n, size_t a) { return (n + a - 1) / a * a; }
static size_t pad8(size_t n) { return round_up(n, 8); }

#include <stdarg.h>
static void set_reason(char *r, size_t rl, const char *fmt, ...) {
  va_list ap;
  if (!r || !rl)
    return;
  va_start(ap, fmt);
  vsnprintf(r, rl, fmt, ap);
  va_end(ap);
}

static void desc_from_buffer(const nn_hgw_buffer *b, nn_tdesc *d) {
  memset(d, 0, sizeof *d);
  d->dtype = b->dtype;
  d->ndim = b->ndim;
  memcpy(d->dims, b->shape, sizeof d->dims);
}

static void desc_from_tensor(const nn_hgw_tensor *t, nn_tdesc *d) {
  memset(d, 0, sizeof *d);
  d->dtype = t->dtype;
  d->ndim = t->ndim;
  memcpy(d->dims, t->shape, sizeof d->dims);
}

static bool desc_valid(const nn_tdesc *d) {
  uint32_t i;
  if (nn_dtype_size(d->dtype) == 0 || d->ndim == 0 || d->ndim > NN_MAX_DIMS)
    return false;
  for (i = 0; i < d->ndim; i++)
    if (d->dims[i] == 0)
      return false;
  for (; i < NN_MAX_DIMS; i++)
    if (d->dims[i] != 0)
      return false;
  return true;
}

/* The signature of op `i` from the file records, with validity checks on
 * the indices it uses. */
typedef struct op_sig_storage {
  nn_attrs attrs;
  nn_tdesc in[NN_MAX_OP_INPUTS];
  nn_tdesc t[NN_MAX_OP_TENSORS];
  nn_op_sig sig;
} op_sig_storage;

static int build_sig(const nn_hgw_file *f, uint32_t i, op_sig_storage *st,
                     char *r, size_t rl) {
  const nn_hgw_op *op = &f->ops[i];
  uint32_t k;
  if (op->n_attrs > NN_MAX_ATTRS || op->n_inputs > NN_MAX_OP_INPUTS ||
      op->n_tensors > NN_MAX_OP_TENSORS) {
    set_reason(r, rl, "op %u: record counts out of range", i);
    return -1;
  }
  memset(st, 0, sizeof *st);
  st->attrs.n = op->n_attrs;
  memcpy(st->attrs.v, op->attrs, sizeof(double) * op->n_attrs);
  for (k = 0; k < op->n_inputs; k++) {
    if (op->inputs[k] >= f->n_buffers) {
      set_reason(r, rl, "op %u: input %u names buffer %u of %u", i, k,
                 op->inputs[k], f->n_buffers);
      return -1;
    }
    desc_from_buffer(&f->buffers[op->inputs[k]], &st->in[k]);
  }
  for (k = 0; k < op->n_tensors; k++) {
    const nn_hgw_tensor *t;
    if (op->tensors[k] >= f->n_tensors) {
      set_reason(r, rl, "op %u: tensor %u names tensor %u of %u", i, k,
                 op->tensors[k], f->n_tensors);
      return -1;
    }
    t = &f->tensors[op->tensors[k]];
    desc_from_tensor(t, &st->t[k]);
    if (!desc_valid(&st->t[k]) || nn_tdesc_bytes(&st->t[k]) != t->size) {
      set_reason(r, rl, "tensor %u (%.*s): shape, type and size disagree",
                 op->tensors[k], (int)NN_HGW_NAME_LEN, t->name);
      return -1;
    }
  }
  st->sig.attrs = &st->attrs;
  st->sig.n_inputs = op->n_inputs;
  st->sig.inputs = st->in;
  st->sig.n_tensors = op->n_tensors;
  st->sig.tensors = st->t;
  return 0;
}

static const nn_op_def *find_def(const nn_hgw_op *op, char *r, size_t rl) {
  char type[sizeof op->type + 1];
  const nn_op_def *def;
  memcpy(type, op->type, sizeof op->type);
  type[sizeof op->type] = 0;
  def = nn_op_find(type, op->version);
  if (!def)
    set_reason(r, rl, "op %s v%u not supported", type, op->version);
  return def;
}

/* Distinct buffers of a binding list in order of first appearance; returns
 * the count, or writes into `out` when non-NULL. */
static uint32_t distinct_buffers(const nn_hgw_binding *b, uint32_t n,
                                 uint32_t *out) {
  uint32_t count = 0, i, j;
  for (i = 0; i < n; i++) {
    for (j = 0; j < i; j++)
      if (b[j].buffer == b[i].buffer)
        break;
    if (j == i) {
      if (out)
        out[count] = b[i].buffer;
      count++;
    }
  }
  return count;
}

int nn_graph_build(const nn_hgw_file *f, const void *map, size_t map_size,
                   bool owns_map, nn_model **out, char *r, size_t rl) {
  uint32_t i, k, n_in, n_out;
  size_t total, buffers_off, ops_off, inbuf_off, outbuf_off, data_off, state_off,
      scratch_off, staging_off, scratch_max = 0, state_total = 0, staging = 0;
  uint8_t *base;
  nn_model *m;
  bool *written = NULL;
  int rc = -1;

  *out = NULL;
  if (f->n_buffers == 0 || f->n_ops == 0) {
    set_reason(r, rl, "the file has %u buffers and %u ops", f->n_buffers, f->n_ops);
    return -1;
  }
  for (i = 0; i < f->n_buffers; i++) {
    nn_tdesc d;
    desc_from_buffer(&f->buffers[i], &d);
    if (!desc_valid(&d)) {
      set_reason(r, rl, "buffer %u (%.*s): invalid type or shape", i,
                 (int)NN_HGW_NAME_LEN, f->buffers[i].name);
      return -1;
    }
  }
  for (i = 0; i < f->n_inputs; i++)
    if (f->inputs[i].buffer >= f->n_buffers) {
      set_reason(r, rl, "input binding %u names buffer %u of %u", i,
                 f->inputs[i].buffer, f->n_buffers);
      return -1;
    }
  for (i = 0; i < f->n_outputs; i++)
    if (f->outputs[i].buffer >= f->n_buffers) {
      set_reason(r, rl, "output binding %u names buffer %u of %u", i,
                 f->outputs[i].buffer, f->n_buffers);
      return -1;
    }
  n_in = distinct_buffers(f->inputs, f->n_inputs, NULL);
  n_out = distinct_buffers(f->outputs, f->n_outputs, NULL);
  if (n_in == 0 || n_out == 0) {
    set_reason(r, rl, "the model binds %u input and %u output buffers", n_in, n_out);
    return -1;
  }

  /* Pass 1: order, ops known, checks, sizes. `written` is the one
   * temporary; it is freed before we return. */
  written = calloc(f->n_buffers, sizeof *written);
  if (!written) {
    set_reason(r, rl, "out of memory");
    return -1;
  }
  for (i = 0; i < f->n_inputs; i++)
    written[f->inputs[i].buffer] = true;
  for (i = 0; i < f->n_ops; i++) {
    op_sig_storage st;
    const nn_op_def *def;
    nn_tdesc outd, bufd;
    const nn_hgw_op *op = &f->ops[i];
    char why[NN_REASON_LEN];
    if (!(def = find_def(op, r, rl)))
      goto done;
    if (build_sig(f, i, &st, r, rl) < 0)
      goto done;
    for (k = 0; k < op->n_inputs; k++)
      if (!written[op->inputs[k]]) {
        set_reason(r, rl, "op %u (%s): reads buffer %u before it is written", i,
                   def->type, op->inputs[k]);
        goto done;
      }
    if (op->output >= f->n_buffers) {
      set_reason(r, rl, "op %u (%s): output names buffer %u of %u", i, def->type,
                 op->output, f->n_buffers);
      goto done;
    }
    if (written[op->output]) {
      set_reason(r, rl, "op %u (%s): buffer %u is already written", i, def->type,
                 op->output);
      goto done;
    }
    why[0] = 0;
    if (def->check(&st.sig, &outd, why, sizeof why) < 0) {
      set_reason(r, rl, "op %u (%s v%u): %s", i, def->type, def->version, why);
      goto done;
    }
    desc_from_buffer(&f->buffers[op->output], &bufd);
    if (!nn_tdesc_equal(&outd, &bufd)) {
      set_reason(r, rl, "op %u (%s): output shape differs from buffer %u (%.*s)", i,
                 def->type, op->output, (int)NN_HGW_NAME_LEN,
                 f->buffers[op->output].name);
      goto done;
    }
    written[op->output] = true;
    if (!nn_op_select_kernel(def)) {
      set_reason(r, rl, "op %u (%s): no kernel for this processor", i, def->type);
      goto done;
    }
    state_total += round_up(def->state_bytes(&st.sig), ARENA_ALIGN);
    if (def->scratch_bytes(&st.sig) > scratch_max)
      scratch_max = def->scratch_bytes(&st.sig);
  }
  for (i = 0; i < f->n_buffers; i++)
    if (!written[i]) {
      set_reason(r, rl, "buffer %u (%.*s) is never written", i, (int)NN_HGW_NAME_LEN,
                 f->buffers[i].name);
      goto done;
    }

  /* Layout of the one arena. */
  total = round_up(sizeof(nn_model), ARENA_ALIGN);
  buffers_off = total;
  total += round_up(sizeof(nn_buffer) * f->n_buffers, ARENA_ALIGN);
  ops_off = total;
  total += round_up(sizeof(nn_op_inst) * f->n_ops, ARENA_ALIGN);
  inbuf_off = total;
  total += round_up(sizeof(uint32_t) * n_in, ARENA_ALIGN);
  outbuf_off = total;
  total += round_up(sizeof(uint32_t) * n_out, ARENA_ALIGN);
  data_off = total;
  for (i = 0; i < f->n_buffers; i++) {
    nn_tdesc d;
    desc_from_buffer(&f->buffers[i], &d);
    total += round_up(nn_tdesc_bytes(&d), ARENA_ALIGN);
  }
  state_off = total;
  total += state_total;
  scratch_off = total;
  total += round_up(scratch_max, ARENA_ALIGN);
  staging_off = total;
  for (i = 0; i < f->n_inputs; i++) {
    uint32_t j;
    for (j = 0; j < i; j++)
      if (f->inputs[j].buffer == f->inputs[i].buffer)
        break;
    if (j == i) {
      nn_tdesc d;
      desc_from_buffer(&f->buffers[f->inputs[i].buffer], &d);
      staging += pad8(nn_tdesc_bytes(&d));
    }
  }
  /* ... and the outputs: the staging area holds one whole probe record */
  for (i = 0; i < f->n_outputs; i++) {
    uint32_t j;
    for (j = 0; j < i; j++)
      if (f->outputs[j].buffer == f->outputs[i].buffer)
        break;
    if (j == i) {
      nn_tdesc d;
      desc_from_buffer(&f->buffers[f->outputs[i].buffer], &d);
      staging += pad8(nn_tdesc_numel(&d) * sizeof(float));
    }
  }
  total += round_up(staging, ARENA_ALIGN);
  total = round_up(total, ARENA_ALIGN);

  base = aligned_alloc(ARENA_ALIGN, total);
  if (!base) {
    set_reason(r, rl, "out of memory for a %zu-byte arena", total);
    goto done;
  }
  memset(base, 0, total);
  m = (nn_model *)base;
  m->arena = base;
  m->arena_bytes = total;
  m->map = map;
  m->map_size = map_size;
  m->owns_map = owns_map;
  m->file = *f;
  m->n_buffers = f->n_buffers;
  m->buffers = (nn_buffer *)(base + buffers_off);
  m->n_ops = f->n_ops;
  m->ops = (nn_op_inst *)(base + ops_off);
  m->n_inputs = n_in;
  m->input_buf = (uint32_t *)(base + inbuf_off);
  m->n_outputs = n_out;
  m->output_buf = (uint32_t *)(base + outbuf_off);
  m->scratch = base + scratch_off;
  m->scratch_bytes = scratch_max;
  m->staging = base + staging_off;
  m->staging_bytes = staging;
  m->total_state_bytes = state_total;
  distinct_buffers(f->inputs, f->n_inputs, m->input_buf);
  distinct_buffers(f->outputs, f->n_outputs, m->output_buf);

  {
    size_t off = data_off;
    for (i = 0; i < f->n_buffers; i++) {
      desc_from_buffer(&f->buffers[i], &m->buffers[i].d);
      m->buffers[i].data = base + off;
      m->buffers[i].name = f->buffers[i].name;
      off += round_up(nn_tdesc_bytes(&m->buffers[i].d), ARENA_ALIGN);
    }
  }
  {
    size_t off = state_off;
    for (i = 0; i < f->n_ops; i++) {
      op_sig_storage st;
      nn_op_inst *in = &m->ops[i];
      const nn_hgw_op *op = &f->ops[i];
      in->def = find_def(op, NULL, 0);
      in->kernel = nn_op_select_kernel(in->def);
      build_sig(f, i, &st, NULL, 0);
      in->attrs = st.attrs;
      in->n_inputs = op->n_inputs;
      for (k = 0; k < op->n_inputs; k++) {
        in->in_buf[k] = op->inputs[k];
        in->in_ptr[k] = m->buffers[op->inputs[k]].data;
        in->in_desc[k] = m->buffers[op->inputs[k]].d;
      }
      in->out_buf = op->output;
      in->out_ptr = (float *)m->buffers[op->output].data;
      in->out_desc = m->buffers[op->output].d;
      in->n_tensors = op->n_tensors;
      for (k = 0; k < op->n_tensors; k++) {
        in->tensors[k].d = st.t[k];
        in->tensors[k].data = f->data + f->tensors[op->tensors[k]].offset;
      }
      in->state_bytes = in->def->state_bytes(&st.sig);
      in->state = in->state_bytes ? base + off : NULL;
      off += round_up(in->state_bytes, ARENA_ALIGN);
    }
  }
  nn_reset(m);
  *out = m;
  rc = 0;
done:
  free(written);
  return rc;
}

static bool all_finite(const float *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++)
    if (!isfinite(v[i]))
      return false;
  return true;
}

static bool all_bounded(const float *v, size_t n, float bound) {
  size_t i;
  for (i = 0; i < n; i++)
    if (!(fabsf(v[i]) <= bound))
      return false;
  return true;
}

int nn_step_bounded(nn_model *m, const void *const *inputs,
                    float *const *outputs, float bound, uint32_t *bad_op) {
  uint32_t i;
  for (i = 0; i < m->n_inputs; i++) {
    nn_buffer *b = &m->buffers[m->input_buf[i]];
    memcpy(b->data, inputs[i], nn_tdesc_bytes(&b->d));
  }
  for (i = 0; i < m->n_ops; i++) {
    nn_op_inst *op = &m->ops[i];
    nn_op_call c;
    c.attrs = &op->attrs;
    c.tensors = op->tensors;
    c.inputs = op->in_ptr;
    c.input_desc = op->in_desc;
    c.output = op->out_ptr;
    c.output_desc = &op->out_desc;
    c.state = op->state;
    c.scratch = m->scratch;
    op->kernel->run(&c);
    if (bound > 0.0f &&
        (!all_bounded(op->out_ptr, nn_tdesc_numel(&op->out_desc), bound) ||
         (op->state_bytes &&
          !all_bounded((const float *)op->state, op->state_bytes / sizeof(float),
                       bound)))) {
      if (bad_op)
        *bad_op = i;
      nn_reset(m);
      return NN_ERR_BOUND;
    }
  }
  m->steps++;
  for (i = 0; i < m->n_outputs; i++) {
    const nn_buffer *b = &m->buffers[m->output_buf[i]];
    if (!all_finite((const float *)b->data, nn_tdesc_numel(&b->d)))
      goto nonfinite;
  }
  for (i = 0; i < m->n_ops; i++)
    if (m->ops[i].state_bytes &&
        !all_finite((const float *)m->ops[i].state,
                    m->ops[i].state_bytes / sizeof(float)))
      goto nonfinite;
  for (i = 0; i < m->n_outputs; i++) {
    const nn_buffer *b = &m->buffers[m->output_buf[i]];
    memcpy(outputs[i], b->data, nn_tdesc_bytes(&b->d));
  }
  return NN_OK;
nonfinite:
  m->nonfinite_count++;
  nn_reset(m);
  return NN_ERR_NONFINITE;
}

int nn_step(nn_model *m, const void *const *inputs, float *const *outputs) {
  return nn_step_bounded(m, inputs, outputs, 0.0f, NULL);
}

void nn_reset(nn_model *m) {
  uint32_t i;
  for (i = 0; i < m->n_ops; i++)
    if (m->ops[i].state_bytes)
      m->ops[i].def->reset(m->ops[i].state, m->ops[i].state_bytes);
}

void nn_free(nn_model *m) {
  if (!m)
    return;
  if (m->owns_map && m->map)
    nn_hgw_unmap(m->map, m->map_size);
  free(m->arena);
}

const nn_tdesc *nn_input_desc(const nn_model *m, uint32_t i) {
  return i < m->n_inputs ? &m->buffers[m->input_buf[i]].d : NULL;
}

size_t nn_input_bytes(const nn_model *m, uint32_t i) {
  const nn_tdesc *d = nn_input_desc(m, i);
  return d ? nn_tdesc_bytes(d) : 0;
}

const nn_tdesc *nn_output_desc(const nn_model *m, uint32_t j) {
  return j < m->n_outputs ? &m->buffers[m->output_buf[j]].d : NULL;
}

size_t nn_output_count(const nn_model *m, uint32_t j) {
  const nn_tdesc *d = nn_output_desc(m, j);
  return d ? nn_tdesc_numel(d) : 0;
}

const void *nn_op_state(const nn_model *m, uint32_t op, size_t *bytes) {
  if (op >= m->n_ops || m->ops[op].state_bytes == 0) {
    if (bytes)
      *bytes = 0;
    return NULL;
  }
  if (bytes)
    *bytes = m->ops[op].state_bytes;
  return m->ops[op].state;
}

size_t nn_probe_stride(const nn_model *m) {
  size_t s = 0;
  uint32_t i;
  for (i = 0; i < m->n_inputs; i++)
    s += pad8(nn_input_bytes(m, i));
  for (i = 0; i < m->n_outputs; i++)
    s += pad8(nn_output_count(m, i) * sizeof(float));
  return s;
}
