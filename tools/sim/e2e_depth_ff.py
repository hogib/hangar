#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""docs/09's adaptive depth hold, end to end in simulation: the experiment
tools/sim/e2e-depth-ff.sh runs once the manager is up under --root.

Arms through safety (admin), assisted with a target depth, trains
python/examples/depth_ff.py's model live on dataset.depth_ff (PyTorch, in this
process), deploys it (same host: hangar.learn.deploy's incoming copy, until
the bridge), promotes residual:depth_ff with its hash, changes the
simulator's buoyancy live, deploys again, then reads the recorder's session
and reports:

  - depth error (truth, sim.pose) at steady hold and on 1 m steps, assisted
    against residual, nominal buoyancy against heavier;
  - the buoyancy change's transient, assisted and residual;
  - loop latency, nav.depth to thr.cmd through the chain of references;
  - swap cost: the policy's output timing during a load against a quiet window;
  - train/serve mismatch: the deployed file run by python/hangar's reference
    on the pairer's rows against the policy's outputs, after a reset_state;
  - lineage: every active model's file in the session, its header against
    the policy's announcement;
  - demotions: the mux leaving residual without being asked.

The schedule is in phases; --quick shortens every one (a smoke run).
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
        line = "[%7.1f] %s %s" % ((t - T0) / 1e9, event, " ".join("%s=%s" % kv for kv in kw.items()))
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
        self.stream = learn.stream("dataset.depth_ff", args.batch, args.seq_len, replay=args.replay,
                                   inputs=depth_ff.INPUTS, labels=depth_ff.label_fields())
        ai = self.stream.input_fields.index("ctl.depth_hold.target.accel_z")
        if args.label == "trim":
            self.label = lambda Y, X: depth_ff.trim(Y, X, ai)
        else:
            self.label = lambda Y, X: depth_ff.residual(Y)
        self.label_mean = float("nan")

    def run(self):
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
        model = depth_ff.make_model(len(self.stream.input_fields), mean, std)
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
                self.model = model
            if self.steps % 200 == 0:
                self.log("trainer", steps=self.steps, loss="%.2e" % self.loss, label_mean="%.4f" % self.label_mean,
                         rows=self.stream.replay.total, lost=self.stream.lost)
            time.sleep(0.002)  # leave the GIL to the main thread

    def deploy(self):
        with self.lock:
            if self.model is None:
                raise RuntimeError("no model yet")
            self.model.eval()
            t = now()
            r = learn.deploy(self.model, "depth_ff", stream=self.stream, outputs=["heave"])
            self.model.train()
        r["seconds"] = (now() - t) / 1e9
        r["steps"] = self.steps
        r["loss"] = self.loss
        return r


# ---- The schedule ------------------------------------------------------------------

def hold(log, depth, seconds, tag):
    call("depth_hold.set", depth_m=depth)
    log("target", depth=depth, tag=tag)
    time.sleep(seconds)


def steps_block(log, tag, a):
    """A 3 m hold, a step to 4 m, a step back: the block every condition gets."""
    log("block.begin", tag=tag)
    hold(log, DEPTH_A, a.hold_s, tag + ":hold")
    hold(log, DEPTH_B, a.step_s, tag + ":down")
    hold(log, DEPTH_A, a.step_s, tag + ":up")
    log("block.end", tag=tag)


def buoyancy(log, value, tag):
    hangar("param", "set", "sim_vehicle", "buoyancy=%g" % value)
    log("buoyancy", value=value, tag=tag)


def set_mode(log, mode, h=0):
    r = call("mux.set_mode", mode=mode, hash=h)
    log("set_mode", mode=mode, hash="%016x" % h, reply=r)


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
    m1 = trainer.deploy()
    log("deploy", model="M1", **m1)
    if m1["state"] != "done":
        raise RuntimeError("M1 rejected: %s" % m1)

    log("phase", what="B1 assisted nominal")
    steps_block(log, "B1", a)
    log("phase", what="T1 assisted, heavier")
    call("depth_hold.set", depth_m=DEPTH_A)
    buoyancy(log, NOMINAL_BUOYANCY + HEAVY_DELTA, "T1")
    time.sleep(a.transient_s)
    buoyancy(log, NOMINAL_BUOYANCY, "T1:back")
    time.sleep(a.transient_s)

    set_mode(log, "residual:depth_ff", m1["hash"])
    log("phase", what="B2 residual M1 nominal")
    steps_block(log, "B2", a)
    log("phase", what="T2 residual, heavier")
    call("depth_hold.set", depth_m=DEPTH_A)
    buoyancy(log, NOMINAL_BUOYANCY + HEAVY_DELTA, "T2")
    time.sleep(a.transient_s)
    log("phase", what="B3 residual M1 heavy")
    steps_block(log, "B3", a)
    # Trained on the heavier vehicle since T2; the deploy lands during a hold.
    call("depth_hold.set", depth_m=DEPTH_A)
    time.sleep(a.hold_s / 2)
    log("swap.begin")
    m2 = trainer.deploy()
    log("deploy", model="M2", **m2)
    time.sleep(2)
    log("swap.end")
    if m2["state"] == "done":
        call("policy_depth_ff.reset_state")
        log("reset_state", model="M2")
        time.sleep(a.hold_s / 2)
        log("phase", what="B4 residual M2 heavy")
        steps_block(log, "B4", a)
    set_mode(log, "assisted")
    log("phase", what="B5 assisted heavy")
    steps_block(log, "B5", a)
    log("phase", what="T3 assisted, back to nominal")
    call("depth_hold.set", depth_m=DEPTH_A)
    buoyancy(log, NOMINAL_BUOYANCY, "T3")
    time.sleep(a.transient_s)
    trainer.stop = True
    return m1, m2


# ---- The analysis, from the recorded session --------------------------------------

def load(session, name):
    path = os.path.join(session, name + ".hgr")
    if not os.path.exists(path):
        return None
    return record.open_recording(path)


def series(rec, field, elem=None):
    """(publish_ns array, value array) of a numeric field."""
    t = numpy.empty(len(rec), numpy.int64)
    v = numpy.empty(len(rec), numpy.float64)
    for i in range(len(rec)):
        r = rec[i]
        t[i] = r.publish_ns
        x = rec.payload(i)[field]
        v[i] = x[elem] if elem is not None else x
    return t, v


def window(t, v, t0, t1):
    k = (t >= t0) & (t < t1)
    return t[k], v[k]


def step_metrics(t, z, t0, t1, target, start):
    tt, zz = window(t, z, t0, t1)
    if len(tt) == 0:
        return {}
    e = zz - target
    out = {}
    tail = tt >= t1 - 8e9
    out["ss_mean_cm"] = 100 * float(e[tail].mean())
    out["ss_rms_cm"] = 100 * float(numpy.sqrt((e[tail] ** 2).mean()))
    out["iae_m_s"] = float(numpy.abs(e).sum() * numpy.median(numpy.diff(tt)) / 1e9) if len(tt) > 1 else 0.0
    if start is not None and abs(target - start) > 0.2:
        d = math.copysign(1, target - start)
        out["overshoot_pct"] = 100 * max(0.0, float((d * (zz - target)).max())) / abs(target - start)
        out_band = numpy.nonzero(numpy.abs(e) > 0.05)[0]
        if len(out_band) == 0:
            out["settle_s"] = 0.0
        elif out_band[-1] == len(tt) - 1:
            out["settle_s"] = None   # never settled within the window
        else:
            out["settle_s"] = float(tt[out_band[-1] + 1] - t0) / 1e9
    return out


def transient_metrics(t, z, t0, t1, target):
    tt, zz = window(t, z, t0, t1)
    e = zz - target
    out_band = numpy.nonzero(numpy.abs(e) > 0.05)[0]
    rec = None if len(out_band) and out_band[-1] == len(tt) - 1 else (
        0.0 if not len(out_band) else float(tt[out_band[-1] + 1] - t0) / 1e9)
    return {"peak_cm": 100 * float(numpy.abs(e).max()), "iae_m_s": float(numpy.abs(e).sum() * 0.005),
            "back_within_5cm_s": rec, "final_cm": 100 * float(e[-200:].mean())}


def refs_index(rec):
    """seq -> record, for following references."""
    return {rec[i].seq: rec[i] for i in range(len(rec))}


def latency(session, log_events):
    """nav.depth's publish to thr.cmd's, through the chain each message names."""
    names = ["thr.cmd", "ctl.wrench", "ctl.depth_hold", "nav.state", "nav.depth", "policy.depth_ff.wrench"]
    recs = {n: load(session, n) for n in names}
    if any(recs[n] is None for n in names[:5]):
        return {"error": "a topic of the chain was not recorded"}
    ids = {recs[n].header.topic_id: n for n in names if recs[n] is not None}
    idx = {n: refs_index(recs[n]) for n in names if recs[n] is not None}

    def ref(r, name):
        for inp in r.inputs[:r.n_inputs]:
            if ids.get(inp.topic_id) == name:
                return idx[name].get(inp.seq)
        return None

    hops = {"nav.depth>nav.state": [], "nav.state>ctl.depth_hold": [], "ctl.depth_hold>ctl.wrench": [],
            "ctl.wrench>thr.cmd": [], "nav.depth>thr.cmd": [], "nav.state>policy.wrench": [],
            "policy.wrench>ctl.wrench": []}
    no_ref = 0
    rc = recs["thr.cmd"]
    for i in range(len(rc)):
        r4 = rc[i]
        r3 = ref(r4, "ctl.wrench")
        r2 = ref(r3, "ctl.depth_hold") if r3 else None
        r1 = ref(r2, "nav.state") if r2 else None
        r0 = ref(r1, "nav.depth") if r1 else None
        if not r0:
            no_ref += 1
            continue
        hops["nav.depth>nav.state"].append(r1.publish_ns - r0.publish_ns)
        hops["nav.state>ctl.depth_hold"].append(r2.publish_ns - r1.publish_ns)
        hops["ctl.depth_hold>ctl.wrench"].append(r3.publish_ns - r2.publish_ns)
        hops["ctl.wrench>thr.cmd"].append(r4.publish_ns - r3.publish_ns)
        hops["nav.depth>thr.cmd"].append(r4.publish_ns - r0.publish_ns)
        rp = ref(r3, "policy.depth_ff.wrench") if "policy.depth_ff.wrench" in idx else None
        if rp:
            hops["policy.wrench>ctl.wrench"].append(r3.publish_ns - rp.publish_ns)
            rs = ref(rp, "nav.state")
            if rs:
                hops["nav.state>policy.wrench"].append(rp.publish_ns - rs.publish_ns)
    out = {"thr.cmd records": len(rc), "without a full chain": no_ref}
    for k, v in hops.items():
        if v:
            a = numpy.array(v) / 1e6
            out[k] = {"n": len(a), "median_ms": round(float(numpy.median(a)), 3),
                      "p99_ms": round(float(numpy.percentile(a, 99)), 3), "max_ms": round(float(a.max()), 3)}
    return out


def swap_cost(session, ev):
    rp = load(session, "policy.depth_ff.wrench")
    rw = load(session, "ctl.wrench")
    b = [e["t"] for e in ev if e["name"] == "swap.begin"]
    e_ = [e["t"] for e in ev if e["name"] == "swap.end"]
    if not (rp and b and e_):
        return {"error": "no swap recorded"}
    t0, t1 = b[0], e_[0]
    quiet = (t0 - 2 * (t1 - t0) - 5_000_000_000, t0 - (t1 - t0) - 5_000_000_000)

    def stats(rec, lo, hi, anchored):
        pub = []
        lat = []
        for i in range(len(rec)):
            r = rec[i]
            if lo <= r.publish_ns < hi:
                pub.append(r.publish_ns)
                if anchored:
                    lat.append(r.publish_ns - r.stamp_ns)
        d = numpy.diff(numpy.array(pub)) / 1e6 if len(pub) > 1 else numpy.array([0.0])
        o = {"messages": len(pub), "interval_max_ms": round(float(d.max()), 2),
             "interval_jitter_ms": round(float(d.std()), 3), "missed_at_50Hz": int(max(0, round((hi - lo) / 20e6) - len(pub)))}
        if lat:
            a = numpy.array(lat) / 1e6
            o["stamp_to_publish_median_ms"] = round(float(numpy.median(a)), 3)
            o["stamp_to_publish_max_ms"] = round(float(a.max()), 3)
        return o

    return {"window_s": round((t1 - t0) / 1e9, 2),
            "policy.wrench during the load": stats(rp, t0, t1, True),
            "policy.wrench quiet": stats(rp, quiet[0], quiet[1], True),
            "ctl.wrench during the load": stats(rw, t0, t1, False),
            "ctl.wrench quiet": stats(rw, quiet[0], quiet[1], False)}


def mismatch(session, ev, trainer_model_files):
    """After reset_state: the deployed file, run by the reference on the
    pairer's rows, against what the policy published for the same anchors."""
    rs = [e for e in ev if e["name"] == "reset_state"]
    dep = [e for e in ev if e["name"] == "deploy" and e.get("model") == "M2"]
    if not rs or not dep:
        return {"error": "no reset_state after M2"}
    h = dep[0]["hash"]
    path = os.path.join(session, "weights", "%016x.hgw" % h)
    if not os.path.exists(path):
        return {"error": "%s not in the session" % path}
    f = hgw.read(path)
    g = f.graph
    rp = load(session, "policy.depth_ff.wrench")
    rd = load(session, "dataset.depth_ff")
    t_reset = rs[0]["t"]
    # The policy's outputs after the reset was served, by anchor stamp.
    outs = {}
    for i in range(len(rp)):
        r = rp[i]
        if r.publish_ns > t_reset:
            outs[r.stamp_ns] = rp.payload(i)["heave"]
    if not outs:
        return {"error": "no policy output after the reset"}
    s0 = min(outs)
    model = reference.Model(g)
    model.reset()
    diffs, n = [], 0
    stale_rows = 0
    for i in range(len(rd)):
        row = rd.payload(i)
        st = row["anchor_stamp_ns"]
        if st < s0:
            continue
        if row["flags"] & 1:   # reset: the policy resets too
            model.reset()
        x = [float(row[b.field]) for b in g.inputs]
        y = model.step([x])[0][0]
        if st in outs:
            diffs.append(abs(y - outs[st]))
            n += 1
        else:
            stale_rows += 1
        if n >= 1000:
            break
    if not diffs:
        return {"error": "no anchor matched"}
    a = numpy.array(diffs)
    return {"model": "%016x" % h, "anchors_compared": n, "rows_without_an_output": stale_rows,
            "max_abs_heave": float(a.max()), "median_abs_heave": float(numpy.median(a)),
            "over_1e-4": int((a > 1e-4).sum())}


def lineage(session):
    ra = load(session, "policy.depth_ff.active")
    out = []
    if not ra:
        return out
    for i in range(len(ra)):
        p = ra.payload(i)
        h = p["hash"]
        path = os.path.join(session, "weights", "%016x.hgw" % h)
        e = {"version": p["version"], "cause": p["cause"], "hash": "%016x" % h, "rows": p["rows"],
             "file_in_session": os.path.exists(path)}
        if os.path.exists(path):
            hd = hgw.read(path).header
            e["header_rows"] = hd.rows
            e["header_dataset_hash"] = "%016x" % hd.dataset_hash
            e["announced_dataset_hash"] = "%016x" % p["dataset_hash"]
            e["header_newest_row_stamp_ns"] = hd.newest_row_stamp_ns
            e["announced_newest_row_stamp_ns"] = p["newest_row_stamp_ns"]
            e["agrees"] = (hd.rows == p["rows"] and hd.dataset_hash == p["dataset_hash"]
                           and hd.newest_row_stamp_ns == p["newest_row_stamp_ns"])
            ln = hd.lineage if hasattr(hd, "lineage") else None
            if ln:
                e["lineage"] = [{"session": l.session, "rows": l.rows, "first_seq": l.first_seq,
                                 "last_seq": l.last_seq} for l in ln]
        out.append(e)
    return out


def demotions(session, ev):
    rm = load(session, "mux.state")
    if not rm:
        return {"error": "mux.state not recorded"}
    asked = [(e["t"], e["mode"]) for e in ev if e["name"] == "set_mode"]
    out, prev = [], None
    for i in range(len(rm)):
        r = rm[i]
        p = rm.payload(i)
        mode = p["mode"]
        if prev is not None and mode != prev:
            requested = any(abs(r.publish_ns - t) < 1_000_000_000 and m == mode for t, m in asked)
            out.append({"t": round((r.publish_ns - T0) / 1e9, 2), "from": prev, "to": mode, "asked": requested})
        prev = mode
    residual_ticks = sum(1 for i in range(len(rm)) if rm.payload(i)["residual_active"])
    limited = sum(1 for i in range(len(rm)) if rm.payload(i)["limits"] >> 13 & 0x3f)
    return {"transitions": out, "demotions": [x for x in out if not x["asked"]],
            "mux.state with residual_active": residual_ticks, "mux.state at the residual envelope": limited}


def analyse(session, ev, a):
    res = {"session": session}
    pose = load(session, "sim.pose")
    t, z = series(pose, "pos", 2)
    # Blocks: per segment, the metrics against the target held.
    blocks = {}
    targets = [e for e in ev if e["name"] == "target"]
    for k, e in enumerate(targets):
        tag = e["tag"]
        if ":" not in tag or not tag[:2] in ("B1", "B2", "B3", "B4", "B5"):
            continue
        blk, seg = tag.split(":")
        t1 = targets[k + 1]["t"] if k + 1 < len(targets) else e["t"] + int(a.step_s * 1e9)
        nxt_block_end = [x["t"] for x in ev if x["name"] == "block.end" and x["tag"] == blk]
        if nxt_block_end:
            t1 = min(t1, nxt_block_end[0])
        start = targets[k - 1]["depth"] if k > 0 else None
        blocks.setdefault(blk, {})[seg] = step_metrics(t, z, e["t"], t1, e["depth"], start if seg != "hold" else None)
    res["blocks"] = blocks
    tr = {}
    for e in ev:
        if e["name"] == "buoyancy" and e["tag"] in ("T1", "T2", "T1:back", "T3"):
            tr[e["tag"]] = transient_metrics(t, z, e["t"], e["t"] + int(a.transient_s * 1e9), DEPTH_A)
    res["buoyancy_transients"] = tr
    # The integrator and the residual through the run: what carried the trim.
    rt = load(session, "ctl.depth_hold.target")
    rp = load(session, "policy.depth_ff.wrench")
    tI, I = series(rt, "integral")
    tp, hp = series(rp, "heave") if rp else (numpy.array([]), numpy.array([]))
    split = {}
    for blk in ("B1", "B2", "B3", "B4", "B5"):
        b0 = [x["t"] for x in ev if x["name"] == "block.begin" and x["tag"] == blk]
        b1 = [x["t"] for x in ev if x["name"] == "block.end" and x["tag"] == blk]
        if b0 and b1:
            _, Iw = window(tI, I, b0[0], b1[0])
            _, pw = window(tp, hp, b0[0], b1[0])
            split[blk] = {"integral_mean": round(float(Iw.mean()), 4) if len(Iw) else None,
                          "policy_heave_mean": round(float(pw.mean()), 4) if len(pw) else None}
    res["trim_carried_by"] = split
    res["latency"] = latency(session, ev)
    res["swap"] = swap_cost(session, ev)
    res["mismatch"] = mismatch(session, ev, None)
    res["lineage"] = lineage(session)
    res["demotions"] = demotions(session, ev)
    return res


def main():
    global T0
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--quick", action="store_true", help="every phase shortened: a smoke run")
    ap.add_argument("--label", choices=("trim", "shortfall"), default="trim")
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--seq-len", type=int, default=50)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--replay", default="3min")
    ap.add_argument("--analyse", help="a session directory and an events.json: the analysis alone")
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
        res = analyse(a.analyse, ev, a)
        print(json.dumps(res, indent=1, default=str))
        return 0
    log = Log(os.path.join(a.out, "events.log"))
    log("start", label=a.label, quick=a.quick)
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
    json.dump(log.events, open(os.path.join(session, "events.json"), "w"), default=str)
    json.dump(log.events, open(os.path.join(a.out, "events.json"), "w"), default=str)
    res = analyse(session, log.events, a)
    res["deploys"] = {"M1": m1, "M2": m2}
    res["trainer"] = {"steps": trainer.steps, "loss": trainer.loss, "rows": trainer.stream.replay.total,
                      "lost": trainer.stream.lost, "label": a.label}
    json.dump(res, open(os.path.join(a.out, "report.json"), "w"), indent=1, default=str)
    print(json.dumps(res, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
