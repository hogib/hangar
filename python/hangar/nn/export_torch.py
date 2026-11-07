# SPDX-License-Identifier: LGPL-3.0-or-later
"""The PyTorch exporter: a sequence of nn.Linear, nn.LSTM, nn.ReLU, nn.Tanh,
nn.Sigmoid and Affine (normalization) modules to an hgw.Graph, and to a
.hgw file with probes computed by the reference.

Imports without torch. Modules are recognized by class name and read
through duck typing (`.weight`, `.bias`, `.weight_ih_l{k}`, ...; a tensor
is anything with .detach().cpu().double().reshape(-1).tolist(), or a plain
nested list), so the graph-building path is testable with stand-in objects.
The real-torch path follows the same code but has not been run here: torch
is not installed on this machine.

    from hangar.nn import export_torch
    graph = export_torch.to_graph([norm, lstm, head, scale], input_fields, output_fields)
    export_torch.export(model_or_layers, "depth_ff.hgw", input_fields, output_fields,
                        probe_inputs=[...], header=hgw.Header(name="depth_ff", ...))
"""

from __future__ import annotations

from typing import Any, List, Optional, Sequence

from . import hgw, reference

try:  # pragma: no cover - torch is optional
    import torch  # type: ignore
    import torch.nn as _tnn  # type: ignore

    class Affine(_tnn.Module):
        """Per-channel y = x * scale + shift, as a module, so that input
        normalization and output scaling travel with the weights."""

        def __init__(self, scale, shift):
            super().__init__()
            self.scale = _tnn.Parameter(torch.as_tensor(scale, dtype=torch.float32), requires_grad=False)
            self.shift = _tnn.Parameter(torch.as_tensor(shift, dtype=torch.float32), requires_grad=False)

        def forward(self, x):
            return x * self.scale + self.shift

except ImportError:  # pragma: no cover
    torch = None
    Affine = None  # type: ignore


class ExportError(ValueError):
    pass


def _to_list(t: Any) -> List[float]:
    """A tensor (torch or stand-in) or nested list, flattened to Python floats."""
    if hasattr(t, "detach"):
        return [float(v) for v in t.detach().cpu().double().reshape(-1).tolist()]
    out: List[float] = []

    def walk(v):
        if isinstance(v, (list, tuple)):
            for w in v:
                walk(w)
        else:
            out.append(float(v))

    walk(t)
    return out


def _f32_round(values: Sequence[float]) -> List[float]:
    import struct

    return [struct.unpack("<f", struct.pack("<f", v))[0] for v in values]


def _layers(model: Any) -> List[Any]:
    if isinstance(model, (list, tuple)):
        return list(model)
    if hasattr(model, "children"):
        ch = list(model.children())
        if ch:
            return ch
    return [model]


def to_graph(
    model: Any,
    input_fields: Sequence[str],
    output_fields: Sequence[str],
    input_dtype: str = "f32",
    cast_scale: float = 1.0,
) -> hgw.Graph:
    """Builds the graph: an input buffer bound to `input_fields`, one op per
    layer in order, the last buffer bound to `output_fields`. With an
    `input_dtype` other than f32, a cast op (× cast_scale) comes first."""
    layers = _layers(model)
    if not layers:
        raise ExportError("no layers")
    buffers: List[hgw.Buffer] = [hgw.Buffer("in", input_dtype, [len(input_fields)])]
    tensors: List[hgw.Tensor] = []
    ops: List[hgw.Op] = []
    cur = 0
    cur_shape = [len(input_fields)]

    def add_buffer(name, shape):
        buffers.append(hgw.Buffer(name, "f32", list(shape)))
        return len(buffers) - 1

    def add_tensor(name, shape, data):
        tensors.append(hgw.Tensor(name, "f32", list(shape), _f32_round(data)))
        return len(tensors) - 1

    if input_dtype != "f32":
        if input_dtype not in hgw.CASTABLE:
            raise ExportError(f"cannot cast from {input_dtype}")
        nxt = add_buffer("in_f32", cur_shape)
        ops.append(hgw.Op("cast", 1, [hgw.dtype_code(input_dtype), float(cast_scale)], [cur], nxt))
        cur = nxt

    for li, layer in enumerate(layers):
        kind = type(layer).__name__
        if kind == "Linear":
            W = _to_list(layer.weight)
            out_f, in_f = int(layer.out_features), int(layer.in_features)
            if cur_shape != [in_f]:
                raise ExportError(f"layer {li} Linear: in_features {in_f}, input is {cur_shape}")
            ti = [add_tensor(f"l{li}.W", [out_f, in_f], W)]
            bias = getattr(layer, "bias", None) is not None
            if bias:
                ti.append(add_tensor(f"l{li}.b", [out_f], _to_list(layer.bias)))
            nxt = add_buffer(f"l{li}", [out_f])
            ops.append(hgw.Op("dense", 1, [in_f, out_f, 1 if bias else 0], [cur], nxt, ti))
            cur, cur_shape = nxt, [out_f]
        elif kind == "LSTM":
            if getattr(layer, "bidirectional", False):
                raise ExportError(f"layer {li} LSTM: bidirectional=True is not supported")
            if int(getattr(layer, "proj_size", 0) or 0) > 0:
                raise ExportError(f"layer {li} LSTM: proj_size > 0 is not supported")
            if not getattr(layer, "bias", True):
                raise ExportError(f"layer {li} LSTM: bias=False is not supported (v1 needs b_ih and b_hh)")
            I, H, L = int(layer.input_size), int(layer.hidden_size), int(layer.num_layers)
            if cur_shape != [I]:
                raise ExportError(f"layer {li} LSTM: input_size {I}, input is {cur_shape}")
            ti = []
            for k in range(L):
                Ik = I if k == 0 else H
                ti.append(add_tensor(f"l{li}.W_ih_l{k}", [4 * H, Ik], _to_list(getattr(layer, f"weight_ih_l{k}"))))
                ti.append(add_tensor(f"l{li}.W_hh_l{k}", [4 * H, H], _to_list(getattr(layer, f"weight_hh_l{k}"))))
                ti.append(add_tensor(f"l{li}.b_ih_l{k}", [4 * H], _to_list(getattr(layer, f"bias_ih_l{k}"))))
                ti.append(add_tensor(f"l{li}.b_hh_l{k}", [4 * H], _to_list(getattr(layer, f"bias_hh_l{k}"))))
            nxt = add_buffer(f"l{li}", [H])
            ops.append(hgw.Op("lstm", 1, [I, H, L], [cur], nxt, ti))
            cur, cur_shape = nxt, [H]
        elif kind in ("ReLU", "Tanh", "Sigmoid"):
            nxt = add_buffer(f"l{li}", cur_shape)
            ops.append(hgw.Op(kind.lower(), 1, [], [cur], nxt))
            cur = nxt
        elif kind == "Affine":
            scale, shift = _to_list(layer.scale), _to_list(layer.shift)
            C = cur_shape[0]
            if len(scale) != C or len(shift) != C:
                raise ExportError(f"layer {li} Affine: {len(scale)} channels for input {cur_shape}")
            ti = [add_tensor(f"l{li}.scale", [C], scale), add_tensor(f"l{li}.shift", [C], shift)]
            nxt = add_buffer(f"l{li}", cur_shape)
            ops.append(hgw.Op("affine", 1, [], [cur], nxt, ti))
            cur = nxt
        elif kind == "BatchNorm1d":
            # eval-mode batchnorm folded to an affine
            C = cur_shape[0]
            mean, var = _to_list(layer.running_mean), _to_list(layer.running_var)
            eps = float(layer.eps)
            gamma = _to_list(layer.weight) if getattr(layer, "weight", None) is not None else [1.0] * C
            beta = _to_list(layer.bias) if getattr(layer, "bias", None) is not None else [0.0] * C
            scale = [g / (v + eps) ** 0.5 for g, v in zip(gamma, var)]
            shift = [b - m * s for b, m, s in zip(beta, mean, scale)]
            ti = [add_tensor(f"l{li}.scale", [C], scale), add_tensor(f"l{li}.shift", [C], shift)]
            nxt = add_buffer(f"l{li}", cur_shape)
            ops.append(hgw.Op("affine", 1, [], [cur], nxt, ti))
            cur = nxt
        elif kind in ("Dropout", "Identity", "Flatten"):
            continue  # no effect at inference on a vector
        else:
            raise ExportError(f"layer {li}: {kind} has no mapping in version 1")

    if hgw.numel(cur_shape) != len(output_fields):
        raise ExportError(f"the model ends with {cur_shape} values, {len(output_fields)} output fields given")
    inputs = [hgw.Binding(0, e, f) for e, f in enumerate(input_fields)]
    outputs = [hgw.Binding(cur, e, f) for e, f in enumerate(output_fields)]
    return hgw.Graph(buffers, tensors, ops, inputs, outputs)


def check_runtime(graph: hgw.Graph, runtime_ops: Optional[Sequence[tuple]] = None) -> None:
    """Refuses a graph using an op the vehicle lacks. `runtime_ops` is the
    list of (type, version) from policy_<name>.runtime_info; None means
    this reference's own set."""
    have = set(runtime_ops) if runtime_ops is not None else reference.SUPPORTED
    for op in graph.ops:
        if (op.type, op.version) not in have:
            raise ExportError(f"op {op.type} v{op.version} not supported by the runtime")


def export(
    model: Any,
    path: str,
    input_fields: Sequence[str],
    output_fields: Sequence[str],
    probe_inputs: Sequence[Sequence[Sequence[float]]] = (),
    header: Optional[hgw.Header] = None,
    input_dtype: str = "f32",
    cast_scale: float = 1.0,
    ranges=None,
    lineage: Sequence[hgw.Lineage] = (),
    runtime_ops=None,
) -> hgw.Graph:
    """Graph, check against the runtime, probes from the reference (one
    probe per sequence in probe_inputs; each step is a list of the input
    values), and the file."""
    graph = to_graph(model, input_fields, output_fields, input_dtype, cast_scale)
    check_runtime(graph, runtime_ops)
    probes = [reference.make_probe(graph, [[list(step)] for step in seq]) for seq in probe_inputs]
    hgw.write(path, graph, header or hgw.Header(), probes, ranges, lineage)
    return graph
