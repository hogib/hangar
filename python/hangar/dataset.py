# SPDX-License-Identifier: LGPL-3.0-or-later
"""Datasets: the spec (.dataset) and the rows (docs/06-recorder.md "Datasets").

A row stream is a recording (hangar.record) whose payload is the row type
lib/hg's hg_dataset generates: the header fields anchor_seq, anchor_stamp_ns,
boot_id, missing, flags, emitted_ns, then the anchor's fields as
`<anchor>.<field>` and each part's `<part>._seq`, `_stamp_ns`, `_count`,
`_flags` and `<part>.<field>` (`<part>.<k>.<field>` over a window, k = 0 the
oldest). Each field's schema comment starts with `role=meta|input|label|
filter`, so a trainer splits inputs from labels without the spec. The same
bytes come from the pairer's sink file (dataset.<name>.hgr), from
`hangar dataset build` and, live, from the dataset.<name> topic.

    ds = hangar.dataset.open_dataset("depth_ff.hgr")
    ds.roles()                       # {"nav.state.depth": "input", ...}
    ds.fields("input"), ds.fields("label")
    for row in ds.rows(): row["anchor_seq"], row["flags"] & dataset.LATE, row["nav.state.depth"]
    ds.array()                       # numpy: one structured array, the dtype of the rows

    spec = hangar.dataset.DatasetSpec.parse(open("depth_ff.dataset").read(), "depth_ff")
    spec.anchor, spec.parts[0].topic, spec.parts[0].join, spec.lateness_ns

Pure standard library; numpy only for array().
"""

import re
from collections import namedtuple

from . import record, schema

# Row flags (hg_dataset.h).
RESET = 1
MASKED = 2
LATE = 4
HEADER_FIELDS = ("anchor_seq", "anchor_stamp_ns", "boot_id", "missing", "flags", "emitted_ns")

_ROLE = re.compile(r"^role=(meta|input|label|filter)\b")


class SpecError(ValueError):
    """A spec the C parser would refuse (the message names the line)."""


Part = namedtuple("Part", "name topic join offset_ns window role fields line")


def _duration_ns(text):
    """'200ms', '1.5s', '1min', '0', '-50ms' as nanoseconds."""
    s = text.strip()
    neg = s.startswith("-")
    if neg or s.startswith("+"):
        s = s[1:]
    m = re.match(r"^(\d+(?:\.\d+)?)(ns|us|ms|s|min)?$", s)
    if not m or (m.group(2) is None and m.group(1) != "0"):
        raise ValueError(text)
    scale = {"ns": 1, "us": 1000, "ms": 10 ** 6, "s": 10 ** 9, "min": 60 * 10 ** 9, None: 1}[m.group(2)]
    v = float(m.group(1)) * scale
    if v != int(v):
        raise ValueError(text)
    return -int(v) if neg else int(v)


class DatasetSpec(object):
    """A .dataset spec, parsed with the rules of lib/hg/hg_dataset.c."""

    def __init__(self, name):
        self.name = name
        self.anchor = None
        self.anchor_role = "label"
        self.anchor_fields = []
        self.rate = ("every", 1)
        self.where = ""
        self.filter = "drop"
        self.missing = "flag"
        self.reset_after_ns = 200 * 10 ** 6
        self.lateness_ns = 100 * 10 ** 6
        self.parts = []

    @classmethod
    def parse(cls, text, name):
        spec = cls(name)
        section, part, seen, dataset_seen = None, None, set(), False
        parts = {}
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = raw.strip()
            if not line or line[0] in "#;":
                continue
            if line.startswith("["):
                if not line.endswith("]"):
                    raise SpecError("line %d: a section line must end in ']'" % lineno)
                inner = line[1:-1].strip().split(None, 1)
                sec, arg = inner[0], (inner[1].strip() if len(inner) > 1 else "")
                seen = set()
                if sec == "Dataset":
                    if arg or dataset_seen:
                        raise SpecError("line %d: [Dataset] once, without an argument" % lineno)
                    dataset_seen, section, part = True, "Dataset", None
                elif sec == "Input":
                    if not arg or not re.match(r"^[a-z0-9_]+(\.[a-z0-9_]+)*$", arg):
                        raise SpecError("line %d: [Input <part>] needs a part name" % lineno)
                    if arg in parts:
                        raise SpecError("line %d: part %s given twice" % (lineno, arg))
                    part = {"name": arg, "topic": arg, "join": None, "offset_ns": 0, "window": 1,
                            "role": "input", "fields": [], "line": lineno}
                    parts[arg] = part
                    spec.parts.append(part)
                    section = "Input"
                else:
                    raise SpecError("line %d: unknown section [%s]" % (lineno, sec))
                continue
            if "=" not in line:
                raise SpecError("line %d: expected [Section] or Key=Value" % lineno)
            if section is None:
                raise SpecError("line %d: a key before any section" % lineno)
            key, value = [x.strip() for x in line.split("=", 1)]
            if key in seen:
                raise SpecError("line %d: %s given twice" % (lineno, key))
            seen.add(key)
            try:
                if section == "Dataset":
                    spec._dataset_key(key, value)
                else:
                    spec._part_key(part, key, value)
            except ValueError as e:
                raise SpecError("line %d: %s" % (lineno, e))
        if not dataset_seen or not spec.anchor:
            raise SpecError("[Dataset] with Anchor= is required")
        for p in spec.parts:
            if p["join"] is None:
                raise SpecError("line %d: [Input %s] needs Join=" % (p["line"], p["name"]))
        spec.parts = [Part(**p) for p in spec.parts]
        return spec

    def _dataset_key(self, key, value):
        if key == "Anchor":
            self.anchor = value
        elif key == "AnchorRole":
            if value not in ("label", "input"):
                raise ValueError("AnchorRole: label or input")
            self.anchor_role = value
        elif key == "Fields":
            self.anchor_fields = value.split()
        elif key == "Rate":
            if value.startswith("every:"):
                self.rate = ("every", int(value[6:]))
            elif value.startswith("rate:"):
                self.rate = ("rate", float(value[5:]))
            else:
                raise ValueError("Rate: every:N or rate:<Hz>")
        elif key == "Where":
            self.where = value
        elif key == "Filter":
            if value not in ("drop", "mask"):
                raise ValueError("Filter: drop or mask")
            self.filter = value
        elif key == "Missing":
            if value not in ("flag", "drop"):
                raise ValueError("Missing: flag or drop")
            self.missing = value
        elif key == "ResetAfter":
            self.reset_after_ns = _duration_ns(value)
        elif key == "Lateness":
            self.lateness_ns = _duration_ns(value)
        else:
            raise ValueError("unknown key %s in [Dataset]" % key)

    @staticmethod
    def _part_key(part, key, value):
        if key == "Topic":
            part["topic"] = value
        elif key == "Join":
            if value == "provenance":
                part["join"] = "provenance"
            elif value.startswith("asof:"):
                part["join"] = "asof"
                part["offset_ns"] = _duration_ns(value[5:])
            else:
                raise ValueError("Join: provenance or asof:<offset>")
        elif key == "Window":
            w = int(value)
            if not 1 <= w <= 64:
                raise ValueError("Window: 1 to 64")
            part["window"] = w
        elif key == "Role":
            if value not in ("input", "label", "filter"):
                raise ValueError("Role: input, label or filter")
            part["role"] = value
        elif key == "Fields":
            part["fields"] = value.split()
        else:
            raise ValueError("unknown key %s in [Input %s]" % (key, part["name"]))

    def wait_ns(self):
        """The live row's wait after its anchor: the largest offset + Lateness
        of the asof joins, at least 0."""
        w = 0
        for p in self.parts:
            if p.join == "asof":
                w = max(w, p.offset_ns + self.lateness_ns)
        return w

    def policy_usable(self):
        """(usable, [input parts whose asof join would have to wait])."""
        bad = [p.name for p in self.parts
               if p.role == "input" and p.join == "asof" and p.offset_ns + self.lateness_ns > 0]
        return (not bad, bad)


class Dataset(record.Recording):
    """A row stream file: a recording whose payload is a dataset row."""

    def __init__(self, path):
        record.Recording.__init__(self, path)
        if self.layout is None or [f.name for f in self.layout.fields[:6]] != list(HEADER_FIELDS):
            self.close()
            raise record.RecordError("%s: not a dataset row stream (no row header fields)" % path)
        self._roles = {}
        for f in self.layout.fields:
            m = _ROLE.match(f.comment or "")
            self._roles[f.name] = m.group(1) if m else "meta"

    def roles(self):
        """{field name: role} for every row field, from the schema's comments."""
        return dict(self._roles)

    def fields(self, role=None):
        """The row fields of one role (input, label, filter, meta), or all."""
        return [f.name for f in self.layout.fields if role is None or self._roles[f.name] == role]

    def parts(self):
        """The part names, in order, from the `<part>._seq` fields."""
        return [f.name[:-5] for f in self.layout.fields if f.name.endswith("._seq")]

    def row(self, i):
        """Row i as a dict of row field to value."""
        return self.payload(i)

    def rows(self):
        for i in range(self.count):
            yield self.payload(i)

    def anchor_seqs(self):
        return self.field("anchor_seq")

    def flags(self):
        return self.field("flags")

    def missing(self):
        return self.field("missing")


def open_dataset(path):
    return Dataset(path)
