// SPDX-License-Identifier: GPL-3.0-or-later
/* pairer: the live dataset pairer of docs/06-recorder.md "Live: the pairer"
 * (modules/pairer/README.md).
 *
 *   pairer [<pairer.conf>]
 *
 * For each dataset spec its configuration lists it reads the anchor topic in
 * order and, for each anchor message, joins the parts from their rings with
 * lib/hg's join code (hg_dataset.h): provenance joins and asof joins with
 * offset + Lateness <= 0 at once, the others at the row's emitted_ns. It
 * applies Where and Filter, sets the row's flags, and publishes each row on
 * dataset.<name>; its status, per dataset and part, goes on pairer.status.
 *
 * GPL-3.0-or-later; C11; libc and lib/hg only. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
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
#include "pairer_status.h"

#define PR_MAX_DATASETS 8
#define PR_PATH 600

struct pending {
	unsigned char *row;
	struct hg_info ainfo;
	uint64_t emitted_ns;
	uint32_t joined;              /* bit i: part i joined */
	int gap;
	struct hg_ds_joined jr[HG_DS_MAX_PARTS];
};

struct dataset {
	char name[HG_DS_NAME];
	struct hg_ds_spec spec;
	struct hg_ds_row_type *rt;
	struct hg_schema *schemas;     /* anchor, then the parts */
	hg_sub *anchor;
	hg_sub *parts[HG_DS_MAX_PARTS];
	struct hg_ds_source src[HG_DS_MAX_PARTS];
	int bound;
	hg_pub *pub;
	unsigned char *apayload, *scratch;
	struct pending *pend;
	int pcap, phead, pcount;
	int pending_gap;
	struct hg_ds_state state;
	unsigned char boot[16];
	uint64_t rows, dropped, masked, forced;
	uint64_t misses[HG_DS_MAX_PARTS], latency_max[HG_DS_MAX_PARTS];
	/* until the bridge: the rows also go to a file, where a test and a
	 * topside without a bridge can read them. */
	struct hgr_writer sink;
	int sink_open;
	uint32_t anchor_id;
};

static struct {
	char datasets[PR_MAX_DATASETS][HG_DS_NAME];
	int n;
	char dataset_dir[PR_PATH];
	char sink_dir[PR_PATH];
	int pending;
} conf;

static struct dataset D[PR_MAX_DATASETS];
static hg_pub *status_pub;
static volatile sig_atomic_t stopping;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("pairer: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
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

static void on_term(int sig)
{
	(void)sig;
	stopping = 1;
	hg_wait_interrupt();
}

/* ---- pairer.conf -------------------------------------------------------------- */

static int c_section(void *ctx, const char *section, const char *arg, int line, char *why, size_t whylen)
{
	int *seen = ctx;
	(void)line;
	if (strcmp(section, "Pairer") != 0 || arg[0]) {
		bfmt(why, whylen, "only [Pairer] is known here");
		return -1;
	}
	if ((*seen)++) {
		bfmt(why, whylen, "[Pairer] given twice");
		return -1;
	}
	return 0;
}

static int c_key(void *ctx, const char *key, const char *value, int line, char *why, size_t whylen)
{
	static uint32_t keys;
	(void)ctx;
	(void)line;
	int k;
	if (strcmp(key, "Datasets") == 0)
		k = 0;
	else if (strcmp(key, "DatasetDir") == 0)
		k = 1;
	else if (strcmp(key, "Sink") == 0)
		k = 2;
	else if (strcmp(key, "Pending") == 0)
		k = 3;
	else {
		bfmt(why, whylen, "unknown key %s in [Pairer]", key);
		return -1;
	}
	if (keys & (1u << k)) {
		bfmt(why, whylen, "%s given twice", key);
		return -1;
	}
	keys |= 1u << k;
	if (k == 0) {
		const char *p = value;
		while (*p) {
			while (*p == ' ' || *p == '\t')
				p++;
			if (!*p)
				break;
			const char *s = p;
			while (*p && *p != ' ' && *p != '\t')
				p++;
			size_t n = (size_t)(p - s);
			if (n >= HG_DS_NAME) {
				bfmt(why, whylen, "Datasets: a name over %d bytes", HG_DS_NAME - 1);
				return -1;
			}
			if (conf.n == PR_MAX_DATASETS) {
				bfmt(why, whylen, "Datasets: more than %d", PR_MAX_DATASETS);
				return -1;
			}
			for (int i = 0; i < conf.n; i++)
				if (strlen(conf.datasets[i]) == n && memcmp(conf.datasets[i], s, n) == 0) {
					bfmt(why, whylen, "Datasets: %.*s twice", (int)n, s);
					return -1;
				}
			memcpy(conf.datasets[conf.n], s, n);
			conf.datasets[conf.n][n] = 0;
			conf.n++;
		}
		if (!conf.n) {
			bfmt(why, whylen, "Datasets: empty");
			return -1;
		}
	} else if (k == 1 || k == 2) {
		if (value[0] != '/' || strlen(value) >= PR_PATH - 80) {
			bfmt(why, whylen, "%s: an absolute path", key);
			return -1;
		}
		bfmt(k == 1 ? conf.dataset_dir : conf.sink_dir, PR_PATH, "%s", value);
	} else {
		char *e;
		long v = strtol(value, &e, 10);
		if (e == value || *e || v < 4 || v > 65536) {
			bfmt(why, whylen, "Pending: 4 to 65536 rows");
			return -1;
		}
		conf.pending = (int)v;
	}
	return 0;
}

static int conf_load(const char *path, char *err, size_t errlen)
{
	const char *root = getenv("HANGAR_ROOT");
	bfmt(conf.dataset_dir, sizeof conf.dataset_dir, "%s/etc/hangar/datasets", root && *root ? root : "");
	conf.pending = 256;
	if (!path)
		return 0;
	static char text[65536];
	size_t len;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		bfmt(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	int seen = 0;
	char why[300];
	if (hg_ini_parse(text, len, c_section, c_key, &seen, why, sizeof why) < 0) {
		bfmt(err, errlen, "%s: %s", path, why);
		return -1;
	}
	if (!conf.n) {
		bfmt(err, errlen, "%s: [Pairer] Datasets= names no dataset", path);
		return -1;
	}
	return 0;
}

/* ---- Binding ------------------------------------------------------------------ */

static int sub_info(hg_sub *s, struct hg_schema *sch, struct hg_ds_topic_info *out)
{
	struct hg_topic_meta m;
	uint32_t slen;
	const char *text = hg_sub_schema(s, &slen);
	if (!text || hg_sub_meta(s, &m) < 0)
		return -1;
	if (hg_schema_parse(text, slen, sch, NULL, 0) < 0)
		return -1;
	out->type = &sch->type;
	out->topic_id = m.topic_id;
	out->rate_mhz = m.rate_mhz;
	out->stale_after_ns = m.stale_after_ns;
	return 0;
}

static void sink_open(struct dataset *d)
{
	if (!conf.sink_dir[0])
		return;
	struct hg_topic_meta m;
	memset(&m, 0, sizeof m);
	bfmt(m.name, sizeof m.name, "dataset.%s", d->name);
	bfmt(m.writer, sizeof m.writer, "pairer");
	m.topic_id = hg_topic_id(m.name);
	m.type_hash = d->rt->schema.type.hash;
	m.payload_bytes = d->rt->row_bytes;
	struct hgr_header h;
	hgr_header_init(&h, &m, d->rt->schema.type.schema, d->rt->schema.type.schema_bytes, HGR_FULL, 1, "full",
			"live", 0);
	char path[PR_PATH + 80];
	bfmt(path, sizeof path, "%s/dataset.%s.hgr", conf.sink_dir, d->name);
	unlink(path);
	if (hgr_create(&d->sink, path, &h) < 0) {
		logf_("%s: cannot create the sink %s: %s", d->name, path, strerror(errno));
		return;
	}
	d->sink_open = 1;
}

/* Once the anchor and every part exist: the row type, the topic, the sink. */
static void try_bind(struct dataset *d)
{
	struct hg_ds_topic_info ai, pi[HG_DS_MAX_PARTS];
	if (sub_info(d->anchor, &d->schemas[0], &ai) < 0)
		return;
	for (int i = 0; i < d->spec.n_parts; i++)
		if (sub_info(d->parts[i], &d->schemas[1 + i], &pi[i]) < 0)
			return;
	char err[300];
	if (hg_ds_bind(&d->spec, &ai, pi, d->rt, err, sizeof err) < 0) {
		logf_("%s: %s", d->name, err);
		exit(78);
	}
	d->rt->schema.type.name = "dataset";
	d->anchor_id = ai.topic_id;
	free(d->apayload);
	free(d->scratch);
	d->apayload = malloc(ai.type->payload_bytes + 8);
	d->scratch = malloc(d->rt->scratch_bytes + 8);
	d->pend = calloc((size_t)conf.pending, sizeof *d->pend);
	unsigned char *rows = d->pend ? malloc((size_t)conf.pending * d->rt->row_bytes) : NULL;
	if (!d->apayload || !d->scratch || !d->pend || !rows) {
		logf_("%s: out of memory", d->name);
		exit(3);
	}
	for (int i = 0; i < conf.pending; i++)
		d->pend[i].row = rows + (size_t)i * d->rt->row_bytes;
	d->pcap = conf.pending;
	for (int i = 0; i < d->spec.n_parts; i++)
		d->src[i] = hg_ds_source_sub(d->parts[i]);
	struct hg_topic_meta am;
	hg_sub_meta(d->anchor, &am);
	int zero = 1;
	for (int i = 0; i < 16; i++)
		zero &= am.origin_boot[i] == 0;
	if (zero)
		hg_ds_boot_id(d->boot);
	else
		memcpy(d->boot, am.origin_boot, 16);
	char topic[HG_NAME_MAX + 1];
	bfmt(topic, sizeof topic, "dataset.%s", d->name);
	struct hg_pub_opts o = { .slots = 256 };
	d->pub = hg_pub_open(topic, &d->rt->schema.type, &o);
	if (!d->pub) {
		logf_("%s: cannot open %s: %s%s", d->name, topic, strerror(errno),
		      errno == EPROTOTYPE ? " (the declared type is not this spec's row type: hangar dataset schema)" : "");
		exit(78);
	}
	sink_open(d);
	d->bound = 1;
	logf_("%s: %d parts, %u-byte rows, wait %lld ms, hash %016llx", d->name, d->spec.n_parts, d->rt->row_bytes,
	      (long long)(d->rt->wait_ns / 1000000), (unsigned long long)d->rt->schema.type.hash);
}

/* ---- Rows --------------------------------------------------------------------- */

static void join_part(struct dataset *d, struct pending *p, int i)
{
	hg_ds_join(d->rt, i, p->row, &p->ainfo, &d->src[i], d->scratch, &p->jr[i]);
	p->joined |= 1u << i;
	if (p->jr[i].missing)
		d->misses[i]++;
	else if (p->jr[i].latency_ns > d->latency_max[i])
		d->latency_max[i] = p->jr[i].latency_ns;
}

static void emit(struct dataset *d, struct pending *p)
{
	for (int i = 0; i < d->spec.n_parts; i++)
		if (!(p->joined & (1u << i)))
			join_part(d, p, i);
	int keep = hg_ds_row_finish(d->rt, &d->state, p->row, p->jr, p->gap, p->ainfo.epoch);
	if (!keep) {
		d->dropped++;
		return;
	}
	uint32_t flags;
	memcpy(&flags, p->row + HG_DS_OFF_FLAGS, 4);
	if (flags & HG_DS_ROW_MASKED)
		d->masked++;
	uint64_t seq = hg_pub_next_seq(d->pub);
	struct hg_input ref = { .topic_id = d->anchor_id, .seq = p->ainfo.seq };
	if (hg_publish_with(d->pub, p->row, d->rt->row_bytes, p->ainfo.stamp_ns, 0, &ref, 1) < 0)
		hg_publish_with(d->pub, p->row, d->rt->row_bytes, p->ainfo.stamp_ns, HG_NO_REFS, NULL, 0);
	d->rows++;
	if (d->sink_open) {
		struct hg_info info = { .result = HG_MSG, .seq = seq, .stamp_ns = p->ainfo.stamp_ns,
					.publish_ns = hg_now_ns(), .n_inputs = 1 };
		info.inputs[0] = ref;
		struct hgr_record r;
		hgr_record_init(&r, &info, 0, 0);
		if (hgr_append(&d->sink, &r, p->row) < 0) {
			logf_("%s: sink: %s", d->name, strerror(errno));
			hgr_close(&d->sink);
			d->sink_open = 0;
		}
	}
}

/* Pending rows whose wait is over, in order. */
static void flush(struct dataset *d, uint64_t now, int all)
{
	while (d->pcount) {
		struct pending *p = &d->pend[d->phead];
		if (!all && p->emitted_ns > now)
			break;
		emit(d, p);
		d->phead = (d->phead + 1) % d->pcap;
		d->pcount--;
	}
}

static void read_anchor(struct dataset *d)
{
	struct hg_info info;
	for (int n = 0; n < 4096; n++) {
		enum hg_result r = hg_next(d->anchor, d->apayload, d->rt->anchor_payload_bytes + 8, &info);
		if (r != HG_MSG)
			break;
		if (info.gap)
			d->pending_gap = 1;
		if (!hg_ds_rate_keeps(d->rt, info.seq))
			continue;
		if (d->pcount == d->pcap) {
			/* More anchors waiting than the buffer holds: the oldest row
			 * closes now, with what has arrived. */
			struct pending *p = &d->pend[d->phead];
			emit(d, p);
			d->phead = (d->phead + 1) % d->pcap;
			d->pcount--;
			d->forced++;
		}
		struct pending *p = &d->pend[(d->phead + d->pcount) % d->pcap];
		d->pcount++;
		p->ainfo = info;
		p->joined = 0;
		p->gap = d->pending_gap;
		d->pending_gap = 0;
		memset(p->jr, 0, sizeof p->jr);
		hg_ds_row_begin(d->rt, p->row, &info, d->apayload, d->boot);
		memcpy(&p->emitted_ns, p->row + HG_DS_OFF_EMITTED, 8);
		for (int i = 0; i < d->spec.n_parts; i++)
			if (d->rt->parts[i].immediate)
				join_part(d, p, i);
	}
}

/* ---- Status ----------------------------------------------------------------- */

static void publish_status(void)
{
	if (!status_pub)
		return;
	int total = 0;
	for (int k = 0; k < conf.n; k++)
		total += 1 + D[k].spec.n_parts;
	int index = 0;
	char line[128];
	line[0] = 0;
	for (int k = 0; k < conf.n; k++) {
		struct dataset *d = &D[k];
		for (int i = -1; i < d->spec.n_parts; i++) {
			struct pairer_status s;
			memset(&s, 0, sizeof s);
			bfmt(s.dataset, sizeof s.dataset, "%s", d->name);
			s.index = (uint8_t)index++;
			s.count = (uint8_t)total;
			s.rows = d->rows;
			s.dropped = d->dropped;
			s.masked = d->masked;
			if (i >= 0) {
				bfmt(s.part, sizeof s.part, "%s", d->spec.parts[i].name);
				s.misses = d->misses[i];
				s.latency_ns = d->latency_max[i];
				struct hg_topic_meta m;
				if (d->bound && hg_sub_meta(d->parts[i], &m) == 0) {
					s.slots = m.slot_count;
					if (m.rate_mhz) {
						/* (Slots - 1) / rate must exceed the latency. */
						uint64_t need = (s.latency_ns * m.rate_mhz + 999999999999ull) / 1000000000000ull + 1;
						uint32_t p2 = 2;
						while (p2 < need && p2 < 65536)
							p2 *= 2;
						s.needed_slots = p2;
					}
				}
			}
			hg_publish(status_pub, &s, sizeof s, hg_now_ns());
		}
		size_t l = strlen(line);
		bfmt(line + l, sizeof line - l, "%s%s %llu rows", l ? "; " : "", d->name, (unsigned long long)d->rows);
	}
	hg_status(line);
}

/* ---- Main -------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_term };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	char err[400];
	if (conf_load(argc > 1 ? argv[1] : NULL, err, sizeof err) < 0) {
		logf_("%s", err);
		hg_status(err);
		return 78;
	}
	if (hg_init() < 0) {
		logf_("hg_init: %s (%s)", hg_init_error(), strerror(errno));
		return 3;
	}
	/* The specs, and the manifest's dataset outputs against the list. */
	const struct hg_wiring *w = hg_wiring();
	if (w) {
		int listed = 0;
		for (int i = 0; i < w->n_pubs; i++) {
			if (strncmp(w->pubs[i].topic, "dataset.", 8) != 0)
				continue;
			listed++;
			int found = 0;
			for (int k = 0; k < conf.n; k++)
				found |= strcmp(conf.datasets[k], w->pubs[i].topic + 8) == 0;
			if (!found) {
				logf_("the manifest publishes %s, which pairer.conf does not list", w->pubs[i].topic);
				return 78;
			}
		}
		if (listed != conf.n) {
			logf_("pairer.conf lists %d datasets, the manifest publishes %d dataset.* topics", conf.n, listed);
			return 78;
		}
	}
	hg_sub *subs[HG_MAX_WAIT];
	int n = 0;
	for (int k = 0; k < conf.n; k++) {
		struct dataset *d = &D[k];
		bfmt(d->name, sizeof d->name, "%s", conf.datasets[k]);
		char path[PR_PATH + 80];
		bfmt(path, sizeof path, "%s/%s.dataset", conf.dataset_dir, d->name);
		if (hg_ds_read_file(path, &d->spec, err, sizeof err) < 0) {
			logf_("%s", err);
			hg_status(err);
			return 78;
		}
		d->rt = calloc(1, sizeof *d->rt);
		d->schemas = calloc(1 + (size_t)d->spec.n_parts, sizeof *d->schemas);
		if (!d->rt || !d->schemas) {
			logf_("out of memory");
			return 3;
		}
		if (n + 1 + d->spec.n_parts > HG_MAX_WAIT) {
			logf_("more topics than one wait holds");
			return 78;
		}
		d->anchor = hg_sub_open(d->spec.anchor, NULL);
		if (!d->anchor) {
			logf_("%s: cannot subscribe to %s: %s", d->name, d->spec.anchor, strerror(errno));
			return 78;
		}
		subs[n++] = d->anchor;
		for (int i = 0; i < d->spec.n_parts; i++) {
			d->parts[i] = hg_sub_open(d->spec.parts[i].topic, NULL);
			if (!d->parts[i]) {
				logf_("%s: cannot subscribe to %s: %s", d->name, d->spec.parts[i].topic, strerror(errno));
				return 78;
			}
			subs[n++] = d->parts[i];
		}
	}
	struct hg_pub_opts eo = { .slots = 256 };
	status_pub = hg_pub_open("pairer.status", &PAIRER_STATUS_TYPE, &eo);
	if (!status_pub)
		logf_("cannot open pairer.status: %s", strerror(errno));
	hg_ready();
	logf_("%d datasets from %s%s%s", conf.n, conf.dataset_dir, conf.sink_dir[0] ? "; sink " : "", conf.sink_dir);
	uint64_t last_tick = hg_now_ns();
	while (!stopping) {
		int timeout = 10;
		uint64_t now = hg_now_ns();
		for (int k = 0; k < conf.n; k++)
			if (D[k].bound && D[k].pcount) {
				uint64_t e = D[k].pend[D[k].phead].emitted_ns;
				int ms = e > now ? (int)((e - now) / 1000000u) : 0;
				if (ms < timeout)
					timeout = ms;
			}
		unsigned r = hg_wait(subs, n, timeout);
		if (r & HG_READY_INTERRUPT)
			break;
		now = hg_now_ns();
		for (int k = 0; k < conf.n; k++) {
			struct dataset *d = &D[k];
			if (!d->bound) {
				try_bind(d);
				if (!d->bound)
					continue;
			}
			read_anchor(d);
			flush(d, hg_now_ns(), 0);
		}
		if (now - last_tick >= 1000000000u) {
			last_tick = now;
			publish_status();
			hg_alive();
		}
	}
	for (int k = 0; k < conf.n; k++) {
		struct dataset *d = &D[k];
		if (!d->bound)
			continue;
		flush(d, 0, 1);
		if (d->sink_open)
			hgr_close(&d->sink);
		hg_pub_close(d->pub);
	}
	publish_status();
	if (status_pub)
		hg_pub_close(status_pub);
	logf_("stopped");
	return 0;
}
