// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar dataset (docs/12-tools.md "Datasets and analysis"; modules/recorder/README.md):
 *
 *   hangar dataset build <spec> <session> [-o <out.hgr>]     (or: hangar dataset <spec> <session>)
 *       the rows of the spec from a session's .hgr files, with lib/hg's join
 *       code (the pairer's): the live rows, except where `late` is set
 *   hangar dataset check <spec> [<policy spec>] [<session>]
 *       the spec; whether a policy can run on it; whether a session can build it
 *   hangar dataset schema <spec> (<session> | --msgdir DIR --type <topic>=<msg> ... [--rate <topic>=<Hz>] ...)
 *       the row schema, as the manager would generate it for Type=dataset:<name>
 *   hangar dataset pairs <session> <module>
 *       the specs a module's manifest implies: each output, its Inputs by provenance
 *
 * <spec> is a name (DatasetDir/<name>.dataset, DatasetDir from hangard.conf
 * under $HANGAR_ROOT, else /etc/hangar/datasets) or a path ending in
 * .dataset. Exit status: 0; 1 for a warning from check; 2 for a failure. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hg_dataset.h"
#include "hg_ini.h"
#include "hg_record.h"
#include "hg_topic.h"

int hangar_dataset_main(int argc, char **argv);

static int dfail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int dfail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("hangar dataset: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	return 2;
}

static int bfmt(char *dst, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int bfmt(char *dst, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, len, fmt, ap);
	va_end(ap);
	return n;
}

static const char *root(void)
{
	const char *r = getenv("HANGAR_ROOT");
	return r && *r ? r : "";
}

/* DatasetDir from hangard.conf, else the default. */
static void dataset_dir(char *out, size_t len)
{
	bfmt(out, len, "%s/etc/hangar/datasets", root());
	char path[600];
	bfmt(path, sizeof path, "%s/etc/hangar/hangard.conf", root());
	FILE *f = fopen(path, "re");
	if (!f)
		return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';')
			continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		while (*k == ' ' || *k == '\t')
			k++;
		size_t n = strlen(k);
		while (n && (k[n - 1] == ' ' || k[n - 1] == '\t'))
			k[--n] = 0;
		while (*v == ' ' || *v == '\t')
			v++;
		n = strlen(v);
		while (n && (v[n - 1] == '\n' || v[n - 1] == ' ' || v[n - 1] == '\t'))
			v[--n] = 0;
		if (strcmp(k, "DatasetDir") == 0)
			bfmt(out, len, "%s", v);
	}
	fclose(f);
}

static int load_spec(const char *what, struct hg_ds_spec *spec)
{
	char path[700], err[400];
	size_t n = strlen(what);
	if (strchr(what, '/') || (n > 8 && strcmp(what + n - 8, ".dataset") == 0)) {
		bfmt(path, sizeof path, "%s", what);
	} else {
		char dir[600];
		dataset_dir(dir, sizeof dir);
		bfmt(path, sizeof path, "%s/%s.dataset", dir, what);
	}
	if (hg_ds_read_file(path, spec, err, sizeof err) < 0)
		return dfail("%s", err);
	return 0;
}

/* ---- Sessions ------------------------------------------------------------------ */

struct part_file {
	struct hgr_reader r;
	struct hg_schema schema;
	int open;
	char path[700];
};

static int open_topic_file(const char *session, const char *topic, struct part_file *pf, char *why, size_t whylen)
{
	memset(pf, 0, sizeof *pf);
	bfmt(pf->path, sizeof pf->path, "%s/%s.hgr", session, topic);
	if (hgr_open(&pf->r, pf->path) < 0) {
		bfmt(why, whylen, "%s: %s", pf->path, errno == EPROTO ? "not a recording" : strerror(errno));
		return -1;
	}
	char plus[700];
	bfmt(plus, sizeof plus, "%s/%s+1.hgr", session, topic);
	if (access(plus, F_OK) == 0) {
		bfmt(why, whylen, "%s has more than one epoch in this session (%s+1.hgr), which the build does not join yet",
		     topic, topic);
		hgr_reader_close(&pf->r);
		return -1;
	}
	char err[200];
	if (hg_schema_parse(pf->r.h->schema, pf->r.h->schema_bytes, &pf->schema, err, sizeof err) < 0) {
		bfmt(why, whylen, "%s: schema: %s", pf->path, err);
		hgr_reader_close(&pf->r);
		return -1;
	}
	pf->open = 1;
	return 0;
}

static struct hg_ds_topic_info file_info(const struct part_file *pf)
{
	struct hg_ds_topic_info i = { &pf->schema.type, pf->r.h->topic_id, pf->r.h->rate_mhz, pf->r.h->stale_after_ns };
	return i;
}

/* Opens the anchor's and the parts' files and binds the row type. A part
 * not recorded `full` is named. 0, or 2 after a message. */
static int open_session(const struct hg_ds_spec *spec, const char *session, struct part_file *anchor,
			struct part_file *parts, struct hg_ds_row_type *rt, int quiet)
{
	char why[800];
	if (open_topic_file(session, spec->anchor, anchor, why, sizeof why) < 0)
		return quiet ? 2 : dfail("anchor %s: %s", spec->anchor, why);
	if (anchor->r.h->policy != HGR_FULL || anchor->r.h->policy_n != 1 || anchor->r.meta)
		return quiet ? 2 : dfail("anchor %s was recorded %s in this session, not full", spec->anchor,
					 anchor->r.h->policy_text);
	struct hg_ds_topic_info pi[HG_DS_MAX_PARTS];
	for (int i = 0; i < spec->n_parts; i++) {
		const struct hg_ds_part_spec *p = &spec->parts[i];
		if (open_topic_file(session, p->topic, &parts[i], why, sizeof why) < 0)
			return quiet ? 2 : dfail("part %s: %s", p->name, why);
		if (parts[i].r.h->policy != HGR_FULL || parts[i].r.h->policy_n != 1 || parts[i].r.meta)
			return quiet ? 2 : dfail("part %s: %s was recorded %s in this session, not full: the build would join "
						 "other messages than the pairer did", p->name, p->topic, parts[i].r.h->policy_text);
		pi[i] = file_info(&parts[i]);
	}
	struct hg_ds_topic_info ai = file_info(anchor);
	char err[400];
	if (hg_ds_bind(spec, &ai, pi, rt, err, sizeof err) < 0)
		return quiet ? 2 : dfail("%s", err);
	return 0;
}

static void close_session(const struct hg_ds_spec *spec, struct part_file *anchor, struct part_file *parts)
{
	if (anchor->open)
		hgr_reader_close(&anchor->r);
	for (int i = 0; i < spec->n_parts; i++)
		if (parts[i].open)
			hgr_reader_close(&parts[i].r);
}

/* ---- build ---------------------------------------------------------------------- */

static int cmd_build(int argc, char **argv)
{
	if (argc < 2)
		return dfail("usage: hangar dataset build <spec> <session> [-o <out.hgr>]");
	const char *session = argv[1];
	char out[700];
	bfmt(out, sizeof out, "%s.hgr", strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0]);
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			bfmt(out, sizeof out, "%s", argv[++i]);
		else
			return dfail("build: unknown option %s", argv[i]);
	}
	static struct hg_ds_spec spec;
	if (load_spec(argv[0], &spec) != 0)
		return 2;
	if (!strchr(argv[0], '/') && strlen(argv[0]) < 9)
		bfmt(out, sizeof out, "%s.hgr", spec.name);
	static struct part_file anchor, parts[HG_DS_MAX_PARTS];
	static struct hg_ds_row_type rt;
	int rc = open_session(&spec, session, &anchor, parts, &rt, 0);
	if (rc)
		return rc;
	/* The output: a recording of the dataset topic, as the pairer's sink writes it. */
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	bfmt(m.name, sizeof m.name, "dataset.%s", spec.name);
	bfmt(m.writer, sizeof m.writer, "hangar dataset build");
	m.topic_id = hg_topic_id(m.name);
	m.type_hash = rt.schema.type.hash;
	m.payload_bytes = rt.row_bytes;
	struct hgr_header h;
	hgr_header_init(&h, &m, rt.schema.type.schema, rt.schema.type.schema_bytes, HGR_FULL, 1, "full",
			anchor.r.h->session, 0);
	unlink(out);
	struct hgr_writer w;
	if (hgr_create(&w, out, &h) < 0)
		return dfail("cannot create %s: %s", out, strerror(errno));
	unsigned char *row = malloc(rt.row_bytes);
	unsigned char *scratch = malloc(rt.scratch_bytes + 8);
	if (!row || !scratch)
		return dfail("out of memory");
	unsigned char boot[16];
	int zero = 1;
	for (int i = 0; i < 16; i++)
		zero &= anchor.r.h->origin_boot[i] == 0;
	if (zero)
		hg_ds_boot_id(boot);
	else
		memcpy(boot, anchor.r.h->origin_boot, 16);
	struct hg_ds_source src[HG_DS_MAX_PARTS];
	for (int i = 0; i < spec.n_parts; i++)
		src[i] = hg_ds_source_file(&parts[i].r);
	struct hg_ds_state st;
	memset(&st, 0, sizeof st);
	uint64_t rows = 0, late = 0, dropped = 0, pending_gap = 0;
	for (uint64_t i = 0; i < anchor.r.count; i++) {
		struct hg_info info;
		hgr_info(&anchor.r, i, &info);
		if (info.gap)
			pending_gap = 1;
		if (!hg_ds_rate_keeps(&rt, info.seq))
			continue;
		hg_ds_row_begin(&rt, row, &info, hgr_payload_at(&anchor.r, i), boot);
		uint64_t emitted;
		memcpy(&emitted, row + HG_DS_OFF_EMITTED, 8);
		struct hg_ds_joined jr[HG_DS_MAX_PARTS];
		int is_late = 0;
		for (int p = 0; p < spec.n_parts; p++) {
			hg_ds_join(&rt, p, row, &info, &src[p], scratch, &jr[p]);
			if (!jr[p].missing && jr[p].publish_ns > emitted)
				is_late = 1;
		}
		int keep = hg_ds_row_finish(&rt, &st, row, jr, (int)pending_gap, info.epoch);
		pending_gap = 0;
		if (!keep) {
			dropped++;
			continue;
		}
		if (is_late) {
			uint32_t flags;
			memcpy(&flags, row + HG_DS_OFF_FLAGS, 4);
			flags |= HG_DS_ROW_LATE;
			memcpy(row + HG_DS_OFF_FLAGS, &flags, 4);
			late++;
		}
		struct hg_info ri = { .result = HG_MSG, .seq = ++rows, .stamp_ns = info.stamp_ns, .publish_ns = emitted,
				      .n_inputs = 1 };
		ri.inputs[0].topic_id = anchor.r.h->topic_id;
		ri.inputs[0].seq = info.seq;
		struct hgr_record rec;
		hgr_record_init(&rec, &ri, 0, 0);
		if (hgr_append(&w, &rec, row) < 0)
			return dfail("%s: %s", out, strerror(errno));
	}
	hgr_close(&w);
	close_session(&spec, &anchor, parts);
	printf("%s: %llu rows (%llu late, %llu dropped) from %llu anchor messages, %u bytes each\n", out,
	       (unsigned long long)rows, (unsigned long long)late, (unsigned long long)dropped,
	       (unsigned long long)anchor.r.count, rt.row_bytes);
	free(row);
	free(scratch);
	return 0;
}

/* ---- check ---------------------------------------------------------------------- */

static int worst;

static void line(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("%-5s", level == 0 ? "OK" : level == 1 ? "WARN" : "FAIL");
	vprintf(fmt, ap);
	putchar('\n');
	va_end(ap);
	if (level > worst)
		worst = level;
}

/* The policy spec's Dataset= (docs/09 "Policy modules"): a [Policy] section. */
static int policy_dataset(const char *path, char *out, size_t len)
{
	static char text[65536];
	size_t n;
	if (hg_ini_read_file(path, text, sizeof text, &n) < 0)
		return -1;
	out[0] = 0;
	const char *p = text, *end = text + n;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
		if (l > 8 && strncmp(p, "Dataset", 7) == 0) {
			const char *eq = memchr(p, '=', l);
			if (eq) {
				eq++;
				while (eq < p + l && (*eq == ' ' || *eq == '\t'))
					eq++;
				size_t vl = (size_t)(p + l - eq);
				while (vl && (eq[vl - 1] == ' ' || eq[vl - 1] == '\t' || eq[vl - 1] == '\r'))
					vl--;
				bfmt(out, len, "%.*s", (int)vl, eq);
			}
		}
		p = nl ? nl + 1 : end;
	}
	return out[0] ? 0 : -1;
}

static int cmd_check(int argc, char **argv)
{
	if (argc < 1)
		return dfail("usage: hangar dataset check <spec> [<policy spec>] [<session>]");
	static struct hg_ds_spec spec;
	if (load_spec(argv[0], &spec) != 0)
		return 2;
	line(0, "spec %s: anchor %s (%s), %d parts, wait %lld ms", spec.name, spec.anchor,
	     hg_ds_role_name(spec.anchor_role), spec.n_parts, (long long)(hg_ds_wait_ns(&spec) / 1000000));
	char mdir[600], err[400];
	bfmt(mdir, sizeof mdir, "%s/etc/hangar/modules", root());
	if (hg_ds_check_manifests(&spec, mdir, err, sizeof err) == 0)
		line(0, "manifests: every provenance join is in the anchor's Inputs or Sources");
	else if (errno == ENOENT)
		line(1, "manifests: %s", err);
	else
		line(2, "manifests: %s", err);
	char bad[512];
	if (hg_ds_policy_usable(&spec, bad, sizeof bad))
		line(0, "policy: usable, no input join waits");
	else
		line(1, "policy: not usable by a policy, these input joins would have to wait: %s", bad);
	int recurrent_ok = spec.rate_every == 1 && spec.rate_kind == HG_DS_EVERY && !spec.missing_drop &&
			   spec.filter == HG_DS_MASK;
	line(recurrent_ok ? 0 : 1, "recurrent policy: %s (Rate=every:1, Missing=flag, Filter=mask)",
	     recurrent_ok ? "a row for every tick" : "not every tick");
	const char *policy = NULL, *session = NULL;
	for (int i = 1; i < argc; i++) {
		size_t n = strlen(argv[i]);
		if (n > 7 && strcmp(argv[i] + n - 7, ".policy") == 0)
			policy = argv[i];
		else
			session = argv[i];
	}
	if (policy) {
		char ds[64];
		if (policy_dataset(policy, ds, sizeof ds) < 0)
			line(2, "policy %s: no Dataset= found", policy);
		else if (strcmp(ds, spec.name) != 0)
			line(2, "policy %s: its Dataset is %s, not %s", policy, ds, spec.name);
		else
			line(0, "policy %s: Dataset=%s", policy, ds);
		line(1, "policy weights: not checked (no policy module yet: the recurrent conditions against its "
			"current weights are listed above from the spec alone)");
	}
	if (session) {
		static struct part_file anchor, parts[HG_DS_MAX_PARTS];
		static struct hg_ds_row_type rt;
		char why[800];
		int ok = 1;
		if (open_topic_file(session, spec.anchor, &anchor, why, sizeof why) < 0) {
			line(2, "session: anchor %s: %s", spec.anchor, why);
			ok = 0;
		} else if (anchor.r.h->policy != HGR_FULL || anchor.r.h->policy_n != 1) {
			line(2, "session: anchor %s recorded %s, not full", spec.anchor, anchor.r.h->policy_text);
			ok = 0;
		}
		for (int i = 0; i < spec.n_parts; i++) {
			if (open_topic_file(session, spec.parts[i].topic, &parts[i], why, sizeof why) < 0) {
				line(2, "session: part %s: %s", spec.parts[i].name, why);
				ok = 0;
			} else if (parts[i].r.h->policy != HGR_FULL || parts[i].r.h->policy_n != 1) {
				line(2, "session: part %s: %s recorded %s, not full", spec.parts[i].name, spec.parts[i].topic,
				     parts[i].r.h->policy_text);
				ok = 0;
			}
		}
		if (ok) {
			struct hg_ds_topic_info ai = file_info(&anchor), pi[HG_DS_MAX_PARTS];
			for (int i = 0; i < spec.n_parts; i++)
				pi[i] = file_info(&parts[i]);
			if (hg_ds_bind(&spec, &ai, pi, &rt, err, sizeof err) < 0)
				line(2, "session: %s", err);
			else
				line(0, "session %s: can be built, %llu anchor messages, %u-byte rows", session,
				     (unsigned long long)anchor.r.count, rt.row_bytes);
		}
		close_session(&spec, &anchor, parts);
	}
	return worst;
}

/* ---- schema --------------------------------------------------------------------- */

static int cmd_schema(int argc, char **argv)
{
	if (argc < 1)
		return dfail("usage: hangar dataset schema <spec> (<session> | --msgdir DIR --type <topic>=<msg> ... "
			     "[--rate <topic>=<Hz>] ...)");
	static struct hg_ds_spec spec;
	if (load_spec(argv[0], &spec) != 0)
		return 2;
	static struct hg_ds_row_type rt;
	if (argc >= 2 && argv[1][0] != '-') {
		static struct part_file anchor, parts[HG_DS_MAX_PARTS];
		int rc = open_session(&spec, argv[1], &anchor, parts, &rt, 0);
		if (rc)
			return rc;
		close_session(&spec, &anchor, parts);
		fputs(rt.schema.type.schema, stdout);
		return 0;
	}
	/* Types by name from a message directory. */
	char msgdir[600] = "";
	struct {
		char topic[HG_NAME_MAX + 1];
		char msg[64];
		uint32_t rate_mhz;
	} types[HG_DS_MAX_PARTS + 1];
	int n = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--msgdir") == 0 && i + 1 < argc) {
			bfmt(msgdir, sizeof msgdir, "%s", argv[++i]);
		} else if ((strcmp(argv[i], "--type") == 0 || strcmp(argv[i], "--rate") == 0) && i + 1 < argc) {
			int is_rate = argv[i][2] == 'r';
			const char *v = argv[++i];
			const char *eq = strchr(v, '=');
			if (!eq || eq == v || (size_t)(eq - v) > HG_NAME_MAX)
				return dfail("%s: expected <topic>=<value>", argv[i - 1]);
			int k = -1;
			for (int j = 0; j < n; j++)
				if (strlen(types[j].topic) == (size_t)(eq - v) && strncmp(types[j].topic, v, (size_t)(eq - v)) == 0)
					k = j;
			if (k < 0) {
				if (n == HG_DS_MAX_PARTS + 1)
					return dfail("too many --type options");
				k = n++;
				memset(&types[k], 0, sizeof types[k]);
				bfmt(types[k].topic, sizeof types[k].topic, "%.*s", (int)(eq - v), v);
			}
			if (is_rate) {
				int64_t mhz = hg_parse_rate_mhz(eq + 1);
				if (mhz < 0)
					return dfail("--rate: '%s' is not a rate", eq + 1);
				types[k].rate_mhz = (uint32_t)mhz;
			} else {
				bfmt(types[k].msg, sizeof types[k].msg, "%s", eq + 1);
			}
		} else {
			return dfail("schema: unknown option %s", argv[i]);
		}
	}
	if (!msgdir[0])
		bfmt(msgdir, sizeof msgdir, "%s/usr/share/hangar/msg", root());
	static struct hg_schema schemas[HG_DS_MAX_PARTS + 1];
	struct hg_ds_topic_info infos[HG_DS_MAX_PARTS + 1];
	for (int t = 0; t <= spec.n_parts; t++) {
		const char *topic = t == 0 ? spec.anchor : spec.parts[t - 1].topic;
		int k = -1;
		for (int j = 0; j < n; j++)
			if (strcmp(types[j].topic, topic) == 0)
				k = j;
		if (k < 0 || !types[k].msg[0])
			return dfail("no --type %s=<msg> given", topic);
		char path[700], err[200];
		static char text[HG_SCHEMA_MAX + 1];
		size_t len;
		bfmt(path, sizeof path, "%s/%s.msg", msgdir, types[k].msg);
		if (hg_ini_read_file(path, text, sizeof text, &len) < 0)
			return dfail("%s: %s", path, strerror(errno));
		if (hg_schema_parse(text, len, &schemas[t], err, sizeof err) < 0)
			return dfail("%s: %s", path, err);
		infos[t].type = &schemas[t].type;
		infos[t].topic_id = hg_topic_id(topic);
		infos[t].rate_mhz = types[k].rate_mhz;
		infos[t].stale_after_ns = types[k].rate_mhz ? 3 * 1000000000000ull / types[k].rate_mhz : 0;
	}
	char err[400];
	if (hg_ds_bind(&spec, &infos[0], &infos[1], &rt, err, sizeof err) < 0)
		return dfail("%s", err);
	fputs(rt.schema.type.schema, stdout);
	return 0;
}

/* ---- pairs ----------------------------------------------------------------------- */

struct pairs_scan {
	const char *module;
	int in_pub;
	char topic[HG_NAME_MAX + 1];
	char inputs[1024];
	int printed;
};

static void pairs_flush(struct pairs_scan *ps)
{
	if (!ps->in_pub || !ps->inputs[0])
		return;
	char name[80];
	bfmt(name, sizeof name, "%s_%s", ps->module, ps->topic);
	for (char *c = name; *c; c++)
		if (*c == '.')
			*c = '_';
	printf("# %s.dataset: what %s computes on %s, from its Inputs by provenance\n[Dataset]\nAnchor=%s\nAnchorRole=label\n",
	       name, ps->module, ps->topic, ps->topic);
	const char *p = ps->inputs;
	while (*p) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		const char *s = p;
		while (*p && *p != ' ')
			p++;
		if (memchr(s, ':', (size_t)(p - s)))
			continue; /* a topic of the other side */
		printf("\n[Input %.*s]\nJoin=provenance\n", (int)(p - s), s);
	}
	putchar('\n');
	ps->printed++;
}

static int p_section(void *ctx, const char *section, const char *arg, int line, char *why, size_t whylen)
{
	struct pairs_scan *ps = ctx;
	(void)line;
	(void)why;
	(void)whylen;
	pairs_flush(ps);
	ps->in_pub = strcmp(section, "Publish") == 0;
	ps->inputs[0] = 0;
	if (ps->in_pub)
		bfmt(ps->topic, sizeof ps->topic, "%s", arg);
	return 0;
}

static int p_key(void *ctx, const char *key, const char *value, int line, char *why, size_t whylen)
{
	struct pairs_scan *ps = ctx;
	(void)line;
	(void)why;
	(void)whylen;
	if (ps->in_pub && strcmp(key, "Inputs") == 0)
		bfmt(ps->inputs, sizeof ps->inputs, "%s", value);
	return 0;
}

static int cmd_pairs(int argc, char **argv)
{
	if (argc < 2)
		return dfail("usage: hangar dataset pairs <session> <module>");
	char path[700];
	bfmt(path, sizeof path, "%s/manifests/%s.module", argv[0], argv[1]);
	static char text[65536];
	size_t len;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0)
		return dfail("%s: %s", path, strerror(errno));
	struct pairs_scan ps = { .module = argv[1] };
	char err[300];
	if (hg_ini_parse(text, len, p_section, p_key, &ps, err, sizeof err) < 0)
		return dfail("%s: %s", path, err);
	pairs_flush(&ps);
	if (!ps.printed)
		return dfail("%s publishes nothing with Inputs=", argv[1]);
	return 0;
}

int hangar_dataset_main(int argc, char **argv)
{
	if (argc < 1) {
		fputs("usage: hangar dataset build <spec> <session> [-o <out.hgr>]\n"
		      "       hangar dataset check <spec> [<policy spec>] [<session>]\n"
		      "       hangar dataset schema <spec> (<session> | --msgdir DIR --type <topic>=<msg> ... [--rate <topic>=<Hz>] ...)\n"
		      "       hangar dataset pairs <session> <module>\n",
		      stderr);
		return 2;
	}
	if (strcmp(argv[0], "build") == 0)
		return cmd_build(argc - 1, argv + 1);
	if (strcmp(argv[0], "check") == 0)
		return cmd_check(argc - 1, argv + 1);
	if (strcmp(argv[0], "schema") == 0)
		return cmd_schema(argc - 1, argv + 1);
	if (strcmp(argv[0], "pairs") == 0)
		return cmd_pairs(argc - 1, argv + 1);
	/* `hangar dataset <spec> <session>`: a build. */
	if (argc >= 2 && argv[0][0] != '-' && argv[1][0] != '-')
		return cmd_build(argc, argv);
	return dfail("unknown subcommand %s (build, check, schema, pairs)", argv[0]);
}
