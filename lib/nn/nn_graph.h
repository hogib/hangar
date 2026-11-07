// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
/*
 * A model: a graph of ops over named buffers in a fixed order, built from a
 * parsed .hgw file (nn_hgw.h) with every op's check passing, one arena
 * holding the buffers, the recurrent state and the scratch, nothing
 * allocated after that. nn_step runs the ops in order and checks every
 * output and the recurrent state for finite values: a non-finite value
 * resets the state, leaves the outputs untouched and returns
 * NN_ERR_NONFINITE.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn_hgw.h"
#include "nn_ops.h"

enum {
  NN_OK = 0,
  NN_ERR_NONFINITE = -1, /* an output or state value was not finite */
  NN_ERR_BOUND = -2,     /* an intermediate value exceeded the activation bound */
  NN_ERR_INVALID = -3,
};

typedef struct nn_buffer {
  nn_tdesc d;
  void *data;
  const char *name; /* points into the mapped file */
} nn_buffer;

typedef struct nn_op_inst {
  const nn_op_def *def;
  const nn_kernel *kernel;
  nn_attrs attrs;
  uint32_t n_inputs;
  uint32_t in_buf[NN_MAX_OP_INPUTS];
  uint32_t out_buf;
  uint32_t n_tensors;
  nn_tensor tensors[NN_MAX_OP_TENSORS];
  /* resolved at build: the call's pointers and descriptors */
  const void *in_ptr[NN_MAX_OP_INPUTS];
  nn_tdesc in_desc[NN_MAX_OP_INPUTS];
  float *out_ptr;
  nn_tdesc out_desc;
  void *state;
  size_t state_bytes;
} nn_op_inst;

typedef struct nn_model {
  const void *map; /* the file, mapped or caller-owned */
  size_t map_size;
  bool owns_map;
  nn_hgw_file file;
  uint32_t n_buffers;
  nn_buffer *buffers;
  uint32_t n_ops;
  nn_op_inst *ops;
  uint32_t n_inputs; /* distinct input buffers, in order of first binding */
  uint32_t *input_buf;
  uint32_t n_outputs;
  uint32_t *output_buf;
  void *scratch;
  size_t scratch_bytes;
  void *staging; /* room for one set of inputs (the loader's range samples) */
  size_t staging_bytes;
  size_t total_state_bytes;
  uint64_t nonfinite_count; /* ticks that produced a non-finite value */
  uint64_t steps;
  void *arena; /* the one allocation; the model struct sits at its start */
  size_t arena_bytes;
} nn_model;

/* Builds the model from a parsed file: every op known (else
 * "op <type> v<version> not supported"), every check passing, buffer
 * indices and order valid (each buffer written once, read only after it is
 * written or bound as an input). One allocation. 0, or -1 with reason. */
int nn_graph_build(const nn_hgw_file *f, const void *map, size_t map_size,
                   bool owns_map, nn_model **out, char *reason,
                   size_t reason_len);

/* One tick: inputs[i] points at nn_input_bytes(m, i) bytes for model input
 * i, outputs[j] receives nn_output_count(m, j) floats for model output j.
 * NN_OK, or NN_ERR_NONFINITE (state reset, outputs not written). */
int nn_step(nn_model *m, const void *const *inputs, float *const *outputs);

/* nn_step with the loader's activation bound (> 0) applied to every op's
 * output and to the state after the step; *bad_op (may be NULL) gets the
 * offending op's index on NN_ERR_BOUND. bound <= 0 is nn_step. */
int nn_step_bounded(nn_model *m, const void *const *inputs,
                    float *const *outputs, float bound, uint32_t *bad_op);

void nn_reset(nn_model *m);
void nn_free(nn_model *m);

const nn_tdesc *nn_input_desc(const nn_model *m, uint32_t i);
size_t nn_input_bytes(const nn_model *m, uint32_t i);
const nn_tdesc *nn_output_desc(const nn_model *m, uint32_t j);
size_t nn_output_count(const nn_model *m, uint32_t j);
/* Pointer to the state of op `op` (NULL if it has none), and its bytes. */
const void *nn_op_state(const nn_model *m, uint32_t op, size_t *bytes);
/* The probe blob's per-step stride for this model (nn_hgw.h, "probes"). */
size_t nn_probe_stride(const nn_model *m);
