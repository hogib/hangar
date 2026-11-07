// SPDX-License-Identifier: LGPL-3.0-or-later
/* Recording files: the header and record layouts of hg_record.h, a writer
 * that appends with one writev per record, a reader over a read-only
 * mapping with binary searches by seq and by stamp. */
#define _GNU_SOURCE
#include "hg_record.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(struct hgr_header) == HGR_HEADER_BYTES, "layout");
_Static_assert(offsetof(struct hgr_header, record_bytes) == 16, "layout");
_Static_assert(offsetof(struct hgr_header, slot_count) == 24, "layout");
_Static_assert(offsetof(struct hgr_header, payload_bytes) == 32, "layout");
_Static_assert(offsetof(struct hgr_header, flags) == 36, "layout");
_Static_assert(offsetof(struct hgr_header, policy_n) == 40, "layout");
_Static_assert(offsetof(struct hgr_header, records_offset) == 48, "layout");
_Static_assert(offsetof(struct hgr_header, topic_id) == 56, "layout");
_Static_assert(offsetof(struct hgr_header, rate_mhz) == 60, "layout");
_Static_assert(offsetof(struct hgr_header, type_hash) == 64, "layout");
_Static_assert(offsetof(struct hgr_header, name) == 72, "layout");
_Static_assert(offsetof(struct hgr_header, writer) == 120, "layout");
_Static_assert(offsetof(struct hgr_header, policy) == 152, "layout");
_Static_assert(offsetof(struct hgr_header, schema_bytes) == 156, "layout");
_Static_assert(offsetof(struct hgr_header, created_real_ns) == 160, "layout");
_Static_assert(offsetof(struct hgr_header, first_seq) == 168, "layout");
_Static_assert(offsetof(struct hgr_header, stale_after_ns) == 176, "layout");
_Static_assert(offsetof(struct hgr_header, created_mono_ns) == 184, "layout");
_Static_assert(offsetof(struct hgr_header, session) == 192, "layout");
_Static_assert(offsetof(struct hgr_header, origin_boot) == 256, "layout");
_Static_assert(offsetof(struct hgr_header, epoch) == 272, "layout");
_Static_assert(offsetof(struct hgr_header, policy_text) == 280, "layout");
_Static_assert(offsetof(struct hgr_header, schema) == 512, "layout");
_Static_assert(sizeof(struct hgr_record) == HGR_RECORD_HEADER, "layout");
_Static_assert(offsetof(struct hgr_record, gap) == 32, "layout");
_Static_assert(offsetof(struct hgr_record, inputs) == 64, "layout");

static uint64_t real_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void hgr_header_init(struct hgr_header *h, const struct hg_topic_meta *m, const char *schema,
		     uint32_t schema_len, enum hgr_policy policy, uint64_t policy_n,
		     const char *policy_text, const char *session, uint32_t epoch)
{
	memset(h, 0, sizeof *h);
	memcpy(h->magic, HGR_MAGIC, 8);
	h->version = 1;
	h->header_bytes = HGR_HEADER_BYTES;
	h->record_header_bytes = HGR_RECORD_HEADER;
	h->payload_bytes = m->payload_bytes;
	h->flags = policy == HGR_META ? HGR_FLAG_META : 0;
	h->record_bytes = HGR_RECORD_HEADER + (policy == HGR_META ? 0 : m->payload_bytes);
	h->slot_count = m->slot_count;
	h->policy = (uint32_t)policy;
	h->policy_n = policy_n ? policy_n : 1;
	h->records_offset = HGR_HEADER_BYTES;
	h->topic_id = m->topic_id;
	h->rate_mhz = m->rate_mhz;
	h->type_hash = m->type_hash;
	memcpy(h->name, m->name, sizeof h->name);
	memcpy(h->writer, m->writer, sizeof h->writer);
	if (schema_len > sizeof h->schema)
		schema_len = sizeof h->schema;
	h->schema_bytes = schema_len;
	if (schema && schema_len)
		memcpy(h->schema, schema, schema_len);
	h->created_real_ns = real_ns();
	h->created_mono_ns = hg_now_ns();
	h->first_seq = m->first_seq;
	h->stale_after_ns = m->stale_after_ns;
	if (session)
		snprintf(h->session, sizeof h->session, "%s", session);
	memcpy(h->origin_boot, m->origin_boot, 16);
	h->epoch = epoch;
	if (policy_text)
		snprintf(h->policy_text, sizeof h->policy_text, "%s", policy_text);
}

void hgr_record_init(struct hgr_record *r, const struct hg_info *info, uint64_t lost, uint64_t skipped)
{
	memset(r, 0, sizeof *r);
	r->seq = info->seq;
	r->stamp_ns = info->stamp_ns;
	r->publish_ns = info->publish_ns;
	r->n_inputs = info->n_inputs > HG_MAX_REFS ? HG_MAX_REFS : info->n_inputs;
	r->flags = info->flags & 0xffffu;
	if (lost) {
		r->flags |= HGR_REC_GAP;
		r->gap = lost;
	} else if (skipped || (info->flags & HG_SLOT_DECIMATED)) {
		r->gap = skipped;
	}
	memcpy(r->inputs, info->inputs, sizeof r->inputs);
}

int hgr_header_valid(const struct hgr_header *h, size_t file_len)
{
	if (file_len < HGR_HEADER_BYTES || memcmp(h->magic, HGR_MAGIC, 8) != 0 || h->version != 1 ||
	    h->header_bytes != HGR_HEADER_BYTES || h->record_header_bytes != HGR_RECORD_HEADER ||
	    h->records_offset != HGR_HEADER_BYTES || h->schema_bytes > sizeof h->schema ||
	    h->record_bytes < HGR_RECORD_HEADER || h->payload_bytes % 8 != 0 ||
	    h->record_bytes != HGR_RECORD_HEADER + ((h->flags & HGR_FLAG_META) ? 0 : h->payload_bytes)) {
		errno = EPROTO;
		return -1;
	}
	return 0;
}

/* ---- Writer -------------------------------------------------------------- */

static int write_all(int fd, const void *p, size_t len)
{
	const unsigned char *b = p;
	while (len) {
		ssize_t n = write(fd, b, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		b += n;
		len -= (size_t)n;
	}
	return 0;
}

int hgr_create(struct hgr_writer *w, const char *path, const struct hgr_header *h)
{
	memset(w, 0, sizeof *w);
	w->fd = -1;
	if (hgr_header_valid(h, HGR_HEADER_BYTES) < 0)
		return -1;
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0640);
	if (fd < 0)
		return -1;
	if (write_all(fd, h, sizeof *h) < 0) {
		int e = errno;
		close(fd);
		unlink(path);
		errno = e;
		return -1;
	}
	w->fd = fd;
	w->record_bytes = (uint32_t)h->record_bytes;
	w->payload_bytes = h->payload_bytes;
	w->meta = (h->flags & HGR_FLAG_META) != 0;
	w->bytes = sizeof *h;
	return 0;
}

int hgr_append(struct hgr_writer *w, const struct hgr_record *r, const void *payload)
{
	if (w->fd < 0) {
		errno = EBADF;
		return -1;
	}
	struct iovec iov[2] = { { (void *)r, HGR_RECORD_HEADER }, { (void *)payload, w->payload_bytes } };
	int n = w->meta ? 1 : 2;
	size_t want = w->record_bytes;
	size_t done = 0;
	while (done < want) {
		ssize_t k = writev(w->fd, iov, n);
		if (k < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)k;
		if (done == want)
			break;
		/* A short writev: advance the vectors and go on. */
		size_t skip = (size_t)k;
		for (int i = 0; i < n && skip; i++) {
			size_t take = skip < iov[i].iov_len ? skip : iov[i].iov_len;
			iov[i].iov_base = (unsigned char *)iov[i].iov_base + take;
			iov[i].iov_len -= take;
			skip -= take;
		}
	}
	w->records++;
	w->bytes += want;
	return 0;
}

int hgr_close(struct hgr_writer *w)
{
	if (w->fd < 0)
		return 0;
	int rc = close(w->fd);
	w->fd = -1;
	return rc;
}

/* ---- Reader -------------------------------------------------------------- */

int hgr_open(struct hgr_reader *r, const char *path)
{
	memset(r, 0, sizeof *r);
	r->fd = -1;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct stat st;
	if (fstat(fd, &st) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	if ((size_t)st.st_size < HGR_HEADER_BYTES) {
		close(fd);
		errno = EPROTO;
		return -1;
	}
	void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	const struct hgr_header *h = m;
	if (hgr_header_valid(h, (size_t)st.st_size) < 0) {
		munmap(m, (size_t)st.st_size);
		close(fd);
		errno = EPROTO;
		return -1;
	}
	r->fd = fd;
	r->map = m;
	r->len = (size_t)st.st_size;
	r->h = h;
	r->record_bytes = (uint32_t)h->record_bytes;
	r->payload_bytes = h->payload_bytes;
	r->meta = (h->flags & HGR_FLAG_META) != 0;
	r->count = (r->len - HGR_HEADER_BYTES) / r->record_bytes;
	return 0;
}

void hgr_reader_close(struct hgr_reader *r)
{
	if (r->map)
		munmap((void *)r->map, r->len);
	if (r->fd >= 0)
		close(r->fd);
	r->map = NULL;
	r->fd = -1;
	r->count = 0;
}

const struct hgr_record *hgr_at(const struct hgr_reader *r, uint64_t i)
{
	if (i >= r->count)
		return NULL;
	return (const struct hgr_record *)(const void *)(r->map + HGR_HEADER_BYTES + i * r->record_bytes);
}

const void *hgr_payload_at(const struct hgr_reader *r, uint64_t i)
{
	if (i >= r->count || r->meta)
		return NULL;
	return r->map + HGR_HEADER_BYTES + i * r->record_bytes + HGR_RECORD_HEADER;
}

int64_t hgr_find_seq(const struct hgr_reader *r, uint64_t seq)
{
	uint64_t lo = 0, hi = r->count;
	while (lo < hi) {
		uint64_t mid = lo + (hi - lo) / 2;
		uint64_t s = hgr_at(r, mid)->seq;
		if (s == seq)
			return (int64_t)mid;
		if (s < seq)
			lo = mid + 1;
		else
			hi = mid;
	}
	return -1;
}

static uint64_t rec_stamp(const struct hgr_record *rec)
{
	return rec->stamp_ns ? rec->stamp_ns : rec->publish_ns;
}

int64_t hgr_find_stamp(const struct hgr_reader *r, uint64_t t_ns)
{
	/* The first record stamped after t; the one before it is the answer. */
	uint64_t lo = 0, hi = r->count;
	while (lo < hi) {
		uint64_t mid = lo + (hi - lo) / 2;
		if (rec_stamp(hgr_at(r, mid)) <= t_ns)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo ? (int64_t)lo - 1 : -1;
}

void hgr_info(const struct hgr_reader *r, uint64_t i, struct hg_info *info)
{
	const struct hgr_record *rec = hgr_at(r, i);
	memset(info, 0, sizeof *info);
	if (!rec) {
		info->result = HG_GONE;
		return;
	}
	info->result = HG_MSG;
	info->seq = rec->seq;
	info->stamp_ns = rec->stamp_ns;
	info->publish_ns = rec->publish_ns;
	info->flags = rec->flags & 0xffffu;
	info->n_inputs = rec->n_inputs > HG_MAX_REFS ? HG_MAX_REFS : rec->n_inputs;
	memcpy(info->inputs, rec->inputs, sizeof info->inputs);
	info->gap = (rec->flags & HGR_REC_GAP) ? rec->gap : 0;
	info->skipped = (rec->flags & HGR_REC_GAP) ? 0 : rec->gap;
	info->epoch = r->h->epoch;
}
