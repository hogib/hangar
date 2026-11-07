// SPDX-License-Identifier: GPL-3.0-or-later
/* The recorder's writer thread (docs/06 "Draining"): the module's thread
 * hands it records through a bounded byte ring and never waits for the
 * disk; when the ring is full the module drops the record and the next one
 * of that topic carries the gap. All file work happens here: the session
 * directory's layout, the .hgr files, session.ini, the black-box dumps. */
#include "recorder.h"

#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wlog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("recorder: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

int rec_mkdir(const char *path)
{
	if (mkdir(path, 02770) < 0 && errno != EEXIST)
		return -1;
	struct group *g = getgrnam("hangar-rec");
	if (g)
		(void)chown(path, (uid_t)-1, g->gr_gid);
	return chmod(path, 02770);
}

static size_t round8(size_t n)
{
	return (n + 7) & ~(size_t)7;
}

int writer_push(struct writer *w, enum wq_kind kind, uint32_t topic, const void *a, size_t alen, const void *b,
		size_t blen)
{
	size_t need = sizeof(struct wq_msg) + round8(alen + blen);
	if (need > w->cap) {
		errno = EMSGSIZE;
		return -1;
	}
	pthread_mutex_lock(&w->mu);
	size_t at;
	if (w->tail + need <= w->cap) {
		if (w->used + need > w->cap)
			goto full;
		at = w->tail;
		w->tail += need;
		w->used += need;
	} else {
		size_t waste = w->cap - w->tail;
		if (w->used + waste + need > w->cap)
			goto full;
		if (waste >= sizeof(struct wq_msg)) {
			struct wq_msg wrap = { WQ_WRAP, 0, (uint32_t)(waste - sizeof wrap), 0 };
			memcpy(w->buf + w->tail, &wrap, sizeof wrap);
		}
		w->used += waste;
		at = 0;
		w->tail = need;
		w->used += need;
	}
	struct wq_msg m = { (uint32_t)kind, topic, (uint32_t)(alen + blen), 0 };
	memcpy(w->buf + at, &m, sizeof m);
	if (alen)
		memcpy(w->buf + at + sizeof m, a, alen);
	if (blen)
		memcpy(w->buf + at + sizeof m + alen, b, blen);
	if (w->tail == w->cap)
		w->tail = 0;
	pthread_cond_signal(&w->cv);
	pthread_mutex_unlock(&w->mu);
	return 0;
full:
	pthread_mutex_unlock(&w->mu);
	errno = EAGAIN;
	return -1;
}

/* ---- The thread's file work ------------------------------------------------- */

static int write_text(const char *path, const char *text, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
	if (fd < 0)
		return -1;
	int rc = 0;
	while (len) {
		ssize_t n = write(fd, text, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -1;
			break;
		}
		text += n;
		len -= (size_t)n;
	}
	close(fd);
	return rc;
}

static void copy_tree_files(const char *from, const char *to, const char *suffix)
{
	DIR *d = opendir(from);
	if (!d)
		return;
	struct dirent *e;
	static char buf[65536];
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name), sl = strlen(suffix);
		if (e->d_name[0] == '.' || n <= sl || strcmp(e->d_name + n - sl, suffix) != 0)
			continue;
		char src[REC_PATH + 300], dst[REC_PATH + 300];
		snprintf(src, sizeof src, "%s/%s", from, e->d_name);
		snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
		int fd = open(src, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		ssize_t got = read(fd, buf, sizeof buf);
		close(fd);
		if (got > 0)
			(void)write_text(dst, buf, (size_t)got);
	}
	closedir(d);
}

static void session_begin(struct writer *w, const char *dir)
{
	snprintf(w->session_dir, sizeof w->session_dir, "%s", dir);
	if (rec_mkdir(dir) < 0)
		wlog("cannot make %s: %s", dir, strerror(errno));
	const char *sub[] = { "manifests", "msg", "video", "weights" };
	char path[REC_PATH + 64];
	for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, sub[i]);
		(void)rec_mkdir(path);
	}
	const char *root = getenv("HANGAR_ROOT");
	root = root && *root ? root : "";
	char from[REC_PATH + 64];
	snprintf(from, sizeof from, "%s/etc/hangar/modules", root);
	snprintf(path, sizeof path, "%s/manifests", dir);
	copy_tree_files(from, path, ".module");
	snprintf(from, sizeof from, "%s/usr/share/hangar/msg", root);
	snprintf(path, sizeof path, "%s/msg", dir);
	copy_tree_files(from, path, ".msg");
}

static void hgr_path(char *out, size_t len, const char *dir, const struct hgr_header *h)
{
	if (h->epoch)
		snprintf(out, len, "%s/%.47s+%u.hgr", dir, h->name, h->epoch);
	else
		snprintf(out, len, "%s/%.47s.hgr", dir, h->name);
}

static void do_open(struct writer *w, uint32_t topic, const struct hgr_header *h)
{
	if (topic >= REC_MAX_TOPICS)
		return;
	if (w->open[topic]) {
		hgr_close(&w->files[topic]);
		w->open[topic] = 0;
	}
	char path[REC_PATH + 80];
	hgr_path(path, sizeof path, w->session_dir, h);
	if (hgr_create(&w->files[topic], path, h) < 0) {
		atomic_fetch_add(&w->errors, 1);
		atomic_store(&w->last_error, (uint64_t)errno);
		wlog("cannot create %s: %s", path, strerror(errno));
		return;
	}
	w->open[topic] = 1;
	atomic_fetch_add(&w->bytes_written, HGR_HEADER_BYTES);
}

static void do_record(struct writer *w, uint32_t topic, const unsigned char *data, uint32_t len)
{
	if (topic >= REC_MAX_TOPICS || !w->open[topic] || len < HGR_RECORD_HEADER) {
		atomic_fetch_add(&w->errors, 1);
		return;
	}
	struct hgr_writer *f = &w->files[topic];
	if (!f->meta && len < HGR_RECORD_HEADER + f->payload_bytes) {
		atomic_fetch_add(&w->errors, 1);
		return;
	}
	if (hgr_append(f, (const struct hgr_record *)(const void *)data, data + HGR_RECORD_HEADER) < 0) {
		atomic_fetch_add(&w->errors, 1);
		atomic_store(&w->last_error, (uint64_t)errno);
		return;
	}
	atomic_fetch_add(&w->records, 1);
	atomic_fetch_add(&w->bytes_written, f->record_bytes);
}

static void close_all(struct writer *w)
{
	for (int i = 0; i < REC_MAX_TOPICS; i++)
		if (w->open[i]) {
			hgr_close(&w->files[i]);
			w->open[i] = 0;
		}
}

static void do_dump(struct writer *w, struct bb_dump *d)
{
	atomic_store(&w->busy, 1);
	if (rec_mkdir(d->dir) < 0) {
		wlog("cannot make %s: %s", d->dir, strerror(errno));
	} else {
		for (int i = 0; i < d->n; i++) {
			struct bb_dump_topic *t = &d->topics[i];
			if (!t->ring.buf || !t->ring.count)
				continue;
			char path[REC_PATH + 80];
			hgr_path(path, sizeof path, d->dir, &t->hdr);
			struct hgr_writer f;
			if (hgr_create(&f, path, &t->hdr) < 0) {
				wlog("cannot create %s: %s", path, strerror(errno));
				atomic_fetch_add(&w->errors, 1);
				continue;
			}
			for (uint32_t k = 0; k < t->ring.count; k++) {
				const struct hgr_record *r = bb_at(&t->ring, k);
				if (hgr_append(&f, r, (const unsigned char *)r + HGR_RECORD_HEADER) < 0) {
					atomic_fetch_add(&w->errors, 1);
					break;
				}
			}
			atomic_fetch_add(&w->bytes_written, f.bytes);
			hgr_close(&f);
		}
		char path[REC_PATH + 80];
		snprintf(path, sizeof path, "%s/session.ini", d->dir);
		(void)write_text(path, d->ini, strlen(d->ini));
	}
	bb_dump_free(d);
	atomic_store(&w->busy, 0);
}

static void *writer_main(void *arg)
{
	struct writer *w = arg;
	for (;;) {
		pthread_mutex_lock(&w->mu);
		while (w->used == 0 && !w->stop)
			pthread_cond_wait(&w->cv, &w->mu);
		if (w->used == 0 && w->stop) {
			pthread_mutex_unlock(&w->mu);
			break;
		}
		/* Too little room for a header before the end: the producer went
		 * on from 0 without a marker. */
		if (w->cap - w->head < sizeof(struct wq_msg)) {
			w->used -= w->cap - w->head;
			w->head = 0;
			pthread_mutex_unlock(&w->mu);
			continue;
		}
		struct wq_msg m;
		memcpy(&m, w->buf + w->head, sizeof m);
		size_t size = sizeof m + round8(m.len);
		if (m.kind == WQ_WRAP) {
			w->used -= size;
			w->head = 0;
			pthread_mutex_unlock(&w->mu);
			continue;
		}
		const unsigned char *data = w->buf + w->head + sizeof m;
		pthread_mutex_unlock(&w->mu);
		/* The producer never writes over an unconsumed message, so the
		 * data is read in place without the lock. */
		switch (m.kind) {
		case WQ_OPEN:
			if (m.len >= sizeof(struct hgr_header))
				do_open(w, m.topic, (const struct hgr_header *)(const void *)data);
			break;
		case WQ_RECORD:
			do_record(w, m.topic, data, m.len);
			break;
		case WQ_SESSION_BEGIN: {
			char dir[REC_PATH];
			snprintf(dir, sizeof dir, "%.*s", (int)(m.len < sizeof dir ? m.len : sizeof dir - 1), (const char *)data);
			session_begin(w, dir);
			break;
		}
		case WQ_TEXT_FILE: {
			size_t nl = strnlen((const char *)data, m.len);
			if (nl < m.len && w->session_dir[0]) {
				char path[REC_PATH + 300];
				snprintf(path, sizeof path, "%s/%.*s", w->session_dir, (int)nl, (const char *)data);
				if (write_text(path, (const char *)data + nl + 1, m.len - nl - 1) < 0)
					wlog("cannot write %s: %s", path, strerror(errno));
			}
			break;
		}
		case WQ_SESSION_END:
			close_all(w);
			w->session_dir[0] = 0;
			break;
		case WQ_DUMP: {
			struct bb_dump *d = NULL;
			if (m.len == sizeof d) {
				memcpy(&d, data, sizeof d);
				do_dump(w, d);
			}
			break;
		}
		default:
			break;
		}
		pthread_mutex_lock(&w->mu);
		w->head += size;
		if (w->head >= w->cap)
			w->head = 0;
		w->used -= size;
		pthread_mutex_unlock(&w->mu);
	}
	close_all(w);
	return NULL;
}

int writer_start(struct writer *w, size_t cap)
{
	memset(w, 0, sizeof *w);
	w->cap = cap;
	w->buf = malloc(cap);
	if (!w->buf)
		return -1;
	for (int i = 0; i < REC_MAX_TOPICS; i++)
		w->files[i].fd = -1;
	pthread_mutex_init(&w->mu, NULL);
	pthread_cond_init(&w->cv, NULL);
	if (pthread_create(&w->thread, NULL, writer_main, w) != 0) {
		free(w->buf);
		w->buf = NULL;
		return -1;
	}
	w->started = 1;
	return 0;
}

void writer_stop(struct writer *w)
{
	if (!w->started)
		return;
	pthread_mutex_lock(&w->mu);
	w->stop = 1;
	pthread_cond_signal(&w->cv);
	pthread_mutex_unlock(&w->mu);
	pthread_join(w->thread, NULL);
	w->started = 0;
	free(w->buf);
	w->buf = NULL;
}
