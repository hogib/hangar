# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar.record against fixtures/depth.hgr, written by modules/recorder/tests/test_rec_hgr.c
(ten depth messages, a gap of two before the third, a decimation before the sixth).

    python3 -m unittest python/tests/test_record.py
"""

import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from hangar import record  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "depth.hgr")

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


class Header(unittest.TestCase):
    def test_header(self):
        rec = record.open_recording(FIXTURE)
        h = rec.header
        self.assertEqual(h.magic, b"HGREC001")
        self.assertEqual(h.version, 1)
        self.assertEqual(h.name, "rt.depth")
        self.assertEqual(h.payload_bytes, 16)
        self.assertEqual(h.record_bytes, 144)
        self.assertEqual(h.slot_count, 16)
        self.assertEqual(h.rate_mhz, 50000)
        self.assertEqual(h.type_hash, 0xF397A321CD6643A1)   # msg/depth.msg
        self.assertEqual(h.policy_name, "full")
        self.assertEqual(h.policy_text, "full")
        self.assertEqual(h.policy_n, 1)
        self.assertEqual(h.session, "20260101-000000-test")
        self.assertEqual(h.epoch, 0)
        self.assertEqual(h.origin_boot, b"\0" * 16)
        self.assertFalse(h.meta)
        self.assertIn("f32 depth_m", h.schema)
        self.assertEqual([f.name for f in rec.layout.fields], ["depth_m", "temp_c", "pressure_mbar"])
        rec.close()

    def test_not_a_recording(self):
        with tempfile.NamedTemporaryFile(suffix=".hgr", delete=False) as f:
            f.write(b"x" * 5000)
        try:
            with self.assertRaises(record.RecordError):
                record.open_recording(f.name)
        finally:
            os.unlink(f.name)
        with tempfile.NamedTemporaryFile(suffix=".hgr", delete=False) as f:
            f.write(b"x" * 100)
        try:
            with self.assertRaises(record.RecordError):
                record.open_recording(f.name)
        finally:
            os.unlink(f.name)


class Records(unittest.TestCase):
    def setUp(self):
        self.rec = record.open_recording(FIXTURE)

    def tearDown(self):
        self.rec.close()

    def test_count_and_seqs(self):
        self.assertEqual(len(self.rec), 10)
        self.assertEqual(self.rec.seqs(), list(range(1, 11)))
        for i, r in enumerate(self.rec):
            self.assertEqual(r.seq, i + 1)
            self.assertEqual(r.stamp_ns, 1000000 * (i + 1))
            self.assertEqual(r.n_inputs, 0)
            self.assertTrue(r.publish_ns > 0)
        self.assertEqual(self.rec[-1].seq, 10)
        with self.assertRaises(IndexError):
            self.rec[10]

    def test_gaps_and_decimation(self):
        r = self.rec[2]
        self.assertTrue(r.flags & record.REC_GAP)
        self.assertEqual(r.gap, 2)
        r = self.rec[5]
        self.assertFalse(r.flags & record.REC_GAP)
        self.assertTrue(r.flags & record.SLOT_DECIMATED)
        self.assertEqual(r.gap, 1)
        self.assertEqual(self.rec.gaps(), [(2, 2)])
        self.assertEqual(self.rec[0].flags, 0)

    def test_payload(self):
        p = self.rec.payload(3)
        self.assertEqual(p["depth_m"], 4.0)
        self.assertEqual(p["temp_c"], 20.0)
        self.assertEqual(p["pressure_mbar"], 1004.0)
        self.assertEqual(self.rec.field("depth_m"), [float(i) for i in range(1, 11)])
        self.assertEqual(len(self.rec.payload_bytes_at(0)), 16)
        self.assertEqual(struct.unpack_from("<f", self.rec.payload_bytes_at(9))[0], 10.0)
        with self.assertRaises(KeyError):
            self.rec.field("salinity")

    def test_searches(self):
        self.assertEqual(self.rec.find_seq(1), 0)
        self.assertEqual(self.rec.find_seq(7), 6)
        self.assertEqual(self.rec.find_seq(11), -1)
        self.assertEqual(self.rec.find_seq(0), -1)
        self.assertEqual(self.rec.find_stamp(999999), -1)
        self.assertEqual(self.rec.find_stamp(1000000), 0)
        self.assertEqual(self.rec.find_stamp(3500000), 2)
        self.assertEqual(self.rec.find_stamp(99000000), 9)

    def test_cut_off_file_ignores_the_partial_record(self):
        data = open(FIXTURE, "rb").read()
        with tempfile.NamedTemporaryFile(suffix=".hgr", delete=False) as f:
            f.write(data[:4096 + 4 * 144 + 50])
        try:
            rec = record.open_recording(f.name)
            self.assertEqual(len(rec), 4)
            self.assertEqual(rec.find_seq(5), -1)
            rec.close()
        finally:
            os.unlink(f.name)

    @unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
    def test_numpy_array(self):
        a = self.rec.array()
        self.assertEqual(a.shape, (10,))
        self.assertEqual(a.dtype.itemsize, 144)
        self.assertEqual(list(a["seq"]), list(range(1, 11)))
        self.assertEqual(list(a["depth_m"]), [float(i) for i in range(1, 11)])
        self.assertEqual(a["gap"][2], 2)
        self.assertEqual(a["inputs"].shape, (10, 4))
        self.assertEqual(a["inputs"]["seq"][0][0], 0)


if __name__ == "__main__":
    unittest.main()
