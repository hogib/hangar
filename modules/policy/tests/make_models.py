#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Toy weights files for modules/policy's suite, written with python/hangar's
own .hgw writer and reference (no PyTorch): a dense layer from the toy
dataset's two input fields to the wrench's heave and surge.

    make_models.py arch                      the architecture hash, 16 hex digits
    make_models.py models <outdir> <dataset hash, hex>

The models (same architecture unless named bad_arch):
  good_a    heave = 0.5 a + 0.25 t + 0.1, surge = a - t
  good_b    heave = -a + 0.1,            surge = 2 t
  ident     heave = a, surge = t          (the features, as the model sees them)
  explode   weights 3e38: finite on the probes (inputs 0), not finite for a, t >= 1
  bad_arch  one output only                (check 2)
  bad_nan   a NaN weight                   (check 5)
  bad_probe a probe output off by 1        (check 6)
  wrong_ds  another dataset hash           (check 4)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get("POLICY_PYTHON") or os.path.join(HERE, "..", "..", "..", "python"))

from hangar.nn import hgw, reference  # noqa: E402

IN = ["toy.anchor.depth_m", "toy.target.depth_m"]


def dense(W, b, outputs):
    n = len(outputs)
    return hgw.Graph(
        buffers=[hgw.Buffer("in", "f32", [2]), hgw.Buffer("out", "f32", [n])],
        tensors=[hgw.Tensor("W", "f32", [n, 2], W), hgw.Tensor("b", "f32", [n], b)],
        ops=[hgw.Op("dense", 1, [2, n, 1], [0], 1, [0, 1])],
        inputs=[hgw.Binding(0, e, f) for e, f in enumerate(IN)],
        outputs=[hgw.Binding(1, e, f) for e, f in enumerate(outputs)],
    )


OUT = ["heave", "surge"]


def write(outdir, name, g, dataset_hash, probe_inputs, warmup=3, tamper=False, nan=False):
    hdr = hgw.Header(name="toy", tick_period_ns=20_000_000, warmup_ticks=warmup, dataset="toy",
                     dataset_hash=dataset_hash, rows=123, newest_row_stamp_ns=42,
                     trainer_program="make_models.py")
    probe = reference.make_probe(g, [[list(x)] for x in probe_inputs]) if not nan else None
    if tamper:
        probe.outputs[0][0][0] += 1.0
    hgw.write(os.path.join(outdir, name + ".hgw"), g, hdr, [probe] if probe else [])


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "arch":
        print("%016x" % hgw.arch_hash(dense([0, 0, 0, 0], [0, 0], OUT)))
        return 0
    if len(sys.argv) != 4 or sys.argv[1] != "models":
        print(__doc__, file=sys.stderr)
        return 64
    outdir, dh = sys.argv[2], int(sys.argv[3], 16)
    os.makedirs(outdir, exist_ok=True)
    probes = [(0.0, 0.0), (1.0, 2.0), (3.5, -1.0)]
    write(outdir, "good_a", dense([0.5, 0.25, 1.0, -1.0], [0.1, 0.0], OUT), dh, probes)
    write(outdir, "good_b", dense([-1.0, 0.0, 0.0, 2.0], [0.1, 0.0], OUT), dh, probes)
    write(outdir, "ident", dense([1.0, 0.0, 0.0, 1.0], [0.0, 0.0], OUT), dh, probes)
    write(outdir, "explode", dense([3e38, 3e38, 0.0, 0.0], [0.0, 0.0], OUT), dh, [(0.0, 0.0)])
    write(outdir, "bad_arch", dense([0.5, 0.25], [0.1], ["heave"]), dh, probes)
    write(outdir, "bad_probe", dense([0.5, 0.25, 1.0, -1.0], [0.1, 0.0], OUT), dh, probes, tamper=True)
    write(outdir, "wrong_ds", dense([0.5, 0.25, 1.0, -1.0], [0.1, 0.0], OUT), dh ^ 1, probes)
    g = dense([0.5, float("nan"), 1.0, -1.0], [0.1, 0.0], OUT)
    write(outdir, "bad_nan", g, dh, probes, nan=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
