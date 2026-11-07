// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * The version-1 ops and the registry. See nn_ops.h for the contract and the
 * attribute table. Each op below is one block: check, state_bytes,
 * scratch_bytes, reset, the portable kernel, and its kernel list; the
 * registry at the end has one line per op version. Faster kernels go
 * before the portable one in an op's kernel list; nothing else changes.
 */
#include "nn_ops.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- shared helpers ---------------------------------------------------- */

size_t nn_dtype_size(uint32_t dtype) {
  switch (dtype) {
  case NN_F32:
  case NN_I32:
  case NN_U32:
    return 4;
  case NN_F64:
    return 8;
  case NN_I8:
  case NN_U8:
    return 1;
  case NN_I16:
  case NN_U16:
    return 2;
  default:
    return 0;
  }
}

const char *nn_dtype_name(uint32_t dtype) {
  switch (dtype) {
  case NN_F32:
    return "f32";
  case NN_F64:
    return "f64";
  case NN_I8:
    return "i8";
  case NN_U8:
    return "u8";
  case NN_I16:
    return "i16";
  case NN_U16:
    return "u16";
  case NN_I32:
    return "i32";
  case NN_U32:
    return "u32";
  default:
    return "?";
  }
}

size_t nn_tdesc_numel(const nn_tdesc *d) {
  size_t n = 1;
  uint32_t i;
  if (d->ndim == 0 || d->ndim > NN_MAX_DIMS)
    return 0;
  for (i = 0; i < d->ndim; i++)
    n *= d->dims[i];
  return n;
}

size_t nn_tdesc_bytes(const nn_tdesc *d) {
  return nn_tdesc_numel(d) * nn_dtype_size(d->dtype);
}

bool nn_tdesc_equal(const nn_tdesc *a, const nn_tdesc *b) {
  uint32_t i;
  if (a->dtype != b->dtype || a->ndim != b->ndim)
    return false;
  for (i = 0; i < a->ndim; i++)
    if (a->dims[i] != b->dims[i])
      return false;
  return true;
}

static bool always(void) { return true; }

/* An attribute that must be an integer in [lo, hi]. */
static bool attr_uint(const nn_attrs *a, uint32_t i, uint32_t lo, uint32_t hi,
                      uint32_t *out) {
  double v;
  if (i >= a->n)
    return false;
  v = a->v[i];
  if (!(v >= (double)lo && v <= (double)hi) || v != floor(v))
    return false;
  *out = (uint32_t)v;
  return true;
}

static int fail(char *reason, size_t len, const char *fmt, const char *a,
                unsigned b, unsigned c) {
  if (reason && len)
    snprintf(reason, len, fmt, a, b, c);
  return -1;
}

static bool is_f32_vec(const nn_tdesc *d, uint32_t n) {
  return d->dtype == NN_F32 && d->ndim == 1 && d->dims[0] == n;
}

static bool is_f32_mat(const nn_tdesc *d, uint32_t rows, uint32_t cols) {
  return d->dtype == NN_F32 && d->ndim == 2 && d->dims[0] == rows &&
         d->dims[1] == cols;
}

static int check_one_f32_input(const nn_op_sig *s, const char *op, char *r,
                               size_t rl) {
  if (s->n_inputs != 1)
    return fail(r, rl, "%s takes 1 input, has %u", op, s->n_inputs, 0);
  if (s->inputs[0].dtype != NN_F32)
    return fail(r, rl, "%s input must be f32, is %s", op,
                (unsigned)s->inputs[0].dtype, 0);
  if (nn_tdesc_numel(&s->inputs[0]) == 0)
    return fail(r, rl, "%s input has no elements", op, 0, 0);
  return 0;
}

static size_t no_state(const nn_op_sig *s) {
  (void)s;
  return 0;
}

static void zero_reset(void *state, size_t bytes) {
  if (bytes)
    memset(state, 0, bytes);
}

static float sigmoidf_(float x) { return 1.0f / (1.0f + expf(-x)); }

/* y[out] = b + W x, W row-major [out][in]. */
static void matvec(float *y, const float *W, const float *x, const float *b,
                   uint32_t out, uint32_t in) {
  uint32_t o, i;
  for (o = 0; o < out; o++) {
    const float *w = W + (size_t)o * in;
    float acc = b ? b[o] : 0.0f;
    for (i = 0; i < in; i++)
      acc += w[i] * x[i];
    y[o] = acc;
  }
}

/* ---- dense v1: y = W x + b --------------------------------------------- */

static int dense_check(const nn_op_sig *s, nn_tdesc *out, char *r, size_t rl) {
  uint32_t in, o, bias;
  if (s->attrs->n != 3)
    return fail(r, rl, "dense needs attrs in, out, bias; has %u", "", s->attrs->n, 0);
  if (!attr_uint(s->attrs, 0, 1, 1u << 20, &in))
    return fail(r, rl, "dense attr in out of range", "", 0, 0);
  if (!attr_uint(s->attrs, 1, 1, 1u << 20, &o))
    return fail(r, rl, "dense attr out out of range", "", 0, 0);
  if (!attr_uint(s->attrs, 2, 0, 1, &bias))
    return fail(r, rl, "dense attr bias must be 0 or 1", "", 0, 0);
  if (check_one_f32_input(s, "dense", r, rl) < 0)
    return -1;
  if (nn_tdesc_numel(&s->inputs[0]) != in)
    return fail(r, rl, "dense input has %u elements, attr in is %u", "",
                (unsigned)nn_tdesc_numel(&s->inputs[0]), in);
  if (s->n_tensors != 1 + bias)
    return fail(r, rl, "dense needs %u tensors, has %u", "", 1 + bias, s->n_tensors);
  if (!is_f32_mat(&s->tensors[0], o, in))
    return fail(r, rl, "dense W must be f32 [%u, %u]", "", o, in);
  if (bias && !is_f32_vec(&s->tensors[1], o))
    return fail(r, rl, "dense b must be f32 [%u]", "", o, 0);
  out->dtype = NN_F32;
  out->ndim = 1;
  out->dims[0] = o;
  out->dims[1] = out->dims[2] = out->dims[3] = 0;
  return 0;
}

static void dense_run_portable(const nn_op_call *c) {
  uint32_t in = (uint32_t)c->attrs->v[0], out = (uint32_t)c->attrs->v[1];
  bool bias = c->attrs->v[2] != 0.0;
  matvec(c->output, (const float *)c->tensors[0].data,
         (const float *)c->inputs[0], bias ? (const float *)c->tensors[1].data : NULL,
         out, in);
}

static const nn_kernel dense_kernels[] = {
    {"portable", always, dense_run_portable},
};

/* ---- lstm v1: PyTorch nn.LSTM, one time step per call ------------------ */

static int lstm_check(const nn_op_sig *s, nn_tdesc *out, char *r, size_t rl) {
  uint32_t I, H, L, k;
  if (s->attrs->n != 3)
    return fail(r, rl, "lstm needs attrs input, hidden, layers; has %u", "",
                s->attrs->n, 0);
  if (!attr_uint(s->attrs, 0, 1, 1u << 16, &I))
    return fail(r, rl, "lstm attr input out of range", "", 0, 0);
  if (!attr_uint(s->attrs, 1, 1, 1u << 16, &H))
    return fail(r, rl, "lstm attr hidden out of range", "", 0, 0);
  if (!attr_uint(s->attrs, 2, 1, NN_MAX_OP_TENSORS / 4, &L))
    return fail(r, rl, "lstm attr layers must be 1 to %u", "",
                NN_MAX_OP_TENSORS / 4, 0);
  if (check_one_f32_input(s, "lstm", r, rl) < 0)
    return -1;
  if (nn_tdesc_numel(&s->inputs[0]) != I)
    return fail(r, rl, "lstm input has %u elements, attr input is %u", "",
                (unsigned)nn_tdesc_numel(&s->inputs[0]), I);
  if (s->n_tensors != 4 * L)
    return fail(r, rl, "lstm with %u layers needs %u tensors", "", L, 4 * L);
  for (k = 0; k < L; k++) {
    uint32_t Ik = k == 0 ? I : H;
    const nn_tdesc *t = s->tensors + 4 * k;
    if (!is_f32_mat(&t[0], 4 * H, Ik))
      return fail(r, rl, "lstm layer %u W_ih must be f32 [%u, %u]", "", k, 4 * H);
    if (!is_f32_mat(&t[1], 4 * H, H))
      return fail(r, rl, "lstm layer %u W_hh must be f32 [%u, H]", "", k, 4 * H);
    if (!is_f32_vec(&t[2], 4 * H))
      return fail(r, rl, "lstm layer %u b_ih must be f32 [%u]", "", k, 4 * H);
    if (!is_f32_vec(&t[3], 4 * H))
      return fail(r, rl, "lstm layer %u b_hh must be f32 [%u]", "", k, 4 * H);
  }
  out->dtype = NN_F32;
  out->ndim = 1;
  out->dims[0] = H;
  out->dims[1] = out->dims[2] = out->dims[3] = 0;
  return 0;
}

/* state: h[L][H] then c[L][H] */
static size_t lstm_state_bytes(const nn_op_sig *s) {
  return 2 * (size_t)s->attrs->v[2] * (size_t)s->attrs->v[1] * sizeof(float);
}

/* scratch: gates[4H] */
static size_t lstm_scratch_bytes(const nn_op_sig *s) {
  return 4 * (size_t)s->attrs->v[1] * sizeof(float);
}

static void lstm_run_portable(const nn_op_call *c) {
  uint32_t I = (uint32_t)c->attrs->v[0], H = (uint32_t)c->attrs->v[1],
           L = (uint32_t)c->attrs->v[2], k, j;
  float *h_all = (float *)c->state, *c_all = h_all + (size_t)L * H;
  float *g = (float *)c->scratch;
  const float *x = (const float *)c->inputs[0];
  for (k = 0; k < L; k++) {
    const nn_tensor *t = c->tensors + 4 * k;
    const float *W_ih = (const float *)t[0].data, *W_hh = (const float *)t[1].data;
    const float *b_ih = (const float *)t[2].data, *b_hh = (const float *)t[3].data;
    float *h = h_all + (size_t)k * H, *cc = c_all + (size_t)k * H;
    uint32_t Ik = k == 0 ? I : H;
    /* g = W_ih x + b_ih + W_hh h + b_hh, gate order i, f, g, o */
    for (j = 0; j < 4 * H; j++) {
      const float *wi = W_ih + (size_t)j * Ik, *wh = W_hh + (size_t)j * H;
      float acc = b_ih[j] + b_hh[j];
      uint32_t i;
      for (i = 0; i < Ik; i++)
        acc += wi[i] * x[i];
      for (i = 0; i < H; i++)
        acc += wh[i] * h[i];
      g[j] = acc;
    }
    for (j = 0; j < H; j++) {
      float ig = sigmoidf_(g[j]), fg = sigmoidf_(g[H + j]);
      float gg = tanhf(g[2 * H + j]), og = sigmoidf_(g[3 * H + j]);
      float cn = fg * cc[j] + ig * gg;
      cc[j] = cn;
      h[j] = og * tanhf(cn);
    }
    x = h; /* the layer above reads this layer's new hidden state */
  }
  memcpy(c->output, h_all + (size_t)(L - 1) * H, (size_t)H * sizeof(float));
}

static const nn_kernel lstm_kernels[] = {
    {"portable", always, lstm_run_portable},
};

/* ---- affine v1: y[c,...] = x[c,...] * scale[c] + shift[c] -------------- */

static int affine_check(const nn_op_sig *s, nn_tdesc *out, char *r, size_t rl) {
  uint32_t C;
  if (s->attrs->n != 0)
    return fail(r, rl, "affine takes no attrs, has %u", "", s->attrs->n, 0);
  if (check_one_f32_input(s, "affine", r, rl) < 0)
    return -1;
  C = s->inputs[0].dims[0];
  if (s->n_tensors != 2)
    return fail(r, rl, "affine needs tensors scale, shift; has %u", "", s->n_tensors, 0);
  if (!is_f32_vec(&s->tensors[0], C))
    return fail(r, rl, "affine scale must be f32 [%u]", "", C, 0);
  if (!is_f32_vec(&s->tensors[1], C))
    return fail(r, rl, "affine shift must be f32 [%u]", "", C, 0);
  *out = s->inputs[0];
  return 0;
}

static void affine_run_portable(const nn_op_call *c) {
  const nn_tdesc *d = &c->input_desc[0];
  uint32_t C = d->dims[0];
  size_t per = nn_tdesc_numel(d) / C, ch, i;
  const float *x = (const float *)c->inputs[0];
  const float *scale = (const float *)c->tensors[0].data;
  const float *shift = (const float *)c->tensors[1].data;
  float *y = c->output;
  for (ch = 0; ch < C; ch++)
    for (i = 0; i < per; i++)
      y[ch * per + i] = x[ch * per + i] * scale[ch] + shift[ch];
}

static const nn_kernel affine_kernels[] = {
    {"portable", always, affine_run_portable},
};

/* ---- cast v1: f32(x) * scale ------------------------------------------- */

bool nn_cast_supports(uint32_t dtype) {
  switch (dtype) {
  case NN_U8:
  case NN_I8:
  case NN_U16:
  case NN_I16:
  case NN_U32:
  case NN_I32:
  case NN_F64:
    return true;
  default:
    return false;
  }
}

static int cast_check(const nn_op_sig *s, nn_tdesc *out, char *r, size_t rl) {
  uint32_t from;
  if (s->attrs->n != 2)
    return fail(r, rl, "cast needs attrs from, scale; has %u", "", s->attrs->n, 0);
  if (!attr_uint(s->attrs, 0, 1, NN_DTYPE_COUNT - 1, &from) || !nn_cast_supports(from))
    return fail(r, rl, "cast attr from names no castable type", "", 0, 0);
  if (!isfinite(s->attrs->v[1]))
    return fail(r, rl, "cast attr scale is not finite", "", 0, 0);
  if (s->n_inputs != 1)
    return fail(r, rl, "cast takes 1 input, has %u", "", s->n_inputs, 0);
  if (s->inputs[0].dtype != from) {
    if (r && rl)
      snprintf(r, rl, "cast input is %s, attr from says %s",
               nn_dtype_name(s->inputs[0].dtype), nn_dtype_name(from));
    return -1;
  }
  if (nn_tdesc_numel(&s->inputs[0]) == 0)
    return fail(r, rl, "cast input has no elements", "", 0, 0);
  if (s->n_tensors != 0)
    return fail(r, rl, "cast takes no tensors, has %u", "", s->n_tensors, 0);
  *out = s->inputs[0];
  out->dtype = NN_F32;
  return 0;
}

static void cast_run_portable(const nn_op_call *c) {
  const nn_tdesc *d = &c->input_desc[0];
  size_t n = nn_tdesc_numel(d), i;
  uint32_t from = (uint32_t)c->attrs->v[0];
  float scale = (float)c->attrs->v[1];
  const uint8_t *p = (const uint8_t *)c->inputs[0];
  float *y = c->output;
  /* memcpy per element: row fields need not be aligned. */
  for (i = 0; i < n; i++) {
    float v = 0.0f;
    switch (from) {
    case NN_U8:
      v = (float)p[i];
      break;
    case NN_I8:
      v = (float)(int8_t)p[i];
      break;
    case NN_U16: {
      uint16_t u;
      memcpy(&u, p + 2 * i, 2);
      v = (float)u;
      break;
    }
    case NN_I16: {
      int16_t u;
      memcpy(&u, p + 2 * i, 2);
      v = (float)u;
      break;
    }
    case NN_U32: {
      uint32_t u;
      memcpy(&u, p + 4 * i, 4);
      v = (float)u;
      break;
    }
    case NN_I32: {
      int32_t u;
      memcpy(&u, p + 4 * i, 4);
      v = (float)u;
      break;
    }
    case NN_F64: {
      double u;
      memcpy(&u, p + 8 * i, 8);
      v = (float)u;
      break;
    }
    default:
      break;
    }
    y[i] = v * scale;
  }
}

static const nn_kernel cast_kernels[] = {
    {"portable", always, cast_run_portable},
};

/* ---- relu, tanh, sigmoid v1 -------------------------------------------- */

static int elementwise_check(const nn_op_sig *s, nn_tdesc *out, const char *op,
                             char *r, size_t rl) {
  if (s->attrs->n != 0)
    return fail(r, rl, "%s takes no attrs, has %u", op, s->attrs->n, 0);
  if (check_one_f32_input(s, op, r, rl) < 0)
    return -1;
  if (s->n_tensors != 0)
    return fail(r, rl, "%s takes no tensors, has %u", op, s->n_tensors, 0);
  *out = s->inputs[0];
  return 0;
}

static int relu_check(const nn_op_sig *s, nn_tdesc *out, char *r, size_t rl) {
  return elementwise_check(s, out, "relu", r, rl);
}
static int tanh_check(const nn_op_sig *s, nn_tdesc *out, char *r, size_t rl) {
  return elementwise_check(s, out, "tanh", r, rl);
}
static int sigmoid_check(const nn_op_sig *s, nn_tdesc *out, char *r, size_t rl) {
  return elementwise_check(s, out, "sigmoid", r, rl);
}

static void relu_run_portable(const nn_op_call *c) {
  size_t n = nn_tdesc_numel(&c->input_desc[0]), i;
  const float *x = (const float *)c->inputs[0];
  for (i = 0; i < n; i++)
    c->output[i] = x[i] > 0.0f ? x[i] : 0.0f;
}

static void tanh_run_portable(const nn_op_call *c) {
  size_t n = nn_tdesc_numel(&c->input_desc[0]), i;
  const float *x = (const float *)c->inputs[0];
  for (i = 0; i < n; i++)
    c->output[i] = tanhf(x[i]);
}

static void sigmoid_run_portable(const nn_op_call *c) {
  size_t n = nn_tdesc_numel(&c->input_desc[0]), i;
  const float *x = (const float *)c->inputs[0];
  for (i = 0; i < n; i++)
    c->output[i] = sigmoidf_(x[i]);
}

static const nn_kernel relu_kernels[] = {{"portable", always, relu_run_portable}};
static const nn_kernel tanh_kernels[] = {{"portable", always, tanh_run_portable}};
static const nn_kernel sigmoid_kernels[] = {{"portable", always, sigmoid_run_portable}};

/* ---- the registry: one line per op version ----------------------------- */

#define KERNELS(k) k, sizeof(k) / sizeof((k)[0])

const nn_op_def nn_op_registry[] = {
    {"dense", 1, dense_check, no_state, no_state, zero_reset, KERNELS(dense_kernels)},
    {"lstm", 1, lstm_check, lstm_state_bytes, lstm_scratch_bytes, zero_reset,
     KERNELS(lstm_kernels)},
    {"affine", 1, affine_check, no_state, no_state, zero_reset, KERNELS(affine_kernels)},
    {"cast", 1, cast_check, no_state, no_state, zero_reset, KERNELS(cast_kernels)},
    {"relu", 1, relu_check, no_state, no_state, zero_reset, KERNELS(relu_kernels)},
    {"tanh", 1, tanh_check, no_state, no_state, zero_reset, KERNELS(tanh_kernels)},
    {"sigmoid", 1, sigmoid_check, no_state, no_state, zero_reset,
     KERNELS(sigmoid_kernels)},
};
const size_t nn_op_registry_len = sizeof(nn_op_registry) / sizeof(nn_op_registry[0]);

const nn_op_def *nn_op_find(const char *type, uint32_t version) {
  size_t i;
  for (i = 0; i < nn_op_registry_len; i++)
    if (nn_op_registry[i].version == version &&
        strcmp(nn_op_registry[i].type, type) == 0)
      return &nn_op_registry[i];
  return NULL;
}

const nn_kernel *nn_op_select_kernel(const nn_op_def *def) {
  size_t i;
  for (i = 0; i < def->n_kernels; i++)
    if (def->kernels[i].available())
      return &def->kernels[i];
  return NULL;
}

const nn_kernel *nn_op_kernel_named(const nn_op_def *def, const char *name) {
  size_t i;
  for (i = 0; i < def->n_kernels; i++)
    if (strcmp(def->kernels[i].name, name) == 0)
      return &def->kernels[i];
  return NULL;
}
