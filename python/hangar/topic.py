# SPDX-License-Identifier: LGPL-3.0-or-later
"""A live topic, read in place (docs/01-topics.md): the file mapped
read-only, the messages read as lib/hg's hg_next reads them. A slot holds
message n whole when its seq word is 2n before and after the copy; a seq
stepped over is a gap. A file holds the messages after its first_seq: a
reader opened "oldest" starts there (or at the oldest message safely in a
ring that has wrapped), and when the writer replaces the file, the seqs
between the last message read and the new file's first_seq are the gap of
the next message. The reader writes nothing, so it needs no wiring: any
process that may read the topic directory may read the topic.

    t = hangar.topic.Topic("dataset.depth_ff")       # $HANGAR_TOPIC_DIR or /dev/shm/hangar
    t.layout                                        # hangar.schema.Layout, from the file's schema
    for m in t.read():                              # what is new since the last read
        m.seq, m.stamp_ns, m.flags, m.gap, m.refs, m.payload (bytes), t.decode(m)

Pure standard library.
"""

import mmap
import os
import struct
import time
from collections import namedtuple

from . import schema

MAGIC = b"HGTOPIC1"
SLOT_STALE = 1
SLOT_DECIMATED = 2

# The header's fields (lib/hg/hg_topic.c).
_OFF_SLOT_COUNT = 24
_OFF_SLOT_HEADER = 28
_OFF_PAYLOAD = 32
_OFF_STRIDE = 40
_OFF_SLOTS = 48
_OFF_TOPIC_ID = 56
_OFF_RATE = 60
_OFF_TYPE_HASH = 64
_OFF_NAME = 72
_OFF_WRITER = 120
_OFF_SCHEMA_BYTES = 156
_OFF_GENERATION = 16
_OFF_FIRST_SEQ = 168
_OFF_STALE_AFTER = 176
_OFF_WRITE_SEQ = 200
_OFF_LAST_PUBLISH = 208
_OFF_ORIGIN_BOOT = 256
_OFF_SCHEMA = 512
# A slot's.
_SL_SEQ = 0
_SL_STAMP = 8
_SL_PUBLISH = 16
_SL_N_INPUTS = 24
_SL_FLAGS = 28
_SL_INPUTS = 64

Message = namedtuple("Message", "seq stamp_ns publish_ns flags gap refs payload")


class TopicError(OSError):
    pass


def topic_dir():
    """$HANGAR_TOPIC_DIR (a module's, set by the manager); else the topic
    directory of $HANGAR_ROOT's manager, as the CLI and learn.state_dir
    take the root; else /dev/shm/hangar."""
    d = os.environ.get("HANGAR_TOPIC_DIR")
    if d:
        return d
    root = os.environ.get("HANGAR_ROOT")
    return os.path.join(root, "dev/shm/hangar") if root else "/dev/shm/hangar"


def _u32(m, off):
    return struct.unpack_from("<I", m, off)[0]


def _u64(m, off):
    return struct.unpack_from("<Q", m, off)[0]


class Topic(object):
    """One topic, read from `start`: "newest" (only messages published from
    now on) or "oldest" (every message still in the ring)."""

    def __init__(self, name, directory=None, start="newest"):
        self.name = name
        self.path = os.path.join(directory or topic_dir(), name)
        self._m = None
        self._f = None
        self._start = start
        self._pending = 0       # seqs stepped over since the last message returned
        self._open()

    def _open(self):
        self.close()
        try:
            self._f = open(self.path, "rb")
        except OSError as e:
            raise TopicError(e.errno, "%s: %s" % (self.path, e.strerror))
        self._m = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        m = self._m
        if m[0:8] != MAGIC:
            raise TopicError(0, "%s: not a topic file" % self.path)
        self.slot_count = _u32(m, _OFF_SLOT_COUNT)
        self.slot_header_bytes = _u32(m, _OFF_SLOT_HEADER)
        self.payload_bytes = _u32(m, _OFF_PAYLOAD)
        self.slot_stride = _u32(m, _OFF_STRIDE)
        self.slots_offset = _u64(m, _OFF_SLOTS)
        self.topic_id = _u32(m, _OFF_TOPIC_ID)
        self.rate_mhz = _u32(m, _OFF_RATE)
        self.type_hash = _u64(m, _OFF_TYPE_HASH)
        self.writer = m[_OFF_WRITER:_OFF_WRITER + 32].split(b"\0", 1)[0].decode("utf-8", "replace")
        self.stale_after_ns = _u64(m, _OFF_STALE_AFTER)
        # The boot id of the machine it was published on, when the bridge
        # mirrors it here from the other side (docs/08 "Mirrored topics");
        # zeros for a topic of this machine.
        self.origin_boot = bytes(m[_OFF_ORIGIN_BOOT:_OFF_ORIGIN_BOOT + 16])
        self.mirrored = any(self.origin_boot)
        self.generation = _u64(m, _OFF_GENERATION)
        n = _u32(m, _OFF_SCHEMA_BYTES)
        self.schema = m[_OFF_SCHEMA:_OFF_SCHEMA + n].split(b"\0", 1)[0].decode("utf-8")
        self.layout = schema.layout_from_schema(self.schema, True, self.path, generated=True) \
            if self.schema.strip() else None
        first = _u64(m, _OFF_FIRST_SEQ)
        if self._start == "oldest":
            # hg_topic.c: s->last = w - first > k - 1 ? w - k + 1 : first.
            # Message first_seq itself is in no file: the first to read is
            # the one after it.
            w = _u64(m, _OFF_WRITE_SEQ)
            self.last = max(first, w - self.slot_count + 1)
        else:
            self.last = _u64(m, _OFF_WRITE_SEQ)
        self._first = first

    def close(self):
        if self._m is not None:
            self._m.close()
            self._m = None
        if self._f is not None:
            self._f.close()
            self._f = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def write_seq(self):
        return _u64(self._m, _OFF_WRITE_SEQ)

    def last_publish_ns(self):
        return _u64(self._m, _OFF_LAST_PUBLISH)

    def _slot(self, n):
        return self.slots_offset + ((n - 1) & (self.slot_count - 1)) * self.slot_stride

    def _replaced(self):
        """The writer replaced the file (a new generation): the new one, from
        where this reader was. A writer that restarts goes on with its seqs,
        so the last seq read stays, and read() counts what lies between it
        and the new file's first_seq, which is in neither file, as a gap. A
        file numbered below the last seq read is a topic made anew (lib/hg
        knows it by its epoch): every message of it, from its first."""
        try:
            st = os.stat(self.path)
        except OSError:
            return False
        if st.st_ino == os.fstat(self._f.fileno()).st_ino:
            return False
        last = self.last
        start, self._start = self._start, "oldest"
        try:
            self._open()
        finally:
            self._start = start
        if last <= self.write_seq():
            self.last = last
        else:
            self.last = self._first
        return True

    def read(self, limit=None):
        """The messages since the last read, in seq order: a list of Message."""
        out = []
        pending = self._pending
        m = self._m
        k = self.slot_count
        while limit is None or len(out) < limit:
            W = _u64(m, _OFF_WRITE_SEQ)
            if self.last < self._first:
                # Messages before this file's first are in no file.
                pending += self._first - self.last
                self.last = self._first
            w = self.last + 1
            if w > W:
                if self._replaced():
                    m = self._m
                    k = self.slot_count
                    continue
                break
            if W - w >= k - 1:
                nw = W - k + 2
                pending += nw - w
                w = nw
                self.last = w - 1
            off = self._slot(w)
            s1 = _u64(m, off + _SL_SEQ)
            if s1 != 2 * w:
                pending += 1
                self.last = w
                continue
            stamp, publish = struct.unpack_from("<QQ", m, off + _SL_STAMP)
            n_in, flags = struct.unpack_from("<II", m, off + _SL_N_INPUTS)
            refs = [struct.unpack_from("<IIQ", m, off + _SL_INPUTS + 16 * i) for i in range(min(n_in, 4))]
            p = off + self.slot_header_bytes
            payload = bytes(m[p:p + self.payload_bytes])
            if _u64(m, off + _SL_SEQ) != s1:
                continue  # overwritten while copying: lost on the next pass
            gap = 0 if flags & SLOT_DECIMATED else pending
            pending = 0
            self.last = w
            out.append(Message(w, stamp, publish, flags, gap,
                               [(tid, seq) for tid, _f, seq in refs], payload))
        self._pending = pending     # for the next message, on a later read
        return out

    def decode(self, msg):
        """A message's payload as a dict of field to value (hangar.record's rules)."""
        buf = msg.payload if isinstance(msg, Message) else msg
        out = {}
        for f in self.layout.fields:
            code = schema.KINDS[f.kind][2]
            if f.kind == "char":
                out[f.name] = buf[f.offset:f.offset + f.count].split(b"\0", 1)[0].decode("utf-8", "replace")
            elif f.is_array:
                out[f.name] = list(struct.unpack_from("<%d%s" % (f.count, code), buf, f.offset))
            else:
                out[f.name] = struct.unpack_from("<" + code, buf, f.offset)[0]
        return out

    def wait(self, timeout=1.0, poll=0.005):
        """Messages, waiting up to timeout seconds for the first (polling: a
        reader that writes nothing cannot use the topic's futex)."""
        end = time.monotonic() + timeout
        while True:
            got = self.read()
            if got or time.monotonic() >= end:
                return got
            time.sleep(poll)


def open_topic(name, directory=None, start="newest", timeout=0.0):
    """A Topic, waiting up to timeout seconds for its file to appear."""
    end = time.monotonic() + timeout
    while True:
        try:
            return Topic(name, directory, start)
        except TopicError:
            if time.monotonic() >= end:
                raise
            time.sleep(0.05)
