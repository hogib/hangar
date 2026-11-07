# lib/nn: the inference runtime

The pure-arithmetic core of `hangar-policy` (docs/09-learning.md, "The runtime"),
version 1: a model is a graph of ops over named buffers in a fixed order, read from a
`.hgw` weights file that is mapped and used in place, with one arena allocated at load
and nothing allocated per tick. No topics and no module here; the policy module comes
later over lib/hg. C11, libc and libm only. LGPL-3.0-or-later, since a policy module
links it.

| File | |
|---|---|
| `nn_ops.h/.c` | the op contract, the registry, the version-1 ops and their portable kernels |
| `nn_graph.h/.c` | the model: build from a parsed file, `nn_step`, `nn_reset`, `nn_free` |
| `nn_hgw.h/.c` | the `.hgw` format (layout pinned by `_Static_assert`), the architecture hash, the loader with checks 1 to 7 |
| `blake2s.h/.c` | BLAKE2s (RFC 7693) for check 1 |
| `nn.mk` | the make fragment the root Makefile includes |
| `tests/test_nn.c`, `tests/vectors/` | the suite and the golden files |
| `python/hangar/nn/` | `hgw.py` (writer/reader, same hash), `reference.py` (double-precision ops), `export_torch.py` (the exporter) |

```c
nn_load_req req = { .file_hash = digest, .arch = policy_arch, .tick_period_ns = 20000000,
                    .dataset = "depth_ff", .dataset_hash = spec_hash,
                    .input_fields = fields, .n_input_fields = n, .output_fields = ..., 
                    .step_budget_ns = 0.5 * 20e6 };
nn_model *m; nn_report rep;
if (nn_load("current.hgw", &req, &m, &rep) < 0)
    /* rep.check 1..7, rep.name, rep.reason */;
const void *in[1] = { row_bytes };  float *out[1] = { wrench_values };
if (nn_step(m, in, out) == NN_ERR_NONFINITE)   /* state reset, nothing written */;
nn_reset(m);  nn_free(m);
```

## The file format

```
[ header: 4096 bytes ][ section directory ][ sections ][ tensor data and probe blobs, 64-byte aligned ]
```

Little-endian throughout; every record is a fixed-size struct, so the file is read in
place after a structural parse (`nn_hgw_parse`). Every offset is pinned by a
`_Static_assert` in `nn_hgw.h`; a change is a new format version.

**Header** (4096 bytes):

| Offset | Size | Field | |
|---|---|---|---|
| 0 | 8 | `magic` | `"HGWEIGHT"` |
| 8 | 4 | `format_version` | 1 |
| 12 | 4 | `header_size` | 4096 |
| 16 | 8 | `arch_hash` | FNV-1a 64 of the canonical architecture (below) |
| 24 | 32 | `name` | model name, NUL-padded |
| 56 | 8 | `tick_period_ns` | the tick period the model was trained at |
| 64 | 4 | `warmup_ticks` | |
| 68 | 4 | `lineage_count` | entries valid in `sessions[]` / `ranges[]` (≤ 8) |
| 72 | 4 | `probe_rel_tol` | f32 |
| 76 | 4 | `probe_abs_tol` | f32 |
| 80 | 8 | `created_ns` | unix time |
| 88 | 32 | `trainer_program` | |
| 120 | 32 | `trainer_version` | |
| 152 | 8 | `trainer_settings_hash` | |
| 160 | 32 | `dataset` | lineage: dataset name |
| 192 | 8 | `dataset_hash` | lineage: hash of the spec |
| 200 | 8 | `rows` | lineage: rows used |
| 208 | 8 | `steps` | lineage: training steps |
| 216 | 8 | `parent_hash` | lineage: the weights this training started from, 0 if none |
| 224 | 16 | `newest_row_boot` | lineage: vehicle boot id of the newest row trained on |
| 240 | 8 | `newest_row_stamp_ns` | lineage: its `anchor_stamp_ns` |
| 248 | 1 | `autopilot_mode` | the mux mode the rows were recorded under, 0 unknown |
| 249 | 3 | reserved | |
| 252 | 4 | `activation_bound` | f32, 0 = none (check 6) |
| 256 | 8 | `file_size` | |
| 264 | 4 | `section_count` | |
| 268 | 4 | `section_dir_offset` | 4096 |
| 272 | 8 | `tensor_data_offset` | first byte of the data region, 64-aligned |
| 280 | 8 | reserved | |
| 288 | 8 × 48 | `sessions[8]` | lineage summary: `u8 boot[16]; char session[32]` (`"live"` for unrecorded rows) |
| 672 | 8 × 40 | `ranges[8]` | lineage summary: `u64 first_seq, last_seq, first_stamp_ns, last_stamp_ns, rows` |
| 992 | 3104 | zero | |

**Section directory**: `section_count` entries of 32 bytes at offset 4096,
`char name[16]; u64 offset; u64 size`. Each section starts 64-byte aligned and is an
array of its record type; a reader skips a name it does not know (the suite renames
`ranges` and loads the file). Required: `ops`, `buffers`, `inputs`, `outputs`.

| Section | Record | Bytes | Fields |
|---|---|---|---|
| `ops` | `nn_hgw_op` | 192 | `type[16]`, `version`, `n_inputs`, `n_tensors`, `n_attrs`, `inputs[4]` (buffer indices), `output` (buffer index), `tensors[16]` (tensor indices), `attrs[8]` f64 |
| `tensors` | `nn_hgw_tensor` | 80 | `name[32]`, `dtype`, `ndim`, `shape[4]`, `offset` (absolute, 64-aligned), `size` |
| `buffers` | `nn_hgw_buffer` | 64 | `name[32]`, `dtype`, `ndim`, `shape[4]` |
| `inputs` | `nn_hgw_binding` | 64 | `buffer`, `element`, `field[56]`: that element of that buffer is that row field |
| `outputs` | `nn_hgw_binding` | 64 | the same, with the fields of the output type |
| `probes` | `nn_hgw_probe` | 32 | `steps`, `offset`, `size` of a blob of `steps` records |
| `ranges` | `nn_hgw_range` | 16 | `lo, hi` f64, one per input binding in order (optional) |
| `lineage` | `nn_hgw_lineage` | 96 | `boot[16]`, `session[32]`, `first_seq`, `last_seq`, `first_stamp_ns`, `last_stamp_ns`, `rows` (optional; the header holds the first 8) |

`dtype` codes: 1 f32, 2 f64, 3 i8, 4 u8, 5 i16, 6 u16, 7 i32, 8 u32. The model's
inputs are the distinct buffers of the `inputs` section in order of first appearance,
every element of each bound exactly once; the outputs likewise. A probe record is, for
each model input in order, its raw bytes padded to 8, then for each model output in
order, its expected f32 values padded to 8 (`nn_probe_stride`). Tensor data and probe
blobs sit in the data region, each 64-byte aligned.

Op attributes (`attrs[]`, f64; integers exact) and tensors, in order:

| Op | `attrs` | tensors | |
|---|---|---|---|
| `dense` v1 | in, out, bias (0/1) | `W[out,in]`, `b[out]` if bias | `y = W x + b` |
| `lstm` v1 | input, hidden, layers | per layer k: `W_ih[4H, I or H]`, `W_hh[4H,H]`, `b_ih[4H]`, `b_hh[4H]` | PyTorch `nn.LSTM`: gates i, f, g, o; one time step per call; state `h[L][H], c[L][H]`; output the top layer's h |
| `affine` v1 | — | `scale[C]`, `shift[C]` | `y[c,…] = x[c,…]·scale[c] + shift[c]`, C the first dimension |
| `cast` v1 | from (dtype code), scale | — | `f32(x) · scale` from u8/i8/u16/i16/u32/i32/f64 |
| `relu`, `tanh`, `sigmoid` v1 | — | — | elementwise, `tanhf`, `1/(1+expf(-x))` |

## The architecture hash

FNV-1a 64 (offset `0xcbf29ce484222325`, prime `0x100000001b3`) over this ASCII text,
produced identically by `nn_hgw_arch_canon()` and `hangar.nn.hgw.arch_canon()`; every
integer is decimal, every attribute the 16 lowercase hex digits of its IEEE-754
binary64 bit pattern, one record per line, `\n` terminated:

```
hgw-arch-1
buffers <n>
buffer <i> <dtype> <ndim> <d0> ... <d(ndim-1)>        one per buffer
tensors <n>
tensor <i> <dtype> <ndim> <d0> ...                    one per tensor
ops <n>
op <i> <type> <version> attrs <n> <hex64>... in <n> <buffer>... out <buffer> tensors <n> <tensor>...
inputs <n>
input <buffer> <element> <field>                      one per input binding
outputs <n>
output <buffer> <element> <field>                     one per output binding
```

It covers the ops, their versions and attributes, every buffer's and tensor's type and
shape, the wiring, and the input and output bindings: everything but tensor values,
names (buffer and tensor names are labels) and the header. Training keeps the hash;
changing the network, an attribute or a binding changes it. The suite checks the text
of `dense_relu` literally and every golden hash against `python/tests/test_nn.py`.

## Adding an op

Three places, nothing else:

1. **`nn_ops.c`**: a block with `check` (attributes and tensor shapes against the input
   shapes, the output shape), `state_bytes`, `scratch_bytes`, `reset`, and the portable
   kernel; and one line in `nn_op_registry[]`: `{type, version, check, state_bytes,
   scratch_bytes, reset, kernels, n_kernels}`.
2. **`python/hangar/nn/reference.py`**: the same op in doubles (`SUPPORTED`,
   `check_graph`, `Model.step`), and its mapping in `export_torch.to_graph`.
3. **`python/tests/test_nn.py`**: a golden model using it; the vectors it writes are
   what `tests/test_nn.c` checks.

A faster **kernel** for an existing op is one entry in that op's kernel list, before
`"portable"`: `{ "neon", nn_have_neon, nn_dense_run_neon }`, the function under
`#if defined(__ARM_NEON)`, the probe a compile-time constant where the target makes
NEON certain or a HWCAP read where it does not. `nn_op_select_kernel` picks the first
available at load; the suite runs every available kernel of every op against the
vectors, so the new kernel must reproduce them within the tolerance. The op, its
attributes and results do not change.

## Loading

`nn_load(path, &req, &model, &report)` (or `nn_load_mem` over a mapping the caller
owns) runs docs/09's checks in order and stops at the first failure, leaving nothing
allocated and naming the check (1 to 7) and the reason:

1. **file hash**: BLAKE2s-256 of the file equals `req.file_hash` (NULL skips: the bridge
   checked it);
2. **format**: magic, version, structure; the header's `arch_hash` equals the hash of
   the graph; both equal `req.arch`;
3. **ops**: every op known (`op conv2d v1 not supported`), every `check` passing, buffers
   written once and read after they are written; the arena is allocated here;
4. **bindings**: `tick_period_ns`, `dataset`, `dataset_hash` as the caller gives them;
   every input field in `req.input_fields`, every output field in `req.output_fields`;
   every element of every input buffer bound exactly once;
5. **tensors**: every f32/f64 tensor value finite;
6. **probes**: each probe from zero state reproduces its expected outputs within the
   header's tolerance; with `activation_bound > 0`, every op output and the state stay
   under it on every step, and with a `ranges` section also on constant inputs at the
   corners and middle of the ranges and at each element's extremes (16 steps each);
7. **step time**: the mean step time over the probe steps is under `req.step_budget_ns`.

Check 8, the warm-up beside the running model, is the policy module's. `nn_step` copies
the inputs in, runs the ops, checks every output buffer and every recurrent state for
finite values and only then writes the outputs; a non-finite value resets the state,
counts in `model->nonfinite_count` and returns `NN_ERR_NONFINITE`.

## What version 1 leaves out

- the ops `conv2d`, `maxpool2d`, `avgpool2d`, `global_avgpool`, `add`, `concat`,
  `flatten`, `reshape`: refused at load by name and version;
- kernels other than the portable one (NEON, int8): the selection hook is in place;
- int8 tensors: the dtype code exists and the format carries them, no op consumes them;
- LSTM with `proj_size`, bidirectional, `bias=False`: the exporter refuses them;
- an op with more than one input (none in v1 needs it; the records allow 4);
- a model with more than 64 inputs or outputs (the loader's fixed arrays);
- everything that is a module's: topics, the warm-up, the swap, `revert`, files on disk.

## Deviations from docs/09, with reasons

- **`check` sees the tensor shapes too.** docs/09 writes `check(attrs, input shapes)`;
  here the signature also carries the tensors' descriptors, since W against `in`/`out`
  is what most checks are. The contract is otherwise as written: the call bundles
  `attrs, tensors, inputs, output, state, scratch` in one struct (`nn_op_call`) rather
  than six arguments, and `reset(state)` also gets the state's size so one zeroing
  `reset` serves every op.
- **The ops share one file.** docs/09 says "each op is one source file"; the seven v1
  ops are a few dozen lines each and sit in `nn_ops.c` as separate blocks, one
  registry line each. A big op (conv2d) gets its own file when it arrives; nothing in
  the registry cares.
- **Lineage summary in the header.** docs/09 lists the lineage fields and a `lineage`
  section of any length; the header here carries the first 8 entries
  (`sessions[8]`, `ranges[8]`) so a reader has them without touching the sections, and
  the section holds them all.
- **The activation bound is in v1.** docs/09 lists it under Later; the header field, the
  `ranges` section and the check (folded into check 6, so the check numbers of
  `policy.<name>.load` stay 1 to 8) are here, because the loader is the place and the
  cost is nil.
- **One hash for the file.** The caller gives a BLAKE2s-256; SHA-256 is not offered,
  since the bridge hashes files with BLAKE2s and two would mean two implementations.
- **Multiple outputs.** The format and the runtime take any number of input and output
  buffers (the `casts` golden has seven of each); a policy uses one.
- **Floats and ints in `nn_step`.** docs/09 has `f32 throughout`; inputs may be raw row
  fields of any castable type, as `cast` requires, read with per-element `memcpy` so a
  packed row needs no alignment.
- **The exporter's real-torch path is untested here**: torch is not installed on this
  machine. It is written against the public attribute names (`weight_ih_l{k}`, ...),
  guarded by `try: import torch`, and exercised with stand-in modules that expose the
  same attributes.
