// SPDX-License-Identifier: GPL-3.0-or-later
/* recorder: the recording module of docs/06-recorder.md (modules/recorder/README.md).
 *
 *   recorder [<recorder.conf>]
 *
 * Subscribes to every topic by glob, drains the rings every 50 ms and hands
 * the records to a writer thread through a bounded buffer; keeps the recent
 * past of every topic in RAM (the black box) and writes it on failsafe, on
 * a module's unasked exit and on `dump`; records sessions, started and
 * stopped by its calls (start, stop) or its AutoStart rule, as one .hgr
 * file per topic in a session directory, with session.ini, the manifests,
 * the message types and hangar.clock.hgr beside them. Read-only on every
 * topic but its own (recorder.status, recorder.session, recorder.rep).
 *
 * GPL-3.0-or-later; C11; libc, pthreads and lib/hg only. */
#include "recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "empty.h"
#include "hg_call.h"
#include "hg_dataset.h"
#include "recorder_clock.h"
#include "recorder_session.h"
#include "recorder_start.h"
#include "recorder_status.h"

#define DRAIN_NS 50000000u
#define DRAIN_NEED_NS 60000000u       /* drain interval + 10 ms of wake-up jitter */
#define DUMP_DEBOUNCE_NS 5000000000ull
#define CLOCK_TOPIC (REC_MAX_TOPICS - 1) /* the writer's slot for hangar.clock.hgr */
#define MAX_TOPICS (REC_MAX_TOPICS - 1)
#define MAX_MODULES 64

struct rtopic {
	hg_sub *sub;
	char name[HG_NAME_MAX + 1];
	struct hg_topic_meta meta;
	int attached;
	int is_frame;
	struct rec_policy pol;
	uint32_t k;                 /* epoch within the session: <topic>+<k>.hgr */
	int have_epoch;
	uint32_t epoch;
	int file_open;
	uint64_t pending_lost;      /* records dropped on a full buffer, carried to the next record */
	uint64_t gaps;              /* gap records written this session */
	uint64_t lost, skipped, records, busy;
	int keep_last;              /* an event topic: its newest message opens a session */
	int have_last;
	struct hgr_record last_rec;
	unsigned char *last_payload;
	int role;                   /* 0 none, 1 safety.state, 2 hangar.<role>.system, 3 hangar.<role>.modules */
	uint32_t off_state, off_name;
};

static struct rec_conf conf;
static struct writer W;
static struct rtopic topics[MAX_TOPICS];
static struct hgr_header hdrs[REC_MAX_TOPICS];
static struct bb_ring rings[REC_MAX_TOPICS];
static int n_topics;
static unsigned char *scratch;
static size_t scratch_cap;
static hg_sub *glob;
static hg_pub *status_pub, *session_pub;
static volatile sig_atomic_t stopping;
static char host[64];
static unsigned char boot[16];

/* The session. */
static int active;
static char session_id[64], session_dir[REC_PATH];
static uint64_t session_start_ns, session_start_real;
static char session_by[96], session_note[128];
static uint64_t last_dump_ns;
static int dumps;
static char status_reason[128];
static struct hg_input start_ref;
static int have_start_ref;

/* The modules, for the unasked-exit trigger. */
static struct {
	char name[32];
	uint8_t state;
	int seen;
} modules[MAX_MODULES];

/* snprintf through a va_list: every copy here is bounded and may truncate
 * by design, and this keeps the compiler from flagging each one. */
static int bfmt(char *dst, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int bfmt(char *dst, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, len, fmt, ap);
	va_end(ap);
	return n;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("recorder: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void on_term(int sig)
{
	(void)sig;
	stopping = 1;
	hg_wait_interrupt();
}

static uint64_t real_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void stamp_text(char *out, size_t len, uint64_t real)
{
	time_t t = (time_t)(real / 1000000000u);
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, len, "%Y%m%d-%H%M%S", &tm);
}

static void hex16(char *out, const unsigned char *b)
{
	for (int i = 0; i < 16; i++)
		bfmt(out + 2 * i, 3, "%02x", b[i]);
}

/* ---- Topics ---------------------------------------------------------------- */

static int grow_scratch(size_t need)
{
	if (need <= scratch_cap)
		return 0;
	unsigned char *n = realloc(scratch, need);
	if (!n)
		return -1;
	scratch = n;
	scratch_cap = need;
	return 0;
}

static void regrant(void)
{
	if (bb_grant(rings, REC_MAX_TOPICS, conf.blackbox_mem) < 0)
		logf_("black box: out of memory while granting budgets");
}

/* A topic has attached: its metadata, policy, black-box budget and header
 * are settled here, once; nothing below allocates per message. */
static void topic_attach(int i)
{
	struct rtopic *t = &topics[i];
	if (hg_sub_meta(t->sub, &t->meta) < 0)
		return;
	uint32_t slen = 0;
	const char *schema = hg_sub_schema(t->sub, &slen);
	static struct hg_schema parsed;
	int parsed_ok = schema && hg_schema_parse(schema, slen, &parsed, NULL, 0) == 0;
	t->is_frame = parsed_ok && hg_type_field(&parsed.type, "fourcc") && hg_type_field(&parsed.type, "data") &&
		      hg_type_field(&parsed.type, "stride");
	rec_conf_resolve(&conf, t->name, t->is_frame, t->meta.rate_mhz, &t->pol);
	t->keep_last = t->meta.rate_mhz == 0 && t->pol.policy != HGR_OFF;
	t->role = 0;
	if (strcmp(t->name, "safety.state") == 0)
		t->role = 1;
	else if (hg_glob_match("hangar.*.system", t->name))
		t->role = 2;
	else if (hg_glob_match("hangar.*.modules", t->name))
		t->role = 3;
	if (t->role && parsed_ok) {
		const struct hg_field *f = hg_type_field(&parsed.type, "state");
		const struct hg_field *nm = hg_type_field(&parsed.type, "name");
		if (!f || f->kind != HG_U8 || (t->role == 3 && (!nm || nm->kind != HG_CHAR))) {
			t->role = 0;
		} else {
			t->off_state = f->offset;
			t->off_name = nm ? nm->offset : 0;
		}
	}
	if (grow_scratch(t->meta.payload_bytes + 8) < 0)
		logf_("%s: cannot grow the read buffer to %u bytes", t->name, t->meta.payload_bytes);
	if (t->keep_last) {
		free(t->last_payload);
		t->last_payload = malloc(t->meta.payload_bytes ? t->meta.payload_bytes : 8);
		t->have_last = 0;
	}
	/* The header, as every file of this topic starts, the epoch set per file. */
	hgr_header_init(&hdrs[i], &t->meta, schema, slen, t->pol.policy == HGR_OFF ? HGR_FULL : t->pol.policy,
			t->pol.every_n, t->pol.text, session_id, t->k);
	/* The black box budget: BlackBox seconds at the rate, or Slots for an
	 * event topic; nothing for class off or a topic recorded off. */
	struct bb_ring *r = &rings[i];
	free(r->buf);
	memset(r, 0, sizeof *r);
	r->rec_bytes = (uint32_t)hdrs[i].record_bytes;
	r->class = t->pol.class;
	if (t->pol.class != REC_BB_OFF && t->pol.policy != HGR_OFF && conf.blackbox_ns > 0) {
		if (t->meta.rate_mhz)
			r->budget = ((uint64_t)conf.blackbox_ns * t->meta.rate_mhz + 999999999999ull) / 1000000000000ull;
		else
			r->budget = t->meta.slot_count;
		if (r->budget < 1)
			r->budget = 1;
		r->wanted = 1;
	}
	regrant();
	if (t->meta.rate_mhz) {
		uint64_t hold_ns = (uint64_t)(t->meta.slot_count - 1) * 1000000000000ull / t->meta.rate_mhz;
		if (hold_ns < DRAIN_NEED_NS)
			logf_("%s: %u slots at %g Hz hold %llu ms, under the %u ms the recorder needs", t->name,
			      t->meta.slot_count, t->meta.rate_mhz / 1000.0, (unsigned long long)hold_ns / 1000000u,
			      DRAIN_NEED_NS / 1000000u);
	}
	t->attached = 1;
	logf_("%s: %s%s, black box %s", t->name, t->pol.text, t->is_frame ? " (frame)" : "",
	      t->pol.class == REC_BB_OFF ? "off" : t->pol.class == REC_BB_LOW ? "low" :
	      t->pol.class == REC_BB_NORMAL ? "normal" : "high");
}

/* New matches of the glob become topics. */
static void topics_scan(void)
{
	int k = hg_glob_count(glob);
	for (int i = n_topics; i < k && n_topics < MAX_TOPICS; i++) {
		struct rtopic *t = &topics[n_topics];
		memset(t, 0, sizeof *t);
		t->sub = hg_glob_at(glob, i);
		bfmt(t->name, sizeof t->name, "%s", hg_sub_name(t->sub));
		n_topics++;
	}
}

/* ---- Sessions -------------------------------------------------------------- */

static int session_ini(char *out, size_t cap, int stopped, const char *stop_by)
{
	char bootid[33], started[32], stopped_at[32];
	hex16(bootid, boot);
	stamp_text(started, sizeof started, session_start_real);
	uint64_t now_real = real_ns(), now_mono = hg_now_ns();
	stamp_text(stopped_at, sizeof stopped_at, now_real);
	int n = bfmt(out, cap,
			 "# session.ini: written by the recorder (docs/06-recorder.md, Sessions)\n"
			 "[Session]\n"
			 "Id=%s\n"
			 "BootId=%s\n"
			 "Host=%s\n"
			 "Recorder=hangar recorder (modules/recorder)\n"
			 "StartedBy=%s\n"
			 "Note=%s\n"
			 "StartedMonotonicNs=%llu\n"
			 "StartedRealNs=%llu\n"
			 "StartedUtc=%s\n",
			 session_id, bootid, host, session_by, session_note, (unsigned long long)session_start_ns,
			 (unsigned long long)session_start_real, started);
	if (n < 0 || (size_t)n >= cap)
		return -1;
	if (stopped) {
		int m = bfmt(out + n, cap - (size_t)n,
				 "StoppedBy=%s\nStoppedMonotonicNs=%llu\nStoppedRealNs=%llu\nStoppedUtc=%s\n"
				 "Records=%llu\nBytes=%llu\nWriteErrors=%llu\n",
				 stop_by, (unsigned long long)now_mono, (unsigned long long)now_real, stopped_at,
				 (unsigned long long)atomic_load(&W.records), (unsigned long long)atomic_load(&W.bytes_written),
				 (unsigned long long)atomic_load(&W.errors));
		if (m < 0 || (size_t)m >= cap - (size_t)n)
			return -1;
		n += m;
	}
	return n;
}

static void publish_session(void)
{
	struct recorder_session s;
	memset(&s, 0, sizeof s);
	s.active = (uint8_t)active;
	bfmt(s.id, sizeof s.id, "%s", session_id);
	bfmt(s.path, sizeof s.path, "%s", session_dir);
	s.started_ns = session_start_ns;
	if (!session_pub)
		return;
	if (have_start_ref && hg_publish_with(session_pub, &s, sizeof s, hg_now_ns(), 0, &start_ref, 1) == 0)
		return;
	hg_publish(session_pub, &s, sizeof s, hg_now_ns());
}

static void publish_status(void)
{
	if (!status_pub)
		return;
	struct recorder_status st;
	memset(&st, 0, sizeof st);
	st.state = (uint8_t)(atomic_load(&W.busy) ? 2 : active ? 1 : 0);
	if (active)
		bfmt(st.session, sizeof st.session, "%s", session_id);
	st.bytes_written = atomic_load(&W.bytes_written);
	struct statvfs vs;
	if (statvfs(conf.dir, &vs) == 0)
		st.free_bytes = (uint64_t)vs.f_bavail * vs.f_frsize;
	uint64_t gaps = 0, least = UINT64_MAX;
	uint32_t recorded = 0;
	for (int i = 0; i < n_topics; i++) {
		gaps += topics[i].gaps;
		if (topics[i].attached && topics[i].pol.policy != HGR_OFF)
			recorded++;
		if (rings[i].count >= 2) {
			uint64_t r = bb_reach_ns(&rings[i]);
			if (r < least)
				least = r;
		}
	}
	st.gaps = (uint32_t)gaps;
	st.topics = recorded;
	st.blackbox_ns = least == UINT64_MAX ? 0 : least;
	hg_publish(status_pub, &st, sizeof st, hg_now_ns());
	char line[128];
	bfmt(line, sizeof line, "%s%s %llu bytes, %u topics, %llu gaps%s%s", active ? "recording " : "idle",
		 active ? session_id : "", (unsigned long long)st.bytes_written, recorded, (unsigned long long)gaps,
		 status_reason[0] ? ": " : "", status_reason);
	hg_status(line);
}

/* The record of a topic's message, into the file (when a session records
 * it) and the black box. */
static void record_message(int i, const struct hg_info *info, const void *payload);

static void write_open(int i)
{
	hdrs[i].epoch = topics[i].k;
	bfmt(hdrs[i].session, sizeof hdrs[i].session, "%s", session_id);
	hdrs[i].created_real_ns = real_ns();
	hdrs[i].created_mono_ns = hg_now_ns();
	if (writer_push(&W, WQ_OPEN, (uint32_t)i, &hdrs[i], sizeof hdrs[i], NULL, 0) < 0)
		return;
	topics[i].file_open = 1;
}

static void clock_record(void)
{
	struct recorder_clock c = { .monotonic_ns = hg_now_ns(), .realtime_ns = real_ns(), .offset_ns = 0 };
	struct hg_info info = { .result = HG_MSG, .seq = ++hdrs[CLOCK_TOPIC].first_seq, .stamp_ns = c.monotonic_ns,
				.publish_ns = c.monotonic_ns };
	struct hgr_record r;
	hgr_record_init(&r, &info, 0, 0);
	(void)writer_push(&W, WQ_RECORD, CLOCK_TOPIC, &r, sizeof r, &c, sizeof c);
}

static void session_start(const char *by, const char *note)
{
	uint64_t real = real_ns();
	char stamp[32];
	stamp_text(stamp, sizeof stamp, real);
	bfmt(session_id, sizeof session_id, "%s-%s", stamp, host);
	bfmt(session_dir, sizeof session_dir, "%s/%s", conf.dir, session_id);
	for (int k = 2; mkdir(session_dir, 02770) < 0; k++) {
		if (errno != EEXIST || k > 99) {
			logf_("cannot create %s: %s", session_dir, strerror(errno));
			bfmt(status_reason, sizeof status_reason, "cannot create %s: %s", session_dir, strerror(errno));
			return;
		}
		bfmt(session_id, sizeof session_id, "%s-%s-%d", stamp, host, k);
		bfmt(session_dir, sizeof session_dir, "%s/%s", conf.dir, session_id);
	}
	(void)rec_mkdir(session_dir);
	active = 1;
	session_start_ns = hg_now_ns();
	session_start_real = real;
	bfmt(session_by, sizeof session_by, "%s", by);
	bfmt(session_note, sizeof session_note, "%s", note ? note : "");
	status_reason[0] = 0;
	atomic_store(&W.bytes_written, 0);
	atomic_store(&W.records, 0);
	atomic_store(&W.errors, 0);
	writer_push(&W, WQ_SESSION_BEGIN, 0, session_dir, strlen(session_dir) + 1, NULL, 0);
	static char ini[4096];
	int n = session_ini(ini, sizeof ini, 0, "");
	if (n > 0) {
		static char msg[4096 + 16];
		memcpy(msg, "session.ini", 12);
		memcpy(msg + 12, ini, (size_t)n);
		writer_push(&W, WQ_TEXT_FILE, 0, msg, 12 + (size_t)n, NULL, 0);
	}
	/* hangar.clock.hgr, and the newest message of every event topic, so
	 * that the state at the start is known without waiting for a change. */
	struct hg_topic_meta cm;
	memset(&cm, 0, sizeof cm);
	bfmt(cm.name, sizeof cm.name, "hangar.clock");
	bfmt(cm.writer, sizeof cm.writer, "recorder");
	cm.topic_id = hg_topic_id("hangar.clock");
	cm.type_hash = RECORDER_CLOCK_TYPE.hash;
	cm.payload_bytes = RECORDER_CLOCK_TYPE.payload_bytes;
	cm.rate_mhz = 1000;
	hgr_header_init(&hdrs[CLOCK_TOPIC], &cm, RECORDER_CLOCK_TYPE.schema, RECORDER_CLOCK_TYPE.schema_bytes, HGR_FULL,
			1, "full", session_id, 0);
	hdrs[CLOCK_TOPIC].first_seq = 0;
	writer_push(&W, WQ_OPEN, CLOCK_TOPIC, &hdrs[CLOCK_TOPIC], sizeof hdrs[CLOCK_TOPIC], NULL, 0);
	clock_record();
	for (int i = 0; i < n_topics; i++) {
		struct rtopic *t = &topics[i];
		t->k = 0;
		t->file_open = 0;
		t->gaps = 0;
		t->pending_lost = 0;
		if (t->attached && t->keep_last && t->have_last && t->pol.policy != HGR_OFF) {
			write_open(i);
			struct hgr_record r = t->last_rec;
			r.flags &= ~HGR_REC_GAP;
			r.gap = 0;
			if (t->file_open &&
			    writer_push(&W, WQ_RECORD, (uint32_t)i, &r, sizeof r, t->last_payload,
					t->pol.policy == HGR_META ? 0 : t->meta.payload_bytes) == 0)
				t->records++;
		}
	}
	logf_("session %s started (%s)", session_id, by);
	publish_session();
	publish_status();
}

static void session_stop(const char *by)
{
	if (!active)
		return;
	static char ini[4096];
	int n = session_ini(ini, sizeof ini, 1, by);
	if (n > 0) {
		static char msg[4096 + 16];
		memcpy(msg, "session.ini", 12);
		memcpy(msg + 12, ini, (size_t)n);
		writer_push(&W, WQ_TEXT_FILE, 0, msg, 12 + (size_t)n, NULL, 0);
	}
	writer_push(&W, WQ_SESSION_END, 0, NULL, 0, NULL, 0);
	logf_("session %s stopped (%s)", session_id, by);
	active = 0;
	for (int i = 0; i < n_topics; i++)
		topics[i].file_open = 0;
	publish_session();
	publish_status();
}

/* The black box to disk, as a session of its own. */
static void dump(const char *reason)
{
	uint64_t now = hg_now_ns();
	if (atomic_load(&W.busy) || (last_dump_ns && now - last_dump_ns < DUMP_DEBOUNCE_NS)) {
		logf_("black box: dump (%s) skipped, one is under way or just done", reason);
		return;
	}
	char stamp[32];
	stamp_text(stamp, sizeof stamp, real_ns());
	char dir[REC_PATH];
	bfmt(dir, sizeof dir, "%s/blackbox-%s-%d", conf.dir, stamp, ++dumps);
	char bootid[33];
	hex16(bootid, boot);
	static char ini[4096];
	int n = bfmt(ini, sizeof ini,
			 "# session.ini: a black-box dump (docs/06-recorder.md, The black box)\n[Session]\n"
			 "Id=blackbox-%s-%d\nKind=blackbox\nBootId=%s\nHost=%s\nReason=%s\n"
			 "DumpedMonotonicNs=%llu\nDumpedRealNs=%llu\nSession=%s\n[BlackBox]\n",
			 stamp, dumps, bootid, host, reason, (unsigned long long)now, (unsigned long long)real_ns(),
			 active ? session_id : "");
	for (int i = 0; i < n_topics && n > 0 && (size_t)n < sizeof ini; i++)
		if (rings[i].count)
			n += bfmt(ini + n, sizeof ini - (size_t)n, "%s=%llu\n", topics[i].name,
				      (unsigned long long)bb_reach_ns(&rings[i]));
	for (int i = 0; i < n_topics; i++) {
		hdrs[i].epoch = 0;
		bfmt(hdrs[i].session, sizeof hdrs[i].session, "blackbox-%s-%d", stamp, dumps);
	}
	struct bb_dump *d = bb_take(rings, hdrs, n_topics, dir, ini);
	if (!d) {
		logf_("black box: out of memory for the dump");
		return;
	}
	if (writer_push(&W, WQ_DUMP, 0, &d, sizeof d, NULL, 0) < 0) {
		logf_("black box: the writer's buffer is full; dump (%s) lost", reason);
		bb_dump_free(d);
		return;
	}
	last_dump_ns = now;
	logf_("black box: dump to %s (%s)", dir, reason);
	publish_status();
}

/* ---- Draining --------------------------------------------------------------- */

static void note_special(struct rtopic *t, const unsigned char *payload)
{
	uint8_t state = payload[t->off_state];
	if (t->role == 1) {
		static int armed = -1;
		int now_armed = state == 1;
		if (conf.autostart == REC_ARMED && armed != now_armed) {
			if (now_armed && !active)
				session_start("autostart:armed", "");
			else if (!now_armed && active && strcmp(session_by, "autostart:armed") == 0)
				session_stop("autostart:disarmed");
		}
		armed = now_armed;
	} else if (t->role == 2) {
		static int sys_state = -1;
		if (state == 2 && sys_state != 2)
			dump("failsafe");
		sys_state = state;
	} else if (t->role == 3) {
		char name[32];
		bfmt(name, sizeof name, "%.31s", (const char *)payload + t->off_name);
		int slot = -1;
		for (int i = 0; i < MAX_MODULES; i++)
			if (modules[i].seen && strcmp(modules[i].name, name) == 0) {
				slot = i;
				break;
			}
		if (slot < 0)
			for (int i = 0; i < MAX_MODULES; i++)
				if (!modules[i].seen) {
					slot = i;
					modules[i].seen = 1;
					bfmt(modules[i].name, sizeof modules[i].name, "%s", name);
					modules[i].state = state;
					break;
				}
		if (slot < 0)
			return;
		uint8_t prev = modules[slot].state;
		modules[slot].state = state;
		/* starting, ready or unhealthy, then backoff or failed: the exit
		 * was nobody's request (a stop goes through stopping). */
		if (prev <= 2 && (state == 3 || state == 4)) {
			char why[96];
			bfmt(why, sizeof why, "module %s exited", name);
			dump(why);
		}
	}
}

static void record_message(int i, const struct hg_info *info, const void *payload)
{
	struct rtopic *t = &topics[i];
	t->records++;
	if (t->have_epoch && info->epoch != t->epoch) {
		/* A new epoch: a new file, and the black box starts over so one
		 * file never holds two numberings. */
		t->k++;
		t->file_open = 0;
		rings[i].count = 0;
		rings[i].head = 0;
		memcpy(hdrs[i].origin_boot, t->meta.origin_boot, 16);
		struct hg_topic_meta m;
		if (hg_sub_meta(t->sub, &m) == 0) {
			t->meta = m;
			memcpy(hdrs[i].origin_boot, m.origin_boot, 16);
			hdrs[i].first_seq = m.first_seq;
		}
	}
	t->have_epoch = 1;
	t->epoch = info->epoch;
	uint64_t lost = info->gap + t->pending_lost;
	t->pending_lost = 0;
	t->lost += info->gap;
	t->skipped += info->skipped;
	struct hgr_record r;
	hgr_record_init(&r, info, lost, info->skipped);
	if (t->role)
		note_special(t, payload);
	if (rings[i].cap)
		bb_push(&rings[i], &r, payload);
	if (t->keep_last && t->last_payload) {
		t->last_rec = r;
		memcpy(t->last_payload, payload, t->meta.payload_bytes);
		t->have_last = 1;
	}
	if (!active || t->pol.policy == HGR_OFF)
		return;
	if (t->pol.every_n > 1 && info->seq % t->pol.every_n != 0) {
		/* Left out by policy: not a gap, but a loss before it still
		 * belongs to the next record written. */
		t->pending_lost = lost;
		return;
	}
	if (!t->file_open)
		write_open(i);
	if (!t->file_open || writer_push(&W, WQ_RECORD, (uint32_t)i, &r, sizeof r, payload,
					 t->pol.policy == HGR_META ? 0 : t->meta.payload_bytes) < 0) {
		/* The disk is behind: this record is lost, and the next one
		 * carries it and whatever it carried. */
		t->pending_lost = lost + 1;
		return;
	}
	if (lost)
		t->gaps++;
}

static void drain(void)
{
	hg_glob_poll(glob);
	topics_scan();
	for (int i = 0; i < n_topics; i++) {
		struct rtopic *t = &topics[i];
		if (!t->attached) {
			topic_attach(i);
			if (!t->attached)
				continue;
		}
		uint32_t limit = t->meta.slot_count * 2 + 8;
		if (scratch_cap < t->meta.payload_bytes)
			continue;
		for (uint32_t n = 0; n < limit; n++) {
			struct hg_info info;
			enum hg_result r = hg_next(t->sub, scratch, scratch_cap, &info);
			if (r == HG_MSG) {
				record_message(i, &info, scratch);
				continue;
			}
			if (r == HG_BUSY) {
				t->busy++;
				continue;
			}
			if (r == HG_ERROR && errno == EMSGSIZE) {
				/* The file was replaced by one with a larger payload. */
				struct hg_topic_meta m;
				if (hg_sub_meta(t->sub, &m) == 0 && grow_scratch(m.payload_bytes + 8) == 0) {
					t->meta = m;
					continue;
				}
			}
			break;
		}
	}
}

/* ---- Calls ------------------------------------------------------------------- */

static int on_start(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)rep;
	if (active) {
		bfmt(reason, HG_CALL_REASON, "already recording %s", session_id);
		return HG_E_REFUSED;
	}
	const struct recorder_start *rs = req;
	char note[128];
	bfmt(note, sizeof note, "%.127s", rs->note);
	char by[96];
	bfmt(by, sizeof by, "call:%s", c->caller);
	start_ref = c->req;
	have_start_ref = 1;
	session_start(by, note);
	have_start_ref = 0;
	if (!active) {
		bfmt(reason, HG_CALL_REASON, "%.63s", status_reason);
		return HG_E_REFUSED;
	}
	return HG_OK;
}

static int on_stop(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	if (!active) {
		bfmt(reason, HG_CALL_REASON, "not recording");
		return HG_E_REFUSED;
	}
	char by[96];
	bfmt(by, sizeof by, "call:%s", c->caller);
	start_ref = c->req;
	have_start_ref = 1;
	session_stop(by);
	have_start_ref = 0;
	return HG_OK;
}

static int on_dump(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)req;
	(void)rep;
	if (atomic_load(&W.busy)) {
		bfmt(reason, HG_CALL_REASON, "a dump is being written");
		return HG_E_REFUSED;
	}
	last_dump_ns = 0;
	char why[96];
	bfmt(why, sizeof why, "call:%s", c->caller);
	dump(why);
	return HG_OK;
}

/* ---- Main -------------------------------------------------------------------- */

static int check_dir(void)
{
	struct stat st;
	if (stat(conf.dir, &st) < 0) {
		if (errno != ENOENT || rec_mkdir(conf.dir) < 0 || stat(conf.dir, &st) < 0) {
			logf_("Dir %s: %s", conf.dir, strerror(errno));
			return -1;
		}
	}
	if (!S_ISDIR(st.st_mode)) {
		logf_("Dir %s is not a directory", conf.dir);
		return -1;
	}
	if ((st.st_mode & 07777) != 02770) {
		logf_("Dir %s has mode %04o, not 2770", conf.dir, (unsigned)(st.st_mode & 07777));
		return -1;
	}
	struct group *g = getgrnam("hangar-rec");
	if (g && st.st_gid != g->gr_gid) {
		logf_("Dir %s is not group hangar-rec", conf.dir);
		return -1;
	}
	if (!g)
		logf_("no group hangar-rec on this machine: the group of %s is not checked", conf.dir);
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_term };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	const char *conf_path = argc > 1 ? argv[1] : NULL;
	char err[400];
	if (rec_conf_load(conf_path, &conf, err, sizeof err) < 0) {
		logf_("%s", err);
		hg_status(err);
		return 78;
	}
	if (hg_init() < 0) {
		logf_("hg_init: %s (%s)", hg_init_error(), strerror(errno));
		return 3;
	}
	if (check_dir() < 0) {
		bfmt(err, sizeof err, "Dir %s: not a directory of group hangar-rec with mode 2770", conf.dir);
		hg_status(err);
		return 78;
	}
	if (gethostname(host, sizeof host) < 0)
		bfmt(host, sizeof host, "host");
	host[sizeof host - 1] = 0;
	hg_ds_boot_id(boot);
	if (writer_start(&W, (size_t)conf.write_buffer) < 0) {
		logf_("cannot start the writer thread: %s", strerror(errno));
		return 3;
	}
	glob = hg_sub_open("*", NULL);
	if (!glob) {
		logf_("cannot subscribe to *: %s (the manifest needs [Subscribe *])", strerror(errno));
		return 4;
	}
	struct hg_pub_opts eo = { .slots = 16 };
	status_pub = hg_pub_open("recorder.status", &RECORDER_STATUS_TYPE, &eo);
	session_pub = hg_pub_open("recorder.session", &RECORDER_SESSION_TYPE, &eo);
	if (!status_pub || !session_pub)
		logf_("cannot open recorder.status or recorder.session: %s", strerror(errno));
	hg_sub *subs[HG_MAX_WAIT];
	int n = 0;
	subs[n++] = glob;
	if (hg_wiring()) {
		if (hg_serve_method("start", on_start, NULL) < 0 || hg_serve_method("stop", on_stop, NULL) < 0 ||
		    hg_serve_method("dump", on_dump, NULL) < 0)
			logf_("the wiring does not serve start, stop and dump: %s", strerror(errno));
		int k = hg_call_subs(subs + n, HG_MAX_WAIT - n);
		if (k > 0)
			n += k;
	} else {
		logf_("unwired: no calls; AutoStart=%s", conf.autostart == REC_BOOT ? "boot" :
		      conf.autostart == REC_ARMED ? "armed" : "manual");
	}
	logf_("recording to %s; black box %llds in %lluMB; AutoStart=%s", conf.dir,
	      (long long)(conf.blackbox_ns / 1000000000), (unsigned long long)(conf.blackbox_mem >> 20),
	      conf.autostart == REC_BOOT ? "boot" : conf.autostart == REC_ARMED ? "armed" : "manual");
	hg_ready();
	drain();
	if (conf.autostart == REC_BOOT)
		session_start("autostart:boot", "");
	publish_status();
	uint64_t last_drain = hg_now_ns(), last_tick = last_drain;
	while (!stopping) {
		unsigned r = hg_wait(subs, n, 10);
		if (r & HG_READY_INTERRUPT)
			break;
		if (n > 1)
			hg_serve();
		uint64_t now = hg_now_ns();
		if (now - last_drain >= DRAIN_NS) {
			last_drain = now;
			drain();
		}
		if (now - last_tick >= 1000000000u) {
			last_tick = now;
			if (active) {
				clock_record();
				struct statvfs vs;
				if (statvfs(conf.dir, &vs) == 0 && (uint64_t)vs.f_bavail * vs.f_frsize < conf.min_free) {
					bfmt(status_reason, sizeof status_reason, "stopped: under MinFree (%llu MB free)",
						 (unsigned long long)((uint64_t)vs.f_bavail * vs.f_frsize >> 20));
					logf_("%s", status_reason);
					session_stop("min-free");
				}
			}
			publish_status();
			hg_alive();
		}
	}
	if (active)
		session_stop("shutdown");
	drain();
	writer_stop(&W);
	if (status_pub)
		hg_pub_close(status_pub);
	if (session_pub)
		hg_pub_close(session_pub);
	logf_("stopped");
	return 0;
}
