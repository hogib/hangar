# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar.topic against topic files written here, byte for byte as lib/hg
lays them out (the offsets are topic.py's own, which the live test of
modules/policy checks against the C writer): where a reader opened "oldest"
starts, what it reports as a gap, and what it does when the writer replaces
the file. The rules are hg_next's (lib/hg/hg_topic.c): a file holds the
messages after its first_seq; a reader starts at first_seq, or at
write_seq - slots + 1 when the ring has wrapped; the seqs between the last
message read and a new file's first_seq are in no file, and are a gap."""

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from hangar import topic  # noqa: E402

SLOTS, SLOT_HEADER, PAYLOAD, STRIDE, SLOTS_OFFSET = 16, 128, 8, 136, 4096
SCHEMA = b"u64 v\n"


def write_topic(path, first_seq, write_seq, flags=None, in_place=False):
    """A topic file holding the messages first_seq + 1 .. write_seq (those
    still in a ring of 16), message n with payload v = n. Written beside
    and renamed over `path`, as a writer replaces its file: a new inode;
    or in place, as a writer publishes."""
    buf = bytearray(SLOTS_OFFSET + SLOTS * STRIDE)
    buf[0:8] = topic.MAGIC
    struct.pack_into("<Q", buf, topic._OFF_GENERATION, 1)
    struct.pack_into("<II", buf, topic._OFF_SLOT_COUNT, SLOTS, SLOT_HEADER)
    struct.pack_into("<I", buf, topic._OFF_PAYLOAD, PAYLOAD)
    struct.pack_into("<I", buf, topic._OFF_STRIDE, STRIDE)
    struct.pack_into("<Q", buf, topic._OFF_SLOTS, SLOTS_OFFSET)
    struct.pack_into("<I", buf, topic._OFF_TOPIC_ID, 7)
    struct.pack_into("<I", buf, topic._OFF_SCHEMA_BYTES, len(SCHEMA))
    struct.pack_into("<Q", buf, topic._OFF_FIRST_SEQ, first_seq)
    struct.pack_into("<Q", buf, topic._OFF_WRITE_SEQ, write_seq)
    buf[topic._OFF_SCHEMA:topic._OFF_SCHEMA + len(SCHEMA)] = SCHEMA
    for n in range(max(first_seq + 1, write_seq - SLOTS + 1), write_seq + 1):
        off = SLOTS_OFFSET + ((n - 1) & (SLOTS - 1)) * STRIDE
        struct.pack_into("<QQQ", buf, off, 2 * n, n * 1000, n * 1000 + 1)
        struct.pack_into("<II", buf, off + topic._SL_N_INPUTS, 0, (flags or {}).get(n, 0))
        struct.pack_into("<Q", buf, off + SLOT_HEADER, n)
    if in_place:
        with open(path, "r+b") as f:
            f.write(buf)
        return
    tmp = path + ".new"
    with open(tmp, "wb") as f:
        f.write(buf)
    os.rename(tmp, path)


class Oldest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "t.x")

    def read(self, t):
        return [(m.seq, m.gap) for m in t.read()]

    def test_starts_at_first_seq_without_a_gap(self):
        # A writer that restarted at seq 3 and has published 4, 5 and 6: seq
        # 3 is in no file and was never this reader's to get. The reader
        # started one before first_seq, did not find 3, and gave 4 a gap of 1.
        write_topic(self.path, 3, 6)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(t.layout.fields[0].name, "v")
            msgs = t.read()
            self.assertEqual([(m.seq, m.gap) for m in msgs], [(4, 0), (5, 0), (6, 0)])
            self.assertEqual([t.decode(m)["v"] for m in msgs], [4, 5, 6])
            self.assertEqual(self.read(t), [])

    def test_a_new_topic_from_its_first_message(self):
        write_topic(self.path, 0, 3)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(self.read(t), [(1, 0), (2, 0), (3, 0)])

    def test_a_ring_that_has_wrapped_from_the_oldest_safely_in_it(self):
        # hg_topic.c: "the oldest still safely in the ring (W-K+2 onward)".
        write_topic(self.path, 0, 40)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(self.read(t), [(n, 0) for n in range(26, 41)])
        write_topic(self.path, 30, 60)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(self.read(t), [(n, 0) for n in range(46, 61)])

    def test_newest_reads_what_comes_after(self):
        write_topic(self.path, 3, 6)
        with topic.Topic("t.x", self.dir) as t:
            self.assertEqual(self.read(t), [])

    def test_a_replaced_file_that_goes_on_where_the_old_one_ended(self):
        # The writer restarted and continues its seqs (hg_pub_open): the new
        # file's first_seq is the last seq of the old one. Nothing was lost;
        # the first message of the new file carried a gap of 1, so
        # learn.LiveStream counted a row lost and ended a run at every
        # restart of the pairer.
        write_topic(self.path, 0, 6)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(self.read(t), [(n, 0) for n in range(1, 7)])
            write_topic(self.path, 6, 8)
            self.assertEqual(self.read(t), [(7, 0), (8, 0)])
            self.assertEqual(self.read(t), [])

    def test_a_replaced_file_with_messages_in_neither(self):
        # Read up to 6; the new file starts after 9: 7, 8 and 9 are in no
        # file. A gap of 3 on the first message there is (hg_next: first_seq
        # less the last read). It was 1 whatever the distance.
        write_topic(self.path, 0, 6)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(len(t.read()), 6)
            write_topic(self.path, 9, 9)
            self.assertEqual(self.read(t), [])          # nothing yet: the gap waits for a message
            write_topic(self.path, 9, 11)
            self.assertEqual(self.read(t), [(10, 3), (11, 0)])

    def test_a_replaced_file_numbered_from_the_start(self):
        # A topic made anew (its seqs below the last one read): every
        # message of the new file, no gap.
        write_topic(self.path, 0, 6)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(len(t.read()), 6)
            write_topic(self.path, 0, 2)
            self.assertEqual(self.read(t), [(1, 0), (2, 0)])

    def test_a_reader_lapped_reports_the_gap(self):
        write_topic(self.path, 0, 6)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            self.assertEqual(len(t.read()), 6)
            write_topic(self.path, 0, 40, in_place=True)   # 34 more: 7..25 gone from a ring of 16
            self.assertEqual(self.read(t), [(26, 19)] + [(n, 0) for n in range(27, 41)])

    def test_decimated_messages_carry_no_gap(self):
        write_topic(self.path, 0, 6)
        with topic.Topic("t.x", self.dir, start="oldest") as t:
            t.read()
            write_topic(self.path, 9, 11, flags={10: topic.SLOT_DECIMATED})
            self.assertEqual(self.read(t), [(10, 0), (11, 0)])


if __name__ == "__main__":
    unittest.main()
