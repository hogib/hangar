# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar.learn without a vehicle (docs/09 "Tests"): stateful chunks (a
chunk ends at a reset, a lost row and a change of boot, the next starts
fresh; a slot started inside a run carries no loss on its first chunk;
masked rows and padding carry none), the replay buffer's span and its
draws, the file stream over the pairer's fixture with the C writer's row
type hash, probes from a run's start, lineage per boot. carry() and export
need PyTorch: skipped without it. The live reader and deploy run against
a real policy in modules/policy's suite."""

import os
import random
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False
try:
    import torch  # noqa: F401
    HAVE_TORCH = True
except ImportError:
    HAVE_TORCH = False

from hangar import learn  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
B0, B1 = b"\x01" * 16, b"\x02" * 16
MS = 10 ** 6


def row(seq, flags=0, boot=B0, lost=False, stamp=None):
    return learn._Row(seq, stamp if stamp is not None else seq * 20 * MS, boot, flags, [float(seq)], [float(-seq)], lost)


def replay(rows, span=None):
    rp = learn.Replay(span, rng=random.Random(1))
    for r in rows:
        rp.add(r)
    return rp


@unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
class Chunks(unittest.TestCase):
    def chunker(self, rows, T, B=1):
        return learn.Chunker(replay(rows), B, T, 1, 1)

    def test_run_from_reset(self):
        rows = [row(1, learn.RESET)] + [row(i) for i in range(2, 11)]
        ch = self.chunker(rows, 4)
        ch.slots[0].pos, ch.slots[0].fresh, ch.slots[0].warm = 0, True, False
        c = ch.next()
        self.assertTrue(c.fresh[0])
        self.assertEqual(list(c.inputs[0, :, 0]), [1, 2, 3, 4])
        self.assertEqual(list(c.labels[0, :, 0]), [-1, -2, -3, -4])
        self.assertEqual(list(c.mask[0]), [1, 1, 1, 1])
        c = ch.next()  # carried on: not fresh
        self.assertFalse(c.fresh[0])
        self.assertEqual(list(c.inputs[0, :, 0]), [5, 6, 7, 8])

    def test_chunk_ends_at_reset_lost_and_boot(self):
        for second in (row(4, learn.RESET), row(4, lost=True), row(4, boot=B1)):
            rows = [row(1, learn.RESET), row(2), row(3), second, row(5), row(6)]
            ch = self.chunker(rows, 5)
            ch.slots[0].pos, ch.slots[0].fresh, ch.slots[0].warm = 0, True, False
            c = ch.next()
            self.assertEqual(list(c.inputs[0, :, 0]), [1, 2, 3, 0, 0])
            self.assertEqual(list(c.mask[0]), [1, 1, 1, 0, 0])  # padding carries no loss
            self.assertIsNone(ch.slots[0].pos)               # drawn again, fresh
            c = ch.next()
            self.assertTrue(c.fresh[0])

    def test_reset_at_the_first_row_of_the_next_chunk(self):
        # A chunk that fills exactly, the next row a reset (a lost row, a new
        # boot): the slot went on there with fresh False, and carry() kept
        # the state of the run before across the reset.
        for second in (row(5, learn.RESET), row(5, lost=True), row(5, boot=B1)):
            rows = [row(1, learn.RESET), row(2), row(3), row(4), second] + \
                [row(i, boot=second.boot) for i in range(6, 13)]
            ch = self.chunker(rows, 4)
            ch.slots[0].pos, ch.slots[0].fresh, ch.slots[0].warm = 0, True, False
            c = ch.next()
            self.assertEqual(list(c.inputs[0, :, 0]), [1, 2, 3, 4])
            c = ch.next()
            self.assertEqual(list(c.inputs[0, :, 0]), [5, 6, 7, 8])
            self.assertTrue(c.fresh[0])                      # a run starts here: zero state
            self.assertEqual(list(c.mask[0]), [1, 1, 1, 1])  # from its start: no warm-up
            c = ch.next()
            self.assertEqual(list(c.inputs[0, :, 0]), [9, 10, 11, 12])
            self.assertFalse(c.fresh[0])

    def test_inside_a_run_warms_up(self):
        rows = [row(1, learn.RESET)] + [row(i) for i in range(2, 21)]
        ch = self.chunker(rows, 3)
        ch.slots[0].pos, ch.slots[0].fresh, ch.slots[0].warm = 5, True, True
        c = ch.next()
        self.assertEqual(list(c.mask[0]), [0, 0, 0])
        self.assertTrue(c.fresh[0])
        c = ch.next()
        self.assertEqual(list(c.mask[0]), [1, 1, 1])
        self.assertFalse(c.fresh[0])

    def test_draw_sets_warm(self):
        rows = [row(1, learn.RESET)] + [row(i) for i in range(2, 200)]
        ch = self.chunker(rows, 3, B=16)
        for _ in range(5):
            ch.next()
        for s in ch.slots:
            if s.pos is not None:
                self.assertFalse(s.warm and not s.fresh)

    def test_masked_rows_no_loss(self):
        rows = [row(1, learn.RESET), row(2, learn.MASKED), row(3)]
        ch = self.chunker(rows, 3)
        ch.slots[0].pos, ch.slots[0].fresh, ch.slots[0].warm = 0, True, False
        self.assertEqual(list(ch.next().mask[0]), [1, 0, 1])

    def test_newest_ends_the_chunk(self):
        rows = [row(1, learn.RESET), row(2)]
        ch = self.chunker(rows, 4)
        ch.slots[0].pos, ch.slots[0].fresh, ch.slots[0].warm = 0, True, False
        c = ch.next()
        self.assertEqual(list(c.mask[0]), [1, 1, 0, 0])
        self.assertIsNone(ch.slots[0].pos)


class ReplayBuffer(unittest.TestCase):
    def test_span_drops_old_rows(self):
        rp = replay([row(i) for i in range(1, 101)], span="1s")  # 20 ms apart
        self.assertEqual(len(rp), 51)
        self.assertEqual(rp.base, 49)
        self.assertEqual(rp.at(rp.base).seq, 50)
        self.assertEqual(rp.total, 100)

    def test_half_the_draws_recent(self):
        rows = [row(i, stamp=i * MS * 100) for i in range(1, 601)]  # 60 s
        rp = learn.Replay("10min", recent="10s", rng=random.Random(3))
        for r in rows:
            rp.add(r)
        draws = [rp.at(rp.draw()).stamp for _ in range(4000)]
        recent = sum(1 for s in draws if rows[-1].stamp - s <= 10 * 10 ** 9) / len(draws)
        # half from the last 10 s, plus the uniform half's share of them (1/6)
        self.assertAlmostEqual(recent, 0.5 + 0.5 * 101 / 600, delta=0.04)

    def test_span_across_a_reboot(self):
        # The anchor's stamps are the vehicle's CLOCK_MONOTONIC: after a
        # reboot they start again near zero. newest - oldest was negative,
        # so nothing was dropped until the new boot's uptime passed the old
        # boot's, and the buffer grew without bound.
        rp = learn.Replay("1s", rng=random.Random(1))
        for i in range(1, 101):                       # an hour up, 20 ms apart
            rp.add(row(i, stamp=3600 * 10 ** 9 + i * 20 * MS))
        self.assertEqual(len(rp), 51)
        for i in range(1, 31):                        # rebooted: 30 rows of the new boot
            rp.add(row(i, boot=B1, stamp=5 * 10 ** 9 + i * 20 * MS))
        self.assertEqual(len(rp), 52)                 # the span goes on over the change of boot
        self.assertEqual([r.boot for r in rp.rows].count(B1), 30)
        for i in range(31, 101):
            rp.add(row(i, boot=B1, stamp=5 * 10 ** 9 + i * 20 * MS))
        self.assertEqual(len(rp), 51)
        self.assertEqual({r.boot for r in rp.rows}, {B1})   # the boot before is gone after a span of the new one
        self.assertEqual(rp.at(rp.base).seq, 50)
        self.assertEqual(rp.total, 200)

    def test_recent_draws_across_a_reboot(self):
        # "The last `recent`" was searched for by stamp over rows whose
        # stamps no longer rise: with the old boot's stamps above the new
        # one's, the search took every row for recent or none.
        for old_stamp in (3600 * 10 ** 9, 0):          # the boot before up longer, or shorter
            rp = learn.Replay("10min", recent="10s", rng=random.Random(3))
            for i in range(1, 501):                   # 50 s of one boot
                rp.add(row(i, stamp=old_stamp + i * 100 * MS))
            for i in range(1, 101):                   # 10 s of the next
                rp.add(row(i, boot=B1, stamp=20 * 10 ** 9 + i * 100 * MS))
            draws = [rp.at(rp.draw()) for _ in range(4000)]
            recent = sum(1 for r in draws if r.boot == B1) / len(draws)
            self.assertAlmostEqual(recent, 0.5 + 0.5 * 100 / 600, delta=0.04)

    def test_boot_change_starts_a_run(self):
        rp = replay([row(1, learn.RESET), row(2), row(3, boot=B1)])
        self.assertTrue(rp.at(2).starts_run)

    def test_durations(self):
        self.assertEqual(learn._duration_ns("10min"), 600 * 10 ** 9)
        self.assertEqual(learn._duration_ns("500ms"), 500 * MS)
        self.assertEqual(learn._duration_ns("2s"), 2 * 10 ** 9)
        self.assertRaises(ValueError, learn._duration_ns, "ten")


@unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
class FromFile(unittest.TestCase):
    PATH = os.path.join(FIXTURES, "dataset.chain.hgr")

    def test_fields_and_hash(self):
        s = learn.open_dataset(self.PATH, batch=4, seq_len=5)
        self.assertEqual(s.input_fields, ["t.a.depth_m", "t.a.temp_c", "t.a.pressure_mbar",
                                          "t.g.depth_m", "t.g.temp_c", "t.g.pressure_mbar"])
        self.assertEqual(s.label_fields, ["t.c.surge", "t.c.heave", "t.s.depth_m"])
        from hangar import dataset
        ds = dataset.open_dataset(self.PATH)
        self.assertEqual(s.dataset_hash, ds.header.type_hash)  # the C writer's row type hash
        self.assertEqual(len(s.replay), ds.count)
        ds.close()

    def test_chunks_and_rows(self):
        s = learn.open_dataset(self.PATH, batch=4, seq_len=5)
        c = s.next_chunk()
        self.assertEqual(c.inputs.shape, (4, 5, 6))
        self.assertEqual(c.labels.shape, (4, 5, 3))
        self.assertEqual(c.mask.shape, (4, 5))
        flat = learn.open_dataset(self.PATH, batch=8)
        c = flat.next_chunk()
        self.assertEqual(c.inputs.shape, (8, 6))
        self.assertTrue(c.fresh.all())

    def test_inputs_named(self):
        s = learn.open_dataset(self.PATH, batch=2, seq_len=3, inputs=["t.a.depth_m"], labels=["t.c.heave"])
        self.assertEqual(s.next_chunk().inputs.shape, (2, 3, 1))
        self.assertRaises(learn.LearnError, learn.open_dataset, self.PATH, inputs=["t.nope"])

    def test_array_element(self):
        s = learn.open_dataset(self.PATH, batch=2, seq_len=3, inputs=["t.b.0.gyro[2]", "t.a.depth_m"])
        from hangar import dataset
        ds = dataset.open_dataset(self.PATH)
        self.assertEqual(s.replay.at(0).x[0], ds.row(0)["t.b.0.gyro"][2])
        ds.close()
        self.assertRaises(learn.LearnError, learn.open_dataset, self.PATH, inputs=["t.b.0.gyro"])
        self.assertRaises(learn.LearnError, learn.open_dataset, self.PATH, inputs=["t.b.0.gyro[9]"])

    def test_no_loss_from_a_missing_part(self):
        # Row 3 of the fixture passed Where (flags 0) and its label part t.s
        # is missing: the label is the zeros of docs/06 "The row", nothing
        # anything published. It had loss weight 1.
        from hangar import dataset
        ds = dataset.open_dataset(self.PATH)
        r3 = ds.row(3)
        self.assertEqual((r3["flags"], r3["missing"], r3["t.s._count"]), (0, 1 << ds.parts().index("t.s"), 0))
        arr = ds.array()
        s = learn.open_dataset(self.PATH, batch=4, seq_len=5)
        self.assertEqual(s.replay.at(3).y[2], 0.0)
        self.assertTrue(s.replay.at(3).partial)
        self.assertFalse(s.replay.at(3).loss)
        ch = s._chunker
        ch.slots[0].pos, ch.slots[0].fresh, ch.slots[0].warm = 0, True, False
        self.assertEqual(float(ch.next().mask[0, 3]), 0.0)
        # A stream that does not read t.s takes the row.
        t = learn.open_dataset(self.PATH, batch=4, inputs=["t.a.depth_m"], labels=["t.c.heave"])
        self.assertTrue(t.replay.at(3).loss)
        self.assertEqual([r.partial for r in t.replay.rows], [False] * len(arr))
        # with_loss(): the same rule over the rows as an array.
        for st in (s, t):
            self.assertEqual(list(learn.with_loss(arr, st.input_fields + st.label_fields)),
                             [r.loss for r in st.replay.rows])
        self.assertEqual(int(learn.with_loss(arr, s.input_fields + s.label_fields).sum()),
                         int(((arr["flags"] & learn.MASKED == 0) & (arr["missing"] == 0)).sum()))
        # In a batch of single rows too.
        flat = learn.open_dataset(self.PATH, batch=64)
        flat.replay.draw = lambda: 3
        self.assertEqual(float(flat.next_chunk().mask.sum()), 0.0)
        ds.close()

    def test_no_loss_from_a_window_not_yet_full(self):
        # t.b is a window of 3, message 2 the newest; the fixture's first
        # rows hold 1, 2 and 3 messages of it. Message 0 is zeros until the
        # count is 3, message 1 until it is 2.
        from hangar import dataset
        ds = dataset.open_dataset(self.PATH)
        arr = ds.array()
        self.assertEqual(list(arr["t.b._count"][:3]), [1, 2, 3])
        ds.close()
        for field, partial in (("t.b.0.gyro[2]", [True, True, False]), ("t.b.1.gyro[0]", [True, False, False]),
                               ("t.b.2.gyro[1]", [False, False, False]), ("t.b._count", [False, False, False])):
            s = learn.open_dataset(self.PATH, batch=2, inputs=[field], labels=["t.c.heave"])
            self.assertEqual([s.replay.at(i).partial for i in range(3)], partial, field)
            self.assertEqual(list(learn.joined(arr, [field])[:3]), [not p for p in partial], field)

    def test_probes_from_a_run_start(self):
        s = learn.open_dataset(self.PATH, batch=2, seq_len=3)
        p = s.probes(4)
        self.assertEqual(len(p), 1)
        self.assertEqual(len(p[0]), 4)
        self.assertEqual(len(p[0][0]), 6)

    def test_lineage(self):
        s = learn.open_dataset(self.PATH, batch=2, seq_len=3)
        lin = s.lineage()
        self.assertEqual(len(lin), 1)
        self.assertEqual(lin[0].rows, len(s.replay))
        self.assertEqual(lin[0].first_seq, s.replay.at(0).seq)


@unittest.skipUnless(HAVE_TORCH, "PyTorch is not installed")
class WithTorch(unittest.TestCase):
    def test_carry(self):
        h = torch.ones(1, 3, 2, requires_grad=True) * 2
        c = (h, h + 1)
        out = learn.carry(c, numpy.array([False, True, False]))
        self.assertFalse(out[0].requires_grad)
        self.assertEqual(out[0][0, 1].abs().sum().item(), 0)
        self.assertEqual(out[1][0, 0, 0].item(), 3)
        self.assertIsNone(learn.carry(None, [True]))

    def test_export_checks_the_runtime(self):
        import tempfile
        s = learn.open_dataset(os.path.join(FIXTURES, "dataset.chain.hgr"), batch=2, seq_len=3)
        model = torch.nn.Sequential(torch.nn.Linear(6, 1))
        path = os.path.join(tempfile.mkdtemp(), "m.hgw")
        learn.export(model, path, s, ["heave"], runtime={"dataset_hash": s.dataset_hash, "ops": [("dense", 1)]})
        from hangar.nn import hgw
        f = hgw.parse(open(path, "rb").read())
        self.assertEqual(f.header.dataset_hash, s.dataset_hash)
        self.assertEqual(f.header.lineage[0].rows, len(s.replay))
        self.assertRaises(learn.LearnError, learn.export, model, path, s, ["heave"],
                          runtime={"dataset_hash": s.dataset_hash ^ 1})

    def test_export_writes_the_lineage_it_is_given(self):
        # steps, parent_hash and the session were 0, 0 and "live" in every
        # file deploy() wrote: export had the first two as arguments that
        # deploy never passed, and no way to name the session.
        import tempfile
        from hangar.nn import hgw
        s = learn.open_dataset(os.path.join(FIXTURES, "dataset.chain.hgr"), batch=2, seq_len=3)
        model = torch.nn.Sequential(torch.nn.Linear(6, 1))
        path = os.path.join(tempfile.mkdtemp(), "m.hgw")
        rt = {"dataset_hash": s.dataset_hash, "ops": [("dense", 1)]}
        learn.export(model, path, s, ["heave"], runtime=rt)
        f = hgw.parse(open(path, "rb").read())
        self.assertEqual((f.header.steps, f.header.parent_hash, f.header.lineage[0].session), (0, 0, "live"))
        learn.export(model, path, s, ["heave"], runtime=rt, steps=1234, parent_hash=0x1122334455667788,
                     session="20261120-101500-rov")
        f = hgw.parse(open(path, "rb").read())
        self.assertEqual(f.header.steps, 1234)
        self.assertEqual(f.header.parent_hash, 0x1122334455667788)
        self.assertEqual(f.header.lineage[0].session, "20261120-101500-rov")
        self.assertEqual(f.header.lineage[0].rows, len(s.replay))

    def test_deploy_fills_the_lineage(self):
        # deploy() with a model: what it hands to export. The policy's side
        # (runtime_info, the incoming directory, load_weights) is stood in
        # for; the file it would send is read back.
        import tempfile
        from hangar.nn import hgw
        s = learn.open_dataset(os.path.join(FIXTURES, "dataset.chain.hgr"), batch=2, seq_len=3)
        model = torch.nn.Sequential(torch.nn.Linear(6, 1))
        inc = tempfile.mkdtemp()
        topics = tempfile.mkdtemp()

        class Sent(Exception):
            pass

        def no_topic(*a, **k):
            raise Sent()

        saved = learn.runtime_info, learn._topic.open_topic
        learn.runtime_info = lambda policy: {"dataset_hash": s.dataset_hash, "ops": [("dense", 1)]}
        try:
            hashes = []
            for steps in (100, 250):
                learn._topic.open_topic = saved[1]  # recorder.session: absent here, so "live"
                real = saved[1]

                def open_topic(name, directory=None, start="newest", timeout=0.0):
                    if name.startswith("policy."):
                        raise Sent()  # the file is in the incoming directory by now
                    return real(name, directory, start, timeout)

                learn._topic.open_topic = open_topic
                self.assertRaises(Sent, learn.deploy, model, "toy", stream=s, outputs=["heave"], incoming=inc,
                                  directory=topics, steps=steps)
                newest = max((os.path.join(inc, n) for n in os.listdir(inc)), key=os.path.getmtime)
                f = hgw.parse(open(newest, "rb").read())
                hashes.append((f.header.steps, f.header.parent_hash, f.header.lineage[0].session,
                               int(os.path.basename(newest)[:16], 16)))
                with torch.no_grad():
                    model[0].weight += 0.5  # trained on: other weights, another hash
        finally:
            learn.runtime_info, learn._topic.open_topic = saved
        (steps1, parent1, session1, hash1), (steps2, parent2, session2, hash2) = hashes
        self.assertEqual((steps1, parent1, session1), (100, 0, "live"))  # the first deploy has no parent
        self.assertEqual(steps2, 250)
        self.assertEqual(parent2, hash1)  # the second started from the first
        self.assertNotEqual(hash2, hash1)
        self.assertEqual(learn.recording_session(topics), "live")


EXAMPLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "examples", "depth_ff.py")


class TopicDir(unittest.TestCase):
    def test_root(self):
        from hangar import topic
        saved = {k: os.environ.pop(k, None) for k in ("HANGAR_TOPIC_DIR", "HANGAR_ROOT")}
        try:
            self.assertEqual(topic.topic_dir(), "/dev/shm/hangar")
            os.environ["HANGAR_ROOT"] = "/dev/shm/r"
            self.assertEqual(topic.topic_dir(), "/dev/shm/r/dev/shm/hangar")
            self.assertEqual(learn.state_dir("x"), "/dev/shm/r/var/lib/hangar/policy_x")
            os.environ["HANGAR_TOPIC_DIR"] = "/t"
            self.assertEqual(topic.topic_dir(), "/t")
        finally:
            for k, v in saved.items():
                os.environ.pop(k, None)
                if v is not None:
                    os.environ[k] = v


class Example(unittest.TestCase):
    @unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
    def test_data_path(self):
        import subprocess
        p = subprocess.run([sys.executable, EXAMPLE, "--synthetic", "--check"], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=30)
        self.assertEqual(p.returncode, 0, p.stdout.decode())
        self.assertIn(b"15000 rows in the buffer, 0 lost", p.stdout)

    @unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
    def test_residual_label(self):
        sys.path.insert(0, os.path.dirname(EXAMPLE))
        import depth_ff
        # The heavy frame's four verticals at 1380 us: heave 0.3 up, as
        # ArduSub mixes it (modules/thrusters checks the mixing against lib/sim).
        L = numpy.array([[0.1, 1380.0, 1380.0, 1380.0, 1380.0]])
        self.assertAlmostEqual(float(depth_ff.residual(L)[0]), 0.2, places=6)
        X = numpy.array([[3.0, 0.0, 0.0, 0.0, -1.0]])
        self.assertAlmostEqual(float(depth_ff.trim(L, X, 4)[0]), 0.3 - depth_ff.FF, places=6)

    @unittest.skipUnless(HAVE_TORCH, "PyTorch is not installed")
    def test_trains_and_exports(self):
        import subprocess
        import tempfile
        out = os.path.join(tempfile.mkdtemp(), "depth_ff.hgw")
        p = subprocess.run([sys.executable, EXAMPLE, "--synthetic", "--steps", "150", "--out", out],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout.decode())
        self.assertTrue(os.path.getsize(out) > 4096)

    @unittest.skipUnless(HAVE_TORCH, "PyTorch is not installed")
    def test_bounded_model(self):
        # --bound: a tanh before the last affine, another architecture, and an
        # output the reference keeps within the bound whatever the input.
        import subprocess
        import tempfile
        from hangar.nn import hgw, reference
        d = tempfile.mkdtemp()
        graphs = {}
        for name, extra in (("plain", []), ("bounded", ["--bound", "0.15"])):
            out = os.path.join(d, name + ".hgw")
            p = subprocess.run([sys.executable, EXAMPLE, "--synthetic", "--steps", "20", "--out", out] + extra,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            self.assertEqual(p.returncode, 0, p.stdout.decode())
            graphs[name] = hgw.read(out).graph
        self.assertEqual([o.type for o in graphs["plain"].ops], ["affine", "lstm", "dense", "affine"])
        self.assertEqual([o.type for o in graphs["bounded"].ops], ["affine", "lstm", "dense", "tanh", "affine"])
        self.assertNotEqual(hgw.arch_hash(graphs["plain"]), hgw.arch_hash(graphs["bounded"]))
        m = reference.Model(graphs["bounded"])
        m.reset()
        for x in ([1e3, -1e3, 50.0, -50.0, 1e3], [-1e3, 1e3, -50.0, 50.0, -1e3], [0.0] * 5):
            for _ in range(20):
                y = m.step([x])[0][0]
                self.assertLessEqual(abs(y), 0.15 + 1e-6)


if __name__ == "__main__":
    unittest.main()
