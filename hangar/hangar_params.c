// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar ardusub-params save|diff|restore <file> (docs/12): every parameter of
 * ArduSub into a file, a file compared with the autopilot, a file set back.
 * Through the vehicle mavlink module's calls, as hangar ardusub-setup:
 * mavlink.params_dump and mavlink.params_load, as admin:<user>. The module
 * keeps its files in its backup directory (ardusub.params_job's first message
 * names it; hangar-admin shares it through the group); this command copies
 * a dump out of it to <file>, or <file> into it to load.
 *
 * The file: one `NAME VALUE TYPE` a line (TYPE MAV_PARAM_TYPE: 9 a float, 1
 * to 8 the integers), '#' comments. Restore and diff also read the YAML of
 * `ros2 param dump /mavros/param` (`    NAME: value`, no type: the
 * autopilot's own), so the backups rov.sh made through MAVROS still serve.
 * ArduSub's own bookkeeping (STAT_*, BARO*_GND_PRESS) is left out of the
 * comparison and never set. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hg_call.h"
#include "hg_topic.h"
#include "params_job.h"

int hangar_call(const char *target, char **kv, int nkv, long timeout_ms, char reason[64], void *rep, size_t rep_max);
const char *hangar_topic_dir(void);

#define MAXP 4096
#define JOB_TIMEOUT_MS 200000L /* the module gives a job 180 s */

struct param {
	char id[17];
	float value;
	unsigned type; /* 0: not given */
	int seen;
};

struct table {
	struct param *p;
	int n;
};

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("hangar ardusub-params: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	return 1;
}

static int self_kept(const char *id)
{
	if (strncmp(id, "STAT_", 5) == 0)
		return 1;
	if (strncmp(id, "BARO", 4) == 0) {
		const char *p = id + 4;
		if (*p >= '0' && *p <= '9')
			p++;
		return strcmp(p, "_GND_PRESS") == 0;
	}
	return 0;
}

static int int_type(unsigned t)
{
	return t >= 1 && t <= 8;
}

/* Without libm, which the command does not otherwise link. A value past what
 * a long holds (an integer parameter written as 6.6e25 in a file) has no
 * conversion C defines: the end of the range stands for it. */
static long round_l(float v)
{
	if (v >= (float)LONG_MAX)
		return LONG_MAX;
	if (v <= (float)LONG_MIN)
		return LONG_MIN;
	return (long)(v < 0 ? v - 0.5f : v + 0.5f);
}

static float abs_f(float v)
{
	return v < 0 ? -v : v;
}

/* As the module's comparison (param_backup.c pj_same). */
static int same(float a, float b, unsigned type)
{
	if (int_type(type))
		return round_l(a) == round_l(b);
	float m = abs_f(a) > abs_f(b) ? abs_f(a) : abs_f(b);
	if (m < 1.0f)
		m = 1.0f;
	return abs_f(a - b) <= 1e-6f * m;
}

static void fmt_value(char *out, size_t len, float v, unsigned type)
{
	if (int_type(type))
		snprintf(out, len, "%ld", round_l(v));
	else
		snprintf(out, len, "%.9g", (double)v);
}

static int is_name(const char *s, size_t n)
{
	if (n == 0 || n > 16 || !(s[0] >= 'A' && s[0] <= 'Z'))
		return 0;
	for (size_t i = 0; i < n; i++)
		if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
			return 0;
	return 1;
}

/* NAME VALUE [TYPE] lines, or MAVROS's YAML (NAME: value under a header of
 * lower-case keys, which are skipped). */
static int read_table(const char *path, struct table *t)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return fail("cannot read %s: %s", path, strerror(errno));
	t->p = calloc(MAXP, sizeof *t->p);
	t->n = 0;
	if (!t->p) {
		fclose(f);
		return fail("out of memory");
	}
	char line[512];
	int lineno = 0;
	while (fgets(line, sizeof line, f)) {
		lineno++;
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		s[strcspn(s, "\r\n")] = 0;
		if (*s == '#' || *s == 0)
			continue;
		size_t nl = strcspn(s, " \t:");
		char *rest = s + nl;
		int yaml = *rest == ':';
		if (!is_name(s, nl)) {
			if (yaml)
				continue; /* "/mavros/param:", "ros__parameters:", "use_sim_time: false" */
			fclose(f);
			return fail("%s line %d: not NAME VALUE [TYPE]", path, lineno);
		}
		if (yaml)
			rest++;
		char *end;
		double v = strtod(rest, &end);
		unsigned type = 0;
		if (end == rest) {
			fclose(f);
			return fail("%s line %d: no value", path, lineno);
		}
		/* strtod takes nan, inf and 1e99: none is a parameter's value, and
		 * none would compare or be restored as one. */
		float fv = (float)v;
		if (fv != fv || fv - fv != 0) {
			fclose(f);
			return fail("%s line %d: the value is not a finite number", path, lineno);
		}
		if (!yaml) {
			char *e2;
			unsigned long ty = strtoul(end, &e2, 10);
			if (e2 != end) {
				if (ty < 1 || ty > 10) {
					fclose(f);
					return fail("%s line %d: type %lu is not a MAV_PARAM_TYPE", path, lineno, ty);
				}
				type = (unsigned)ty;
				end = e2;
			}
		}
		while (*end == ' ' || *end == '\t')
			end++;
		if (*end && *end != '#') {
			fclose(f);
			return fail("%s line %d: more than NAME VALUE [TYPE]", path, lineno);
		}
		if (t->n == MAXP) {
			fclose(f);
			return fail("%s: more than %d parameters", path, MAXP);
		}
		struct param *p = &t->p[t->n++];
		memcpy(p->id, s, nl);
		p->id[nl] = 0;
		p->value = (float)v;
		p->type = type;
	}
	fclose(f);
	return 0;
}

static struct param *find(struct table *t, const char *id)
{
	for (int i = 0; i < t->n; i++)
		if (strcmp(t->p[i].id, id) == 0)
			return &t->p[i];
	return NULL;
}

/* ---- The module's side -------------------------------------------------------- */

static char backup_dir[512];

/* The module's backup directory, from ardusub.params_job. */
static hg_sub *open_jobs(void)
{
	hangar_topic_dir();
	hg_sub *s = hg_sub_open("ardusub.params_job", NULL);
	if (!s) {
		fail("no ardusub.params_job: the vehicle's mavlink module is not running, or its manifest predates "
		     "params_dump/params_load");
		return NULL;
	}
	struct params_job m;
	struct hg_info info;
	if (hg_sub_payload_bytes(s) != sizeof m) {
		hg_sub_close(s);
		fail("ardusub.params_job: a payload of %u bytes, %zu expected (another version of the module?)",
		     hg_sub_payload_bytes(s), sizeof m);
		return NULL;
	}
	if (hg_latest(s, &m, sizeof m, &info) != HG_MSG) {
		hg_sub_close(s);
		fail("ardusub.params_job is empty: the module has not started properly");
		return NULL;
	}
	m.file[sizeof m.file - 1] = 0;
	if (m.job == 0) {
		snprintf(backup_dir, sizeof backup_dir, "%s", m.file);
	} else {
		char tmp[sizeof m.file];
		memcpy(tmp, m.file, sizeof tmp);
		snprintf(backup_dir, sizeof backup_dir, "%s", dirname(tmp));
	}
	return s;
}

/* A job started by a call, followed on ardusub.params_job to its end. */
static int run_job(hg_sub *jobs, const char *method, const char *name, struct params_job *out)
{
	char kv[128], reason[64];
	snprintf(kv, sizeof kv, "name=%s", name);
	char *args[] = { kv };
	struct params_job rep;
	memset(&rep, 0, sizeof rep);
	int st = hangar_call(method, args, 1, 3000, reason, &rep, sizeof rep);
	if (st == -100)
		return 1;
	if (st != HG_OK)
		return fail("%s: %s%s%s", method, hg_status_name(st), reason[0] ? ": " : "", reason);
	uint64_t deadline = hg_now_ns() + (uint64_t)JOB_TIMEOUT_MS * 1000000u;
	uint64_t said = 0;
	for (;;) {
		struct params_job m;
		struct hg_info info;
		enum hg_result r;
		while ((r = hg_next(jobs, &m, sizeof m, &info)) == HG_MSG) {
			if (m.job != rep.job)
				continue;
			m.reason[sizeof m.reason - 1] = 0;
			m.file[sizeof m.file - 1] = 0;
			if (m.state != 1) {
				*out = m;
				return 0;
			}
			uint64_t now = hg_now_ns();
			if (isatty(2) && now - said > 1000000000ull) {
				said = now;
				if (m.total && m.received < m.total)
					fprintf(stderr, "  %u of %u read\n", m.received, m.total);
				else if (m.differing)
					fprintf(stderr, "  %u of %u set\n", m.set_ok, m.differing);
			}
		}
		uint64_t now = hg_now_ns();
		if (now >= deadline)
			return fail("job %u: no end on ardusub.params_job within %ld s", rep.job, JOB_TIMEOUT_MS / 1000);
		hg_wait(&jobs, 1, 200);
	}
}

static void stamp(char *out, size_t len, const char *what)
{
	time_t t = time(NULL);
	struct tm tm;
	char when[32] = "x";
	if (gmtime_r(&t, &tm))
		strftime(when, sizeof when, "%Y%m%d-%H%M%S", &tm);
	snprintf(out, len, "%s-%s-%d", what, when, (int)getpid());
}

/* A dump into the module's directory: its path. */
static int dump(hg_sub *jobs, const char *what, char *path, size_t len, struct params_job *m)
{
	char name[64];
	stamp(name, sizeof name, what);
	if (run_job(jobs, "mavlink.params_dump", name, m))
		return 1;
	if (m->state != 2)
		return fail("dump failed: %s", m->reason);
	snprintf(path, len, "%s", m->file);
	return 0;
}

static int copy_out(const char *from, const char *to)
{
	FILE *in = fopen(from, "r");
	if (!in)
		return fail("cannot read %s: %s (is this user in the module's group, hangar?)", from, strerror(errno));
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.tmp%d", to, (int)getpid());
	FILE *out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return fail("cannot write %s: %s", tmp, strerror(errno));
	}
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		fwrite(buf, 1, n, out);
	int bad = ferror(in) || ferror(out);
	fclose(in);
	if (fclose(out) != 0 || bad || rename(tmp, to) != 0) {
		unlink(tmp);
		return fail("cannot write %s: %s", to, strerror(errno));
	}
	return 0;
}

/* ---- The three commands -------------------------------------------------------- */

static int cmd_save(const char *file)
{
	hg_sub *jobs = open_jobs();
	if (!jobs)
		return 1;
	char path[600];
	struct params_job m;
	int rc = dump(jobs, "save", path, sizeof path, &m);
	hg_sub_close(jobs);
	if (rc || copy_out(path, file))
		return 1;
	printf("%u parameters saved to %s (the module keeps %s)\n", m.received, file, path);
	return 0;
}

/* What differs between the file and the autopilot now, printed; the count. */
static int compare(struct table *want, struct table *now, int quiet)
{
	int n = 0;
	for (int i = 0; i < want->n; i++) {
		struct param *w = &want->p[i];
		if (self_kept(w->id))
			continue;
		struct param *c = find(now, w->id);
		char a[48], b[48];
		if (!c) {
			fmt_value(a, sizeof a, w->value, w->type);
			if (!quiet)
				printf("  %-16s file %s, not on the autopilot\n", w->id, a);
			n++;
			continue;
		}
		c->seen = 1;
		unsigned type = w->type ? w->type : c->type;
		if (!same(w->value, c->value, type)) {
			fmt_value(a, sizeof a, w->value, type);
			fmt_value(b, sizeof b, c->value, type);
			if (!quiet)
				printf("  %-16s file %s, autopilot %s\n", w->id, a, b);
			n++;
		}
	}
	/* What the file does not name: the first ten (a partial file names few). */
	int only = 0;
	for (int i = 0; i < now->n; i++) {
		struct param *c = &now->p[i];
		if (c->seen || self_kept(c->id))
			continue;
		char b[48];
		fmt_value(b, sizeof b, c->value, c->type);
		if (!quiet && only < 10)
			printf("  %-16s autopilot %s, not in the file\n", c->id, b);
		only++;
		n++;
	}
	if (!quiet && only > 10)
		printf("  and %d more on the autopilot, not in the file\n", only - 10);
	return n;
}

static int cmd_diff(const char *file)
{
	struct table want, now;
	if (read_table(file, &want))
		return 2;
	hg_sub *jobs = open_jobs();
	if (!jobs)
		return 2;
	char path[600];
	struct params_job m;
	int rc = dump(jobs, "diff", path, sizeof path, &m);
	hg_sub_close(jobs);
	if (rc)
		return 2;
	rc = read_table(path, &now);
	unlink(path);
	if (rc)
		return 2;
	int n = compare(&want, &now, 0);
	if (n == 0) {
		printf("every parameter as in %s (%d compared; STAT_* and BARO*_GND_PRESS left out)\n", file, want.n);
		return 0;
	}
	printf("%d differ%s from %s\n", n, n == 1 ? "s" : "", file);
	return 1;
}

static int cmd_restore(const char *file)
{
	struct table want;
	if (read_table(file, &want))
		return 1;
	hg_sub *jobs = open_jobs();
	if (!jobs)
		return 1;
	/* The file, as the module reads it, into its directory. */
	char name[64], path[700];
	stamp(name, sizeof name, "restore");
	snprintf(path, sizeof path, "%s/%s.params", backup_dir, name);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0660);
	if (fd < 0) {
		hg_sub_close(jobs);
		return fail("cannot write %s: %s (is this user in the module's group, hangar?)", path, strerror(errno));
	}
	FILE *f = fdopen(fd, "w");
	if (!f) {
		int e = errno;
		close(fd);
		unlink(path);
		hg_sub_close(jobs);
		return fail("cannot write %s: %s", path, strerror(e));
	}
	fprintf(f, "# hangar ardusub-params restore: from %s\n", file);
	int n = 0;
	for (int i = 0; i < want.n; i++) {
		if (self_kept(want.p[i].id))
			continue;
		char v[48];
		fmt_value(v, sizeof v, want.p[i].value, want.p[i].type);
		if (want.p[i].type)
			fprintf(f, "%s %s %u\n", want.p[i].id, v, want.p[i].type);
		else
			fprintf(f, "%s %s\n", want.p[i].id, v);
		n++;
	}
	if (fclose(f) != 0) {
		hg_sub_close(jobs);
		unlink(path);
		return fail("cannot write %s", path);
	}
	struct params_job m;
	int rc = run_job(jobs, "mavlink.params_load", name, &m);
	hg_sub_close(jobs);
	if (rc)
		return 1;
	char before[700];
	snprintf(before, sizeof before, "%s/%s.before", backup_dir, name);
	if (m.state != 2) {
		printf("restore failed: %s\n  %u of %u differing set and confirmed, %u not; the values before: %s\n", m.reason,
		       m.set_ok, m.differing, m.set_failed, before);
		return 1;
	}
	if (m.differing == 0)
		printf("nothing to restore: the autopilot's %u parameters already as %s (%d compared)\n", m.total, file, n);
	else
		printf("%u parameter%s set and confirmed (of %d compared); the values before: %s\n", m.set_ok,
		       m.set_ok == 1 ? "" : "s", n, before);
	if (m.unknown)
		printf("  %u names of %s not on the autopilot (hangar ardusub-params diff lists them)\n", m.unknown, file);
	return 0;
}

int hangar_params_main(int argc, char **argv)
{
	if (argc != 2) {
		fputs("usage: hangar ardusub-params save|diff|restore <file>\n"
		      "  save     every ArduSub parameter into <file> (NAME VALUE TYPE lines)\n"
		      "  diff     <file> against the autopilot now; exit 1 when something differs\n"
		      "  restore  sets what differs from <file>, each read back; refused while armed\n",
		      stderr);
		return 2;
	}
	if (strcmp(argv[0], "save") == 0)
		return cmd_save(argv[1]);
	if (strcmp(argv[0], "diff") == 0)
		return cmd_diff(argv[1]);
	if (strcmp(argv[0], "restore") == 0)
		return cmd_restore(argv[1]);
	return fail("unknown command %s: save, diff or restore", argv[0]);
}
