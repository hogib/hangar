// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * msggen -- `hangar msg gen`, hangar's message-type generator.
 *
 * Reads .msg files (the grammar is in tools/msggen/README.md) and writes,
 * for each, msg/<name>.h (the struct, its padding made explicit, and a
 * static assert for its size and for every field's offset) and msg/<name>.c
 * (the type descriptor: the schema text verbatim, the field table, the
 * FNV-1a 64 hash of the canonical form; for parameter types also the table
 * of defaults, ranges and @restart marks). The descriptor's shape is
 * lib/hg/hg_type_abi.md, which binds this tool and lib/hg alike.
 * python/hangar/schema.py parses the same grammar and must compute the same
 * hash; `make check-msggen` compares the two over every file in msg/.
 *
 *   msggen [--out <dir>] [--params] <file.msg>...      write <dir>/<name>.{h,c}
 *   msggen --print-hash [--params] <file.msg>...      "<name> <hash-hex> <payload_bytes>"
 *   msggen --check [--out <dir>] [--params] <file.msg>...
 *                            regenerate in memory; fail if <dir>'s files differ
 *
 * <dir> defaults to the directory the .msg file is in. Every refusal names
 * the file and the line. C11, no dependencies: built for the build machine
 * (HOSTCC), it is not needed at run time.
 *
 * The test suite (tests/test_msggen.c) includes this file with MSGGEN_NO_MAIN
 * defined, so the parser and the generators are reachable without a process.
 */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  MSG_MAX_BYTES = 3584,   // the topic header's schema field (docs/01-topics.md)
  MSG_MAX_FIELDS = 128,   // lib/hg's run-time parser holds this many (hg_type_abi.md)
  MSG_MAX_NAME = 63,      // field and type names, bytes
};

// Same order and meaning as enum hg_field_kind in hg_type.h.
enum msg_kind {
  MK_U8, MK_I8, MK_U16, MK_I16, MK_U32, MK_I32, MK_U64, MK_I64, MK_F32, MK_F64,
  MK_CHAR, MK_COUNT
};

struct kind_info {
  const char *name;     // as written in the .msg
  const char *ctype;    // the C type in the generated struct
  const char *hg_name;  // the enum hg_field_kind constant
  unsigned size;        // bytes; also the natural alignment
  unsigned is_float;
  unsigned is_signed;
};

static const struct kind_info KINDS[MK_COUNT] = {
    {"u8", "uint8_t", "HG_U8", 1, 0, 0},    {"i8", "int8_t", "HG_I8", 1, 0, 1},
    {"u16", "uint16_t", "HG_U16", 2, 0, 0}, {"i16", "int16_t", "HG_I16", 2, 0, 1},
    {"u32", "uint32_t", "HG_U32", 4, 0, 0}, {"i32", "int32_t", "HG_I32", 4, 0, 1},
    {"u64", "uint64_t", "HG_U64", 8, 0, 0}, {"i64", "int64_t", "HG_I64", 8, 0, 1},
    {"f32", "float", "HG_F32", 4, 1, 1},    {"f64", "double", "HG_F64", 8, 1, 1},
    {"char", "char", "HG_CHAR", 1, 0, 0},
};

struct msg_field {
  char *name;
  enum msg_kind kind;
  uint32_t count;     // 1 for a scalar, N for name[N]
  int is_array;       // written with [N] (char always is)
  uint32_t offset;
  char *comment;      // trailing comment, "" if none
  char *dflt;         // parameter attributes, as written; "" if none
  char *min;
  char *max;
  int restart;
  int line;
};

struct msg_type {
  char name[MSG_MAX_NAME + 1];
  const char *text;   // the .msg bytes, not owned
  size_t text_len;
  int params;
  uint32_t n_fields;
  uint32_t payload_bytes;
  uint64_t hash;
  struct msg_field fields[MSG_MAX_FIELDS];
};

// ---------------------------------------------------------------- helpers

static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    fputs("msggen: out of memory\n", stderr);
    exit(1);
  }
  return p;
}

static char *xstrndup(const char *s, size_t n) {
  char *d = xmalloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

struct buf {
  char *p;
  size_t n, cap;
};

static void buf_reserve(struct buf *b, size_t extra) {
  if (b->n + extra + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap : 1024;
  while (cap < b->n + extra + 1) cap *= 2;
  char *p = xmalloc(cap);
  if (b->n) memcpy(p, b->p, b->n);
  free(b->p);
  b->p = p;
  b->cap = cap;
  b->p[b->n] = 0;
}

static void buf_putn(struct buf *b, const char *s, size_t n) {
  buf_reserve(b, n);
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}

static void buf_puts(struct buf *b, const char *s) { buf_putn(b, s, strlen(s)); }

static void buf_printf(struct buf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  buf_reserve(b, (size_t)n);
  vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  b->n += (size_t)n;
}

static void buf_free(struct buf *b) {
  free(b->p);
  b->p = NULL;
  b->n = b->cap = 0;
}

// A C string literal for s[0..n): the characters the ABI's schema field may
// hold are kept readable, everything else is an octal escape. `?` is escaped
// so no trigraph can form, whatever the compiler's -std.
static void buf_put_cstr(struct buf *b, const char *s, size_t n) {
  buf_putn(b, "\"", 1);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\\' || c == '"' || c == '?') {
      buf_printf(b, "\\%c", c);
    } else if (c == '\n') {
      buf_puts(b, "\\n");
    } else if (c == '\t') {
      buf_puts(b, "\\t");
    } else if (c >= 0x20 && c < 0x7f) {
      buf_putn(b, (const char *)&c, 1);
    } else {
      buf_printf(b, "\\%03o", c);
    }
  }
  buf_putn(b, "\"", 1);
}

static uint64_t fnv1a64(const void *data, size_t n) {
  const unsigned char *p = data;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

static void trim(const char **s, size_t *n) {
  while (*n && is_space(**s)) {
    (*s)++;
    (*n)--;
  }
  while (*n && is_space((*s)[*n - 1])) (*n)--;
}

static int ident_ok(const char *s, size_t n) {
  if (n == 0) return 0;
  if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
  for (size_t i = 1; i < n; i++)
    if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
  return 1;
}

static int fail(char *err, size_t errsz, const char *file, int line, const char *fmt, ...) {
  va_list ap;
  int n = 0;
  if (errsz) {
    if (line > 0)
      n = snprintf(err, errsz, "%s:%d: ", file, line);
    else
      n = snprintf(err, errsz, "%s: ", file);
    if (n < 0 || (size_t)n >= errsz) n = (int)errsz - 1;
    va_start(ap, fmt);
    vsnprintf(err + n, errsz - (size_t)n, fmt, ap);
    va_end(ap);
  }
  return -1;
}

// ---------------------------------------------------------------- numbers

struct num {
  int is_float;
  double f;
  int64_t i;
  uint64_t u;
};

// Parses tok[0..n) as a value of kind k. Decimal only: the same literals
// Python's int() and float() accept, so both sides agree on what is valid.
static int parse_num(enum msg_kind k, const char *tok, size_t n, struct num *out, char *why,
                     size_t whysz) {
  char tmp[64];
  const struct kind_info *ki = &KINDS[k];
  if (n == 0 || n >= sizeof tmp) {
    snprintf(why, whysz, "'%.*s' is not a number", (int)n, tok);
    return -1;
  }
  memcpy(tmp, tok, n);
  tmp[n] = 0;
  memset(out, 0, sizeof *out);
  char *end;
  errno = 0;
  if (ki->is_float) {
    for (size_t i = 0; i < n; i++) {
      char c = tmp[i];
      if (!(isdigit((unsigned char)c) || c == '.' || c == 'e' || c == 'E' || c == '+' ||
            c == '-')) {
        snprintf(why, whysz, "'%s' is not a decimal number", tmp);
        return -1;
      }
    }
    out->is_float = 1;
    out->f = strtod(tmp, &end);
    if (*end || end == tmp) {
      snprintf(why, whysz, "'%s' is not a number", tmp);
      return -1;
    }
    return 0;
  }
  if (ki->is_signed) {
    long long v = strtoll(tmp, &end, 10);
    if (*end || end == tmp || errno) {
      snprintf(why, whysz, "'%s' is not an integer", tmp);
      return -1;
    }
    long long lo = -(1ll << (ki->size * 8 - 1)), hi = (1ll << (ki->size * 8 - 1)) - 1;
    if (ki->size == 8) {
      lo = INT64_MIN;
      hi = INT64_MAX;
    }
    if (v < lo || v > hi) {
      snprintf(why, whysz, "%s does not fit %s", tmp, ki->name);
      return -1;
    }
    out->i = v;
    return 0;
  }
  if (tmp[0] == '-') {
    snprintf(why, whysz, "%s is negative, the field is %s", tmp, ki->name);
    return -1;
  }
  unsigned long long v = strtoull(tmp, &end, 10);
  if (*end || end == tmp || errno) {
    snprintf(why, whysz, "'%s' is not an integer", tmp);
    return -1;
  }
  if (ki->size < 8 && v > ((1ull << (ki->size * 8)) - 1)) {
    snprintf(why, whysz, "%s does not fit %s", tmp, ki->name);
    return -1;
  }
  out->u = v;
  return 0;
}

static int num_cmp(enum msg_kind k, const struct num *a, const struct num *b) {
  if (KINDS[k].is_float) return a->f < b->f ? -1 : a->f > b->f ? 1 : 0;
  if (KINDS[k].is_signed) return a->i < b->i ? -1 : a->i > b->i ? 1 : 0;
  return a->u < b->u ? -1 : a->u > b->u ? 1 : 0;
}

// ---------------------------------------------------------------- parsing

static int field_attrs_ok(const char *file, const struct msg_field *f, char *err, size_t errsz) {
  const struct kind_info *ki = &KINDS[f->kind];
  char why[160];
  struct num lo, hi;
  int has_range = f->min[0] != 0;

  if (has_range) {
    if (f->kind == MK_CHAR)
      return fail(err, errsz, file, f->line, "a char field has no range");
    if (parse_num(f->kind, f->min, strlen(f->min), &lo, why, sizeof why))
      return fail(err, errsz, file, f->line, "range: %s", why);
    if (parse_num(f->kind, f->max, strlen(f->max), &hi, why, sizeof why))
      return fail(err, errsz, file, f->line, "range: %s", why);
    if (num_cmp(f->kind, &lo, &hi) > 0)
      return fail(err, errsz, file, f->line, "range [%s, %s] has min above max", f->min, f->max);
  }
  if (f->dflt[0]) {
    size_t n = strlen(f->dflt);
    if (f->kind == MK_CHAR) {
      if (n < 2 || f->dflt[0] != '"' || f->dflt[n - 1] != '"')
        return fail(err, errsz, file, f->line, "a char default is quoted text: = \"...\"");
      for (size_t i = 1; i + 1 < n; i++)
        if (f->dflt[i] == '"' || f->dflt[i] == '\\')
          return fail(err, errsz, file, f->line,
                      "a char default holds no quotes or backslashes");
      if (n - 2 > (size_t)f->count - 1)
        return fail(err, errsz, file, f->line, "default %s is longer than %s[%" PRIu32
                    "] holds (%" PRIu32 " bytes and a NUL)",
                    f->dflt, f->name, f->count, f->count - 1);
      return 0;
    }
    // One value, or one per element.
    uint32_t nvals = 0;
    const char *p = f->dflt, *end = f->dflt + n;
    while (p <= end) {
      const char *comma = memchr(p, ',', (size_t)(end - p));
      const char *stop = comma ? comma : end;
      size_t len = (size_t)(stop - p);
      trim(&p, &len);
      struct num v;
      if (parse_num(f->kind, p, len, &v, why, sizeof why))
        return fail(err, errsz, file, f->line, "default: %s", why);
      if (has_range && (num_cmp(f->kind, &v, &lo) < 0 || num_cmp(f->kind, &v, &hi) > 0))
        return fail(err, errsz, file, f->line, "default %.*s is outside [%s, %s]", (int)len, p,
                    f->min, f->max);
      nvals++;
      if (!comma) break;
      p = comma + 1;
    }
    if (nvals != 1 && nvals != f->count)
      return fail(err, errsz, file, f->line,
                  "default has %" PRIu32 " values; %s[%" PRIu32 "] takes 1 or %" PRIu32, nvals,
                  f->name, f->count, f->count);
    if (nvals != 1 && !f->is_array)
      return fail(err, errsz, file, f->line, "default has %" PRIu32 " values for a scalar",
                  nvals);
    (void)ki;
  }
  return 0;
}

static void field_free(struct msg_field *f) {
  free(f->name);
  free(f->comment);
  free(f->dflt);
  free(f->min);
  free(f->max);
  memset(f, 0, sizeof *f);
}

static void msg_free(struct msg_type *t) {
  for (uint32_t i = 0; i < t->n_fields; i++) field_free(&t->fields[i]);
  t->n_fields = 0;
}

// One line of a .msg: blank, a comment, or
//   <type> <name>[ [N] ] [= default] [[min, max]] [@restart] [# comment]
static int parse_line(const char *file, int lineno, const char *line, size_t len, int params,
                      struct msg_type *t, char *err, size_t errsz) {
  // The comment starts at the first '#' outside quotes.
  size_t body = len;
  int quoted = 0;
  for (size_t i = 0; i < len; i++) {
    if (line[i] == '"') quoted = !quoted;
    if (line[i] == '#' && !quoted) {
      body = i;
      break;
    }
  }
  const char *comment = "";
  size_t comment_len = 0;
  if (body < len) {
    comment = line + body + 1;
    comment_len = len - body - 1;
    trim(&comment, &comment_len);
  }
  const char *p = line;
  size_t n = body;
  trim(&p, &n);
  if (n == 0) return 0;  // blank or comment-only

  if (t->n_fields >= MSG_MAX_FIELDS)
    return fail(err, errsz, file, lineno, "more than %d fields", MSG_MAX_FIELDS);

  // type
  size_t i = 0;
  while (i < n && !is_space(p[i])) i++;
  int kind = -1;
  for (int k = 0; k < MK_COUNT; k++)
    if (strlen(KINDS[k].name) == i && memcmp(KINDS[k].name, p, i) == 0) kind = k;
  if (kind < 0)
    return fail(err, errsz, file, lineno,
                "unknown type '%.*s' (u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 char; no nested types)",
                (int)i, p);
  p += i;
  n -= i;
  trim(&p, &n);

  // name[N]
  i = 0;
  while (i < n && !is_space(p[i]) && p[i] != '=') i++;
  if (i == 0) return fail(err, errsz, file, lineno, "missing field name after '%s'", KINDS[kind].name);
  const char *name = p;
  size_t name_len = i;
  uint32_t count = 1;
  int is_array = 0;
  const char *br = memchr(name, '[', name_len);
  if (br) {
    size_t head = (size_t)(br - name);
    if (name[name_len - 1] != ']')
      return fail(err, errsz, file, lineno, "array size: expected %.*s[N]", (int)head, name);
    const char *digits = br + 1;
    size_t dn = name_len - head - 2;
    if (dn == 0 || dn > 10)
      return fail(err, errsz, file, lineno, "array size: expected %.*s[N] with N from 1", (int)head, name);
    unsigned long long v = 0;
    for (size_t d = 0; d < dn; d++) {
      if (!isdigit((unsigned char)digits[d]))
        return fail(err, errsz, file, lineno, "array size '%.*s' is not a number", (int)dn, digits);
      v = v * 10 + (unsigned long long)(digits[d] - '0');
    }
    if (v < 1) return fail(err, errsz, file, lineno, "array size must be at least 1: %.*s", (int)name_len, name);
    if (v > 0xffffffffull) return fail(err, errsz, file, lineno, "array size %llu is too large", v);
    count = (uint32_t)v;
    is_array = 1;
    name_len = head;
  } else if (kind == MK_CHAR) {
    return fail(err, errsz, file, lineno, "char needs a size: char %.*s[N]", (int)name_len, name);
  }
  if (memchr(name, '.', name_len))
    return fail(err, errsz, file, lineno,
                "dotted name '%.*s': only generated dataset row types carry dots", (int)name_len, name);
  if (!ident_ok(name, name_len))
    return fail(err, errsz, file, lineno, "'%.*s' is not a C identifier", (int)name_len, name);
  if (name[0] == '_')
    return fail(err, errsz, file, lineno,
                "'%.*s': names starting with '_' are reserved for generated names (padding, _seq)",
                (int)name_len, name);
  if (name_len > MSG_MAX_NAME)
    return fail(err, errsz, file, lineno, "name '%.*s' is longer than %d bytes", (int)name_len, name,
                MSG_MAX_NAME);
  for (uint32_t k = 0; k < t->n_fields; k++)
    if (strlen(t->fields[k].name) == name_len && memcmp(t->fields[k].name, name, name_len) == 0)
      return fail(err, errsz, file, lineno, "duplicate field '%.*s' (first on line %d)", (int)name_len,
                  name, t->fields[k].line);
  p += i;
  n -= i;
  trim(&p, &n);

  // attributes, in order: = default, [min, max], @restart
  const char *dflt = "", *min = "", *max = "";
  size_t dflt_len = 0, min_len = 0, max_len = 0;
  int restart = 0, stage = 0;
  while (n) {
    if (p[0] == '=') {
      if (stage >= 1) return fail(err, errsz, file, lineno, "the default comes first: = default [min, max] @restart");
      stage = 1;
      p++;
      n--;
      trim(&p, &n);
      if (n == 0) return fail(err, errsz, file, lineno, "missing value after '='");
      size_t j = 0;
      if (p[0] == '"') {
        j = 1;
        while (j < n && p[j] != '"') j++;
        if (j >= n) return fail(err, errsz, file, lineno, "unterminated string in default");
        j++;
      } else {
        while (j < n && p[j] != '[' && p[j] != '@') j++;
      }
      dflt = p;
      dflt_len = j;
      trim(&dflt, &dflt_len);
      if (dflt_len == 0) return fail(err, errsz, file, lineno, "missing value after '='");
      p += j;
      n -= j;
    } else if (p[0] == '[') {
      if (stage >= 2) return fail(err, errsz, file, lineno, "the range comes before @restart, and only once");
      stage = 2;
      const char *close = memchr(p, ']', n);
      if (!close) return fail(err, errsz, file, lineno, "unterminated range: expected [min, max]");
      const char *inner = p + 1;
      size_t inner_len = (size_t)(close - inner);
      const char *comma = memchr(inner, ',', inner_len);
      if (!comma || memchr(comma + 1, ',', (size_t)(close - comma - 1)))
        return fail(err, errsz, file, lineno, "range: expected [min, max], got [%.*s]", (int)inner_len, inner);
      min = inner;
      min_len = (size_t)(comma - inner);
      max = comma + 1;
      max_len = (size_t)(close - comma - 1);
      trim(&min, &min_len);
      trim(&max, &max_len);
      if (min_len == 0 || max_len == 0)
        return fail(err, errsz, file, lineno, "range: expected [min, max], got [%.*s]", (int)inner_len, inner);
      n -= (size_t)(close + 1 - p);
      p = close + 1;
    } else if (n >= 8 && memcmp(p, "@restart", 8) == 0 && (n == 8 || is_space(p[8]))) {
      if (stage >= 3) return fail(err, errsz, file, lineno, "@restart given twice");
      stage = 3;
      restart = 1;
      p += 8;
      n -= 8;
    } else {
      size_t j = 0;
      while (j < n && !is_space(p[j])) j++;
      return fail(err, errsz, file, lineno, "unexpected '%.*s' after the field", (int)j, p);
    }
    trim(&p, &n);
  }
  if (stage && !params)
    return fail(err, errsz, file, lineno,
                "defaults, ranges and @restart belong to parameter types: pass --params");

  struct msg_field *f = &t->fields[t->n_fields];
  memset(f, 0, sizeof *f);
  f->name = xstrndup(name, name_len);
  f->kind = (enum msg_kind)kind;
  f->count = count;
  f->is_array = is_array;
  f->comment = xstrndup(comment, comment_len);
  f->dflt = xstrndup(dflt, dflt_len);
  f->min = xstrndup(min, min_len);
  f->max = xstrndup(max, max_len);
  f->restart = restart;
  f->line = lineno;
  if (field_attrs_ok(file, f, err, errsz)) {
    field_free(f);
    return -1;
  }
  t->n_fields++;
  return 0;
}

// The canonical form: "type name\n" or "type name[N]\n" per field.
static void msg_canonical(const struct msg_type *t, struct buf *b) {
  for (uint32_t i = 0; i < t->n_fields; i++) {
    const struct msg_field *f = &t->fields[i];
    if (f->is_array)
      buf_printf(b, "%s %s[%" PRIu32 "]\n", KINDS[f->kind].name, f->name, f->count);
    else
      buf_printf(b, "%s %s\n", KINDS[f->kind].name, f->name);
  }
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// Offsets at natural alignment; the size is the C struct's, rounded up to 8.
static int msg_layout(const char *file, struct msg_type *t, char *err, size_t errsz) {
  uint64_t off = 0, maxalign = 1;
  for (uint32_t i = 0; i < t->n_fields; i++) {
    struct msg_field *f = &t->fields[i];
    uint64_t a = KINDS[f->kind].size;
    off = align_up(off, a);
    f->offset = (uint32_t)off;
    off += a * f->count;
    if (a > maxalign) maxalign = a;
    if (off > 0xffffffffull) return fail(err, errsz, file, f->line, "the payload exceeds 4 GiB");
  }
  off = align_up(align_up(off, maxalign), 8);
  if (off > 0xffffffffull) return fail(err, errsz, file, 0, "the payload exceeds 4 GiB");
  t->payload_bytes = (uint32_t)off;
  struct buf c = {0};
  msg_canonical(t, &c);
  t->hash = fnv1a64(c.p, c.n);
  buf_free(&c);
  return 0;
}

// Parses text[0..len), the bytes of file (named in errors), into t. The
// type's name must already be in t->name (msg_name_from_path). Returns 0, or
// -1 with a one-line message in err.
static int msg_parse(const char *file, const char *text, size_t len, int params, struct msg_type *t,
                     char *err, size_t errsz) {
  t->text = text;
  t->text_len = len;
  t->params = params;
  t->n_fields = 0;
  t->payload_bytes = 0;
  t->hash = 0;
  if (len > MSG_MAX_BYTES)
    return fail(err, errsz, file, 0, "%zu bytes; the topic header holds a schema of at most %d",
                len, MSG_MAX_BYTES);
  if (memchr(text, 0, len)) return fail(err, errsz, file, 0, "NUL byte in the file");
  int lineno = 0;
  size_t pos = 0;
  while (pos < len) {
    const char *nl = memchr(text + pos, '\n', len - pos);
    size_t ll = nl ? (size_t)(nl - (text + pos)) : len - pos;
    lineno++;
    if (parse_line(file, lineno, text + pos, ll, params, t, err, errsz)) {
      msg_free(t);
      return -1;
    }
    pos += ll + 1;
  }
  if (t->n_fields == 0) {
    return fail(err, errsz, file, 0, "no fields (an empty type is 'u8 reserved')");
  }
  if (msg_layout(file, t, err, errsz)) {
    msg_free(t);
    return -1;
  }
  return 0;
}

// "msg/depth.msg" -> "depth". Refuses anything but an identifier stem.
static int msg_name_from_path(const char *path, char *name, size_t namesz, char *err, size_t errsz) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  size_t n = strlen(base);
  if (n < 5 || strcmp(base + n - 4, ".msg") != 0)
    return fail(err, errsz, path, 0, "not a .msg file");
  n -= 4;
  if (memchr(base, '.', n))
    return fail(err, errsz, path, 0, "dotted type name '%.*s': only generated dataset row types carry dots",
                (int)n, base);
  if (!ident_ok(base, n))
    return fail(err, errsz, path, 0, "type name '%.*s' is not a C identifier", (int)n, base);
  if (base[0] == '_')
    return fail(err, errsz, path, 0, "type name '%.*s' starts with '_', reserved", (int)n, base);
  if (n > MSG_MAX_NAME || n >= namesz)
    return fail(err, errsz, path, 0, "type name '%.*s' is longer than %d bytes", (int)n, base, MSG_MAX_NAME);
  memcpy(name, base, n);
  name[n] = 0;
  return 0;
}

// The label written into the generated files: the file's directory's last
// component and its name ("msg/depth.msg"), the same however it was invoked.
static void msg_label(const char *path, char *label, size_t sz) {
  const char *base = strrchr(path, '/');
  if (!base) {
    snprintf(label, sz, "%s", path);
    return;
  }
  const char *dir_end = base;
  const char *dir_start = dir_end;
  while (dir_start > path && dir_start[-1] != '/') dir_start--;
  if (dir_start == dir_end)
    snprintf(label, sz, "%s", base + 1);
  else
    snprintf(label, sz, "%.*s/%s", (int)(dir_end - dir_start), dir_start, base + 1);
}

// ---------------------------------------------------------------- generation

static void upper_name(const struct msg_type *t, char *out, size_t sz) {
  size_t i;
  for (i = 0; i + 1 < sz && t->name[i]; i++) out[i] = (char)toupper((unsigned char)t->name[i]);
  out[i] = 0;
}

struct decl {
  char text[192];
  const char *comment;
};

static void gen_header(const struct msg_type *t, const char *label, struct buf *b) {
  char up[MSG_MAX_NAME + 1];
  upper_name(t, up, sizeof up);
  buf_printf(b, "// Generated by hangar msg gen from %s; do not edit.\n", label);
  buf_puts(b, "// SPDX-License-Identifier: LGPL-3.0-or-later\n");
  buf_printf(b, "#ifndef HG_MSG_%s_H\n#define HG_MSG_%s_H\n", up, up);
  buf_puts(b, "#include <stddef.h>\n#include <stdint.h>\n#include \"hg_type.h\"\n");

  // The struct, padding made explicit so that sizeof == payload_bytes.
  struct decl *decls = xmalloc(sizeof *decls * (2u * MSG_MAX_FIELDS + 1));
  size_t nd = 0, width = 0;
  uint64_t off = 0;
  int npad = 0;
  for (uint32_t i = 0; i < t->n_fields; i++) {
    const struct msg_field *f = &t->fields[i];
    if (f->offset > off) {
      snprintf(decls[nd].text, sizeof decls[nd].text, "uint8_t _pad%d[%" PRIu32 "];", npad++,
               (uint32_t)(f->offset - off));
      decls[nd].comment = "padding";
      nd++;
    }
    if (f->is_array)
      snprintf(decls[nd].text, sizeof decls[nd].text, "%s %s[%" PRIu32 "];", KINDS[f->kind].ctype,
               f->name, f->count);
    else
      snprintf(decls[nd].text, sizeof decls[nd].text, "%s %s;", KINDS[f->kind].ctype, f->name);
    decls[nd].comment = f->comment;
    nd++;
    off = (uint64_t)f->offset + (uint64_t)KINDS[f->kind].size * f->count;
  }
  if (t->payload_bytes > off) {
    snprintf(decls[nd].text, sizeof decls[nd].text, "uint8_t _pad%d[%" PRIu32 "];", npad++,
             (uint32_t)(t->payload_bytes - off));
    decls[nd].comment = "padding";
    nd++;
  }
  for (size_t i = 0; i < nd; i++)
    if (decls[i].comment[0] && strlen(decls[i].text) > width) width = strlen(decls[i].text);
  buf_printf(b, "typedef struct %s {\n", t->name);
  for (size_t i = 0; i < nd; i++) {
    buf_printf(b, "    %s", decls[i].text);
    if (decls[i].comment[0])
      buf_printf(b, "%*s  // %s", (int)(width - strlen(decls[i].text)), "", decls[i].comment);
    buf_puts(b, "\n");
  }
  buf_printf(b, "} %s;\n", t->name);
  free(decls);

  buf_printf(b, "extern const struct hg_type %s_TYPE;   // defined in %.*s.c, also generated\n", up,
             (int)(strlen(label) - 4), label);
  if (t->params) {
    buf_puts(b, "// Parameter attributes, one entry per field in field order, as written in the .msg.\n");
    buf_puts(b, "#ifndef HG_PARAM_META_DEFINED\n#define HG_PARAM_META_DEFINED\n");
    buf_puts(b, "struct hg_param_meta {\n"
                "    const char *dflt;     // \"\" if none; a list for arrays, quoted text for char\n"
                "    const char *min;      // \"\" if no range\n"
                "    const char *max;\n"
                "    uint8_t restart;      // 1: changing it restarts the module\n"
                "};\n#endif\n");
    buf_printf(b, "extern const struct hg_param_meta %s_META[%" PRIu32 "];\n", up, t->n_fields);
  }
  buf_printf(b, "_Static_assert(sizeof(struct %s) == %" PRIu32 ", \"layout\");\n", t->name,
             t->payload_bytes);
  for (uint32_t i = 0; i < t->n_fields; i++)
    buf_printf(b, "_Static_assert(offsetof(struct %s, %s) == %" PRIu32 ", \"layout\");\n", t->name,
               t->fields[i].name, t->fields[i].offset);
  buf_puts(b, "#endif\n");
}

static void gen_source(const struct msg_type *t, const char *label, struct buf *b) {
  char up[MSG_MAX_NAME + 1];
  upper_name(t, up, sizeof up);
  buf_printf(b, "// Generated by hangar msg gen from %s; do not edit.\n", label);
  buf_puts(b, "// SPDX-License-Identifier: LGPL-3.0-or-later\n");
  buf_printf(b, "#include \"%s.h\"\n\n", t->name);

  buf_printf(b, "static const struct hg_field %s_fields[%" PRIu32 "] = {\n", t->name, t->n_fields);
  for (uint32_t i = 0; i < t->n_fields; i++) {
    const struct msg_field *f = &t->fields[i];
    buf_printf(b, "    {\"%s\", %s, %" PRIu32 ", %" PRIu32 ", ", f->name, KINDS[f->kind].hg_name,
               f->count, f->offset);
    buf_put_cstr(b, f->comment, strlen(f->comment));
    buf_puts(b, "},\n");
  }
  buf_puts(b, "};\n\n");

  buf_printf(b, "const struct hg_type %s_TYPE = {\n", up);
  buf_printf(b, "    .name = \"%s\",\n", t->name);
  buf_puts(b, "    .schema =\n");
  size_t pos = 0;
  while (pos < t->text_len) {
    const char *nl = memchr(t->text + pos, '\n', t->text_len - pos);
    size_t ll = nl ? (size_t)(nl - (t->text + pos)) + 1 : t->text_len - pos;
    buf_puts(b, "        ");
    buf_put_cstr(b, t->text + pos, ll);
    buf_puts(b, "\n");
    pos += ll;
  }
  buf_puts(b, "        ,\n");
  buf_printf(b, "    .schema_bytes = %zu,\n", t->text_len);
  buf_printf(b, "    .hash = 0x%016" PRIx64 "ull,   // FNV-1a 64 of the canonical form\n", t->hash);
  buf_printf(b, "    .payload_bytes = %" PRIu32 ",\n", t->payload_bytes);
  buf_printf(b, "    .n_fields = %" PRIu32 ",\n", t->n_fields);
  buf_printf(b, "    .fields = %s_fields,\n};\n", t->name);

  if (t->params) {
    buf_printf(b, "\nconst struct hg_param_meta %s_META[%" PRIu32 "] = {\n", up, t->n_fields);
    size_t width = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
      for (uint32_t i = 0; i < t->n_fields; i++) {
        const struct msg_field *f = &t->fields[i];
        struct buf e = {0};
        buf_puts(&e, "{");
        buf_put_cstr(&e, f->dflt, strlen(f->dflt));
        buf_puts(&e, ", ");
        buf_put_cstr(&e, f->min, strlen(f->min));
        buf_puts(&e, ", ");
        buf_put_cstr(&e, f->max, strlen(f->max));
        buf_printf(&e, ", %d},", f->restart);
        if (pass == 0) {
          if (e.n > width) width = e.n;
        } else {
          buf_printf(b, "    %s%*s  // %s\n", e.p, (int)(width - e.n), "", f->name);
        }
        buf_free(&e);
      }
    }
    buf_puts(b, "};\n");
  }
}

// ---------------------------------------------------------------- files

#ifndef MSGGEN_NO_MAIN

// Reads at most MSG_MAX_BYTES + 1 bytes (enough to see that a file is over
// the limit). Returns the length, or (size_t)-1.
static size_t read_file(const char *path, char *out, size_t cap) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return (size_t)-1;
  size_t n = fread(out, 1, cap, fp);
  int bad = ferror(fp);
  fclose(fp);
  return bad ? (size_t)-1 : n;
}

static char *read_whole(const char *path, size_t *len) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  struct buf b = {0};
  char tmp[4096];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) buf_putn(&b, tmp, n);
  int bad = ferror(fp);
  fclose(fp);
  if (bad) {
    buf_free(&b);
    return NULL;
  }
  *len = b.n;
  return b.p ? b.p : xstrndup("", 0);
}

static int write_file(const char *path, const struct buf *b) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "msggen: cannot write %s: %s\n", path, strerror(errno));
    return -1;
  }
  if (fwrite(b->p, 1, b->n, fp) != b->n || fclose(fp)) {
    fprintf(stderr, "msggen: cannot write %s: %s\n", path, strerror(errno));
    return -1;
  }
  return 0;
}

// Compares the generated text with the file at path; 0 if equal.
static int check_file(const char *path, const struct buf *b) {
  size_t len;
  char *have = read_whole(path, &len);
  if (!have) {
    fprintf(stderr, "msggen: %s is missing; run make msg\n", path);
    return -1;
  }
  int differs = len != b->n || memcmp(have, b->p, len) != 0;
  free(have);
  if (differs) {
    fprintf(stderr, "msggen: %s is out of date; run make msg\n", path);
    return -1;
  }
  return 0;
}

static void usage(void) {
  fputs("usage: msggen [--out <dir>] [--params] <file.msg>...\n"
        "       msggen --print-hash [--params] <file.msg>...\n"
        "       msggen --check [--out <dir>] [--params] <file.msg>...\n",
        stderr);
}

int main(int argc, char **argv) {
  const char *out = NULL;
  int params = 0, print_hash = 0, check = 0;
  int first = argc;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out = argv[++i];
    } else if (strcmp(argv[i], "--params") == 0) {
      params = 1;
    } else if (strcmp(argv[i], "--print-hash") == 0) {
      print_hash = 1;
    } else if (strcmp(argv[i], "--check") == 0) {
      check = 1;
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage();
      return 0;
    } else if (argv[i][0] == '-' && argv[i][1]) {
      fprintf(stderr, "msggen: unknown option %s\n", argv[i]);
      usage();
      return 2;
    } else {
      first = i;
      break;
    }
  }
  if (first >= argc || (print_hash && check)) {
    usage();
    return 2;
  }

  static struct msg_type t;
  static char text[MSG_MAX_BYTES + 1];
  int rc = 0;
  for (int i = first; i < argc; i++) {
    const char *path = argv[i];
    char err[512];
    if (path[0] == '-') {
      fprintf(stderr, "msggen: options go before the files: %s\n", path);
      return 2;
    }
    size_t len = read_file(path, text, sizeof text);
    if (len == (size_t)-1) {
      fprintf(stderr, "msggen: cannot read %s: %s\n", path, strerror(errno));
      rc = 1;
      continue;
    }
    if (msg_name_from_path(path, t.name, sizeof t.name, err, sizeof err) ||
        msg_parse(path, text, len, params, &t, err, sizeof err)) {
      fprintf(stderr, "msggen: %s\n", err);
      rc = 1;
      continue;
    }
    if (print_hash) {
      printf("%s %016" PRIx64 " %" PRIu32 "\n", t.name, t.hash, t.payload_bytes);
      msg_free(&t);
      continue;
    }
    char label[512], hpath[1024], cpath[1024];
    msg_label(path, label, sizeof label);
    if (out) {
      snprintf(hpath, sizeof hpath, "%s/%s.h", out, t.name);
      snprintf(cpath, sizeof cpath, "%s/%s.c", out, t.name);
    } else {
      const char *slash = strrchr(path, '/');
      int dn = slash ? (int)(slash - path) : 1;
      const char *dir = slash ? path : ".";
      snprintf(hpath, sizeof hpath, "%.*s/%s.h", dn, dir, t.name);
      snprintf(cpath, sizeof cpath, "%.*s/%s.c", dn, dir, t.name);
    }
    struct buf h = {0}, c = {0};
    gen_header(&t, label, &h);
    gen_source(&t, label, &c);
    if (check) {
      if (check_file(hpath, &h) || check_file(cpath, &c)) rc = 1;
    } else {
      if (write_file(hpath, &h) || write_file(cpath, &c)) rc = 1;
    }
    buf_free(&h);
    buf_free(&c);
    msg_free(&t);
  }
  return rc;
}

#endif  // MSGGEN_NO_MAIN
