// SPDX-License-Identifier: LGPL-3.0-or-later
/* Message types: the .msg schema text, its canonical form and hash, the
 * field layout a reader without generated code works from, and the frame
 * types built at run time (docs/01-topics.md "Message types").
 *
 * enum hg_field_kind, struct hg_field and struct hg_type are the contract
 * with the generator (lib/hg/hg_type_abi.md): generated headers depend on
 * these three definitions and nothing else here. Change them there and here
 * together, or not at all. */
#ifndef HG_TYPE_H
#define HG_TYPE_H

#include <stddef.h>
#include <stdint.h>

enum hg_field_kind { HG_U8, HG_I8, HG_U16, HG_I16, HG_U32, HG_I32, HG_U64, HG_I64,
		     HG_F32, HG_F64, HG_CHAR };

struct hg_field {
	const char *name;        /* as written in the .msg */
	enum hg_field_kind kind;
	uint32_t count;          /* 1 for a scalar; N for name[N]; N for char name[N] */
	uint32_t offset;         /* byte offset in the payload (natural alignment) */
	const char *comment;     /* the trailing comment, "" if none (units travel with it) */
};

struct hg_type {
	const char *name;        /* the .msg file's stem: "depth" */
	const char *schema;      /* the .msg text verbatim, comments included (goes in the header) */
	uint32_t schema_bytes;
	uint64_t hash;           /* FNV-1a 64 of the canonical form */
	uint32_t payload_bytes;  /* struct size rounded up to 8 */
	uint32_t n_fields;
	const struct hg_field *fields;
};
typedef struct hg_type hg_type;

/* The attributes of a parameter type's field (docs/05 "Parameter types"),
 * as the generator emits them (tools/msggen/README.md): the default, the
 * range and the @restart mark, as written in the .msg. The generated
 * headers define the same struct unless this macro says it is here. */
#ifndef HG_PARAM_META_DEFINED
#define HG_PARAM_META_DEFINED
struct hg_param_meta {
	const char *dflt;     /* "" if none; a list for arrays, quoted text for char */
	const char *min;      /* "" if no range */
	const char *max;
	uint8_t restart;      /* 1: changing it restarts the module */
};
#endif

#define HG_SCHEMA_MAX 3584       /* the header's schema field */
#define HG_SCHEMA_MAX_FIELDS 128
#define HG_FIELD_NAME_MAX 63     /* hg_type_abi.md; dotted dataset names fit too */

/* A type parsed at run time from the schema in a topic's header, with the
 * storage its descriptor points into: the text, the field table and the
 * names and comments. The descriptor is `type`; nothing is allocated. */
struct hg_schema {
	struct hg_type type;
	struct hg_field fields[HG_SCHEMA_MAX_FIELDS];
	char text[HG_SCHEMA_MAX + 1];
	char pool[HG_SCHEMA_MAX + 2 * HG_SCHEMA_MAX_FIELDS + 2];
};

/* Parses schema text (len bytes, need not be NUL-terminated, at most
 * HG_SCHEMA_MAX) into *out. Parameter attributes (= default, [min, max],
 * @restart) are accepted and ignored, as the hash ignores them. 0 on
 * success; -1 with errno EINVAL and, if err is given, one line saying what
 * was wrong. The descriptor's name is "" (the text has no stem). */
int hg_schema_parse(const char *text, size_t len, struct hg_schema *out, char *err, size_t errlen);

/* The canonical form: one "type name[N]\n" per field. Returns the length it
 * needs (without the NUL), writing at most outlen bytes of it, NUL-terminated
 * when they fit; (size_t)-1 if the text does not parse. */
size_t hg_schema_canonical(const char *text, size_t len, char *out, size_t outlen);

/* The field named name, or NULL. */
const struct hg_field *hg_type_field(const struct hg_type *t, const char *name);
/* The element size of a kind (1, 2, 4 or 8); a field takes count times it. */
uint32_t hg_field_elem_size(enum hg_field_kind kind);

uint64_t hg_fnv1a64(const void *data, size_t len);
uint32_t hg_fnv1a32(const char *s);   /* of a NUL-terminated name: the topic id */

/* Frames. A frame type is built for one size and format, its schema written
 * with literal sizes, so a reader expecting 720p cannot attach to 480p. */
#define HG_FOURCC(a, b, c, d) \
	((uint32_t)(unsigned char)(a) | ((uint32_t)(unsigned char)(b) << 8) | \
	 ((uint32_t)(unsigned char)(c) << 16) | ((uint32_t)(unsigned char)(d) << 24))
#define HG_I422 HG_FOURCC('I', '4', '2', '2')   /* planar YUV 4:2:2, 2 bytes per pixel */
#define HG_I420 HG_FOURCC('I', '4', '2', '0')   /* planar YUV 4:2:0, 1.5 bytes per pixel */
#define HG_GREY HG_FOURCC('G', 'R', 'E', 'Y')   /* one luma plane */

/* The header a frame payload starts with; data follows at offset 40. */
struct hg_frame_header {
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint32_t stride[3];
	uint32_t offset[3];
};

struct hg_frame_type {
	struct hg_type type;     /* .schema and .fields point into this struct */
	struct hg_field fields[6];
	char text[160];
	uint32_t data_bytes;
	uint32_t stride[3];
	uint32_t offset[3];
};

/* 0 on success; -1 with errno EINVAL for a size of 0, an unknown fourcc, or
 * odd dimensions where the format subsamples. */
int hg_frame_type(uint32_t width, uint32_t height, uint32_t fourcc, struct hg_frame_type *out);

#endif
