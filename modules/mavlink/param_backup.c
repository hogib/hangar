// SPDX-License-Identifier: GPL-3.0-or-later
// The vehicle module's parameter dump and load (param_backup.h).
#define _POSIX_C_SOURCE 200809L
#include "param_backup.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mavlink_gen.h"

#define PJ_MAX 2048                    /* more parameters than ArduSub has (about 1 000) */
#define LIST_WAIT_NS 3000000000ull     /* no first PARAM_VALUE after a list request: ask again */
#define QUIET_NS 1000000000ull         /* the list's stream over: ask for what is missing */
#define STEP_NS 50000000ull            /* one request or one PARAM_SET each 50 ms */
#define CONFIRM_NS 1000000000ull       /* a PARAM_SET unanswered this long is sent again */
#define TRIES 3
#define DEADLINE_NS 180000000000ull    /* a whole job */
#define STATUS_NS 500000000ull

enum { IDLE, LISTING, REREADING, SETTING };

/* Time since t, 0 if t is later: a call's handler stamps with the clock of the
 * moment, the loop's tick with the one of its start. */
static uint64_t since(uint64_t now, uint64_t t)
{
	return now > t ? now - t : 0;
}
enum { WANT_SAME, WANT_TO_SET, WANT_SENT, WANT_OK, WANT_FAILED };

static struct pj_send S;
static char dir[512];

static struct {
	char id[17];
	float value;
	uint8_t type;
	unsigned char have;
} cur[PJ_MAX];

static struct {
	char id[17];
	float value;
	uint8_t type;
	uint8_t state;
	int tries;
	uint64_t sent_ns;
} want[PJ_MAX];
static int n_want;

static struct {
	int phase;
	uint32_t job;
	uint8_t kind, state;
	int count, received, differing, set_ok, set_failed, unknown;
	int list_tries, round, next;
	uint64_t started_ns, step_ns, last_rx_ns, list_ns;
	char name[64], file[160], reason[96];
	int changed;
	uint64_t published_ns;
} J;

void pj_init(const char *d, const struct pj_send *send)
{
	snprintf(dir, sizeof dir, "%s", d);
	S = *send;
	/* Shared with hangar-admin through the group (docs/12, hangar
	 * ardusub-params): the command reads the dumps and puts the files to load
	 * here; setgid keeps the group on what is created. */
	mkdir(dir, 0770);
	chmod(dir, 02770);
	/* Job 0: none yet; the first message names the directory. */
	snprintf(J.file, sizeof J.file, "%.159s", dir);
	J.changed = 1;
}

const char *pj_dir(void)
{
	return dir;
}

int pj_running(void)
{
	return J.phase != IDLE;
}

int pj_self_kept(const char *id)
{
	if (strncmp(id, "STAT_", 5) == 0)
		return 1;
	/* BARO_GND_PRESS and BARO1_GND_PRESS ... BARO9_GND_PRESS: calibrated at every boot. */
	if (strncmp(id, "BARO", 4) == 0) {
		const char *p = id + 4;
		if (*p >= '0' && *p <= '9')
			p++;
		return strcmp(p, "_GND_PRESS") == 0;
	}
	return 0;
}

static int int_type(uint8_t t)
{
	return t >= MAV_PARAM_TYPE_UINT8 && t <= MAV_PARAM_TYPE_INT64;
}

int pj_same(float a, float b, uint8_t type)
{
	if (int_type(type))
		return lroundf(a) == lroundf(b);
	float m = fmaxf(1.0f, fmaxf(fabsf(a), fabsf(b)));
	return fabsf(a - b) <= 1e-6f * m;
}

static void fmt_value(char *out, size_t len, float v, uint8_t type)
{
	if (int_type(type))
		snprintf(out, len, "%ld", lroundf(v));
	else
		snprintf(out, len, "%.9g", (double)v);
}

static int valid_name(const char *n)
{
	size_t l = strlen(n);
	if (l == 0 || l > 63 || n[0] == '.')
		return 0;
	for (size_t i = 0; i < l; i++) {
		char c = n[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
		      c == '-' || c == '_'))
			return 0;
	}
	return 1;
}

/* The job over: state 2 done or 3 failed, with a reason. */
static void finish(int state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void finish(int state, const char *fmt, ...)
{
	J.phase = IDLE;
	J.state = (uint8_t)state;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(J.reason, sizeof J.reason, fmt, ap);
	va_end(ap);
	J.changed = 1;
}

/* Done; the reason says what was skipped, if anything. */
static void finish_ok(void)
{
	if (J.unknown)
		finish(2, "%d names of the file not on the autopilot", J.unknown);
	else
		finish(2, "%s", "");
}

/* NAME VALUE TYPE, by index; a temporary file renamed into place. */
static int write_file(const char *path)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	fchmod(fileno(f), 0660);
	time_t t = time(NULL);
	struct tm tm;
	char when[32] = "";
	if (gmtime_r(&t, &tm))
		strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
	fprintf(f, "# hangar: ArduSub's parameters, %d, read %s (mavlink.params_dump)\n", J.count, when);
	fprintf(f, "# NAME VALUE TYPE (MAV_PARAM_TYPE); hangar ardusub-params restore sets them back\n");
	for (int i = 0; i < J.count; i++) {
		if (!cur[i].have)
			continue;
		char v[48];
		fmt_value(v, sizeof v, cur[i].value, cur[i].type);
		fprintf(f, "%s %s %u\n", cur[i].id, v, (unsigned)cur[i].type);
	}
	int bad = ferror(f);
	if (fclose(f) != 0 || bad || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int find_cur(const char *id)
{
	for (int i = 0; i < J.count; i++)
		if (cur[i].have && strcmp(cur[i].id, id) == 0)
			return i;
	return -1;
}

static void begin_read(uint64_t now)
{
	memset(cur, 0, sizeof cur);
	J.count = J.received = 0;
	J.phase = LISTING;
	J.list_tries = 1;
	J.list_ns = J.last_rx_ns = now;
	S.list();
}

/* Everything read: a dump writes its file; a load writes the values in force
 * beside as <name>.before, then compares. */
static void read_done(uint64_t now)
{
	char path[600];
	if (J.kind == 1) {
		snprintf(path, sizeof path, "%s/%s.params", dir, J.name);
		if (write_file(path) < 0) {
			finish(3, "cannot write %s: %s", path, strerror(errno));
			return;
		}
		finish_ok();
		return;
	}
	snprintf(path, sizeof path, "%s/%s.before", dir, J.name);
	if (write_file(path) < 0) {
		finish(3, "cannot write %s: %s", path, strerror(errno));
		return;
	}
	J.differing = J.unknown = 0;
	for (int i = 0; i < n_want; i++) {
		int k = find_cur(want[i].id);
		if (k < 0) {
			J.unknown++;
			want[i].state = WANT_FAILED;
			continue;
		}
		if (!want[i].type)
			want[i].type = cur[k].type;
		if (pj_same(cur[k].value, want[i].value, want[i].type)) {
			want[i].state = WANT_SAME;
		} else {
			want[i].state = WANT_TO_SET;
			J.differing++;
		}
	}
	if (!J.differing) {
		finish_ok();
		return;
	}
	J.phase = SETTING;
	J.step_ns = now;
	J.changed = 1;
}

static int read_file(const char *path, char *reason, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(reason, len, "cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	n_want = 0;
	char line[256];
	int lineno = 0;
	while (fgets(line, sizeof line, f)) {
		lineno++;
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == 0)
			continue;
		char id[64], val[64];
		unsigned type = 0;
		int n = sscanf(p, "%63s %63s %u", id, val, &type);
		char *end;
		double v = n >= 2 ? strtod(val, &end) : 0;
		if (n < 2 || *end || strlen(id) > 16 || type > MAV_PARAM_TYPE_REAL64) {
			snprintf(reason, len, "%s line %d: not NAME VALUE [TYPE]", path, lineno);
			fclose(f);
			return -1;
		}
		if (pj_self_kept(id))
			continue;
		if (n_want == PJ_MAX) {
			snprintf(reason, len, "%s: more than %d parameters", path, PJ_MAX);
			fclose(f);
			return -1;
		}
		memset(&want[n_want], 0, sizeof want[n_want]);
		memcpy(want[n_want].id, id, strlen(id) + 1); /* 16 at most, checked above */
		want[n_want].value = (float)v;
		want[n_want].type = (uint8_t)type;
		n_want++;
	}
	fclose(f);
	return 0;
}

static int start(int kind, const char *name, uint64_t now, char *reason, size_t len)
{
	if (J.phase != IDLE) {
		snprintf(reason, len, "a %s is under way (job %u)", J.kind == 1 ? "dump" : "load", J.job);
		return -1;
	}
	if (!valid_name(name)) {
		snprintf(reason, len, "name: letters, digits, '.', '-' and '_', at most 63, not starting with '.'");
		return -1;
	}
	char path[600];
	int pl = snprintf(path, sizeof path, "%s/%s.params", dir, name);
	if (pl < 0 || (size_t)pl >= sizeof J.file) {
		snprintf(reason, len, "the backup directory's path is too long");
		return -1;
	}
	if (kind == 2 && read_file(path, reason, len) < 0)
		return -1;
	J.job++;
	J.kind = (uint8_t)kind;
	J.state = 1;
	J.differing = J.set_ok = J.set_failed = J.unknown = 0;
	J.started_ns = now;
	J.reason[0] = 0;
	snprintf(J.name, sizeof J.name, "%s", name);
	memcpy(J.file, path, (size_t)pl + 1);
	J.changed = 1;
	begin_read(now);
	return 0;
}

int pj_start_dump(const char *name, uint64_t now, char *reason, size_t len)
{
	return start(1, name, now, reason, len);
}

int pj_start_load(const char *name, uint64_t now, char *reason, size_t len)
{
	return start(2, name, now, reason, len);
}

void pj_abort(const char *why, uint64_t now)
{
	(void)now;
	if (J.phase == IDLE)
		return;
	if (J.phase == SETTING)
		for (int i = 0; i < n_want; i++)
			if (want[i].state == WANT_TO_SET || want[i].state == WANT_SENT)
				want[i].state = WANT_FAILED;
	finish(3, "%s", why);
}

void pj_param_value(const char *id, float value, uint8_t type, uint16_t index, uint16_t count, uint64_t now)
{
	if (J.phase == IDLE)
		return;
	J.last_rx_ns = now;
	if (!J.count && count) {
		if (count > PJ_MAX) {
			finish(3, "the autopilot has %u parameters, more than %d", count, PJ_MAX);
			return;
		}
		J.count = count;
		J.changed = 1;
	}
	int slot = -1;
	if (J.count && index < J.count)
		slot = index;
	else
		slot = find_cur(id);
	if (slot >= 0) {
		if (!cur[slot].have) {
			cur[slot].have = 1;
			J.received++;
		}
		snprintf(cur[slot].id, sizeof cur[slot].id, "%s", id);
		cur[slot].value = value;
		cur[slot].type = type;
	}
	if (J.phase == SETTING)
		for (int i = 0; i < n_want; i++)
			if (want[i].state == WANT_SENT && strcmp(want[i].id, id) == 0 && pj_same(value, want[i].value, want[i].type)) {
				want[i].state = WANT_OK;
				J.set_ok++;
				J.changed = 1;
			}
}

static void tick_read(uint64_t now)
{
	if (J.count && J.received >= J.count) {
		read_done(now);
		return;
	}
	if (J.phase == LISTING) {
		if (!J.count) {
			if (since(now, J.list_ns) >= LIST_WAIT_NS) {
				if (J.list_tries >= TRIES) {
					finish(3, "no PARAM_VALUE after %d PARAM_REQUEST_LIST", TRIES);
					return;
				}
				J.list_tries++;
				J.list_ns = now;
				S.list();
			}
			return;
		}
		if (since(now, J.last_rx_ns) >= QUIET_NS) {
			J.phase = REREADING;
			J.round = 1;
			J.next = 0;
			J.step_ns = now;
		}
		return;
	}
	/* REREADING: one missing index each step; a quiet second between rounds. */
	if (now < J.step_ns)
		return;
	while (J.next < J.count && cur[J.next].have)
		J.next++;
	if (J.next < J.count) {
		S.read_index(J.next++);
		J.step_ns = now + STEP_NS;
		return;
	}
	if (J.round >= TRIES) {
		finish(3, "%d of %d parameters not received", J.count - J.received, J.count);
		return;
	}
	J.round++;
	J.next = 0;
	J.step_ns = now + QUIET_NS;
}

static void tick_set(uint64_t now)
{
	if (now < J.step_ns)
		return;
	/* The next to set, one a step. */
	for (int i = 0; i < n_want; i++) {
		if (want[i].state != WANT_TO_SET)
			continue;
		S.set(want[i].id, want[i].value, want[i].type);
		want[i].state = WANT_SENT;
		want[i].tries++;
		want[i].sent_ns = now;
		J.step_ns = now + STEP_NS;
		return;
	}
	/* All sent: unconfirmed ones again, until their tries are used. */
	int waiting = 0;
	for (int i = 0; i < n_want; i++) {
		if (want[i].state != WANT_SENT)
			continue;
		if (since(now, want[i].sent_ns) < CONFIRM_NS) {
			waiting = 1;
		} else if (want[i].tries < TRIES) {
			want[i].state = WANT_TO_SET;
			waiting = 1;
		} else {
			want[i].state = WANT_FAILED;
			J.set_failed++;
			J.changed = 1;
		}
	}
	if (waiting)
		return;
	if (J.set_failed)
		finish(3, "%d of %d parameters not confirmed", J.set_failed, J.differing);
	else
		finish_ok();
}

void pj_tick(uint64_t now)
{
	if (J.phase == IDLE)
		return;
	if (since(now, J.started_ns) >= DEADLINE_NS) {
		pj_abort("not done within 180 s", now);
		return;
	}
	if (J.phase == SETTING)
		tick_set(now);
	else
		tick_read(now);
}

void pj_current(struct params_job *out)
{
	memset(out, 0, sizeof *out);
	out->job = J.job;
	out->kind = J.kind;
	out->state = J.state;
	out->total = (uint32_t)J.count;
	out->received = (uint32_t)J.received;
	out->differing = (uint32_t)J.differing;
	out->set_ok = (uint32_t)J.set_ok;
	out->set_failed = (uint32_t)J.set_failed;
	out->unknown = (uint32_t)J.unknown;
	snprintf(out->file, sizeof out->file, "%s", J.file);
	snprintf(out->reason, sizeof out->reason, "%s", J.reason);
}

int pj_status(struct params_job *out, uint64_t now)
{
	int due = J.changed || (J.phase != IDLE && since(now, J.published_ns) >= STATUS_NS);
	if (!due)
		return 0;
	pj_current(out);
	J.changed = 0;
	J.published_ns = now;
	return 1;
}
