# SPDX-License-Identifier: LGPL-3.0-or-later
"""The learning loop's topside side, version 1 (docs/09-learning.md
"Training"): rows in, weights out, lineage recorded.

    from hangar import learn
    stream = learn.stream("dataset.depth_ff", batch=32, seq_len=50)   # stateful chunks
    for chunk in stream:
        h = learn.carry(h, chunk.fresh)          # torch: detached, zero where a slot starts a run
        out, h = model(torch.from_numpy(chunk.inputs), h)
        loss = (((out - torch.from_numpy(chunk.labels)) ** 2).mean(-1) * torch.from_numpy(chunk.mask)).sum()
        ...
        learn.deploy(model, policy="depth_ff", stream=stream, outputs=["heave"], probes=stream.probes(8))

stream() reads the live dataset.<name> topic in place (hangar.topic) into a
replay buffer; open_dataset() does the same over a built dataset file.
Chunks are numpy arrays: inputs [batch, seq_len, n_in], labels [batch,
seq_len, n_label], mask [batch, seq_len] (0 on masked rows, padding and a
warm-up chunk), fresh [batch] (the slot starts from zero state). Without
seq_len a chunk is `batch` rows drawn from the buffer, [batch, n].

export() writes the .hgw with lib/nn's exporter (hangar.nn.export_torch):
bindings to the stream's fields, probes from the reference, the lineage of
the rows trained on, checked against the policy's runtime_info.

until the bridge: deploy() puts the file in the policy's incoming
directory itself (`<HANGAR_ROOT>/var/lib/hangar/policy_<name>/incoming/`),
which only works where trainer and policy share a file system (the bench,
the sim); the bridge's bridge.send replaces that copy (docs/08). Calls go
through `hangar call` (the manager, as admin), as a script run from a shell
does (docs/12 "The python package").

numpy for the stream; PyTorch only for carry() and for exporting a model.
"""

import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import time
from collections import deque, namedtuple

from . import dataset as _dataset
from . import record, schema
from . import topic as _topic

RESET = _dataset.RESET
MASKED = _dataset.MASKED
OFF_FLAGS = 36

Chunk = namedtuple("Chunk", "inputs labels mask fresh")


class LearnError(RuntimeError):
    pass


def _np():
    import numpy
    return numpy


def _duration_ns(text):
    if isinstance(text, (int, float)):
        return int(text * 1e9)
    s = text.strip()
    for unit, scale in (("min", 60 * 10 ** 9), ("ms", 10 ** 6), ("us", 10 ** 3), ("ns", 1), ("h", 3600 * 10 ** 9),
                        ("s", 10 ** 9)):
        if s.endswith(unit):
            return int(float(s[:-len(unit)]) * scale)
    raise ValueError("a duration: 10min, 30s, 500ms: %r" % text)


# ---- Rows ------------------------------------------------------------------------

class _RowCodec(object):
    """A row type's input and label fields as vectors (the row schema's
    role= comments, docs/06 "The row")."""

    def __init__(self, layout, schema_text, inputs=None, labels=None):
        self.layout = layout
        self.schema_text = schema_text
        self.hash = schema.type_hash(schema_text, generated=True)
        by_name = {f.name: f for f in layout.fields}
        roles = {}
        for f in layout.fields:
            c = (f.comment or "")
            roles[f.name] = c[5:].split()[0] if c.startswith("role=") else "meta"

        def numeric(f):
            return f.kind != "char" and not f.is_array and f.count == 1

        self.inputs = list(inputs) if inputs is not None else \
            [f.name for f in layout.fields if roles[f.name] == "input" and numeric(f) and not f.name.endswith(
                ("._seq", "._stamp_ns", "._count", "._flags"))]
        self.labels = list(labels) if labels is not None else \
            [f.name for f in layout.fields if roles[f.name] == "label" and numeric(f) and not f.name.endswith(
                ("._seq", "._stamp_ns", "._count", "._flags"))]
        self._in = [self._locate(by_name, n) for n in self.inputs]
        self._lab = [self._locate(by_name, n) for n in self.labels]

    @staticmethod
    def _locate(by_name, n):
        """(offset, struct code) of a scalar field, or of one element of an
        array field named `field[i]` (thr.out.pwm[4])."""
        base, idx = n, 0
        if n.endswith("]") and "[" in n:
            base, _, i = n[:-1].partition("[")
            if not i.isdigit():
                raise LearnError("%s: an element is field[i]" % n)
            idx = int(i)
        f = by_name.get(base)
        if f is None or f.kind == "char" or idx >= f.count or (f.is_array and base == n):
            raise LearnError("%s: not a numeric field (or element) of the row" % n)
        size = schema.KINDS[f.kind][0]
        return (f.offset + idx * size, "<" + schema.KINDS[f.kind][2])

    def row(self, payload, lost_before=False):
        seq, stamp = struct.unpack_from("<QQ", payload, 0)
        boot = bytes(payload[16:32])
        flags = struct.unpack_from("<I", payload, OFF_FLAGS)[0]
        x = [struct.unpack_from(c, payload, o)[0] for o, c in self._in]
        y = [struct.unpack_from(c, payload, o)[0] for o, c in self._lab]
        return _Row(seq, stamp, boot, flags, x, y, lost_before)


class _Row(object):
    __slots__ = ("seq", "stamp", "boot", "flags", "x", "y", "starts_run")

    def __init__(self, seq, stamp, boot, flags, x, y, lost_before):
        self.seq, self.stamp, self.boot, self.flags, self.x, self.y = seq, stamp, boot, flags, x, y
        # A run starts at a reset, after a row lost (a fresh start the vehicle
        # did not make), and at a change of boot (set by the buffer).
        self.starts_run = bool(flags & RESET) or lost_before


# ---- The replay buffer and the chunks ------------------------------------------

class Replay(object):
    """The recent rows, by the anchor's stamp: rows older than `span` behind
    the newest are dropped. Indices are absolute (they keep counting as rows
    are dropped from the front)."""

    def __init__(self, span="10min", recent="2min", rng=None):
        self.span_ns = _duration_ns(span) if span is not None else None
        self.recent_ns = _duration_ns(recent)
        self.rows = deque()
        self.base = 0
        self.rng = rng or random.Random(0)
        self.total = 0

    def __len__(self):
        return len(self.rows)

    @property
    def end(self):
        return self.base + len(self.rows)

    def add(self, row):
        if self.rows and row.boot != self.rows[-1].boot:
            row.starts_run = True
        self.rows.append(row)
        self.total += 1
        if self.span_ns is not None:
            newest = row.stamp
            while len(self.rows) > 1 and newest - self.rows[0].stamp > self.span_ns:
                self.rows.popleft()
                self.base += 1

    def at(self, i):
        return self.rows[i - self.base]

    def draw(self):
        """An absolute index: half of the draws from the last `recent`."""
        n = len(self.rows)
        if n == 0:
            return None
        if self.rng.random() < 0.5:
            newest = self.rows[-1].stamp
            lo, hi = 0, n - 1
            while lo < hi:  # the first row within `recent` of the newest
                mid = (lo + hi) // 2
                if newest - self.rows[mid].stamp > self.recent_ns:
                    lo = mid + 1
                else:
                    hi = mid
            return self.base + self.rng.randrange(lo, n)
        return self.base + self.rng.randrange(n)


class _Slot(object):
    __slots__ = ("pos", "fresh", "warm")

    def __init__(self):
        self.pos, self.fresh, self.warm = None, True, False


class Chunker(object):
    """Stateful chunks over a Replay (docs/09 "Stateful chunks")."""

    def __init__(self, replay, batch, seq_len, n_in, n_label):
        self.replay, self.batch, self.seq_len = replay, batch, seq_len
        self.n_in, self.n_label = n_in, n_label
        self.slots = [_Slot() for _ in range(batch)]

    def _start(self, s):
        s.pos = self.replay.draw()
        s.fresh = True
        # Inside a run, the first chunk carries no loss: the state it learns
        # from has then seen seq_len rows.
        s.warm = not self.replay.at(s.pos).starts_run

    def next(self):
        np = _np()
        B, T = self.batch, self.seq_len
        X = np.zeros((B, T, self.n_in), np.float32)
        Y = np.zeros((B, T, self.n_label), np.float32)
        M = np.zeros((B, T), np.float32)
        F = np.zeros((B,), np.bool_)
        rp = self.replay
        for b, s in enumerate(self.slots):
            if s.pos is None or s.pos < rp.base or s.pos >= rp.end:
                self._start(s)
            elif not s.fresh and rp.at(s.pos).starts_run:
                # The chunk before filled exactly and the run ended with it:
                # this one starts a run, from zero state, at its first row.
                s.fresh, s.warm = True, False
            F[b] = s.fresh
            t = 0
            i = s.pos
            ended = False
            while t < T:
                if i >= rp.end:
                    ended = True  # reached the newest row
                    break
                r = rp.at(i)
                if t > 0 and r.starts_run:
                    ended = True  # a reset, a row lost, a new boot: the run ends here
                    break
                X[b, t] = r.x
                Y[b, t] = r.y
                M[b, t] = 0.0 if (s.warm or r.flags & MASKED) else 1.0
                t += 1
                i += 1
            if ended:
                s.pos = None
            else:
                s.pos = i
                s.fresh = False
                s.warm = False
        return Chunk(X, Y, M, F)


class _Stream(object):
    """Rows from a source into a Replay, as chunks."""

    def __init__(self, codec, batch, seq_len, replay, name):
        self.codec = codec
        self.batch = batch
        self.seq_len = seq_len
        self.replay = replay
        self.name = name
        self.lost = 0           # rows lost on the way (the topic's gaps)
        self.chunks = 0
        self._chunker = Chunker(replay, batch, seq_len, len(codec.inputs), len(codec.labels)) if seq_len else None

    @property
    def input_fields(self):
        return list(self.codec.inputs)

    @property
    def label_fields(self):
        return list(self.codec.labels)

    @property
    def dataset_hash(self):
        return self.codec.hash

    def _ingest(self):
        return 0

    def _ready(self):
        return len(self.replay) >= max(self.seq_len or 1, 1)

    def next_chunk(self, timeout=None):
        end = None if timeout is None else time.monotonic() + timeout
        self._ingest()
        while not self._ready():
            if end is not None and time.monotonic() >= end:
                raise LearnError("%s: no rows within %.1f s" % (self.name, timeout))
            if not self._wait():
                raise LearnError("%s: %d rows, fewer than one chunk needs" % (self.name, len(self.replay)))
            self._ingest()
        self.chunks += 1
        if self._chunker:
            return self._chunker.next()
        np = _np()
        idx = [self.replay.draw() for _ in range(self.batch)]
        rows = [self.replay.at(i) for i in idx]
        X = np.array([r.x for r in rows], np.float32).reshape(self.batch, len(self.codec.inputs))
        Y = np.array([r.y for r in rows], np.float32).reshape(self.batch, len(self.codec.labels))
        M = np.array([0.0 if r.flags & MASKED else 1.0 for r in rows], np.float32)
        return Chunk(X, Y, M, np.ones((self.batch,), np.bool_))

    def _wait(self):
        return False

    def __iter__(self):
        while True:
            yield self.next_chunk()

    def probes(self, n=8, count=1):
        """`count` sequences of n input rows for export's probes: from a run's
        start when the buffer has one that long (a probe starts from zero
        state, as a run does), else the newest n rows."""
        rp = self.replay
        out = []
        starts = [i for i in range(rp.base, rp.end - n + 1)
                  if rp.at(i).starts_run and not any(rp.at(j).starts_run for j in range(i + 1, i + n))]
        for k in range(count):
            i = starts[-1 - k] if k < len(starts) else max(rp.base, rp.end - n)
            out.append([list(rp.at(j).x) for j in range(i, min(i + n, rp.end))])
        return out

    def lineage(self, session="live"):
        """hgw.Lineage per boot over the rows in the buffer (at most 8)."""
        from .nn import hgw
        by_boot = []
        for r in self.replay.rows:
            if not by_boot or by_boot[-1][0] != r.boot:
                by_boot.append([r.boot, r.seq, r.seq, r.stamp, r.stamp, 0])
            e = by_boot[-1]
            e[2], e[4], e[5] = r.seq, r.stamp, e[5] + 1
        return [hgw.Lineage(boot=b, session=session, first_seq=fs, last_seq=ls, first_stamp_ns=ft,
                            last_stamp_ns=lt, rows=n) for b, fs, ls, ft, lt, n in by_boot[-hgw.MAX_LINEAGE:]]


class LiveStream(_Stream):
    def __init__(self, topic_name, batch, seq_len=None, replay="10min", inputs=None, labels=None,
                 directory=None, open_timeout=5.0, seed=0):
        self.topic = _topic.open_topic(topic_name, directory, start="oldest", timeout=open_timeout)
        if not self.topic.layout:
            raise LearnError("%s: no schema" % topic_name)
        codec = _RowCodec(self.topic.layout, self.topic.schema, inputs, labels)
        _Stream.__init__(self, codec, batch, seq_len, Replay(replay, rng=random.Random(seed)), topic_name)

    def _ingest(self):
        msgs = self.topic.read()
        for m in msgs:
            if m.gap:
                self.lost += m.gap
            self.replay.add(self.codec.row(m.payload, lost_before=m.gap > 0))
        return len(msgs)

    def _wait(self):
        time.sleep(0.01)
        return True


class FileStream(_Stream):
    def __init__(self, path, batch, seq_len=None, inputs=None, labels=None, seed=0):
        ds = _dataset.open_dataset(path)
        try:
            codec = _RowCodec(ds.layout, ds.header.schema, inputs, labels)
            rp = Replay(None, rng=random.Random(seed))
            for i in range(ds.count):
                rp.add(codec.row(ds.payload_bytes_at(i)))
        finally:
            ds.close()
        _Stream.__init__(self, codec, batch, seq_len, rp, path)


def stream(topic, batch, seq_len=None, replay="10min", **kw):
    """Rows from the live topic, through a replay buffer (see the module's doc)."""
    return LiveStream(topic, batch, seq_len, replay, **kw)


def open_dataset(path, batch=32, seq_len=None, **kw):
    """The same over a built dataset file (`hangar dataset build`, the pairer's sink)."""
    return FileStream(path, batch, seq_len, **kw)


def carry(state, fresh):
    """The recurrent state, detached, zeroed for the slots marked fresh.
    state: a tensor or a tuple of tensors (an LSTM's (h, c), [layers, batch,
    hidden]; or [batch, hidden]); None stays None (the model starts at zero)."""
    if state is None:
        return None
    if isinstance(state, (tuple, list)):
        return type(state)(carry(s, fresh) for s in state)
    import torch
    f = torch.as_tensor(fresh, dtype=torch.bool, device=state.device)
    keep = (~f).to(state.dtype)
    s = state.detach()
    if s.dim() == 3:
        return s * keep.view(1, -1, 1)
    return s * keep.view(-1, *([1] * (s.dim() - 1)))


# ---- The vehicle's side: calls and topics ---------------------------------------

def _hangar_bin():
    return os.environ.get("HANGAR_BIN") or shutil.which("hangar") or "hangar"


def call(target, timeout=5.0, **fields):
    """`hangar call target field=value ... --json`: the reply's fields, or
    LearnError with the status and reason."""
    argv = [_hangar_bin(), "call", target] + ["%s=%s" % kv for kv in fields.items()] + \
        ["--timeout", str(int(timeout * 1000)), "--json"]
    p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout + 5)
    out = p.stdout.decode("utf-8", "replace").strip()
    try:
        r = json.loads(out.splitlines()[-1]) if out else None
    except ValueError:
        r = None
    if not r:
        raise LearnError("%s: %s" % (target, (p.stderr.decode("utf-8", "replace") or out).strip()))
    if r.get("status") != "HG_OK":
        raise LearnError("%s: %s %s" % (target, r.get("status"), r.get("reason", "")))
    return r.get("reply", {})


def runtime_info(policy, timeout=5.0):
    """policy_<name>.runtime_info, with ops as a list of (type, version)."""
    r = call("policy_%s.runtime_info" % policy, timeout)
    ops = []
    for tok in str(r.get("ops", "")).split():
        t, _, v = tok.partition(":")
        ops.append((t, int(v)))
    r["ops"] = ops
    for k in ("arch", "dataset_hash", "tick_period_ns", "format_version"):
        if isinstance(r.get(k), str):
            r[k] = int(r[k], 0)
    return r


def revert(policy, timeout=5.0):
    return call("policy_%s.revert" % policy, timeout)


def watch(policy, timeout=None, directory=None):
    """("active" | "load", fields) as the policy publishes them, until
    timeout seconds pass (None: forever)."""
    # Opened now, not at the first next(): what is published after the call
    # is seen, even when the caller iterates later.
    ta = _topic.open_topic("policy.%s.active" % policy, directory, timeout=5.0)
    tl = _topic.open_topic("policy.%s.load" % policy, directory, timeout=5.0)
    return _watch(ta, tl, timeout)


def _watch(ta, tl, timeout):
    end = None if timeout is None else time.monotonic() + timeout
    while end is None or time.monotonic() < end:
        got = False
        for kind, t in (("load", tl), ("active", ta)):
            for m in t.read():
                got = True
                yield kind, t.decode(m)
        if not got:
            time.sleep(0.01)


# ---- Export and deploy ----------------------------------------------------------

def export(model, path, stream, outputs, probes=None, runtime=None, name=None, warmup_ticks=None,
           tick_period_ns=None, parent_hash=0, steps=0, session="live"):
    """The .hgw of a PyTorch model (hangar.nn.export_torch): bound to the
    stream's input fields and to `outputs` (wrench fields), the probes from
    the reference, the header's lineage from the stream's buffer; refused
    unless it matches `runtime` (runtime_info's dict) when given. The
    lineage's `steps`, `parent_hash` (the first 8 bytes of the BLAKE2s of
    the weights this training started from, 0 for none) and `session` (the
    recorder's session the rows are in, "live" for rows nobody recorded) are
    the caller's to give: the stream knows the rows, not the training."""
    from .nn import export_torch, hgw
    newest = stream.replay.rows[-1] if len(stream.replay) else None
    tp = tick_period_ns or (runtime or {}).get("tick_period_ns") or 0
    hdr = hgw.Header(
        name=name or (runtime or {}).get("dataset", "") or "",
        tick_period_ns=int(tp),
        warmup_ticks=warmup_ticks if warmup_ticks is not None else (stream.seq_len or 10),
        created_ns=time.time_ns(),
        trainer_program=os.path.basename(sys.argv[0] or "python")[:31],
        dataset=(runtime or {}).get("dataset", "") or _dataset_name(stream.name),
        dataset_hash=stream.dataset_hash,
        rows=stream.replay.total,
        steps=steps,
        parent_hash=parent_hash,
        newest_row_boot=newest.boot if newest else b"\0" * 16,
        newest_row_stamp_ns=newest.stamp if newest else 0,
        lineage=stream.lineage(session),
    )
    if runtime:
        if runtime.get("dataset_hash") and runtime["dataset_hash"] != stream.dataset_hash:
            raise LearnError("the stream's row type %016x is not the policy's %016x (another spec, or "
                             "other topics)" % (stream.dataset_hash, runtime["dataset_hash"]))
    graph = export_torch.export(model, path, stream.input_fields, outputs,
                                probe_inputs=probes if probes is not None else stream.probes(8),
                                header=hdr, lineage=hdr.lineage,
                                runtime_ops=(runtime or {}).get("ops"))
    if runtime and runtime.get("arch") and hgw.arch_hash(graph) != runtime["arch"]:
        os.unlink(path)
        raise LearnError("architecture %016x, the policy's Arch is %016x" % (hgw.arch_hash(graph), runtime["arch"]))
    return path


def _dataset_name(topic_name):
    base = os.path.basename(topic_name)
    if base.startswith("dataset."):
        base = base[8:]
    for ext in (".hgr",):
        if base.endswith(ext):
            base = base[:-len(ext)]
    return base


def state_dir(policy):
    """The policy's state directory (hangar-policy's mod_state_dir)."""
    root = os.environ.get("HANGAR_ROOT", "")
    return os.path.join(root + "/var/lib/hangar" if root else "/var/lib/hangar", "policy_" + policy)


def recording_session(directory=None):
    """The id of the recorder's open session (recorder.session), or "live"
    when nothing is being recorded or there is no recorder."""
    try:
        t = _topic.open_topic("recorder.session", directory, start="oldest")
    except _topic.TopicError:
        return "live"
    try:
        msgs = t.read()
        if not msgs:
            return "live"
        d = t.decode(msgs[-1])
        return d["id"] if int(d.get("active", 0)) and d.get("id") else "live"
    finally:
        t.close()


def deploy(model_or_file, policy, stream=None, outputs=None, probes=None, timeout=60.0, directory=None,
           incoming=None, steps=None, parent_hash=None, session=None):
    """Export (unless given a file), send, load_weights, and wait for the
    load's end: {"version", "state" ("done" | "rejected"), "check", "reason",
    "hash"}. until the bridge: the file is copied into the policy's incoming
    directory, not sent with bridge.send.

    The lineage a model is exported with: `steps`, the training steps so far
    (the caller counts them; 0 when not given); `parent_hash`, by default the
    hash under which this same model object was last deployed (its training
    went on from those weights), 0 at its first deploy; `session`, by default
    the recorder's open session, else "live". They were left at 0, 0 and
    "live" whatever the trainer knew: the header had the fields and deploy()
    had no way to fill them."""
    exported = not isinstance(model_or_file, (str, os.PathLike))
    if not exported:
        path = str(model_or_file)
    else:
        if stream is None or outputs is None:
            raise LearnError("deploying a model needs the stream and the output fields")
        rt = runtime_info(policy)
        import tempfile
        fd, path = tempfile.mkstemp(suffix=".hgw")
        os.close(fd)
        if parent_hash is None:
            parent_hash = getattr(model_or_file, "hangar_deployed_hash", 0)
        export(model_or_file, path, stream, outputs, probes, runtime=rt, steps=int(steps or 0),
               parent_hash=int(parent_hash), session=session or recording_session(directory))
    with open(path, "rb") as f:
        data = f.read()
    digest = hashlib.blake2s(data, digest_size=32).hexdigest()
    h64 = struct.unpack("<Q", bytes.fromhex(digest)[:8])[0]
    if exported:
        try:
            model_or_file.hangar_deployed_hash = h64  # the next deploy of this model started from here
        except AttributeError:
            pass
    name = "%016x.hgw" % h64
    inc = incoming or os.path.join(state_dir(policy), "incoming")  # until the bridge
    tmp = os.path.join(inc, "." + name + ".tmp")
    with open(tmp, "wb") as f:
        f.write(data)
    os.rename(tmp, os.path.join(inc, name))
    tl = _topic.open_topic("policy.%s.load" % policy, directory, timeout=5.0)
    tl.read()  # from now on
    rep = call("policy_%s.load_weights" % policy, 5.0, name=name, blake2s=digest)
    version = int(rep.get("version", 0))
    end = time.monotonic() + timeout
    states = {1: "checking", 2: "warming", 3: "done", 4: "rejected"}
    while time.monotonic() < end:
        for m in tl.read():
            d = tl.decode(m)
            if int(d["version"]) == version and int(d["state"]) in (3, 4):
                return {"version": version, "state": states[int(d["state"])], "check": int(d["check"]),
                        "reason": d.get("reason", ""), "hash": h64}
        time.sleep(0.01)
    raise LearnError("policy_%s: load of version %d did not end within %.0f s" % (policy, version, timeout))
