# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar.schema against the same vectors as tools/msggen/tests/test_msggen.c.

    python3 -m unittest discover -s python/tests          (from the repository root)
    python3 -m unittest python/tests/test_schema.py
"""

import glob
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
REPO = os.path.dirname(os.path.dirname(HERE))

from hangar import schema  # noqa: E402
from hangar.schema import SchemaError  # noqa: E402

DEPTH_DOCS = (
    "# msg/depth.msg\n"
    "f32 depth_m         # m, positive down\n"
    "f32 temp_c          # degrees Celsius\n"
    "f32 pressure_mbar   # mbar, absolute\n"
)
DEPTH_HASH = 0xF397A321CD6643A1

PARAMS_DOCS = (
    "# msg/depth_hold_params.msg\n"
    "f32  kp = 1.5          [0, 10]   # N/m\n"
    "f32  ki = 0.1          [0, 2]    # N/(m s)\n"
    "f32  max_thrust = 0.6  [0, 1]    # fraction of full thrust\n"
    "f32  gains[3] = 1, 1, 0.5        # one value applies to every element\n"
    "u8   enabled = 0       [0, 1]\n"
    'char frame[16] = "ned"\n'
    "u32  rate_hz = 50      [10, 200] @restart\n"
)


def offsets(text):
    return [f.offset for f in schema.layout_from_schema(text).fields]


class DepthVector(unittest.TestCase):
    def test_canonical(self):
        self.assertEqual(schema.canonical(DEPTH_DOCS), "f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n")

    def test_hash(self):
        self.assertEqual(schema.type_hash(DEPTH_DOCS), DEPTH_HASH)
        self.assertEqual(schema.fnv1a64(b"f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n"), DEPTH_HASH)

    def test_layout(self):
        lay = schema.layout_from_schema(DEPTH_DOCS)
        self.assertEqual(lay.payload_bytes, 16)
        self.assertEqual([f.name for f in lay.fields], ["depth_m", "temp_c", "pressure_mbar"])
        self.assertEqual([f.offset for f in lay.fields], [0, 4, 8])
        self.assertEqual([f.size for f in lay.fields], [4, 4, 4])
        self.assertEqual(lay.fields[0].comment, "m, positive down")
        self.assertEqual(lay.fields[0].kind, "f32")
        self.assertFalse(lay.fields[0].is_array)
        self.assertEqual(lay.fields[1].line, 3)

    def test_bytes_input(self):
        self.assertEqual(schema.type_hash(DEPTH_DOCS.encode()), DEPTH_HASH)


class Layout(unittest.TestCase):
    def test_alignment(self):
        self.assertEqual(offsets("u8 a\nu64 b\n"), [0, 8])
        self.assertEqual(schema.layout_from_schema("u8 a\nu64 b\n").payload_bytes, 16)
        self.assertEqual(offsets("char s[3]\nu16 x\n"), [0, 4])
        self.assertEqual(schema.layout_from_schema("char s[3]\nu16 x\n").payload_bytes, 8)
        self.assertEqual(offsets("u32 a\nf64 b\n"), [0, 8])
        self.assertEqual(schema.layout_from_schema("u32 a\nf64 b\n").payload_bytes, 16)
        self.assertEqual(offsets("u8 a\nu16 b\nu8 c\nu32 d\n"), [0, 2, 4, 8])
        self.assertEqual(offsets("u8 a\ni16 b[3]\nu8 c\n"), [0, 2, 8])

    def test_rounding(self):
        for text, size in [("u8 a\n", 8), ("u8 a\nu8 b\nu8 c\n", 8), ("u32 a\nu32 b\nu32 c\n", 16),
                           ("u16 a[3]\n", 8), ("f32 x[3]\n", 16), ("u64 a\n", 8), ("char s[9]\n", 16)]:
            self.assertEqual(schema.layout_from_schema(text).payload_bytes, size, text)

    def test_params_layout(self):
        lay = schema.layout_from_schema(PARAMS_DOCS)
        self.assertEqual([f.offset for f in lay.fields], [0, 4, 8, 12, 24, 25, 44])
        self.assertEqual(lay.payload_bytes, 48)


class Canonical(unittest.TestCase):
    def test_equivalence(self):
        bare = schema.type_hash("f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n")
        self.assertEqual(bare, DEPTH_HASH)
        self.assertEqual(schema.type_hash(DEPTH_DOCS), bare)
        self.assertEqual(schema.type_hash("\n\nf32   depth_m\n\n\tf32 temp_c   # c\n f32  pressure_mbar\n# end"), bare)
        self.assertEqual(schema.type_hash("f32 depth_m = 1 [0, 10] @restart\nf32 temp_c = 2\nf32 pressure_mbar\n"), bare)
        self.assertEqual(schema.type_hash("f32 depth_m\r\nf32 temp_c\r\nf32 pressure_mbar\r\n"), bare)

    def test_difference(self):
        bare = DEPTH_HASH
        self.assertNotEqual(schema.type_hash("f32 depth\nf32 temp_c\nf32 pressure_mbar\n"), bare)
        self.assertNotEqual(schema.type_hash("f32 temp_c\nf32 depth_m\nf32 pressure_mbar\n"), bare)
        self.assertNotEqual(schema.type_hash("f64 depth_m\nf32 temp_c\nf32 pressure_mbar\n"), bare)
        self.assertNotEqual(schema.type_hash("f32 depth_m\nf32 temp_c\n"), bare)
        self.assertNotEqual(schema.type_hash("f32 depth_m[1]\nf32 temp_c\nf32 pressure_mbar\n"), bare)
        self.assertEqual(schema.canonical("u8 a[1]\n"), "u8 a[1]\n")


class Refusals(unittest.TestCase):
    def refuse(self, text, what, line=1, params=False):
        with self.assertRaises(SchemaError) as cm:
            schema.layout_from_schema(text, params=params)
        msg = str(cm.exception)
        where = "<schema>:%d: " % line if line else "<schema>: "
        self.assertTrue(msg.startswith(where), "%r: %s" % (text, msg))
        self.assertIn(what, msg, text)

    def test_names(self):
        self.refuse("f32 1abc\n", "not a C identifier")
        self.refuse("f32 a-b\n", "not a C identifier")
        self.refuse("f32 a\nu8 b\nf32 a\n", "duplicate field 'a'", line=3)
        self.refuse("f32 nav.depth\n", "dotted name")
        self.refuse("f32 _a\n", "reserved")
        self.refuse("f32\n", "missing field name")
        self.refuse("f32 é\n", "not a C identifier")

    def test_types(self):
        self.refuse("depth d\n", "unknown type 'depth'")
        self.refuse("struct d\n", "unknown type")
        self.refuse("f32 a[0]\n", "at least 1")
        self.refuse("f32 a[]\n", "array size")
        self.refuse("f32 a[x]\n", "array size")
        self.refuse("f32 a[3\n", "array size")
        self.refuse("char s\n", "char needs a size")
        self.refuse("f32 a foo\n", "unexpected 'foo'")
        self.refuse("", "no fields", line=0)
        self.refuse("# only a comment\n\n", "no fields", line=0)

    def test_attributes_need_params(self):
        self.refuse("f32 a = 1\n", "pass --params")
        self.refuse("f32 a [0, 1]\n", "pass --params")
        self.refuse("u32 a @restart\n", "pass --params")
        schema.layout_from_schema("f32 a = 1\n")  # accepted by default
        schema.layout_from_schema("f32 a = 1\n", params=True)

    def test_default_shapes(self):
        p = dict(params=True)
        self.refuse("f32 a[3] = 1, 2\n", "default has 2 values", **p)
        self.refuse("f32 a = 1, 2\n", "default has 2 values", **p)
        self.refuse("u8 a = 300\n", "does not fit u8", **p)
        self.refuse("u8 a = -1\n", "negative", **p)
        self.refuse("i8 a = 200\n", "does not fit i8", **p)
        self.refuse('u8 a = "x"\n', "not an integer", **p)
        self.refuse("f32 a = abc\n", "not a decimal number", **p)
        self.refuse("f32 a = 0x10\n", "not a decimal number", **p)
        self.refuse("f32 a = 1_0\n", "not a decimal number", **p)
        self.refuse("u8 a = 1_0\n", "not an integer", **p)
        self.refuse('char s[4] = "abcd"\n', "longer than s[4] holds", **p)
        self.refuse("char s[4] = abc\n", "quoted text", **p)
        self.refuse('char s[4] = "abc\n', "unterminated string", **p)
        self.refuse("f32 a =\n", "missing value", **p)
        self.refuse("f32 a = 1, 2, 3, 4\n", "default has 4 values", **p)

    def test_range_shapes(self):
        p = dict(params=True)
        self.refuse("char s[4] [0, 1]\n", "char field has no range", **p)
        self.refuse("f32 a [5, 1]\n", "min above max", **p)
        self.refuse("f32 a [1]\n", "expected [min, max]", **p)
        self.refuse("f32 a [1, 2, 3]\n", "expected [min, max]", **p)
        self.refuse("f32 a [1, 2\n", "unterminated range", **p)
        self.refuse("u8 a [0, 256]\n", "does not fit u8", **p)
        self.refuse("f32 a = 11 [0, 10]\n", "outside [0, 10]", **p)
        self.refuse("f32 a [0, 10] = 1\n", "default comes first", **p)
        self.refuse("u32 a @restart [0, 1]\n", "range comes before @restart", **p)
        self.refuse("u32 a @restart @restart\n", "given twice", **p)
        self.refuse("u32 a @reboot\n", "unexpected '@reboot'", **p)

    def test_line_numbers(self):
        self.refuse("# header\n\nf32 ok\n\n   u8 bad.name\n", "dotted name", line=5)

    def test_size_limit(self):
        big = "u8 a\n" + "#" * (3584 - 6) + "\n"
        self.assertEqual(len(big), 3584)
        schema.layout_from_schema(big)
        self.refuse(big[:-1] + "#\n", "at most 3584", line=0)

    def test_field_count_limit(self):
        many = "".join("u8 f%d\n" % i for i in range(128))
        lay = schema.layout_from_schema(many)
        self.assertEqual(len(lay.fields), 128)
        self.assertEqual(lay.payload_bytes, 128)
        self.refuse(many + "u8 f128\n", "more than 128 fields", line=129)

    def test_names_from_paths(self):
        self.assertEqual(schema.name_from_path("msg/depth.msg"), "depth")
        self.assertEqual(schema.name_from_path("depth.msg"), "depth")
        for bad in ["msg/nav.state.msg", "msg/depth.txt", "msg/.msg", "msg/1depth.msg", "msg/my-type.msg", "msg/_x.msg"]:
            with self.assertRaises(SchemaError):
                schema.name_from_path(bad)


class Params(unittest.TestCase):
    def test_parse_params(self):
        meta = {m.name: m for m in schema.parse_params(PARAMS_DOCS)}
        self.assertEqual(meta["kp"], ("kp", 1.5, 0.0, 10.0, False))
        self.assertEqual(meta["gains"].default, [1.0, 1.0, 0.5])
        self.assertIsNone(meta["gains"].min)
        self.assertEqual(meta["enabled"], ("enabled", 0, 0, 1, False))
        self.assertEqual(meta["frame"].default, "ned")
        self.assertEqual(meta["rate_hz"], ("rate_hz", 50, 10, 200, True))

    def test_defaults_when_absent(self):
        meta = {m.name: m for m in schema.parse_params("f32 a\nu8 b[3]\nchar s[4]\nf32 g[2] = 7\n")}
        self.assertEqual(meta["a"].default, 0)
        self.assertEqual(meta["b"].default, [0, 0, 0])
        self.assertEqual(meta["s"].default, "")
        self.assertEqual(meta["g"].default, [7.0, 7.0])

    def test_hash_in_quoted_default_is_not_a_comment(self):
        lay = schema.layout_from_schema('char s[8] = "a#b"   # the comment\n')
        self.assertEqual(lay.fields[0].comment, "the comment")
        self.assertEqual(schema.parse_params('char s[8] = "a#b"\n')[0].default, "a#b")


class MsgDirectory(unittest.TestCase):
    """Every checked-in .msg parses, and its generated .c carries the same hash."""

    def test_all_msg_files(self):
        files = sorted(glob.glob(os.path.join(REPO, "msg", "*.msg")))
        self.assertGreaterEqual(len(files), 19)
        for path in files:
            with open(path, "rb") as fp:
                data = fp.read()
            name = schema.name_from_path(path)
            lay = schema.layout_from_schema(data, file=path)
            self.assertEqual(lay.payload_bytes % 8, 0, path)
            h = schema.type_hash(data)
            cpath = path[:-4] + ".c"
            if os.path.exists(cpath):
                with open(cpath) as fp:
                    src = fp.read()
                self.assertIn(".hash = 0x%016xull" % h, src, path)
                self.assertIn(".payload_bytes = %d," % lay.payload_bytes, src, path)
                self.assertIn('.name = "%s"' % name, src, path)

    def test_envelopes(self):
        req = schema.layout_from_schema(open(os.path.join(REPO, "msg", "call_request_envelope.msg")).read())
        self.assertEqual(req.payload_bytes, 112)
        self.assertEqual({f.name: f.offset for f in req.fields},
                         {"call_id": 0, "deadline_ns": 8, "request_type": 16, "target_id": 24,
                          "reserved": 28, "method": 32, "caller": 64})
        rep = schema.layout_from_schema(open(os.path.join(REPO, "msg", "call_reply_envelope.msg")).read())
        self.assertEqual(rep.payload_bytes, 96)
        self.assertEqual({f.name: f.offset for f in rep.fields},
                         {"call_id": 0, "caller_id": 8, "status": 12, "reply_type": 16,
                          "received_ns": 24, "reason": 32})


class Dtype(unittest.TestCase):
    def test_dtype(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest("numpy not installed")
        dt = schema.dtype_from_schema(PARAMS_DOCS)
        self.assertEqual(dt.itemsize, 48)
        self.assertEqual(dt.names, ("kp", "ki", "max_thrust", "gains", "enabled", "frame", "rate_hz"))
        self.assertEqual([dt.fields[n][1] for n in dt.names], [0, 4, 8, 12, 24, 25, 44])
        self.assertEqual(dt.fields["gains"][0].shape, (3,))
        self.assertEqual(dt.fields["frame"][0].itemsize, 16)
        self.assertEqual(dt.fields["rate_hz"][0].str, "<u4")
        d2 = schema.dtype_from_schema(DEPTH_DOCS)
        self.assertEqual(d2.itemsize, 16)


if __name__ == "__main__":
    unittest.main()
