# SPDX-License-Identifier: LGPL-3.0-or-later
"""The .hgw weights file (lib/nn/nn_hgw.h), written and read over plain lists.

A model is a Graph: buffers, tensors, ops, input bindings, output bindings.
`write(path, graph, header, probes)` produces a file byte for byte as
lib/nn reads it; `read(path)` gives it back; `arch_hash(graph)` is the
FNV-1a 64 hash of the canonical architecture text that lib/nn computes
(lib/nn/README.md, "The architecture hash"). No numpy: tensors are flat
lists of Python numbers, so the writer has no dependency and the hash is
the same everywhere.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

MAGIC = b"HGWEIGHT"
FORMAT_VERSION = 1
HEADER_SIZE = 4096
ALIGN = 64
MAX_DIMS = 4
MAX_OP_INPUTS = 4
MAX_OP_TENSORS = 16
MAX_ATTRS = 8
MAX_LINEAGE = 8

# nn_dtype codes and their struct formats (little-endian).
DTYPES: Dict[str, Tuple[int, str]] = {
    "f32": (1, "<f"),
    "f64": (2, "<d"),
    "i8": (3, "<b"),
    "u8": (4, "<B"),
    "i16": (5, "<h"),
    "u16": (6, "<H"),
    "i32": (7, "<i"),
    "u32": (8, "<I"),
}
DTYPE_BY_CODE = {code: name for name, (code, _) in DTYPES.items()}
CASTABLE = ("u8", "i8", "u16", "i16", "u32", "i32", "f64")

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


def fnv1a64(data: bytes, h: int = FNV_OFFSET) -> int:
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def dtype_code(name: str) -> int:
    return DTYPES[name][0]


def dtype_size(name: str) -> int:
    return struct.calcsize(DTYPES[name][1])


def numel(shape: Sequence[int]) -> int:
    n = 1
    for d in shape:
        n *= d
    return n


def _pad8(n: int) -> int:
    return (n + 7) // 8 * 8


def _align(n: int, a: int = ALIGN) -> int:
    return (n + a - 1) // a * a


def _cstr(s: str, n: int) -> bytes:
    b = s.encode("utf-8")
    if len(b) >= n:
        raise ValueError(f"string {s!r} does not fit in {n} bytes with a NUL")
    return b + b"\0" * (n - len(b))


def _rcstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("utf-8")


@dataclass
class Buffer:
    name: str
    dtype: str
    shape: List[int]


@dataclass
class Tensor:
    name: str
    dtype: str
    shape: List[int]
    data: List[float]  # flat, row-major

    def __post_init__(self):
        if len(self.data) != numel(self.shape):
            raise ValueError(
                f"tensor {self.name}: {len(self.data)} values for shape {self.shape}"
            )


@dataclass
class Op:
    type: str
    version: int
    attrs: List[float]
    inputs: List[int]  # buffer indices
    output: int  # buffer index
    tensors: List[int] = field(default_factory=list)  # tensor indices


@dataclass
class Binding:
    buffer: int
    element: int
    field: str


@dataclass
class Graph:
    buffers: List[Buffer]
    tensors: List[Tensor]
    ops: List[Op]
    inputs: List[Binding]
    outputs: List[Binding]

    def input_buffers(self) -> List[int]:
        """Distinct input buffers in order of first binding: the model's inputs."""
        out: List[int] = []
        for b in self.inputs:
            if b.buffer not in out:
                out.append(b.buffer)
        return out

    def output_buffers(self) -> List[int]:
        out: List[int] = []
        for b in self.outputs:
            if b.buffer not in out:
                out.append(b.buffer)
        return out


@dataclass
class Lineage:
    boot: bytes = b"\0" * 16
    session: str = "live"
    first_seq: int = 0
    last_seq: int = 0
    first_stamp_ns: int = 0
    last_stamp_ns: int = 0
    rows: int = 0


@dataclass
class Header:
    name: str = ""
    tick_period_ns: int = 0
    warmup_ticks: int = 0
    probe_rel_tol: float = 1e-4
    probe_abs_tol: float = 1e-5
    created_ns: int = 0
    trainer_program: str = ""
    trainer_version: str = ""
    trainer_settings_hash: int = 0
    dataset: str = ""
    dataset_hash: int = 0
    rows: int = 0
    steps: int = 0
    parent_hash: int = 0
    newest_row_boot: bytes = b"\0" * 16
    newest_row_stamp_ns: int = 0
    autopilot_mode: int = 0
    activation_bound: float = 0.0
    lineage: List[Lineage] = field(default_factory=list)
    # filled by read(): the hash the file carries, and the file's own fields
    arch_hash: int = 0
    format_version: int = FORMAT_VERSION


@dataclass
class Probe:
    """`steps` records; inputs[s][i] is a flat list of model input i's values
    (in its buffer's dtype) and outputs[s][j] the expected f32 values of
    model output j."""

    inputs: List[List[List[float]]]
    outputs: List[List[List[float]]]

    @property
    def steps(self) -> int:
        return len(self.inputs)


@dataclass
class File:
    header: Header
    graph: Graph
    probes: List[Probe]
    ranges: Optional[List[Tuple[float, float]]] = None
    sections: List[Tuple[str, int, int]] = field(default_factory=list)


# ---- the canonical architecture text and its hash ------------------------


def arch_canon(g: Graph) -> bytes:
    lines = ["hgw-arch-1"]
    lines.append(f"buffers {len(g.buffers)}")
    for i, b in enumerate(g.buffers):
        lines.append(
            f"buffer {i} {dtype_code(b.dtype)} {len(b.shape)} " + " ".join(str(d) for d in b.shape)
        )
    lines.append(f"tensors {len(g.tensors)}")
    for i, t in enumerate(g.tensors):
        lines.append(
            f"tensor {i} {dtype_code(t.dtype)} {len(t.shape)} " + " ".join(str(d) for d in t.shape)
        )
    lines.append(f"ops {len(g.ops)}")
    for i, op in enumerate(g.ops):
        parts = [f"op {i} {op.type} {op.version} attrs {len(op.attrs)}"]
        for a in op.attrs:
            (bits,) = struct.unpack("<Q", struct.pack("<d", float(a)))
            parts.append(f"{bits:016x}")
        parts.append(f"in {len(op.inputs)}")
        parts += [str(b) for b in op.inputs]
        parts.append(f"out {op.output} tensors {len(op.tensors)}")
        parts += [str(t) for t in op.tensors]
        lines.append(" ".join(parts))
    lines.append(f"inputs {len(g.inputs)}")
    for b in g.inputs:
        lines.append(f"input {b.buffer} {b.element} {b.field}")
    lines.append(f"outputs {len(g.outputs)}")
    for b in g.outputs:
        lines.append(f"output {b.buffer} {b.element} {b.field}")
    return ("\n".join(lines) + "\n").encode("ascii")


def arch_hash(g: Graph) -> int:
    return fnv1a64(arch_canon(g))


# ---- probe records --------------------------------------------------------


def probe_stride(g: Graph) -> int:
    s = 0
    for bi in g.input_buffers():
        b = g.buffers[bi]
        s += _pad8(numel(b.shape) * dtype_size(b.dtype))
    for bi in g.output_buffers():
        s += _pad8(numel(g.buffers[bi].shape) * 4)
    return s


def pack_values(dtype: str, values: Sequence[float]) -> bytes:
    fmt = DTYPES[dtype][1]
    if dtype in ("f32", "f64"):
        return b"".join(struct.pack(fmt, float(v)) for v in values)
    return b"".join(struct.pack(fmt, int(v)) for v in values)


def unpack_values(dtype: str, data: bytes) -> List[float]:
    fmt = DTYPES[dtype][1]
    n = struct.calcsize(fmt)
    return [struct.unpack(fmt, data[i : i + n])[0] for i in range(0, len(data), n)]


def _probe_blob(g: Graph, p: Probe) -> bytes:
    ins, outs = g.input_buffers(), g.output_buffers()
    out = bytearray()
    for s in range(p.steps):
        if len(p.inputs[s]) != len(ins) or len(p.outputs[s]) != len(outs):
            raise ValueError(f"probe step {s}: wrong number of inputs or outputs")
        for i, bi in enumerate(ins):
            b = g.buffers[bi]
            if len(p.inputs[s][i]) != numel(b.shape):
                raise ValueError(f"probe step {s} input {i}: {len(p.inputs[s][i])} values")
            raw = pack_values(b.dtype, p.inputs[s][i])
            out += raw + b"\0" * (_pad8(len(raw)) - len(raw))
        for j, bj in enumerate(outs):
            if len(p.outputs[s][j]) != numel(g.buffers[bj].shape):
                raise ValueError(f"probe step {s} output {j}: {len(p.outputs[s][j])} values")
            raw = pack_values("f32", p.outputs[s][j])
            out += raw + b"\0" * (_pad8(len(raw)) - len(raw))
    return bytes(out)


def _probe_from_blob(g: Graph, blob: bytes, steps: int) -> Probe:
    ins, outs = g.input_buffers(), g.output_buffers()
    stride = probe_stride(g)
    inputs, outputs = [], []
    for s in range(steps):
        off = s * stride
        row_in, row_out = [], []
        for bi in ins:
            b = g.buffers[bi]
            n = numel(b.shape) * dtype_size(b.dtype)
            row_in.append(unpack_values(b.dtype, blob[off : off + n]))
            off += _pad8(n)
        for bj in outs:
            n = numel(g.buffers[bj].shape) * 4
            row_out.append(unpack_values("f32", blob[off : off + n]))
            off += _pad8(n)
        inputs.append(row_in)
        outputs.append(row_out)
    return Probe(inputs, outputs)


# ---- records ----------------------------------------------------------------

HEADER_FMT = (
    "<8s I I Q 32s Q I I f f Q 32s 32s Q 32s Q Q Q Q 16s Q B 3s f Q I I Q Q"
)
SESSION_FMT = "<16s 32s"
RANGE_SUMMARY_FMT = "<5Q"
SECTION_FMT = "<16s Q Q"
OP_FMT = "<16s I I I I 4I I I 16I 2I 8d"
TENSOR_FMT = "<32s I I 4I Q Q Q"
BUFFER_FMT = "<32s I I 4I 2I"
BINDING_FMT = "<I I 56s"
PROBE_FMT = "<I I Q Q Q"
RANGE_FMT = "<d d"
LINEAGE_FMT = "<16s 32s 5Q Q"

assert struct.calcsize(HEADER_FMT) == 288
assert struct.calcsize(SECTION_FMT) == 32
assert struct.calcsize(OP_FMT) == 192
assert struct.calcsize(TENSOR_FMT) == 80
assert struct.calcsize(BUFFER_FMT) == 64
assert struct.calcsize(BINDING_FMT) == 64
assert struct.calcsize(PROBE_FMT) == 32
assert struct.calcsize(RANGE_FMT) == 16
assert struct.calcsize(LINEAGE_FMT) == 96


def _shape4(shape: Sequence[int]) -> List[int]:
    if not 1 <= len(shape) <= MAX_DIMS:
        raise ValueError(f"shape {shape} has {len(shape)} dims, 1 to {MAX_DIMS} allowed")
    return list(shape) + [0] * (MAX_DIMS - len(shape))


def _pack_header(h: Header, arch: int, file_size: int, n_sections: int, tensor_off: int) -> bytes:
    if len(h.lineage) > MAX_LINEAGE:
        raise ValueError("the header holds at most 8 lineage entries; the rest go in the section")
    fixed = struct.pack(
        HEADER_FMT,
        MAGIC,
        FORMAT_VERSION,
        HEADER_SIZE,
        arch,
        _cstr(h.name, 32),
        h.tick_period_ns,
        h.warmup_ticks,
        len(h.lineage),
        h.probe_rel_tol,
        h.probe_abs_tol,
        h.created_ns,
        _cstr(h.trainer_program, 32),
        _cstr(h.trainer_version, 32),
        h.trainer_settings_hash,
        _cstr(h.dataset, 32),
        h.dataset_hash,
        h.rows,
        h.steps,
        h.parent_hash,
        bytes(h.newest_row_boot).ljust(16, b"\0"),
        h.newest_row_stamp_ns,
        h.autopilot_mode,
        b"\0\0\0",
        h.activation_bound,
        file_size,
        n_sections,
        HEADER_SIZE,
        tensor_off,
        0,
    )
    sessions = b""
    ranges = b""
    for i in range(MAX_LINEAGE):
        ln = h.lineage[i] if i < len(h.lineage) else Lineage()
        sessions += struct.pack(SESSION_FMT, bytes(ln.boot).ljust(16, b"\0"), _cstr(ln.session, 32))
        ranges += struct.pack(
            RANGE_SUMMARY_FMT, ln.first_seq, ln.last_seq, ln.first_stamp_ns, ln.last_stamp_ns, ln.rows
        )
    out = fixed + sessions + ranges
    assert len(out) == 992
    return out + b"\0" * (HEADER_SIZE - len(out))


def _unpack_header(data: bytes) -> Header:
    f = struct.unpack(HEADER_FMT, data[:288])
    if f[0] != MAGIC:
        raise ValueError("magic is not HGWEIGHT")
    h = Header(
        name=_rcstr(f[4]),
        tick_period_ns=f[5],
        warmup_ticks=f[6],
        probe_rel_tol=f[8],
        probe_abs_tol=f[9],
        created_ns=f[10],
        trainer_program=_rcstr(f[11]),
        trainer_version=_rcstr(f[12]),
        trainer_settings_hash=f[13],
        dataset=_rcstr(f[14]),
        dataset_hash=f[15],
        rows=f[16],
        steps=f[17],
        parent_hash=f[18],
        newest_row_boot=f[19],
        newest_row_stamp_ns=f[20],
        autopilot_mode=f[21],
        activation_bound=f[23],
        arch_hash=f[3],
        format_version=f[1],
    )
    count = f[7]
    for i in range(count):
        s = struct.unpack(SESSION_FMT, data[288 + 48 * i : 288 + 48 * (i + 1)])
        r = struct.unpack(RANGE_SUMMARY_FMT, data[672 + 40 * i : 672 + 40 * (i + 1)])
        h.lineage.append(Lineage(s[0], _rcstr(s[1]), r[0], r[1], r[2], r[3], r[4]))
    return h


def _pack_op(op: Op) -> bytes:
    if len(op.inputs) > MAX_OP_INPUTS or len(op.tensors) > MAX_OP_TENSORS or len(op.attrs) > MAX_ATTRS:
        raise ValueError(f"op {op.type}: too many inputs, tensors or attrs")
    return struct.pack(
        OP_FMT,
        _cstr(op.type, 16),
        op.version,
        len(op.inputs),
        len(op.tensors),
        len(op.attrs),
        *(list(op.inputs) + [0] * (MAX_OP_INPUTS - len(op.inputs))),
        op.output,
        0,
        *(list(op.tensors) + [0] * (MAX_OP_TENSORS - len(op.tensors))),
        0,
        0,
        *([float(a) for a in op.attrs] + [0.0] * (MAX_ATTRS - len(op.attrs))),
    )


def _unpack_op(rec: bytes) -> Op:
    f = struct.unpack(OP_FMT, rec)
    n_in, n_t, n_a = f[2], f[3], f[4]
    return Op(
        type=_rcstr(f[0]),
        version=f[1],
        attrs=list(f[29 : 29 + n_a]),
        inputs=list(f[5 : 5 + n_in]),
        output=f[9],
        tensors=list(f[11 : 11 + n_t]),
    )


# ---- write / read -----------------------------------------------------------


def build(
    graph: Graph,
    header: Header,
    probes: Sequence[Probe] = (),
    ranges: Optional[Sequence[Tuple[float, float]]] = None,
    lineage: Sequence[Lineage] = (),
) -> bytes:
    """The whole file as bytes. `lineage` fills the optional lineage section;
    the header's own lineage list is the summary (first 8 entries)."""
    g = graph
    for op in g.ops:
        if op.type not in ("dense", "lstm", "affine", "cast", "relu", "tanh", "sigmoid"):
            pass  # the writer writes any op; the runtime refuses what it lacks
    stride = probe_stride(g)

    # Section payloads.
    sections: List[Tuple[str, bytes]] = []
    sections.append(("ops", b"".join(_pack_op(op) for op in g.ops)))
    # tensors: offsets are filled once the data region is laid out.
    sections.append(("tensors", b""))
    sections.append(
        (
            "buffers",
            b"".join(
                struct.pack(BUFFER_FMT, _cstr(b.name, 32), dtype_code(b.dtype), len(b.shape), *_shape4(b.shape), 0, 0)
                for b in g.buffers
            ),
        )
    )
    sections.append(
        ("inputs", b"".join(struct.pack(BINDING_FMT, b.buffer, b.element, _cstr(b.field, 56)) for b in g.inputs))
    )
    sections.append(
        ("outputs", b"".join(struct.pack(BINDING_FMT, b.buffer, b.element, _cstr(b.field, 56)) for b in g.outputs))
    )
    if probes:
        sections.append(("probes", b""))
    if ranges is not None:
        if len(ranges) != len(g.inputs):
            raise ValueError("ranges: one (lo, hi) per input binding")
        sections.append(("ranges", b"".join(struct.pack(RANGE_FMT, float(lo), float(hi)) for lo, hi in ranges)))
    if lineage:
        sections.append(
            (
                "lineage",
                b"".join(
                    struct.pack(
                        LINEAGE_FMT,
                        bytes(ln.boot).ljust(16, b"\0"),
                        _cstr(ln.session, 32),
                        ln.first_seq,
                        ln.last_seq,
                        ln.first_stamp_ns,
                        ln.last_stamp_ns,
                        ln.rows,
                        0,
                    )
                    for ln in lineage
                ),
            )
        )

    # Lay the sections out: directory, then each section 64-aligned.
    n_tensor_recs = len(g.tensors)
    n_probe_recs = len(probes)
    sizes = {
        "tensors": n_tensor_recs * struct.calcsize(TENSOR_FMT),
        "probes": n_probe_recs * struct.calcsize(PROBE_FMT),
    }
    off = HEADER_SIZE + len(sections) * struct.calcsize(SECTION_FMT)
    layout: List[Tuple[str, int, int]] = []
    for name, payload in sections:
        off = _align(off)
        size = sizes.get(name, len(payload))
        layout.append((name, off, size))
        off += size
    tensor_off = _align(off)

    # Tensor data, then probe blobs, each 64-aligned.
    data = bytearray()
    tensor_recs = []
    for t in g.tensors:
        raw = pack_values(t.dtype, t.data)
        pos = tensor_off + len(data)
        tensor_recs.append(
            struct.pack(TENSOR_FMT, _cstr(t.name, 32), dtype_code(t.dtype), len(t.shape), *_shape4(t.shape), pos, len(raw), 0)
        )
        data += raw + b"\0" * (_align(len(raw)) - len(raw))
    probe_recs = []
    for p in probes:
        blob = _probe_blob(g, p)
        pos = tensor_off + len(data)
        probe_recs.append(struct.pack(PROBE_FMT, p.steps, 0, pos, len(blob), 0))
        data += blob + b"\0" * (_align(len(blob)) - len(blob))
        assert len(blob) == p.steps * stride
    file_size = tensor_off + len(data)

    out = bytearray(file_size)
    out[:HEADER_SIZE] = _pack_header(header, arch_hash(g), file_size, len(sections), tensor_off)
    dpos = HEADER_SIZE
    for (name, soff, size), (_, payload) in zip(layout, sections):
        out[dpos : dpos + 32] = struct.pack(SECTION_FMT, _cstr(name, 16), soff, size)
        dpos += 32
        if name == "tensors":
            payload = b"".join(tensor_recs)
        elif name == "probes":
            payload = b"".join(probe_recs)
        assert len(payload) == size
        out[soff : soff + size] = payload
    out[tensor_off:] = data
    return bytes(out)


def write(path: str, graph: Graph, header: Header, probes: Sequence[Probe] = (),
          ranges=None, lineage: Sequence[Lineage] = ()) -> bytes:
    data = build(graph, header, probes, ranges, lineage)
    with open(path, "wb") as f:
        f.write(data)
    return data


def parse(data: bytes) -> File:
    if len(data) < HEADER_SIZE:
        raise ValueError("shorter than the header")
    header = _unpack_header(data)
    if header.format_version != FORMAT_VERSION:
        raise ValueError(f"format version {header.format_version}, this reader reads {FORMAT_VERSION}")
    (_, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, _, file_size, n_sections, dir_off, tensor_off, _) = struct.unpack(HEADER_FMT, data[:288])
    if file_size != len(data):
        raise ValueError(f"header says {file_size} bytes, data has {len(data)}")
    secs: Dict[str, Tuple[int, int]] = {}
    order = []
    for i in range(n_sections):
        name, off, size = struct.unpack(SECTION_FMT, data[dir_off + 32 * i : dir_off + 32 * (i + 1)])
        name = _rcstr(name)
        secs[name] = (off, size)
        order.append((name, off, size))

    def records(name: str, fmt: str):
        if name not in secs:
            return []
        off, size = secs[name]
        n = struct.calcsize(fmt)
        if size % n:
            raise ValueError(f"section {name}: {size} bytes is not whole {n}-byte records")
        return [struct.unpack(fmt, data[off + k * n : off + (k + 1) * n]) for k in range(size // n)]

    buffers = [Buffer(_rcstr(r[0]), DTYPE_BY_CODE[r[1]], list(r[3 : 3 + r[2]])) for r in records("buffers", BUFFER_FMT)]
    tensors = []
    for r in records("tensors", TENSOR_FMT):
        dtype = DTYPE_BY_CODE[r[1]]
        shape = list(r[3 : 3 + r[2]])
        toff, tsize = r[7], r[8]
        tensors.append(Tensor(_rcstr(r[0]), dtype, shape, unpack_values(dtype, data[toff : toff + tsize])))
    off, size = secs["ops"]
    ops = [_unpack_op(data[off + k * 192 : off + (k + 1) * 192]) for k in range(size // 192)]
    inputs = [Binding(r[0], r[1], _rcstr(r[2])) for r in records("inputs", BINDING_FMT)]
    outputs = [Binding(r[0], r[1], _rcstr(r[2])) for r in records("outputs", BINDING_FMT)]
    g = Graph(buffers, tensors, ops, inputs, outputs)
    probes = [_probe_from_blob(g, data[r[2] : r[2] + r[3]], r[0]) for r in records("probes", PROBE_FMT)]
    ranges = [(r[0], r[1]) for r in records("ranges", RANGE_FMT)] if "ranges" in secs else None
    return File(header, g, probes, ranges, order)


def read(path: str) -> File:
    with open(path, "rb") as f:
        return parse(f.read())
