// SPDX-License-Identifier: LGPL-3.0-or-later
/* Parameters (docs/05-parameters.md): the grammar of values, ranges and
 * the saved file, then the module side: loading at start, the
 * `<module>.params` topic, and `set_params` served inside hg_serve(). */
#define _GNU_SOURCE
#include "hg_params.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hg_call.h"
#include "hg_control.h"
#include "hg_topic.h"

/* snprintf through a va_list: a bounded copy that may truncate is the
 * intent everywhere a name goes into a fixed field, and this keeps the
 * compiler from flagging each one (hangard's mf_fmt does the same). */
static int bfmt(char *dst, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int bfmt(char *dst, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, len, fmt, ap);
	va_end(ap);
	return n;
}

/* ---- Attributes --------------------------------------------------------- */

static void set_err(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
	if (!err || !errlen)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static const char *skip_blank(const char *s, const char *end)
{
	while (s < end && (*s == ' ' || *s == '\t' || *s == '\r'))
		s++;
	return s;
}

static const char *trim_end(const char *s, const char *e)
{
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		e--;
	return e;
}

/* The end of a quoted default: the closing quote, escapes skipped. */
static const char *quoted_end(const char *s, const char *end)
{
	if (s >= end || *s != '"')
		return NULL;
	for (s++; s < end; s++) {
		if (*s == '\\' && s + 1 < end) {
			s++;
			continue;
		}
		if (*s == '"')
			return s + 1;
	}
	return NULL;
}

int hg_params_attrs(const struct hg_type *t, struct hg_param_attr *out, char *err, size_t errlen)
{
	const char *s = t->schema, *end = t->schema + t->schema_bytes;
	uint32_t fi = 0;
	int line = 0;
	while (s < end && fi < t->n_fields) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		const char *le = nl ? nl : end;
		line++;
		const char *p = skip_blank(s, le);
		s = nl ? nl + 1 : end;
		if (p == le || *p == '#')
			continue;
		/* The type and the name: hg_schema_parse has checked them. */
		while (p < le && *p != ' ' && *p != '\t')
			p++;
		p = skip_blank(p, le);
		while (p < le && *p != ' ' && *p != '\t' && *p != '=' && *p != '#' && *p != '@' &&
		       !(*p == '[' && p > t->schema && (p[-1] == ' ' || p[-1] == '\t')))
			p++; /* the name, with its [N] */
		struct hg_param_attr *a = &out[fi];
		memset(a, 0, sizeof *a);
		a->dflt = a->min = a->max = "";
		const struct hg_field *f = &t->fields[fi];
		p = skip_blank(p, le);
		if (p < le && *p == '=') {
			p = skip_blank(p + 1, le);
			const char *v = p;
			if (f->kind == HG_CHAR) {
				const char *qe = quoted_end(p, le);
				if (!qe) {
					set_err(err, errlen, "schema line %d: %s: a char default is quoted text", line, f->name);
					goto bad;
				}
				p = qe;
			} else {
				while (p < le && *p != '[' && *p != '#' && *p != '@')
					p++;
			}
			a->dflt = v;
			a->dflt_len = (size_t)(trim_end(v, p) - v);
			if (!a->dflt_len) {
				set_err(err, errlen, "schema line %d: %s: empty default", line, f->name);
				goto bad;
			}
		}
		p = skip_blank(p, le);
		if (p < le && *p == '[') {
			if (f->kind == HG_CHAR) {
				set_err(err, errlen, "schema line %d: %s: no range for char", line, f->name);
				goto bad;
			}
			const char *close = memchr(p, ']', (size_t)(le - p));
			const char *comma = memchr(p, ',', (size_t)(le - p));
			if (!close || !comma || comma > close) {
				set_err(err, errlen, "schema line %d: %s: a range is [min, max]", line, f->name);
				goto bad;
			}
			const char *lo = skip_blank(p + 1, comma), *hi = skip_blank(comma + 1, close);
			a->min = lo;
			a->min_len = (size_t)(trim_end(lo, comma) - lo);
			a->max = hi;
			a->max_len = (size_t)(trim_end(hi, close) - hi);
			if (!a->min_len || !a->max_len) {
				set_err(err, errlen, "schema line %d: %s: a range is [min, max]", line, f->name);
				goto bad;
			}
			p = skip_blank(close + 1, le);
		}
		if (p < le && *p == '@') {
			if (le - p >= 8 && strncmp(p, "@restart", 8) == 0) {
				a->restart = 1;
				p = skip_blank(p + 8, le);
			} else {
				set_err(err, errlen, "schema line %d: %s: unknown mark", line, f->name);
				goto bad;
			}
		}
		if (p < le && *p != '#') {
			set_err(err, errlen, "schema line %d: %s: unexpected text after the attributes", line, f->name);
			goto bad;
		}
		fi++;
	}
	if (fi != t->n_fields) {
		set_err(err, errlen, "the schema text has %u field lines, the type %u", fi, t->n_fields);
		goto bad;
	}
	return 0;
bad:
	errno = EINVAL;
	return -1;
}

/* ---- Values ------------------------------------------------------------- */

static int is_float(enum hg_field_kind k)
{
	return k == HG_F32 || k == HG_F64;
}

static int is_signed(enum hg_field_kind k)
{
	return k == HG_I8 || k == HG_I16 || k == HG_I32 || k == HG_I64;
}

/* One number of the field's kind from text [s, e). */
static int parse_number(enum hg_field_kind k, const char *s, const char *e, unsigned char *dst, char *err,
			size_t errlen, const char *name)
{
	char buf[64];
	size_t n = (size_t)(e - s);
	if (n == 0 || n >= sizeof buf) {
		set_err(err, errlen, "%s: %s", name, n ? "number too long" : "empty value");
		return -1;
	}
	memcpy(buf, s, n);
	buf[n] = 0;
	char *tail;
	errno = 0;
	if (is_float(k)) {
		double v = strtod(buf, &tail);
		if (*tail || errno == ERANGE) {
			set_err(err, errlen, "%s: '%s' is not a number", name, buf);
			return -1;
		}
		if (k == HG_F32) {
			float f = (float)v;
			memcpy(dst, &f, 4);
		} else {
			memcpy(dst, &v, 8);
		}
		return 0;
	}
	if (is_signed(k)) {
		long long v = strtoll(buf, &tail, 10);
		if (*tail || errno == ERANGE) {
			set_err(err, errlen, "%s: '%s' is not an integer", name, buf);
			return -1;
		}
		long long lo = k == HG_I8 ? -128 : k == HG_I16 ? -32768 : k == HG_I32 ? -2147483648LL : LLONG_MIN;
		long long hi = k == HG_I8 ? 127 : k == HG_I16 ? 32767 : k == HG_I32 ? 2147483647LL : LLONG_MAX;
		if (v < lo || v > hi) {
			set_err(err, errlen, "%s: %s does not fit the field's type", name, buf);
			return -1;
		}
		switch (k) {
		case HG_I8: { int8_t x = (int8_t)v; memcpy(dst, &x, 1); break; }
		case HG_I16: { int16_t x = (int16_t)v; memcpy(dst, &x, 2); break; }
		case HG_I32: { int32_t x = (int32_t)v; memcpy(dst, &x, 4); break; }
		default: { int64_t x = (int64_t)v; memcpy(dst, &x, 8); break; }
		}
		return 0;
	}
	if (buf[0] == '-') {
		set_err(err, errlen, "%s: %s is negative; the field is unsigned", name, buf);
		return -1;
	}
	unsigned long long v = strtoull(buf, &tail, 10);
	if (*tail || errno == ERANGE) {
		set_err(err, errlen, "%s: '%s' is not an integer", name, buf);
		return -1;
	}
	unsigned long long hi = k == HG_U8 ? 255u : k == HG_U16 ? 65535u : k == HG_U32 ? 4294967295ull : ULLONG_MAX;
	if (v > hi) {
		set_err(err, errlen, "%s: %s does not fit the field's type", name, buf);
		return -1;
	}
	switch (k) {
	case HG_U8: { uint8_t x = (uint8_t)v; memcpy(dst, &x, 1); break; }
	case HG_U16: { uint16_t x = (uint16_t)v; memcpy(dst, &x, 2); break; }
	case HG_U32: { uint32_t x = (uint32_t)v; memcpy(dst, &x, 4); break; }
	default: { uint64_t x = (uint64_t)v; memcpy(dst, &x, 8); break; }
	}
	return 0;
}

int hg_param_parse(const struct hg_field *f, const char *text, size_t len, unsigned char *dst, char *err,
		   size_t errlen)
{
	const char *s = skip_blank(text, text + len), *e = trim_end(s, text + len);
	unsigned char *field = dst + f->offset;
	if (f->kind == HG_CHAR) {
		const char *qe = quoted_end(s, e);
		if (!qe || qe != e) {
			set_err(err, errlen, "%s: text is written in double quotes", f->name);
			errno = EINVAL;
			return -1;
		}
		char tmp[HG_SCHEMA_MAX];
		size_t n = 0;
		for (const char *p = s + 1; p < qe - 1 && n < sizeof tmp; p++) {
			if (*p == '\\' && p + 1 < qe - 1) {
				p++;
				tmp[n++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
			} else {
				tmp[n++] = *p;
			}
		}
		if (n >= f->count) {
			set_err(err, errlen, "%s: text of %zu bytes; at most %u", f->name, n, f->count - 1);
			errno = EINVAL;
			return -1;
		}
		memset(field, 0, f->count);
		memcpy(field, tmp, n);
		return 0;
	}
	uint32_t es = hg_field_elem_size(f->kind);
	/* The list: one value for every element, or exactly count values. */
	const char *parts[HG_SCHEMA_MAX / 2];
	size_t plen[HG_SCHEMA_MAX / 2];
	uint32_t k = 0;
	const char *p = s;
	while (p <= e && k < sizeof parts / sizeof parts[0]) {
		const char *c = memchr(p, ',', (size_t)(e - p));
		const char *pe = c ? c : e;
		const char *vs = skip_blank(p, pe), *ve = trim_end(vs, pe);
		parts[k] = vs;
		plen[k] = (size_t)(ve - vs);
		k++;
		if (!c)
			break;
		p = c + 1;
	}
	if (k != 1 && k != f->count) {
		set_err(err, errlen, "%s: %u values for %u elements", f->name, k, f->count);
		errno = EINVAL;
		return -1;
	}
	unsigned char tmp[8 * 4096];
	if ((size_t)es * f->count > sizeof tmp) {
		set_err(err, errlen, "%s: too large", f->name);
		errno = EINVAL;
		return -1;
	}
	for (uint32_t i = 0; i < f->count; i++) {
		uint32_t j = k == 1 ? 0 : i;
		char nm[80];
		if (f->count > 1)
			bfmt(nm, sizeof nm, "%s[%u]", f->name, i);
		else
			bfmt(nm, sizeof nm, "%s", f->name);
		if (parse_number(f->kind, parts[j], parts[j] + plen[j], tmp + (size_t)i * es, err, errlen, nm) < 0) {
			errno = EINVAL;
			return -1;
		}
	}
	memcpy(field, tmp, (size_t)es * f->count);
	return 0;
}

static int fmt_number(enum hg_field_kind k, const unsigned char *p, char *out, size_t outlen)
{
	switch (k) {
	case HG_U8: return bfmt(out, outlen, "%u", *p);
	case HG_I8: return bfmt(out, outlen, "%d", *(const signed char *)p);
	case HG_U16: { uint16_t v; memcpy(&v, p, 2); return bfmt(out, outlen, "%u", v); }
	case HG_I16: { int16_t v; memcpy(&v, p, 2); return bfmt(out, outlen, "%d", v); }
	case HG_U32: { uint32_t v; memcpy(&v, p, 4); return bfmt(out, outlen, "%u", v); }
	case HG_I32: { int32_t v; memcpy(&v, p, 4); return bfmt(out, outlen, "%d", v); }
	case HG_U64: { uint64_t v; memcpy(&v, p, 8); return bfmt(out, outlen, "%llu", (unsigned long long)v); }
	case HG_I64: { int64_t v; memcpy(&v, p, 8); return bfmt(out, outlen, "%lld", (long long)v); }
	case HG_F32: {
		/* The shortest decimal that reads back as the same float: 1.5 is
		 * "1.5", not "1.50000000"; 0.1f reads back from "0.1". */
		float v;
		memcpy(&v, p, 4);
		char tmp[48];
		for (int prec = 6; prec <= 9; prec++) {
			bfmt(tmp, sizeof tmp, "%.*g", prec, (double)v);
			if (prec == 9 || (float)strtod(tmp, NULL) == v)
				break;
		}
		return bfmt(out, outlen, "%s", tmp);
	}
	case HG_F64: {
		double v;
		memcpy(&v, p, 8);
		char tmp[48];
		for (int prec = 15; prec <= 17; prec++) {
			bfmt(tmp, sizeof tmp, "%.*g", prec, v);
			if (prec == 17 || strtod(tmp, NULL) == v)
				break;
		}
		return bfmt(out, outlen, "%s", tmp);
	}
	default: return bfmt(out, outlen, "?");
	}
}

int hg_param_format(const struct hg_field *f, const unsigned char *src, char *out, size_t outlen)
{
	const unsigned char *p = src + f->offset;
	size_t n = 0;
#define PUT(...)                                                                                     \
	do {                                                                                         \
		int w_ = bfmt(out + (n < outlen ? n : outlen), n < outlen ? outlen - n : 0, __VA_ARGS__); \
		if (w_ > 0)                                                                          \
			n += (size_t)w_;                                                             \
	} while (0)
	if (f->kind == HG_CHAR) {
		PUT("\"");
		for (uint32_t i = 0; i < f->count && p[i]; i++) {
			if (p[i] == '"' || p[i] == '\\')
				PUT("\\%c", p[i]);
			else if (p[i] == '\n')
				PUT("\\n");
			else
				PUT("%c", p[i]);
		}
		PUT("\"");
		return (int)n;
	}
	uint32_t es = hg_field_elem_size(f->kind);
	for (uint32_t i = 0; i < f->count; i++) {
		char num[48];
		fmt_number(f->kind, p + (size_t)i * es, num, sizeof num);
		PUT("%s%s", i ? ", " : "", num);
	}
#undef PUT
	return (int)n;
}

int hg_param_check_range(const struct hg_field *f, const struct hg_param_attr *a, const unsigned char *src,
			 char *reason, size_t rlen)
{
	if (!a->min_len || f->kind == HG_CHAR)
		return 0;
	unsigned char lo[8], hi[8];
	char err[64];
	if (parse_number(f->kind, a->min, a->min + a->min_len, lo, err, sizeof err, f->name) < 0 ||
	    parse_number(f->kind, a->max, a->max + a->max_len, hi, err, sizeof err, f->name) < 0) {
		set_err(reason, rlen, "%s: bad range in the type", f->name);
		return -1;
	}
	uint32_t es = hg_field_elem_size(f->kind);
	for (uint32_t i = 0; i < f->count; i++) {
		const unsigned char *p = src + f->offset + (size_t)i * es;
		int below = 0, above = 0;
		if (is_float(f->kind)) {
			double v, l, h;
			if (f->kind == HG_F32) {
				float x;
				memcpy(&x, p, 4); v = x;
				memcpy(&x, lo, 4); l = x;
				memcpy(&x, hi, 4); h = x;
			} else {
				memcpy(&v, p, 8);
				memcpy(&l, lo, 8);
				memcpy(&h, hi, 8);
			}
			below = v < l || isnan(v);
			above = v > h;
		} else if (is_signed(f->kind)) {
			int64_t v = 0, l = 0, h = 0;
			switch (f->kind) {
			case HG_I8: v = *(const int8_t *)p; l = *(const int8_t *)lo; h = *(const int8_t *)hi; break;
			case HG_I16: { int16_t x; memcpy(&x, p, 2); v = x; memcpy(&x, lo, 2); l = x; memcpy(&x, hi, 2); h = x; break; }
			case HG_I32: { int32_t x; memcpy(&x, p, 4); v = x; memcpy(&x, lo, 4); l = x; memcpy(&x, hi, 4); h = x; break; }
			default: memcpy(&v, p, 8); memcpy(&l, lo, 8); memcpy(&h, hi, 8); break;
			}
			below = v < l;
			above = v > h;
		} else {
			uint64_t v = 0, l = 0, h = 0;
			switch (f->kind) {
			case HG_U8: v = *p; l = *lo; h = *hi; break;
			case HG_U16: { uint16_t x; memcpy(&x, p, 2); v = x; memcpy(&x, lo, 2); l = x; memcpy(&x, hi, 2); h = x; break; }
			case HG_U32: { uint32_t x; memcpy(&x, p, 4); v = x; memcpy(&x, lo, 4); l = x; memcpy(&x, hi, 4); h = x; break; }
			default: memcpy(&v, p, 8); memcpy(&l, lo, 8); memcpy(&h, hi, 8); break;
			}
			below = v < l;
			above = v > h;
		}
		if (below || above) {
			char val[48];
			fmt_number(f->kind, p, val, sizeof val);
			if (f->count > 1)
				set_err(reason, rlen, "%s[%u] %s %s %.*s", f->name, i, val, below ? "below" : "above",
					below ? (int)a->min_len : (int)a->max_len, below ? a->min : a->max);
			else
				set_err(reason, rlen, "%s %s %s %.*s", f->name, val, below ? "below" : "above",
					below ? (int)a->min_len : (int)a->max_len, below ? a->min : a->max);
			return -1;
		}
	}
	return 0;
}

int hg_params_defaults(const struct hg_type *t, const struct hg_param_attr *attrs, unsigned char *dst,
		       char *err, size_t errlen)
{
	memset(dst, 0, t->payload_bytes);
	for (uint32_t i = 0; i < t->n_fields; i++) {
		const struct hg_param_attr *a = &attrs[i];
		if (!a->dflt_len)
			continue;
		if (hg_param_parse(&t->fields[i], a->dflt, a->dflt_len, dst, err, errlen) < 0)
			return -1;
	}
	return 0;
}

/* ---- The saved file ------------------------------------------------------ */

int hg_params_file_parse(const struct hg_type *t, const char *text, size_t len, unsigned char *dst,
			 struct hg_params_header *hdr, unsigned char *present, int require_header,
			 char *err, size_t errlen)
{
	memset(hdr, 0, sizeof *hdr);
	bfmt(hdr->changed_by, sizeof hdr->changed_by, "saved");
	if (present)
		memset(present, 0, t->n_fields);
	const char *s = text, *end = text + len;
	int line = 0, seen_header = 0;
	while (s < end) {
		const char *nl = memchr(s, '\n', (size_t)(end - s));
		const char *le = nl ? nl : end;
		line++;
		const char *p = skip_blank(s, le);
		const char *pe = trim_end(p, le);
		s = nl ? nl + 1 : end;
		if (line == 1 && require_header) {
			static const char head[] = "# hangar parameters: ";
			size_t hl = sizeof head - 1;
			if ((size_t)(pe - p) <= hl || strncmp(p, head, hl) != 0 ||
			    (size_t)(pe - p - hl) != strlen(t->name) || strncmp(p + hl, t->name, strlen(t->name)) != 0) {
				set_err(err, errlen, "line 1: expected '# hangar parameters: %s'", t->name);
				goto bad;
			}
			seen_header = 1;
			continue;
		}
		if (p == pe)
			continue;
		if (*p == '#') {
			if ((size_t)(pe - p) > 10 && strncmp(p, "# version ", 10) == 0) {
				/* # version N, set by X, <time>[, restart] */
				char buf[256];
				bfmt(buf, sizeof buf, "%.*s", (int)(pe - p - 10), p + 10);
				char *tail;
				unsigned long long v = strtoull(buf, &tail, 10);
				if (tail == buf) {
					set_err(err, errlen, "line %d: bad version line", line);
					goto bad;
				}
				hdr->version = v;
				hdr->have_version = 1;
				const char *by = strstr(tail, "set by ");
				if (by) {
					by += 7;
					const char *c = strchr(by, ',');
					bfmt(hdr->changed_by, sizeof hdr->changed_by, "%.*s", c ? (int)(c - by) : (int)strlen(by), by);
				}
				const char *last = strrchr(buf, ',');
				if (last && strcmp(skip_blank(last + 1, buf + strlen(buf)), "restart") == 0)
					hdr->restart_pending = 1;
			}
			continue;
		}
		const char *eq = memchr(p, '=', (size_t)(pe - p));
		if (!eq) {
			set_err(err, errlen, "line %d: expected 'name = value'", line);
			goto bad;
		}
		const char *ne = trim_end(p, eq);
		char name[HG_FIELD_NAME_MAX + 2];
		if (ne - p == 0 || (size_t)(ne - p) > HG_FIELD_NAME_MAX) {
			set_err(err, errlen, "line %d: bad field name", line);
			goto bad;
		}
		bfmt(name, sizeof name, "%.*s", (int)(ne - p), p);
		const struct hg_field *f = hg_type_field(t, name);
		if (!f) {
			set_err(err, errlen, "line %d: unknown field %s", line, name);
			goto bad;
		}
		uint32_t idx = (uint32_t)(f - t->fields);
		if (present && present[idx]) {
			set_err(err, errlen, "line %d: %s given twice", line, name);
			goto bad;
		}
		char verr[128];
		if (hg_param_parse(f, eq + 1, (size_t)(pe - eq - 1), dst, verr, sizeof verr) < 0) {
			set_err(err, errlen, "line %d: %s", line, verr);
			goto bad;
		}
		if (present)
			present[idx] = 1;
	}
	if (require_header && !seen_header) {
		set_err(err, errlen, "empty file: no '# hangar parameters: %s' line", t->name);
		goto bad;
	}
	return 0;
bad:
	errno = EINVAL;
	return -1;
}

static void utc_now(char *out, size_t len)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	gmtime_r(&ts.tv_sec, &tm);
	strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int hg_params_file_format(const struct hg_type *t, const unsigned char *src, const struct hg_params_header *hdr,
			  char *out, size_t outlen)
{
	size_t n = 0;
	char when[40];
	utc_now(when, sizeof when);
#define PUT(...)                                                                                     \
	do {                                                                                         \
		int w_ = bfmt(out + (n < outlen ? n : outlen), n < outlen ? outlen - n : 0, __VA_ARGS__); \
		if (w_ > 0)                                                                          \
			n += (size_t)w_;                                                             \
	} while (0)
	PUT("# hangar parameters: %s\n", t->name);
	PUT("# version %llu, set by %s, %s%s\n", (unsigned long long)hdr->version, hdr->changed_by, when,
	    hdr->restart_pending ? ", restart" : "");
	for (uint32_t i = 0; i < t->n_fields; i++) {
		char val[HG_SCHEMA_MAX];
		hg_param_format(&t->fields[i], src, val, sizeof val);
		PUT("%s = %s\n", t->fields[i].name, val);
	}
#undef PUT
	return (int)n;
}

/* ---- The module -------------------------------------------------------- */

static struct {
	const struct hg_type *type;
	unsigned char *P;              /* the module's struct: the values in force */
	unsigned char *cand;           /* a candidate, checked before it is applied */
	unsigned char *topic;          /* one <module>.params message */
	hg_params_validate validate;
	void *ctx;
	struct hg_param_attr attrs[HG_SCHEMA_MAX_FIELDS];
	struct hg_schema topic_schema;
	hg_pub *pub;
	uint64_t version;
	char changed_by[HG_PARAMS_CHANGED_BY];
	int restart_pending;
	int want_restart;              /* a restart to ask the manager for, after the reply */
	int changed;
	char path[512];
	char module[32];
	int open;
} M;

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("hg_params: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* The saved parameters are refused: the reason on the status line, exit
 * 78, and the manager leaves the module failed (docs/05 "Saved values"). */
static void fail78(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void fail78(const char *fmt, ...)
{
	char line[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	hg_status(line);
	log_line("%s: refusing to start", line);
	fflush(stderr);
	exit(78);
}

/* Written to a temporary file and renamed into place; no fsync (docs/05:
 * a sync on an SD card can outlast a control loop period). */
static int save_file(const unsigned char *values, const struct hg_params_header *hdr)
{
	static char text[HG_SCHEMA_MAX * 4];
	int n = hg_params_file_format(M.type, values, hdr, text, sizeof text);
	if (n < 0 || n >= (int)sizeof text) {
		errno = EMSGSIZE;
		return -1;
	}
	char tmp[560];
	bfmt(tmp, sizeof tmp, "%s.tmp.%ld", M.path, (long)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
	if (fd < 0)
		return -1;
	ssize_t w = write(fd, text, (size_t)n);
	if (w != n || close(fd) < 0) {
		int e = w < 0 ? errno : EIO;
		close(fd);
		unlink(tmp);
		errno = e;
		return -1;
	}
	if (rename(tmp, M.path) < 0) {
		int e = errno;
		unlink(tmp);
		errno = e;
		return -1;
	}
	return 0;
}

static void publish_params(const struct hg_input *ref)
{
	uint64_t now = hg_now_ns();
	memset(M.topic, 0, 64);
	memcpy(M.topic, &M.version, 8);
	memcpy(M.topic + 8, M.changed_by, strnlen(M.changed_by, 47));
	M.topic[56] = (unsigned char)M.restart_pending;
	memcpy(M.topic + 64, M.P, M.type->payload_bytes);
	size_t len = 64 + M.type->payload_bytes;
	if (!ref || hg_publish_with(M.pub, M.topic, len, now, 0, ref, 1) < 0)
		hg_publish(M.pub, M.topic, len, now);
}

static int check_ranges(const unsigned char *values, char *reason, size_t rlen)
{
	for (uint32_t i = 0; i < M.type->n_fields; i++)
		if (hg_param_check_range(&M.type->fields[i], &M.attrs[i], values, reason, rlen) < 0)
			return -1;
	return 0;
}

/* set_params, inside hg_serve(): docs/05 "Setting parameters", steps 2 to
 * 6 (the Allow check is hg_serve's). */
static int on_set_params(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	uint64_t base;
	memcpy(&base, req, 8);
	if (M.restart_pending) {
		bfmt(reason, HG_CALL_REASON, "restart pending");
		return HG_E_REFUSED;
	}
	if (base != M.version) {
		bfmt(reason, HG_CALL_REASON, "stale: version is %llu", (unsigned long long)M.version);
		return HG_E_REFUSED;
	}
	memcpy(M.cand, (const unsigned char *)req + 8, M.type->payload_bytes);
	if (check_ranges(M.cand, reason, HG_CALL_REASON) < 0)
		return HG_E_REFUSED;
	if (M.validate) {
		reason[0] = 0;
		int rc = M.validate(M.cand, M.P, reason);
		if (rc != HG_OK) {
			if (!reason[0])
				bfmt(reason, HG_CALL_REASON, "refused by the module's validator");
			return HG_E_REFUSED;
		}
	}
	/* A change that touches a @restart field is saved, not applied. */
	int restart = 0;
	for (uint32_t i = 0; i < M.type->n_fields; i++) {
		const struct hg_field *f = &M.type->fields[i];
		size_t bytes = (size_t)hg_field_elem_size(f->kind) * f->count;
		if (M.attrs[i].restart && memcmp(M.cand + f->offset, M.P + f->offset, bytes) != 0)
			restart = 1;
	}
	struct hg_params_header hdr = { .version = M.version + 1, .restart_pending = restart };
	bfmt(hdr.changed_by, sizeof hdr.changed_by, "%s", c->caller);
	if (save_file(M.cand, &hdr) < 0) {
		bfmt(reason, HG_CALL_REASON, "cannot save %s: %s", M.path, strerror(errno));
		return HG_E_REFUSED;
	}
	uint64_t new_version = M.version + 1;
	if (restart) {
		M.restart_pending = 1;
		bfmt(M.changed_by, sizeof M.changed_by, "%s", c->caller);
		M.want_restart = 1;
	} else {
		memcpy(M.P, M.cand, M.type->payload_bytes);
		M.version = new_version;
		bfmt(M.changed_by, sizeof M.changed_by, "%s", c->caller);
		M.changed = 1;
	}
	publish_params(&c->req);
	unsigned char *r = rep;
	memcpy(r, &new_version, 8);
	r[8] = (unsigned char)restart;
	return HG_OK;
}

/* After the reply is out: the manager stops and starts us, which does not
 * count as a failure; the request is repeated until the manager answers. */
static void after_set_params(void *ctx)
{
	(void)ctx;
	if (!M.want_restart)
		return;
	M.want_restart = 0;
	if (!getenv("HANGAR_CONTROL")) {
		log_line("a @restart parameter changed; no manager to restart %s (restart it by hand)", M.module);
		return;
	}
	char reply[128];
	if (hg_control_call(NULL, "restart-self", 12, reply, sizeof reply, NULL, 1) < 0)
		log_line("restart-self: %s", strerror(errno));
	else if (!hg_control_ok(reply))
		log_line("restart-self refused: %s", reply);
}

static int build_topic_type(const struct hg_type *type, char *err, size_t errlen)
{
	static char text[HG_SCHEMA_MAX + 1024];
	int n = bfmt(text, sizeof text,
			 "# params:%s: the parameter topic (docs/05), then the fields of %s\n"
			 "u64  version\n"
			 "char changed_by[48]  # the caller, or \"saved\" at start\n"
			 "u8   restart_pending # a change to a @restart field is saved, not in force\n"
			 "u8   reserved[7]\n",
			 M.module, type->name);
	/* The type's own text, attributes included, so a reader can build its
	 * controls from the header; the canonical lines when it would not fit. */
	if (n + (int)type->schema_bytes <= HG_SCHEMA_MAX) {
		memcpy(text + n, type->schema, type->schema_bytes);
		n += (int)type->schema_bytes;
		if (text[n - 1] != '\n')
			text[n++] = '\n';
	} else {
		size_t c = hg_schema_canonical(type->schema, type->schema_bytes, text + n, sizeof text - (size_t)n);
		if (c == (size_t)-1 || n + c > HG_SCHEMA_MAX) {
			set_err(err, errlen, "the parameter topic's schema does not fit");
			return -1;
		}
		n += (int)c;
	}
	if (hg_schema_parse(text, (size_t)n, &M.topic_schema, err, errlen) < 0)
		return -1;
	M.topic_schema.type.name = "params";
	return 0;
}

int hg_params_open(const struct hg_type *type, void *P, hg_params_validate validate, void *ctx)
{
	if (!type || !P || M.open) {
		errno = M.open ? EEXIST : EINVAL;
		return -1;
	}
	if (hg_init() < 0)
		return -1;
	const struct hg_wiring *w = hg_wiring();
	bfmt(M.module, sizeof M.module, "%s", hg_module_name());
	char topic[HG_WIRING_NAME + 8];
	bfmt(topic, sizeof topic, "%s.params", M.module);
	const struct hg_wiring_pub *line = w ? hg_wiring_find_pub(w, topic) : NULL;
	if (!line) {
		log_line("%s is not an output of %s: no [Params] in the manifest", topic, M.module);
		errno = EACCES;
		return -1;
	}
	char err[160];
	/* The type must be the manifest's: the set_params request hash is
	 * `u64 base_version` then the fields. */
	const struct hg_wiring_serve *sv = NULL;
	for (int i = 0; i < w->n_serves; i++)
		if (strcmp(w->serves[i].method, "set_params") == 0)
			sv = &w->serves[i];
	{
		static char text[HG_SCHEMA_MAX + 64];
		static struct hg_schema parsed;
		int n = bfmt(text, sizeof text, "u64 base_version\n%.*s", (int)type->schema_bytes, type->schema);
		if (n <= 0 || n > HG_SCHEMA_MAX + 32 || hg_schema_parse(text, (size_t)n, &parsed, err, sizeof err) < 0) {
			log_line("cannot build set_params:%s: %s", type->name, err);
			errno = EINVAL;
			return -1;
		}
		if (!sv || sv->m.req_hash != parsed.type.hash) {
			log_line("%s: the manifest's [Params] Type is not %s (hash %016llx)", M.module, type->name,
				 (unsigned long long)parsed.type.hash);
			errno = EPROTOTYPE;
			return -1;
		}
	}
	if (hg_params_attrs(type, M.attrs, err, sizeof err) < 0) {
		log_line("%s: %s", type->name, err);
		errno = EINVAL;
		return -1;
	}
	if (build_topic_type(type, err, sizeof err) < 0) {
		log_line("%s: %s", type->name, err);
		errno = EINVAL;
		return -1;
	}
	if (line->hash && line->hash != M.topic_schema.type.hash) {
		log_line("%s: the wiring's type hash %016llx differs from the library's %016llx", topic,
			 (unsigned long long)line->hash, (unsigned long long)M.topic_schema.type.hash);
		errno = EPROTOTYPE;
		return -1;
	}
	M.type = type;
	M.P = P;
	M.validate = validate;
	M.ctx = ctx;
	M.cand = calloc(1, type->payload_bytes);
	M.topic = calloc(1, 64 + type->payload_bytes);
	if (!M.cand || !M.topic) {
		errno = ENOMEM;
		return -1;
	}
	const char *dir = getenv("HANGAR_STATE_DIR");
	if (dir && *dir) {
		bfmt(M.path, sizeof M.path, "%s/params", dir);
	} else {
		const char *root = getenv("HANGAR_ROOT");
		bfmt(M.path, sizeof M.path, "%s/var/lib/hangar/%s/params", root ? root : "", M.module);
	}
	if (hg_params_defaults(type, M.attrs, M.P, err, sizeof err) < 0)
		fail78("parameter defaults: %s", err);

	/* Loading at start (docs/05 "Saved values"). */
	static char text[HG_SCHEMA_MAX * 4];
	int fd = open(M.path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 && errno == ENOENT) {
		/* The first start: the defaults, saved before the first publish. */
		M.version = 1;
		bfmt(M.changed_by, sizeof M.changed_by, "saved");
		struct hg_params_header hdr = { .version = M.version };
		bfmt(hdr.changed_by, sizeof hdr.changed_by, "saved");
		char dirpath[512];
		bfmt(dirpath, sizeof dirpath, "%s", M.path);
		char *slash = strrchr(dirpath, '/');
		if (slash) {
			*slash = 0;
			mkdir(dirpath, 0750);
		}
		if (save_file(M.P, &hdr) < 0)
			fail78("cannot write %s: %s", M.path, strerror(errno));
		log_line("first start: defaults written to %s", M.path);
	} else if (fd < 0) {
		fail78("cannot read %s: %s", M.path, strerror(errno));
	} else {
		ssize_t n = read(fd, text, sizeof text - 1);
		close(fd);
		if (n < 0)
			fail78("cannot read %s: %s", M.path, strerror(errno));
		struct hg_params_header hdr;
		unsigned char present[HG_SCHEMA_MAX_FIELDS];
		if (hg_params_file_parse(type, text, (size_t)n, M.P, &hdr, present, 1, err, sizeof err) < 0)
			fail78("saved parameters refused: %s", err);
		for (uint32_t i = 0; i < type->n_fields; i++)
			if (!present[i])
				log_line("%s: %s is not in the saved file: default taken", M.path, type->fields[i].name);
		char reason[HG_PARAMS_REASON];
		if (check_ranges(M.P, reason, sizeof reason) < 0)
			fail78("saved parameters refused: %s", reason);
		if (validate) {
			reason[0] = 0;
			if (validate(M.P, M.P, reason) != HG_OK)
				fail78("saved parameters refused: %s", reason[0] ? reason : "by the validator");
		}
		M.version = hdr.have_version ? hdr.version : 1;
		/* The instance a @restart change asked for publishes with the
		 * caller's name, and clears the mark so the next start says saved. */
		if (hdr.restart_pending) {
			bfmt(M.changed_by, sizeof M.changed_by, "%s", hdr.changed_by);
			struct hg_params_header clean = { .version = M.version };
			bfmt(clean.changed_by, sizeof clean.changed_by, "%s", hdr.changed_by);
			if (save_file(M.P, &clean) < 0)
				log_line("cannot rewrite %s: %s", M.path, strerror(errno));
		} else {
			bfmt(M.changed_by, sizeof M.changed_by, "saved");
		}
	}
	M.pub = hg_pub_open(topic, &M.topic_schema.type, NULL);
	if (!M.pub) {
		int e = errno;
		log_line("cannot open %s: %s", topic, strerror(e));
		errno = e;
		return -1;
	}
	if (hg_serve_method("set_params", on_set_params, NULL) < 0 && errno != EEXIST) {
		int e = errno;
		log_line("cannot serve set_params: %s", strerror(e));
		errno = e;
		return -1;
	}
	hg_serve_after("set_params", after_set_params);
	M.open = 1;
	publish_params(NULL);
	return 0;
}

int hg_params_changed(void)
{
	int c = M.changed;
	M.changed = 0;
	return c;
}

uint64_t hg_params_version(void)
{
	return M.version;
}

const char *hg_params_changed_by(void)
{
	return M.changed_by;
}

int hg_params_restart_pending(void)
{
	return M.restart_pending;
}

void *hg_params_ctx(void)
{
	return M.ctx;
}
