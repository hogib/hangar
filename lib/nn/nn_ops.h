// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
/*
 * The op contract (docs/09-learning.md, "The runtime"): each op is a type
 * and a version implementing check / state_bytes / scratch_bytes / run /
 * reset, and one line in nn_op_registry[]. A kernel is one implementation
 * of an op version's run; the registry line lists the op's kernels fastest
 * first, the portable one last, and nn_op_select_kernel() picks the first
 * whose available() says the processor has what it needs.
 *
 * Adding a NEON kernel to, say, dense: write nn_dense_run_neon(const
 * nn_op_call *) under #if defined(__ARM_NEON) (in nn_ops.c or a file of its
 * own), an availability probe (a constant true where __ARM_NEON is a
 * compile-time fact, or a /proc/cpuinfo or HWCAP probe where it is not), and
 * put { "neon", nn_have_neon, nn_dense_run_neon } before { "portable", ... }
 * in dense's kernel list. The op's check, attributes, state and results do
 * not change; the kernel must pass the same golden vectors within the
 * tolerance (lib/nn/tests/test_nn.c runs every available kernel).
 *
 * Version-1 ops, their attributes (attrs.v[], in this order) and tensors:
 *
 *   dense    v1  in, out, bias(0/1)          W[out,in], b[out] if bias       x[in] f32 -> y[out]
 *   lstm     v1  input, hidden, layers        per layer k: W_ih[4H, I or H],  x[I] f32 -> h_top[H]
 *                                             W_hh[4H,H], b_ih[4H], b_hh[4H]  state: h[L][H], c[L][H]
 *   affine   v1  (none)                       scale[C], shift[C]             x[C,...] -> x*scale[c]+shift[c]
 *   cast     v1  from (nn_dtype code), scale  (none)                         x[...] of `from` -> f32 * scale
 *   relu     v1  (none)                       (none)                         f32 elementwise
 *   tanh     v1  (none)                       (none)                         f32 elementwise, tanhf
 *   sigmoid  v1  (none)                       (none)                         f32 elementwise, 1/(1+expf(-x))
 *
 * Every op writes f32. Attributes travel as f64 in the file; integers are
 * exact. Shapes are up to 4 dimensions; the first dimension of an affine's
 * input is its channel axis.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NN_MAX_DIMS 4
#define NN_MAX_OP_INPUTS 4
#define NN_MAX_OP_TENSORS 16
#define NN_MAX_ATTRS 8
#define NN_REASON_LEN 96

typedef enum nn_dtype {
  NN_DTYPE_NONE = 0,
  NN_F32 = 1,
  NN_F64 = 2,
  NN_I8 = 3,
  NN_U8 = 4,
  NN_I16 = 5,
  NN_U16 = 6,
  NN_I32 = 7,
  NN_U32 = 8,
  NN_DTYPE_COUNT
} nn_dtype;

size_t nn_dtype_size(uint32_t dtype); /* 0 for an unknown code */
const char *nn_dtype_name(uint32_t dtype);

typedef struct nn_tdesc {
  uint32_t dtype;
  uint32_t ndim;
  uint32_t dims[NN_MAX_DIMS];
} nn_tdesc;

size_t nn_tdesc_numel(const nn_tdesc *d);
size_t nn_tdesc_bytes(const nn_tdesc *d);
bool nn_tdesc_equal(const nn_tdesc *a, const nn_tdesc *b);

typedef struct nn_attrs {
  uint32_t n;
  double v[NN_MAX_ATTRS];
} nn_attrs;

typedef struct nn_tensor {
  nn_tdesc d;
  const void *data;
} nn_tensor;

/* What check, state_bytes and scratch_bytes see: attributes, the input
 * buffers' descriptors and the tensors' descriptors (the tensors' shapes
 * are checked here too, against the attributes). */
typedef struct nn_op_sig {
  const nn_attrs *attrs;
  uint32_t n_inputs;
  const nn_tdesc *inputs;
  uint32_t n_tensors;
  const nn_tdesc *tensors;
} nn_op_sig;

/* What run sees: attrs, tensors, inputs, output, state, scratch. */
typedef struct nn_op_call {
  const nn_attrs *attrs;
  const nn_tensor *tensors;
  const void *const *inputs;
  const nn_tdesc *input_desc;
  float *output;
  const nn_tdesc *output_desc;
  void *state;
  void *scratch;
} nn_op_call;

typedef void (*nn_run_fn)(const nn_op_call *c);

typedef struct nn_kernel {
  const char *name;
  bool (*available)(void);
  nn_run_fn run;
} nn_kernel;

typedef struct nn_op_def {
  const char *type;
  uint32_t version;
  /* 0 and *out filled, or -1 with the reason (reason_len bytes). */
  int (*check)(const nn_op_sig *s, nn_tdesc *out, char *reason, size_t reason_len);
  size_t (*state_bytes)(const nn_op_sig *s);
  size_t (*scratch_bytes)(const nn_op_sig *s);
  void (*reset)(void *state, size_t state_bytes);
  const nn_kernel *kernels;
  size_t n_kernels;
} nn_op_def;

extern const nn_op_def nn_op_registry[];
extern const size_t nn_op_registry_len;

const nn_op_def *nn_op_find(const char *type, uint32_t version);
const nn_kernel *nn_op_select_kernel(const nn_op_def *def);
/* The registry entry for a kernel by name (the tests run every kernel). */
const nn_kernel *nn_op_kernel_named(const nn_op_def *def, const char *name);

/* The cast attribute `from` names an nn_dtype; this says whether the
 * runtime converts it. */
bool nn_cast_supports(uint32_t dtype);
