# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar.dataset against fixtures/chain.dataset and fixtures/dataset.chain.hgr, the
rows modules/recorder/tests/test_rec_dataset.c built from recordings of its toy
chain (40 anchors; t.x referenced every fourth; t.b stale in 11..18 and skipped
at tick 30; t.s published after its stamp every fifth tick, hence `late`).

    python3 -m unittest python/tests/test_dataset.py
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from hangar import dataset, record, schema  # noqa: E402

FIXTURES = os.path.join(HERE, "fixtures")

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


class Spec(unittest.TestCase):
    def test_chain_spec(self):
        text = open(os.path.join(FIXTURES, "chain.dataset")).read()
        s = dataset.DatasetSpec.parse(text, "chain")
        self.assertEqual(s.anchor, "t.c")
        self.assertEqual(s.anchor_role, "label")
        self.assertEqual(s.anchor_fields, ["surge", "heave"])
        self.assertEqual(s.lateness_ns, 0)
        self.assertEqual(s.filter, "mask")
        self.assertEqual(s.missing, "flag")
        self.assertEqual(s.reset_after_ns, 100 * 10 ** 6)
        self.assertEqual(s.rate, ("every", 1))
        self.assertEqual([p.name for p in s.parts], ["t.a", "t.b", "t.x", "t.s", "t.g"])
        self.assertEqual(s.parts[1].window, 3)
        self.assertEqual(s.parts[1].fields, ["gyro"])
        self.assertEqual(s.parts[2].role, "filter")
        self.assertEqual(s.parts[3].join, "asof")
        self.assertEqual(s.parts[3].offset_ns, 0)
        self.assertEqual(s.parts[3].role, "label")
        self.assertEqual(s.wait_ns(), 0)
        self.assertEqual(s.policy_usable(), (True, []))

    def test_docs_example_and_defaults(self):
        text = ("[Dataset]\nAnchor=nav.state\nAnchorRole=input\n"
                "[Input future]\nTopic=nav.state\nJoin=asof:50ms\n"
                "[Input ctl.depth_hold]\nJoin=asof:-20ms\nRole=label\n")
        s = dataset.DatasetSpec.parse(text, "x")
        self.assertEqual(s.lateness_ns, 100 * 10 ** 6)
        self.assertEqual(s.filter, "drop")
        self.assertEqual(s.parts[0].topic, "nav.state")
        self.assertEqual(s.parts[0].offset_ns, 50 * 10 ** 6)
        self.assertEqual(s.parts[1].offset_ns, -20 * 10 ** 6)
        self.assertEqual(s.wait_ns(), 150 * 10 ** 6)
        self.assertEqual(s.policy_usable(), (False, ["future"]))

    def refuse(self, text, what):
        with self.assertRaises(dataset.SpecError) as cm:
            dataset.DatasetSpec.parse(text, "bad")
        self.assertIn(what, str(cm.exception), text)

    def test_refusals(self):
        self.refuse("Anchor=a.b\n", "before any section")
        self.refuse("[Dataset]\n", "Anchor= is required")
        self.refuse("[Dataset]\nAnchor=a.b\nAnchor=a.c\n", "given twice")
        self.refuse("[Dataset]\nAnchor=a.b\nAnchorRole=filter\n", "label or input")
        self.refuse("[Dataset]\nAnchor=a.b\nRate=4\n", "every:N or rate")
        self.refuse("[Dataset]\nAnchor=a.b\nFilter=keep\n", "drop or mask")
        self.refuse("[Dataset]\nAnchor=a.b\nMissing=zero\n", "flag or drop")
        self.refuse("[Dataset]\nAnchor=a.b\nColour=red\n", "unknown key")
        self.refuse("[Dataset]\nAnchor=a.b\n[Output x]\n", "unknown section")
        self.refuse("[Dataset]\nAnchor=a.b\n[Input x]\n", "needs Join=")
        self.refuse("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=nearest\n", "provenance or asof")
        self.refuse("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\nWindow=0\n", "Window")
        self.refuse("[Dataset]\nAnchor=a.b\n[Input x]\nJoin=provenance\n[Input x]\nJoin=provenance\n", "twice")
        self.refuse("[Dataset]\nAnchor=a.b\nthis is not a key\n", "expected [Section] or Key=Value")


class Rows(unittest.TestCase):
    def setUp(self):
        self.ds = dataset.open_dataset(os.path.join(FIXTURES, "dataset.chain.hgr"))

    def tearDown(self):
        self.ds.close()

    def test_header_and_roles(self):
        h = self.ds.header
        self.assertEqual(h.name, "dataset.chain")
        self.assertEqual(h.payload_bytes, 272)
        self.assertEqual(h.record_bytes, 400)
        self.assertEqual(len(self.ds), 40)
        roles = self.ds.roles()
        self.assertEqual(roles["anchor_seq"], "meta")
        self.assertEqual(roles["t.c.surge"], "label")
        self.assertEqual(roles["t.a._seq"], "input")
        self.assertEqual(roles["t.a.depth_m"], "input")
        self.assertEqual(roles["t.b.2.gyro"], "input")
        self.assertEqual(roles["t.x.depth_m"], "filter")
        self.assertEqual(roles["t.s.depth_m"], "label")
        self.assertEqual(self.ds.fields("meta"), list(dataset.HEADER_FIELDS))
        self.assertEqual(self.ds.parts(), ["t.a", "t.b", "t.x", "t.s", "t.g"])
        self.assertIn("t.c.heave", self.ds.fields("label"))
        self.assertNotIn("t.c.heave", self.ds.fields("input"))
        # The row type hashes like any schema: the C side put it in the header.
        self.assertEqual(schema.type_hash(h.schema, generated=True), h.type_hash)
        lay = schema.layout_from_schema(h.schema, generated=True)
        offs = {f.name: f.offset for f in lay.fields}
        self.assertEqual(offs["missing"], 32)
        self.assertEqual(offs["emitted_ns"], 40)
        self.assertEqual(offs["t.c.surge"], 48)
        self.assertEqual(offs["t.a._seq"], 56)
        self.assertEqual(offs["t.b.0.gyro"], 120)
        self.assertEqual(offs["t.g.depth_m"], 256)

    def test_rows(self):
        rows = list(self.ds.rows())
        self.assertEqual([r["anchor_seq"] for r in rows], list(range(1, 41)))
        for n, r in enumerate(rows, 1):
            self.assertEqual(r["t.c.surge"], float(n))
            self.assertEqual(r["t.a._seq"], n)
            self.assertEqual(r["t.a._count"], 1)
            self.assertAlmostEqual(r["t.a.depth_m"], n * 0.01, places=5)
            self.assertEqual(r["emitted_ns"] > r["anchor_stamp_ns"], True)
            # t.x every fourth anchor, missing otherwise (bit 2).
            self.assertEqual(bool(r["missing"] & 4), n % 4 != 0)
            if n % 4 == 0:
                self.assertEqual(r["t.x.depth_m"], float(n))
                self.assertEqual(r["t.x._seq"], n // 4)
            else:
                self.assertEqual(r["t.x._seq"], 0)
            # Where: heave >= 0.2 and t.x present, else masked.
            where = n % 7 >= 2 and n % 4 == 0
            self.assertEqual(bool(r["flags"] & dataset.MASKED), not where)
            # t.b: a window of three gyro vectors, the last the message named.
            bseq = n if n < 30 else n - 1
            self.assertEqual(r["t.b._seq"], bseq)
            self.assertEqual(r["t.b._count"], min(bseq, 3))
            tick = lambda k: float(k if k < 30 else k + 1)  # noqa: E731
            self.assertEqual(r["t.b.2.gyro"][0], tick(bseq))
            if bseq >= 2:
                self.assertEqual(r["t.b.1.gyro"][0], tick(bseq - 1))
            self.assertEqual(bool(r["t.b._flags"] & record.SLOT_STALE), 11 <= n < 19)
            self.assertEqual(bool(r["t.b._flags"] & (1 << 8)), n == 30)   # reused
            # t.s: the newest stamped at or before the anchor; late every fifth.
            self.assertEqual(bool(r["flags"] & dataset.LATE), n % 5 == 0)
            if n >= 5:
                self.assertEqual(r["t.s.depth_m"], float((n // 5) * 5))
            else:
                self.assertTrue(r["missing"] & 8)
            # t.g: present offline (the build has every message).
            self.assertFalse(r["missing"] & 16)
            self.assertEqual(r["t.g.depth_m"], float(n) * 2)
        self.assertTrue(rows[0]["flags"] & dataset.RESET)
        resets = [r["anchor_seq"] for r in rows if r["flags"] & dataset.RESET]
        self.assertTrue(all(n == 1 or 15 <= n <= 19 for n in resets), resets)
        self.assertEqual(self.ds.anchor_seqs(), list(range(1, 41)))
        self.assertEqual(len(self.ds.flags()), 40)
        self.assertEqual(self.ds.missing()[0] & 4, 4)

    def test_a_plain_recording_is_not_a_dataset(self):
        with self.assertRaises(record.RecordError):
            dataset.open_dataset(os.path.join(FIXTURES, "depth.hgr"))

    @unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
    def test_numpy(self):
        a = self.ds.array()
        self.assertEqual(a.shape, (40,))
        self.assertEqual(list(a["anchor_seq"]), list(range(1, 41)))
        self.assertEqual(a["t.b.2.gyro"].shape, (40, 3))
        self.assertEqual(a["t.c.surge"][39], 40.0)


if __name__ == "__main__":
    unittest.main()
