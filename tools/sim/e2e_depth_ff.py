#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""docs/09's adaptive depth hold, end to end in simulation: the experiment
tools/sim/e2e-depth-ff.sh runs once the manager is up under --root
(bench/e2e-depth-ff/README.md has the method and a run's results).

Arms through safety (admin), assisted with a target depth, trains
python/examples/depth_ff.py's model live on dataset.depth_ff (PyTorch, in this
process), deploys it (same host: hangar.learn.deploy's incoming copy, until
the bridge), promotes residual:depth_ff with its hash, changes the
simulator's buoyancy live, deploys again, goes back to assisted. Then reads
the recorder's session and reports (report.json, and report.md's tables):

  - depth error (truth, sim.pose) at a 3 m hold and on 1 m steps, assisted
    against residual, nominal buoyancy against 4 N heavier (blocks B1 to B5);
  - the transients at the hold: the buoyancy changes, the promotion, the swap
    to the second model, the return to assisted;
  - who carried the trim in each block: the integrator or the policy;
  - the control loop's latency, nav.depth to thr.cmd through the references;
  - the learning loop's latency: from the newest row a model was trained on
    to its first output and the first thruster command carrying it;
  - swap cost: every 50 Hz topic's tick intervals during a load against the
    10 s before it;
  - train/serve mismatch: the input messages the policy read against the ones
    the pairer joined, and the deployed file run by python/hangar's reference
    on the pairer's rows against the policy's outputs;
  - lineage: every model's file in the session against its announcement, and
    one thruster command followed back to the model, its rows and the sensor
    messages;
  - demotions: the mux leaving residual without being asked;
  - rows: lost on the way to the trainer, lost by the recorder, and both
    labels over each block's rows.

The schedule is in phases; --quick shortens every one (a smoke run).
--analyse <session> runs the analysis alone on a session that holds the
run's events.json.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "python"))
sys.path.insert(0, os.path.join(REPO, "python", "examples"))

import numpy  # noqa: E402
import torch  # noqa: E402

import depth_ff  # noqa: E402
from hangar import learn, record  # noqa: E402
from hangar import topic as htopic  # noqa: E402
from hangar.nn import hgw, reference  # noqa: E402

HANGAR = os.environ.get("HANGAR_BIN", "hangar")
NOMINAL_BUOYANCY = 114.8   # msg/sim_vehicle_params.msg's default
HEAVY_DELTA = -4.0         # N: the vehicle 4 N heavier (net -2 N instead of +2 N)
DEPTH_A, DEPTH_B = 3.0, 4.0


def now():
    return time.monotonic_ns()   # CLOCK_MONOTONIC, hg_now_ns's clock


class Log(object):
    def __init__(self, path):
        self.f = open(path, "a")
        self.events = []

    def __call__(self, event, **kw):
        t = now()
        self.events.append(dict(kw, t=t, name=event))
        line = ("[%7.1f] %s %s" % ((t - T0) / 1e9, event, " ".join("%s=%s" % kv for kv in kw.items()))).rstrip()
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()


def hangar(*argv, check=True):
    p = subprocess.run([HANGAR] + list(argv), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    out = p.stdout.decode("utf-8", "replace").strip()
    if check and p.returncode != 0:
        raise RuntimeError("hangar %s: %s" % (" ".join(argv), out))
    return out


def call(target, **fields):
    return learn.call(target, 10.0, **fields)


# ---- The trainer: depth_ff.py's model and loop, in a thread ----------------------

class Trainer(threading.Thread):
    def __init__(self, log, args):
        threading.Thread.__init__(self, daemon=True)
        self.log = log
        self.a = args
        self.lock = threading.Lock()
        self.stop = False
        self.steps = 0
        self.loss = float("nan")
        self.model = None
        self.stream = None
        self.label_mean = float("nan")
        self.logged = time.monotonic()
        self.t_first = self.t_last = None   # the first and the last training step

    def run(self):
        # The stream is opened here, when the reading starts: opened earlier,
        # the topic's ring (1024 rows, 20 s) would lap the reader before its
        # first read, and those rows would count as lost.
        self.stream = learn.stream("dataset.depth_ff", self.a.batch, self.a.seq_len, replay=self.a.replay,
                                   inputs=depth_ff.INPUTS, labels=depth_ff.label_fields())
        ai = self.stream.input_fields.index("ctl.depth_hold.target.accel_z")
        if self.a.label == "trim":
            self.label = lambda Y, X: depth_ff.trim(Y, X, ai)
        else:
            self.label = lambda Y, X: depth_ff.residual(Y)
        # Wait for enough rows for the normalizer's statistics.
        while not self.stop and len(self.stream.replay) < 50 * self.a.stats_after:
            with self.lock:
                self.stream._ingest()
            time.sleep(0.05)
        if self.stop:
            return
        mean, std = depth_ff.stats(self.stream)
        self.log("trainer.stats", rows=len(self.stream.replay), mean=[round(m, 3) for m in mean],
                 std=[round(s, 3) for s in std])
        torch.manual_seed(0)
        # A 16-unit LSTM: one thread is faster than a pool, and leaves the
        # cores to the vehicle's modules (a pool took 5.5 cores here).
        torch.set_num_threads(1)
        model = depth_ff.make_model(len(self.stream.input_fields), mean, std, self.a.bound)
        opt = torch.optim.Adam([p for p in model.parameters() if p.requires_grad], lr=self.a.lr)
        h = None
        while not self.stop:
            with self.lock:   # the stream too: deploy() reads its buffer for the probes and lineage
                try:
                    c = self.stream.next_chunk(timeout=1.0)
                except learn.LearnError:
                    continue
                h = learn.carry(h, c.fresh)
                out, h = model(torch.from_numpy(c.inputs), h)
                y = torch.from_numpy(self.label(c.labels, c.inputs).astype(numpy.float32))
                m = torch.from_numpy(c.mask)
                if float(m.sum()) > 0:
                    loss = (((out[..., 0] - y) ** 2) * m).sum() / m.sum()
                    opt.zero_grad()
                    loss.backward()
                    opt.step()
                    self.loss = loss.item()
                    self.label_mean = float((y * m).sum() / m.sum())
                self.steps += 1
                self.t_last = now()
                if self.t_first is None:
                    self.t_first = self.t_last
                self.model = model
            if time.monotonic() - self.logged >= 10.0:
                self.logged = time.monotonic()
                self.log("trainer", steps=self.steps, loss="%.2e" % self.loss, label_mean="%.4f" % self.label_mean,
                         rows=self.stream.replay.total, lost=self.stream.lost)
            time.sleep(0.002)  # leave the GIL to the main thread

    def deploy(self, name):
        """Export, send and load; training waits meanwhile (the lock)."""
        self.log("deploy.begin", model=name)
        with self.lock:
            if self.model is None:
                raise RuntimeError("no model yet")
            self.model.eval()
            t = now()
            r = learn.deploy(self.model, "depth_ff", stream=self.stream, outputs=["heave"], steps=self.steps)
            self.model.train()
        r["seconds"] = (now() - t) / 1e9
        r["steps"] = self.steps
        r["loss"] = self.loss
        r["label_mean"] = self.label_mean
        r["hash"] = "%016x" % r["hash"]
        self.log("deploy", model=name, **r)
        return r


# ---- The schedule ------------------------------------------------------------------

# The mode and the model are what the schedule asked for: whether the residual
# was in the command is measured per block (the mux may have demoted it).
BLOCKS = [("B1", "assisted", "-", "nominal"), ("B2", "residual", "M1", "nominal"),
          ("B3", "residual", "M1", "heavier"), ("B4", "residual", "M2", "heavier"),
          ("B5", "assisted", "-", "heavier")]
TRANSIENTS = [("T1", "assisted", "buoyancy -4 N"), ("T1:back", "assisted", "buoyancy +4 N"),
              ("H1", "assisted to residual M1", "promotion"), ("T2", "residual M1", "buoyancy -4 N"),
              ("S2", "residual M1 to M2", "deploy and swap"),
              ("H1b", "assisted to residual M2", "promotion again, after a demotion"),
              ("H2", "residual M2 to assisted", "set_mode assisted"), ("T3", "assisted", "buoyancy +4 N")]


class Mux(object):
    """The mux's mode now, from mux.state."""

    def __init__(self):
        self.t = htopic.open_topic("mux.state", timeout=5.0, start="oldest")
        self.mode = ""

    def now(self):
        for m in self.t.read():
            self.mode = str(self.t.decode(m)["mode"])
        return self.mode


def hold(log, depth, seconds, tag):
    call("depth_hold.set", depth_m=depth)
    log("target", depth=depth, tag=tag)
    time.sleep(seconds)


def steps_block(log, tag, a, mux):
    """A 3 m hold, a step to 4 m, a step back: the block every condition gets."""
    log("block.begin", tag=tag, mode=mux.now())
    hold(log, DEPTH_A, a.hold_s, tag + ":hold")
    hold(log, DEPTH_B, a.step_s, tag + ":down")
    hold(log, DEPTH_A, a.step_s, tag + ":up")
    log("block.end", tag=tag)


def transient(log, tag, seconds):
    """A disturbance at the 3 m hold, logged just before it is made."""
    log("transient", tag=tag, seconds=seconds)


def buoyancy(log, value, tag, seconds):
    transient(log, tag, seconds)
    hangar("param", "set", "sim_vehicle", "buoyancy=%g" % value)
    log("buoyancy", value=value, tag=tag)
    time.sleep(seconds)


def set_mode(log, mode, h=0):
    try:
        r = call("mux.set_mode", mode=mode, hash=h)
    except learn.LearnError as e:
        log("set_mode.refused", mode=mode, hash="%016x" % h, error=str(e))
        return False
    log("set_mode", mode=mode, hash="%016x" % h, reply=r)
    return True


def arm(log):
    for _ in range(40):
        try:
            call("safety.arm")
            log("armed")
            return
        except learn.LearnError as e:
            last = str(e)
            time.sleep(0.5)
    raise RuntimeError("arm refused: %s" % last)


def schedule(log, a, trainer):
    mux = Mux()
    arm(log)
    set_mode(log, "assisted")
    hold(log, DEPTH_A, a.settle_s, "descent")
    trainer.start()
    # Training data: steps across the range, assisted, nominal buoyancy.
    log("phase", what="train")
    t_end = time.monotonic() + a.train_s
    targets = [2.5, 4.0, 3.0, 5.0, 3.5, 2.0, 4.5, 3.0]
    k = 0
    while time.monotonic() < t_end:
        hold(log, targets[k % len(targets)], min(a.train_hold_s, max(1.0, t_end - time.monotonic())), "train")
        k += 1
    hold(log, DEPTH_A, a.hold_s, "train:return")
    m1 = trainer.deploy("M1")
    if m1["state"] != "done":
        raise RuntimeError("M1 rejected: %s" % m1)

    log("phase", what="B1 assisted, nominal")
    steps_block(log, "B1", a, mux)
    log("phase", what="T1 assisted, 4 N heavier and back")
    call("depth_hold.set", depth_m=DEPTH_A)
    buoyancy(log, NOMINAL_BUOYANCY + HEAVY_DELTA, "T1", a.transient_s)
    buoyancy(log, NOMINAL_BUOYANCY, "T1:back", a.transient_s)

    log("phase", what="H1 promotion: residual:depth_ff with M1")
    transient(log, "H1", a.hold_s)
    set_mode(log, "residual:depth_ff", int(m1["hash"], 16))
    time.sleep(a.hold_s)
    log("phase", what="B2 residual M1, nominal")
    steps_block(log, "B2", a, mux)
    log("phase", what="T2 residual M1, 4 N heavier")
    call("depth_hold.set", depth_m=DEPTH_A)
    buoyancy(log, NOMINAL_BUOYANCY + HEAVY_DELTA, "T2", a.transient_s)
    log("phase", what="B3 residual M1, heavier")
    steps_block(log, "B3", a, mux)
    # Trained on the heavier vehicle since T2; the deploy lands during a hold.
    call("depth_hold.set", depth_m=DEPTH_A)
    time.sleep(a.hold_s / 2)
    log("phase", what="S2 deploy M2 during the hold")
    transient(log, "S2", a.hold_s)
    m2 = trainer.deploy("M2")
    if m2["state"] == "done":
        time.sleep(a.hold_s / 2)
        if not mux.now().startswith("residual:"):
            # The mux demoted the policy on the way: what an operator would do
            # with a newer model, promote it.
            log("phase", what="H1b the policy was demoted: residual:depth_ff again, with M2", mode=mux.now())
            transient(log, "H1b", a.hold_s)
            set_mode(log, "residual:depth_ff", int(m2["hash"], 16))
            time.sleep(a.hold_s)
        log("reset_state.begin")
        call("policy_depth_ff.reset_state")
        log("reset_state", model="M2")
        time.sleep(a.hold_s / 2)
        log("phase", what="B4 residual M2, heavier")
        steps_block(log, "B4", a, mux)
    log("phase", what="H2 back to assisted")
    call("depth_hold.set", depth_m=DEPTH_A)
    transient(log, "H2", a.hold_s)
    set_mode(log, "assisted")
    time.sleep(a.hold_s)
    log("phase", what="B5 assisted, heavier")
    steps_block(log, "B5", a, mux)
    log("phase", what="T3 assisted, back to nominal")
    call("depth_hold.set", depth_m=DEPTH_A)
    buoyancy(log, NOMINAL_BUOYANCY, "T3", a.transient_s)
    trainer.stop = True
    return m1, m2


# ---- The analysis, from the recorded session --------------------------------------

class Session(object):
    """The recorder's session: each topic's file as one numpy array."""

    def __init__(self, path):
        self.path = path
        self._a, self._id = {}, {}

    def arr(self, name):
        if name not in self._a:
            p = os.path.join(self.path, name + ".hgr")
            self._a[name] = None
            if os.path.exists(p):
                r = record.open_recording(p)
                self._id[name] = r.header.topic_id
                if r.count:
                    self._a[name] = r.array()
        return self._a[name]

    def tid(self, name):
        self.arr(name)
        return self._id.get(name)

    def pub(self, name):
        return self.arr(name)["publish_ns"].astype(numpy.int64)

    def ref(self, src, dst):
        """For each record of src, the index in dst of the message it names
        as an input, -1 when it names none (or one not recorded)."""
        a, b = self.arr(src), self.arr(dst)
        if a is None or b is None:
            return numpy.full(0 if a is None else len(a), -1, numpy.int64)
        inp = a["inputs"]
        m = (inp["topic_id"] == self.tid(dst)) & (numpy.arange(4)[None, :] < a["n_inputs"][:, None])
        seq = inp["seq"][numpy.arange(len(a)), m.argmax(1)]
        i = numpy.minimum(numpy.searchsorted(b["seq"], seq), len(b) - 1)
        return numpy.where(m.any(1) & (b["seq"][i] == seq), i, -1).astype(numpy.int64)


def via(i, j):
    """i: indices into X (-1 none); j: X's indices into Y. Indices into Y."""
    if len(j) == 0:
        return numpy.full(len(i), -1, numpy.int64)
    return numpy.where(i >= 0, j[numpy.maximum(i, 0)], -1)


def ms_stats(ns):
    a = numpy.asarray(ns, numpy.float64) / 1e6
    if len(a) == 0:
        return None
    return {"n": int(len(a)), "median_ms": round(float(numpy.median(a)), 3),
            "p99_ms": round(float(numpy.percentile(a, 99)), 3), "max_ms": round(float(a.max()), 3)}


def events(ev, name, **match):
    return [e for e in ev if e["name"] == name and all(e.get(k) == v for k, v in match.items())]


def step_metrics(t, z, t0, t1, target, start):
    k = (t >= t0) & (t < t1)
    tt, e = t[k], z[k] - target
    if len(tt) < 2:
        return {}
    dt = float(numpy.median(numpy.diff(tt))) / 1e9
    tail = tt >= t1 - min(8e9, 0.4 * (t1 - t0))
    out = {"seconds": round((t1 - t0) / 1e9, 1),
           "ss_mean_cm": round(100 * float(e[tail].mean()), 2),
           "ss_rms_cm": round(100 * float(numpy.sqrt((e[tail] ** 2).mean())), 2),
           "iae_m_s": round(float(numpy.abs(e).sum() * dt), 3),
           "peak_cm": round(100 * float(numpy.abs(e).max()), 2)}
    if start is not None:
        d = math.copysign(1, target - start)
        out["overshoot_pct"] = round(100 * max(0.0, float((d * e).max())) / abs(target - start), 2)
    outside = numpy.nonzero(numpy.abs(e) > 0.05)[0]
    if len(outside) == 0:
        out["within_5cm_s"] = 0.0
    elif outside[-1] == len(tt) - 1:
        out["within_5cm_s"] = None   # not within the window
    else:
        out["within_5cm_s"] = round(float(tt[outside[-1] + 1] - t0) / 1e9, 2)
    return out


def depth_error(s, ev, res):
    """Truth (sim.pose) against the target, per block segment and transient."""
    pose = s.arr("sim.pose")
    t, z = pose["publish_ns"].astype(numpy.int64), pose["pos"][:, 2].astype(numpy.float64)
    targets = events(ev, "target")
    blocks = {}
    for k, e in enumerate(targets):
        blk, _, seg = e["tag"].partition(":")
        if blk not in [b[0] for b in BLOCKS] or not seg:
            continue
        end = events(ev, "block.end", tag=blk)
        t1 = targets[k + 1]["t"] if k + 1 < len(targets) else end[0]["t"]
        if end:
            t1 = min(t1, end[0]["t"])
        blocks.setdefault(blk, {})[seg] = step_metrics(t, z, e["t"], t1, e["depth"],
                                                       None if seg == "hold" else targets[k - 1]["depth"])
    res["blocks"] = blocks
    tr = {}
    for e in events(ev, "transient"):
        tr[e["tag"]] = step_metrics(t, z, e["t"], e["t"] + int(e["seconds"] * 1e9), DEPTH_A, None)
    res["transients"] = tr


def trim_split(s, ev, res):
    """Who carried the trim in each block's hold: the integrator, the policy."""
    tg, pw, cw = s.arr("ctl.depth_hold.target"), s.arr("policy.depth_ff.wrench"), s.arr("ctl.wrench")
    out = {}
    for blk in [b[0] for b in BLOCKS]:
        h = events(ev, "target", tag=blk + ":hold")
        d = events(ev, "target", tag=blk + ":down")
        if not h or not d:
            continue
        t1 = d[0]["t"]
        t0 = t1 - int(0.4 * (t1 - h[0]["t"]))

        def mean(a, field):
            if a is None:
                return None
            k = (a["publish_ns"] >= t0) & (a["publish_ns"] < t1)
            return round(float(a[field][k].mean()), 4) if k.any() else None
        out[blk] = {"integral": mean(tg, "integral"), "policy_heave": mean(pw, "heave"),
                    "ctl_wrench_heave": mean(cw, "heave")}
    res["trim_carried_by"] = out
    # Whether the residual was in the command, per block: the share of
    # ctl.wrench messages that name a policy.depth_ff.wrench.
    w_p = s.ref("ctl.wrench", "policy.depth_ff.wrench")
    share = {}
    for blk in [b[0] for b in BLOCKS]:
        b0, b1 = events(ev, "block.begin", tag=blk), events(ev, "block.end", tag=blk)
        if b0 and b1 and cw is not None:
            k = (cw["publish_ns"] >= b0[0]["t"]) & (cw["publish_ns"] < b1[0]["t"])
            share[blk] = round(100.0 * float((w_p[k] >= 0).mean()), 1) if k.any() else None
    res["residual_share_pct"] = share


def control_latency(s, res):
    """The control loop: nav.depth's publish to thr.cmd's, through the chain
    of messages each one names; and the policy's branch of it."""
    need = ["thr.cmd", "ctl.wrench", "ctl.depth_hold", "nav.state", "nav.depth"]
    if any(s.arr(n) is None for n in need):
        res["control_latency"] = {"error": "a topic of the chain was not recorded"}
        return
    p = {n: s.pub(n) for n in need}
    c_w = s.ref("thr.cmd", "ctl.wrench")
    c_d = via(c_w, s.ref("ctl.wrench", "ctl.depth_hold"))
    c_s = via(c_d, s.ref("ctl.depth_hold", "nav.state"))
    c_n = via(c_s, s.ref("nav.state", "nav.depth"))
    ok = c_n >= 0
    out = {"thr.cmd records": int(len(ok)), "without the whole chain": int((~ok).sum())}
    tc = p["thr.cmd"][ok]
    w, d, st, n = c_w[ok], c_d[ok], c_s[ok], c_n[ok]
    out["nav.depth to nav.state"] = ms_stats(p["nav.state"][st] - p["nav.depth"][n])
    out["nav.state to ctl.depth_hold"] = ms_stats(p["ctl.depth_hold"][d] - p["nav.state"][st])
    out["ctl.depth_hold to ctl.wrench"] = ms_stats(p["ctl.wrench"][w] - p["ctl.depth_hold"][d])
    out["ctl.wrench to thr.cmd"] = ms_stats(tc - p["ctl.wrench"][w])
    out["nav.depth to thr.cmd"] = ms_stats(tc - p["nav.depth"][n])
    if s.arr("policy.depth_ff.wrench") is not None:
        pp = s.pub("policy.depth_ff.wrench")
        w_p = s.ref("ctl.wrench", "policy.depth_ff.wrench")
        p_s = s.ref("policy.depth_ff.wrench", "nav.state")
        k = w_p >= 0
        out["policy.wrench to ctl.wrench"] = ms_stats(p["ctl.wrench"][k] - pp[w_p[k]])
        k2 = k & (via(w_p, p_s) >= 0)
        out["nav.state to policy.wrench"] = ms_stats(pp[w_p[k2]] - p["nav.state"][via(w_p, p_s)[k2]])
        out["nav.state to ctl.wrench, by the policy"] = ms_stats(p["ctl.wrench"][k2] - p["nav.state"][via(w_p, p_s)[k2]])
    res["control_latency"] = out


def loads(s):
    """Per version: when its load was checking, warming and done (or rejected)."""
    ld = s.arr("policy.depth_ff.load")
    out = {}
    if ld is None:
        return out
    names = {1: "checking", 2: "warming", 3: "done", 4: "rejected"}
    for i in range(len(ld)):
        out.setdefault(int(ld["version"][i]), {})[names.get(int(ld["state"][i]), "?")] = int(ld["since_ns"][i])
    return out


def loop_latency(s, ev, res):
    """The learning loop, per model: from the newest row it was trained on to
    its first output, and to the first thruster command that carries it."""
    act, pw, cw, tc, ds = (s.arr(n) for n in ("policy.depth_ff.active", "policy.depth_ff.wrench", "ctl.wrench",
                                               "thr.cmd", "dataset.depth_ff"))
    if act is None or pw is None:
        res["loop_latency"] = {"error": "no model was announced"}
        return
    p_a = s.ref("policy.depth_ff.wrench", "policy.depth_ff.active")
    w_p = s.ref("ctl.wrench", "policy.depth_ff.wrench")
    w_a = via(w_p, p_a)
    c_w = s.ref("thr.cmd", "ctl.wrench")
    c_a = via(c_w, w_a)
    ld = loads(s)
    out = []
    for k in range(len(act)):
        v = int(act["version"][k])
        newest = int(act["newest_row_stamp_ns"][k])
        e = {"version": v, "hash": "%016x" % int(act["hash"][k]), "rows": int(act["rows"][k])}
        dep = [x for x in events(ev, "deploy") if x.get("version") == v]
        beg = [x for x in events(ev, "deploy.begin") if dep and x["model"] == dep[0]["model"]]
        if dep:
            e["model"] = dep[0]["model"]

        def after(t):
            return None if t is None else round((int(t) - newest) / 1e6, 2)
        if ds is not None:
            i = numpy.nonzero(ds["anchor_stamp_ns"] == newest)[0]
            if len(i):
                e["its row published"] = after(ds["publish_ns"][i[0]])
        e["deploy called"] = after(beg[0]["t"]) if beg else None
        e["load: checking"] = after(ld.get(v, {}).get("checking"))
        e["load: warming"] = after(ld.get(v, {}).get("warming"))
        e["swapped (policy.active)"] = after(act["swapped_ns"][k])
        i = numpy.nonzero(p_a == k)[0]
        e["first policy.wrench of the model"] = after(pw["publish_ns"][i[0]]) if len(i) else None
        i = numpy.nonzero(w_a == k)[0]
        e["first ctl.wrench carrying it"] = after(cw["publish_ns"][i[0]]) if len(i) else None
        i = numpy.nonzero(c_a == k)[0]
        e["first thr.cmd carrying it"] = after(tc["publish_ns"][i[0]]) if len(i) else None
        e["ctl.wrench messages carrying it"] = int((w_a == k).sum())
        out.append(e)
    res["loop_latency"] = {"unit": "ms after the stamp of the newest row trained on", "models": out}


def tick_stats(s, name, lo, hi):
    a = s.arr(name)
    if a is None:
        return None
    k = (a["publish_ns"] >= lo) & (a["publish_ns"] < hi)
    pub = a["publish_ns"][k].astype(numpy.int64)
    if len(pub) < 3:
        return {"messages": int(len(pub))}
    d = numpy.diff(pub) / 1e6
    o = {"messages": int(len(pub)), "interval_max_ms": round(float(d.max()), 3),
         "interval_min_ms": round(float(d.min()), 3), "interval_sd_ms": round(float(d.std()), 4),
         "intervals_over_30ms": int((d > 30).sum())}
    if name == "policy.depth_ff.wrench":   # stamped with its anchor's stamp
        lat = (pub - a["stamp_ns"][k].astype(numpy.int64)) / 1e6
        o["anchor_to_output_median_ms"] = round(float(numpy.median(lat)), 4)
        o["anchor_to_output_max_ms"] = round(float(lat.max()), 4)
    return o


def swap_cost(s, ev, res):
    """Tick jitter while a model is exported, sent and loaded (checks,
    warm-up, swap, 200 ms more) against the 10 s before its deploy began."""
    ld = loads(s)
    out = {}
    names = ["policy.depth_ff.wrench", "ctl.depth_hold", "ctl.wrench", "thr.cmd", "nav.state"]
    for dep in events(ev, "deploy"):
        v = ld.get(dep.get("version"), {})
        beg = events(ev, "deploy.begin", model=dep["model"])
        end = v.get("done") or v.get("rejected")
        if not (beg and end and "checking" in v):
            continue
        e = {"export and send (trainer, same machine)_ms": round((v["checking"] - beg[0]["t"]) / 1e6, 1),
             "checks_ms": round((v.get("warming", end) - v["checking"]) / 1e6, 2),
             "warm_up_ms": round((end - v.get("warming", end)) / 1e6, 2)}
        windows = {"before (10 s)": (beg[0]["t"] - 10_500_000_000, beg[0]["t"] - 500_000_000),
                   "load": (beg[0]["t"], end + 200_000_000)}
        for wn, (lo, hi) in windows.items():
            e[wn] = {n: tick_stats(s, n, lo, hi) for n in names}
        out[dep["model"]] = e
    res["swap_cost"] = out or {"error": "no load recorded"}


def mismatch(s, ev, res):
    """Train against serve. Inputs: the message of each input part the policy
    read at a tick (its output names it) against the one the pairer joined
    into that anchor's row. Outputs: the deployed file run by python/hangar's
    reference on the pairer's rows against what the policy published, from
    the reset_state on (the tick the reset fell on is not in the stream: it
    is the one in the call's window whose outputs agree)."""
    out = {}
    pw, ds = s.arr("policy.depth_ff.wrench"), s.arr("dataset.depth_ff")
    if pw is None or ds is None:
        res["mismatch"] = {"error": "policy.depth_ff.wrench or dataset.depth_ff not recorded"}
        return
    # Inputs, by anchor seq.
    ns, tg = s.arr("nav.state"), s.arr("ctl.depth_hold.target")
    p_n = s.ref("policy.depth_ff.wrench", "nav.state")
    p_t = s.ref("policy.depth_ff.wrench", "ctl.depth_hold.target")
    k = (p_n >= 0) & (p_t >= 0)
    anchor = ns["seq"][p_n[k]]
    used = tg["seq"][p_t[k]].astype(numpy.int64)
    i = numpy.minimum(numpy.searchsorted(ds["anchor_seq"], anchor), len(ds) - 1)
    both = ds["anchor_seq"][i] == anchor
    joined = ds["ctl.depth_hold.target._seq"][i].astype(numpy.int64)
    d = (used - joined)[both]
    out["inputs"] = {"anchors with an output and a row": int(both.sum()),
                     "outputs without a row": int((~both).sum()),
                     "rows without an output": int(len(ds) - both.sum()),
                     "same ctl.depth_hold.target message": int((d == 0).sum()),
                     "another message": int((d != 0).sum()),
                     "policy read a newer one": int((d > 0).sum()), "policy read an older one": int((d < 0).sum())}
    if (d != 0).any():
        # What that did to the input: accel_z read against accel_z joined.
        ai = p_t[k][both][d != 0]
        da = numpy.abs(tg["accel_z"][ai] - ds["ctl.depth_hold.target.accel_z"][i[both][d != 0]])
        out["inputs"]["accel_z difference on those, max"] = float(da.max())
        out["inputs"]["accel_z difference on those, median"] = float(numpy.median(da))
    # Outputs.
    rs, rb = events(ev, "reset_state"), events(ev, "reset_state.begin")
    dep = events(ev, "deploy", model="M2")
    if not (rs and rb and dep):
        out["outputs"] = {"error": "no reset_state after M2"}
        res["mismatch"] = out
        return
    path = os.path.join(s.path, "weights", "%s.hgw" % dep[0]["hash"])
    if not os.path.exists(path):
        out["outputs"] = {"error": "%s not in the session" % path}
        res["mismatch"] = out
        return
    g = hgw.read(path).graph
    fields = [b.field for b in g.inputs]
    stamps = ds["anchor_stamp_ns"].astype(numpy.int64)
    pstamp = pw["stamp_ns"].astype(numpy.int64)
    heave = pw["heave"]

    def run(first_row, n):
        m = reference.Model(g)
        m.reset()
        diffs = []
        for r in range(first_row, min(first_row + n, len(ds))):
            if int(ds["flags"][r]) & learn.RESET and r != first_row:
                m.reset()
            y = m.step([[float(ds[f][r]) for f in fields]])[0][0]
            j = numpy.searchsorted(pstamp, stamps[r])
            if j < len(pw) and pstamp[j] == stamps[r]:
                diffs.append(abs(y - float(heave[j])))
        return numpy.array(diffs)

    cand = numpy.nonzero((pw["publish_ns"] >= rb[0]["t"] - 40_000_000) & (pw["publish_ns"] <= rs[0]["t"] + 40_000_000))[0]
    best = None
    for j in cand:
        r = numpy.searchsorted(stamps, pstamp[j])
        if r >= len(ds) or stamps[r] != pstamp[j]:
            continue
        dd = run(r, 100)
        if len(dd) and (best is None or dd.max() < best[0]):
            best = (float(dd.max()), int(r))
    if best is None:
        out["outputs"] = {"error": "no row for an output in the reset's window"}
    else:
        dd = run(best[1], 3000)
        out["outputs"] = {"model": dep[0]["hash"], "ticks in the reset's window": int(len(cand)),
                          "anchors compared": int(len(dd)), "heave difference, max": float(dd.max()),
                          "heave difference, median": float(numpy.median(dd)),
                          "over 1e-4": int((dd > 1e-4).sum()), "over 1e-5": int((dd > 1e-5).sum()),
                          "largest at a wrong reset tick": None}
        # How well the search separates: the second best candidate.
        others = []
        for j in cand:
            r = numpy.searchsorted(stamps, pstamp[j])
            if r < len(ds) and stamps[r] == pstamp[j] and r != best[1]:
                dd2 = run(r, 100)
                if len(dd2):
                    others.append(float(dd2.max()))
        if others:
            out["outputs"]["largest at a wrong reset tick"] = min(others)
    res["mismatch"] = out


def lineage(s, ev, res):
    """Every announced model's file against its announcement; then one
    thruster command that carried the newest model's residual (the middle
    one of them) followed back: the mux's command, the policy's output, the
    model, its file, its rows, the sensor messages of the newest row and of
    the tick itself."""
    act = s.arr("policy.depth_ff.active")
    models = []
    for i in range(0 if act is None else len(act)):
        h = int(act["hash"][i])
        path = os.path.join(s.path, "weights", "%016x.hgw" % h)
        e = {"version": int(act["version"][i]), "cause": int(act["cause"][i]), "hash": "%016x" % h,
             "rows": int(act["rows"][i]), "file_in_session": os.path.exists(path)}
        if os.path.exists(path):
            hd = hgw.read(path).header
            e["header"] = {"rows": hd.rows, "steps": hd.steps, "dataset": hd.dataset,
                           "dataset_hash": "%016x" % hd.dataset_hash, "parent_hash": "%016x" % hd.parent_hash,
                           "newest_row_stamp_ns": hd.newest_row_stamp_ns, "trainer": hd.trainer_program,
                           "lineage": [{"session": ln.session, "rows": ln.rows, "first_anchor_seq": ln.first_seq,
                                        "last_anchor_seq": ln.last_seq} for ln in hd.lineage]}
            e["file agrees with the announcement"] = bool(
                hd.rows == int(act["rows"][i]) and hd.dataset_hash == int(act["dataset_hash"][i])
                and hd.newest_row_stamp_ns == int(act["newest_row_stamp_ns"][i]))
        models.append(e)
    out = {"models": models}
    # One command, followed back.
    tc = s.arr("thr.cmd")
    if tc is not None and act is not None and s.arr("policy.depth_ff.wrench") is not None:
        c_w = s.ref("thr.cmd", "ctl.wrench")
        w_p = s.ref("ctl.wrench", "policy.depth_ff.wrench")
        w_d = s.ref("ctl.wrench", "ctl.depth_hold")
        p_a = s.ref("policy.depth_ff.wrench", "policy.depth_ff.active")
        c_a = via(via(c_w, w_p), p_a)
        carried = via(c_w, w_p) >= 0
        out["thr.cmd carrying a residual"] = int(carried.sum())
        out["of them leading back to a policy.active"] = int((carried & (c_a >= 0)).sum())
        newest = numpy.nonzero(c_a == c_a.max())[0] if (c_a >= 0).any() else []
    else:
        newest = []
    if len(newest):
        c = int(newest[len(newest) // 2])
        p_n = s.ref("policy.depth_ff.wrench", "nav.state")
        p_t = s.ref("policy.depth_ff.wrench", "ctl.depth_hold.target")
        n_d = s.ref("nav.state", "nav.depth")
        n_a = s.ref("nav.state", "nav.attitude")
        n_i = s.ref("nav.state", "nav.imu")
        ns, ds = s.arr("nav.state"), s.arr("dataset.depth_ff")
        tr = {"thr.cmd": {"seq": int(tc["seq"][c]), "t_s": round((int(tc["publish_ns"][c]) - T0) / 1e9, 2),
                          "pwm": [int(x) for x in tc["pwm"][c]]}}
        chain = ["thr.cmd"]

        def step(name, idx, extra=None):
            if idx < 0:
                tr[name] = None
                return False
            a = s.arr(name.split(" ")[0])
            tr[name] = dict({"seq": int(a["seq"][idx])}, **(extra(a, idx) if extra else {}))
            chain.append(name)
            return True
        w = int(c_w[c])
        ok = step("ctl.wrench", w, lambda a, i: {"heave": round(float(a["heave"][i]), 5)})
        if ok:
            step("ctl.depth_hold", int(w_d[w]), lambda a, i: {"heave": round(float(a["heave"][i]), 5)})
            p = int(w_p[w])
            ok = step("policy.depth_ff.wrench", p, lambda a, i: {"heave": round(float(a["heave"][i]), 5)})
        if ok:
            step("ctl.depth_hold.target", int(p_t[p]), lambda a, i: {"accel_z": round(float(a["accel_z"][i]), 5)})
            n = int(p_n[p])
            if step("nav.state (the tick's anchor)", n):
                for nm, j in (("nav.depth", n_d), ("nav.attitude", n_a), ("nav.imu", n_i)):
                    step(nm + " (the tick's)", int(j[n]))
            k = int(p_a[p])
            ok = step("policy.depth_ff.active", k, lambda a, i: {
                "version": int(a["version"][i]), "hash": "%016x" % int(a["hash"][i]), "rows": int(a["rows"][i])})
        if ok:
            m = [x for x in models if x["version"] == int(act["version"][k]) and x["hash"] == "%016x" % int(act["hash"][k])]
            tr["weights file"] = {"in the session": bool(m and m[0]["file_in_session"]),
                                  "agrees": bool(m and m[0].get("file agrees with the announcement"))}
            if m and m[0].get("header") and ds is not None:
                chain.append("weights file")
                rows = []
                for ln in m[0]["header"]["lineage"]:
                    inr = (ds["anchor_seq"] >= ln["first_anchor_seq"]) & (ds["anchor_seq"] <= ln["last_anchor_seq"])
                    rows.append({"session": ln["session"], "anchor_seq": [ln["first_anchor_seq"], ln["last_anchor_seq"]],
                                 "rows the buffer held": ln["rows"], "of them in the recording": int(inr.sum())})
                tr["rows"] = rows
                r = numpy.nonzero(ds["anchor_stamp_ns"] == m[0]["header"]["newest_row_stamp_ns"])[0]
                if len(r):
                    chain.append("rows")
                    r = int(r[0])
                    tr["newest row"] = {"anchor_seq": int(ds["anchor_seq"][r]),
                                        "ctl.depth_hold.target seq": int(ds["ctl.depth_hold.target._seq"][r]),
                                        "ctl.depth_hold seq": int(ds["ctl.depth_hold._seq"][r]),
                                        "thr.out seq": int(ds["thr.out._seq"][r])}
                    n = int(numpy.searchsorted(ns["seq"], ds["anchor_seq"][r]))
                    if n < len(ns) and ns["seq"][n] == ds["anchor_seq"][r]:
                        if step("nav.state (the newest row's anchor)", n):
                            for nm, j in (("nav.depth", n_d), ("nav.attitude", n_a), ("nav.imu", n_i)):
                                step(nm + " (the newest row's)", int(j[n]))
        tr["chain"] = chain
        tr["complete"] = bool(sum(1 for x in chain if x.endswith("row's)")) == 3
                              and sum(1 for x in chain if x.endswith("tick's)")) == 3)
        out["one command followed back"] = tr
    res["lineage"] = out


def demotions(s, ev, res):
    ms = s.arr("mux.state")
    if ms is None:
        res["demotions"] = {"error": "mux.state not recorded"}
        return
    mode = numpy.array([m.split(b"\0")[0].decode() for m in ms["mode"]])
    asked = [(e["t"], e["mode"]) for e in events(ev, "set_mode")]
    out = []
    for i in numpy.nonzero(mode[1:] != mode[:-1])[0] + 1:
        t = int(ms["publish_ns"][i])
        e = {"t_s": round((t - T0) / 1e9, 2), "from": str(mode[i - 1]), "to": str(mode[i]),
             "asked": any(abs(t - ta) < 1_000_000_000 and m == mode[i] for ta, m in asked)}
        if not e["asked"]:
            before = [ta for ta, m in asked if ta <= t and m == mode[i - 1]]
            e["seconds_after_it_was_asked_for"] = round((t - before[-1]) / 1e9, 2) if before else None
        out.append(e)
    res["demotions"] = {"mode changes": out, "demotions": [x for x in out if not x["asked"]],
                        "mux.state with residual_active": int((ms["residual_active"] != 0).sum()),
                        "mux.state at the residual envelope": int(((ms["limits"] >> 13) & 0x3f != 0).sum()),
                        "mux.state in a residual mode": int(numpy.char.startswith(mode, "residual:").sum())}


def rows(s, ev, res, trainer=None):
    """The training rows: what the pairer wrote, what the recorder kept, what
    the trainer's stream got; and the two labels over each block's rows."""
    ds = s.arr("dataset.depth_ff")
    if ds is None:
        res["rows"] = {"error": "dataset.depth_ff not recorded"}
        return
    span = int(ds["anchor_seq"][-1]) - int(ds["anchor_seq"][0]) + 1
    out = {"rows recorded": int(len(ds)), "recorder gaps": int(ds["gap"].sum()),
           "anchors from the first row to the last": span, "anchors without a row": span - int(len(ds)),
           "masked (the vehicle moving, or outputs off)": int((ds["flags"] & learn.MASKED != 0).sum()),
           "resets": int((ds["flags"] & learn.RESET != 0).sum()),
           "anchor to row published, median_ms": round(float(numpy.median(
               ds["publish_ns"].astype(numpy.int64) - ds["anchor_stamp_ns"].astype(numpy.int64))) / 1e6, 3)}
    if trainer is not None:
        out["the trainer's stream"] = trainer
    Y = numpy.concatenate([ds[depth_ff.CTL_HEAVE][:, None].astype(numpy.float64)]
                          + [ds["thr.out.pwm"][:, ch][:, None].astype(numpy.float64) for ch in sorted(depth_ff.VERTICAL)], 1)
    short = depth_ff.residual(Y)
    trim = depth_ff.applied_heave(Y) + depth_ff.FF * ds["ctl.depth_hold.target.accel_z"]
    keep = ds["flags"] & learn.MASKED == 0
    lab = {}
    spans = [("train", events(ev, "phase", what="train"), events(ev, "deploy.begin", model="M1"))]
    spans += [(b[0], events(ev, "block.begin", tag=b[0]), events(ev, "block.end", tag=b[0])) for b in BLOCKS]
    for name, b0, b1 in spans:
        if not (b0 and b1):
            continue
        k = keep & (ds["anchor_stamp_ns"] >= b0[0]["t"]) & (ds["anchor_stamp_ns"] < b1[0]["t"])
        if k.any():
            lab[name] = {"rows with loss": int(k.sum()),
                         "shortfall (docs/09) mean": round(float(short[k].mean()), 5),
                         "shortfall sd": round(float(short[k].std()), 5),
                         "shortfall abs max": round(float(numpy.abs(short[k]).max()), 5),
                         "trim mean": round(float(trim[k].mean()), 5), "trim sd": round(float(trim[k].std()), 5)}
    out["labels"] = lab
    res["rows"] = out


def recorder_gaps(s, res):
    """Messages the recorder lost, per topic: what the analysis could not see."""
    out = {}
    for f in sorted(os.listdir(s.path)):
        if f.endswith(".hgr"):
            a = s.arr(f[:-4])
            if a is not None and int(a["gap"].sum()):
                out[f[:-4]] = int(a["gap"].sum())
    res["recorder_gaps"] = out


def analyse(session, ev, trainer=None):
    s = Session(session)
    res = {"session": session}
    for f in (depth_error, trim_split, control_latency, loop_latency, swap_cost, mismatch, lineage, demotions):
        f(s, ev, res) if f is not control_latency else f(s, res)
    rows(s, ev, res, trainer)
    recorder_gaps(s, res)
    return res


# ---- The report --------------------------------------------------------------------

def fmt(x, nd=1):
    return "-" if x is None else ("%.*f" % (nd, x))


def markdown(res):
    L = []
    b = res.get("blocks", {})
    L += ["| Block | Mode asked | Model | Buoyancy | Residual in the command (% of ticks) | Hold: mean / rms (cm) | "
          "3 to 4 m: IAE (m s), overshoot, within 5 cm | 4 to 3 m: IAE (m s), overshoot, within 5 cm |",
          "|---|---|---|---|---|---|---|---|"]
    for tag, mode, model, buoy in BLOCKS:
        x = b.get(tag)
        if not x:
            continue

        def seg(g):
            return "%s, %s %%, %s s" % (fmt(g.get("iae_m_s"), 2), fmt(g.get("overshoot_pct")), fmt(g.get("within_5cm_s")))
        h = x.get("hold", {})
        L.append("| %s | %s | %s | %s | %s | %s / %s | %s | %s |" % (
            tag, mode, model, buoy, fmt(res.get("residual_share_pct", {}).get(tag)),
            fmt(h.get("ss_mean_cm"), 2), fmt(h.get("ss_rms_cm"), 2),
            seg(x.get("down", {})), seg(x.get("up", {}))))
    L += ["", "| Transient at the 3 m hold | Mode | What | Peak (cm) | IAE (m s) | Within 5 cm after (s) | "
              "End: mean / rms (cm) |", "|---|---|---|---|---|---|---|"]
    for tag, mode, what in TRANSIENTS:
        x = res.get("transients", {}).get(tag)
        if x:
            L.append("| %s | %s | %s | %s | %s | %s | %s / %s |" % (
                tag, mode, what, fmt(x.get("peak_cm")), fmt(x.get("iae_m_s"), 2), fmt(x.get("within_5cm_s")),
                fmt(x.get("ss_mean_cm"), 2), fmt(x.get("ss_rms_cm"), 2)))
    L += ["", "| Block | Integrator | Policy's heave | ctl.wrench heave |", "|---|---|---|---|"]
    for tag, x in res.get("trim_carried_by", {}).items():
        L.append("| %s | %s | %s | %s |" % (tag, fmt(x["integral"], 4), fmt(x["policy_heave"], 4),
                                            fmt(x["ctl_wrench_heave"], 4)))
    ll = res.get("loop_latency", {}).get("models", [])
    if ll:
        keys = [k for k in ll[0] if k not in ("version", "hash", "rows", "model", "ctl.wrench messages carrying it")]
        L += ["", "| After the newest row trained on (ms) | " + " | ".join(
            "%s (v%d)" % (m.get("model", "?"), m["version"]) for m in ll) + " |", "|---|" + "---|" * len(ll)]
        for k in keys:
            L.append("| %s | %s |" % (k, " | ".join(fmt(m.get(k), 1) for m in ll)))
    sw = res.get("swap_cost", {})
    if "error" not in sw:
        L += ["", "| Load | Window | Topic | Messages | Interval min / max (ms) | sd (ms) | Over 30 ms | "
                  "Anchor to output median / max (ms) |", "|---|---|---|---|---|---|---|---|"]
        for model, e in sw.items():
            for wn in ("before (10 s)", "load"):
                for n, x in e[wn].items():
                    if not x or "interval_max_ms" not in x:
                        continue
                    L.append("| %s | %s | %s | %d | %s / %s | %s | %d | %s |" % (
                        model, wn, n, x["messages"], fmt(x["interval_min_ms"], 2), fmt(x["interval_max_ms"], 2),
                        fmt(x["interval_sd_ms"], 3), x["intervals_over_30ms"],
                        "%s / %s" % (fmt(x["anchor_to_output_median_ms"], 3), fmt(x["anchor_to_output_max_ms"], 3))
                        if "anchor_to_output_median_ms" in x else "-"))
    return "\n".join(L) + "\n"


def main():
    global T0
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", help="the manager's HANGAR_ROOT")
    ap.add_argument("--out", required=True, help="where events.log, events.json, report.json and report.md go")
    ap.add_argument("--quick", action="store_true", help="every phase shortened: a smoke run")
    ap.add_argument("--label", choices=("trim", "shortfall"), default="trim")
    ap.add_argument("--bound", type=float, default=None,
                    help="depth_ff.py's bounded model (a tanh, +-BOUND): the policy's Arch must be its")
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--seq-len", type=int, default=50)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--replay", default="3min")
    ap.add_argument("--analyse", help="a session directory holding events.json: the analysis alone")
    a = ap.parse_args()
    q = 0.4 if a.quick else 1.0
    a.settle_s, a.train_s, a.train_hold_s = 40 * q, 180 * q, 15 * q
    a.hold_s, a.step_s, a.transient_s = 20 * q, 25 * q, 40 * q
    a.stats_after = 20 * q
    os.makedirs(a.out, exist_ok=True)
    T0 = now()
    if a.analyse:
        ev = json.load(open(os.path.join(a.analyse, "events.json")))
        T0 = ev[0]["t"]
        # What only the run knew (the trainer's counts, the mux's log) is
        # taken over from the report already in --out, if there is one.
        old = os.path.join(a.out, "report.json")
        old = json.load(open(old)) if os.path.exists(old) else {}
        res = analyse(a.analyse, ev, old.get("rows", {}).get("the trainer's stream"))
        res["deploys"] = {e["model"]: {k: v for k, v in e.items() if k not in ("t", "name", "model")}
                          for e in events(ev, "deploy")}
        if "the mux's log" in old.get("demotions", {}):
            res["demotions"]["the mux's log"] = old["demotions"]["the mux's log"]
    else:
        if not a.root:
            ap.error("--root is needed for a run")
        log = Log(os.path.join(a.out, "events.log"))
        log("start", label=a.label, quick=a.quick, bound=a.bound)
        st = htopic.open_topic("recorder.session", timeout=5.0, start="oldest")
        trainer = Trainer(log, a)
        try:
            m1, m2 = schedule(log, a, trainer)
        finally:
            trainer.stop = True
            try:
                call("safety.disarm")
                log("disarmed")
            except Exception as e:  # noqa: BLE001
                log("disarm failed", error=str(e))
        time.sleep(2)
        session = None
        for m in st.read():
            d = st.decode(m)
            if d["path"]:
                session = d["path"]
        if not session:
            print("no recorder session", file=sys.stderr)
            return 1
        log("session", path=session)
        ev = json.loads(json.dumps(log.events, default=str))
        json.dump(ev, open(os.path.join(session, "events.json"), "w"))
        json.dump(ev, open(os.path.join(a.out, "events.json"), "w"))
        span = (trainer.t_last - trainer.t_first) / 1e9 if trainer.t_first else 0.0
        if trainer.stream is None:
            print("the trainer never started", file=sys.stderr)
            return 1
        res = analyse(session, ev, {"rows": trainer.stream.replay.total, "lost": trainer.stream.lost,
                                    "steps": trainer.steps, "steps_per_s": round(trainer.steps / span, 1) if span else None,
                                    "label": a.label, "bound": a.bound, "batch": a.batch, "seq_len": a.seq_len,
                                    "replay": a.replay})
        res["deploys"] = {"M1": m1, "M2": m2}
        try:   # why the mux demoted, in its own words
            with open(os.path.join(a.root, "run", "hangar", "log", "mux.log")) as f:
                res["demotions"]["the mux's log"] = [ln.strip() for ln in f if "demoted" in ln]
        except OSError:
            pass
    json.dump(res, open(os.path.join(a.out, "report.json"), "w"), indent=1, default=str)
    open(os.path.join(a.out, "report.md"), "w").write(markdown(res))
    print(json.dumps(res, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
