// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared by the recorder's files (modules/recorder/README.md): the
 * configuration (recorder_conf.c), the writer thread and its bounded queue
 * (writer.c), the black box (blackbox.c), the module itself (recorder.c). */
#ifndef RECORDER_H
#define RECORDER_H

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "hg_record.h"
#include "hg_topic.h"

#define REC_MAX_TOPICS 512          /* lib/hg's registry holds as many subscriptions */
#define REC_MAX_RULES 64
#define REC_PATH 600

/* ---- Configuration (recorder.conf) ---------------------------------------- */

enum rec_autostart { REC_MANUAL = 0, REC_ARMED = 1, REC_BOOT = 2 };
enum rec_class { REC_BB_OFF = 0, REC_BB_LOW = 1, REC_BB_NORMAL = 2, REC_BB_HIGH = 3 };

struct rec_rule {
	char glob[HG_NAME_MAX + 1];
	int have_policy;
	enum hgr_policy policy;
	uint64_t every_n;         /* every:N */
	uint32_t rate_mhz;        /* rate:<Hz> */
	char text[32];            /* the Policy= value as written */
	int have_class;
	enum rec_class class;
	int line;
};

struct rec_conf {
	enum rec_autostart autostart;
	char dir[REC_PATH];
	uint64_t min_free;
	int64_t blackbox_ns;
	uint64_t blackbox_mem;
	uint64_t write_buffer;    /* the writer queue's size (not in docs/06: README) */
	int n_rules;
	struct rec_rule rules[REC_MAX_RULES];
};

/* What a topic gets: its policy and its class, from the first rule that
 * matches, else the defaults of docs/06 "What it records". */
struct rec_policy {
	enum hgr_policy policy;
	uint64_t every_n;         /* 1 for full and meta */
	enum rec_class class;
	char text[32];
};

/* Defaults, then the file when path is given. 0, or -1 with the reason
 * (the file and line) in err. */
int rec_conf_load(const char *path, struct rec_conf *c, char *err, size_t errlen);
void rec_conf_resolve(const struct rec_conf *c, const char *topic, int is_frame, uint32_t rate_mhz,
		      struct rec_policy *out);

/* ---- The writer thread ---------------------------------------------------- */

enum wq_kind {
	WQ_WRAP = 0,              /* the rest of the buffer is unused; read on from 0 */
	WQ_OPEN,                  /* topic; data: struct hgr_header */
	WQ_RECORD,                /* topic; data: struct hgr_record [+ payload] */
	WQ_SESSION_BEGIN,         /* data: the session directory (NUL-terminated) */
	WQ_TEXT_FILE,             /* data: "<relative path>\0<text>" into the session directory */
	WQ_SESSION_END,           /* closes every file */
	WQ_DUMP,                  /* data: a struct bb_dump * (the writer frees it) */
	WQ_STOP,
};

struct wq_msg {
	uint32_t kind;
	uint32_t topic;
	uint32_t len;             /* bytes of data after this header */
	uint32_t pad;
};

struct writer {
	pthread_t thread;
	int started;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	unsigned char *buf;
	size_t cap, head, tail, used;
	int stop;
	/* The writer thread's own. */
	struct hgr_writer files[REC_MAX_TOPICS];
	int open[REC_MAX_TOPICS];
	char session_dir[REC_PATH];
	/* Read by the module's thread. */
	_Atomic uint64_t bytes_written;
	_Atomic uint64_t records;
	_Atomic uint64_t errors;
	_Atomic uint64_t last_error;   /* errno of the last failed write */
	_Atomic int busy;             /* 1 while a dump is being written */
};

int writer_start(struct writer *w, size_t cap);
/* Queues one message of two parts (b may be NULL). Never blocks: -1 with
 * EAGAIN when the buffer is full, and the caller counts the loss. */
int writer_push(struct writer *w, enum wq_kind kind, uint32_t topic, const void *a, size_t alen, const void *b,
		size_t blen);
void writer_stop(struct writer *w);
/* Used by the writer and by the module: a directory with mode 2770, group
 * hangar-rec when that group exists. */
int rec_mkdir(const char *path);

/* ---- The black box ---------------------------------------------------------- */

struct bb_ring {
	unsigned char *buf;
	uint32_t rec_bytes;       /* 128 + payload, or 128 under meta */
	uint32_t cap;             /* entries */
	uint32_t head;            /* the oldest */
	uint32_t count;
	uint64_t budget;          /* entries asked for */
	uint64_t pushed;
	enum rec_class class;
	int wanted;               /* has a budget (the topic is attached and not off) */
};

/* Grants the budgets class by class within mem (docs/06 "The black box"),
 * resizing the rings that change; the newest entries are kept. Allocates:
 * called when a topic appears, never per message. 0, or -1 (ENOMEM). */
int bb_grant(struct bb_ring *rings, int n, uint64_t mem);
void bb_push(struct bb_ring *r, const struct hgr_record *rec, const void *payload);
/* How far back the ring reaches: newest publish_ns - oldest publish_ns. */
uint64_t bb_reach_ns(const struct bb_ring *r);
const struct hgr_record *bb_at(const struct bb_ring *r, uint32_t i);

/* A dump handed to the writer: the rings' buffers move here and the live
 * rings get fresh ones. */
struct bb_dump_topic {
	struct hgr_header hdr;
	struct bb_ring ring;
};
struct bb_dump {
	char dir[REC_PATH];
	char ini[4096];
	int n;
	struct bb_dump_topic topics[];
};
/* Builds a dump from rings[n] with headers hdrs[n] (an entry with no
 * buffer is skipped), taking the buffers; the rings are left empty with
 * new buffers of the same size. NULL on ENOMEM, with the rings intact. */
struct bb_dump *bb_take(struct bb_ring *rings, const struct hgr_header *hdrs, int n, const char *dir,
			const char *ini);
void bb_dump_free(struct bb_dump *d);

#endif
