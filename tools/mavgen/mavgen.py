#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mavgen: hangar's MAVLink header generator (modules/mavlink/mavlink_gen.h).

Reads the pinned MAVLink XML definitions in this directory (the files of the
mavlink submodule the ArduPilot tree pins at tag Sub-4.7.2, see README.md),
takes the messages and enums named in messages.txt, and writes a C11 header
with, per message: its id, CRC_EXTRA, full and minimum payload length, a C
struct in wire order, and pack/unpack functions over byte arrays. No code of
pymavlink or libmavlink is used or emitted: the XML is the source, this file
the generator, and the header is checked in. Run it with `python3 -I` (the
XML is downloaded data; -I keeps the interpreter from loading modules beside
it).

The wire format is docs/10's MAVLink 2, as the serialization guide specifies
(https://mavlink.io/en/guide/serialization.html, read 2026-11-07):
  - fields are sorted by native size, largest first, stable, extensions
    excluded and appended in XML order;
  - CRC_EXTRA is the CRC-16/MCRF4XX of "<name> " then, per base field in wire
    order, "<type> " "<field> " and, for an array, one byte with its length;
    folded to 8 bits as (crc & 0xff) ^ (crc >> 8);
  - the payload is little-endian; trailing zero bytes may be trimmed by the
    sender (never the first byte); a receiver zero-fills what is missing.

    python3 -I tools/mavgen/mavgen.py --out modules/mavlink/mavlink_gen.h
    python3 -I tools/mavgen/mavgen.py --check modules/mavlink/mavlink_gen.h
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))

SIZES = {
    "uint8_t": 1, "int8_t": 1, "char": 1, "uint8_t_mavlink_version": 1,
    "uint16_t": 2, "int16_t": 2,
    "uint32_t": 4, "int32_t": 4, "float": 4,
    "uint64_t": 8, "int64_t": 8, "double": 8,
}
CTYPES = {
    "uint8_t": "uint8_t", "int8_t": "int8_t", "char": "char", "uint8_t_mavlink_version": "uint8_t",
    "uint16_t": "uint16_t", "int16_t": "int16_t",
    "uint32_t": "uint32_t", "int32_t": "int32_t", "float": "float",
    "uint64_t": "uint64_t", "int64_t": "int64_t", "double": "double",
}


def x25_accumulate(crc, b):
    tmp = b ^ (crc & 0xff)
    tmp = (tmp ^ (tmp << 4)) & 0xff
    return ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xffff


def x25(data, crc=0xffff):
    for b in data:
        crc = x25_accumulate(crc, b)
    return crc


class Field:
    def __init__(self, el, ext):
        t = el.get("type")
        m = re.match(r"^([a-z0-9_]+)(?:\[(\d+)\])?$", t)
        if not m:
            raise SystemExit("unknown field type %r" % t)
        self.base = m.group(1)
        self.count = int(m.group(2)) if m.group(2) else 0  # 0: a scalar
        if self.base not in SIZES:
            raise SystemExit("unknown field type %r" % t)
        self.name = el.get("name")
        self.ext = ext
        self.size = SIZES[self.base]
        self.total = self.size * (self.count or 1)

    @property
    def crc_type(self):
        return "uint8_t" if self.base == "uint8_t_mavlink_version" else self.base


class Message:
    def __init__(self, el):
        self.id = int(el.get("id"))
        self.name = el.get("name")
        fields = []
        ext = False
        for child in el:
            if child.tag == "extensions":
                ext = True
            elif child.tag == "field":
                fields.append(Field(child, ext))
        base = [f for f in fields if not f.ext]
        exts = [f for f in fields if f.ext]
        base.sort(key=lambda f: -f.size)  # stable: equal sizes keep XML order
        self.fields = base + exts
        self.min_len = sum(f.total for f in base)
        self.len = sum(f.total for f in self.fields)
        if self.len > 255:
            raise SystemExit("%s: payload over 255 bytes" % self.name)
        crc = x25((self.name + " ").encode())
        for f in base:
            crc = x25((f.crc_type + " ").encode(), crc)
            crc = x25((f.name + " ").encode(), crc)
            if f.count:
                crc = x25_accumulate(crc, f.count)
        self.crc_extra = (crc & 0xff) ^ (crc >> 8)


def load(name, seen, messages, enums):
    """Parses one XML file and the files it includes (those present here)."""
    if name in seen:
        return
    seen.add(name)
    path = os.path.join(HERE, name)
    if not os.path.exists(path):
        sys.stderr.write("mavgen: include %s not present here, skipped (a dialect hangar does not use)\n" % name)
        return
    root = ET.parse(path).getroot()
    for inc in root.findall("include"):
        load(inc.text.strip(), seen, messages, enums)
    for el in root.iter("message"):
        m = Message(el)
        messages.setdefault(m.name, m)
    for el in root.iter("enum"):
        ename = el.get("name")
        entries = enums.setdefault(ename, [])
        have = {n for n, _ in entries}
        value = 0
        for e in el.findall("entry"):
            v = e.get("value")
            value = int(v, 0) if v is not None else value + 1
            if e.get("name") not in have:
                entries.append((e.get("name"), value))
                have.add(e.get("name"))


def read_list(path):
    msgs, enums = [], []
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        kind, rest = line.split(None, 1)
        if kind == "message":
            msgs.extend(rest.split())
        elif kind == "enum":
            name, _, only = rest.partition(":")
            enums.append((name.strip(), [s.strip() for s in only.split(",") if s.strip()]))
        else:
            raise SystemExit("messages.txt: unknown line %r" % line)
    return msgs, enums


PUT = {
    1: "buf[{o}] = (uint8_t){v};",
    2: "mav_put_u16(buf + {o}, (uint16_t){v});",
    4: "mav_put_u32(buf + {o}, (uint32_t){v});",
    8: "mav_put_u64(buf + {o}, (uint64_t){v});",
}
GET = {
    1: "({t})buf[{o}]",
    2: "({t})mav_get_u16(buf + {o})",
    4: "({t})mav_get_u32(buf + {o})",
    8: "({t})mav_get_u64(buf + {o})",
}


def emit_message(out, m):
    lname = m.name.lower()
    out.append("/* %s (id %d, CRC_EXTRA %d, %d bytes, %d without extensions) */" %
               (m.name, m.id, m.crc_extra, m.len, m.min_len))
    out.append("#define MAV_MSG_ID_%s %du" % (m.name, m.id))
    out.append("#define MAV_CRC_%s %du" % (m.name, m.crc_extra))
    out.append("#define MAV_LEN_%s %du" % (m.name, m.len))
    out.append("#define MAV_MIN_LEN_%s %du" % (m.name, m.min_len))
    out.append("struct mav_%s {" % lname)
    for f in m.fields:
        arr = "[%d]" % f.count if f.count else ""
        out.append("\t%s %s%s;%s" % (CTYPES[f.base], f.name, arr, " /* extension */" if f.ext else ""))
    out.append("};")
    out.append("static inline size_t mav_pack_%s(const struct mav_%s *m, uint8_t *buf)" % (lname, lname))
    out.append("{")
    off = 0
    for f in m.fields:
        if f.count:
            if f.base in ("float", "double"):
                out.append("\tfor (int i = 0; i < %d; i++) mav_put_f%d(buf + %d + %d * i, m->%s[i]);" %
                           (f.count, f.size * 8, off, f.size, f.name))
            elif f.size == 1:
                out.append("\tmemcpy(buf + %d, m->%s, %d);" % (off, f.name, f.count))
            else:
                out.append("\tfor (int i = 0; i < %d; i++) %s" %
                           (f.count, PUT[f.size].format(o="%d + %d * i" % (off, f.size), v="m->%s[i]" % f.name)))
        elif f.base == "float":
            out.append("\tmav_put_f32(buf + %d, m->%s);" % (off, f.name))
        elif f.base == "double":
            out.append("\tmav_put_f64(buf + %d, m->%s);" % (off, f.name))
        else:
            out.append("\t" + PUT[f.size].format(o=off, v="m->%s" % f.name))
        off += f.total
    out.append("\treturn %du;" % m.len)
    out.append("}")
    out.append("static inline void mav_unpack_%s(const uint8_t *in, size_t len, struct mav_%s *m)" % (lname, lname))
    out.append("{")
    out.append("\tuint8_t buf[%d];" % m.len)
    out.append("\tmemset(buf, 0, sizeof buf);")
    out.append("\tmemcpy(buf, in, len < sizeof buf ? len : sizeof buf);")
    off = 0
    for f in m.fields:
        t = CTYPES[f.base]
        if f.count:
            if f.base in ("float", "double"):
                out.append("\tfor (int i = 0; i < %d; i++) m->%s[i] = mav_get_f%d(buf + %d + %d * i);" %
                           (f.count, f.name, f.size * 8, off, f.size))
            elif f.size == 1:
                out.append("\tmemcpy(m->%s, buf + %d, %d);" % (f.name, off, f.count))
            else:
                out.append("\tfor (int i = 0; i < %d; i++) m->%s[i] = %s;" %
                           (f.count, f.name, GET[f.size].format(t=t, o="%d + %d * i" % (off, f.size))))
        elif f.base == "float":
            out.append("\tm->%s = mav_get_f32(buf + %d);" % (f.name, off))
        elif f.base == "double":
            out.append("\tm->%s = mav_get_f64(buf + %d);" % (f.name, off))
        else:
            out.append("\tm->%s = %s;" % (f.name, GET[f.size].format(t=t, o=off)))
        off += f.total
    out.append("}")
    out.append("")


def generate():
    messages, enums = {}, {}
    seen = set()
    load("ardupilotmega.xml", seen, messages, enums)
    want_msgs, want_enums = read_list(os.path.join(HERE, "messages.txt"))
    out = []
    out.append("// Generated by tools/mavgen/mavgen.py from the MAVLink XML definitions in")
    out.append("// tools/mavgen/ (mavlink/mavlink at the commit ArduPilot's Sub-4.7.2 pins,")
    out.append("// see tools/mavgen/README.md); do not edit. The definitions are MIT (the")
    out.append("// generated-code exception of tools/mavgen/COPYING.mavlink); the generator")
    out.append("// and this header are hangar's.")
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("#ifndef MAVLINK_GEN_H")
    out.append("#define MAVLINK_GEN_H")
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    out.append("#include <string.h>")
    out.append("")
    out.append("/* Little-endian field access; the host's float representation is IEEE 754")
    out.append(" * on every supported target, moved through an integer of the same size. */")
    out.append("static inline void mav_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }")
    out.append("static inline void mav_put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }")
    out.append("static inline void mav_put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }")
    out.append("static inline uint16_t mav_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }")
    out.append("static inline uint32_t mav_get_u32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }")
    out.append("static inline uint64_t mav_get_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }")
    out.append("static inline void mav_put_f32(uint8_t *p, float f) { uint32_t v; memcpy(&v, &f, 4); mav_put_u32(p, v); }")
    out.append("static inline void mav_put_f64(uint8_t *p, double f) { uint64_t v; memcpy(&v, &f, 8); mav_put_u64(p, v); }")
    out.append("static inline float mav_get_f32(const uint8_t *p) { uint32_t v = mav_get_u32(p); float f; memcpy(&f, &v, 4); return f; }")
    out.append("static inline double mav_get_f64(const uint8_t *p) { uint64_t v = mav_get_u64(p); double f; memcpy(&f, &v, 8); return f; }")
    out.append("")
    for ename, only in want_enums:
        if ename not in enums:
            raise SystemExit("enum %s not in the definitions" % ename)
        out.append("/* enum %s */" % ename)
        for n, v in enums[ename]:
            if only and n not in only:
                continue
            out.append("#define %s %d" % (n, v))
        out.append("")
    msgs = []
    for name in want_msgs:
        if name not in messages:
            raise SystemExit("message %s not in the definitions" % name)
        msgs.append(messages[name])
    msgs.sort(key=lambda m: m.id)
    for m in msgs:
        emit_message(out, m)
    out.append("/* Every message this header knows, by id: what the parser checks a CRC with. */")
    out.append("struct mav_msg_info { uint32_t id; uint8_t crc_extra; uint8_t len; uint8_t min_len; const char *name; };")
    out.append("static const struct mav_msg_info MAV_MSGS[] = {")
    for m in msgs:
        out.append("\t{ %du, %du, %du, %du, \"%s\" }," % (m.id, m.crc_extra, m.len, m.min_len, m.name))
    out.append("};")
    out.append("#define MAV_MSGS_COUNT %du" % len(msgs))
    out.append("static inline const struct mav_msg_info *mav_msg_info(uint32_t id)")
    out.append("{")
    out.append("\tfor (size_t i = 0; i < MAV_MSGS_COUNT; i++)")
    out.append("\t\tif (MAV_MSGS[i].id == id)")
    out.append("\t\t\treturn &MAV_MSGS[i];")
    out.append("\treturn NULL;")
    out.append("}")
    out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def main(argv):
    text = generate()
    if len(argv) == 3 and argv[1] == "--out":
        with open(argv[2], "w") as f:
            f.write(text)
        return 0
    if len(argv) == 3 and argv[1] == "--check":
        have = open(argv[2]).read() if os.path.exists(argv[2]) else ""
        if have != text:
            sys.stderr.write("%s is stale: run python3 -I tools/mavgen/mavgen.py --out %s\n" % (argv[2], argv[2]))
            return 1
        return 0
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
