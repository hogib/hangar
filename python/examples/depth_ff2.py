#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The depth feed-forward trainer of docs/09-learning.md "Example: adaptive
depth hold", second design (the first is depth_ff.py; what became of it:
bench/e2e-depth-ff/README.md).

The depth controller commands heave = I - ff x accel_z: its integrator I and
a feed-forward that takes the thrusters for linear and the vehicle for 16 kg
in still water. Neither holds (a deadband, a thrust curve that is not a
line, added mass, drag), so the vehicle does not make the acceleration the
controller wants. Two networks:

  the plant   accel made = phi(heave applied) - phi(heave at rest) - drag(vel_z)
              fitted to the rows: thr.out's pulse widths LAG ticks before the
              anchor (scaled linearly: the command as it left the mux, the
              controller's and any residual's together) and nav.state's
              accel[2]. "Heave at rest" is the trainer's running mean of the
              heave applied over the rows in which the vehicle was still:
              what holds it, whoever supplies it. phi is the thrust curve over
              the mass, drag the damping over it, both learned, drag(0) = 0.
              The plant has no constant: at the heave at rest with the vehicle
              still it says zero whatever phi and drag are, so nothing in it
              can hold a trim, and a buoyancy change moves the point at which
              phi is read, not the function.
  the policy  f(accel wanted, vel_z, I) -> heave to add
              what runs on the vehicle. Its target is the plant inverted: the
              heave h nearest the trim with phi(h) = phi(trim) + drag(vel_z)
              + accel wanted, less the controller's own command trim - ff x
              accel wanted. On the vehicle the integrator I stands for the
              trim. Zero at accel 0, vel_z 0 (a loss term pins it), and zero
              for trims outside the commands the rows hold, where phi was
              never fitted; a tanh keeps it within BOUND, inside the mux's
              residual envelope (0.2).

Nothing here knows the thrust curve, the mass or the drag. Fitting the
inverse directly (the command from the acceleration it made) does not work:
in the deadband many commands make the same acceleration, and the mean of
them is no command to give (tools/sim/depth_ff2_offline.py shows it).

    depth_ff2.py --synthetic --out depth_ff.hgw      offline: a heave axis in a few lines
    depth_ff2.py --dataset depth_ff.hgr --out depth_ff.hgw
    depth_ff2.py --live --deploy-every 180           the trainer module (docs/09 "Training")
    depth_ff2.py --oracle --out oracle.hgw           the policy fitted to the simulator's own
                                                     thrust table, mass and drag: the ceiling
    depth_ff2.py --synthetic --check                 the data path alone, without PyTorch
"""
import argparse
import csv
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

from hangar import learn  # noqa: E402

# What the policy reads at its tick, in the model's order.
INPUTS = ["ctl.depth_hold.target.accel_z", "nav.state.vel_z", "ctl.depth_hold.target.integral"]
A, W, I = 0, 1, 2
# The vertical thrusters' servo outputs (0-based) and their share of heave
# (depth_ff.py: a BlueROV2 heavy, heave up below 1500 us on all four).
VERTICAL = {4: -0.25, 5: -0.25, 6: -0.25, 7: -0.25}
PWM_STOP, PWM_FULL = 1500.0, 400.0
WINDOW = 8            # the dataset's thr.out window; message WINDOW-1 is the anchor's
LAG = 2               # ticks from a command sent to the acceleration it makes in nav.state
FF = 0.1              # depth_hold's ff: heave per m/s^2 wanted
BOUND = 0.18          # the policy's bound: 0.9 of the mux's residual envelope
STILL_ACCEL, STILL_VEL = 0.05, 0.02   # m/s^2, m/s: a row in which the vehicle is still
REST_ROWS = 50        # the heave at rest: a running mean over this many still rows
MOVING_WEIGHT = 3.0   # a moving row's weight in the plant's loss (most rows are still)
HIDDEN, PLANT_HIDDEN = 32, 32
SCALE = [1.0, 0.25, 0.1]            # the policy's input scales: accel, vel_z, I
PLANT_SCALE = [0.1, 0.25]           # the plant's: heave (applied, at rest), vel_z
H_GRID = (-0.8, 0.8, 321)           # the commands the inversion tries

MADE = "made.accel[2]"
# A slot in the label vector for the heave at rest, which no field of the row
# holds: the trainer writes it there (RestTracker). The field named is read
# and thrown away.
REST_SLOT = "made.accel[0]"
Y_MADE, Y_PWM, Y_REST = 0, 1, 5


def label_fields(lag=LAG):
    """The row's fields the plant is fitted on: the acceleration made, the
    vertical pulse widths of the command `lag` ticks before the anchor, and
    the slot for the heave at rest."""
    k = WINDOW - 1 - lag
    return [MADE] + ["thr.out.%d.pwm[%d]" % (k, ch) for ch in sorted(VERTICAL)] + [REST_SLOT]


def applied_heave(Y):
    """The heave applied, in the wrench's units, from the vertical pulse
    widths (Y[..., 1:5])."""
    h = 0.0
    for k, ch in enumerate(sorted(VERTICAL)):
        h = h + VERTICAL[ch] * ((Y[..., Y_PWM + k] - PWM_STOP) / PWM_FULL).clip(-1, 1)
    return h


def is_still(accel_made, vel_z):
    return abs(accel_made) < STILL_ACCEL and abs(vel_z) < STILL_VEL


class RestTracker(object):
    """The heave at rest, row by row in the rows' order: the running mean of
    the heave applied over the still rows so far. It is written into each
    row's label vector (Y_REST). Until a still row has been seen it is the
    integrator's value."""

    def __init__(self):
        self.rest = None
        self.lo = self.hi = None      # the range it has taken
        self.stream = None

    def row(self, r):
        h = 0.0
        for k, ch in enumerate(sorted(VERTICAL)):
            h += VERTICAL[ch] * max(-1.0, min(1.0, (r.y[Y_PWM + k] - PWM_STOP) / PWM_FULL))
        if not r.flags & learn.MASKED and is_still(r.y[Y_MADE], r.x[W]):
            self.rest = h if self.rest is None else self.rest + (h - self.rest) / REST_ROWS
            self.lo = self.rest if self.lo is None else min(self.lo, self.rest)
            self.hi = self.rest if self.hi is None else max(self.hi, self.rest)
        r.y[Y_REST] = self.rest if self.rest is not None else r.x[I]

    def attach(self, stream):
        """The rows the stream holds, and from now on every row on its way
        into the buffer: no row is drawn without its heave at rest."""
        if stream is self.stream:
            return
        self.stream = stream
        for r in stream.replay.rows:
            self.row(r)
        add = stream.replay.add

        def tracked(row):
            self.row(row)
            add(row)
        stream.replay.add = tracked

    def applied(self, stream):
        """The range of the commands among the buffer's rows with loss, its
        outermost hundredths left out: where phi has been fitted."""
        v = []
        for r in stream.replay.rows:
            if not r.flags & learn.MASKED:
                v.append(sum(VERTICAL[ch] * max(-1.0, min(1.0, (r.y[Y_PWM + k] - PWM_STOP) / PWM_FULL))
                             for k, ch in enumerate(sorted(VERTICAL))))
        if not v:
            return (0.0, 0.0)
        v.sort()
        return (v[len(v) // 100], v[len(v) - 1 - len(v) // 100])


def plant_xy(chunk):
    """A chunk of the stream as the plant's inputs (heave applied, heave at
    rest, vel_z), its target (the acceleration made) and its rows' weights."""
    import numpy
    X, Y = chunk.inputs, chunk.labels
    P = numpy.stack([applied_heave(Y), Y[..., Y_REST], X[..., W]], -1).astype(numpy.float32)
    a = Y[..., Y_MADE].astype(numpy.float32)
    moving = (numpy.abs(a) >= STILL_ACCEL) | (numpy.abs(X[..., W]) >= STILL_VEL)
    return P, a, (chunk.mask * numpy.where(moving, MOVING_WEIGHT, 1.0)).astype(numpy.float32)


# ---- The simulator's heave axis, for the oracle and for --synthetic -------------

MASS, D_LIN, D_QUAD = 11.5 + 14.57, 5.18, 36.99   # lib/sim: mass and added mass, heave damping
_TABLE = {}


def thrust_table(voltage=16.0):
    """(pulse widths, newtons of one T200) at `voltage`, from lib/sim's table."""
    if voltage not in _TABLE:
        path = os.path.join(HERE, "..", "..", "lib", "sim", "data", "t200_thrust.csv")
        with open(path) as f:
            rows = [r for r in csv.reader(f) if r and not r[0].startswith("#")]
        volts = [float(c[4:-1]) for c in rows[0][1:]]
        pwm, n = [], []
        for r in rows[1:]:
            v = [float(x) * 9.80665 for x in r[1:]]
            j = max(0, min(len(volts) - 2, sum(1 for x in volts[1:-1] if voltage > x)))
            f = (min(max(voltage, volts[0]), volts[-1]) - volts[j]) / (volts[j + 1] - volts[j])
            pwm.append(float(r[0]))
            n.append(v[j] * (1 - f) + v[j + 1] * f)
        _TABLE[voltage] = (pwm, n)
    return _TABLE[voltage]


def up_force(h, voltage=16.0):
    """Newtons up of the four verticals at heave h (numpy)."""
    import numpy
    pwm, n = thrust_table(voltage)
    return -4.0 * numpy.interp(PWM_STOP - PWM_FULL * numpy.clip(h, -1, 1), pwm, n)


def sim_plant(P, voltage=16.0):
    """The simulator's own answer to the plant's question: the acceleration
    (m/s^2 down) at heave P[..., 0] with the vehicle trimmed at P[..., 1],
    moving at P[..., 2]: thrust at rest less thrust applied less drag, over
    mass and added mass."""
    import numpy
    P = numpy.asarray(P, numpy.float64)
    w = P[..., 2]
    return (up_force(P[..., 1], voltage) - up_force(P[..., 0], voltage) - D_LIN * w - D_QUAD * numpy.abs(w) * w) / MASS


def oracle(X, voltage=16.0, ff=FF, bound=None):
    """The residual the simulator's own model asks for, per row of X (accel
    wanted, vel_z, I): the heave whose thrust is the trim's (the thrust at I)
    less the drag at vel_z less mass x accel, less the controller's command
    I - ff x accel. Zero at accel 0, vel_z 0. Within the deadband the nearer
    edge is taken."""
    import numpy
    X = numpy.asarray(X, numpy.float64)
    a, w, i = X[..., A], X[..., W], X[..., I]
    hg = numpy.linspace(-1, 1, 8001)
    ug = up_force(hg, voltage)
    want = up_force(i, voltage) - D_LIN * w - D_QUAD * numpy.abs(w) * w - MASS * a
    lo = hg[numpy.clip(numpy.searchsorted(ug, want, side="left"), 0, len(hg) - 1)]     # first h with U >= want
    hi = hg[numpy.clip(numpy.searchsorted(ug, want, side="right") - 1, 0, len(hg) - 1)]  # last h with U <= want
    h = numpy.where(want > 1e-6, lo, numpy.where(want < -1e-6, hi, numpy.clip(i, hi.min(), lo.max())))
    still = numpy.abs(want - up_force(i, voltage)) < 1e-6
    h = numpy.where(still, i, h)
    r = h - (i - ff * a)
    return numpy.clip(r, -bound, bound) if bound else r


def policy_samples(n, rng, i_range=(-0.3, 0.3)):
    """Inputs over the range the policy meets: accelerations wanted up to the
    controller's clamp (most of them small), velocities to 0.6 m/s, the
    integrator over its whole range."""
    import numpy
    a = numpy.where(rng.random(n) < 0.6, rng.normal(0, 0.3, n), rng.uniform(-4, 4, n))
    w = numpy.where(rng.random(n) < 0.5, rng.normal(0, 0.05, n), rng.uniform(-0.6, 0.6, n))
    i = rng.uniform(i_range[0], i_range[1], n)
    return numpy.stack([a, w, i], -1).astype(numpy.float32)


class _SynthCodec(object):
    inputs = list(INPUTS)
    labels = label_fields()
    hash = 0


def synthetic(batch, minutes=4.0, seed=0, net_up=2.0):
    """Rows from the heave axis above under depth_hold's law (kp 4, kd 4,
    accel_max 4, ki 0.1), stepping between depths: 50 Hz, the command LAG
    ticks old when it acts. net_up: newtons the vehicle floats by."""
    import numpy
    from hangar.learn import Replay, _Row, _Stream, RESET
    rng = random.Random(seed)
    rp = Replay(None, rng=random.Random(seed + 1))
    s = _Stream(_SynthCodec(), batch, None, rp, "synthetic")
    z, w, integ, target = 3.0, 0.0, 0.0, 3.0
    queue = [0.0] * LAG
    boot = b"\x5a" * 16
    for k in range(int(minutes * 60 * 50)):
        if k % 400 == 0:
            target = rng.uniform(2.0, 5.0)
        err = target - z
        raw = 4 * err - 4 * w
        a_want = max(-4.0, min(4.0, raw))
        if abs(err) < 0.6 and abs(raw) <= 4:
            integ = max(-0.3, min(0.3, integ - 0.1 * err * 0.02))
        h = max(-0.6, min(0.6, integ - FF * a_want))
        queue.append(h)
        h_then = queue.pop(0)
        acc = (-net_up - float(up_force(numpy.array(h_then))) - D_LIN * w - D_QUAD * abs(w) * w) / MASS
        pwm = PWM_STOP - PWM_FULL * h_then
        rp.add(_Row(k + 1, (k + 1) * 20_000_000, boot, RESET if k == 0 else 0,
                    [a_want, w, integ], [acc + rng.gauss(0, 0.01)] + [pwm] * 4 + [0.0], False))
        w += acc * 0.02
        z += w * 0.02
    return s


# ---- The two networks -------------------------------------------------------------

def make_plant(hidden=PLANT_HIDDEN):
    """(heave applied, heave at rest, vel_z) -> acceleration made, m/s^2 down:
    phi(applied) - phi(at rest) - (drag(vel_z) - drag(0))."""
    import torch

    def net():
        return torch.nn.Sequential(torch.nn.Linear(1, hidden), torch.nn.Tanh(),
                                   torch.nn.Linear(hidden, hidden), torch.nn.Tanh(), torch.nn.Linear(hidden, 1))

    class Plant(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.phi, self.drag = net(), net()

        def forward(self, p):
            h, rest, w = (p[..., k:k + 1] / s for k, s in zip(range(3), (PLANT_SCALE[0], PLANT_SCALE[0], PLANT_SCALE[1])))
            return (self.phi(h) - self.phi(rest) - self.drag(w) + self.drag(torch.zeros_like(w)))[..., 0]

    return Plant()


def make_model(bound=BOUND, hidden=HIDDEN):
    """The policy: Affine (the inputs' scales), Linear, Tanh, Linear, Tanh,
    Linear, Tanh, Affine (the bound), bound to the wrench's heave: no state,
    one step a tick."""
    import torch
    from hangar.nn.export_torch import Affine

    class DepthFF2(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.norm = Affine([1.0 / s for s in SCALE], [0.0] * len(SCALE))
            self.l1 = torch.nn.Linear(len(SCALE), hidden)
            self.t1 = torch.nn.Tanh()
            self.l2 = torch.nn.Linear(hidden, hidden)
            self.t2 = torch.nn.Tanh()
            self.l3 = torch.nn.Linear(hidden, 1)
            self.t3 = torch.nn.Tanh()
            self.scale = Affine([bound], [0.0])

        def forward(self, x):
            return self.scale(self.t3(self.l3(self.t2(self.l2(self.t1(self.l1(self.norm(x))))))))

    return DepthFF2()


def plant_loss(plant, P, a, weight):
    """Weighted squared error on the acceleration made."""
    return (((plant(P) - a) ** 2) * weight).sum() / weight.sum().clamp(min=1)


def invert(plant, X, trims=None, ff=FF, bound=BOUND):
    """The policy's target per row of X (accel wanted, vel_z, trim): the
    command on H_GRID whose acceleration by `plant` is nearest the one
    wanted, the nearer to the trim among equals, less the controller's own
    trim - ff x accel; within the bound. `plant` maps an array [..., 3] to
    accelerations (numpy); one that has `parts` (the learner's) is asked
    for phi on the grid, phi at the trims and the drag instead, which costs
    a grid of passes less. trims = (lo, hi): for a trim outside it the
    target is zero, the rows having said nothing there."""
    import numpy
    X = numpy.asarray(X, numpy.float32)
    hg = numpy.linspace(*H_GRID).astype(numpy.float32)
    n = len(X)
    if hasattr(plant, "parts"):
        phi_grid, phi_trim, drag = plant.parts(hg, X[:, I], X[:, W])
        made = phi_grid[None, :] - phi_trim[:, None] - drag[:, None]
    else:
        grid = numpy.stack([numpy.tile(hg, (n, 1)), numpy.repeat(X[:, I:I + 1], len(hg), 1),
                            numpy.repeat(X[:, W:W + 1], len(hg), 1)], -1)
        made = plant(grid)
    cost = numpy.abs(made - X[:, A:A + 1]) + 0.02 * numpy.abs(hg[None, :] - X[:, I:I + 1])
    r = hg[cost.argmin(1)] - (X[:, I] - ff * X[:, A])
    r = numpy.clip(r, -bound, bound)
    if trims is not None:
        r = numpy.where((X[:, I] < trims[0]) | (X[:, I] > trims[1]), 0.0, r)
    return r.astype(numpy.float32)


def policy_loss(model, X, y):
    """Squared error against the target, and the pin: the output at accel 0,
    vel_z 0 and each row's integrator, squared."""
    import torch
    still = torch.zeros_like(X)
    still[..., I] = X[..., I]
    return ((model(X)[..., 0] - y) ** 2).mean() + (model(still)[..., 0] ** 2).mean()


class Learner(object):
    """The plant and the policy with their optimizers: one step fits the
    plant to a chunk of rows and the policy to the plant inverted."""

    def __init__(self, seed=0, lr=3e-3):
        import numpy
        import torch
        torch.manual_seed(seed)
        self.plant, self.model = make_plant(), make_model()
        self.opt_plant = torch.optim.Adam(self.plant.parameters(), lr=lr)
        self.opt_model = torch.optim.Adam([p for p in self.model.parameters() if p.requires_grad], lr=lr)
        self.rng = numpy.random.default_rng(seed)
        self.tracker = RestTracker()
        self.steps = 0
        self.trims = None
        self.plant_loss = self.policy_loss = float("nan")

    def plant_np(self, P):
        import torch
        with torch.no_grad():
            return self.plant(torch.from_numpy(P)).numpy()

    def parts(self, grid, trims, vel_z):
        """phi on the grid, phi at the trims, drag at the velocities (numpy)."""
        import torch
        with torch.no_grad():
            f = lambda net, v, s: net(torch.from_numpy(v)[:, None] / s)[:, 0]  # noqa: E731
            z = torch.zeros(1, 1)
            return (f(self.plant.phi, grid, PLANT_SCALE[0]).numpy(), f(self.plant.phi, trims, PLANT_SCALE[0]).numpy(),
                    (f(self.plant.drag, vel_z, PLANT_SCALE[1]) - self.plant.drag(z)[0, 0]).numpy())

    def step_plant(self, chunk):
        import torch
        P, a, wt = plant_xy(chunk)
        loss = plant_loss(self.plant, torch.from_numpy(P), torch.from_numpy(a), torch.from_numpy(wt))
        self.opt_plant.zero_grad()
        loss.backward()
        self.opt_plant.step()
        self.plant_loss = loss.item()

    def step_policy(self, trims, n=256):
        import torch
        X = policy_samples(n, self.rng)
        y = invert(self, X, trims)
        loss = policy_loss(self.model, torch.from_numpy(X), torch.from_numpy(y))
        self.opt_model.zero_grad()
        loss.backward()
        self.opt_model.step()
        self.policy_loss = loss.item()

    def step(self, stream):
        """One step of each, on the stream's rows as they are now."""
        self.tracker.attach(stream)
        self.step_plant(stream.next_chunk())
        if self.steps % 50 == 0:      # a walk over the buffer: not every step
            self.trims = self.tracker.applied(stream)
        self.step_policy(self.trims)
        self.steps += 1

    def settle(self, stream, steps=300):
        """Before an export: the policy brought up to the plant as it is."""
        self.tracker.attach(stream)
        trims = self.tracker.applied(stream)
        for _ in range(steps):
            self.step_policy(trims)


def fit_oracle(model, steps=6000, seed=0, voltage=16.0, lr=3e-3, log=None):
    """The policy fitted to oracle(): the ceiling of what this architecture
    and bound can do with the simulator's own numbers."""
    import numpy
    import torch
    rng = numpy.random.default_rng(seed)
    opt = torch.optim.Adam([p for p in model.parameters() if p.requires_grad], lr=lr)
    sched = torch.optim.lr_scheduler.StepLR(opt, max(1, steps // 3), 0.3)
    for step in range(1, steps + 1):
        X = policy_samples(256, rng)
        y = oracle(X, voltage, bound=BOUND).astype(numpy.float32)
        loss = policy_loss(model, torch.from_numpy(X), torch.from_numpy(y))
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
        if log and step % 500 == 0:
            log("oracle fit step %d: loss %.2e" % (step, loss.item()))
    return model


class _OracleStream(object):
    """What export() needs of a stream, for a model no rows trained."""
    input_fields = list(INPUTS)
    seq_len = None
    name = "dataset.depth_ff"
    dataset_hash = 0

    class _Empty(object):
        rows = []
        total = 0

        def __len__(self):
            return 0
    replay = _Empty()

    def lineage(self):
        return []

    def probes(self, n=8):
        import numpy
        return [policy_samples(n, numpy.random.default_rng(1)).tolist()]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--synthetic", action="store_true")
    src.add_argument("--dataset", help="a built dataset file (.hgr)")
    src.add_argument("--live", action="store_true", help="the dataset.depth_ff topic")
    src.add_argument("--oracle", action="store_true", help="fit the simulator's own model instead of rows")
    ap.add_argument("--steps", type=int, default=3000)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--out", default="depth_ff.hgw")
    ap.add_argument("--policy", default="depth_ff")
    ap.add_argument("--deploy-every", type=float, default=0, help="seconds; 0: write --out at the end")
    ap.add_argument("--check", action="store_true", help="the data path only (no PyTorch): a few batches")
    a = ap.parse_args()

    if a.oracle:
        import torch
        torch.manual_seed(0)
        model = fit_oracle(make_model(), a.steps, log=print)
        model.eval()
        learn.export(model, a.out, _OracleStream(), ["heave"], tick_period_ns=20_000_000, steps=a.steps,
                     name="depth_ff")
        print("wrote %s" % a.out)
        return 0

    kw = dict(inputs=INPUTS, labels=label_fields())
    if a.synthetic:
        stream = synthetic(a.batch)
    elif a.dataset:
        stream = learn.open_dataset(a.dataset, a.batch, None, **kw)
    else:
        stream = learn.stream("dataset.depth_ff", a.batch, None, **kw)

    if a.check:
        tracker = RestTracker()
        for _ in range(5):
            stream._ingest()
            tracker.attach(stream)
            c = stream.next_chunk()
            P, acc, wt = plant_xy(c)
            print("batch %s: %d rows with loss, %d moving; heave applied less at rest: sd %.4f; accel made: sd %.4f" % (
                c.inputs.shape, int(c.mask.sum()), int((wt > 1).sum()), float((P[:, 0] - P[:, 1]).std()),
                float(acc.std())))
        print("%d rows in the buffer, %d lost; commands %+.3f to %+.3f" % (
            (len(stream.replay), stream.lost) + tracker.applied(stream)))
        return 0

    import time
    import torch
    torch.set_num_threads(1)
    L = Learner()
    deployed = time.monotonic()
    for step in range(1, a.steps + 1):
        L.step(stream)
        if step % 500 == 0:
            print("step %d: plant loss %.2e, policy loss %.2e" % (step, L.plant_loss, L.policy_loss))
        if a.deploy_every and time.monotonic() - deployed >= a.deploy_every:
            L.settle(stream)
            L.model.eval()
            print(learn.deploy(L.model, a.policy, stream=stream, outputs=["heave"]))
            L.model.train()
            deployed = time.monotonic()
    L.settle(stream)
    L.model.eval()
    if a.synthetic:
        # What it learned against the simulator's own answer, vehicle still.
        import numpy
        trim = float(numpy.median([r.y[Y_REST] for r in stream.replay.rows]))
        with torch.no_grad():
            for acc in (-1.0, -0.3, -0.1, 0.0, 0.1, 0.3, 1.0):
                x = numpy.array([[acc, 0.0, trim]], numpy.float32)
                print("trim %+.3f, accel wanted %+.1f: residual %+.3f (the simulator's own %+.3f)" % (
                    trim, acc, L.model(torch.from_numpy(x))[0, 0].item(), float(oracle(x, bound=BOUND)[0])))
    learn.export(L.model.cpu(), a.out, stream, ["heave"], tick_period_ns=20_000_000, steps=L.steps,
                 name="depth_ff")
    print("wrote %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
