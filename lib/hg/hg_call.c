// SPDX-License-Identifier: LGPL-3.0-or-later
/* Calls over topics (docs/04-calls.md). One module per process: the
 * caller's state (its `.req` output, the `.rep` topics it polls, the calls
 * in flight) and the server's (its `.rep` output, the request topics it
 * reads, the handlers) are static and initialized the first time they are
 * needed, from the wiring. No allocation after that first use. */
#define _GNU_SOURCE
#include "hg_call.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

_Static_assert(sizeof(struct hg_call_request_envelope) == HG_CALL_REQ_ENVELOPE, "request envelope");
_Static_assert(offsetof(struct hg_call_request_envelope, method) == 32, "request envelope");
_Static_assert(offsetof(struct hg_call_request_envelope, caller) == 64, "request envelope");
_Static_assert(sizeof(struct hg_call_reply_envelope) == HG_CALL_REP_ENVELOPE, "reply envelope");
_Static_assert(offsetof(struct hg_call_reply_envelope, status) == 12, "reply envelope");
_Static_assert(offsetof(struct hg_call_reply_envelope, reason) == 32, "reply envelope");

#define DEFAULT_TIMEOUT_NS 100000000ull
#define ANSWERED_MAX 64

const char *hg_status_name(int status)
{
	switch (status) {
	case HG_OK: return "HG_OK";
	case HG_PENDING: return "HG_PENDING";
	case HG_E_REFUSED: return "HG_E_REFUSED";
	case HG_E_DENIED: return "HG_E_DENIED";
	case HG_E_NOMETHOD: return "HG_E_NOMETHOD";
	case HG_E_BADTYPE: return "HG_E_BADTYPE";
	case HG_E_EXPIRED: return "HG_E_EXPIRED";
	case HG_E_BUSY: return "HG_E_BUSY";
	case HG_E_NOROUTE: return "HG_E_NOROUTE";
	case HG_E_TIMEOUT: return "HG_E_TIMEOUT";
	default: return "HG_E_?";
	}
}

/* ---- The generated types ----------------------------------------------- */

/* The same text the manager generates (hangard/manifest.c): the hash
 * covers the canonical form only, but the header then shows the same
 * schema whoever created the file. */
int hg_call_req_schema(const char *module, uint32_t largest, char *out, size_t outlen)
{
	if (largest < 8)
		largest = 8;
	return bfmt(out, outlen,
			"# req:%s: the request envelope (docs/04), then the largest request\n"
			"# among the methods %s calls\n"
			"u64  call_id        # per caller, counting from 1\n"
			"u64  deadline_ns    # CLOCK_MONOTONIC: when the caller stops waiting\n"
			"u64  request_type   # type hash\n"
			"u32  target_id      # topic id of the server's <server>.rep\n"
			"u32  reserved       # 0\n"
			"char method[32]\n"
			"char caller[48]     # empty; set by the manager on hangar.<role>.admin.req only\n"
			"u8   request[%u]\n",
			module, module, largest);
}

int hg_call_rep_schema(const char *module, uint32_t largest, char *out, size_t outlen)
{
	if (largest < 8)
		largest = 8;
	return bfmt(out, outlen,
			"# rep:%s: the reply envelope (docs/04), then the largest reply among\n"
			"# the methods %s serves\n"
			"u64  call_id\n"
			"u32  caller_id      # topic id of the request topic the request was read from\n"
			"i32  status         # HG_OK or an error (docs/04 Status codes)\n"
			"u64  reply_type     # type hash\n"
			"u64  received_ns    # CLOCK_MONOTONIC: when the server read the request\n"
			"char reason[64]     # one line, when not HG_OK\n"
			"u8   reply[%u]\n",
			module, module, largest);
}

/* The bytes a C struct of this type occupies before the payload's
 * padding: what a module's struct can be copied from or into. */
static uint32_t type_extent(const struct hg_type *t)
{
	uint32_t e = 0;
	for (uint32_t i = 0; i < t->n_fields; i++) {
		const struct hg_field *f = &t->fields[i];
		uint32_t end = f->offset + hg_field_elem_size(f->kind) * f->count;
		if (end > e)
			e = end;
	}
	return e;
}

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("hg_call: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- Caller state ------------------------------------------------------ */

struct rep_topic {
	char server[32];
	hg_sub *sub;
	uint32_t topic_id;
};

struct hg_pending {
	int in_use;
	uint64_t call_id;
	uint64_t started_ns;
	uint64_t deadline_ns;
	const struct hg_type *rep_type;
	int srv;                /* index into reps[] */
	int have_reply;
	struct hg_call_reply_envelope env;
	unsigned char *body;    /* into rep_pool, max_rep bytes */
};

static struct {
	int inited;
	int failed;             /* errno of a failed init */
	hg_pub *pub;
	uint32_t topic_id;
	struct hg_schema schema;
	unsigned char *rep_pool;
	uint32_t max_rep;
	struct rep_topic reps[HG_WIRING_MAX_CALLS];
	int n_reps;
	struct hg_pending pend[HG_CALL_IN_FLIGHT];
	uint64_t next_call_id;
	unsigned char buf[HG_CALL_REP_ENVELOPE + HG_CALL_MAX_PAYLOAD]; /* one reply, read */
	unsigned char req[HG_CALL_REQ_ENVELOPE + HG_CALL_MAX_PAYLOAD]; /* one request, built */
} C;

static const struct hg_wiring_call *find_call(const struct hg_wiring *w, const char *target)
{
	for (int i = 0; i < w->n_calls; i++)
		if (strcmp(w->calls[i].target, target) == 0)
			return &w->calls[i];
	return NULL;
}

static const struct hg_wiring_serve *find_serve(const struct hg_wiring *w, const char *method)
{
	for (int i = 0; i < w->n_serves; i++)
		if (strcmp(w->serves[i].method, method) == 0)
			return &w->serves[i];
	return NULL;
}

static int rep_topic_index(const char *server)
{
	for (int i = 0; i < C.n_reps; i++)
		if (strcmp(C.reps[i].server, server) == 0)
			return i;
	return -1;
}

static int caller_init(void)
{
	if (C.inited)
		return 0;
	if (C.failed) {
		errno = C.failed;
		return -1;
	}
	const struct hg_wiring *w = hg_wiring();
	if (hg_init() < 0 || !w) {
		C.failed = errno = EACCES;
		return -1;
	}
	char topic[HG_WIRING_NAME + 8];
	bfmt(topic, sizeof topic, "%s.req", w->module);
	const struct hg_wiring_pub *line = hg_wiring_find_pub(w, topic);
	if (!line || w->n_calls == 0) {
		C.failed = errno = EACCES; /* the module declares no call */
		return -1;
	}
	uint32_t largest_req = 8, largest_rep = 8;
	for (int i = 0; i < w->n_calls; i++) {
		if (w->calls[i].m.req_size > largest_req)
			largest_req = w->calls[i].m.req_size;
		if (w->calls[i].m.rep_size > largest_rep)
			largest_rep = w->calls[i].m.rep_size;
	}
	static char text[HG_SCHEMA_MAX];
	int n = hg_call_req_schema(w->module, largest_req, text, sizeof text);
	char err[160];
	if (n <= 0 || n >= (int)sizeof text ||
	    hg_schema_parse(text, (size_t)n, &C.schema, err, sizeof err) < 0) {
		log_line("cannot build req:%s: %s", w->module, err);
		C.failed = errno = EINVAL;
		return -1;
	}
	C.schema.type.name = "req";
	if (line->hash && line->hash != C.schema.type.hash) {
		log_line("req:%s: the wiring's hash %016llx differs from the library's %016llx", w->module,
			 (unsigned long long)line->hash, (unsigned long long)C.schema.type.hash);
		C.failed = errno = EPROTOTYPE;
		return -1;
	}
	/* The reply topics, read in order from the oldest reply still there. A
	 * call nothing serves on either side has no reply topic to read. */
	for (int i = 0; i < w->n_calls; i++) {
		if (w->calls[i].noroute)
			continue;
		const char *dot = strchr(w->calls[i].target, '.');
		char server[32];
		bfmt(server, sizeof server, "%.*s", (int)(dot - w->calls[i].target), w->calls[i].target);
		if (rep_topic_index(server) >= 0)
			continue;
		struct rep_topic *rt = &C.reps[C.n_reps];
		bfmt(rt->server, sizeof rt->server, "%s", server);
		bfmt(topic, sizeof topic, "%s.rep", server);
		rt->sub = hg_sub_open_from(topic, NULL, HG_OLDEST);
		if (!rt->sub) {
			int e = errno;
			log_line("cannot subscribe to %s: %s", topic, strerror(e));
			C.failed = errno = e;
			return -1;
		}
		rt->topic_id = hg_topic_id(topic);
		C.n_reps++;
	}
	/* One buffer per call in flight for its reply: the one allocation, here. */
	C.max_rep = largest_rep;
	C.rep_pool = calloc(HG_CALL_IN_FLIGHT, largest_rep);
	if (!C.rep_pool) {
		C.failed = errno = ENOMEM;
		return -1;
	}
	for (int i = 0; i < HG_CALL_IN_FLIGHT; i++)
		C.pend[i].body = C.rep_pool + (size_t)i * largest_rep;
	bfmt(topic, sizeof topic, "%s.req", w->module);
	C.pub = hg_pub_open(topic, &C.schema.type, NULL);
	if (!C.pub) {
		int e = errno;
		log_line("cannot open %s: %s", topic, strerror(e));
		C.failed = errno = e;
		return -1;
	}
	C.topic_id = hg_pub_topic_id(C.pub);
	C.next_call_id = 1;
	C.inited = 1;
	return 0;
}

const struct hg_type *hg_call_req_type(void)
{
	return caller_init() < 0 ? NULL : &C.schema.type;
}

static int module_has_actuator(void)
{
	const struct hg_wiring *w = hg_wiring();
	if (!w)
		return 0;
	for (int i = 0; i < w->n_pubs; i++)
		if (w->pubs[i].actuator)
			return 1;
	return 0;
}

int hg_call_start(const char *target, const struct hg_type *req_type, const void *req,
		  const struct hg_type *rep_type, hg_pending **out, int timeout_ms)
{
	if (out)
		*out = NULL;
	if (!target || !req_type || !rep_type || !out) {
		errno = EINVAL;
		return HG_E_NOMETHOD;
	}
	if (caller_init() < 0)
		return HG_E_NOMETHOD;
	const struct hg_wiring *w = hg_wiring();
	const struct hg_wiring_call *line = find_call(w, target);
	if (!line) {
		errno = EACCES;
		return HG_E_NOMETHOD;
	}
	if (line->noroute)
		return HG_E_NOROUTE;
	if (req_type->hash != line->m.req_hash || rep_type->hash != line->m.rep_hash)
		return HG_E_BADTYPE;
	uint32_t extent = type_extent(req_type);
	if (extent && !req) {
		errno = EINVAL;
		return HG_E_BADTYPE;
	}
	struct hg_pending *pc = NULL;
	for (int i = 0; i < HG_CALL_IN_FLIGHT; i++)
		if (!C.pend[i].in_use) {
			pc = &C.pend[i];
			break;
		}
	if (!pc)
		return HG_E_BUSY;
	const char *dot = strchr(target, '.');
	char server[32];
	bfmt(server, sizeof server, "%.*s", (int)(dot - target), target);
	int srv = rep_topic_index(server);
	if (srv < 0) {
		errno = EACCES;
		return HG_E_NOMETHOD;
	}
	uint64_t now = hg_now_ns();
	uint64_t timeout = timeout_ms > 0 ? (uint64_t)timeout_ms * 1000000u : DEFAULT_TIMEOUT_NS;
	struct hg_call_request_envelope env;
	memset(&env, 0, sizeof env);
	env.call_id = C.next_call_id++;
	env.deadline_ns = now + timeout;
	env.request_type = req_type->hash;
	env.target_id = C.reps[srv].topic_id;
	bfmt(env.method, sizeof env.method, "%s", dot + 1);
	memset(C.req, 0, sizeof C.req);
	memcpy(C.req, &env, sizeof env);
	if (extent)
		memcpy(C.req + HG_CALL_REQ_ENVELOPE, req, extent);
	if (hg_publish_with(C.pub, C.req, C.schema.type.payload_bytes, now, HG_NO_REFS, NULL, 0) < 0) {
		int e = errno;
		log_line("cannot publish the request for %s: %s", target, strerror(e));
		errno = e;
		return HG_E_BUSY;
	}
	memset(pc, 0, offsetof(struct hg_pending, body));
	pc->in_use = 1;
	pc->call_id = env.call_id;
	pc->started_ns = now;
	pc->deadline_ns = env.deadline_ns;
	pc->rep_type = rep_type;
	pc->srv = srv;
	*out = pc;
	return HG_OK;
}

/* Reads everything new on a reply topic, keeping the replies to this
 * module's calls in flight; the rest (other callers', timed-out calls') is
 * passed over. A reply received before the request was published cannot be
 * its answer: a restarted caller counts its call ids from 1 again. */
static void scan_replies(int srv)
{
	struct rep_topic *rt = &C.reps[srv];
	struct hg_info info;
	for (;;) {
		enum hg_result r = hg_next(rt->sub, C.buf, sizeof C.buf, &info);
		if (r != HG_MSG)
			break;
		uint32_t pl = hg_sub_payload_bytes(rt->sub);
		if (pl < HG_CALL_REP_ENVELOPE)
			continue;
		struct hg_call_reply_envelope env;
		memcpy(&env, C.buf, sizeof env);
		if (env.caller_id != C.topic_id)
			continue;
		for (int i = 0; i < HG_CALL_IN_FLIGHT; i++) {
			struct hg_pending *p = &C.pend[i];
			if (!p->in_use || p->have_reply || p->srv != srv || p->call_id != env.call_id ||
			    env.received_ns < p->started_ns)
				continue;
			p->env = env;
			uint32_t body = pl - HG_CALL_REP_ENVELOPE;
			if (body > C.max_rep)
				body = C.max_rep;
			memcpy(p->body, C.buf + HG_CALL_REP_ENVELOPE, body);
			p->have_reply = 1;
			break;
		}
	}
}

hg_call_result hg_call_poll(hg_pending *pc, void *rep)
{
	hg_call_result r;
	memset(&r, 0, sizeof r);
	if (!pc || !pc->in_use) {
		r.status = HG_E_TIMEOUT;
		bfmt(r.reason, sizeof r.reason, "not a call in flight");
		return r;
	}
	r.call_id = pc->call_id;
	if (!pc->have_reply)
		scan_replies(pc->srv);
	if (pc->have_reply) {
		r.status = pc->env.status;
		r.received_ns = pc->env.received_ns;
		bfmt(r.reason, sizeof r.reason, "%.63s", pc->env.reason);
		if (r.status == HG_OK) {
			if (pc->env.reply_type != pc->rep_type->hash) {
				r.status = HG_E_BADTYPE;
				bfmt(r.reason, sizeof r.reason, "reply type %016llx, expected %016llx",
					 (unsigned long long)pc->env.reply_type,
					 (unsigned long long)pc->rep_type->hash);
			} else if (rep) {
				uint32_t extent = type_extent(pc->rep_type);
				if (extent > C.max_rep)
					extent = C.max_rep;
				memcpy(rep, pc->body, extent);
			}
		}
		pc->in_use = 0;
		return r;
	}
	if (hg_now_ns() >= pc->deadline_ns) {
		r.status = HG_E_TIMEOUT;
		bfmt(r.reason, sizeof r.reason, "no reply within the timeout");
		pc->in_use = 0;
		return r;
	}
	r.status = HG_PENDING;
	return r;
}

hg_call_result hg_call(const char *target, const struct hg_type *req_type, const void *req,
		       const struct hg_type *rep_type, void *rep, int timeout_ms)
{
	hg_call_result r;
	memset(&r, 0, sizeof r);
	if (module_has_actuator()) {
		r.status = HG_E_REFUSED;
		bfmt(r.reason, sizeof r.reason, "hg_call is refused in a module with an actuator output");
		return r;
	}
	hg_pending *pc;
	int st = hg_call_start(target, req_type, req, rep_type, &pc, timeout_ms);
	if (st != HG_OK) {
		r.status = st;
		bfmt(r.reason, sizeof r.reason, "%s", st == HG_E_NOMETHOD ? "not a call this module declares" :
							       st == HG_E_NOROUTE ? "nothing serves it on either side" :
							       st == HG_E_BUSY ? "15 calls already in flight" :
							       "request or reply type differs from the wiring's");
		return r;
	}
	hg_sub *sub = C.reps[pc->srv].sub;
	for (;;) {
		r = hg_call_poll(pc, rep);
		if (r.status != HG_PENDING)
			return r;
		uint64_t now = hg_now_ns();
		int ms = now >= pc->deadline_ns ? 0 : (int)((pc->deadline_ns - now + 999999u) / 1000000u);
		hg_wait(&sub, 1, ms);
	}
}

/* ---- Server state ------------------------------------------------------ */

struct method {
	char name[32];
	hg_handler fn;
	void *ctx;
	void (*after)(void *ctx);
};

struct answered {
	uint32_t caller_id;
	uint64_t call_id;
	uint64_t received_ns;
};

static struct {
	int inited;
	int failed;
	hg_pub *pub;
	uint32_t topic_id;
	struct hg_schema schema;
	char role[16];
	hg_sub *subs[HG_WIRING_MAX_SUBS];
	int n_subs;
	hg_sub *glob;           /* the implicit subglob *.req, or NULL */
	struct method methods[HG_WIRING_MAX_SERVES];
	int n_methods;
	struct answered answered[ANSWERED_MAX];
	int n_answered;
	int rr;                 /* the topic the next pass starts with */
	int no_ref_logged;
	unsigned char req[HG_CALL_REQ_ENVELOPE + HG_CALL_MAX_PAYLOAD];
	unsigned char rep[HG_CALL_REP_ENVELOPE + HG_CALL_MAX_PAYLOAD];
} S;

static int ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), k = strlen(suffix);
	return n > k && strcmp(s + n - k, suffix) == 0;
}

static struct method *find_method(const char *name)
{
	for (int i = 0; i < S.n_methods; i++)
		if (strcmp(S.methods[i].name, name) == 0)
			return &S.methods[i];
	return NULL;
}

/* A restarted server: the replies its predecessor left in the `.rep` ring
 * say which requests are already answered (docs/04 "While the server
 * restarts"). */
static void read_own_replies(const char *topic)
{
	hg_sub *own = hg_sub_open_own(topic, NULL, HG_OLDEST);
	if (!own)
		return;
	struct hg_info info;
	for (;;) {
		enum hg_result r = hg_next(own, S.rep, sizeof S.rep, &info);
		if (r != HG_MSG)
			break;
		if (hg_sub_payload_bytes(own) < HG_CALL_REP_ENVELOPE)
			continue;
		struct hg_call_reply_envelope env;
		memcpy(&env, S.rep, sizeof env);
		struct answered *a = &S.answered[S.n_answered < ANSWERED_MAX ? S.n_answered : ANSWERED_MAX - 1];
		if (S.n_answered < ANSWERED_MAX)
			S.n_answered++;
		a->caller_id = env.caller_id;
		a->call_id = env.call_id;
		a->received_ns = env.received_ns;
	}
	hg_sub_close(own);
	if (S.n_answered)
		log_line("%s: %d replies of the previous instance found in the ring; their requests are not run again",
			 topic, S.n_answered);
}

static int already_answered(uint32_t caller_id, uint64_t call_id, uint64_t request_publish_ns)
{
	for (int i = 0; i < S.n_answered; i++)
		if (S.answered[i].caller_id == caller_id && S.answered[i].call_id == call_id &&
		    S.answered[i].received_ns >= request_publish_ns)
			return 1;
	return 0;
}

static int server_init(void)
{
	if (S.inited)
		return 0;
	if (S.failed) {
		errno = S.failed;
		return -1;
	}
	const struct hg_wiring *w = hg_wiring();
	if (hg_init() < 0 || !w) {
		S.failed = errno = EACCES;
		return -1;
	}
	char topic[HG_WIRING_NAME + 8];
	bfmt(topic, sizeof topic, "%s.rep", w->module);
	const struct hg_wiring_pub *line = hg_wiring_find_pub(w, topic);
	if (!line) {
		S.failed = errno = EACCES; /* the module serves nothing */
		return -1;
	}
	bfmt(S.role, sizeof S.role, "%s", w->role[0] ? w->role : "vehicle");
	uint32_t largest = 8;
	for (int i = 0; i < w->n_serves; i++)
		if (w->serves[i].m.rep_size > largest)
			largest = w->serves[i].m.rep_size;
	static char text[HG_SCHEMA_MAX];
	int n = hg_call_rep_schema(w->module, largest, text, sizeof text);
	char err[160];
	if (n <= 0 || n >= (int)sizeof text ||
	    hg_schema_parse(text, (size_t)n, &S.schema, err, sizeof err) < 0) {
		log_line("cannot build rep:%s: %s", w->module, err);
		S.failed = errno = EINVAL;
		return -1;
	}
	S.schema.type.name = "rep";
	if (line->hash && line->hash != S.schema.type.hash) {
		log_line("rep:%s: the wiring's hash %016llx differs from the library's %016llx", w->module,
			 (unsigned long long)line->hash, (unsigned long long)S.schema.type.hash);
		S.failed = errno = EPROTOTYPE;
		return -1;
	}
	/* What the predecessor answered, before the file is replaced. */
	read_own_replies(topic);
	S.pub = hg_pub_open(topic, &S.schema.type, NULL);
	if (!S.pub) {
		int e = errno;
		log_line("cannot open %s: %s", topic, strerror(e));
		S.failed = errno = e;
		return -1;
	}
	S.topic_id = hg_pub_topic_id(S.pub);
	/* The request topics: every named `.req` subscription, from the oldest
	 * request still in its ring, and the glob of mirrored ones. */
	for (int i = 0; i < w->n_subs; i++) {
		if (!ends_with(w->subs[i].topic, ".req"))
			continue;
		hg_sub *s = hg_sub_open_from(w->subs[i].topic, NULL, HG_OLDEST);
		if (!s) {
			int e = errno;
			log_line("cannot subscribe to %s: %s", w->subs[i].topic, strerror(e));
			S.failed = errno = e;
			return -1;
		}
		S.subs[S.n_subs++] = s;
	}
	if (hg_wiring_find_glob(w, "*.req"))
		S.glob = hg_sub_open_from("*.req", NULL, HG_OLDEST);
	S.inited = 1;
	return 0;
}

const struct hg_type *hg_call_rep_type(void)
{
	return server_init() < 0 ? NULL : &S.schema.type;
}

int hg_serve_method(const char *name, hg_handler fn, void *ctx)
{
	const struct hg_wiring *w;
	if (!name || !fn || hg_init() < 0 || !(w = hg_wiring()) || !find_serve(w, name)) {
		errno = EACCES;
		return -1;
	}
	if (find_method(name)) {
		errno = EEXIST;
		return -1;
	}
	if (S.n_methods == HG_WIRING_MAX_SERVES) {
		errno = ENOSPC;
		return -1;
	}
	struct method *m = &S.methods[S.n_methods++];
	memset(m, 0, sizeof *m);
	bfmt(m->name, sizeof m->name, "%s", name);
	m->fn = fn;
	m->ctx = ctx;
	return 0;
}

int hg_serve_after(const char *name, void (*after)(void *ctx))
{
	struct method *m = find_method(name);
	if (!m) {
		errno = ENOENT;
		return -1;
	}
	m->after = after;
	return 0;
}

void hg_call_caller_token(const hg_sub *sub, const struct hg_call_request_envelope *env,
			  const char *my_role, char *out)
{
	struct hg_topic_meta meta;
	out[0] = 0;
	if (hg_sub_meta(sub, &meta) < 0) {
		bfmt(out, HG_CALL_CALLER, "?");
		return;
	}
	int mirrored = 0;
	for (int i = 0; i < 16; i++)
		if (meta.origin_boot[i])
			mirrored = 1;
	const char *name = hg_sub_name(sub);
	char writer[33];
	bfmt(writer, sizeof writer, "%.32s", meta.writer);
	/* hangar.<role>.admin.req, written by the manager: the one topic whose
	 * `caller` field is trusted, naming the operator. */
	if (strncmp(name, "hangar.", 7) == 0 && ends_with(name, ".admin.req") && strcmp(writer, "hangar") == 0) {
		char user[49];
		bfmt(user, sizeof user, "%.48s", env->caller);
		if (!mirrored) {
			bfmt(out, HG_CALL_CALLER, "%s", user);
		} else {
			const char *r = name + 7, *dot = strchr(r, '.');
			bfmt(out, HG_CALL_CALLER, "%.*s:%s", dot ? (int)(dot - r) : 0, r, user);
		}
		return;
	}
	if (!mirrored)
		bfmt(out, HG_CALL_CALLER, "%s", writer);
	else
		bfmt(out, HG_CALL_CALLER, "%s:%s",
			 my_role && strcmp(my_role, "topside") == 0 ? "vehicle" : "topside", writer);
}

int hg_call_allowed(const char *token, const char (*allow)[HG_WIRING_NAME], int n)
{
	const char *colon = strchr(token, ':');
	for (int i = 0; i < n; i++) {
		const char *a = allow[i];
		if (strcmp(a, "*") == 0) {
			if (!colon)
				return 1; /* every module on this machine */
			continue;
		}
		if (strcmp(a, "admin") == 0) {
			if (strncmp(token, "admin:", 6) == 0)
				return 1; /* every operator on this machine */
			continue;
		}
		const char *acolon = strchr(a, ':');
		if (acolon && (strcmp(acolon + 1, "*") == 0 || strcmp(acolon + 1, "admin") == 0)) {
			/* <role>:* or <role>:admin: the same role, then a module or an operator. */
			if (!colon || (size_t)(acolon - a) != (size_t)(colon - token) ||
			    strncmp(a, token, (size_t)(colon - token)) != 0)
				continue;
			const char *rest_t = colon + 1;
			int is_admin = strncmp(rest_t, "admin:", 6) == 0;
			if (acolon[1] == '*' ? !is_admin && !strchr(rest_t, ':') : is_admin)
				return 1;
			continue;
		}
		if (strcmp(a, token) == 0)
			return 1; /* <module>, <role>:<module>: its own form only */
	}
	return 0;
}

/* One request addressed to this module: docs/04 "Serving", the four
 * steps, then the reply with a reference to the request. */
static void serve_one(hg_sub *s, const struct hg_info *info)
{
	const struct hg_wiring *w = hg_wiring();
	struct hg_call_request_envelope env;
	memcpy(&env, S.req, sizeof env);
	uint64_t received = hg_now_ns();
	char method[33];
	bfmt(method, sizeof method, "%.32s", env.method);
	struct hg_call_reply_envelope rep;
	memset(&rep, 0, sizeof rep);
	rep.call_id = env.call_id;
	rep.caller_id = hg_sub_topic_id(s);
	rep.received_ns = received;
	const struct hg_wiring_serve *line = find_serve(w, method);
	struct method *m = find_method(method);
	uint32_t rep_size = line ? line->m.rep_size : 0;
	if (line)
		rep.reply_type = line->m.rep_hash;
	memset(S.rep + HG_CALL_REP_ENVELOPE, 0, S.schema.type.payload_bytes - HG_CALL_REP_ENVELOPE);
	char token[HG_CALL_CALLER];
	hg_call_caller_token(s, &env, S.role, token);
	int ran = 0;
	if (received > env.deadline_ns) {
		rep.status = HG_E_EXPIRED;
		bfmt(rep.reason, sizeof rep.reason, "expired %llu ms before it was read",
			 (unsigned long long)((received - env.deadline_ns) / 1000000u));
	} else if (!line) {
		rep.status = HG_E_NOMETHOD;
		bfmt(rep.reason, sizeof rep.reason, "no method %s", method);
	} else if (!hg_call_allowed(token, line->allow, line->n_allow)) {
		rep.status = HG_E_DENIED;
		bfmt(rep.reason, sizeof rep.reason, "%s is not in Allow of %s", token, method);
	} else if (!m) {
		rep.status = HG_E_NOMETHOD;
		bfmt(rep.reason, sizeof rep.reason, "no handler registered for %s", method);
	} else if (env.request_type != line->m.req_hash) {
		rep.status = HG_E_BADTYPE;
		bfmt(rep.reason, sizeof rep.reason, "request type %016llx; %s takes %s",
			 (unsigned long long)env.request_type, method, line->m.req_type);
	} else {
		hg_call_ctx c = { .caller = token,
				  .method = method,
				  .call_id = env.call_id,
				  .deadline_ns = env.deadline_ns,
				  .received_ns = received,
				  .req = { .topic_id = hg_sub_topic_id(s), .flags = 0, .seq = info->seq },
				  .ctx = m->ctx };
		char reason[HG_CALL_REASON];
		reason[0] = 0;
		int rc = m->fn(&c, S.req + HG_CALL_REQ_ENVELOPE, S.rep + HG_CALL_REP_ENVELOPE, reason);
		ran = 1;
		if (rc == HG_OK) {
			rep.status = HG_OK;
		} else {
			rep.status = rc < 0 && rc >= HG_E_TIMEOUT ? rc : HG_E_REFUSED;
			if (!reason[0])
				bfmt(reason, sizeof reason, "refused (%d)", rc);
			bfmt(rep.reason, sizeof rep.reason, "%.63s", reason);
			memset(S.rep + HG_CALL_REP_ENVELOPE, 0, rep_size);
		}
	}
	memcpy(S.rep, &rep, sizeof rep);
	uint64_t now = hg_now_ns();
	struct hg_input ref = { .topic_id = hg_sub_topic_id(s), .flags = 0, .seq = info->seq };
	if (hg_publish_with(S.pub, S.rep, S.schema.type.payload_bytes, now, 0, &ref, 1) < 0) {
		/* The request topic is not among the output's Sources (a wiring
		 * the manager did not write): the reply goes out unreferenced. */
		if (!S.no_ref_logged) {
			log_line("%s is not a Source of %s.rep: replies to it carry no reference",
				 hg_sub_name(s), w->module);
			S.no_ref_logged = 1;
		}
		hg_publish_with(S.pub, S.rep, S.schema.type.payload_bytes, now, HG_NO_REFS, NULL, 0);
	}
	if (ran && line && now - received > line->deadline_ns)
		log_line("%s: handler overran its deadline: %llu ms, deadline %llu ms (call %llu from %s)", method,
			 (unsigned long long)((now - received) / 1000000u),
			 (unsigned long long)(line->deadline_ns / 1000000u), (unsigned long long)env.call_id,
			 token);
	if (ran && m->after)
		m->after(m->ctx);
}

/* Reads one request addressed to this module from s, serving it. 1 if it
 * did, 0 if the topic has nothing more for us. Messages for other servers
 * (the admin topic carries everyone's) are passed over without counting. */
static int serve_next(hg_sub *s)
{
	struct hg_info info;
	for (;;) {
		enum hg_result r = hg_next(s, S.req, sizeof S.req, &info);
		if (r != HG_MSG)
			return 0;
		if (hg_sub_payload_bytes(s) < HG_CALL_REQ_ENVELOPE)
			return 0;
		struct hg_call_request_envelope env;
		memcpy(&env, S.req, sizeof env);
		if (env.target_id != S.topic_id)
			continue;
		if (S.n_answered && already_answered(hg_sub_topic_id(s), env.call_id, info.publish_ns))
			continue;
		serve_one(s, &info);
		return 1;
	}
}

static int is_named_sub(const char *name)
{
	for (int i = 0; i < S.n_subs; i++)
		if (strcmp(hg_sub_name(S.subs[i]), name) == 0)
			return 1;
	return 0;
}

int hg_serve(void)
{
	if (server_init() < 0)
		return -1;
	/* The topics of this pass: the named ones, then the glob's matches that
	 * are not among them (a module on a timer polls the glob here). */
	hg_sub *tops[HG_WIRING_MAX_SUBS + 64];
	int ntop = 0;
	for (int i = 0; i < S.n_subs; i++)
		tops[ntop++] = S.subs[i];
	if (S.glob) {
		hg_glob_poll(S.glob);
		int k = hg_glob_count(S.glob);
		for (int i = 0; i < k && ntop < (int)(sizeof tops / sizeof tops[0]); i++) {
			hg_sub *g = hg_glob_at(S.glob, i);
			if (!is_named_sub(hg_sub_name(g)))
				tops[ntop++] = g;
		}
	}
	if (!ntop)
		return 0;
	int budget = HG_SERVE_PER_PASS, served = 0;
	S.rr %= ntop;
	while (budget > 0) {
		int any = 0;
		for (int k = 0; k < ntop && budget > 0; k++) {
			if (serve_next(tops[(S.rr + k) % ntop])) {
				budget--;
				served++;
				any = 1;
			}
		}
		if (!any)
			break;
	}
	S.rr = (S.rr + 1) % ntop;
	return served;
}

int hg_call_subs(hg_sub **out, int max)
{
	const struct hg_wiring *w = hg_wiring();
	if (hg_init() < 0 || !w) {
		errno = EACCES;
		return -1;
	}
	char topic[HG_WIRING_NAME + 8];
	bfmt(topic, sizeof topic, "%s.req", w->module);
	if (hg_wiring_find_pub(w, topic) && caller_init() < 0)
		return -1;
	bfmt(topic, sizeof topic, "%s.rep", w->module);
	if (hg_wiring_find_pub(w, topic) && server_init() < 0)
		return -1;
	int n = S.n_subs + (S.glob ? 1 : 0) + C.n_reps;
	if (n > max) {
		errno = E2BIG;
		return -1;
	}
	n = 0;
	for (int i = 0; i < S.n_subs; i++)
		out[n++] = S.subs[i];
	if (S.glob)
		out[n++] = S.glob;
	for (int i = 0; i < C.n_reps; i++)
		out[n++] = C.reps[i].sub;
	return n;
}
