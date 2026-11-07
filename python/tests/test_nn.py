# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar.nn: the .hgw writer/reader, the reference, the exporter, and the
golden vectors lib/nn/tests/test_nn.c reads from lib/nn/tests/vectors/.

    python3 -m unittest python/tests/test_nn.py              (from the repository root)
    HANGAR_NN_WRITE_VECTORS=1 python3 -m unittest python/tests/test_nn.py

The vectors are generated deterministically (seeded, pure Python, no numpy)
and checked in; the test regenerates them and compares: everything exact
except the floats that pass through libm (within a tolerance, since tanh and
exp differ by platform in the last ulp), or writes them when the environment
variable is set or a file is missing. HANGAR_NN_VECTORS points at another
vectors directory.
"""

import hashlib
import math
import os
import random
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
REPO = os.path.dirname(os.path.dirname(HERE))
VECTORS = os.environ.get("HANGAR_NN_VECTORS") or os.path.join(REPO, "lib", "nn", "tests", "vectors")

from hangar.nn import export_torch, hgw, reference  # noqa: E402
from hangar.nn.hgw import Binding, Buffer, Graph, Header, Op, Tensor  # noqa: E402

CREATED_NS = 1_762_500_000_000_000_000  # fixed, so the files are reproducible
TOL = dict(probe_rel_tol=1e-5, probe_abs_tol=1e-6)


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def f32s(vs):
    return [f32(v) for v in vs]


def uniform(rng, n, scale):
    return f32s(rng.uniform(-scale, scale) for _ in range(n))


# ---- the golden models --------------------------------------------------------


def dense_relu():
    rng = random.Random(1)
    g = Graph(
        buffers=[Buffer("in", "f32", [5]), Buffer("h", "f32", [4]), Buffer("out", "f32", [4])],
        tensors=[Tensor("W", "f32", [4, 5], uniform(rng, 20, 0.8)), Tensor("b", "f32", [4], uniform(rng, 4, 0.5))],
        ops=[Op("dense", 1, [5, 4, 1], [0], 1, [0, 1]), Op("relu", 1, [], [1], 2)],
        inputs=[Binding(0, e, f"nav.state.{n}") for e, n in enumerate(["depth", "vel_z", "pitch", "roll", "accel_z"])],
        outputs=[Binding(2, e, f"wrench.{n}") for e, n in enumerate(["surge", "sway", "heave", "yaw"])],
    )
    steps = [[uniform(rng, 5, 2.0)] for _ in range(4)]
    return g, steps, Header(name="dense_relu", tick_period_ns=20_000_000, created_ns=CREATED_NS, **TOL)


def lstm_tensors(rng, I, H, L, prefix="l"):
    ts = []
    for k in range(L):
        Ik = I if k == 0 else H
        s = 1.0 / math.sqrt(H)
        ts += [
            Tensor(f"{prefix}.W_ih_l{k}", "f32", [4 * H, Ik], uniform(rng, 4 * H * Ik, s)),
            Tensor(f"{prefix}.W_hh_l{k}", "f32", [4 * H, H], uniform(rng, 4 * H * H, s)),
            Tensor(f"{prefix}.b_ih_l{k}", "f32", [4 * H], uniform(rng, 4 * H, s)),
            Tensor(f"{prefix}.b_hh_l{k}", "f32", [4 * H], uniform(rng, 4 * H, s)),
        ]
    return ts


def lstm1():
    rng = random.Random(2)
    I, H = 3, 4
    g = Graph(
        buffers=[Buffer("in", "f32", [I]), Buffer("out", "f32", [H])],
        tensors=lstm_tensors(rng, I, H, 1),
        ops=[Op("lstm", 1, [I, H, 1], [0], 1, [0, 1, 2, 3])],
        inputs=[Binding(0, e, f"x{e}") for e in range(I)],
        outputs=[Binding(1, e, f"y{e}") for e in range(H)],
    )
    steps = [[uniform(rng, I, 1.5)] for _ in range(8)]
    return g, steps, Header(name="lstm1", tick_period_ns=20_000_000, warmup_ticks=8, created_ns=CREATED_NS, **TOL)


def lstm2():
    rng = random.Random(3)
    I, H = 8, 16
    g = Graph(
        buffers=[Buffer("in", "f32", [I]), Buffer("out", "f32", [H])],
        tensors=lstm_tensors(rng, I, H, 2),
        ops=[Op("lstm", 1, [I, H, 2], [0], 1, list(range(8)))],
        inputs=[Binding(0, e, f"x{e}") for e in range(I)],
        outputs=[Binding(1, e, f"y{e}") for e in range(H)],
    )
    steps = [[uniform(rng, I, 1.5)] for _ in range(8)]
    return g, steps, Header(name="lstm2", tick_period_ns=20_000_000, warmup_ticks=8, created_ns=CREATED_NS, **TOL)


def affine_cast():
    rng = random.Random(4)
    g = Graph(
        buffers=[Buffer("raw", "u16", [3]), Buffer("f", "f32", [3]), Buffer("out", "f32", [3])],
        tensors=[Tensor("scale", "f32", [3], f32s([0.5, -2.0, 1.25])), Tensor("shift", "f32", [3], f32s([-1.0, 0.25, 3.0]))],
        ops=[Op("cast", 1, [hgw.dtype_code("u16"), 0.001], [0], 1), Op("affine", 1, [], [1], 2, [0, 1])],
        inputs=[Binding(0, e, f"raw.{e}") for e in range(3)],
        outputs=[Binding(2, e, f"out.{e}") for e in range(3)],
    )
    steps = [[[rng.randrange(0, 65536) for _ in range(3)]] for _ in range(4)]
    steps.append([[0, 65535, 32768]])
    return g, steps, Header(name="affine_cast", tick_period_ns=20_000_000, created_ns=CREATED_NS, **TOL)


INT_RANGES = {"u8": (0, 255), "i8": (-128, 127), "u16": (0, 65535), "i16": (-32768, 32767),
              "u32": (0, 2**32 - 1), "i32": (-(2**31), 2**31 - 1)}


def casts():
    """Every castable type, one cast op each, seven model inputs and outputs."""
    rng = random.Random(5)
    dtypes = ["u8", "i8", "u16", "i16", "u32", "i32", "f64"]
    scales = [1 / 255, 1 / 128, 1 / 65535, 1 / 32768, 2.0 ** -32, 2.0 ** -31, 1.0]
    buffers, ops, inputs, outputs = [], [], [], []
    for i, dt in enumerate(dtypes):
        buffers.append(Buffer(f"raw_{dt}", dt, [2]))
    for i, dt in enumerate(dtypes):
        buffers.append(Buffer(f"f_{dt}", "f32", [2]))
        ops.append(Op("cast", 1, [hgw.dtype_code(dt), scales[i]], [i], 7 + i))
        inputs += [Binding(i, e, f"{dt}.{e}") for e in range(2)]
        outputs += [Binding(7 + i, e, f"{dt}_f32.{e}") for e in range(2)]
    g = Graph(buffers, [], ops, inputs, outputs)
    steps = []
    for s in range(3):
        row = []
        for dt in dtypes:
            if dt == "f64":
                row.append([rng.uniform(-1e6, 1e6), rng.uniform(-1, 1)])
            else:
                lo, hi = INT_RANGES[dt]
                row.append([rng.randint(lo, hi), [lo, hi, 0][s]])
        steps.append(row)
    return g, steps, Header(name="casts", tick_period_ns=20_000_000, created_ns=CREATED_NS, **TOL)


def acts():
    rng = random.Random(6)
    g = Graph(
        buffers=[Buffer("in", "f32", [6]), Buffer("t", "f32", [6]), Buffer("out", "f32", [6])],
        tensors=[],
        ops=[Op("tanh", 1, [], [0], 1), Op("sigmoid", 1, [], [1], 2)],
        inputs=[Binding(0, e, f"x{e}") for e in range(6)],
        outputs=[Binding(2, e, f"y{e}") for e in range(6)],
    )
    steps = [[uniform(rng, 6, 4.0)] for _ in range(3)]
    return g, steps, Header(name="acts", tick_period_ns=20_000_000, created_ns=CREATED_NS, **TOL)


def depth_ff():
    """docs/09's adaptive depth hold: 5 inputs -> affine (normalize) -> dense 16
    -> lstm 16 -> dense 1 -> affine (scale), output bound to the heave field."""
    rng = random.Random(7)
    I, D, H = 5, 16, 16
    tensors = [
        Tensor("norm.scale", "f32", [I], f32s([0.1, 2.0, 3.0, 3.0, 0.5])),
        Tensor("norm.shift", "f32", [I], f32s([-1.0, 0.0, 0.0, 0.0, 0.0])),
        Tensor("fc1.W", "f32", [D, I], uniform(rng, D * I, 1 / math.sqrt(I))),
        Tensor("fc1.b", "f32", [D], uniform(rng, D, 0.1)),
    ]
    tensors += lstm_tensors(rng, D, H, 1, "rnn")
    tensors += [
        Tensor("head.W", "f32", [1, H], uniform(rng, H, 1 / math.sqrt(H))),
        Tensor("head.b", "f32", [1], uniform(rng, 1, 0.1)),
        Tensor("out.scale", "f32", [1], f32s([4.0])),
        Tensor("out.shift", "f32", [1], f32s([0.0])),
    ]
    g = Graph(
        buffers=[Buffer("in", "f32", [I]), Buffer("norm", "f32", [I]), Buffer("fc1", "f32", [D]),
                 Buffer("rnn", "f32", [H]), Buffer("head", "f32", [1]), Buffer("out", "f32", [1])],
        tensors=tensors,
        ops=[
            Op("affine", 1, [], [0], 1, [0, 1]),
            Op("dense", 1, [I, D, 1], [1], 2, [2, 3]),
            Op("lstm", 1, [D, H, 1], [2], 3, [4, 5, 6, 7]),
            Op("dense", 1, [H, 1, 1], [3], 4, [8, 9]),
            Op("affine", 1, [], [4], 5, [10, 11]),
        ],
        inputs=[Binding(0, e, f) for e, f in enumerate(
            ["nav.state.depth", "nav.state.vel_z", "nav.state.pitch", "nav.state.roll", "ctl.depth_hold.target.accel_z"])],
        outputs=[Binding(5, 0, "heave")],
    )
    steps = []
    depth = 10.0
    for s in range(32):
        depth += rng.uniform(-0.05, 0.05)
        steps.append([f32s([depth, rng.uniform(-0.1, 0.1), rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2), rng.uniform(-1, 1)])])
    hdr = Header(name="depth_ff", tick_period_ns=20_000_000, warmup_ticks=50, created_ns=CREATED_NS,
                 trainer_program="hangar.learn", trainer_version="0.1", trainer_settings_hash=0x1234,
                 dataset="depth_ff", dataset_hash=0xD0D0_CAFE_F00D_0001, rows=12345, steps=678,
                 activation_bound=1000.0,
                 lineage=[hgw.Lineage(bytes(range(16)), "20261107T120000", 100, 12444, 1_000_000, 2_000_000, 12345)],
                 **TOL)
    ranges = [(0.0, 50.0), (-1.0, 1.0), (-0.5, 0.5), (-0.5, 0.5), (-2.0, 2.0)]
    return g, steps, hdr, ranges


GOLDEN = {
    "dense_relu": dense_relu,
    "lstm1": lstm1,
    "lstm2": lstm2,
    "affine_cast": affine_cast,
    "casts": casts,
    "acts": acts,
    "depth_ff": depth_ff,
}

# Architecture hashes of the golden graphs. A change here is a change of the
# canonical form (or of a golden graph) and must be deliberate: it changes
# every policy spec's Arch.
ARCH = {
    "dense_relu": 0x0837305AF963EDA5,
    "lstm1": 0x138A13D67D6DB091,
    "lstm2": 0xD82F2FC9AF97274E,
    "affine_cast": 0xE1DBD299E48AB53F,
    "casts": 0xFB3E03D233747C42,
    "acts": 0x98D4CB2A9BA6C5AC,
    "depth_ff": 0xA64977B8EB7E2F3D,
}


def build_golden(name):
    """(file bytes, sidecar text) for one golden model."""
    spec = GOLDEN[name]()
    g, steps, hdr = spec[0], spec[1], spec[2]
    ranges = spec[3] if len(spec) > 3 else None
    probes = [reference.make_probe(g, steps)]
    if name == "depth_ff":  # a second, shorter probe
        probes.append(reference.make_probe(g, steps[:5]))
    data = hgw.build(g, hdr, probes, ranges)
    m = reference.Model(g)
    m.run(steps)
    state = []
    for i, op in enumerate(g.ops):
        state += m.state[i]
    lines = [
        f"blake2s {hashlib.blake2s(data).hexdigest()}",
        f"arch {hgw.arch_hash(g):016x}",
        f"steps {len(steps)}",
        f"state {len(state)} " + " ".join(repr(v) for v in state),
    ]
    return data, "\n".join(lines) + "\n"


# ---- tests ------------------------------------------------------------------------


class TestHgw(unittest.TestCase):
    def test_round_trip(self):
        for name, mk in GOLDEN.items():
            with self.subTest(name=name):
                spec = mk()
                g, steps, hdr = spec[0], spec[1], spec[2]
                ranges = spec[3] if len(spec) > 3 else None
                probe = reference.make_probe(g, steps)
                data = hgw.build(g, hdr, [probe], ranges)
                f = hgw.parse(data)
                self.assertEqual(f.graph, g)
                self.assertEqual(f.header.name, hdr.name)
                self.assertEqual(f.header.arch_hash, hgw.arch_hash(g))
                self.assertEqual(f.header.tick_period_ns, hdr.tick_period_ns)
                self.assertEqual(f.header.dataset_hash, hdr.dataset_hash)
                self.assertEqual(f.header.lineage, hdr.lineage)
                self.assertEqual(f.ranges, ranges)
                self.assertEqual(len(f.probes), 1)
                self.assertEqual(f.probes[0].inputs, probe.inputs)
                # expected outputs come back as f32: compare after rounding
                for s in range(probe.steps):
                    for j in range(len(probe.outputs[s])):
                        self.assertEqual(f.probes[0].outputs[s][j], f32s(probe.outputs[s][j]))
                self.assertEqual(data[:8], b"HGWEIGHT")
                self.assertEqual(len(data) % 64, 0 if len(data) >= 4096 else len(data) % 64)

    def test_layout_constants(self):
        self.assertEqual(struct.calcsize(hgw.HEADER_FMT), 288)
        self.assertEqual(struct.calcsize(hgw.OP_FMT), 192)
        self.assertEqual(struct.calcsize(hgw.TENSOR_FMT), 80)
        g, steps, hdr = dense_relu()
        data = hgw.build(g, hdr, [reference.make_probe(g, steps)])
        # section directory at 4096, sections and tensor data 64-aligned
        n = struct.unpack_from("<I", data, 264)[0]
        for i in range(n):
            name, off, size = struct.unpack_from("<16sQQ", data, 4096 + 32 * i)
            self.assertEqual(off % 64, 0, name)
        self.assertEqual(struct.unpack_from("<Q", data, 272)[0] % 64, 0)

    def test_hash_stability(self):
        for name, mk in GOLDEN.items():
            with self.subTest(name=name):
                self.assertEqual(hgw.arch_hash(mk()[0]), ARCH[name], f"{name}: {hgw.arch_hash(mk()[0]):016x}")

    def test_hash_covers_architecture_not_values(self):
        g, _, _ = dense_relu()
        h0 = hgw.arch_hash(g)
        g.tensors[0].data[0] = 123.0  # a weight value: same architecture
        self.assertEqual(hgw.arch_hash(g), h0)
        g.buffers[0].name = "renamed"  # names are not architecture
        self.assertEqual(hgw.arch_hash(g), h0)
        g.ops[0].attrs[2] = 0  # bias off: another architecture
        self.assertNotEqual(hgw.arch_hash(g), h0)
        g.ops[0].attrs[2] = 1
        g.inputs[0].field = "other"  # a binding: another architecture
        self.assertNotEqual(hgw.arch_hash(g), h0)

    def test_fnv1a(self):
        self.assertEqual(hgw.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(hgw.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)
        self.assertEqual(hgw.fnv1a64(b"foobar"), 0x85944171F73967E8)


class TestReference(unittest.TestCase):
    def test_dense_relu_by_hand(self):
        g = Graph(
            [Buffer("in", "f32", [2]), Buffer("h", "f32", [2]), Buffer("out", "f32", [2])],
            [Tensor("W", "f32", [2, 2], [1.0, 2.0, -3.0, 4.0]), Tensor("b", "f32", [2], [0.5, -0.5])],
            [Op("dense", 1, [2, 2, 1], [0], 1, [0, 1]), Op("relu", 1, [], [1], 2)],
            [Binding(0, 0, "a"), Binding(0, 1, "b")],
            [Binding(2, 0, "y0"), Binding(2, 1, "y1")],
        )
        m = reference.Model(g)
        self.assertEqual(m.step([[1.0, 1.0]]), [[3.5, 0.5]])
        self.assertEqual(m.step([[1.0, -1.0]]), [[0.0, 0.0]])

    def test_lstm_state_and_reset(self):
        g, steps, _ = lstm1()
        m = reference.Model(g)
        outs = m.run(steps)
        self.assertNotEqual(outs[0], outs[1])
        self.assertTrue(any(v != 0.0 for v in m.state[0]))
        m.reset()
        self.assertEqual(m.state[0], [0.0] * 8)
        self.assertEqual(m.step(steps[0]), outs[0])
        # zero weights: the LSTM output is exactly 0 from zero state
        for t in g.tensors:
            t.data = [0.0] * len(t.data)
        m = reference.Model(g)
        self.assertEqual(m.step(steps[0]), [[0.0] * 4])

    def test_lstm_single_step_by_hand(self):
        # H=1, I=1, all weights 1: gates = x + h + 2
        g = Graph(
            [Buffer("in", "f32", [1]), Buffer("out", "f32", [1])],
            [Tensor("W_ih", "f32", [4, 1], [1.0] * 4), Tensor("W_hh", "f32", [4, 1], [1.0] * 4),
             Tensor("b_ih", "f32", [4], [1.0] * 4), Tensor("b_hh", "f32", [4], [1.0] * 4)],
            [Op("lstm", 1, [1, 1, 1], [0], 1, [0, 1, 2, 3])],
            [Binding(0, 0, "x")], [Binding(1, 0, "y")],
        )
        m = reference.Model(g)
        x = 0.5
        gate = x + 0.0 + 2.0
        c = reference.sigmoid(gate) * math.tanh(gate)
        h = reference.sigmoid(gate) * math.tanh(c)
        self.assertAlmostEqual(m.step([[x]])[0][0], h, places=15)
        self.assertAlmostEqual(m.state[0][1], c, places=15)

    def test_unsupported_op_refused(self):
        g = Graph([Buffer("in", "f32", [1, 4, 4]), Buffer("out", "f32", [1, 4, 4])], [],
                  [Op("conv2d", 1, [], [0], 1)], [Binding(0, e, f"p{e}") for e in range(16)], [Binding(1, 0, "y")])
        with self.assertRaises(reference.UnsupportedOp) as cm:
            reference.Model(g)
        self.assertEqual(str(cm.exception), "op conv2d v1 not supported")
        with self.assertRaises(reference.UnsupportedOp):
            g.ops[0] = Op("dense", 2, [16, 1, 0], [0], 1)
            reference.Model(g)

    def test_shape_check(self):
        g, _, _ = dense_relu()
        g.buffers[2].shape = [5]
        with self.assertRaises(ValueError):
            reference.Model(g)


# A stand-in for torch tensors and modules, duck-typed as the exporter reads them.
class FakeTensor:
    def __init__(self, data):
        self.data = data

    def detach(self):
        return self

    def cpu(self):
        return self

    def double(self):
        return self

    def reshape(self, _):
        flat = []

        def walk(v):
            if isinstance(v, list):
                for w in v:
                    walk(w)
            else:
                flat.append(v)

        walk(self.data)
        return FakeTensor(flat)

    def tolist(self):
        return list(self.data)


class Linear:
    def __init__(self, i, o, bias=True, rng=None):
        self.in_features, self.out_features = i, o
        self.weight = FakeTensor([[rng.uniform(-1, 1) for _ in range(i)] for _ in range(o)])
        self.bias = FakeTensor([rng.uniform(-1, 1) for _ in range(o)]) if bias else None


class LSTM:
    def __init__(self, i, h, num_layers=1, bidirectional=False, proj_size=0, rng=None):
        self.input_size, self.hidden_size, self.num_layers = i, h, num_layers
        self.bidirectional, self.proj_size, self.bias = bidirectional, proj_size, True
        for k in range(num_layers):
            ik = i if k == 0 else h
            setattr(self, f"weight_ih_l{k}", FakeTensor([[rng.uniform(-1, 1) for _ in range(ik)] for _ in range(4 * h)]))
            setattr(self, f"weight_hh_l{k}", FakeTensor([[rng.uniform(-1, 1) for _ in range(h)] for _ in range(4 * h)]))
            setattr(self, f"bias_ih_l{k}", FakeTensor([rng.uniform(-1, 1) for _ in range(4 * h)]))
            setattr(self, f"bias_hh_l{k}", FakeTensor([rng.uniform(-1, 1) for _ in range(4 * h)]))


class ReLU:
    pass


class Tanh:
    pass


class Affine:
    def __init__(self, scale, shift):
        self.scale, self.shift = FakeTensor(scale), FakeTensor(shift)


class Sequential:
    def __init__(self, *layers):
        self._layers = layers

    def children(self):
        return iter(self._layers)


class TestExporter(unittest.TestCase):
    def test_graph_from_fake_modules(self):
        rng = random.Random(11)
        model = Sequential(Affine([0.1] * 5, [0.0] * 5), Linear(5, 16, rng=rng), LSTM(16, 16, rng=rng),
                           Linear(16, 1, rng=rng), Affine([4.0], [0.0]))
        fields = ["nav.state.depth", "nav.state.vel_z", "nav.state.pitch", "nav.state.roll",
                  "ctl.depth_hold.target.accel_z"]
        g = export_torch.to_graph(model, fields, ["heave"])
        self.assertEqual([op.type for op in g.ops], ["affine", "dense", "lstm", "dense", "affine"])
        self.assertEqual([b.shape for b in g.buffers], [[5], [5], [16], [16], [1], [1]])
        self.assertEqual(g.ops[2].attrs, [16, 16, 1])
        self.assertEqual([t.shape for t in g.tensors[4:8]], [[64, 16], [64, 16], [64], [64]])
        self.assertEqual([b.field for b in g.inputs], fields)
        self.assertEqual(g.outputs, [Binding(5, 0, "heave")])
        # the same shape as the golden depth_ff: the same architecture hash
        self.assertEqual(hgw.arch_hash(g), ARCH["depth_ff"])
        # every tensor value is an f32
        for t in g.tensors:
            self.assertEqual(t.data, f32s(t.data))
        # the reference runs it
        m = reference.Model(g)
        out = m.step([[10.0, 0.0, 0.1, -0.1, 0.5]])
        self.assertEqual(len(out), 1)
        self.assertEqual(len(out[0]), 1)

    def test_cast_and_activations(self):
        rng = random.Random(12)
        g = export_torch.to_graph([Linear(3, 2, bias=False, rng=rng), ReLU(), Tanh()], ["a", "b", "c"], ["y0", "y1"],
                                  input_dtype="i16", cast_scale=1 / 32768)
        self.assertEqual([op.type for op in g.ops], ["cast", "dense", "relu", "tanh"])
        self.assertEqual(g.buffers[0].dtype, "i16")
        self.assertEqual(g.ops[1].attrs, [3, 2, 0])
        self.assertEqual(len(g.ops[1].tensors), 1)

    def test_refusals(self):
        rng = random.Random(13)
        with self.assertRaises(export_torch.ExportError):
            export_torch.to_graph([LSTM(4, 4, bidirectional=True, rng=rng)], ["a"] * 4, ["y"] * 4)
        with self.assertRaises(export_torch.ExportError):
            export_torch.to_graph([LSTM(4, 4, proj_size=2, rng=rng)], ["a"] * 4, ["y"] * 4)
        with self.assertRaises(export_torch.ExportError):
            export_torch.to_graph([Linear(4, 2, rng=rng)], ["a"] * 3, ["y"] * 2)  # in_features mismatch
        with self.assertRaises(export_torch.ExportError):
            export_torch.to_graph([Linear(4, 2, rng=rng)], ["a"] * 4, ["y"] * 3)  # output count

        class Conv2d:
            pass

        with self.assertRaises(export_torch.ExportError) as cm:
            export_torch.to_graph([Conv2d()], ["a"], ["y"])
        self.assertIn("Conv2d", str(cm.exception))
        g = export_torch.to_graph([Linear(4, 2, rng=rng)], ["a"] * 4, ["y"] * 2)
        with self.assertRaises(export_torch.ExportError):
            export_torch.check_runtime(g, runtime_ops=[("lstm", 1)])
        export_torch.check_runtime(g, runtime_ops=[("dense", 1)])

    def test_export_writes_file_with_probes(self):
        rng = random.Random(14)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "m.hgw")
            g = export_torch.export([Linear(2, 2, rng=rng), Tanh()], path, ["a", "b"], ["y0", "y1"],
                                    probe_inputs=[[[0.1, 0.2], [0.3, 0.4], [0.5, 0.6]]],
                                    header=Header(name="t", tick_period_ns=1))
            f = hgw.read(path)
            self.assertEqual(f.graph, g)
            self.assertEqual(f.header.name, "t")
            self.assertEqual(len(f.probes), 1)
            self.assertEqual(f.probes[0].steps, 3)
            ref = reference.Model(g).run([[[0.1, 0.2]], [[0.3, 0.4]], [[0.5, 0.6]]])
            self.assertEqual(f.probes[0].outputs[2][0], f32s(ref[2][0]))


def parse_sidecar(text):
    out = {}
    for line in text.splitlines():
        key, _, rest = line.partition(" ")
        if key == "state":
            n, _, vals = rest.partition(" ")
            out["state"] = [float(v) for v in vals.split()]
            assert len(out["state"]) == int(n)
        elif key in ("arch",):
            out[key] = int(rest, 16)
        elif key == "steps":
            out[key] = int(rest)
        else:
            out[key] = rest
    return out


def within(a, b, rel, abs_):
    return abs(a - b) <= abs_ + rel * abs(b)


class TestVectors(unittest.TestCase):
    """Regenerates the golden files and compares with the checked-in ones.

    Not byte for byte: math.tanh and math.exp are libm calls whose last ulp
    differs by platform, so the reference's doubles (the sidecar's state, and
    through f32 rounding one bit of a probe output now and then) may differ
    between the machine that wrote the vectors and the one checking them.
    Everything that is not such a float is compared exactly; the floats within
    a tolerance. The C suite has its own tolerance against the same files.
    """

    SIDE_REL, SIDE_ABS = 1e-9, 1e-12
    F32_REL, F32_ABS = 1e-6, 1e-9

    def assert_close_lists(self, got, want, rel, abs_, what):
        self.assertEqual(len(got), len(want), what)
        for i, (a, b) in enumerate(zip(got, want)):
            if not within(a, b, rel, abs_):
                self.fail(f"{what}[{i}]: {a!r} != {b!r} (rel {rel}, abs {abs_})")

    def assert_hgw_equivalent(self, regenerated, checked_in, name):
        a, b = hgw.parse(regenerated), hgw.parse(checked_in)
        # every byte before the data region: header, directory, every record
        off = struct.unpack_from("<Q", checked_in, 272)[0]
        self.assertEqual(regenerated[:off], checked_in[:off], f"{name}: header, directory or records differ")
        self.assertEqual(len(regenerated), len(checked_in))
        self.assertEqual(a.header, b.header)
        self.assertEqual(a.sections, b.sections)
        self.assertEqual(a.ranges, b.ranges)
        g, h = a.graph, b.graph
        self.assertEqual((g.buffers, g.ops, g.inputs, g.outputs), (h.buffers, h.ops, h.inputs, h.outputs))
        self.assertEqual(len(g.tensors), len(h.tensors))
        for t, u in zip(g.tensors, h.tensors):
            self.assertEqual((t.name, t.dtype, t.shape), (u.name, u.dtype, u.shape))
            if t.dtype in ("f32", "f64"):
                self.assert_close_lists(t.data, u.data, self.F32_REL, self.F32_ABS, f"{name}: tensor {t.name}")
            else:
                self.assertEqual(t.data, u.data, f"{name}: tensor {t.name}")
        self.assertEqual(len(a.probes), len(b.probes))
        ins = [g.buffers[i].dtype for i in g.input_buffers()]
        for pi, (p, q) in enumerate(zip(a.probes, b.probes)):
            self.assertEqual(p.steps, q.steps)
            for s_ in range(p.steps):
                for i, dt in enumerate(ins):
                    if dt in ("f32", "f64"):
                        self.assert_close_lists(p.inputs[s_][i], q.inputs[s_][i], self.F32_REL, self.F32_ABS,
                                                f"{name}: probe {pi} step {s_} input {i}")
                    else:
                        self.assertEqual(p.inputs[s_][i], q.inputs[s_][i], f"{name}: probe {pi} step {s_} input {i}")
                for j in range(len(p.outputs[s_])):
                    self.assert_close_lists(p.outputs[s_][j], q.outputs[s_][j], self.F32_REL, self.F32_ABS,
                                            f"{name}: probe {pi} step {s_} output {j}")

    def assert_sidecar_equivalent(self, regenerated, checked_in, checked_in_hgw, name):
        a, b = parse_sidecar(regenerated), parse_sidecar(checked_in)
        self.assertEqual(set(a), set(b))
        # the sidecar's hash is that of the checked-in file, not of this run's bytes
        self.assertEqual(b["blake2s"], hashlib.blake2s(checked_in_hgw).hexdigest(), f"{name}: sidecar blake2s")
        self.assertEqual(a["arch"], b["arch"], f"{name}: arch")
        self.assertEqual(a["steps"], b["steps"], f"{name}: steps")
        self.assert_close_lists(a["state"], b["state"], self.SIDE_REL, self.SIDE_ABS, f"{name}: state")

    def test_golden_files(self):
        write = os.environ.get("HANGAR_NN_WRITE_VECTORS") == "1"
        os.makedirs(VECTORS, exist_ok=True)
        for name in GOLDEN:
            with self.subTest(name=name):
                data, side = build_golden(name)
                hpath, spath = os.path.join(VECTORS, name + ".hgw"), os.path.join(VECTORS, name + ".golden.txt")
                if write or not os.path.exists(hpath) or not os.path.exists(spath):
                    with open(hpath, "wb") as f:
                        f.write(data)
                    with open(spath, "w") as f:
                        f.write(side)
                with open(hpath, "rb") as f:
                    checked_in = f.read()
                with open(spath) as f:
                    checked_in_side = f.read()
                self.assert_hgw_equivalent(data, checked_in, name)
                self.assert_sidecar_equivalent(side, checked_in_side, checked_in, name)

    def test_golden_probes_reproduce(self):
        """The reference, over the checked-in file's own graph and probe inputs,
        gives the file's expected outputs (within the f32 tolerance: libm)."""
        for name in GOLDEN:
            with self.subTest(name=name):
                f = hgw.read(os.path.join(VECTORS, name + ".hgw"))
                m = reference.Model(f.graph)
                for pi, p in enumerate(f.probes):
                    out = m.run(p.inputs)
                    for s in range(p.steps):
                        for j in range(len(out[s])):
                            self.assert_close_lists(f32s(out[s][j]), p.outputs[s][j], self.F32_REL, self.F32_ABS,
                                                    f"{name}: probe {pi} step {s} output {j}")


if __name__ == "__main__":
    unittest.main()
