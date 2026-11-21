#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""docs/09's adaptive depth hold, second design, end to end in simulation: the
experiment tools/sim/e2e-depth-ff2.sh runs once the manager is up under --root
(bench/e2e-depth-ff/README.md, "Second design", has the method and the results).

The schedule, the measurements and the checks are e2e_depth_ff.py's (lineage,
train/serve mismatch, loop and control latency, swap cost, demotions, rows
lost); the model, its label and its training are python/examples/depth_ff2.py's.

  --variant learned   the model trained live on dataset.depth_ff, M1 after the
                      training phase, M2 after the buoyancy change
  --variant oracle    the same architecture fitted to the simulator's own thrust
                      table, mass and drag, deployed as M1 and again as M2: the
                      ceiling
  --variant collect   no model: assisted, steps and holds at both buoyancies, for
                      the offline check (tools/sim/depth_ff2_offline.py)

--change supply makes the schedule's change one the policy cannot read from
its inputs: the thrusters' supply from 16.8 to 12.6 V (the simulator's
battery_full_v and battery_empty_v) where the buoyancy would go down 4 N, so
"heavier" in the tables reads "weaker". The thrust curve itself is then
another, and only rows from after the change can say so.

--adapt is the adaptation experiment (bench/e2e-depth-ff/README.md,
"Adaptation"): another schedule, in which the vehicle takes on a payload that
none of the policy's inputs shows (depth_ff2.PAYLOAD), M1 drives it while the
rows are recorded, M2 is trained on them and loaded, and the two are compared
on the vehicle with the payload (M1, M2, M1 again, the controller alone) and
M2 on the vehicle without it. With --variant oracle M1 is the oracle of the
vehicle without the payload and M2 the oracle of the vehicle with it.
tools/sim/e2e-adapt.sh makes the whole comparison.

--analyse <session> runs the analysis alone on a session that holds the run's
events.json. --summarise <out>... prints the table over several runs' report.json.
"""
import argparse
import json
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "python"))
sys.path.insert(0, os.path.join(REPO, "python", "examples"))

import numpy  # noqa: E402
import torch  # noqa: E402

import depth_ff2  # noqa: E402
import e2e_depth_ff as E  # noqa: E402
from hangar import learn  # noqa: E402
from hangar import topic as htopic  # noqa: E402

# The excitation of the training phase: steps of 0.1 to 1.5 m, 10 s each.
TRAIN_TARGETS = [3.3, 3.0, 3.6, 2.8, 3.1, 4.0, 3.5, 3.0, 2.5, 3.4, 4.2, 3.0]


# The adaptation experiment's blocks and transients (E.BLOCKS' and
# E.TRANSIENTS' shape: the analysis reads whichever the run was made with).
ADAPT_BLOCKS = [("A1", "assisted", "-", "bare"), ("A2", "residual", "M1", "bare"),
                ("P1", "residual", "M1", "payload"), ("P2", "residual", "M2", "payload"),
                ("N2", "residual", "M2", "bare"), ("P3", "residual", "M1 again", "payload"),
                ("P4", "assisted", "-", "payload")]
ADAPT_TRANSIENTS = [("H1", "assisted to residual M1", "promotion"), ("C1", "residual M1", "payload on"),
                    ("S2", "residual M1 to M2", "deploy and swap"),
                    ("H1b", "assisted to residual", "promotion again, after a demotion"),
                    ("C2", "residual M2", "payload off"), ("C3", "residual M2", "payload on"),
                    ("S3", "residual M2 to M1", "revert"), ("H2", "residual M1 to assisted", "set_mode assisted"),
                    ("C4", "assisted", "payload off")]


def lower(nice):
    """This thread at a lower priority (on Linux a nice value is a thread's
    own): the training, never the vehicle's modules or the operator."""
    try:
        os.setpriority(os.PRIO_PROCESS, threading.get_native_id(), nice)
    except (OSError, AttributeError):
        pass


def niced(f, nice):
    """f() in a thread of its own at a lower priority."""
    def run():
        lower(nice)
        f()
    t = threading.Thread(target=run)
    t.start()
    t.join()


class Load(threading.Thread):
    """The machine's load average while the run lasts: these runs are in real
    time, and other work on the machine changes them."""

    def __init__(self):
        threading.Thread.__init__(self, daemon=True)
        self.start_avg = os.getloadavg()
        self.highest = self.start_avg[0]
        self.stop = False

    def run(self):
        while not self.stop:
            self.highest = max(self.highest, os.getloadavg()[0])
            time.sleep(5.0)

    def report(self):
        return {"at the start (1, 5, 15 min)": [round(x, 2) for x in self.start_avg],
                "at the end (1, 5, 15 min)": [round(x, 2) for x in os.getloadavg()],
                "highest 1 min average during the run": round(self.highest, 2), "threads": os.cpu_count()}


class Trainer(threading.Thread):
    """depth_ff2.py's model and step on the live rows, in a thread. With
    variant oracle the rows are read (and counted) but the model is the one
    fitted to the simulator's own numbers before the run."""

    def __init__(self, log, a):
        threading.Thread.__init__(self, daemon=True)
        self.log, self.a = log, a
        self.lock = threading.Lock()
        self.stop = False
        self.steps = 0
        self.loss = float("nan")
        self.stream = None
        self.t_first = self.t_last = None
        torch.set_num_threads(1)
        self.L = depth_ff2.Learner(a.seed, a.lr)
        self.second = None    # --adapt, oracle: the oracle of the vehicle with the payload, deployed as M2
        if a.variant == "oracle":
            niced(lambda: depth_ff2.fit_oracle(self.L.model, a.oracle_steps, seed=a.seed, voltage=a.voltage),
                  a.trainer_nice)
            log("oracle.fitted", steps=a.oracle_steps, voltage=a.voltage)
            if a.adapt:
                self.second = depth_ff2.make_model()
                niced(lambda: depth_ff2.fit_oracle(self.second, a.oracle_steps, seed=a.seed, voltage=a.voltage,
                                                   **depth_ff2.PAYLOAD), a.trainer_nice)
                log("oracle.fitted", steps=a.oracle_steps, voltage=a.voltage, vehicle="payload")

    def run(self):
        # Opened when the reading starts (e2e_depth_ff.py: a stream opened
        # earlier is lapped before its first read).
        lower(self.a.trainer_nice)
        self.stream = learn.stream("dataset.depth_ff", self.a.batch, None, replay=self.a.replay,
                                   inputs=depth_ff2.INPUTS, labels=depth_ff2.label_fields(), seed=self.a.seed)
        logged = time.monotonic()
        period = 1.0 / self.a.steps_per_s
        while not self.stop:
            t = time.monotonic()
            with self.lock:   # deploy() reads the buffer for the probes and the lineage
                self.stream._ingest()
                self.L.tracker.attach(self.stream)
                if self.a.variant == "learned" and len(self.stream.replay) >= 50 * self.a.rows_first_s:
                    self.L.step(self.stream)
                    self.steps = self.L.steps
                    self.t_last = E.now()
                    if self.t_first is None:
                        self.t_first = self.t_last
            if time.monotonic() - logged >= 10.0:
                logged = time.monotonic()
                self.log("trainer", steps=self.steps, plant_loss="%.2e" % self.L.plant_loss,
                         policy_loss="%.2e" % self.L.policy_loss, rows=self.stream.replay.total,
                         lost=self.stream.lost, rest="%+.3f" % (self.L.tracker.rest or 0.0))
            time.sleep(max(0.001, period - (time.monotonic() - t)))

    def deploy(self, name):
        """The policy brought up to the plant, exported, sent and loaded;
        training waits meanwhile (the lock)."""
        self.log("deploy.begin", model=name)
        with self.lock:
            self.stream._ingest()
            if self.a.variant == "learned":
                self.L.settle(self.stream, self.a.settle_steps)
            model = self.second if name == "M2" and self.second is not None else self.L.model
            model.eval()
            t = E.now()
            r = learn.deploy(model, "depth_ff", stream=self.stream, outputs=["heave"])
            model.train()
            r["commands"] = "%+.3f to %+.3f" % self.L.tracker.applied(self.stream)
            r["rest"] = "%+.3f" % (self.L.tracker.rest or 0.0)
        r["seconds"] = (E.now() - t) / 1e9
        r["steps"] = self.steps
        r["plant_loss"], r["policy_loss"] = self.L.plant_loss, self.L.policy_loss
        r["hash"] = "%016x" % r["hash"]
        self.log("deploy", model=name, **r)
        return r


def change(log, a, changed, tag, seconds):
    """The schedule's change at the 3 m hold: the buoyancy 4 N down (or back),
    or with --change supply the thrusters' supply to 12.6 V (or back)."""
    if a.change == "buoyancy":
        E.buoyancy(log, E.NOMINAL_BUOYANCY + (E.HEAVY_DELTA if changed else 0.0), tag, seconds)
        return
    E.transient(log, tag, seconds)
    full, empty = (12.6, 10.0) if changed else (16.8, 13.2)
    E.hangar("param", "set", "sim_vehicle", "battery_full_v=%g" % full, "battery_empty_v=%g" % empty)
    log("supply", full_v=full, empty_v=empty, tag=tag)
    time.sleep(seconds)


def excite(log, a, seconds, tag):
    t_end = time.monotonic() + seconds
    k = 0
    while time.monotonic() < t_end:
        E.hold(log, TRAIN_TARGETS[k % len(TRAIN_TARGETS)], min(a.train_hold_s, max(1.0, t_end - time.monotonic())), tag)
        k += 1
    E.hold(log, E.DEPTH_A, a.hold_s, tag + ":return")


def schedule(log, a, trainer):
    """e2e_depth_ff.py's schedule: the same blocks and transients under the
    same names, so its analysis reads this run's events."""
    mux = E.Mux()
    E.arm(log)
    E.set_mode(log, "assisted")
    E.hold(log, E.DEPTH_A, a.settle_s, "descent")
    trainer.start()
    log("phase", what="train")
    excite(log, a, a.train_s, "train")
    if a.variant == "collect":
        log("phase", what="B1 assisted, nominal")
        E.steps_block(log, "B1", a, mux)
        E.call("depth_hold.set", depth_m=E.DEPTH_A)
        change(log, a, True, "T1", a.transient_s)
        log("phase", what="heavier: the excitation again")
        excite(log, a, a.train_s / 2, "train:heavier")
        log("phase", what="B5 assisted, heavier")
        E.steps_block(log, "B5", a, mux)
        trainer.stop = True
        return None, None
    m1 = trainer.deploy("M1")
    if m1["state"] != "done":
        raise RuntimeError("M1 rejected: %s" % m1)

    log("phase", what="B1 assisted, nominal")
    E.steps_block(log, "B1", a, mux)
    log("phase", what="T1 assisted, 4 N heavier and back")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    change(log, a, True, "T1", a.transient_s)
    change(log, a, False, "T1:back", a.transient_s)

    log("phase", what="H1 promotion: residual:depth_ff with M1")
    E.transient(log, "H1", a.hold_s)
    E.set_mode(log, "residual:depth_ff", int(m1["hash"], 16))
    time.sleep(a.hold_s)
    log("phase", what="B2 residual M1, nominal")
    E.steps_block(log, "B2", a, mux)
    log("phase", what="T2 residual M1, 4 N heavier")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    change(log, a, True, "T2", a.transient_s)
    log("phase", what="B3 residual M1, heavier")
    E.steps_block(log, "B3", a, mux)
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    time.sleep(a.hold_s / 2)
    log("phase", what="S2 deploy M2 during the hold")
    E.transient(log, "S2", a.hold_s)
    m2 = trainer.deploy("M2")
    if m2["state"] == "done":
        time.sleep(a.hold_s / 2)
        if not mux.now().startswith("residual:"):
            log("phase", what="H1b the policy was demoted: residual:depth_ff again, with M2", mode=mux.now())
            E.transient(log, "H1b", a.hold_s)
            E.set_mode(log, "residual:depth_ff", int(m2["hash"], 16))
            time.sleep(a.hold_s)
        # The model has no state; the call is e2e_depth_ff.py's mark for its
        # train/serve comparison, which starts there.
        log("reset_state.begin")
        E.call("policy_depth_ff.reset_state")
        log("reset_state", model="M2")
        time.sleep(a.hold_s / 2)
        log("phase", what="B4 residual M2, heavier")
        E.steps_block(log, "B4", a, mux)
    log("phase", what="H2 back to assisted")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    E.transient(log, "H2", a.hold_s)
    E.set_mode(log, "assisted")
    time.sleep(a.hold_s)
    log("phase", what="B5 assisted, heavier")
    E.steps_block(log, "B5", a, mux)
    log("phase", what="T3 assisted, back to nominal")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    change(log, a, False, "T3", a.transient_s)
    trainer.stop = True
    return m1, m2


def payload(log, on, tag, seconds):
    """The adaptation experiment's change, at the 3 m hold: the payload on or
    off, in one parameter change."""
    E.transient(log, tag, seconds)
    E.hangar("param", "set", "sim_vehicle", *depth_ff2.payload_params(on))
    log("payload", on=on, tag=tag, params=" ".join(depth_ff2.payload_params(on)))
    time.sleep(seconds)


def promoted(log, a, mux, h, why):
    """The policy still drives; had the mux demoted it, it is promoted again
    (what an operator would do), and the run's report says so."""
    if mux.now().startswith("residual:"):
        return
    log("phase", what="H1b the policy was demoted: residual:depth_ff again, %s" % why, mode=mux.now())
    E.transient(log, "H1b", a.hold_s)
    E.set_mode(log, "residual:depth_ff", h)
    time.sleep(a.hold_s)


def schedule_adapt(log, a, trainer):
    """The adaptation experiment: M1 from the vehicle without the payload;
    the payload; the rows recorded while M1 drives it; M2 from those rows;
    then the same block of steps with M1, with M2, with M2 on the vehicle
    without the payload, with M1 again and with the controller alone."""
    mux = E.Mux()
    E.arm(log)
    E.set_mode(log, "assisted")
    E.hold(log, E.DEPTH_A, a.settle_s, "descent")
    trainer.start()
    log("phase", what="train")
    excite(log, a, a.train_s, "train")
    m1 = trainer.deploy("M1")
    if m1["state"] != "done":
        raise RuntimeError("M1 rejected: %s" % m1)
    h1 = int(m1["hash"], 16)

    log("phase", what="A1 assisted, without the payload")
    E.steps_block(log, "A1", a, mux)
    log("phase", what="H1 promotion: residual:depth_ff with M1")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    E.transient(log, "H1", a.hold_s)
    E.set_mode(log, "residual:depth_ff", h1)
    time.sleep(a.hold_s)
    log("phase", what="A2 residual M1, without the payload")
    E.steps_block(log, "A2", a, mux)

    log("phase", what="C1 the payload on, M1 driving")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    payload(log, True, "C1", a.hold_s)
    log("phase", what="record: M1 on the vehicle with the payload, the training phase's steps")
    excite(log, a, a.record_s, "record")
    promoted(log, a, mux, h1, "with M1")
    log("phase", what="P1 residual M1, payload")
    E.steps_block(log, "P1", a, mux)

    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    time.sleep(a.hold_s / 2)
    log("phase", what="S2 deploy M2 during the hold")
    E.transient(log, "S2", a.hold_s)
    m2 = trainer.deploy("M2")
    if m2["state"] != "done":
        raise RuntimeError("M2 rejected: %s" % m2)
    h2 = int(m2["hash"], 16)
    time.sleep(a.hold_s / 2)
    promoted(log, a, mux, h2, "with M2")
    log("reset_state.begin")
    E.call("policy_depth_ff.reset_state")
    log("reset_state", model="M2")
    time.sleep(a.hold_s / 2)
    log("phase", what="P2 residual M2, payload")
    E.steps_block(log, "P2", a, mux)

    log("phase", what="C2 the payload off, M2 driving")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    payload(log, False, "C2", a.hold_s)
    promoted(log, a, mux, h2, "with M2")
    log("phase", what="N2 residual M2, without the payload")
    E.steps_block(log, "N2", a, mux)
    log("phase", what="C3 the payload on again, M2 driving")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    payload(log, True, "C3", a.hold_s)

    log("phase", what="S3 revert: M1 again")
    E.transient(log, "S3", a.hold_s)
    log("revert", reply=learn.revert("depth_ff"))
    time.sleep(a.hold_s)
    promoted(log, a, mux, h1, "with M1 again")
    log("phase", what="P3 residual M1 again, payload")
    E.steps_block(log, "P3", a, mux)

    log("phase", what="H2 back to assisted")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    E.transient(log, "H2", a.hold_s)
    E.set_mode(log, "assisted")
    time.sleep(a.hold_s)
    log("phase", what="P4 assisted, payload")
    E.steps_block(log, "P4", a, mux)
    log("phase", what="C4 the payload off: the vehicle as it was")
    E.call("depth_hold.set", depth_m=E.DEPTH_A)
    payload(log, False, "C4", a.hold_s)
    trainer.stop = True
    return m1, m2


# ---- The analysis: e2e_depth_ff.py's, and what this design adds ---------------------

def span_of(ev, name):
    if name == "train":
        b0, b1 = E.events(ev, "phase", what="train"), E.events(ev, "deploy.begin", model="M1") or \
            E.events(ev, "phase", what="B1 assisted, nominal")
    else:
        b0, b1 = E.events(ev, "block.begin", tag=name), E.events(ev, "block.end", tag=name)
    return (b0[0]["t"], b1[0]["t"]) if b0 and b1 else None


def rows(s, ev, res, trainer=None):
    """The training rows: what the pairer wrote, what the recorder kept, what
    the trainer's stream got; and the label over each block's rows."""
    ds = s.arr("dataset.depth_ff")
    if ds is None:
        res["rows"] = {"error": "dataset.depth_ff not recorded"}
        return
    span = int(ds["anchor_seq"][-1]) - int(ds["anchor_seq"][0]) + 1
    out = {"rows recorded": int(len(ds)), "recorder gaps": int(ds["gap"].sum()),
           "anchors from the first row to the last": span, "anchors without a row": span - int(len(ds)),
           "masked (outputs off, or the controller not driving)": int((ds["flags"] & learn.MASKED != 0).sum()),
           "resets": int((ds["flags"] & learn.RESET != 0).sum()),
           "anchor to row published, median_ms": round(float(numpy.median(
               ds["publish_ns"].astype(numpy.int64) - ds["anchor_stamp_ns"].astype(numpy.int64))) / 1e6, 3)}
    if trainer is not None:
        out["the trainer's stream"] = trainer
    # The plant's rows per block: the acceleration made, the heave applied
    # (LAG ticks before), and the simulator's own model on the same rows with
    # the integrator standing for the heave at rest.
    k = depth_ff2.WINDOW - 1 - depth_ff2.LAG
    made = ds["made.accel"][:, 2].astype(numpy.float64)
    vel = ds["nav.state.vel_z"].astype(numpy.float64)
    integ = ds["ctl.depth_hold.target.integral"].astype(numpy.float64)
    applied = sum(depth_ff2.VERTICAL[ch] * ((ds["thr.out.%d.pwm" % k][:, ch].astype(numpy.float64) - depth_ff2.PWM_STOP)
                                             / depth_ff2.PWM_FULL) for ch in depth_ff2.VERTICAL)
    # The rows the trainer gives loss, by the trainer's own rule.
    keep = depth_ff2.with_loss(ds)
    out["passed Where, without loss (a part missing, or thr.out's window short of the command)"] = int(
        ((ds["flags"] & learn.MASKED == 0) & ~keep).sum())
    out["thr.out's window not full"] = int((ds["thr.out._count"] != depth_ff2.WINDOW).sum())
    lab = {}
    for name in ["train"] + [b[0] for b in E.BLOCKS]:
        sp = span_of(ev, name)
        if not sp:
            continue
        m = keep & (ds["anchor_stamp_ns"] >= sp[0]) & (ds["anchor_stamp_ns"] < sp[1])
        if m.any():
            still = m & (numpy.abs(made) < depth_ff2.STILL_ACCEL) & (numpy.abs(vel) < depth_ff2.STILL_VEL)
            sim = depth_ff2.sim_plant(numpy.stack([applied, integ, vel], -1)[m], 16.8)
            lab[name] = {"rows with loss": int(m.sum()), "still rows": int(still.sum()),
                         "accel made: sd": round(float(made[m].std()), 4),
                         "heave applied less the integrator: sd": round(float((applied - integ)[m].std()), 4),
                         "on still rows: mean": round(float((applied - integ)[still].mean()), 5) if still.any() else None,
                         "r2 of the simulator's own model on accel made": round(
                             1 - float(((made[m] - sim) ** 2).sum()) / float(((made[m] - made[m].mean()) ** 2).sum()), 3)}
    out["plant rows"] = lab
    res["rows"] = out


def policy_output(s, ev, res):
    """What the policy asked for, per block: its output's rms, and the share
    of ticks it was at its own bound (the mux sees no saturation there: the
    bound is inside the residual envelope)."""
    pw = s.arr("policy.depth_ff.wrench")
    out = {}
    if pw is not None:
        for name in [b[0] for b in E.BLOCKS]:
            sp = span_of(ev, name)
            if not sp:
                continue
            k = (pw["publish_ns"] >= sp[0]) & (pw["publish_ns"] < sp[1])
            if k.any():
                h = pw["heave"][k].astype(numpy.float64)
                out[name] = {"rms": round(float(numpy.sqrt((h ** 2).mean())), 4),
                             "at its bound (share of ticks)": round(float((numpy.abs(h) > 0.95 * depth_ff2.BOUND).mean()), 4),
                             "sign changes per second": round(float((numpy.diff(numpy.sign(h)) != 0).sum())
                                                              / max(1e-9, (sp[1] - sp[0]) / 1e9), 2)}
    res["policy_output"] = out
    bat = s.arr("power.battery")
    if bat is not None and "voltage_v" in bat.dtype.names:
        res["battery_v"] = [round(float(bat["voltage_v"].max()), 2), round(float(bat["voltage_v"].min()), 2)]


def command_activity(s, ev, res):
    """The thrusters' work per block: the rms of the heave command's change
    per tick, and how often it crossed zero (a reversal of the verticals)."""
    cw = s.arr("ctl.wrench")
    out = {}
    if cw is not None:
        for name in [b[0] for b in E.BLOCKS]:
            sp = span_of(ev, name)
            if not sp:
                continue
            k = (cw["publish_ns"] >= sp[0]) & (cw["publish_ns"] < sp[1])
            if k.sum() > 2:
                h = cw["heave"][k].astype(numpy.float64)
                out[name] = {"rms of the change per tick": round(float(numpy.sqrt((numpy.diff(h) ** 2).mean())), 5),
                             "reversals per second": round(float((numpy.diff(numpy.sign(h)) != 0).sum())
                                                           / max(1e-9, (sp[1] - sp[0]) / 1e9), 2)}
    res["command_activity"] = out


def mismatch_split(s, ev, res, limit=6000):
    """Train against serve for a model without state: the newest model's file,
    run by python/hangar's reference on the pairer's rows, against what the
    policy published at the same anchors; apart for the anchors at which the
    policy read the ctl.depth_hold.target message the pairer joined, and for
    those at which it read another."""
    from hangar.nn import hgw, reference
    pw, ds, tg, ns, act = (s.arr(n) for n in ("policy.depth_ff.wrench", "dataset.depth_ff", "ctl.depth_hold.target",
                                              "nav.state", "policy.depth_ff.active"))
    if pw is None or ds is None or act is None or tg is None:
        return
    p_a = s.ref("policy.depth_ff.wrench", "policy.depth_ff.active")
    p_n = s.ref("policy.depth_ff.wrench", "nav.state")
    p_t = s.ref("policy.depth_ff.wrench", "ctl.depth_hold.target")
    k = int(p_a.max())
    path = os.path.join(s.path, "weights", "%016x.hgw" % int(act["hash"][k]))
    if k < 0 or not os.path.exists(path):
        return
    g = hgw.read(path).graph
    fields = [b.field for b in g.inputs]
    idx = numpy.nonzero((p_a == k) & (p_n >= 0) & (p_t >= 0))[0]
    anchor = ns["seq"][p_n[idx]]
    r = numpy.minimum(numpy.searchsorted(ds["anchor_seq"], anchor), len(ds) - 1)
    ok = ds["anchor_seq"][r] == anchor
    idx, r = idx[ok], r[ok]
    same = tg["seq"][p_t[idx]].astype(numpy.int64) == ds["ctl.depth_hold.target._seq"][r].astype(numpy.int64)
    # Every anchor read differently, and the newest of the others up to the limit.
    pick = numpy.concatenate([numpy.nonzero(~same)[0], numpy.nonzero(same)[0][-limit:]])
    m = reference.Model(g)
    d = numpy.array([abs(m.step([[float(ds[f][r[i]]) for f in fields]])[0][0] - float(pw["heave"][idx[i]])) for i in pick])
    sm = same[pick]

    def stats(x):
        return {"anchors": int(len(x)), "heave difference, max": float(x.max()) if len(x) else None,
                "over 1e-4": int((x > 1e-4).sum()), "over 1e-6": int((x > 1e-6).sum())}
    res["mismatch"]["outputs, by the message read"] = {
        "model": "%016x" % int(act["hash"][k]), "the same message": stats(d[sm]), "another message": stats(d[~sm])}


def block_models(s, ev, res):
    """Which model's residual each block's commands carried: the hashes of
    the policy.depth_ff.active messages its ctl.wrench messages lead back to."""
    act, cw = s.arr("policy.depth_ff.active"), s.arr("ctl.wrench")
    out = {}
    if act is not None and cw is not None and s.arr("policy.depth_ff.wrench") is not None:
        w_a = E.via(s.ref("ctl.wrench", "policy.depth_ff.wrench"), s.ref("policy.depth_ff.wrench", "policy.depth_ff.active"))
        for name in [b[0] for b in E.BLOCKS]:
            sp = span_of(ev, name)
            if sp:
                k = (cw["publish_ns"] >= sp[0]) & (cw["publish_ns"] < sp[1])
                out[name] = sorted(set("%016x" % int(act["hash"][i]) for i in numpy.unique(w_a[k]) if i >= 0))
    res["block_models"] = out


def timing(s, res):
    """Whether the run was disturbed: every 50 Hz topic's intervals over the
    whole session."""
    out = {}
    for n in ("nav.state", "ctl.depth_hold", "ctl.wrench", "thr.cmd", "policy.depth_ff.wrench"):
        a = s.arr(n)
        if a is None or len(a) < 3:
            continue
        d = numpy.diff(a["publish_ns"].astype(numpy.int64)) / 1e6
        out[n] = {"messages": int(len(a)), "interval_max_ms": round(float(d.max()), 2),
                  "over_30ms": int((d > 30).sum()), "over_25ms": int((d > 25).sum())}
    res["timing"] = out


def analyse(session, ev, trainer=None):
    s = E.Session(session)
    res = {"session": session}
    for f in (E.depth_error, E.trim_split):
        f(s, ev, res)
    E.control_latency(s, res)
    for f in (E.loop_latency, E.swap_cost, E.mismatch, mismatch_split, E.lineage, E.demotions, policy_output,
              command_activity, block_models):
        f(s, ev, res)
    timing(s, res)
    rows(s, ev, res, trainer)
    E.recorder_gaps(s, res)
    return res


# ---- The table over several runs -----------------------------------------------------

def med(xs, nd=2):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "-"
    return "%.*f (%.*f to %.*f)" % (nd, float(numpy.median(xs)), nd, min(xs), nd, max(xs))


def _get(r, *path):
    for p in path:
        r = r.get(p) if isinstance(r, dict) else None
        if r is None:
            return None
    return r


def both_steps(r, b):
    """A block's two 1 m steps together: the integral of |error|, m s."""
    d, u = _get(r, "blocks", b, "down", "iae_m_s"), _get(r, "blocks", b, "up", "iae_m_s")
    return None if d is None or u is None else d + u


def run_faults(r):
    """What makes an adaptation run unfit for the comparison: a block that
    did not run under the model the schedule names, or without the residual
    in every command, or a demotion."""
    f = []
    h1, h2 = _get(r, "deploys", "M1", "hash"), _get(r, "deploys", "M2", "hash")
    want = {"A2": h1, "P1": h1, "P3": h1, "P2": h2, "N2": h2}
    for b, h in want.items():
        if _get(r, "block_models", b) != [h]:
            f.append("%s ran %s, not %s" % (b, _get(r, "block_models", b), h))
        if _get(r, "residual_share_pct", b) != 100.0:
            f.append("%s: the residual in %s %% of the commands" % (b, _get(r, "residual_share_pct", b)))
    if h1 == h2:
        f.append("M1 and M2 are one file")
    n = len(_get(r, "demotions", "demotions") or [])
    if n:
        f.append("%d demotions" % n)
    return f


def run_disturbed(r):
    """The machine's marks on a run: a 50 Hz interval over 30 ms, a message
    the recorder lost, a row the trainer lost."""
    d = []
    n = sum(x.get("over_30ms", 0) for x in (r.get("timing") or {}).values())
    if n:
        d.append("%d intervals over 30 ms" % n)
    n = sum((r.get("recorder_gaps") or {}).values())
    if n:
        d.append("the recorder lost %d" % n)
    n = _get(r, "rows", "the trainer's stream", "lost")
    if n:
        d.append("the trainer lost %d rows" % n)
    return d


def adapt_verdict(learned, oracle):
    """The criterion of bench/e2e-depth-ff/README.md, "Adaptation", written
    there before the comparison was run, applied to the runs' reports:
    (verdict, the lines that say why). J is both_steps() with the payload."""
    L = []

    def separated(rs, what_new, what_old):
        new = [both_steps(r, "P2") for r in rs]
        old = [both_steps(r, b) for r in rs for b in ("P1", "P3")]
        if not rs or None in new or None in old:
            return None
        lower_in = sum(1 for r in rs if both_steps(r, "P2") < min(both_steps(r, "P1"), both_steps(r, "P3")))
        apart = max(new) < min(old)
        gain = [0.5 * (both_steps(r, "P1") + both_steps(r, "P3")) - both_steps(r, "P2") for r in rs]
        L.append("%s: J %.2f to %.2f m s; %s: %.2f to %.2f. Lower than before and after it in the same run in "
                 "%d of %d runs; the ranges %s. The run's own gain (the mean of before and after, less it): %s m s." % (
                     what_new, min(new), max(new), what_old, min(old), max(old), lower_in, len(rs),
                     "do not overlap" if apart else "overlap", med(gain)))
        return lower_in == len(rs) and apart
    faults = [(r.get("variant"), r.get("seed"), f) for r in learned + oracle for f in run_faults(r)]
    for v, sd, f in faults:
        L.append("Unfit: %s, seed %s: %s." % (v, sd, f))
    o = separated(oracle, "The oracle of the vehicle with the payload (P2)", "the oracle of the vehicle without it (P1, P3)")
    m = separated(learned, "M2 (P2)", "M1 (P1, P3)")
    if len(learned) < 5 or len(oracle) < 5:
        L.append("Fewer than five runs of a variant: %d learned, %d oracle." % (len(learned), len(oracle)))
        return "undecided", L
    if faults or o is None or m is None:
        return "undecided", L
    if not o:
        L.append("The two oracles do not differ by the criterion: on this vehicle and with these runs there was "
                 "nothing a later model could have been seen to gain.")
        return "undecided", L
    return ("shown" if m else "not shown"), L


def summarise_adapt(dirs):
    """The adaptation experiment's table: every run with the load it was
    made under, every metric per seed, and the criterion's verdict."""
    rs = []
    for d in dirs:
        r = json.load(open(os.path.join(d, "report.json")))
        r["dir"] = os.path.basename(os.path.normpath(d))
        rs.append(r)
    rs.sort(key=lambda r: (r.get("variant"), r.get("seed")))
    L = ["## The runs", "",
         "| Run | Load average, 1 min: at the start, highest, at the end (of %s threads) | nav.depth to thr.cmd: median, "
         "p99 (ms) | 50 Hz intervals over 25 ms, over 30 ms | Recorder lost | Trainer: rows lost, steps | "
         "Demotions | Unfit |" % (_get(rs[0], "load_average", "threads") if rs else "?"), "|---|---|---|---|---|---|---|---|"]
    for r in rs:
        la = r.get("load_average") or {}
        L.append("| %s | %s, %s, %s | %s, %s | %d, %d | %d | %s, %s | %d | %s |" % (
            r["dir"], (la.get("at the start (1, 5, 15 min)") or ["?"])[0], la.get("highest 1 min average during the run", "?"),
            (la.get("at the end (1, 5, 15 min)") or ["?"])[0],
            _get(r, "control_latency", "nav.depth to thr.cmd", "median_ms"),
            _get(r, "control_latency", "nav.depth to thr.cmd", "p99_ms"),
            sum(x.get("over_25ms", 0) for x in (r.get("timing") or {}).values()),
            sum(x.get("over_30ms", 0) for x in (r.get("timing") or {}).values()),
            sum((r.get("recorder_gaps") or {}).values()),
            _get(r, "rows", "the trainer's stream", "lost"), _get(r, "rows", "the trainer's stream", "steps"),
            len(_get(r, "demotions", "demotions") or []), "; ".join(run_faults(r)) or "no"))
    cols = [("A1", "Without the payload: controller alone (A1)"), ("A2", "M1 (A2)"), ("N2", "M2 (N2)"),
            ("P4", "With the payload: controller alone (P4)"), ("P1", "M1 (P1)"), ("P2", "M2 (P2)"),
            ("P3", "M1 again (P3)")]
    metrics = [("Both steps: IAE (m s)", lambda r, b: both_steps(r, b), 2),
               ("Step 3 to 4 m: IAE (m s)", lambda r, b: _get(r, "blocks", b, "down", "iae_m_s"), 2),
               ("Step 4 to 3 m: IAE (m s)", lambda r, b: _get(r, "blocks", b, "up", "iae_m_s"), 2),
               ("Step 3 to 4 m: overshoot (%)", lambda r, b: _get(r, "blocks", b, "down", "overshoot_pct"), 1),
               ("Step 4 to 3 m: overshoot (%)", lambda r, b: _get(r, "blocks", b, "up", "overshoot_pct"), 1),
               ("Hold at 3 m: rms (cm)", lambda r, b: _get(r, "blocks", b, "hold", "ss_rms_cm"), 2),
               ("Heave command: reversals per second", lambda r, b: _get(r, "command_activity", b, "reversals per second"), 2),
               ("Policy at its bound (% of ticks)", lambda r, b: None if _get(r, "policy_output", b) is None or
                _get(r, "residual_share_pct", b) in (None, 0.0) else
                100 * _get(r, "policy_output", b, "at its bound (share of ticks)"), 1),
               ("At the end of the block's hold: the integrator (heave)",
                lambda r, b: _get(r, "trim_carried_by", b, "integral"), 3),
               ("At the end of the block's hold: the policy's output (heave)", lambda r, b: None if
                _get(r, "residual_share_pct", b) in (None, 0.0) else _get(r, "trim_carried_by", b, "policy_heave"), 3)]
    by = {}
    for r in rs:
        by.setdefault(r.get("variant", "?"), []).append(r)
    for v, vr in sorted(by.items()):
        L += ["", "## %s: %d runs" % (v, len(vr)), ""]
        if v == "oracle":
            L += ["M1 is the oracle of the vehicle without the payload, M2 the oracle of the vehicle with it.", ""]
        for name, f, nd in metrics:
            L += ["| %s | %s |" % (name, " | ".join(c for _, c in cols)), "|---|" + "---|" * len(cols)]
            for r in vr:
                L.append("| seed %s | %s |" % (r.get("seed"), " | ".join(
                    "-" if f(r, b) is None else "%.*f" % (nd, f(r, b)) for b, _ in cols)))
            L.append("| median (smallest to largest) | %s |" % " | ".join(med([f(r, b) for r in vr], nd) for b, _ in cols))
            L.append("")
        L += ["| Transient at the 3 m hold | Peak (cm) | IAE (m s) |", "|---|---|---|"]
        for tag, mode, what in ADAPT_TRANSIENTS:
            xs = [x for x in (_get(r, "transients", tag) for r in vr) if x]
            if xs:
                L.append("| %s: %s, %s | %s | %s |" % (tag, what, mode, med([x.get("peak_cm") for x in xs]),
                                                       med([x.get("iae_m_s") for x in xs])))
    verdict, why = adapt_verdict(by.get("learned", []), by.get("oracle", []))
    dist = ["%s: %s" % (r["dir"], ", ".join(run_disturbed(r))) for r in rs if run_disturbed(r)]
    L += ["", "## The criterion", "", "Adaptation: **%s**." % verdict, ""] + ["- " + x for x in why]
    L.append("- Runs with the machine's marks on them: %s." % ("; ".join(dist) if dist else "none"))
    return "\n".join(L) + "\n"


def summarise(dirs):
    """Median (smallest to largest) over the runs of each variant, per metric."""
    if any(json.load(open(os.path.join(d, "report.json"))).get("schedule") == "adapt" for d in dirs):
        return summarise_adapt(dirs)
    runs = {}
    for d in dirs:
        r = json.load(open(os.path.join(d, "report.json")))
        runs.setdefault(r.get("variant", "?") + (", the supply changed" if r.get("change") == "supply" else ""), []).append(r)
    L = []

    def get(r, *path):
        for p in path:
            r = r.get(p) if isinstance(r, dict) else None
            if r is None:
                return None
        return r
    cols = [("assisted, nominal (B1)", "B1"), ("M1, nominal (B2)", "B2"), ("M1, heavier (B3)", "B3"),
            ("M2, heavier (B4)", "B4"), ("assisted, heavier (B5)", "B5")]
    for v, rs in sorted(runs.items()):
        L += ["", "### %s: %d runs (seeds %s)" % (v, len(rs), ", ".join(str(r.get("seed")) for r in rs)), "",
              "| | " + " | ".join(c for c, _ in cols) + " |", "|---|" + "---|" * len(cols)]
        for name, path in (("residual in the command (% of ticks)", None),
                           ("hold at 3 m: rms (cm)", ("hold", "ss_rms_cm")),
                           ("step 3 to 4 m: IAE (m s)", ("down", "iae_m_s")),
                           ("step 3 to 4 m: overshoot (%)", ("down", "overshoot_pct")),
                           ("step 4 to 3 m: IAE (m s)", ("up", "iae_m_s")),
                           ("step 4 to 3 m: overshoot (%)", ("up", "overshoot_pct")),
                           ("policy at its bound (% of ticks)", "bound"),
                           ("heave command: reversals per second", "reversals"),
                           ("at the hold's end: the integrator", "integral"),
                           ("at the hold's end: the policy's output", "policy_heave")):
            cells = []
            for _, b in cols:
                if path is None:
                    cells.append(med([get(r, "residual_share_pct", b) for r in rs]))
                elif path == "bound":
                    cells.append(med([None if get(r, "policy_output", b) is None else
                                      100 * get(r, "policy_output", b, "at its bound (share of ticks)") for r in rs]))
                elif path == "reversals":
                    cells.append(med([get(r, "command_activity", b, "reversals per second") for r in rs]))
                elif isinstance(path, str):
                    cells.append(med([get(r, "trim_carried_by", b, path) for r in rs], 3))
                else:
                    cells.append(med([get(r, "blocks", b, *path) for r in rs]))
            L.append("| %s | %s |" % (name, " | ".join(cells)))
        L += ["", "| Transient at the 3 m hold | Peak (cm) | IAE (m s) | Within 5 cm after (s) | Runs still outside at the end |",
              "|---|---|---|---|---|"]
        for tag, mode, what in E.TRANSIENTS:
            xs = [get(r, "transients", tag) for r in rs]
            xs = [x for x in xs if x]
            if xs:
                L.append("| %s: %s, %s | %s | %s | %s | %d of %d |" % (
                    tag, what, mode, med([x.get("peak_cm") for x in xs]), med([x.get("iae_m_s") for x in xs]),
                    med([x.get("within_5cm_s") for x in xs]),
                    sum(1 for x in xs if x.get("within_5cm_s") is None), len(xs)))
        L += ["", "Demotions per run: %s. Rows lost per run: %s. Intervals over 30 ms per run (five 50 Hz topics): %s." % (
            ", ".join(str(len(get(r, "demotions", "demotions") or [])) for r in rs),
            ", ".join(str(get(r, "rows", "the trainer's stream", "lost")) for r in rs),
            ", ".join(str(sum(x.get("over_30ms", 0) for x in (r.get("timing") or {}).values())) for r in rs))]
        # Each run's own loop delay: the seed does not fix it (docs/14
        # "Determinism and replay"), so it is reported beside the seeds.
        L.append("Loop delay per run, nav.depth to thr.cmd, median (ms): %s." % ", ".join(
            str(get(r, "control_latency", "nav.depth to thr.cmd", "median_ms")) for r in rs))
        # The first experiment's checks.
        ll = [m for r in rs for m in (get(r, "loop_latency", "models") or [])]
        for name in ("M1", "M2"):
            for key in ("deploy called", "swapped (policy.active)", "first thr.cmd carrying it"):
                xs = [m.get(key) for m in ll if m.get("model") == name]
                L.append("%s, newest row trained on to %s (ms): %s." % (name, key, med(xs, 1)))
        mi = [get(r, "mismatch", "inputs") or {} for r in rs]
        mo = [get(r, "mismatch", "outputs") or {} for r in rs]
        L.append("Train/serve: %d anchors with an output and a row, %d with another ctl.depth_hold.target message; "
                 "outputs against the reference on the rows, largest difference per run: %s." % (
                     sum(x.get("anchors with an output and a row", 0) for x in mi), sum(x.get("another message", 0) for x in mi),
                     ", ".join("%.1e" % x["heave difference, max"] if "heave difference, max" in x else "-" for x in mo)))
        L.append("Lineage complete per run: %s; thr.cmd carrying a residual / leading back to a policy.active: %s." % (
            ", ".join(str(get(r, "lineage", "one command followed back", "complete")) for r in rs),
            ", ".join("%s/%s" % (get(r, "lineage", "thr.cmd carrying a residual"),
                                 get(r, "lineage", "of them leading back to a policy.active")) for r in rs)))
        L.append("Recorder gaps per run: %s. Anchors without a row: %s. Trainer steps: %s." % (
            ", ".join(str(sum((r.get("recorder_gaps") or {}).values())) for r in rs),
            ", ".join(str(get(r, "rows", "anchors without a row")) for r in rs),
            ", ".join(str(get(r, "rows", "the trainer's stream", "steps")) for r in rs)))
        # Paired: each run's residual block less its own assisted block.
        L += ["", "| The run's own difference, residual less assisted | Median (smallest to largest) | Runs better / worse |",
              "|---|---|---|"]
        for name, b_res, b_ass, path in (
                ("M1 nominal, step 3 to 4 m: IAE", "B2", "B1", ("down", "iae_m_s")),
                ("M1 nominal, step 4 to 3 m: IAE", "B2", "B1", ("up", "iae_m_s")),
                ("M1 nominal, hold rms (cm)", "B2", "B1", ("hold", "ss_rms_cm")),
                ("M1 heavier, step 3 to 4 m: IAE", "B3", "B5", ("down", "iae_m_s")),
                ("M1 heavier, step 4 to 3 m: IAE", "B3", "B5", ("up", "iae_m_s")),
                ("M2 heavier, step 3 to 4 m: IAE", "B4", "B5", ("down", "iae_m_s")),
                ("M2 heavier, step 4 to 3 m: IAE", "B4", "B5", ("up", "iae_m_s")),
                ("M2 heavier, hold rms (cm)", "B4", "B5", ("hold", "ss_rms_cm"))):
            dd = [get(r, "blocks", b_res, *path) - get(r, "blocks", b_ass, *path) for r in rs
                  if get(r, "blocks", b_res, *path) is not None and get(r, "blocks", b_ass, *path) is not None]
            if dd:
                L.append("| %s | %s | %d / %d |" % (name, med(dd), sum(1 for x in dd if x < 0), sum(1 for x in dd if x > 0)))
        dd = [(get(r, "transients", "T2", "peak_cm"), get(r, "transients", "T1", "peak_cm")) for r in rs]
        dd = [x - y for x, y in dd if x is not None and y is not None]
        if dd:
            L.append("| buoyancy -4 N, peak (cm): with M1 (T2) less assisted (T1) | %s | %d / %d |" % (
                med(dd), sum(1 for x in dd if x < 0), sum(1 for x in dd if x > 0)))
        dd = [(get(r, "transients", "T2", "iae_m_s"), get(r, "transients", "T1", "iae_m_s")) for r in rs]
        dd = [x - y for x, y in dd if x is not None and y is not None]
        if dd:
            L.append("| buoyancy -4 N, IAE (m s): with M1 (T2) less assisted (T1) | %s | %d / %d |" % (
                med(dd), sum(1 for x in dd if x < 0), sum(1 for x in dd if x > 0)))
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", help="the manager's HANGAR_ROOT")
    ap.add_argument("--out", help="where events.log, events.json, report.json and report.md go")
    ap.add_argument("--variant", choices=("learned", "oracle", "collect"), default="learned")
    ap.add_argument("--seed", type=int, default=1, help="the trainer's (the simulator's is the manifest's)")
    ap.add_argument("--change", choices=("buoyancy", "supply"), default="buoyancy",
                    help="what changes at T1, T2 and T3: the buoyancy by 4 N, or the thrusters' supply to 12.6 V")
    ap.add_argument("--adapt", action="store_true",
                    help="the adaptation experiment's schedule: a payload none of the policy's inputs shows")
    ap.add_argument("--record-s", type=float, default=180.0,
                    help="--adapt: seconds of the training phase's steps with M1 on the vehicle with the payload")
    ap.add_argument("--trainer-nice", type=int, default=10, help="the nice value of the training thread")
    ap.add_argument("--quick", action="store_true", help="every phase shortened: a smoke run")
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--replay", default="3min")
    ap.add_argument("--steps-per-s", type=float, default=60.0, help="the trainer's pace")
    ap.add_argument("--oracle-steps", type=int, default=6000)
    ap.add_argument("--settle-steps", type=int, default=300, help="policy steps on the plant before an export")
    ap.add_argument("--voltage", type=float, default=16.8, help="the supply the oracle reads the thrust table at")
    ap.add_argument("--analyse", help="a session directory holding events.json: the analysis alone")
    ap.add_argument("--summarise", nargs="+", help="directories holding a report.json each: the table over them")
    a = ap.parse_args()
    if a.summarise:
        sys.stdout.write(summarise(a.summarise))
        return 0
    if not a.out:
        ap.error("--out is needed")
    q = 0.4 if a.quick else 1.0
    a.settle_s, a.train_s, a.train_hold_s = 30 * q, 120 * q, 10 * q
    a.hold_s, a.step_s, a.transient_s = 20 * q, 25 * q, 40 * q
    a.rows_first_s = 10 * q
    a.record_s *= q
    os.makedirs(a.out, exist_ok=True)
    E.T0 = E.now()
    if a.analyse:
        ev = json.load(open(os.path.join(a.analyse, "events.json")))
        E.T0 = ev[0]["t"]
        old = os.path.join(a.out, "report.json")
        old = json.load(open(old)) if os.path.exists(old) else {}
        if ev[0].get("schedule") == "adapt":
            E.BLOCKS, E.TRANSIENTS = ADAPT_BLOCKS, ADAPT_TRANSIENTS
        res = analyse(a.analyse, ev, old.get("rows", {}).get("the trainer's stream"))
        for k in ("variant", "seed", "change", "deploys", "schedule", "load_average"):
            if k in old:
                res[k] = old[k]
        if "the mux's log" in old.get("demotions", {}):
            res["demotions"]["the mux's log"] = old["demotions"]["the mux's log"]
    else:
        if not a.root:
            ap.error("--root is needed for a run")
        log = E.Log(os.path.join(a.out, "events.log"))
        if a.adapt:
            E.BLOCKS, E.TRANSIENTS = ADAPT_BLOCKS, ADAPT_TRANSIENTS
        log("start", variant=a.variant, seed=a.seed, quick=a.quick, change="payload" if a.adapt else a.change,
            schedule="adapt" if a.adapt else "second")
        st = htopic.open_topic("recorder.session", timeout=5.0, start="oldest")
        trainer = Trainer(log, a)
        load = Load()
        load.start()
        log("load", **load.report())
        try:
            m1, m2 = (schedule_adapt if a.adapt else schedule)(log, a, trainer)
        finally:
            trainer.stop = True
            try:
                E.call("safety.disarm")
                log("disarmed")
            except Exception as e:  # noqa: BLE001
                log("disarm failed", error=str(e))
        load.stop = True
        log("load", **load.report())
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
        if trainer.stream is None:
            print("the trainer never started", file=sys.stderr)
            return 1
        span = (trainer.t_last - trainer.t_first) / 1e9 if trainer.t_first else 0.0
        res = analyse(session, ev, {"rows": trainer.stream.replay.total, "lost": trainer.stream.lost,
                                    "steps": trainer.steps, "steps_per_s": round(trainer.steps / span, 1) if span else None,
                                    "batch": a.batch, "replay": a.replay, "lag": depth_ff2.LAG,
                                    "bound": depth_ff2.BOUND})
        res["variant"], res["seed"], res["change"] = a.variant, a.seed, "payload" if a.adapt else a.change
        res["schedule"], res["load_average"] = "adapt" if a.adapt else "second", load.report()
        res["deploys"] = {"M1": m1, "M2": m2}
        try:   # why the mux demoted, in its own words
            with open(os.path.join(a.root, "run", "hangar", "log", "mux.log")) as f:
                res["demotions"]["the mux's log"] = [ln.strip() for ln in f if "demoted" in ln]
        except OSError:
            pass
    json.dump(res, open(os.path.join(a.out, "report.json"), "w"), indent=1, default=str)
    open(os.path.join(a.out, "report.md"), "w").write(E.markdown(res))
    print(json.dumps({k: res[k] for k in ("blocks", "transients", "residual_share_pct", "demotions", "policy_output",
                                          "block_models", "load_average") if k in res}, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
