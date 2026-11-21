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


# The limits of lib/hg (hg_topic.h, hg_type.h, hg_dataset.h).
NAME_MAX = 47
FIELD_NAME_MAX = 63
MAX_PARTS = 16
MAX_SELECT = 32
MAX_WINDOW = 64
WHERE_TEXT = 512

_NAME = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)*$")
_DURATION = re.compile(r"^(\d*)(?:\.(\d*))?(ns|us|ms|s|min)?$")
_PLACES = {None: 0, "ns": 0, "us": 3, "ms": 6, "s": 9, "min": 9}


def _valid_name(text):
    """A topic name (hg_valid_name): segments of a-z, 0-9 and _ between
    single dots, at most 47 bytes."""
    return len(text) <= NAME_MAX and bool(_NAME.match(text))


def _decimal(whole, frac, places):
    """An unsigned decimal with up to `places` fractional places, as an
    integer of that scale (lib/hg's scaled()): an empty number, or more
    precision than the scale holds, is refused."""
    if not whole and not frac:
        raise ValueError("not a number")
    if frac[places:].strip("0"):
        raise ValueError("more precision than the unit holds")
    if int(whole or "0") > 1 << 50:
        raise ValueError("too large")
    v = int((whole or "0") + frac[:places].ljust(places, "0"))
    if v > 2 ** 63 - 1:
        raise ValueError("too large")
    return v


def _duration_ns(text, signed=False):
    """'200ms', '1.5s', '1min', '0' as nanoseconds, as hg_ini_parse_duration
    reads them: a decimal and a unit (ns, us, ms, s, min), a whole number of
    nanoseconds, only a bare 0 without a unit; no sign, unless `signed` (an
    asof offset: '-50ms', '+20ms')."""
    s = text
    neg = False
    if signed and s[:1] in ("-", "+"):
        neg = s[0] == "-"
        s = s[1:]
    m = _DURATION.match(s)
    if not m:
        raise ValueError(text)
    try:
        v = _decimal(m.group(1), m.group(2) or "", _PLACES[m.group(3)])
    except ValueError:
        raise ValueError(text)
    if m.group(3) is None and v != 0:
        raise ValueError(text)
    if m.group(3) == "min":
        v *= 60
    if v > 2 ** 63 - 1:
        raise ValueError(text)
    return -v if neg else v


def _fields(value):
    """Fields=: names of a-z, 0-9 and _, at least one, none twice (split_fields)."""
    out = []
    for f in value.split():
        if len(f) > FIELD_NAME_MAX:
            raise ValueError("field name too long")
        if len(out) == MAX_SELECT:
            raise ValueError("more than %d fields" % MAX_SELECT)
        if not re.match(r"^[a-z0-9_]+$", f):
            raise ValueError("bad field name '%s'" % f)
        if f in out:
            raise ValueError("field '%s' listed twice" % f)
        out.append(f)
    if not out:
        raise ValueError("Fields= is empty")
    return out


class DatasetSpec(object):
    """A .dataset spec, parsed with the rules of lib/hg/hg_dataset.c: what
    the C parser refuses of names, durations, rates, fields, windows and
    counts is refused here with its words. Not checked here: `Where` is
    kept as text and only its length is tested (the C parser compiles it),
    and nothing is checked against the topics' types (the row type's job,
    `hangar dataset check`)."""

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
                    if not arg or not _valid_name(arg):
                        raise SpecError("line %d: [Input <part>] needs a part name (a topic name)" % lineno)
                    if arg in parts:
                        raise SpecError("line %d: part %s given twice" % (lineno, arg))
                    if len(parts) == MAX_PARTS:
                        raise SpecError("line %d: more than %d parts" % (lineno, MAX_PARTS))
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
            if not _valid_name(value):
                raise ValueError("Anchor: '%s' is not a topic name" % value)
            self.anchor = value
        elif key == "AnchorRole":
            if value not in ("label", "input"):
                raise ValueError("AnchorRole: label or input")
            self.anchor_role = value
        elif key == "Fields":
            self.anchor_fields = _fields(value)
        elif key == "Rate":
            if value.startswith("every:"):
                if not re.match(r"^\+?\d+$", value[6:]) or int(value[6:]) == 0 or int(value[6:]) >= 2 ** 64:
                    raise ValueError("Rate: every:N needs N from 1")
                self.rate = ("every", int(value[6:]))
            elif value.startswith("rate:"):
                m = re.match(r"^(\d*)(?:\.(\d*))?$", value[5:])
                try:
                    mhz = _decimal(m.group(1), m.group(2) or "", 3) if m else 0
                except ValueError:
                    mhz = 0
                if not 0 < mhz <= 0xffffffff:
                    raise ValueError("Rate: rate:<Hz> needs a rate above 0")
                self.rate = ("rate", mhz / 1000.0)
            else:
                raise ValueError("Rate: every:N or rate:<Hz>")
        elif key == "Where":
            if len(value.encode("utf-8")) >= WHERE_TEXT:
                raise ValueError("Where: longer than %d bytes" % (WHERE_TEXT - 1))
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
            try:
                self.reset_after_ns = _duration_ns(value)
            except ValueError:
                raise ValueError("ResetAfter: '%s' is not a duration" % value)
        elif key == "Lateness":
            try:
                self.lateness_ns = _duration_ns(value)
            except ValueError:
                raise ValueError("Lateness: '%s' is not a duration" % value)
        else:
            raise ValueError("unknown key %s in [Dataset]" % key)

    @staticmethod
    def _part_key(part, key, value):
        if key == "Topic":
            if not _valid_name(value):
                raise ValueError("Topic: '%s' is not a topic name" % value)
            part["topic"] = value
        elif key == "Join":
            if value == "provenance":
                part["join"] = "provenance"
            elif value.startswith("asof:"):
                try:
                    part["offset_ns"] = _duration_ns(value[5:], signed=True)
                except ValueError:
                    raise ValueError("Join: provenance or asof:<offset>, not '%s'" % value)
                part["join"] = "asof"
            else:
                raise ValueError("Join: provenance or asof:<offset>, not '%s'" % value)
        elif key == "Window":
            if not re.match(r"^\+?\d+$", value) or not 1 <= int(value) <= MAX_WINDOW:
                raise ValueError("Window: 1 to %d, not '%s'" % (MAX_WINDOW, value))
            part["window"] = int(value)
        elif key == "Role":
            if value not in ("input", "label", "filter"):
                raise ValueError("Role: input, label or filter")
            part["role"] = value
        elif key == "Fields":
            part["fields"] = _fields(value)
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
