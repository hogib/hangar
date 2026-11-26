# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar.learn.deploy across the tether, end to end on this machine
(docs/09 "The loop, step by step", step 4; docs/08 "Files"): two managers in
two roots, a vehicle and a topside, joined by nothing but their bridges on
loopback, with a UDP forwarder in this process between the two that a test
can cut. On the vehicle a toy policy (modules/policy's hangar-policy on two
50 Hz topics) whose spec names the bridge's directory as its Incoming=; on
the topside the trainer's manifest, and this process as the operator.

Needs the built binaries (make all) and the manager's toy talker (built by
make check); skipped, with the reason, without them. Standard library only:
the models are written with hangar.nn's own writer and reference."""

import glob
import os
import shutil
import signal
import socket
import subprocess
import threading
import time
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from hangar import learn, topic  # noqa: E402
from hangar.nn import hgw, reference  # noqa: E402

BUILD = os.environ.get("HANGAR_BUILDDIR") or os.path.join(REPO, "builddir")
IN = ["toy.anchor.depth_m", "toy.target.depth_m"]
OUT = ["heave", "surge"]
HIDDEN = 128      # two layers of it: a weights file of about 70 KB, 2 s at FileRate=320kbit


def _binaries():
    toys = os.environ.get("HANGARD_TOYS")
    talker = os.path.join(toys, "talker") if toys else None
    if not talker:
        found = sorted(glob.glob(os.path.join(BUILD, "tests-*", "hangard-modules", "talker")), key=os.path.getmtime)
        talker = found[-1] if found else os.path.join(BUILD, "tests-debugoptimized", "hangard-modules", "talker")
    b = {n: os.path.join(BUILD, n) for n in ("hangard", "hangar", "hangar-bridge", "hangar-policy")}
    for k in ("hangard", "hangar"):
        env = os.environ.get(k.upper() + "_BIN")
        if env:
            b[k] = env
    b["talker"] = talker
    return b


BIN = _binaries()
MISSING = [p for p in BIN.values() if not os.access(p, os.X_OK)]


def model(seed):
    """Two inputs, two dense layers of HIDDEN, heave and surge; small weights
    drawn from the seed."""
    x = seed * 2654435761 % (1 << 32)

    def draw(n):
        nonlocal x
        out = []
        for _ in range(n):
            x = (x * 1664525 + 1013904223) % (1 << 32)
            out.append((x / float(1 << 32) - 0.5) * 0.02)
        return out

    H = HIDDEN
    g = hgw.Graph(
        buffers=[hgw.Buffer("in", "f32", [2]), hgw.Buffer("h1", "f32", [H]), hgw.Buffer("h2", "f32", [H]),
                 hgw.Buffer("out", "f32", [2])],
        tensors=[hgw.Tensor("W1", "f32", [H, 2], draw(2 * H)), hgw.Tensor("b1", "f32", [H], draw(H)),
                 hgw.Tensor("W2", "f32", [H, H], draw(H * H)), hgw.Tensor("b2", "f32", [H], draw(H)),
                 hgw.Tensor("W3", "f32", [2, H], draw(2 * H)), hgw.Tensor("b3", "f32", [2], draw(2))],
        ops=[hgw.Op("dense", 1, [2, H, 1], [0], 1, [0, 1]), hgw.Op("dense", 1, [H, H, 1], [1], 2, [2, 3]),
             hgw.Op("dense", 1, [H, 2, 1], [2], 3, [4, 5])],
        inputs=[hgw.Binding(0, e, f) for e, f in enumerate(IN)],
        outputs=[hgw.Binding(3, e, f) for e, f in enumerate(OUT)],
    )
    return g


def write_model(path, seed, dataset_hash, tamper=False):
    """The model's file; tamper: a probe output off by one (check 6). The file's hash as deploy() names it."""
    g = model(seed)
    hdr = hgw.Header(name="toy", tick_period_ns=20_000_000, warmup_ticks=3, dataset="toy",
                     dataset_hash=dataset_hash, rows=1, newest_row_stamp_ns=1, trainer_program="test_deploy_tether")
    probe = reference.make_probe(g, [[[0.0, 0.0]], [[1.0, 2.0]]])
    if tamper:
        probe.outputs[0][0][0] += 1.0
    data = hgw.write(path, g, hdr, [probe])
    return learn.struct.unpack("<Q", learn.hashlib.blake2s(data, digest_size=32).digest()[:8])[0]


class Forwarder(threading.Thread):
    """The tether: datagrams from one bridge's port to the other's, unless cut."""

    def __init__(self):
        threading.Thread.__init__(self, daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.ports = None      # (vehicle, topside)
        self.cut = False
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                d, frm = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            if self.cut or not self.ports:
                continue
            to = self.ports[1] if frm[1] == self.ports[0] else self.ports[0]
            try:
                self.sock.sendto(d, ("127.0.0.1", to))
            except OSError:
                pass


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def until(cond, seconds, what):
    """Waits for cond() to hold; its value, or an AssertionError naming what."""
    end = time.monotonic() + seconds
    while True:
        v = cond()
        if v:
            return v
        if time.monotonic() >= end:
            raise AssertionError("%s: not within %.0f s" % (what, seconds))
        time.sleep(0.02)


class Side(object):
    def __init__(self, role, base):
        self.role = role
        self.root = os.path.join(base, role[0])
        self.topics = os.path.join(self.root, "dev/shm/hangar")
        self.port = free_port()
        self.proc = None

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def write(self, rel, text):
        p = self.path(rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text)

    def lay_out(self, fwd_port, files_conf):
        for d in ("etc/hangar/modules", "usr/share/hangar/msg", "var/lib/hangar", "run/hangar"):
            os.makedirs(self.path(d), exist_ok=True)
        for m in glob.glob(os.path.join(REPO, "msg", "*.msg")):
            shutil.copy(m, self.path("usr/share/hangar/msg"))
        self.write("etc/hangar/hangard.conf", "Role=%s\nHealthTick=50ms\n" % self.role)
        self.write("etc/hangar/bridge.key", "5a" * 32 + "\n")
        self.write("etc/hangar/bridge.conf",
                   "[Bridge]\nPair=deploy\nKey=%s\nPort=%d\nPeer=127.0.0.1:%d\nMsgDir=%s\nFileRate=320kbit\n"
                   "[Mirror policy.*.active policy.*.load]\nDirection=up\nDelivery=reliable\n"
                   "[Calls]\nRemote=policy_*.*\n%s"
                   % (self.path("etc/hangar/bridge.key"), self.port, fwd_port, self.path("usr/share/hangar/msg"),
                      files_conf))

    def module(self, name, text):
        self.write("etc/hangar/modules/%s.module" % name, text)

    def env(self):
        e = dict(os.environ)
        for k in ("HANGAR_CONTROL", "HANGAR_WIRING", "HANGAR_WIRING_FILE", "HANGAR_PEER_TOPIC_DIR",
                  "HANGAR_TOPIC_DIR", "HANGAR_STATE_DIR", "NOTIFY_SOCKET"):
            e.pop(k, None)
        e["HANGAR_ROOT"] = self.root
        e["XDG_RUNTIME_DIR"] = self.root
        return e

    def start(self):
        log = open(self.path("hangard.log"), "ab")
        self.proc = subprocess.Popen([BIN["hangard"]], env=self.env(), stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        log.close()
        until(lambda: os.path.exists(self.path("run/hangar/control")), 10, "%s: the control socket" % self.role)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(5)
        self.proc = None

    def latest(self, name):
        """The newest message of a topic of this side, decoded; None without one."""
        try:
            t = topic.Topic(name, self.topics, start="oldest")
        except OSError:
            return None
        try:
            msgs = t.read()
            return t.decode(msgs[-1]) if msgs else None
        finally:
            t.close()

    def link_up(self):
        d = self.latest("bridge.%s.link" % self.role)
        return bool(d and d["state"] == 3 and d["config_match"] == 1)

    def logs(self):
        out = []
        for p in [self.path("hangard.log")] + sorted(glob.glob(self.path("run/hangar/log/*.log"))):
            try:
                with open(p, errors="replace") as f:
                    out.append("== %s\n%s" % (p, "".join(f.readlines()[-25:])))
            except OSError:
                pass
        return "\n".join(out)


FILES_CONF = "[Files policy_toy]\nDirection=down\nMaxSize=1MB\nAllow=admin\n"
POLICY_ALLOW = "Allow=admin topside:admin topside:trainer\n"


@unittest.skipIf(MISSING, "not built: %s (make all; make check builds the toy talker)"
                 % ", ".join(os.path.relpath(p, REPO) for p in MISSING))
class DeployAcrossTheTether(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = os.environ.get("HANGAR_TEST_BASE") or "/dev/shm/hangar-test-%d" % os.getuid()
        cls.base = os.path.join(base, "dt%d" % os.getpid())
        shutil.rmtree(cls.base, ignore_errors=True)
        cls.saved_env = {k: os.environ.get(k) for k in ("HANGAR_ROOT", "HANGAR_BIN", "HANGAR_TOPIC_DIR")}
        cls.fwd = Forwarder()
        cls.V, cls.T = Side("vehicle", cls.base), Side("topside", cls.base)
        V, T = cls.V, cls.T
        cls.fwd.ports = (V.port, T.port)
        cls.models = os.path.join(cls.base, "models")
        os.makedirs(cls.models)
        cls.arrive = V.path("var/lib/hangar/bridge/files/policy_toy")
        try:
            arch = "%016x" % hgw.arch_hash(model(1))
            V.lay_out(cls.fwd.port, FILES_CONF)
            T.lay_out(cls.fwd.port, FILES_CONF)
            # The vehicle: the anchor and the target at 50 Hz, the policy, the bridge.
            V.write("etc/hangar/datasets/toy.dataset",
                    "[Dataset]\nAnchor=toy.anchor\nAnchorRole=input\nFields=depth_m\nLateness=0\nResetAfter=200ms\n\n"
                    "[Input toy.target]\nJoin=asof:0\nFields=depth_m\n")
            V.write("etc/hangar/policies/toy.policy",
                    "[Policy]\nName=toy\nDataset=toy\nArch=%s\nTickBudget=50%%\nIncoming=%s\n" % (arch, cls.arrive))
            for t in ("anchor", "target"):
                V.module(t, "[Module]\nExec=%s --topic toy.%s --rate 50\n[Publish toy.%s]\nType=depth\nRate=50\n"
                         % (BIN["talker"], t, t))
            V.module("policy_toy",
                     "[Module]\nExec=%s %s\n[Subscribe toy.anchor]\n[Subscribe toy.target]\n"
                     "[Publish policy.toy.wrench]\nType=wrench\nRate=50\nInputs=toy.anchor toy.target policy.toy.active\n"
                     "[Publish policy.toy.active]\nType=policy_active\n[Publish policy.toy.load]\nType=policy_load\n"
                     "[Serve load_weights]\nRequest=load_weights_req\nReply=load_weights_rep\n%s"
                     "[Serve revert]\nRequest=empty\nReply=policy_version\n%s"
                     "[Serve runtime_info]\nRequest=empty\nReply=runtime_info\n%s"
                     % (BIN["hangar-policy"], V.path("etc/hangar/policies/toy.policy"), POLICY_ALLOW, POLICY_ALLOW,
                        POLICY_ALLOW))
            serve = "[Bridge]\n[Serve send]\nRequest=bridge_send_req\nReply=bridge_send_rep\n"
            V.module("bridge", "[Module]\nExec=%s %s\n%s" % (BIN["hangar-bridge"], V.path("etc/hangar/bridge.conf"), serve))
            # The topside: the bridge, and the trainer's manifest, which gives
            # the manager the types of the policy's methods (docs/09 "Training").
            # Its bridge changes one byte of the file named below on the way.
            T.module("trainer",
                     "[Module]\nExec=/bin/true\n[Restart]\nPolicy=never\n"
                     "[Call bridge.send]\n"
                     "[Call policy_toy.load_weights]\nRequest=load_weights_req\nReply=load_weights_rep\n"
                     "[Call policy_toy.revert]\nRequest=empty\nReply=policy_version\n"
                     "[Call policy_toy.runtime_info]\nRequest=empty\nReply=runtime_info\n")
            cls.fwd.start()
            V.start()
            os.environ.pop("HANGAR_TOPIC_DIR", None)
            os.environ["HANGAR_BIN"] = BIN["hangar"]
            # The models need the dataset's hash, which the policy tells: asked
            # on the vehicle itself, before the topside is up.
            os.environ["HANGAR_ROOT"] = V.root
            rt = until(cls.runtime, 20, "policy_toy.runtime_info on the vehicle")
            cls.h = {}
            for name, seed, tamper in (("a", 1, False), ("b", 2, False), ("c", 3, False), ("probe", 4, True)):
                cls.h[name] = write_model(os.path.join(cls.models, name + ".hgw"), seed, rt["dataset_hash"], tamper)
            T.module("bridge", "[Module]\nExec=/usr/bin/env HANGAR_BRIDGE_TEST_FILE_CORRUPT=%016x.hgw %s %s\n%s"
                     % (cls.h["c"], BIN["hangar-bridge"], T.path("etc/hangar/bridge.conf"), serve))
            T.start()
            os.environ["HANGAR_ROOT"] = T.root
            until(lambda: V.link_up() and T.link_up(), 20, "the link up with config_match on both sides")
        except BaseException:
            print(V.logs(), T.logs())
            cls.tearDownClass()
            raise

    @staticmethod
    def runtime():
        # The policy answers before its dataset is bound, with a hash of 0:
        # that is no answer yet (a model written from it is refused).
        try:
            rt = learn.runtime_info("toy", timeout=2.0)
        except learn.LearnError:
            return None
        return rt if rt["dataset_hash"] else None

    @classmethod
    def tearDownClass(cls):
        cls.fwd.cut = False
        cls.T.stop()
        cls.V.stop()
        cls.fwd.stop = True
        if cls.fwd.is_alive():
            cls.fwd.join(2)
        cls.fwd.sock.close()
        for k, v in cls.saved_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        if not os.environ.get("HD_KEEP"):
            shutil.rmtree(cls.base, ignore_errors=True)

    def setUp(self):
        os.environ["HANGAR_ROOT"] = self.T.root
        self.fwd.cut = False

    def run(self, result=None):
        before = len(result.failures) + len(result.errors) if result else 0
        r = unittest.TestCase.run(self, result)
        if result and len(result.failures) + len(result.errors) > before:
            print(self.V.logs(), self.T.logs())
        return r

    def active(self, side):
        d = side.latest("policy.toy.active")
        return d["hash"] if d else None

    def arrived(self):
        """(visible names, hidden names, bytes of the hidden ones) in the vehicle's directory."""
        try:
            names = os.listdir(self.arrive)
        except OSError:
            return [], [], 0
        hidden = [n for n in names if n.startswith(".")]
        size = 0
        for n in hidden:
            try:
                size += os.path.getsize(os.path.join(self.arrive, n))
            except OSError:
                pass
        return sorted(n for n in names if not n.startswith(".")), hidden, size

    def model(self, name):
        return os.path.join(self.models, name + ".hgw")

    def test_1_a_deploy_from_the_topside(self):
        self.assertEqual(learn.local_modules(), {"bridge", "trainer"})
        route, why = learn.deploy_route("toy")
        self.assertEqual(route, "bridge", why)
        r = learn.deploy(self.model("a"), "toy")
        self.assertEqual((r["state"], r["via"], r["hash"]), ("done", "bridge", self.h["a"]), r)
        # The policy on the vehicle runs it, and the topside sees that on the mirrored topic.
        self.assertEqual(self.active(self.V), self.h["a"])
        until(lambda: self.active(self.T) == self.h["a"], 5, "policy.toy.active mirrored up")
        self.assertEqual(os.listdir(self.T.path("var/lib/hangar/bridge/outbox")), [])
        self.assertEqual(self.arrived()[1], [])
        # On the vehicle the same question has the other answer: the policy is a module there.
        os.environ["HANGAR_ROOT"] = self.V.root
        self.assertEqual(learn.deploy_route("toy")[0], "local")

    def test_2_the_tether_cut_during_the_transfer(self):
        name = "%016x.hgw" % self.h["b"]
        out = {}

        def go():
            try:
                out["r"] = learn.deploy(self.model("b"), "toy", via="bridge", send_timeout=60)
            except Exception as e:      # reported by the test's thread
                out["e"] = e

        th = threading.Thread(target=go, daemon=True)
        th.start()
        try:
            part = until(lambda: self.arrived()[2] >= 16384 and self.arrived()[2], 15, "16 KB of the file across")
            self.fwd.cut = True
            until(lambda: not self.V.link_up() and not self.T.link_up(), 10, "both links down")
            visible, hidden, held = self.arrived()
            self.assertNotIn(name, visible)             # nothing under its name
            self.assertEqual(len(hidden), 1)
            self.assertLess(held, os.path.getsize(self.model("b")))
            self.assertEqual(self.active(self.V), self.h["a"])   # the model before runs on
            self.assertTrue(th.is_alive())                       # and deploy() waits
        finally:
            self.fwd.cut = False
        th.join(60)
        self.assertFalse(th.is_alive(), "deploy() did not return")
        self.assertNotIn("e", out, out.get("e"))
        self.assertEqual((out["r"]["state"], out["r"]["via"], out["r"]["hash"]), ("done", "bridge", self.h["b"]))
        self.assertEqual(self.active(self.V), self.h["b"])
        self.assertGreaterEqual(part, 16384)
        self.assertEqual(self.arrived()[1], [])

    def test_3_a_file_changed_on_the_way(self):
        before = self.active(self.V)
        name = "%016x.hgw" % self.h["c"]
        with self.assertRaises(learn.LearnError) as c:
            learn.deploy(self.model("c"), "toy")
        self.assertIn("was not sent", str(c.exception))
        self.assertIn("hash", str(c.exception))
        visible, hidden, _ = self.arrived()
        self.assertNotIn(name, visible)
        self.assertEqual(hidden, [])
        self.assertEqual(self.active(self.V), before)
        ev = self.V.latest("bridge.vehicle.files")
        self.assertEqual((ev["name"], ev["state"]), (name, 4), ev)

    def test_4_a_file_the_policy_refuses(self):
        before = self.active(self.V)
        # Whole and as sent, so the bridge delivers it; its probe does not hold.
        r = learn.deploy(self.model("probe"), "toy")
        self.assertEqual((r["state"], r["check"], r["via"]), ("rejected", 6, "bridge"), r)
        # And one that is not a weights file at all.
        junk = os.path.join(self.models, "junk.hgw")
        with open(junk, "wb") as f:
            f.write(bytes((i * 7 + 3) % 256 for i in range(5000)))
        r = learn.deploy(junk, "toy")
        self.assertEqual(r["state"], "rejected", r)
        self.assertEqual(r["check"], 2, r)
        self.assertEqual(self.active(self.V), before)

    def test_5_what_send_refuses(self):
        box = self.T.path("var/lib/hangar/bridge/outbox")
        with self.assertRaises(learn.LearnError) as c:
            learn.send_file(self.model("a"), "nosuch", timeout=10)
        self.assertIn("no such [Files]", str(c.exception))
        with self.assertRaises(learn.LearnError) as c:
            learn.send_file(self.model("a"), "policy_toy", name="..hgw", timeout=10)
        self.assertIn("name:", str(c.exception))
        self.assertEqual(os.listdir(box), [])       # what was not taken is not left there
        # The vehicle has no [Files] going up: its bridge has no outbox, and
        # refuses the destination when asked all the same.
        os.environ["HANGAR_ROOT"] = self.V.root
        with self.assertRaises(learn.LearnError) as c:
            learn.send_file(self.model("a"), "policy_toy", timeout=10)
        self.assertIn("outbox", str(c.exception))
        with self.assertRaises(learn.LearnError) as c:
            learn.call("bridge.send", 5.0, dest="policy_toy", name="a.hgw")
        self.assertIn("no such [Files]", str(c.exception))

    def test_6_the_copy_on_the_policy_s_own_machine(self):
        # deploy() as before the bridge, on the vehicle: the file copied into
        # the directory the policy reads, the call local.
        os.environ["HANGAR_ROOT"] = self.V.root
        r = learn.deploy(self.model("a"), "toy", incoming=self.arrive)
        self.assertEqual((r["state"], r["via"], r["hash"]), ("done", "local", self.h["a"]), r)
        self.assertEqual(self.active(self.V), self.h["a"])


class Route(unittest.TestCase):
    """deploy_route() where nothing runs: it says so rather than choose."""

    def test_nothing_here_says_where_the_policy_is(self):
        import tempfile
        saved = {k: os.environ.get(k) for k in ("HANGAR_ROOT", "HANGAR_TOPIC_DIR")}
        with tempfile.TemporaryDirectory() as d:
            os.environ["HANGAR_ROOT"] = d
            os.environ.pop("HANGAR_TOPIC_DIR", None)
            try:
                self.assertIsNone(learn.local_modules())
                with self.assertRaises(learn.LearnError) as c:
                    learn.deploy_route("toy")
                self.assertIn("via=", str(c.exception))
                self.assertEqual(learn.deploy_route("toy", via="bridge")[0], "bridge")
                self.assertEqual(learn.deploy_route("toy", incoming=d)[0], "local")
                self.assertRaises(learn.LearnError, learn.deploy_route, "toy", via="tether")
                os.makedirs(os.path.join(d, "var/lib/hangar/policy_toy/incoming"))
                self.assertEqual(learn.deploy_route("toy")[0], "local")
            finally:
                for k, v in saved.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v


if __name__ == "__main__":
    unittest.main()
