# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar message schemas: the .msg grammar, its layout and its hash, in Python.

A reader that opens a topic without a generated type (a dashboard, the
recorder, a notebook) parses the schema text in the topic's header with this
module. It must agree byte for byte with tools/msggen/msggen.c: the same
grammar, the same natural-alignment layout with the size rounded up to 8, the
same canonical form and the same FNV-1a 64 hash (lib/hg/hg_type_abi.md).
`make check-msggen` compares the two over every file in msg/.

Pure standard library. numpy is imported only inside dtype_from_schema, so
everything else works where numpy is not installed.

    python3 -m hangar.schema --print-hash msg/*.msg     "<name> <hash-hex> <payload_bytes>"
    python3 -m hangar.schema --canonical msg/depth.msg
"""

import os
import re
import sys
from collections import namedtuple

MAX_BYTES = 3584      # the topic header's schema field (docs/01-topics.md)
MAX_FIELDS = 128      # lib/hg's run-time parser holds this many (hg_type_abi.md)
MAX_NAME = 63

# kind -> (size in bytes, numpy code, struct code); the size is also the alignment.
KINDS = {
    "u8": (1, "u1", "B"), "i8": (1, "i1", "b"),
    "u16": (2, "u2", "H"), "i16": (2, "i2", "h"),
    "u32": (4, "u4", "I"), "i32": (4, "i4", "i"),
    "u64": (8, "u8", "Q"), "i64": (8, "i8", "q"),
    "f32": (4, "f4", "f"), "f64": (8, "f8", "d"),
    "char": (1, "S", "s"),
}
FLOAT_KINDS = ("f32", "f64")

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3


class SchemaError(ValueError):
    """A .msg text the generator would refuse; the message names the line."""


Field = namedtuple("Field", "name kind count is_array offset size comment line")
Layout = namedtuple("Layout", "fields payload_bytes")
ParamMeta = namedtuple("ParamMeta", "name default min max restart")

_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_FLOAT_CHARS = re.compile(r"^[0-9.eE+-]+$")


def fnv1a64(data):
    """FNV-1a 64 over bytes."""
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def _err(line, msg, file=None):
    where = "%s:%d" % (file or "<schema>", line) if line else (file or "<schema>")
    raise SchemaError("%s: %s" % (where, msg))


def _parse_num(kind, tok, line, what, file):
    """A value of kind, as the C generator accepts it: decimal only."""
    size = KINDS[kind][0]
    if kind in FLOAT_KINDS:
        if not _FLOAT_CHARS.match(tok):
            _err(line, "%s: '%s' is not a decimal number" % (what, tok), file)
        try:
            return float(tok)
        except ValueError:
            _err(line, "%s: '%s' is not a number" % (what, tok), file)
    try:
        if "_" in tok or not tok.isascii():
            raise ValueError(tok)
        v = int(tok, 10)
    except ValueError:
        _err(line, "%s: '%s' is not an integer" % (what, tok), file)
    if kind.startswith("i"):
        lo, hi = -(1 << (size * 8 - 1)), (1 << (size * 8 - 1)) - 1
    else:
        if tok.lstrip().startswith("-"):
            _err(line, "%s: %s is negative, the field is %s" % (what, tok, kind), file)
        lo, hi = 0, (1 << (size * 8)) - 1
    if v < lo or v > hi:
        _err(line, "%s: %s does not fit %s" % (what, tok, kind), file)
    return v


class _RawField(object):
    __slots__ = ("name", "kind", "count", "is_array", "comment", "default", "min",
                 "max", "restart", "line")


def _split_comment(line):
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c == "#" and not quoted:
            return line[:i], line[i + 1:].strip()
    return line, ""


def _parse_line(lineno, line, params, seen, file):
    body, comment = _split_comment(line)
    body = body.strip()
    if not body:
        return None
    parts = body.split(None, 1)
    kind = parts[0]
    if kind not in KINDS:
        _err(lineno, "unknown type '%s' (u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 char; "
             "no nested types)" % kind, file)
    rest = parts[1].strip() if len(parts) > 1 else ""
    if not rest:
        _err(lineno, "missing field name after '%s'" % kind, file)
    m = re.match(r"^([^\s=]+)", rest)
    name = m.group(1)
    rest = rest[m.end():].strip()

    count, is_array = 1, False
    if "[" in name:
        head, _, tail = name.partition("[")
        if not tail.endswith("]"):
            _err(lineno, "array size: expected %s[N]" % head, file)
        digits = tail[:-1]
        if not digits or len(digits) > 10 or not digits.isdigit() or not digits.isascii():
            _err(lineno, "array size: expected %s[N] with N from 1" % head, file)
        count = int(digits)
        if count < 1:
            _err(lineno, "array size must be at least 1: %s" % name, file)
        if count > 0xFFFFFFFF:
            _err(lineno, "array size %d is too large" % count, file)
        is_array = True
        name = head
    elif kind == "char":
        _err(lineno, "char needs a size: char %s[N]" % name, file)
    if "." in name:
        _err(lineno, "dotted name '%s': only generated dataset row types carry dots" % name, file)
    if not _IDENT.match(name) or not name.isascii():
        _err(lineno, "'%s' is not a C identifier" % name, file)
    if name.startswith("_"):
        _err(lineno, "'%s': names starting with '_' are reserved for generated names "
             "(padding, _seq)" % name, file)
    if len(name) > MAX_NAME:
        _err(lineno, "name '%s' is longer than %d bytes" % (name, MAX_NAME), file)
    if name in seen:
        _err(lineno, "duplicate field '%s' (first on line %d)" % (name, seen[name]), file)

    default, rmin, rmax, restart, stage = "", "", "", False, 0
    while rest:
        if rest[0] == "=":
            if stage >= 1:
                _err(lineno, "the default comes first: = default [min, max] @restart", file)
            stage = 1
            rest = rest[1:].strip()
            if not rest:
                _err(lineno, "missing value after '='", file)
            if rest[0] == '"':
                j = rest.find('"', 1)
                if j < 0:
                    _err(lineno, "unterminated string in default", file)
                j += 1
            else:
                j = 0
                while j < len(rest) and rest[j] not in "[@":
                    j += 1
            default, rest = rest[:j].strip(), rest[j:]
            if not default:
                _err(lineno, "missing value after '='", file)
        elif rest[0] == "[":
            if stage >= 2:
                _err(lineno, "the range comes before @restart, and only once", file)
            stage = 2
            j = rest.find("]")
            if j < 0:
                _err(lineno, "unterminated range: expected [min, max]", file)
            inner = rest[1:j]
            vals = inner.split(",")
            if len(vals) != 2 or not vals[0].strip() or not vals[1].strip():
                _err(lineno, "range: expected [min, max], got [%s]" % inner, file)
            rmin, rmax = vals[0].strip(), vals[1].strip()
            rest = rest[j + 1:]
        elif rest.startswith("@restart") and (len(rest) == 8 or rest[8].isspace()):
            if stage >= 3:
                _err(lineno, "@restart given twice", file)
            stage = 3
            restart = True
            rest = rest[8:]
        else:
            _err(lineno, "unexpected '%s' after the field" % rest.split()[0], file)
        rest = rest.strip()
    if stage and not params:
        _err(lineno, "defaults, ranges and @restart belong to parameter types: pass --params", file)

    f = _RawField()
    f.name, f.kind, f.count, f.is_array, f.comment = name, kind, count, is_array, comment
    f.default, f.min, f.max, f.restart, f.line = default, rmin, rmax, restart, lineno
    _check_attrs(f, file)
    return f


def _check_attrs(f, file):
    lo = hi = None
    if f.min:
        if f.kind == "char":
            _err(f.line, "a char field has no range", file)
        lo = _parse_num(f.kind, f.min, f.line, "range", file)
        hi = _parse_num(f.kind, f.max, f.line, "range", file)
        if lo > hi:
            _err(f.line, "range [%s, %s] has min above max" % (f.min, f.max), file)
    if f.default:
        d = f.default
        if f.kind == "char":
            if len(d) < 2 or d[0] != '"' or d[-1] != '"':
                _err(f.line, 'a char default is quoted text: = "..."', file)
            if '"' in d[1:-1] or "\\" in d[1:-1]:
                _err(f.line, "a char default holds no quotes or backslashes", file)
            if len(d[1:-1].encode("utf-8")) > f.count - 1:
                _err(f.line, "default %s is longer than %s[%d] holds (%d bytes and a NUL)"
                     % (d, f.name, f.count, f.count - 1), file)
            return
        vals = [v.strip() for v in d.split(",")]
        for v in vals:
            n = _parse_num(f.kind, v, f.line, "default", file)
            if lo is not None and (n < lo or n > hi):
                _err(f.line, "default %s is outside [%s, %s]" % (v, f.min, f.max), file)
        if len(vals) != 1 and len(vals) != f.count:
            _err(f.line, "default has %d values; %s[%d] takes 1 or %d"
                 % (len(vals), f.name, f.count, f.count), file)
        if len(vals) != 1 and not f.is_array:
            _err(f.line, "default has %d values for a scalar" % len(vals), file)


def _parse(text, params=True, file=None):
    """The raw fields of a .msg text, before layout. Raises SchemaError."""
    if isinstance(text, bytes):
        data = text
    else:
        data = text.encode("utf-8")
    if len(data) > MAX_BYTES:
        _err(0, "%d bytes; the topic header holds a schema of at most %d" % (len(data), MAX_BYTES), file)
    if b"\0" in data:
        _err(0, "NUL byte in the file", file)
    text = data.decode("utf-8", "replace")
    fields, seen = [], {}
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()  # the empty tail after a final newline is not a line
    for lineno, line in enumerate(lines, 1):
        f = _parse_line(lineno, line, params, seen, file)
        if f is None:
            continue
        if len(fields) >= MAX_FIELDS:
            _err(lineno, "more than %d fields" % MAX_FIELDS, file)
        seen[f.name] = lineno
        fields.append(f)
    if not fields:
        _err(0, "no fields (an empty type is 'u8 reserved')", file)
    return fields


def _align_up(v, a):
    return (v + a - 1) // a * a


def layout_from_schema(text, params=True, file=None):
    """Layout(fields, payload_bytes): each field with its offset and size in bytes.

    Natural alignment, the struct size rounded up to a multiple of 8, as
    lib/hg/hg_type_abi.md specifies and as the C generator computes.
    """
    raw = _parse(text, params, file)
    off, maxalign, fields = 0, 1, []
    for f in raw:
        a = KINDS[f.kind][0]
        off = _align_up(off, a)
        size = a * f.count
        fields.append(Field(f.name, f.kind, f.count, f.is_array, off, size, f.comment, f.line))
        off += size
        maxalign = max(maxalign, a)
    payload = _align_up(_align_up(off, maxalign), 8)
    if payload > 0xFFFFFFFF:
        _err(0, "the payload exceeds 4 GiB", file)
    return Layout(fields, payload)


def canonical(text, file=None):
    """The canonical form: one 'type name' or 'type name[N]' per line, each ended by \\n."""
    out = []
    for f in _parse(text, True, file):
        out.append("%s %s[%d]\n" % (f.kind, f.name, f.count) if f.is_array
                   else "%s %s\n" % (f.kind, f.name))
    return "".join(out)


def type_hash(text, file=None):
    """FNV-1a 64 of the canonical form, as an int; equal to the C generator's."""
    return fnv1a64(canonical(text, file).encode("ascii"))


def _parse_default(kind, count, d):
    if not d:
        if kind == "char":
            return ""
        return 0 if count == 1 else [0] * count
    if kind == "char":
        return d[1:-1]
    conv = float if kind in FLOAT_KINDS else int
    vals = [conv(v.strip()) for v in d.split(",")]
    if count == 1:
        return vals[0]
    return vals if len(vals) == count else vals * count


def parse_params(text, file=None):
    """ParamMeta(name, default, min, max, restart) per field, values parsed.

    A default missing from the text is 0 (or "" for char), as docs/05 says;
    arrays get a list of count values; min and max are None without a range.
    """
    out = []
    for f in _parse(text, True, file):
        conv = float if f.kind in FLOAT_KINDS else int
        lo = conv(f.min) if f.min else None
        hi = conv(f.max) if f.max else None
        out.append(ParamMeta(f.name, _parse_default(f.kind, f.count, f.default), lo, hi, f.restart))
    return out


def dtype_from_schema(text, file=None):
    """A numpy structured dtype of the layout, little-endian, itemsize payload_bytes.

    char[N] becomes 'S<N>'; an array field a subarray. Padding comes from the
    explicit offsets and itemsize. Imports numpy here, so the rest of the
    module needs none.
    """
    import numpy  # pylint: disable=import-outside-toplevel

    lay = layout_from_schema(text, True, file)
    names, formats, offsets = [], [], []
    for f in lay.fields:
        names.append(f.name)
        offsets.append(f.offset)
        code = KINDS[f.kind][1]
        if f.kind == "char":
            formats.append("S%d" % f.count)
        elif f.is_array:
            formats.append((("<" + code), (f.count,)))
        else:
            formats.append("<" + code)
    return numpy.dtype({"names": names, "formats": formats, "offsets": offsets,
                        "itemsize": lay.payload_bytes})


def name_from_path(path):
    """'msg/depth.msg' -> 'depth'; raises SchemaError as the generator refuses."""
    base = os.path.basename(path)
    if not base.endswith(".msg") or len(base) < 5:
        raise SchemaError("%s: not a .msg file" % path)
    stem = base[:-4]
    if "." in stem:
        raise SchemaError("%s: dotted type name '%s': only generated dataset row types carry dots"
                          % (path, stem))
    if not _IDENT.match(stem) or not stem.isascii():
        raise SchemaError("%s: type name '%s' is not a C identifier" % (path, stem))
    if stem.startswith("_"):
        raise SchemaError("%s: type name '%s' starts with '_', reserved" % (path, stem))
    if len(stem) > MAX_NAME:
        raise SchemaError("%s: type name '%s' is longer than %d bytes" % (path, stem, MAX_NAME))
    return stem


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    mode, files = None, []
    for a in argv:
        if a in ("--print-hash", "--canonical"):
            mode = a
        elif a == "--params":
            pass  # attributes are always accepted here; the hash ignores them
        elif a.startswith("-"):
            sys.stderr.write("schema: unknown option %s\n" % a)
            return 2
        else:
            files.append(a)
    if mode is None or not files:
        sys.stderr.write("usage: python3 -m hangar.schema --print-hash|--canonical <file.msg>...\n")
        return 2
    rc = 0
    for path in files:
        try:
            name = name_from_path(path)
            with open(path, "rb") as fp:
                data = fp.read()
            if mode == "--print-hash":
                lay = layout_from_schema(data, True, path)
                sys.stdout.write("%s %016x %d\n" % (name, type_hash(data, path), lay.payload_bytes))
            else:
                sys.stdout.write(canonical(data, path))
        except (SchemaError, OSError) as e:
            sys.stderr.write("schema: %s\n" % e)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
