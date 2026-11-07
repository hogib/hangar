// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for lib/hg/hg_type.h, used only to compile the generated msg/*.c
// in the msggen test suite without depending on lib/hg. It defines the three
// ABI types exactly as lib/hg/hg_type_abi.md shows them, so the two headers
// are interchangeable; if the ABI changes, change both.
#ifndef MSGGEN_TEST_HG_TYPE_H
#define MSGGEN_TEST_HG_TYPE_H
#include <stdint.h>

enum hg_field_kind { HG_U8, HG_I8, HG_U16, HG_I16, HG_U32, HG_I32, HG_U64, HG_I64,
                     HG_F32, HG_F64, HG_CHAR };

struct hg_field {
    const char *name;        // as written in the .msg
    enum hg_field_kind kind;
    uint32_t count;          // 1 for a scalar; N for name[N]; N for char name[N]
    uint32_t offset;         // byte offset in the payload (natural alignment)
    const char *comment;     // the trailing comment, "" if none (units travel with it)
};

struct hg_type {
    const char *name;        // the .msg file's stem: "depth"
    const char *schema;      // the .msg text verbatim, comments included (goes in the header)
    uint32_t schema_bytes;
    uint64_t hash;           // FNV-1a 64 of the canonical form
    uint32_t payload_bytes;  // struct size rounded up to 8
    uint32_t n_fields;
    const struct hg_field *fields;
};

#endif
