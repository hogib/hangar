// SPDX-License-Identifier: LGPL-3.0-or-later
/* Schema text to layout and hash. One pass over the lines; no allocation. */
#include "hg_type.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

uint64_t hg_fnv1a64(const void *data, size_t len)
{
	const unsigned char *p = data;
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

uint32_t hg_fnv1a32(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}
	return h;
}

static const struct {
	const char *name;
	uint32_t size;
} kinds[] = {
	[HG_U8] = { "u8", 1 },   [HG_I8] = { "i8", 1 },   [HG_U16] = { "u16", 2 },
	[HG_I16] = { "i16", 2 }, [HG_U32] = { "u32", 4 }, [HG_I32] = { "i32", 4 },
	[HG_U64] = { "u64", 8 }, [HG_I64] = { "i64", 8 }, [HG_F32] = { "f32", 4 },
	[HG_F64] = { "f64", 8 }, [HG_CHAR] = { "char", 1 },
};

uint32_t hg_field_elem_size(enum hg_field_kind kind)
{
	return kinds[kind].size;
}

static int is_name_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

static void set_err(char *err, size_t errlen, int line, const char *what)
{
	if (err && errlen)
		snprintf(err, errlen, "schema line %d: %s", line, what);
}

/* What one line says: a field (kind, count, name span, comment span). */
struct line {
	int kind;
	uint32_t count;
	const char *name;
	size_t name_len;
	const char *comment;
	size_t comment_len;
};

/* 1 if the line holds a field, 0 if it is blank or a comment, -1 on an
 * error. Everything after the name (a default, a range, @restart) is a
 * parameter attribute: accepted, since it is not part of the type, and not
 * checked here. The trailing comment is kept: it carries units. */
static int parse_line(const char *s, const char *end, struct line *l, const char **what)
{
	while (s < end && (*s == ' ' || *s == '\t' || *s == '\r'))
		s++;
	if (s == end || *s == '#')
		return 0;
	const char *t = s;
	while (s < end && *s != ' ' && *s != '\t' && *s != '#')
		s++;
	size_t tlen = (size_t)(s - t);
	l->kind = -1;
	for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
		if (strlen(kinds[k].name) == tlen && memcmp(kinds[k].name, t, tlen) == 0)
			l->kind = (int)k;
	if (l->kind < 0) {
		*what = "unknown field type";
		return -1;
	}
	while (s < end && (*s == ' ' || *s == '\t'))
		s++;
	l->name = s;
	while (s < end && is_name_char(*s))
		s++;
	l->name_len = (size_t)(s - l->name);
	if (l->name_len == 0 || l->name_len > HG_FIELD_NAME_MAX ||
	    (l->name[0] >= '0' && l->name[0] <= '9') || l->name[0] == '.' ||
	    l->name[l->name_len - 1] == '.') {
		*what = "bad field name";
		return -1;
	}
	l->count = 1;
	int is_array = 0;
	if (s < end && *s == '[') {
		s++;
		uint64_t v = 0;
		const char *d = s;
		while (s < end && *s >= '0' && *s <= '9') {
			v = v * 10 + (uint64_t)(*s - '0');
			if (v > 0xffffffffu) {
				*what = "array too large";
				return -1;
			}
			s++;
		}
		if (s == d || s == end || *s != ']' || v == 0) {
			*what = "bad array size";
			return -1;
		}
		s++;
		l->count = (uint32_t)v;
		is_array = 1;
	}
	if (l->kind == HG_CHAR && !is_array) {
		*what = "char needs a size: char name[N]";
		return -1;
	}
	if (s < end && !(*s == ' ' || *s == '\t' || *s == '\r' || *s == '#' || *s == '=' ||
			 *s == '[' || *s == '@')) {
		*what = "unexpected text after the field name";
		return -1;
	}
	const char *hash = memchr(s, '#', (size_t)(end - s));
	l->comment = NULL;
	l->comment_len = 0;
	if (hash) {
		hash++;
		while (hash < end && (*hash == ' ' || *hash == '\t'))
			hash++;
		const char *ce = end;
		while (ce > hash && (ce[-1] == ' ' || ce[-1] == '\t' || ce[-1] == '\r'))
			ce--;
		l->comment = hash;
		l->comment_len = (size_t)(ce - hash);
	}
	return 1;
}

/* The canonical line of a field: "type name[N]\n", [N] only for arrays. */
static size_t canon_line(const struct hg_field *f, char *buf, size_t len)
{
	if (f->count > 1 || f->kind == HG_CHAR)
		return (size_t)snprintf(buf, len, "%s %s[%u]\n", kinds[f->kind].name, f->name, f->count);
	return (size_t)snprintf(buf, len, "%s %s\n", kinds[f->kind].name, f->name);
}

int hg_schema_parse(const char *text, size_t len, struct hg_schema *out, char *err, size_t errlen)
{
	if (len > HG_SCHEMA_MAX) {
		set_err(err, errlen, 0, "schema longer than 3584 bytes");
		errno = EINVAL;
		return -1;
	}
	memset(&out->type, 0, sizeof out->type);
	memcpy(out->text, text, len);
	out->text[len] = 0;
	out->type.name = "";
	out->type.schema = out->text;
	out->type.schema_bytes = (uint32_t)len;
	out->type.fields = out->fields;
	size_t pool_used = 0;
	uint64_t h = 0xcbf29ce484222325ull;
	uint32_t off = 0;
	const char *s = out->text, *end = out->text + len;
	int line = 0;
	while (s < end) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		const char *le = nl ? nl : end;
		line++;
		const char *what = "";
		struct line l;
		int r = parse_line(s, le, &l, &what);
		s = nl ? nl + 1 : end;
		if (r < 0) {
			set_err(err, errlen, line, what);
			goto bad;
		}
		if (r == 0)
			continue;
		if (out->type.n_fields == HG_SCHEMA_MAX_FIELDS) {
			set_err(err, errlen, line, "too many fields");
			goto bad;
		}
		struct hg_field *f = &out->fields[out->type.n_fields];
		/* The pool holds every name and comment with its NUL; the text is
		 * at most HG_SCHEMA_MAX bytes, so it always fits. */
		f->name = out->pool + pool_used;
		memcpy(out->pool + pool_used, l.name, l.name_len);
		pool_used += l.name_len;
		out->pool[pool_used++] = 0;
		f->comment = out->pool + pool_used;
		if (l.comment_len)
			memcpy(out->pool + pool_used, l.comment, l.comment_len);
		pool_used += l.comment_len;
		out->pool[pool_used++] = 0;
		f->kind = (enum hg_field_kind)l.kind;
		f->count = l.count;
		/* Duplicate names would make the layout ambiguous to a reader. */
		for (uint32_t i = 0; i < out->type.n_fields; i++)
			if (strcmp(out->fields[i].name, f->name) == 0) {
				set_err(err, errlen, line, "duplicate field name");
				goto bad;
			}
		/* Natural alignment, padding explicit: the C layout. */
		uint32_t a = kinds[f->kind].size;
		off = (off + a - 1) & ~(a - 1);
		f->offset = off;
		off += a * f->count;
		char cl[HG_FIELD_NAME_MAX + 32];
		size_t cn = canon_line(f, cl, sizeof cl);
		for (size_t i = 0; i < cn; i++) {
			h ^= (unsigned char)cl[i];
			h *= 0x100000001b3ull;
		}
		out->type.n_fields++;
	}
	if (out->type.n_fields == 0) {
		set_err(err, errlen, line, "no fields");
		goto bad;
	}
	out->type.payload_bytes = (off + 7) & ~7u;
	out->type.hash = h;
	return 0;
bad:
	errno = EINVAL;
	return -1;
}

size_t hg_schema_canonical(const char *text, size_t len, char *out, size_t outlen)
{
	static struct hg_schema s; /* 12 KB: too much for the stack of a small module */
	if (hg_schema_parse(text, len, &s, NULL, 0) < 0)
		return (size_t)-1;
	size_t n = 0;
	for (uint32_t i = 0; i < s.type.n_fields; i++) {
		char cl[HG_FIELD_NAME_MAX + 32];
		size_t cn = canon_line(&s.fields[i], cl, sizeof cl);
		if (out && n < outlen) {
			size_t room = outlen - n;
			memcpy(out + n, cl, cn < room ? cn : room);
		}
		n += cn;
	}
	if (out && outlen)
		out[n < outlen ? n : outlen - 1] = 0;
	return n;
}

const struct hg_field *hg_type_field(const struct hg_type *t, const char *name)
{
	for (uint32_t i = 0; i < t->n_fields; i++)
		if (strcmp(t->fields[i].name, name) == 0)
			return &t->fields[i];
	return NULL;
}

int hg_frame_type(uint32_t width, uint32_t height, uint32_t fourcc, struct hg_frame_type *out)
{
	memset(out, 0, sizeof *out);
	/* The planes are laid out one after another, each row width bytes (half
	 * for subsampled chroma), with no row padding: the stride is what a
	 * reader needs to find row y, the offsets where each plane starts. */
	uint64_t total;
	if (width == 0 || height == 0 || width > 16384 || height > 16384) {
		errno = EINVAL;
		return -1;
	}
	switch (fourcc) {
	case HG_I422:
		if (width & 1)
			goto bad;
		out->stride[0] = width;
		out->stride[1] = out->stride[2] = width / 2;
		out->offset[1] = width * height;
		out->offset[2] = out->offset[1] + (width / 2) * height;
		total = (uint64_t)width * height * 2;
		break;
	case HG_I420:
		if ((width & 1) || (height & 1))
			goto bad;
		out->stride[0] = width;
		out->stride[1] = out->stride[2] = width / 2;
		out->offset[1] = width * height;
		out->offset[2] = out->offset[1] + (width / 2) * (height / 2);
		total = (uint64_t)width * height * 3 / 2;
		break;
	case HG_GREY:
		out->stride[0] = width;
		total = (uint64_t)width * height;
		break;
	default:
		goto bad;
	}
	out->data_bytes = (uint32_t)total;
	int n = snprintf(out->text, sizeof out->text,
			 "u32 width\nu32 height\nu32 fourcc\nu32 stride[3]\nu32 offset[3]\nu8 data[%u]\n",
			 out->data_bytes);
	/* The field table is written out rather than parsed: the names are
	 * literals, and a frame type is built on a hot-ish path (a camera
	 * renegotiating) where a 12 KB parse buffer would be out of place. */
	static const char *const names[6] = { "width", "height", "fourcc", "stride", "offset", "data" };
	static const uint32_t counts[6] = { 1, 1, 1, 3, 3, 0 };
	static const uint32_t offsets[6] = { 0, 4, 8, 12, 24, 36 };
	for (int i = 0; i < 6; i++) {
		out->fields[i].name = names[i];
		out->fields[i].kind = i < 5 ? HG_U32 : HG_U8;
		out->fields[i].count = i < 5 ? counts[i] : out->data_bytes;
		out->fields[i].offset = offsets[i];
		out->fields[i].comment = "";
	}
	out->type.name = "frame";
	out->type.schema = out->text;
	out->type.schema_bytes = (uint32_t)n;
	out->type.hash = hg_fnv1a64(out->text, (size_t)n); /* the text is already canonical */
	out->type.payload_bytes = (36 + out->data_bytes + 7) & ~7u;
	out->type.n_fields = 6;
	out->type.fields = out->fields;
	return 0;
bad:
	errno = EINVAL;
	return -1;
}
