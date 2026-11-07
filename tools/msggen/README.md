# msggen: `hangar msg gen`

The message-type generator. It reads `msg/<name>.msg` and writes `msg/<name>.h` (the
struct, with its padding made explicit, and a static assert for its size and for every
field's offset) and `msg/<name>.c` (the type descriptor: the schema text verbatim, the
field table, the type hash). It is a small C11 program with no dependencies, built for
the build machine (`HOSTCC`); nothing needs it at run time. `python/hangar/schema.py`
parses the same grammar in Python and must compute the same layout and hash.

```
msggen [--out <dir>] [--params] <file.msg>...      write <dir>/<name>.h and <name>.c
msggen --print-hash [--params] <file.msg>...      "<name> <hash-hex> <payload_bytes>" per file
msggen --check [--out <dir>] [--params] <file.msg>...
                                                  regenerate in memory; fail if <dir>'s differ
```

`<dir>` defaults to the directory the `.msg` file is in. Every refusal names the file
and the line.

## Grammar

A `.msg` file is a flat list of fixed-size fields, one per line, with `#` comments:

```
# msg/depth.msg
f32 depth_m         # m, positive down
f32 temp_c          # degrees Celsius
f32 pressure_mbar   # mbar, absolute
```

```
line     := [ field ] [ '#' comment ]
field    := type name [ '[' N ']' ] [ '=' default ] [ '[' min ',' max ']' ] [ '@restart' ]
type     := u8 | i8 | u16 | i16 | u32 | i32 | u64 | i64 | f32 | f64 | char
```

- `name` is a C identifier of at most 63 bytes, unique in the file. Names starting with
  `_` are refused: the generator's padding members and the manager's row fields
  (`_seq`) use them. Dotted names (`nav.state.depth_m`) are refused: only the dataset
  row types the manager generates carry dots ([docs/01](../../docs/01-topics.md#message-types)).
- `name[N]`, N ≥ 1, is a fixed array. `char name[N]` is text of at most N−1 bytes,
  NUL-padded; `char` always takes a size.
- The trailing comment is kept in the descriptor (`hg_field.comment`): it carries the
  field's units, which the recorder copies into datasets. A `#` inside a quoted default
  is not a comment.
- The three attributes belong to **parameter types** and are refused without
  `--params` ([docs/05](../../docs/05-parameters.md#parameter-types)): `= default` (a
  number; for an array one value or N comma-separated values; for `char` quoted text of
  at most N−1 bytes), `[min, max]` (inclusive, both numbers of the field's type, not for
  `char`; a default must lie within it), `@restart`, in that order. Numbers are decimal:
  what C's `strtod`/`strtoll` and Python's `float()`/`int()` both accept.
- The file is at most 3584 bytes, the topic header's schema field, and holds at most
  128 fields, the bound lib/hg's run-time parser shares (both fixed in the ABI file).
  It must have at least one field; the empty request or reply is `u8 reserved`.
- No nested types, no variable lengths, no pointers.

The type's name is the file's stem (`depth` for `msg/depth.msg`), with the same rules
as a field name. The C struct is `struct depth`, also `typedef`'d as `depth`
(docs/04 and docs/05 use the bare name); the descriptor is `DEPTH_TYPE`.

## Layout

The C layout of a struct with the fields in order: every scalar and array takes its
natural alignment (1, 2, 4 or 8 bytes, its element's size), and the struct's size is
rounded up to a multiple of 8. The generated struct spells the padding out
(`uint8_t _pad0[4];`), so `sizeof(struct depth)` is the payload size and the header's
`_Static_assert`s hold on every compiler; a compiler that laid the struct out
differently would fail the build, not corrupt a topic. Byte order is native, little-endian
on every supported target.

## Canonical form and hash

The hash covers the layout alone. The **canonical form** is one line per field,
`type name` or `type name[N]`, single spaces, each line ended by `\n`, nothing else:
no comments, blank lines, defaults, ranges or `@restart`. The **type hash** is FNV-1a
64 (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`) over the canonical bytes.
So renaming, reordering or retyping a field changes the hash; changing a comment or a
default does not.

Test vector: `msg/depth.msg` above has the canonical form
`"f32 depth_m\nf32 temp_c\nf32 pressure_mbar\n"` and the hash `f397a321cd6643a1`.
`tests/test_msggen.c`, `python/tests/test_schema.py` and lib/hg's own test all check it.

## Generated files, and keeping them in sync

The `.msg` files are the source. The `.h` and `.c` are generated **and checked in**, so
a build needs no generator and a reader of the tree sees the structs. Two Makefile
targets keep them honest:

- `make msg` regenerates every `msg/*.h` and `msg/*.c` (`msg/*_params.msg` with
  `--params`: a parameter type is named `<something>_params`, as the docs name them).
- `make check-msggen`, part of `make check`, runs the generator's suite, runs
  `msggen --check` over `msg/` (fails if a committed file is stale or missing), compares
  `msggen --print-hash` with `python3 -m hangar.schema --print-hash` for every file,
  and runs the Python unit tests.

Edit a `.msg`, run `make msg`, commit all three files together.

The suite compiles the generated `.c` files against `tests/hg_type.h`, a stand-in that
defines the three ABI types exactly as the ABI file shows them, so it does not depend
on lib/hg; lib/hg's `hg_type.h` and the stand-in are interchangeable.

## Parameter types

With `--params` the `.h` also declares `extern const struct hg_param_meta
<NAME>_META[n_fields]`, and the `.c` defines it: per field the default, min and max
**as written** (`""` if absent; a list for arrays, quoted text for `char`) and the
`@restart` flag. The struct is

```c
struct hg_param_meta { const char *dflt; const char *min; const char *max; uint8_t restart; };
```

defined in the generated header under `#ifndef HG_PARAM_META_DEFINED`, so lib/hg's
`hg_type.h` can take the definition over by defining that macro. The values are text
so that no precision is lost (a `u64` range does not fit a `double`) and so that
lib/hg's parameter code parses them with the same rules as the schema text in a topic's
header, which carries the attributes too.

## ABI

The descriptor the generated `.c` defines (`struct hg_type`, `struct hg_field`,
`enum hg_field_kind`), the canonical form and the layout rules are the contract in
[lib/hg/hg_type_abi.md](../../lib/hg/hg_type_abi.md), binding on this tool and on
lib/hg alike. Change it in both places or in neither.
