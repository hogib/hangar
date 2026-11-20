#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The depth feed-forward trainer of docs/09-learning.md "Example: adaptive
depth hold": an Affine that normalizes the inputs, a 1-layer LSTM of 16, a
Linear to one output and an Affine that scales it, bound to the wrench's
heave; trained on the heave the controller's command was short by, in
stateful chunks of 50 ticks with the state carried across them.

    depth_ff.py --synthetic --out depth_ff.hgw          offline, no vehicle, no recordings
    depth_ff.py --dataset depth_ff.hgr --out depth_ff.hgw
    depth_ff.py --live --deploy-every 180               the trainer module (docs/09 "Training")
    depth_ff.py --synthetic --check                     the data path alone, without PyTorch

--synthetic stands in for the vehicle: a random walk in depth, vertical
velocity, pitch, roll and the controller's wanted acceleration, and a
residual the controller was short by of 0.08 + 0.02 x depth (a buoyancy
that changes with depth), which the model should learn.

The label from a recording: the heave the thrusters applied, from thr.out's
pulse widths through the vehicle's thrust curve and allocation (VERTICAL,
PWM_*: the vehicle's configuration, to be checked against its frame), less
the controller's own heave command. until L2: ctl.depth_hold.target (the
wanted acceleration) is not published yet, so a recording built with the
docs' spec has no such part; --inputs names the inputs a recording has.
"""
import argparse
import math
import os
import random
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from hangar import learn  # noqa: E402

INPUTS = ["nav.state.depth_m", "nav.state.vel_z", "nav.state.pitch", "nav.state.roll",
          "ctl.depth_hold.target.accel_z"]
CTL_HEAVE = "ctl.depth_hold.heave"
# The vehicle's configuration: the vertical thrusters' servo outputs (0-based)
# and their share of heave, sign included; the thrust curve as linear around
# 1500 us with 400 us to full thrust. A BlueROV2 heavy (vectored 6DOF frame):
# outputs 5 to 8, throttle factor -1 in ArduPilot's mixing (lib/sim/README.md
# "thruster directions"), so heave up is a pulse width below 1500 us on all
# four. The mixing is linear in the wrench's units until it scales the axes
# down, so this inverts it exactly for a heave-only command.
VERTICAL = {4: -0.25, 5: -0.25, 6: -0.25, 7: -0.25}
PWM_STOP, PWM_FULL = 1500.0, 400.0
# depth_hold's feed-forward (msg/depth_control_params.msg ff): heave per m/s^2
# wanted. --label trim adds it back to the applied heave.
FF = 0.1


def label_fields():
    return [CTL_HEAVE] + ["thr.out.pwm[%d]" % ch for ch in sorted(VERTICAL)]


def applied_heave(labels):
    """The heave the thrusters applied, in the wrench's units, from the
    vertical pwms (labels[..., 1:])."""
    applied = 0.0
    for k, ch in enumerate(sorted(VERTICAL)):
        t = (labels[..., 1 + k] - PWM_STOP) / PWM_FULL
        applied = applied + VERTICAL[ch] * t.clip(-1, 1)
    return applied


def residual(labels):
    """[..., 1 + len(VERTICAL)] (the controller's heave, the vertical pwms) to
    [...] the heave applied less the controller's: what residual mode adds
    (docs/09, --label shortfall)."""
    return applied_heave(labels) - labels[..., 0]


def trim(labels, inputs, accel_index):
    """The heave applied less the controller's feedback (-FF x accel_z): the
    feed-forward that held the vehicle, its integrator's share and the
    residual's together (--label trim). On a still row, the heave the net
    buoyancy needs."""
    return applied_heave(labels) + FF * inputs[..., accel_index]


# ---- A vehicle in a few lines, for --synthetic ----------------------------------

class _SynthCodec(object):
    inputs = list(INPUTS)
    labels = ["residual"]
    hash = 0


def synthetic(batch, seq_len, minutes=5.0, seed=0):
    """A stream of 50 Hz rows with runs of 20 to 60 s, a masked share of 20 %."""
    from hangar.learn import Replay, _Row, _Stream, RESET, MASKED
    rng = random.Random(seed)
    rp = Replay("10min", rng=random.Random(seed + 1))
    s = _Stream(_SynthCodec(), batch, seq_len, rp, "synthetic")
    t, seq = 0, 0
    boot = b"\x5a" * 16
    n = int(minutes * 60 * 50)
    run_left = 0
    depth = vel = pitch = roll = acc = 0.0
    while seq < n:
        flags = 0
        if run_left <= 0:
            run_left = rng.randint(20 * 50, 60 * 50)
            flags |= RESET
            depth, vel = rng.uniform(0.5, 8.0), 0.0
        run_left -= 1
        acc = 0.95 * acc + rng.gauss(0, 0.02)
        vel = max(-0.5, min(0.5, 0.98 * vel + acc * 0.02))
        depth = max(0.2, depth + vel * 0.02)
        pitch = 0.9 * pitch + rng.gauss(0, 0.01)
        roll = 0.9 * roll + rng.gauss(0, 0.01)
        if rng.random() < 0.2:
            flags |= MASKED
        y = 0.08 + 0.02 * depth + rng.gauss(0, 0.005)
        seq += 1
        t += 20_000_000
        rp.add(_Row(seq, t, boot, flags, [depth, vel, pitch, roll, acc], [y], False))
    return s


# ---- The model -------------------------------------------------------------------

def make_model(n_in, mean, std):
    import torch
    from hangar.nn.export_torch import Affine

    class DepthFF(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.norm = Affine([1.0 / max(s, 1e-6) for s in std], [-m / max(s, 1e-6) for m, s in zip(mean, std)])
            self.lstm = torch.nn.LSTM(n_in, 16, batch_first=True)
            self.head = torch.nn.Linear(16, 1)
            self.scale = Affine([0.2], [0.0])  # the residual envelope's order of magnitude

        def forward(self, x, h=None):
            y, h = self.lstm(self.norm(x), h)
            return self.scale(self.head(y)), h

    return DepthFF()


def stats(stream):
    import numpy
    X = numpy.array([r.x for r in stream.replay.rows], numpy.float64)
    return X.mean(0).tolist(), X.std(0).tolist()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--synthetic", action="store_true")
    src.add_argument("--dataset", help="a built dataset file (.hgr)")
    src.add_argument("--live", action="store_true", help="the dataset.depth_ff topic")
    ap.add_argument("--inputs", help="comma-separated row fields, instead of the docs' five")
    ap.add_argument("--steps", type=int, default=600)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--seq-len", type=int, default=50)
    ap.add_argument("--out", default="depth_ff.hgw")
    ap.add_argument("--policy", default="depth_ff")
    ap.add_argument("--deploy-every", type=float, default=0, help="seconds; 0: write --out at the end")
    ap.add_argument("--check", action="store_true", help="the data path only (no PyTorch): a few chunks")
    ap.add_argument("--label", choices=("shortfall", "trim"), default="shortfall",
                    help="shortfall: applied less the controller's heave (docs/09); trim: applied less "
                         "the controller's feedback, which is not zero where nothing but the "
                         "controller drives")
    a = ap.parse_args()

    inputs = a.inputs.split(",") if a.inputs else INPUTS
    if a.synthetic:
        stream = synthetic(a.batch, a.seq_len)
        label = lambda Y, X: Y[..., 0]  # noqa: E731
    else:
        kw = dict(inputs=inputs, labels=label_fields())
        stream = learn.open_dataset(a.dataset, a.batch, a.seq_len, **kw) if a.dataset else \
            learn.stream("dataset.depth_ff", a.batch, a.seq_len, **kw)
        if a.label == "trim":
            ai = stream.input_fields.index("ctl.depth_hold.target.accel_z")
            label = lambda Y, X: trim(Y, X, ai)  # noqa: E731
        else:
            label = lambda Y, X: residual(Y)  # noqa: E731

    if a.check:
        for _ in range(5):
            c = stream.next_chunk()
            y = label(c.labels, c.inputs)
            print("chunk %s: %d rows with loss, %d fresh slots, label mean %.4f" % (
                c.inputs.shape, int(c.mask.sum()), int(c.fresh.sum()), float((y * c.mask).sum() / max(c.mask.sum(), 1))))
        print("%d rows in the buffer, %d lost" % (len(stream.replay), stream.lost))
        return 0

    import torch
    mean, std = stats(stream)
    model = make_model(len(stream.input_fields), mean, std)
    opt = torch.optim.Adam([p for p in model.parameters() if p.requires_grad], lr=3e-3)
    h = None
    deployed = time.monotonic()
    for step in range(1, a.steps + 1):
        c = stream.next_chunk()
        h = learn.carry(h, c.fresh)
        out, h = model(torch.from_numpy(c.inputs), h)
        y = torch.from_numpy(label(c.labels, c.inputs))
        m = torch.from_numpy(c.mask)
        loss = (((out[..., 0] - y) ** 2) * m).sum() / m.sum().clamp(min=1)
        opt.zero_grad()
        loss.backward()
        opt.step()
        if step % 50 == 0:
            print("step %d: loss %.6f" % (step, loss.item()))
        if a.deploy_every and time.monotonic() - deployed >= a.deploy_every:
            print(learn.deploy(model, a.policy, stream=stream, outputs=["heave"]))
            deployed = time.monotonic()
    model.eval()
    if a.synthetic:
        # What it learned: the residual at depth 1 and 6 m, still.
        with torch.no_grad():
            for d in (1.0, 6.0):
                x = torch.tensor([[[d, 0.0, 0.0, 0.0, 0.0]] * 50])
                print("depth %.0f m: residual %.3f (true %.3f)" % (d, model(x)[0][0, -1, 0].item(), 0.08 + 0.02 * d))
    learn.export(model.cpu(), a.out, stream, ["heave"], tick_period_ns=20_000_000, steps=a.steps,
                 name="depth_ff")
    print("wrote %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
