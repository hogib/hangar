#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The offline check of docs/09's adaptive depth hold, second design, made
before any model drove (bench/e2e-depth-ff/README.md, "Second design"):

    depth_ff2_offline.py <session>...       sessions of e2e-depth-ff2.sh --variant collect

  1. the plant's target of python/examples/depth_ff2.py over the recorded
     rows (the acceleration made, against the heave applied less the heave at
     rest, and vel_z): its distribution, and how much of its variance the
     inputs explain: a straight line, the simulator's own model
     (depth_ff2.sim_plant) and the example's plant network fitted on the
     first 70 % of each session and scored on the rest, and fitted on the
     rows before the buoyancy change and scored on those after it (what a
     model from before the change knows of the vehicle after it); for each
     lag of the command behind the acceleration;
  2. the acceleration nav.state reports against the simulator's truth;
  3. what a feed-forward gains in closed loop, on a heave axis in a few lines
     (the simulator's thrust table, mass, drag and thruster lag, depth_hold's
     law, the mux's residual envelope and rate limit, three ticks of loop
     delay): the controller alone; with the oracle; with the policy the
     example's learner makes of the recorded rows; and with the inverse
     fitted directly (the command from the acceleration it made), the design
     this check ruled out. The live runs measure the first three again; this
     says whether to make them.
"""
import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "python"))
sys.path.insert(0, os.path.join(REPO, "python", "examples"))

import numpy  # noqa: E402

import depth_ff2  # noqa: E402
from hangar import learn  # noqa: E402

A, W, I = depth_ff2.A, depth_ff2.W, depth_ff2.I


def load(session, lag, batch=256, seed=0):
    """The session's dataset as depth_ff2's stream, the heave at rest filled in."""
    s = learn.open_dataset(os.path.join(session, "dataset.depth_ff.hgr"), batch, None, inputs=depth_ff2.INPUTS,
                           labels=depth_ff2.label_fields(lag), seed=seed)
    t = depth_ff2.RestTracker()
    t.attach(s)
    return s, t


def buoyancy_change(session):
    """When the session's first buoyancy change was made (its events.json), ns."""
    with open(os.path.join(session, "events.json")) as f:
        t = [e["t"] for e in json.load(f) if e["name"] == "buoyancy"]
    return t[0] if t else None


def arrays(stream):
    """(P, a, X, keep): the plant's inputs and target, the policy's inputs,
    and the rows with loss, in the rows' order."""
    rows = list(stream.replay.rows)
    X = numpy.array([r.x for r in rows], numpy.float64)
    Y = numpy.array([r.y for r in rows], numpy.float64)
    keep = numpy.array([not r.flags & learn.MASKED for r in rows])
    P = numpy.stack([depth_ff2.applied_heave(Y), Y[:, depth_ff2.Y_REST], X[:, W]], -1)
    return P, Y[:, depth_ff2.Y_MADE], X, keep


def r2(y, p):
    return 1.0 - float(((y - p) ** 2).sum()) / float(((y - y.mean()) ** 2).sum())


def fit_plant(P, a, steps, seed=0):
    import torch
    torch.manual_seed(seed)
    plant = depth_ff2.make_plant()
    opt = torch.optim.Adam(plant.parameters(), lr=3e-3)
    rng = numpy.random.default_rng(seed)
    Pt, at = torch.from_numpy(P.astype(numpy.float32)), torch.from_numpy(a.astype(numpy.float32))
    moving = (numpy.abs(a) >= depth_ff2.STILL_ACCEL) | (numpy.abs(P[:, 2]) >= depth_ff2.STILL_VEL)
    wt = torch.from_numpy(numpy.where(moving, depth_ff2.MOVING_WEIGHT, 1.0).astype(numpy.float32))
    for _ in range(steps):
        k = torch.from_numpy(rng.integers(0, len(P), 256))
        loss = depth_ff2.plant_loss(plant, Pt[k], at[k], wt[k])
        opt.zero_grad()
        loss.backward()
        opt.step()

    def f(Q):
        with torch.no_grad():
            return plant(torch.from_numpy(numpy.asarray(Q, numpy.float32))).numpy().astype(numpy.float64)
    return f


def split(sessions, lag):
    tr, te = [], []
    for s in sessions:
        P, a, X, keep = arrays(load(s, lag)[0])
        P, a = P[keep], a[keep]
        n = int(0.7 * len(a))
        tr.append((P[:n], a[:n]))
        te.append((P[n:], a[n:]))
    cat = lambda v, k: numpy.concatenate([x[k] for x in v])  # noqa: E731
    return cat(tr, 0), cat(tr, 1), cat(te, 0), cat(te, 1)


def target_check(sessions, steps):
    out = {"lags": {}}
    for lag in range(0, depth_ff2.WINDOW):
        Ptr, atr, Pte, ate = split(sessions, lag)
        lin = numpy.linalg.lstsq(numpy.c_[Ptr, numpy.ones(len(Ptr))], atr, rcond=None)[0]
        e = {"r2 of a straight line": round(r2(ate, numpy.c_[Pte, numpy.ones(len(Pte))] @ lin), 3),
             "r2 of the simulator's own model": round(r2(ate, depth_ff2.sim_plant(Pte, 16.8)), 3)}
        if lag == depth_ff2.LAG or steps <= 2000:
            e["r2 of the plant network (fitted on the first 70 %, scored on the rest)"] = round(
                r2(ate, fit_plant(Ptr, atr, steps)(Pte)), 3)
        out["lags"][lag] = e
    # Across the buoyancy change: fitted before it, scored after it.
    bef, aft = [], []
    for s in sessions:
        stream = load(s, depth_ff2.LAG)[0]
        P, a, X, keep = arrays(stream)
        t = numpy.array([r.stamp for r in stream.replay.rows])
        tb = buoyancy_change(s)
        if tb is not None:
            bef.append((P[keep & (t < tb)], a[keep & (t < tb)]))
            aft.append((P[keep & (t >= tb)], a[keep & (t >= tb)]))
    if bef:
        Pb, ab = numpy.concatenate([x[0] for x in bef]), numpy.concatenate([x[1] for x in bef])
        Pa, aa = numpy.concatenate([x[0] for x in aft]), numpy.concatenate([x[1] for x in aft])
        out["fitted on the rows before the buoyancy change, scored on those after"] = {
            "rows before, after": [int(len(ab)), int(len(aa))],
            "heave at rest before: smallest, largest": [round(float(Pb[:, 1].min()), 3), round(float(Pb[:, 1].max()), 3)],
            "heave at rest after": [round(float(Pa[:, 1].min()), 3), round(float(Pa[:, 1].max()), 3)],
            "r2 of the plant network": round(r2(aa, fit_plant(Pb, ab, steps)(Pa)), 3),
            "r2 of the simulator's own model": round(r2(aa, depth_ff2.sim_plant(Pa, 16.8)), 3)}
    Ptr, atr, Pte, ate = split(sessions, depth_ff2.LAG)
    P, a = numpy.concatenate([Ptr, Pte]), numpy.concatenate([atr, ate])
    still = (numpy.abs(a) < depth_ff2.STILL_ACCEL) & (numpy.abs(P[:, 2]) < depth_ff2.STILL_VEL)
    rel = P[:, 0] - P[:, 1]
    pc = lambda v: [round(float(x), 3) for x in numpy.percentile(v, [1, 5, 25, 50, 75, 95, 99])]  # noqa: E731
    out["rows at lag %d" % depth_ff2.LAG] = {
        "rows with loss": int(len(a)), "still rows": int(still.sum()),
        "acceleration made (m/s^2): sd": round(float(a.std()), 4), "percentiles 1 5 25 50 75 95 99": pc(a),
        "on still rows: mean, sd": [round(float(a[still].mean()), 4), round(float(a[still].std()), 4)],
        "heave applied less heave at rest: sd": round(float(rel.std()), 4), "its percentiles": pc(rel),
        "on still rows: mean, sd ": [round(float(rel[still].mean()), 4), round(float(rel[still].std()), 4)],
        "share of rows with it beyond 0.02": round(float((numpy.abs(rel) > 0.02).mean()), 3),
        "heave at rest: smallest, largest": [round(float(P[:, 1].min()), 3), round(float(P[:, 1].max()), 3)]}
    return out


def accel_check(session):
    """nav.state's accel[2] against the simulator's body acceleration."""
    from hangar import record
    ns = record.open_recording(os.path.join(session, "nav.state.hgr")).array()
    sv = record.open_recording(os.path.join(session, "sim.velocity.hgr")).array()
    t = ns["publish_ns"].astype(numpy.int64)
    i = numpy.clip(numpy.searchsorted(sv["publish_ns"].astype(numpy.int64), t) - 1, 0, len(sv) - 1)
    d = ns["accel"][:, 2].astype(numpy.float64) - sv["nu_dot"][i, 2].astype(numpy.float64)
    k = slice(len(d) // 10, None)
    return {"nav.state accel[2] less the truth: mean": round(float(d[k].mean()), 4), "sd": round(float(d[k].std()), 4),
            "the truth's sd": round(float(sv["nu_dot"][i, 2][k].std()), 4)}


def learned_policy(sessions, steps, seed=0):
    """depth_ff2's learner on the sessions' rows, one after the other, as the
    trainer would have had them; the policy as a function of numpy rows."""
    import torch
    torch.set_num_threads(1)
    L = depth_ff2.Learner(seed)
    for s in sessions:
        stream, L.tracker = load(s, depth_ff2.LAG, seed=seed)
        for _ in range(steps // len(sessions)):
            L.step(stream)
        L.settle(stream)
    L.model.eval()

    def f(X):
        with torch.no_grad():
            return L.model(torch.from_numpy(numpy.asarray(X, numpy.float32)))[..., 0].numpy().astype(numpy.float64)
    return f, L


def direct_inverse(sessions, steps, seed=0):
    """The design ruled out: the policy's network fitted to the command that
    made each row's acceleration (heave applied less heave at rest, plus
    ff x accel made), read at (accel made, vel_z, heave at rest)."""
    import torch
    torch.manual_seed(seed)
    Xs, ys = [], []
    for s in sessions:
        P, a, X, keep = arrays(load(s, depth_ff2.LAG)[0])
        Xs.append(numpy.stack([a, P[:, 2], P[:, 1]], -1)[keep])
        ys.append((P[:, 0] - P[:, 1] + depth_ff2.FF * a)[keep])
    Xs, ys = numpy.concatenate(Xs).astype(numpy.float32), numpy.concatenate(ys).astype(numpy.float32)
    model = depth_ff2.make_model()
    opt = torch.optim.Adam([p for p in model.parameters() if p.requires_grad], lr=3e-3)
    rng = numpy.random.default_rng(seed)
    Xt, yt = torch.from_numpy(Xs), torch.from_numpy(ys).clamp(-depth_ff2.BOUND, depth_ff2.BOUND)
    for _ in range(steps):
        k = torch.from_numpy(rng.integers(0, len(Xs), 256))
        loss = depth_ff2.policy_loss(model, Xt[k], yt[k])
        opt.zero_grad()
        loss.backward()
        opt.step()
    model.eval()

    def f(X):
        with torch.no_grad():
            return model(torch.from_numpy(numpy.asarray(X, numpy.float32)))[..., 0].numpy().astype(numpy.float64)
    n = int(0.7 * len(ys))
    return f, {"label (heave applied less at rest, plus ff x accel made): sd": round(float(ys.std()), 4),
               "r2 of the simulator's own inverse on it": round(r2(ys, depth_ff2.oracle(Xs, 16.8)), 3),
               "r2 of the fitted network on it (within the bound)": round(
                   r2(numpy.clip(ys, -depth_ff2.BOUND, depth_ff2.BOUND), f(Xs)), 3), "rows": int(len(ys)), "split": n}


# ---- A heave axis in a few lines ----------------------------------------------------

def closed_loop(policy, seed=0, voltage=16.8, gains=None):
    """depth_hold's law on the simulator's heave axis at 200 Hz, the command
    three ticks late: a 3 m hold, steps to 4 m and back, buoyancy -4 N, the
    steps again. policy(rows of accel wanted, vel_z, I) -> residual, or None."""
    g = dict(kp=4.0, kd=4.0, accel_max=4.0, ff=0.1, ki=0.1, i_max=0.3, i_band=0.6, max_thrust=0.6)
    g.update(gains or {})
    rng = numpy.random.default_rng(seed)
    hg = numpy.linspace(-1, 1, 8001)
    ug = depth_ff2.up_force(hg, voltage)
    dt, env = 0.005, 0.2
    z, w, integ, net_up = 3.0, 0.0, -0.085, 2.0
    thrust = float(numpy.interp(integ, hg, ug))
    target, queue, last, happ, r = 3.0, [integ] * 3, integ, integ, 0.0
    sched = [(20, "t", 4.0), (45, "t", 3.0), (70, "b", -2.0), (110, "t", 4.0), (135, "t", 3.0), (160, "end", 0)]
    zs, log, sat, t, k = [z] * 5, [], 0, 0.0, 0
    while True:
        if t >= sched[0][0]:
            _, kind, v = sched.pop(0)
            if kind == "t":
                target = v
            elif kind == "b":
                net_up = v
            else:
                break
        if k % 4 == 0:
            zm, wm = zs[0] + 0.02 + rng.normal(0, 0.004), w + rng.normal(0, 0.015)
            err = target - zm
            raw = g["kp"] * err - g["kd"] * wm
            a = min(max(raw, -g["accel_max"]), g["accel_max"])
            if abs(err) < g["i_band"] and abs(raw) <= g["accel_max"]:
                integ = min(max(integ - g["ki"] * err * 0.02, -g["i_max"]), g["i_max"])
            hc = min(max(integ - g["ff"] * a, -g["max_thrust"]), g["max_thrust"])
            r = 0.0
            if policy is not None:
                r = float(policy(numpy.array([[a, wm, integ]]))[0])
                if abs(r) > env:
                    sat += 1
                    r = math.copysign(env, r)
            out = min(max(hc + r, -1.0), 1.0)
            out = min(max(out, last - 0.16), last + 0.16)   # the mux's rate limit, 8/s
            last = out
            queue.append(out)
            happ = queue.pop(0)
        thrust += (1 - math.exp(-dt / 0.05)) * (float(numpy.interp(happ, hg, ug)) - thrust)
        acc = (-net_up - thrust - depth_ff2.D_LIN * w - depth_ff2.D_QUAD * abs(w) * w) / depth_ff2.MASS
        w += acc * dt
        z += w * dt
        zs.append(z)
        zs.pop(0)
        log.append((t, z))
        t += dt
        k += 1
    L = numpy.array(log)

    def seg(t0, t1, tg, start=None):
        e = L[(L[:, 0] >= t0) & (L[:, 0] < t1), 1] - tg
        o = {"iae_m_s": round(float(numpy.abs(e).sum() * dt), 2), "peak_cm": round(100 * float(numpy.abs(e).max()), 1)}
        if start is not None:
            o["overshoot_pct"] = round(100 * max(0.0, float((math.copysign(1, tg - start) * e).max())), 1)
        return o
    e = L[(L[:, 0] >= 12) & (L[:, 0] < 20), 1] - 3.0
    return {"hold rms (cm)": round(100 * float(numpy.sqrt((e ** 2).mean())), 2),
            "nominal 3 to 4 m": seg(20, 45, 4, 3), "nominal 4 to 3 m": seg(45, 70, 3, 4),
            "buoyancy -4 N": seg(70, 110, 3), "heavier 3 to 4 m": seg(110, 135, 4, 3),
            "heavier 4 to 3 m": seg(135, 160, 3, 4), "ticks beyond the envelope": sat}


def line(r):
    return ("hold %.2f cm | nominal: 3 to 4 m %.2f m s, %.1f %%; 4 to 3 m %.2f, %.1f %% | buoyancy -4 N: %.2f m s, "
            "%.1f cm | heavier: %.2f, %.1f %%; %.2f, %.1f %% | beyond the envelope %d ticks" % (
                r["hold rms (cm)"], r["nominal 3 to 4 m"]["iae_m_s"], r["nominal 3 to 4 m"]["overshoot_pct"],
                r["nominal 4 to 3 m"]["iae_m_s"], r["nominal 4 to 3 m"]["overshoot_pct"],
                r["buoyancy -4 N"]["iae_m_s"], r["buoyancy -4 N"]["peak_cm"],
                r["heavier 3 to 4 m"]["iae_m_s"], r["heavier 3 to 4 m"]["overshoot_pct"],
                r["heavier 4 to 3 m"]["iae_m_s"], r["heavier 4 to 3 m"]["overshoot_pct"],
                r["ticks beyond the envelope"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sessions", nargs="+")
    ap.add_argument("--steps", type=int, default=6000, help="training steps of each fitted network")
    ap.add_argument("--seeds", type=int, default=3, help="closed-loop runs per variant")
    ap.add_argument("--out", help="write the result here too (json)")
    a = ap.parse_args()
    res = {"sessions": a.sessions}
    res["the plant's target"] = target_check(a.sessions, a.steps)
    res["acceleration"] = accel_check(a.sessions[0])
    pol, L = learned_policy(a.sessions, a.steps)
    inv, res["the inverse fitted directly"] = direct_inverse(a.sessions, a.steps)
    grid = {}
    lo, hi = L.tracker.lo, L.tracker.hi
    for i0 in (round(lo, 3), round(hi, 3)):
        for acc in (-2.0, -0.5, -0.2, -0.1, 0.0, 0.1, 0.2, 0.5, 2.0):
            x = numpy.array([[acc, 0.0, i0]])
            grid["trim %+.3f, accel wanted %+.1f" % (i0, acc)] = [
                round(float(pol(x)[0]), 3), round(float(depth_ff2.oracle(x, 16.8, bound=depth_ff2.BOUND)[0]), 3),
                round(float(inv(x)[0]), 3)]
    res["residual at vel_z 0: [learned policy, oracle, inverse fitted directly]"] = grid
    cl = {}
    for name, p in (("the controller alone", None),
                    ("with the oracle, unbounded (the envelope alone bounds it)", lambda X: depth_ff2.oracle(X, 16.8)),
                    ("with the oracle within the bound", lambda X: depth_ff2.oracle(X, 16.8, bound=depth_ff2.BOUND)),
                    ("with the policy learned from the recorded rows", pol),
                    ("with the inverse fitted directly", inv)):
        cl[name] = [closed_loop(p, seed) for seed in range(a.seeds)]
    res["closed loop, a heave axis in a few lines"] = cl
    text = json.dumps(res, indent=1)
    if a.out:
        open(a.out, "w").write(text + "\n")
    print(json.dumps({k: v for k, v in res.items() if not k.startswith("closed loop")}, indent=1))
    for name, rs in cl.items():
        print(name)
        for r in rs:
            print("   " + line(r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
