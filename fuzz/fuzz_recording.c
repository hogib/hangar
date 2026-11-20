// SPDX-License-Identifier: GPL-3.0-or-later
/* Recordings and dataset specs (lib/hg/hg_record.c, hg_dataset.c, hg_ini.c): a
 * .hgr file as hgr_open maps it, and the offline build of `hangar dataset
 * build` over it (hangar/hangar_dataset.c cmd_build, followed here step by
 * step). Recordings are copied from the vehicle and between people; a spec is
 * a text file the operator writes.
 *
 * Input: `<spec text> NUL <the bytes of a .hgr file>`. The one file stands for
 * the anchor's recording and for every part's. The reader's own entry points
 * are run whether or not the spec parses. */
#include "fuzz.h"
#include "hg_dataset.h"
#include "hg_record.h"

#define ROW_CAP (1u << 20)
#define MAX_ROWS 48

static struct hg_ds_spec spec;
static struct hg_ds_row_type rt;
static struct hg_schema sch;

static void reader_alone(const struct hgr_reader *r)
{
	FZ_CHECK(r->record_bytes >= HGR_RECORD_HEADER);
	FZ_CHECK(HGR_HEADER_BYTES + r->count * r->record_bytes <= r->len);
	uint64_t sum = 0;
	for (uint64_t i = 0; i < r->count && i < MAX_ROWS; i++) {
		const struct hgr_record *rec = hgr_at(r, i);
		const unsigned char *p = hgr_payload_at(r, i);
		struct hg_info info;
		hgr_info(r, i, &info);
		FZ_CHECK(info.n_inputs <= HG_MAX_REFS);
		sum += rec->seq;
		if (p && r->payload_bytes)
			sum += p[0] + p[r->payload_bytes - 1]; /* the whole payload is in the mapping */
		(void)hgr_find_seq(r, rec->seq);
		(void)hgr_find_stamp(r, rec->stamp_ns);
	}
	FZ_CHECK(hgr_at(r, r->count) == NULL);
	(void)hgr_find_seq(r, sum);
	(void)hgr_find_stamp(r, UINT64_MAX);
	(void)hgr_find_stamp(r, 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const uint8_t *sep = memchr(data, 0, size);
	size_t tlen = sep ? (size_t)(sep - data) : size;
	char *text = fz_text(data, tlen);
	char err[400], path[64];
	int have_spec = hg_ds_parse(text, tlen, "fuzz", &spec, err, sizeof err) == 0;
	if (!have_spec)
		FZ_NOTE("spec refused: %s", err);
	if (have_spec) {
		FZ_CHECK(spec.n_parts >= 0 && spec.n_parts <= HG_DS_MAX_PARTS);
		(void)hg_ds_wait_ns(&spec);
		char bad[64];
		(void)hg_ds_policy_usable(&spec, bad, sizeof bad);
	}
	free(text);
	if (!sep)
		return 0;
	int fd = fz_file(sep + 1, size - tlen - 1, path);
	struct hgr_reader r;
	if (hgr_open(&r, path) < 0) {
		FZ_NOTE("not a recording");
		close(fd);
		return 0;
	}
	reader_alone(&r);
	FZ_NOTE("recording: %llu records of %u bytes%s", (unsigned long long)r.count, r.record_bytes, r.meta ? ", meta" : "");
	if (hgr_schema(&r, &sch, err, sizeof err) < 0 || !have_spec || r.meta)
		goto out;
	struct hg_ds_topic_info ai = { &sch.type, r.h->topic_id, r.h->rate_mhz, r.h->stale_after_ns };
	struct hg_ds_topic_info pi[HG_DS_MAX_PARTS];
	for (int i = 0; i < spec.n_parts; i++)
		pi[i] = ai;
	if (hg_ds_bind(&spec, &ai, pi, &rt, err, sizeof err) < 0) {
		FZ_NOTE("bind refused: %s", err);
		goto out;
	}
	FZ_NOTE("bound: rows of %u bytes", rt.row_bytes);
	FZ_CHECK(rt.row_bytes == rt.schema.type.payload_bytes);
	if (rt.row_bytes > ROW_CAP || rt.scratch_bytes > ROW_CAP)
		goto out;
	/* Exactly the sizes the build allocates, so that a write past either
	 * is a write past a heap block. */
	unsigned char *row = malloc(rt.row_bytes ? rt.row_bytes : 1);
	unsigned char *scratch = malloc(rt.scratch_bytes + 8);
	if (!row || !scratch)
		abort();
	unsigned char boot[16] = { 1 };
	struct hg_ds_source src = hg_ds_source_file(&r);
	struct hg_ds_state st;
	memset(&st, 0, sizeof st);
	for (uint64_t i = 0; i < r.count && i < MAX_ROWS; i++) {
		struct hg_info info;
		hgr_info(&r, i, &info);
		if (!hg_ds_rate_keeps(&rt, info.seq))
			continue;
		hg_ds_row_begin(&rt, row, &info, hgr_payload_at(&r, i), boot);
		struct hg_ds_joined jr[HG_DS_MAX_PARTS];
		for (int p = 0; p < spec.n_parts; p++)
			hg_ds_join(&rt, p, row, &info, &src, scratch, &jr[p]);
		(void)hg_ds_row_finish(&rt, &st, row, jr, info.gap != 0, info.epoch);
		(void)hg_ds_where(&rt, row);
	}
	free(row);
	free(scratch);
out:
	hgr_reader_close(&r);
	close(fd);
	return 0;
}
