# SPDX-License-Identifier: LGPL-3.0-or-later
"""A pure-Python, double-precision reference of lib/nn's ops.

Same graph semantics as lib/nn/nn_graph.c, same op semantics as
lib/nn/nn_ops.c, in Python floats (binary64). lib/nn computes in f32 and
differs in the last bits; the golden tests compare within a tolerance.
Numpy is deliberately not used, even when importable: the golden vectors
must be the same bytes on every machine, and numpy's summation order
differs from a plain loop's.
"""

from __future__ import annotations

import math
from typing import Dict, List, Sequence

from . import hgw

SUPPORTED = {
    ("dense", 1),
    ("lstm", 1),
    ("affine", 1),
    ("cast", 1),
    ("relu", 1),
    ("tanh", 1),
    ("sigmoid", 1),
}


class UnsupportedOp(ValueError):
    pass


def sigmoid(x: float) -> float:
    if x >= 0:
        return 1.0 / (1.0 + math.exp(-x))
    e = math.exp(x)
    return e / (1.0 + e)


def matvec(W: Sequence[float], x: Sequence[float], rows: int, cols: int, bias=None) -> List[float]:
    out = []
    for o in range(rows):
        acc = bias[o] if bias is not None else 0.0
        base = o * cols
        for i in range(cols):
            acc += W[base + i] * x[i]
        out.append(acc)
    return out


def check_graph(g: hgw.Graph) -> Dict[int, List[int]]:
    """Shape-checks the graph the way lib/nn does, returning each op's output
    shape by op index. Raises ValueError (UnsupportedOp for an unknown op)."""
    shapes = {}
    for i, op in enumerate(g.ops):
        if (op.type, op.version) not in SUPPORTED:
            raise UnsupportedOp(f"op {op.type} v{op.version} not supported")
        ins = [g.buffers[b] for b in op.inputs]
        ts = [g.tensors[t] for t in op.tensors]
        out = g.buffers[op.output]
        a = op.attrs
        if op.type == "dense":
            n_in, n_out, bias = int(a[0]), int(a[1]), int(a[2])
            if hgw.numel(ins[0].shape) != n_in or ins[0].dtype != "f32":
                raise ValueError(f"op {i} dense: input shape")
            if ts[0].shape != [n_out, n_in] or (bias and ts[1].shape != [n_out]):
                raise ValueError(f"op {i} dense: tensor shapes")
            shape = [n_out]
        elif op.type == "lstm":
            I, H, L = int(a[0]), int(a[1]), int(a[2])
            if hgw.numel(ins[0].shape) != I or len(ts) != 4 * L:
                raise ValueError(f"op {i} lstm: input or tensor count")
            for k in range(L):
                Ik = I if k == 0 else H
                exp = [[4 * H, Ik], [4 * H, H], [4 * H], [4 * H]]
                if [t.shape for t in ts[4 * k : 4 * k + 4]] != exp:
                    raise ValueError(f"op {i} lstm layer {k}: tensor shapes")
            shape = [H]
        elif op.type == "affine":
            C = ins[0].shape[0]
            if ts[0].shape != [C] or ts[1].shape != [C]:
                raise ValueError(f"op {i} affine: scale/shift shape")
            shape = list(ins[0].shape)
        elif op.type == "cast":
            if hgw.DTYPE_BY_CODE[int(a[0])] != ins[0].dtype or ins[0].dtype not in hgw.CASTABLE:
                raise ValueError(f"op {i} cast: input dtype")
            shape = list(ins[0].shape)
        else:
            if ins[0].dtype != "f32":
                raise ValueError(f"op {i} {op.type}: input must be f32")
            shape = list(ins[0].shape)
        if out.shape != shape or out.dtype != "f32":
            raise ValueError(f"op {i} {op.type}: output buffer {out.name} shape {out.shape}, op gives {shape}")
        shapes[i] = shape
    return shapes


class Model:
    """Steps a Graph one tick at a time. `state[i]` is op i's recurrent
    state (h[L][H] + c[L][H] for an lstm, [] otherwise), as lib/nn lays it out."""

    def __init__(self, g: hgw.Graph):
        check_graph(g)
        self.g = g
        self.values: Dict[int, List[float]] = {}
        self.state: Dict[int, List[float]] = {}
        self.reset()

    def reset(self):
        for i, op in enumerate(self.g.ops):
            if op.type == "lstm":
                H, L = int(op.attrs[1]), int(op.attrs[2])
                self.state[i] = [0.0] * (2 * L * H)
            else:
                self.state[i] = []

    def step(self, inputs: Sequence[Sequence[float]]) -> List[List[float]]:
        g = self.g
        for bi, vals in zip(g.input_buffers(), inputs):
            b = g.buffers[bi]
            if len(vals) != hgw.numel(b.shape):
                raise ValueError(f"input {b.name}: {len(vals)} values for shape {b.shape}")
            self.values[bi] = [float(v) for v in vals]
        for i, op in enumerate(g.ops):
            x = self.values[op.inputs[0]]
            ts = [g.tensors[t].data for t in op.tensors]
            a = op.attrs
            if op.type == "dense":
                n_in, n_out, bias = int(a[0]), int(a[1]), int(a[2])
                y = matvec(ts[0], x, n_out, n_in, ts[1] if bias else None)
            elif op.type == "lstm":
                y = self._lstm(i, x, ts, int(a[0]), int(a[1]), int(a[2]))
            elif op.type == "affine":
                C = g.buffers[op.inputs[0]].shape[0]
                per = len(x) // C
                y = [x[c * per + k] * ts[0][c] + ts[1][c] for c in range(C) for k in range(per)]
            elif op.type == "cast":
                y = [float(v) * a[1] for v in x]
            elif op.type == "relu":
                y = [v if v > 0.0 else 0.0 for v in x]
            elif op.type == "tanh":
                y = [math.tanh(v) for v in x]
            elif op.type == "sigmoid":
                y = [sigmoid(v) for v in x]
            else:
                raise UnsupportedOp(f"op {op.type} v{op.version} not supported")
            self.values[op.output] = y
        return [list(self.values[bi]) for bi in g.output_buffers()]

    def _lstm(self, i, x, ts, I, H, L):
        st = self.state[i]
        for k in range(L):
            W_ih, W_hh, b_ih, b_hh = ts[4 * k : 4 * k + 4]
            Ik = I if k == 0 else H
            h = st[k * H : (k + 1) * H]
            c = st[L * H + k * H : L * H + (k + 1) * H]
            gates = []
            for j in range(4 * H):
                acc = b_ih[j] + b_hh[j]
                for q in range(Ik):
                    acc += W_ih[j * Ik + q] * x[q]
                for q in range(H):
                    acc += W_hh[j * H + q] * h[q]
                gates.append(acc)
            new_h, new_c = [], []
            for j in range(H):
                ig = sigmoid(gates[j])
                fg = sigmoid(gates[H + j])
                gg = math.tanh(gates[2 * H + j])
                og = sigmoid(gates[3 * H + j])
                cn = fg * c[j] + ig * gg
                new_c.append(cn)
                new_h.append(og * math.tanh(cn))
            st[k * H : (k + 1) * H] = new_h
            st[L * H + k * H : L * H + (k + 1) * H] = new_c
            x = new_h
        return list(st[(L - 1) * H : L * H])

    def run(self, steps: Sequence[Sequence[Sequence[float]]], reset: bool = True) -> List[List[List[float]]]:
        if reset:
            self.reset()
        return [self.step(s) for s in steps]


def make_probe(g: hgw.Graph, inputs: Sequence[Sequence[Sequence[float]]]) -> hgw.Probe:
    """A Probe from zero state: `inputs[s][i]` per step and model input, the
    expected outputs computed by this reference."""
    m = Model(g)
    outputs = m.run(inputs)
    return hgw.Probe([[list(v) for v in s] for s in inputs], outputs)
