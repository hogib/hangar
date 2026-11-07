# SPDX-License-Identifier: LGPL-3.0-or-later
"""Recording files (.hgr): what the recorder writes, read back in Python.

The format is lib/hg/hg_record.h (docs/06-recorder.md "Recording files"): a
4096-byte header, then fixed-size records, each a 128-byte record header and
the message's payload (absent in a `meta` file). The schema text in the
header describes the payload; hangar.schema lays it out. A file whose length
is not a whole number of records was cut off mid-write; the partial record
is ignored.

Pure standard library: records are decoded with struct from a read-only
mmap. With numpy installed, Recording.array() gives the whole file as one
structured array (numpy.memmap underneath), as docs/06 promises:

    rec = hangar.record.open_recording("session/nav.depth.hgr")
    len(rec), rec.header.name, rec.header.policy_text
    rec[3].seq, rec[3].stamp_ns, rec[3].gap, rec[3].inputs
    rec.payload(3)["depth_m"]
    rec.find_seq(100), rec.find_stamp(t_ns)
    a = rec.array(); a["seq"], a["depth_m"]            # numpy
"""

import mmap
import struct
from collections import namedtuple

from . import schema

MAGIC = b"HGREC001"
HEADER_BYTES = 4096
RECORD_HEADER_BYTES = 128
FLAG_META = 1            # header flags: payload absent
REC_GAP = 1 << 16        # record flags: messages are missing before this one
SLOT_STALE = 1           # slot flags, as in a topic
SLOT_DECIMATED = 2
POLICIES = {0: "full", 1: "every", 2: "rate", 3: "meta", 4: "off"}

# (name, struct code, offset) of the header, lib/hg/hg_record.h.
_HEADER_FIELDS = [
    ("magic", "8s", 0), ("version", "<I", 8), ("header_bytes", "<I", 12), ("record_bytes", "<Q", 16),
    ("slot_count", "<I", 24), ("record_header_bytes", "<I", 28), ("payload_bytes", "<I", 32),
    ("flags", "<I", 36), ("policy_n", "<Q", 40), ("records_offset", "<Q", 48), ("topic_id", "<I", 56),
    ("rate_mhz", "<I", 60), ("type_hash", "<Q", 64), ("name", "48s", 72), ("writer", "32s", 120),
    ("policy", "<I", 152), ("schema_bytes", "<I", 156), ("created_real_ns", "<Q", 160),
    ("first_seq", "<Q", 168), ("stale_after_ns", "<Q", 176), ("created_mono_ns", "<Q", 184),
    ("session", "64s", 192), ("origin_boot", "16s", 256), ("epoch", "<I", 272), ("policy_text", "32s", 280),
]

Header = namedtuple("Header", [f[0] for f in _HEADER_FIELDS] + ["schema", "policy_name", "meta"])
Record = namedtuple("Record", "seq stamp_ns publish_ns n_inputs flags gap inputs")
Input = namedtuple("Input", "topic_id flags seq")


class RecordError(ValueError):
    """A file that is not a recording."""


def _cstr(b):
    i = b.find(b"\0")
    return (b if i < 0 else b[:i]).decode("utf-8", "replace")


def parse_header(buf):
    """The header of a recording from its first 4096 bytes."""
    if len(buf) < HEADER_BYTES:
        raise RecordError("shorter than a header")
    vals = {}
    for name, code, off in _HEADER_FIELDS:
        vals[name] = struct.unpack_from(code, buf, off)[0]
    if vals["magic"] != MAGIC or vals["version"] != 1 or vals["header_bytes"] != HEADER_BYTES \
            or vals["record_header_bytes"] != RECORD_HEADER_BYTES or vals["records_offset"] != HEADER_BYTES \
            or vals["schema_bytes"] > 3584:
        raise RecordError("not a recording (HGREC001)")
    meta = bool(vals["flags"] & FLAG_META)
    if vals["record_bytes"] != RECORD_HEADER_BYTES + (0 if meta else vals["payload_bytes"]):
        raise RecordError("record size does not match the payload size")
    for name in ("name", "writer", "session", "policy_text"):
        vals[name] = _cstr(vals[name])
    vals["schema"] = buf[512:512 + vals["schema_bytes"]].decode("utf-8", "replace")
    vals["policy_name"] = POLICIES.get(vals["policy"], str(vals["policy"]))
    vals["meta"] = meta
    return Header(**vals)


def _unpack_record(buf, off):
    seq, stamp, pub, n_inputs, flags, gap = struct.unpack_from("<QQQIIQ", buf, off)
    inputs = tuple(Input(*struct.unpack_from("<IIQ", buf, off + 64 + 16 * k)) for k in range(4))
    return Record(seq, stamp, pub, n_inputs, flags, gap, inputs)


class Recording(object):
    """One .hgr file, mapped read-only."""

    def __init__(self, path):
        self.path = path
        self._f = open(path, "rb")
        size = self._f.seek(0, 2)
        if size < HEADER_BYTES:
            self._f.close()
            raise RecordError("%s: shorter than a header" % path)
        self._m = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            self.header = parse_header(self._m[:HEADER_BYTES])
        except RecordError as e:
            self.close()
            raise RecordError("%s: %s" % (path, e))
        self.record_bytes = self.header.record_bytes
        self.payload_bytes = self.header.payload_bytes
        self.meta = self.header.meta
        self.count = (size - HEADER_BYTES) // self.record_bytes
        self.layout = schema.layout_from_schema(self.header.schema, True, path, generated=True) \
            if self.header.schema.strip() else None
        self._struct = None

    def close(self):
        if getattr(self, "_m", None) is not None:
            self._m.close()
            self._m = None
        if getattr(self, "_f", None) is not None:
            self._f.close()
            self._f = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def __len__(self):
        return self.count

    def _off(self, i):
        if i < 0:
            i += self.count
        if not 0 <= i < self.count:
            raise IndexError(i)
        return HEADER_BYTES + i * self.record_bytes

    def __getitem__(self, i):
        """The record header of record i."""
        return _unpack_record(self._m, self._off(i))

    def __iter__(self):
        for i in range(self.count):
            yield self[i]

    def payload_bytes_at(self, i):
        """The raw payload of record i (b'' in a meta file)."""
        if self.meta:
            return b""
        off = self._off(i) + RECORD_HEADER_BYTES
        return self._m[off:off + self.payload_bytes]

    def payload(self, i):
        """The payload of record i as a dict of field name to value (a list for
        an array, bytes up to the NUL for char[N])."""
        if self.meta or self.layout is None:
            return {}
        buf = self.payload_bytes_at(i)
        out = {}
        for f in self.layout.fields:
            code = schema.KINDS[f.kind][2]
            if f.kind == "char":
                out[f.name] = _cstr(buf[f.offset:f.offset + f.count])
            elif f.is_array:
                out[f.name] = list(struct.unpack_from("<%d%s" % (f.count, code), buf, f.offset))
            else:
                out[f.name] = struct.unpack_from("<" + code, buf, f.offset)[0]
        return out

    def field(self, name):
        """One payload field over every record, as a list."""
        f = None
        for g in self.layout.fields if self.layout else ():
            if g.name == name:
                f = g
        if f is None:
            raise KeyError(name)
        code = schema.KINDS[f.kind][2]
        out = []
        for i in range(self.count):
            buf = self.payload_bytes_at(i)
            if f.kind == "char":
                out.append(_cstr(buf[f.offset:f.offset + f.count]))
            elif f.is_array:
                out.append(list(struct.unpack_from("<%d%s" % (f.count, code), buf, f.offset)))
            else:
                out.append(struct.unpack_from("<" + code, buf, f.offset)[0])
        return out

    def seqs(self):
        return [self[i].seq for i in range(self.count)]

    def find_seq(self, seq):
        """The index of the record with this seq, or -1 (seqs rise with the index)."""
        lo, hi = 0, self.count
        while lo < hi:
            mid = (lo + hi) // 2
            s = struct.unpack_from("<Q", self._m, self._off(mid))[0]
            if s == seq:
                return mid
            if s < seq:
                lo = mid + 1
            else:
                hi = mid
        return -1

    def _stamp(self, i):
        stamp, pub = struct.unpack_from("<QQ", self._m, self._off(i) + 8)
        return stamp or pub

    def find_stamp(self, t_ns):
        """The newest record stamped at or before t_ns (publish_ns when the
        stamp is 0), or -1: what lib/hg's hgr_find_stamp and the pairer's
        asof join give."""
        lo, hi = 0, self.count
        while lo < hi:
            mid = (lo + hi) // 2
            if self._stamp(mid) <= t_ns:
                lo = mid + 1
            else:
                hi = mid
        return lo - 1 if lo else -1

    def gaps(self):
        """[(index, lost)] for every record that carries a gap."""
        return [(i, r.gap) for i, r in enumerate(self) if r.flags & REC_GAP]

    def dtype(self):
        """The numpy dtype of one record: the record header's fields, then the
        payload's (prefixed with nothing: a payload field named `seq` would
        shadow the header's, so the header's are `_seq`, `_stamp_ns`,
        `_publish_ns`, `_n_inputs`, `_flags`, `_gap`, `_inputs` when a payload
        field of the plain name exists)."""
        import numpy  # pylint: disable=import-outside-toplevel

        payload_names = set(f.name for f in self.layout.fields) if self.layout and not self.meta else set()
        hdr = [("seq", "<u8", 0), ("stamp_ns", "<u8", 8), ("publish_ns", "<u8", 16), ("n_inputs", "<u4", 24),
               ("flags", "<u4", 28), ("gap", "<u8", 32)]
        names, formats, offsets = [], [], []
        for n, code, off in hdr:
            names.append(("_" + n) if n in payload_names else n)
            formats.append(code)
            offsets.append(off)
        names.append("_inputs" if "inputs" in payload_names else "inputs")
        formats.append(numpy.dtype({"names": ["topic_id", "flags", "seq"], "formats": ["<u4", "<u4", "<u8"],
                                    "offsets": [0, 4, 8], "itemsize": 16}, (4,)))
        offsets.append(64)
        if self.layout and not self.meta:
            pd = schema.dtype_from_schema(self.header.schema, self.path, generated=True)
            for n in pd.names:
                names.append(n)
                formats.append(pd.fields[n][0])
                offsets.append(RECORD_HEADER_BYTES + pd.fields[n][1])
        return numpy.dtype({"names": names, "formats": formats, "offsets": offsets, "itemsize": self.record_bytes})

    def array(self):
        """Every record as a numpy structured array over the file (memmap)."""
        import numpy  # pylint: disable=import-outside-toplevel

        return numpy.memmap(self.path, dtype=self.dtype(), mode="r", offset=HEADER_BYTES, shape=(self.count,))


def open_recording(path):
    return Recording(path)
