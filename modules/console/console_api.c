// SPDX-License-Identifier: GPL-3.0-or-later
/* Dawn Patrol: the manager's topics, the topic directory and the fallback
 * log, as JSON. Nothing here touches the network; dawn_patrold.c calls it
 * from one thread. What `hangar status`, `hangar topics` and `hangar echo
 * --json` print, with the same readings (hangar/hangar.c): topic headers by
 * offset from the mapped file, messages through lib/hg. */
#define _GNU_SOURCE
#include "console.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- The buffer ----------------------------------------------------------- */

static int cbuf_reserve(struct cbuf *b, size_t more)
{
	if (b->len + more + 1 <= b->cap)
		return 0;
	size_t cap = b->cap ? b->cap : 4096;
	while (cap < b->len + more + 1)
		cap *= 2;
	char *p = realloc(b->p, cap);
	if (!p)
		return -1;
	b->p = p;
	b->cap = cap;
	return 0;
}

int cbuf_printf(struct cbuf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0 || cbuf_reserve(b, (size_t)n) < 0) {
		va_end(ap2);
		return -1;
	}
	vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
	va_end(ap2);
	b->len += (size_t)n;
	return n;
}

void cbuf_json_str(struct cbuf *b, const char *s, size_t maxlen)
{
	size_t n = 0;
	while (n < maxlen && s[n])
		n++;
	if (cbuf_reserve(b, 2 + n * 6) < 0)
		return;
	b->p[b->len++] = '"';
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch == '"' || ch == '\\') {
			b->p[b->len++] = '\\';
			b->p[b->len++] = (char)ch;
		} else if (ch == '\n') {
			b->p[b->len++] = '\\';
			b->p[b->len++] = 'n';
		} else if (ch < 0x20 || ch == 0x7f) {
			b->len += (size_t)snprintf(b->p + b->len, 7, "\\u%04x", ch);
		} else {
			b->p[b->len++] = (char)ch;
		}
	}
	b->p[b->len++] = '"';
	b->p[b->len] = 0;
}

void cbuf_free(struct cbuf *b)
{
	free(b->p);
	b->p = NULL;
	b->len = b->cap = 0;
}

/* ---- Names ---------------------------------------------------------------- */

static const char *const state_names[8] = { "starting", "ready",   "unhealthy", "backoff",
					    "failed",   "stopped", "refused",   "stopping" };
static const char *const sys_names[3] = { "nominal", "degraded", "failsafe" };

static const char *state_name(unsigned s)
{
	return s < 8 ? state_names[s] : "unknown";
}

static double secs_since(uint64_t t, uint64_t now)
{
	return !t || t > now ? 0.0 : (double)(now - t) / 1e9;
}

/* ---- The manager's topics --------------------------------------------------- */

static struct modrec *mod_find(struct console *c, const char *name)
{
	for (int i = 0; i < c->n_mods; i++)
		if (strncmp(c->mods[i].m.name, name, sizeof c->mods[i].m.name) == 0)
			return &c->mods[i];
	return NULL;
}

static void mod_update(struct console *c, const struct hangar_modules *m, uint64_t now)
{
	struct modrec *r = mod_find(c, m->name);
	if (!r) {
		if (c->n_mods == CON_MAX_MODULES)
			return;
		r = &c->mods[c->n_mods++];
		memset(r, 0, sizeof *r);
	}
	/* A new, non-zero pid after another non-zero one: the module was
	 * started again, for whatever reason. */
	if (m->pid > 0 && r->last_pid > 0 && m->pid != r->last_pid)
		r->restarts++;
	if (m->pid > 0)
		r->last_pid = m->pid;
	r->m = *m;
	r->seen_ns = now;
}

void api_drain(struct console *c)
{
	uint64_t now = hg_now_ns();
	if (c->sub_system) {
		struct hangar_system s;
		struct hg_info info;
		if (hg_latest(c->sub_system, &s, sizeof s, &info) == HG_MSG) {
			c->sys = s;
			c->sys_info = info;
			c->have_sys = 1;
		}
	}
	if (c->sub_modules) {
		struct hangar_modules m;
		struct hg_info info;
		for (int n = 0; n < 4096; n++) {
			enum hg_result r = hg_next(c->sub_modules, &m, sizeof m, &info);
			if (r != HG_MSG)
				break;
			m.name[sizeof m.name - 1] = 0;
			mod_update(c, &m, now);
		}
	}
}

/* ---- Topic headers (docs/01, lib/hg/hg_topic.c struct hg_header) --------- */

#define H_SLOT_COUNT 24
#define H_PAYLOAD_BYTES 32
#define H_FLAGS 36
#define H_TOPIC_ID 56
#define H_RATE_MHZ 60
#define H_TYPE_HASH 64
#define H_WRITER 120
#define H_WRITER_PID 152
#define H_SCHEMA_BYTES 156
#define H_FIRST_SEQ 168
#define H_STALE_AFTER 176
#define H_WRITE_SEQ 200
#define H_LAST_PUBLISH 208

struct topic_row {
	char name[HG_NAME_MAX + 1];
	char writer[33];
	uint64_t type_hash, seq, first_seq, last_publish_ns, stale_after_ns;
	uint32_t slots, payload_bytes, flags, rate_mhz, topic_id, writer_pid, schema_bytes;
};

static uint64_t u64at(const unsigned char *h, size_t off)
{
	uint64_t v;
	memcpy(&v, h + off, 8);
	return v;
}

static uint32_t u32at(const unsigned char *h, size_t off)
{
	uint32_t v;
	memcpy(&v, h + off, 4);
	return v;
}

/* Reads one topic file's header into *row; -1 if it is not a topic. The
 * publish line is read as words, like `hangar topics` does: a torn read of
 * a counter costs one odd sample, never a crash. */
static int topic_read(const char *dir, const char *name, struct topic_row *row)
{
	char path[CON_PATH + HG_NAME_MAX + 2];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 4096) {
		close(fd);
		return -1;
	}
	unsigned char *h = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (h == MAP_FAILED)
		return -1;
	if (memcmp(h, "HGTOPIC1", 8) != 0) {
		munmap(h, 4096);
		return -1;
	}
	memset(row, 0, sizeof *row);
	snprintf(row->name, sizeof row->name, "%s", name);
	memcpy(row->writer, h + H_WRITER, 32);
	row->writer[32] = 0;
	row->type_hash = u64at(h, H_TYPE_HASH);
	row->seq = u64at(h, H_WRITE_SEQ);
	row->first_seq = u64at(h, H_FIRST_SEQ);
	row->last_publish_ns = u64at(h, H_LAST_PUBLISH);
	row->stale_after_ns = u64at(h, H_STALE_AFTER);
	row->slots = u32at(h, H_SLOT_COUNT);
	row->payload_bytes = u32at(h, H_PAYLOAD_BYTES);
	row->flags = u32at(h, H_FLAGS);
	row->rate_mhz = u32at(h, H_RATE_MHZ);
	row->topic_id = u32at(h, H_TOPIC_ID);
	row->writer_pid = u32at(h, H_WRITER_PID);
	row->schema_bytes = u32at(h, H_SCHEMA_BYTES);
	munmap(h, 4096);
	return 0;
}

static int cmp_row(const void *a, const void *b)
{
	return strcmp(((const struct topic_row *)a)->name, ((const struct topic_row *)b)->name);
}

/* Every topic file of the directory, sorted by name; the count. */
static int topics_scan(struct console *c, struct topic_row *rows, int max)
{
	DIR *d = opendir(c->topic_dir);
	if (!d)
		return -1;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < max) {
		if (e->d_name[0] == '.' || !hg_valid_name(e->d_name))
			continue;
		if (topic_read(c->topic_dir, e->d_name, &rows[n]) == 0)
			n++;
	}
	closedir(d);
	qsort(rows, (size_t)n, sizeof rows[0], cmp_row);
	return n;
}

static struct raterec *rate_find(struct console *c, const char *name)
{
	for (int i = 0; i < c->n_rates; i++)
		if (strcmp(c->rates[i].name, name) == 0)
			return &c->rates[i];
	return NULL;
}

static struct topic_row scan_rows[CON_MAX_TOPICS];

void api_tick(struct console *c)
{
	api_drain(c);
	uint64_t now = hg_now_ns();
	/* Modules the manager stopped describing (removed on a reload) go
	 * after a few rounds without a message. */
	for (int i = 0; i < c->n_mods;) {
		if (now - c->mods[i].seen_ns > 5000000000ull) {
			c->mods[i] = c->mods[--c->n_mods];
			continue;
		}
		i++;
	}
	/* Rates from the write_seq deltas since the last tick. */
	int n = topics_scan(c, scan_rows, CON_MAX_TOPICS);
	for (int i = 0; i < c->n_rates; i++)
		c->rates[i].seen = 0;
	for (int i = 0; i < n; i++) {
		struct raterec *r = rate_find(c, scan_rows[i].name);
		if (!r) {
			if (c->n_rates == CON_MAX_TOPICS)
				continue;
			r = &c->rates[c->n_rates++];
			memset(r, 0, sizeof *r);
			memcpy(r->name, scan_rows[i].name, sizeof r->name); /* both HG_NAME_MAX + 1 */
			r->hz = -1.0; /* not measured yet */
		} else if (r->t_ns && now > r->t_ns) {
			double dt = (double)(now - r->t_ns) / 1e9;
			r->hz = scan_rows[i].seq >= r->seq ? (double)(scan_rows[i].seq - r->seq) / dt : 0.0;
		}
		r->seq = scan_rows[i].seq;
		r->t_ns = now;
		r->seen = 1;
	}
	for (int i = 0; i < c->n_rates;) {
		if (!c->rates[i].seen) {
			c->rates[i] = c->rates[--c->n_rates];
			continue;
		}
		i++;
	}
	c->last_tick_ns = now;
}

/* ---- JSON ----------------------------------------------------------------- */

int api_system_json(struct console *c, struct cbuf *b)
{
	uint64_t now = hg_now_ns();
	cbuf_printf(b, "{\"role\":\"%s\",\"topic\":\"hangar.%s.system\",\"present\":%s", c->role, c->role,
		    c->have_sys ? "true" : "false");
	if (c->have_sys) {
		const struct hangar_system *s = &c->sys;
		cbuf_printf(b, ",\"state\":\"%s\",\"since_ns\":%llu,\"in_state_s\":%.1f,\"modules\":%u,"
			       "\"manager_pid\":%u,\"age_ms\":%.0f,\"seq\":%llu,\"reason\":",
			    s->state < 3 ? sys_names[s->state] : "unknown", (unsigned long long)s->since_ns,
			    secs_since(s->since_ns, now), s->modules, s->manager_pid,
			    secs_since(c->sys_info.publish_ns, now) * 1e3, (unsigned long long)c->sys_info.seq);
		cbuf_json_str(b, s->reason, sizeof s->reason);
	}
	cbuf_printf(b, "}\n");
	return 200;
}

int api_modules_json(struct console *c, struct cbuf *b)
{
	uint64_t now = hg_now_ns();
	cbuf_printf(b, "{\"role\":\"%s\",\"topic\":\"hangar.%s.modules\",\"modules\":[", c->role, c->role);
	for (int i = 0; i < c->n_mods; i++) {
		const struct modrec *r = &c->mods[i];
		const struct hangar_modules *m = &r->m;
		char ex[32];
		if (!m->exit_known)
			snprintf(ex, sizeof ex, "-");
		else if (m->last_exit >= 0)
			snprintf(ex, sizeof ex, "exit:%d", m->last_exit);
		else
			snprintf(ex, sizeof ex, "signal:%d", -m->last_exit);
		cbuf_printf(b, "%s{\"name\":", i ? "," : "");
		cbuf_json_str(b, m->name, sizeof m->name);
		cbuf_printf(b, ",\"state\":\"%s\",\"state_code\":%u,\"pid\":%d,\"failures\":%u,\"restarts\":%u,"
			       "\"last_exit\":\"%s\",\"since_ns\":%llu,\"in_state_s\":%.1f,\"age_ms\":%.0f,\"status\":",
			    state_name(m->state), m->state, m->pid, m->failures, r->restarts, ex,
			    (unsigned long long)m->since_ns, secs_since(m->since_ns, now),
			    secs_since(m->published_ns, now) * 1e3);
		cbuf_json_str(b, m->status, sizeof m->status);
		cbuf_printf(b, "}");
	}
	cbuf_printf(b, "]}\n");
	return 200;
}

static void flags_json(struct cbuf *b, const struct topic_row *r, uint64_t now)
{
	int first = 1;
	cbuf_printf(b, "[");
	if (r->flags & HG_FLAG_CLOSED)
		cbuf_printf(b, "%s\"closed\"", first-- > 0 ? "" : ",");
	if (r->flags & HG_FLAG_DIED)
		cbuf_printf(b, "%s\"died\"", first-- > 0 ? "" : ",");
	if (r->flags & HG_FLAG_REPLACED)
		cbuf_printf(b, "%s\"replaced\"", first-- > 0 ? "" : ",");
	if (r->rate_mhz && r->last_publish_ns && now > r->last_publish_ns &&
	    now - r->last_publish_ns > r->stale_after_ns)
		cbuf_printf(b, "%s\"stale\"", first-- > 0 ? "" : ",");
	cbuf_printf(b, "]");
}

int api_topics_json(struct console *c, struct cbuf *b)
{
	static struct topic_row rows[CON_MAX_TOPICS];
	int n = topics_scan(c, rows, CON_MAX_TOPICS);
	uint64_t now = hg_now_ns();
	cbuf_printf(b, "{\"dir\":");
	cbuf_json_str(b, c->topic_dir, sizeof c->topic_dir);
	if (n < 0) {
		cbuf_printf(b, ",\"error\":\"cannot read the topic directory: %s\",\"topics\":[]}\n", strerror(errno));
		return 200;
	}
	cbuf_printf(b, ",\"topics\":[");
	for (int i = 0; i < n; i++) {
		const struct topic_row *r = &rows[i];
		const struct raterec *rate = rate_find(c, r->name);
		cbuf_printf(b, "%s{\"name\":\"%s\",\"type_hash\":\"%016llx\",\"topic_id\":%u,\"rate_hz\":%.3f,"
			       "\"measured_hz\":",
			    i ? "," : "", r->name, (unsigned long long)r->type_hash, r->topic_id,
			    r->rate_mhz / 1000.0);
		if (rate && rate->hz >= 0.0)
			cbuf_printf(b, "%.2f", rate->hz);
		else
			cbuf_printf(b, "null");
		cbuf_printf(b, ",\"age_ms\":");
		if (r->last_publish_ns)
			cbuf_printf(b, "%.0f", secs_since(r->last_publish_ns, now) * 1e3);
		else
			cbuf_printf(b, "null");
		cbuf_printf(b, ",\"stale_after_ms\":%.0f,\"writer\":", (double)r->stale_after_ns / 1e6);
		cbuf_json_str(b, r->writer, 32);
		cbuf_printf(b, ",\"writer_pid\":%u,\"slots\":%u,\"payload_bytes\":%u,\"schema_bytes\":%u,"
			       "\"seq\":%llu,\"first_seq\":%llu,\"flags\":",
			    r->writer_pid, r->slots, r->payload_bytes, r->schema_bytes, (unsigned long long)r->seq,
			    (unsigned long long)r->first_seq);
		flags_json(b, r, now);
		cbuf_printf(b, "}");
	}
	cbuf_printf(b, "]}\n");
	return 200;
}

static const char *kind_name(enum hg_field_kind k)
{
	static const char *const names[] = { "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64", "char" };
	return (unsigned)k < 11 ? names[k] : "?";
}

static void scalar_json(struct cbuf *b, enum hg_field_kind k, const unsigned char *q)
{
	switch (k) {
	case HG_U8: cbuf_printf(b, "%u", *q); break;
	case HG_I8: cbuf_printf(b, "%d", *(const signed char *)q); break;
	case HG_U16: { uint16_t v; memcpy(&v, q, 2); cbuf_printf(b, "%u", v); break; }
	case HG_I16: { int16_t v; memcpy(&v, q, 2); cbuf_printf(b, "%d", v); break; }
	case HG_U32: { uint32_t v; memcpy(&v, q, 4); cbuf_printf(b, "%u", v); break; }
	case HG_I32: { int32_t v; memcpy(&v, q, 4); cbuf_printf(b, "%d", v); break; }
	case HG_U64: { uint64_t v; memcpy(&v, q, 8); cbuf_printf(b, "%llu", (unsigned long long)v); break; }
	case HG_I64: { int64_t v; memcpy(&v, q, 8); cbuf_printf(b, "%lld", (long long)v); break; }
	case HG_F32: {
		float v;
		memcpy(&v, q, 4);
		if (isfinite(v))
			cbuf_printf(b, "%.7g", (double)v);
		else
			cbuf_printf(b, "null"); /* JSON has no NaN or infinity */
		break;
	}
	case HG_F64: {
		double v;
		memcpy(&v, q, 8);
		if (isfinite(v))
			cbuf_printf(b, "%.15g", v);
		else
			cbuf_printf(b, "null");
		break;
	}
	default: cbuf_printf(b, "null"); break;
	}
}

/* A field's value: a string for char[N], a scalar, an array, or for an array
 * past 64 elements (a frame's pixels) its size as a string. */
#define CON_MAX_ARRAY 64
static void field_json(struct cbuf *b, const struct hg_field *f, const unsigned char *p)
{
	if (f->kind == HG_CHAR) {
		cbuf_json_str(b, (const char *)p, f->count);
		return;
	}
	if (f->count == 1) {
		scalar_json(b, f->kind, p);
		return;
	}
	if (f->count > CON_MAX_ARRAY) {
		cbuf_printf(b, "\"%s[%u]: %u bytes\"", kind_name(f->kind), f->count, f->count * hg_field_elem_size(f->kind));
		return;
	}
	cbuf_printf(b, "[");
	for (uint32_t i = 0; i < f->count; i++) {
		if (i)
			cbuf_printf(b, ",");
		scalar_json(b, f->kind, p + i * hg_field_elem_size(f->kind));
	}
	cbuf_printf(b, "]");
}

/* The newest message of a topic, decoded by the schema in its header, as
 * `hangar echo --json` does, the fields in an array so that their order,
 * type and comment travel with them. The subscription is opened for the
 * request alone: under the manager the wiring's `[Subscribe *]` lets any
 * topic be opened by name. */
int api_topic_json(struct console *c, const char *name, struct cbuf *b)
{
	if (!hg_valid_name(name)) {
		cbuf_printf(b, "{\"error\":\"not a topic name\"}\n");
		return 404;
	}
	char path[CON_PATH + HG_NAME_MAX + 2];
	snprintf(path, sizeof path, "%s/%s", c->topic_dir, name);
	if (access(path, F_OK) < 0) {
		cbuf_printf(b, "{\"error\":\"no topic %s\"}\n", name);
		return 404;
	}
	hg_sub *s = hg_sub_open(name, NULL);
	if (!s) {
		cbuf_printf(b, "{\"error\":\"cannot open %s: %s\"}\n", name, strerror(errno));
		return errno == EACCES ? 403 : 500;
	}
	uint64_t now = hg_now_ns();
	cbuf_printf(b, "{\"topic\":\"%s\"", name);
	uint32_t pl = hg_sub_payload_bytes(s);
	if (!pl) {
		cbuf_printf(b, ",\"present\":false,\"error\":\"the topic is not attached\"}\n");
		hg_sub_close(s);
		return 200;
	}
	unsigned char *buf = malloc(pl);
	struct hg_schema *schema = calloc(1, sizeof *schema);
	if (!buf || !schema) {
		free(buf);
		free(schema);
		hg_sub_close(s);
		b->len = 0;
		cbuf_printf(b, "{\"error\":\"out of memory\"}\n");
		return 500;
	}
	struct hg_topic_meta meta;
	if (hg_sub_meta(s, &meta) == 0)
		cbuf_printf(b, ",\"type_hash\":\"%016llx\",\"writer\":\"%.32s\",\"payload_bytes\":%u",
			    (unsigned long long)meta.type_hash, meta.writer, meta.payload_bytes);
	uint32_t slen = 0;
	const char *text = hg_sub_schema(s, &slen);
	char err[128] = "";
	int parsed = text && hg_schema_parse(text, slen, schema, err, sizeof err) == 0;
	if (!parsed)
		cbuf_printf(b, ",\"schema_error\":\"%s\"", err[0] ? err : "no schema");
	struct hg_info info;
	enum hg_result r = hg_latest(s, buf, pl, &info);
	if (r != HG_MSG) {
		cbuf_printf(b, ",\"present\":false,\"result\":\"%s\"}\n",
			    r == HG_NONE ? "no message yet" : r == HG_CLOSED ? "closed" : r == HG_ABSENT ? "absent" : "error");
	} else {
		cbuf_printf(b, ",\"present\":true,\"seq\":%llu,\"stamp_ns\":%llu,\"publish_ns\":%llu,\"age_ms\":%.0f,"
			       "\"stale\":%s,\"gap\":%llu,\"fields\":[",
			    (unsigned long long)info.seq, (unsigned long long)info.stamp_ns,
			    (unsigned long long)info.publish_ns, secs_since(info.publish_ns, now) * 1e3,
			    info.flags & HG_SLOT_STALE ? "true" : "false", (unsigned long long)info.gap);
		if (parsed)
			for (uint32_t i = 0; i < schema->type.n_fields; i++) {
				const struct hg_field *f = &schema->fields[i];
				if (f->offset + f->count * hg_field_elem_size(f->kind) > pl)
					break;
				cbuf_printf(b, "%s{\"name\":\"%s\",\"type\":\"%s\",\"count\":%u,\"comment\":", i ? "," : "",
					    f->name, kind_name(f->kind), f->count);
				cbuf_json_str(b, f->comment, 256);
				cbuf_printf(b, ",\"value\":");
				field_json(b, f, buf + f->offset);
				cbuf_printf(b, "}");
			}
		cbuf_printf(b, "]}\n");
	}
	free(buf);
	free(schema);
	hg_sub_close(s);
	return 200;
}

/* ---- The fallback log --------------------------------------------------------- */

#define CON_LOG_TAIL 16384

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The last `lines` lines of a file (within its last 16 KB) as a JSON string. */
static void tail_json(struct cbuf *b, const char *path, int lines, long *bytes)
{
	static char buf[CON_LOG_TAIL + 1];
	*bytes = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		cbuf_printf(b, "\"\"");
		return;
	}
	struct stat st;
	off_t off = 0;
	if (fstat(fd, &st) == 0) {
		*bytes = (long)st.st_size;
		if (st.st_size > (off_t)CON_LOG_TAIL)
			off = st.st_size - CON_LOG_TAIL;
	}
	ssize_t n = pread(fd, buf, CON_LOG_TAIL, off);
	close(fd);
	if (n <= 0) {
		cbuf_printf(b, "\"\"");
		return;
	}
	buf[n] = 0;
	/* Back up over `lines` newlines from the end (the trailing one aside). */
	const char *start = buf + n;
	int seen = 0;
	if (start > buf && start[-1] == '\n')
		start--;
	while (start > buf) {
		if (start[-1] == '\n' && ++seen == lines)
			break;
		start--;
	}
	if (off && start == buf) {
		const char *nl = strchr(buf, '\n'); /* a cut line: drop it */
		if (nl)
			start = nl + 1;
	}
	cbuf_json_str(b, start, (size_t)(buf + n - start));
}

int api_log_json(struct console *c, int lines, struct cbuf *b)
{
	if (lines < 1)
		lines = 1;
	if (lines > 1000)
		lines = 1000;
	int journald = access(c->journal_path, F_OK) == 0;
	cbuf_printf(b, "{\"journald\":%s,\"dir\":", journald ? "true" : "false");
	cbuf_json_str(b, c->log_dir, sizeof c->log_dir);
	cbuf_printf(b, ",\"note\":");
	if (journald)
		cbuf_printf(b, "\"journald holds the modules' output: journalctl -t <module>, or journalctl -u hangard\"");
	else
		cbuf_printf(b, "\"no journald: the manager writes each module's output to <dir>/<module>.log\"");
	cbuf_printf(b, ",\"files\":[");
	DIR *d = opendir(c->log_dir);
	if (d) {
		char *names[CON_MAX_MODULES];
		int n = 0;
		struct dirent *e;
		while ((e = readdir(d)) && n < CON_MAX_MODULES) {
			size_t len = strlen(e->d_name);
			if (e->d_name[0] == '.' || len < 5 || strcmp(e->d_name + len - 4, ".log") != 0)
				continue;
			names[n] = strdup(e->d_name);
			if (names[n])
				n++;
		}
		closedir(d);
		qsort(names, (size_t)n, sizeof names[0], cmp_name);
		for (int i = 0; i < n; i++) {
			char path[CON_PATH + 300];
			snprintf(path, sizeof path, "%s/%s", c->log_dir, names[i]);
			names[i][strlen(names[i]) - 4] = 0;
			cbuf_printf(b, "%s{\"name\":", i ? "," : "");
			cbuf_json_str(b, names[i], 64);
			cbuf_printf(b, ",\"tail\":");
			long bytes;
			tail_json(b, path, lines, &bytes);
			cbuf_printf(b, ",\"bytes\":%ld}", bytes);
			free(names[i]);
		}
	}
	cbuf_printf(b, "]}\n");
	return 200;
}
