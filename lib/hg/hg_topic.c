// SPDX-License-Identifier: LGPL-3.0-or-later
/* hangar topics: shared-memory ring files, seqlocked slots, futex wake-ups,
 * provenance. The layout and the algorithms are those of docs/01-topics.md;
 * where this file departs from the text, lib/hg/NOTES.md says so and why. */
#define _GNU_SOURCE
#include "hg_topic.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "hg_sys.h"

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "64-bit atomics must be lock-free");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "32-bit atomics must be lock-free");

/* ---- File layout ------------------------------------------------------- */

#define HEADER_BYTES 4096u
#define SLOT_HEADER_BYTES 128u
#define WAKE_BYTES 64u
#define MAGIC "HGTOPIC1"
#define HG_PATH_MAX 512

/* Every field written while readers may be reading is atomic: the 64-bit
 * ones as _Atomic uint64_t so that ARMv7 never tears them into halves. */
struct hg_header {
	char magic[8];                    /*   0 */
	uint32_t version;                 /*   8 */
	uint32_t header_bytes;            /*  12 */
	uint64_t generation;              /*  16 */
	uint32_t slot_count;              /*  24 */
	uint32_t slot_header_bytes;       /*  28 */
	uint32_t payload_bytes;           /*  32 */
	_Atomic uint32_t flags;           /*  36 */
	uint64_t slot_stride;             /*  40 */
	uint64_t slots_offset;            /*  48 */
	uint32_t topic_id;                /*  56 */
	uint32_t rate_mhz;                /*  60 */
	uint64_t type_hash;               /*  64 */
	char name[48];                    /*  72 */
	char writer[32];                  /* 120 */
	int32_t writer_pid;               /* 152 */
	uint32_t schema_bytes;            /* 156 */
	uint64_t created_real_ns;         /* 160 */
	uint64_t first_seq;               /* 168 */
	uint64_t stale_after_ns;          /* 176 */
	uint64_t reserved0;               /* 184 */
	/* The publish line: its own cache line, so metadata readers do not
	 * share one with the writer's stores. */
	_Atomic uint32_t futex_word;      /* 192 */
	uint32_t reserved1;               /* 196 */
	_Atomic uint64_t write_seq;       /* 200 */
	_Atomic uint64_t last_publish_ns; /* 208 */
	unsigned char reserved2[40];      /* 216 */
	unsigned char origin_boot[16];    /* 256 */
	unsigned char reserved3[240];     /* 272 */
	char schema[HG_SCHEMA_MAX];       /* 512 */
};

struct hg_slot_input {
	_Atomic uint32_t topic_id;
	_Atomic uint32_t flags;
	_Atomic uint64_t seq;
};

/* Every slot field, the payload included (as 8-byte words), is atomic and
 * accessed relaxed: a reader overtaken by the writer reads a mix of old and
 * new values, which the seqlock check discards, but never races. */
struct hg_slot {
	_Atomic uint64_t seq;             /*   0 */
	_Atomic uint64_t stamp_ns;        /*   8 */
	_Atomic uint64_t publish_ns;      /*  16 */
	_Atomic uint32_t n_inputs;        /*  24 */
	_Atomic uint32_t flags;           /*  28 */
	unsigned char reserved[32];       /*  32 */
	struct hg_slot_input inputs[4];   /*  64 */
	_Atomic uint64_t payload[];       /* 128, payload_bytes rounded up to 8 */
};

/* The wake file and the directory word share one shape: a u32 at 0. */
struct hg_wake {
	_Atomic uint32_t wait_flag;
	unsigned char rest[WAKE_BYTES - 4];
};

#define CHECK_OFFSET(type, field, off) \
	_Static_assert(offsetof(struct type, field) == (off), #field " offset")
CHECK_OFFSET(hg_header, magic, 0);
CHECK_OFFSET(hg_header, version, 8);
CHECK_OFFSET(hg_header, header_bytes, 12);
CHECK_OFFSET(hg_header, generation, 16);
CHECK_OFFSET(hg_header, slot_count, 24);
CHECK_OFFSET(hg_header, slot_header_bytes, 28);
CHECK_OFFSET(hg_header, payload_bytes, 32);
CHECK_OFFSET(hg_header, flags, 36);
CHECK_OFFSET(hg_header, slot_stride, 40);
CHECK_OFFSET(hg_header, slots_offset, 48);
CHECK_OFFSET(hg_header, topic_id, 56);
CHECK_OFFSET(hg_header, rate_mhz, 60);
CHECK_OFFSET(hg_header, type_hash, 64);
CHECK_OFFSET(hg_header, name, 72);
CHECK_OFFSET(hg_header, writer, 120);
CHECK_OFFSET(hg_header, writer_pid, 152);
CHECK_OFFSET(hg_header, schema_bytes, 156);
CHECK_OFFSET(hg_header, created_real_ns, 160);
CHECK_OFFSET(hg_header, first_seq, 168);
CHECK_OFFSET(hg_header, stale_after_ns, 176);
CHECK_OFFSET(hg_header, futex_word, 192);
CHECK_OFFSET(hg_header, reserved1, 196);
CHECK_OFFSET(hg_header, write_seq, 200);
CHECK_OFFSET(hg_header, last_publish_ns, 208);
CHECK_OFFSET(hg_header, origin_boot, 256);
CHECK_OFFSET(hg_header, schema, 512);
CHECK_OFFSET(hg_slot, seq, 0);
CHECK_OFFSET(hg_slot, stamp_ns, 8);
CHECK_OFFSET(hg_slot, publish_ns, 16);
CHECK_OFFSET(hg_slot, n_inputs, 24);
CHECK_OFFSET(hg_slot, flags, 28);
CHECK_OFFSET(hg_slot, reserved, 32);
CHECK_OFFSET(hg_slot, inputs, 64);
CHECK_OFFSET(hg_slot, payload, 128);
CHECK_OFFSET(hg_slot_input, topic_id, 0);
CHECK_OFFSET(hg_slot_input, flags, 4);
CHECK_OFFSET(hg_slot_input, seq, 8);
_Static_assert(sizeof(struct hg_header) == HEADER_BYTES, "header size");
_Static_assert(sizeof(struct hg_slot) == SLOT_HEADER_BYTES, "slot header size");
_Static_assert(sizeof(struct hg_slot_input) == 16, "slot input size");
_Static_assert(sizeof(struct hg_wake) == WAKE_BYTES, "wake file size");
_Static_assert(sizeof(struct hg_input) == sizeof(struct hg_slot_input), "input record");

/* ---- Helpers ----------------------------------------------------------- */

uint64_t hg_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t real_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint32_t hg_topic_id(const char *name)
{
	return hg_fnv1a32(name);
}

int hg_futex_wake_all(void *addr)
{
	return (int)syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static int futex_wake_private(void *addr)
{
	return (int)syscall(SYS_futex, addr, FUTEX_WAKE | HG_FUTEX_PRIVATE_FLAG, INT_MAX, NULL, NULL, 0);
}

static uint64_t stride_for(uint32_t payload_bytes)
{
	uint64_t p = ((uint64_t)payload_bytes + 7) & ~(uint64_t)7;
	return (SLOT_HEADER_BYTES + p + 63) & ~(uint64_t)63;
}

static int valid_segments(const char *name, size_t len, int allow_glob)
{
	if (len == 0 || len > HG_NAME_MAX || name[0] == '.' || name[len - 1] == '.')
		return 0;
	for (size_t i = 0; i < len; i++) {
		char c = name[i];
		if (c == '.') {
			if (name[i + 1] == '.')
				return 0;
		} else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
			     (allow_glob && (c == '*' || c == '?')))) {
			return 0;
		}
	}
	return 1;
}

int hg_valid_name(const char *name)
{
	return valid_segments(name, strlen(name), 0);
}

static int is_glob(const char *name)
{
	return strchr(name, '*') || strchr(name, '?');
}

int hg_topic_path(char *out, size_t len, const char *prefix, const char *name, const char *suffix)
{
	const char *dir = getenv("HANGAR_TOPIC_DIR");
	if (!dir || !*dir) {
		errno = EINVAL;
		return -1;
	}
	int n = snprintf(out, len, "%s/%s%s%s", dir, prefix, name, suffix);
	if (n < 0 || (size_t)n >= len) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/* Copies len bytes out of shared memory that a writer may be changing, as
 * relaxed atomic words; a tail shorter than 8 is read as a whole word (the
 * slot has room) of which len % 8 bytes are kept. */
static void load_words(void *dst, const _Atomic uint64_t *src, size_t len)
{
	unsigned char *d = dst;
	size_t i = 0;
	for (; len - 8 * i >= 8; i++) {
		uint64_t w = atomic_load_explicit(&src[i], memory_order_relaxed);
		memcpy(d + 8 * i, &w, 8);
	}
	if (8 * i < len) {
		uint64_t w = atomic_load_explicit(&src[i], memory_order_relaxed);
		memcpy(d + 8 * i, &w, len - 8 * i);
	}
}

/* Copies len bytes into shared memory as relaxed atomic words; the last word
 * is padded with zeros (the slot has room: payload_bytes rounds up to 8). */
static void store_words(_Atomic uint64_t *dst, const void *src, size_t len)
{
	const unsigned char *b = src;
	size_t i = 0;
	for (; len - 8 * i >= 8; i++) {
		uint64_t w;
		memcpy(&w, b + 8 * i, 8);
		atomic_store_explicit(&dst[i], w, memory_order_relaxed);
	}
	if (8 * i < len) {
		uint64_t w = 0;
		memcpy(&w, b + 8 * i, len - 8 * i);
		atomic_store_explicit(&dst[i], w, memory_order_relaxed);
	}
}

/* The fields of a header a reader relies on, validated. */
struct layout {
	uint64_t stride;
	uint64_t first_seq;
	uint64_t type_hash;
	uint64_t stale_after_ns;
	uint32_t slot_count;
	uint32_t payload_bytes;
	uint32_t rate_mhz;
	uint32_t schema_bytes;
	unsigned char origin_boot[16];
};

/* Fills *out from one copy of the header, taken before it is validated, so
 * that a writer changing the shared header cannot get unchecked sizes past
 * the check. 0 on success, -1 with errno EPROTO if it is not a valid topic. */
static int read_layout(const struct hg_header *h, uint64_t file_size, struct layout *out)
{
	struct hg_header c; /* only the fields before futex_word and origin_boot are copied */
	if (file_size < HEADER_BYTES)
		goto bad;
	load_words(&c, (const _Atomic uint64_t *)(const void *)h, offsetof(struct hg_header, futex_word));
	load_words(c.origin_boot,
		   (const _Atomic uint64_t *)(const void *)((const unsigned char *)h +
							    offsetof(struct hg_header, origin_boot)),
		   16);
	if (memcmp(c.magic, MAGIC, 8) != 0 || c.version != 1 || c.header_bytes != HEADER_BYTES ||
	    c.slot_header_bytes != SLOT_HEADER_BYTES || c.slots_offset != HEADER_BYTES)
		goto bad;
	uint32_t k = c.slot_count;
	if (k < 2 || k > 65536 || (k & (k - 1)) != 0 || c.payload_bytes == 0 ||
	    c.slot_stride != stride_for(c.payload_bytes) ||
	    file_size < HEADER_BYTES + (uint64_t)k * c.slot_stride || c.schema_bytes > HG_SCHEMA_MAX)
		goto bad;
	*out = (struct layout){ .stride = c.slot_stride, .first_seq = c.first_seq,
				.type_hash = c.type_hash, .stale_after_ns = c.stale_after_ns,
				.slot_count = k, .payload_bytes = c.payload_bytes,
				.rate_mhz = c.rate_mhz, .schema_bytes = c.schema_bytes };
	memcpy(out->origin_boot, c.origin_boot, 16);
	return 0;
bad:
	errno = EPROTO;
	return -1;
}

/* Maps a 64-byte word file (a wake file or .dir). Created with O_CREAT when
 * asked, never by a temporary and a rename and never truncated, so that it
 * keeps its inode for the whole boot: sleepers wait on it by inode. A file
 * this process may not write (the manager's .dir) is mapped read-only when
 * rw is 0: a sleeper only reads the word. */
static struct hg_wake *map_wake_mode(const char *path, int create, int rw)
{
	int fd = open(path, (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC | (create ? O_CREAT : 0), 0660);
	if (fd < 0)
		return NULL;
	struct stat st;
	struct hg_wake *w = NULL;
	if (fstat(fd, &st) < 0)
		goto out;
	if (st.st_size < (off_t)WAKE_BYTES) {
		/* Only a file just created is shorter: extending it is part of its
		 * creation, not a truncation. */
		if (!create) {
			errno = EPROTO;
			goto out;
		}
		if (ftruncate(fd, WAKE_BYTES) < 0)
			goto out;
	}
	void *m = mmap(NULL, WAKE_BYTES, PROT_READ | (rw ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
	if (m != MAP_FAILED)
		w = m;
out:;
	int e = errno;
	close(fd);
	errno = e;
	return w;
}

static struct hg_wake *map_wake(const char *path, int create)
{
	return map_wake_mode(path, create, 1);
}

/* The directory word, mapped once per process. In standalone mode the
 * library stands in for the manager and creates .dir if it is missing; under
 * a manager, which owns it, a module maps the existing file read-only. */
static struct hg_wake *dir_map;
static int dir_rw;

static struct hg_wake *dir_word(void)
{
	if (dir_map)
		return dir_map;
	char path[HG_PATH_MAX];
	if (hg_topic_path(path, sizeof path, ".", "dir", "") < 0)
		return NULL;
	dir_map = map_wake_mode(path, 1, 1);
	dir_rw = dir_map != NULL;
	if (!dir_map && (errno == EACCES || errno == EPERM || errno == EROFS))
		dir_map = map_wake_mode(path, 0, 0);
	return dir_map;
}

static void bump_dir_word(void)
{
	struct hg_wake *d = dir_word();
	if (!d || !dir_rw)
		return;
	atomic_fetch_add_explicit(&d->wait_flag, 1, memory_order_seq_cst);
	hg_futex_wake_all(&d->wait_flag);
}

static uint64_t random_u64(void)
{
	uint64_t v;
	if (getrandom(&v, sizeof v, 0) == (ssize_t)sizeof v)
		return v;
	return hg_now_ns() ^ ((uint64_t)getpid() << 32);
}

/* ---- Module state ------------------------------------------------------ */

static struct hg_wiring wiring_store;
static const struct hg_wiring *wiring; /* NULL when unwired */
static int inited;
static char init_err[160];
static char module_name[32];

int hg_init(void)
{
	if (inited)
		return 0;
	init_err[0] = 0;
	const char *fd_s = getenv("HANGAR_WIRING");
	const char *file = getenv("HANGAR_WIRING_FILE");
	int rc = 0;
	if (fd_s && *fd_s) {
		char *e;
		long fd = strtol(fd_s, &e, 10);
		if (*e || fd < 0 || fd > INT_MAX) {
			snprintf(init_err, sizeof init_err, "HANGAR_WIRING is not a descriptor");
			errno = EINVAL;
			return -1;
		}
		rc = hg_wiring_read_fd((int)fd, &wiring_store, init_err, sizeof init_err);
	} else if (file && *file) {
		rc = hg_wiring_read_file(file, &wiring_store, init_err, sizeof init_err);
	} else {
		wiring = NULL;
	}
	if (rc < 0) {
		if (!init_err[0])
			snprintf(init_err, sizeof init_err, "cannot read the wiring: %s", strerror(errno));
		return -1;
	}
	if ((fd_s && *fd_s) || (file && *file))
		wiring = &wiring_store;
	const char *env_name = getenv("HANGAR_MODULE");
	snprintf(module_name, sizeof module_name, "%s",
		 wiring ? wiring->module : env_name && *env_name ? env_name : "standalone");
	inited = 1;
	return 0;
}

/* The opens call this so a module that skipped hg_init() is still wired. */
static int ensure_init(void)
{
	return inited ? 0 : hg_init();
}

const char *hg_init_error(void)
{
	return init_err;
}

const struct hg_wiring *hg_wiring(void)
{
	return wiring;
}

const char *hg_module_name(void)
{
	ensure_init();
	return module_name;
}

/* The module page needs a manager to hand it over: stage 2. */
void hg_ready(void)
{
}

void hg_alive(void)
{
}

void hg_status(const char *line)
{
	(void)line;
}

/* ---- Registry ---------------------------------------------------------- */

/* The open subscriptions and outputs of this process, by name, so that a
 * publisher's provenance links find the subscriptions they are computed
 * from. One module per process, one thread opens topics: no lock. */
#define REG_MAX 512
static hg_sub *reg_subs[REG_MAX];
static hg_pub *reg_pubs[REG_MAX];
static int reg_nsubs, reg_npubs;

/* ---- Provenance links -------------------------------------------------- */

struct link {
	char name[HG_WIRING_NAME];
	uint32_t topic_id;
	int remote;             /* "<role>:<topic>": recorded as given, never checked */
	int glob;               /* a Sources pattern such as *.req */
	hg_sub *sub;            /* resolved lazily */
	hg_pub *pub;            /* or another of the module's own outputs */
	uint64_t last_ref_seq;  /* what the previous publish recorded */
	int ever;               /* recorded at least once */
};

struct hg_pub {
	struct hg_header *h;
	size_t map_len;
	unsigned char *slots;
	struct hg_wake *wake;
	uint64_t seq;           /* messages published (== write_seq) */
	uint64_t stride;
	uint32_t mask;
	uint32_t payload_bytes;
	uint32_t topic_id;
	uint32_t last_flags;    /* slot flags of the last message: transitive stale through own outputs */
	int mirror;             /* opened with hg_pub_open_mirror */
	char name[48];
	int n_inputs;
	struct link inputs[HG_MAX_REFS];
	int n_sources;
	struct link sources[HG_WIRING_MAX_SOURCES];
};

/* ---- Reader state ------------------------------------------------------ */

struct hg_sub {
	char name[48];
	char path[HG_PATH_MAX];
	char wake_path[HG_PATH_MAX];
	uint32_t topic_id;
	struct hg_header *h;        /* NULL while absent */
	size_t map_len;
	unsigned char *slots;
	struct hg_wake *wake;
	dev_t dev;
	ino_t ino;
	uint64_t last;              /* last seq returned by hg_next/hg_latest */
	uint64_t first;             /* first_seq of the mapped file */
	uint64_t stride;
	uint64_t reopens;
	uint64_t reopen_since;      /* replaced, but no new file at the path since then; 0 if not */
	uint32_t reopen_word;       /* the old file's futex_word as of reopen_since */
	uint32_t mask;
	uint32_t payload_bytes;
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
	uint32_t seen_flags;        /* header flags already reported */
	uint32_t last_flags;        /* slot flags of the last message returned */
	uint32_t epoch;
	unsigned char origin[16];
	int started;                /* attached at least once */
	enum hg_start start;
	uint64_t expect_hash;
	int has_expect;
	int type_bad;               /* the mapped file is of another type */
	int type_bad_reported;
	int attach_err;             /* errno of a failed attach other than ENOENT */
	int attach_err_reported;
	int appeared;               /* attached since the last report */
	int appeared_reported;
	int use_set;
	uint64_t use_seq;
	uint32_t use_flags;
	int in_fd_set;              /* in the hg_wait_fd helper's set */
	/* Glob subscriptions. */
	int glob;
	hg_sub **kids;
	int n_kids, cap_kids;
	uint32_t scan_word;         /* the directory word at the last scan */
	int scanned;
};

/* ---- hg_wait_fd state (declared early: attach and reopen tell it) ------ */

static struct {
	pthread_t thread;
	pthread_mutex_t lock;
	int running;
	int efd;
	hg_sub *subs[HG_MAX_WAIT_FD];
	int n;
	_Atomic uint32_t helper_word;  /* always in the helper's set; bumped on every set change */
	_Atomic uint32_t gen;          /* the set's generation */
	_Atomic uint32_t ack;          /* the generation the helper last snapshotted */
	_Atomic int stop;
} FD = { .lock = PTHREAD_MUTEX_INITIALIZER, .efd = -1 };

/* The module's thread changed s's mapping: the helper must take a new
 * snapshot before the old mapping goes. Called with the new mapping in place
 * under FD.lock already released; returns once the helper has acknowledged
 * the generation, so the caller may unmap. */
static void fd_set_changed(void)
{
	if (!FD.running)
		return;
	uint32_t g = atomic_fetch_add_explicit(&FD.gen, 1, memory_order_seq_cst) + 1;
	atomic_fetch_add_explicit(&FD.helper_word, 1, memory_order_seq_cst);
	futex_wake_private(&FD.helper_word);
	for (;;) {
		uint32_t a = atomic_load_explicit(&FD.ack, memory_order_acquire);
		if ((int32_t)(a - g) >= 0)
			return;
		syscall(SYS_futex, &FD.ack, FUTEX_WAIT | HG_FUTEX_PRIVATE_FLAG, a, NULL, NULL, 0);
	}
}

static void fd_lock(const hg_sub *s)
{
	if (s->in_fd_set)
		pthread_mutex_lock(&FD.lock);
}

static void fd_unlock(const hg_sub *s)
{
	if (s->in_fd_set)
		pthread_mutex_unlock(&FD.lock);
}

/* ---- Writer ------------------------------------------------------------ */

/* Tells readers of h that its flags changed. */
static void announce(struct hg_header *h)
{
	atomic_fetch_add_explicit(&h->futex_word, 1, memory_order_seq_cst);
	hg_futex_wake_all(&h->futex_word);
}

/* The old file, read-write, if it is a valid topic. */
static struct hg_header *map_old_rw(const char *path, struct layout *l)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	struct hg_header *h = NULL;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)HEADER_BYTES) {
		void *m = mmap(NULL, HEADER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m != MAP_FAILED) {
			/* Only the header is mapped; the size check uses the file's. */
			if (read_layout(m, (uint64_t)st.st_size, l) == 0)
				h = m;
			else
				munmap(m, HEADER_BYTES);
		}
	}
	close(fd);
	return h;
}

static int zero16(const unsigned char *b)
{
	static const unsigned char z[16];
	return memcmp(b, z, 16) == 0;
}

static void link_init(struct link *l, const char *name)
{
	memset(l, 0, sizeof *l);
	snprintf(l->name, sizeof l->name, "%s", name);
	const char *colon = strchr(name, ':');
	if (colon) {
		l->remote = 1;
		l->topic_id = hg_fnv1a32(colon + 1);
	} else if (is_glob(name)) {
		l->glob = 1;
	} else {
		l->topic_id = hg_fnv1a32(name);
	}
}

/* What the open takes from the manifest, by way of the wiring or the opts. */
struct open_params {
	uint32_t slots;
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
	const struct hg_wiring_pub *line;
};

static int resolve_open(const char *name, const struct hg_type *type, int mirror,
			const struct hg_pub_opts *opts, struct open_params *out)
{
	memset(out, 0, sizeof *out);
	if (wiring) {
		if (mirror) {
			const struct hg_wiring_mirror *m = hg_wiring_match_mirror(wiring, name);
			if (!m) {
				errno = EACCES;
				return -1;
			}
			out->slots = 16;
			out->rate_mhz = m->rate_mhz;
			out->stale_after_ns = m->stale_after_ns;
		} else {
			const struct hg_wiring_pub *p = hg_wiring_find_pub(wiring, name);
			if (!p) {
				errno = EACCES;
				return -1;
			}
			/* "frame" accepts any frame type; a line with a hash names one type. */
			if (p->hash && p->hash != type->hash) {
				errno = EPROTOTYPE;
				return -1;
			}
			out->slots = p->slots;
			out->rate_mhz = p->rate_mhz;
			out->stale_after_ns = p->stale_after_ns;
			out->line = p;
		}
	} else {
		out->slots = opts && opts->slots ? opts->slots
			     : type->name && strcmp(type->name, "frame") == 0 ? 4 : 16;
		out->rate_mhz = opts ? opts->rate_mhz : 0;
		out->stale_after_ns = opts ? opts->stale_after_ns : 0;
	}
	if (out->rate_mhz && !out->stale_after_ns)
		out->stale_after_ns = 3 * 1000000000000ull / out->rate_mhz; /* three periods */
	if (out->slots < 2 || out->slots > 65536 || (out->slots & (out->slots - 1))) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* Creates the topic file by the manager's steps (docs/01 "Opening",
 * docs/03 "Opening topics"): a temporary file, its memory reserved, the
 * header written, the wake file created, bit 2 on the old file, the rename,
 * the directory word. */
static hg_pub *pub_open(const char *name, const struct hg_type *type, int mirror,
			const unsigned char origin_boot[16], uint64_t first_seq,
			const struct hg_pub_opts *opts)
{
	char path[HG_PATH_MAX], tmp[HG_PATH_MAX], wake_path[HG_PATH_MAX], suffix[32];
	if (ensure_init() < 0)
		return NULL;
	if (!hg_valid_name(name) || !type || type->payload_bytes == 0 || !type->schema ||
	    type->schema_bytes > HG_SCHEMA_MAX || (mirror && zero16(origin_boot))) {
		errno = EINVAL;
		return NULL;
	}
	struct open_params op;
	if (resolve_open(name, type, mirror, opts, &op) < 0)
		return NULL;
	if (reg_npubs == REG_MAX) {
		errno = ENOSPC;
		return NULL;
	}
	snprintf(suffix, sizeof suffix, ".tmp.%ld", (long)getpid());
	if (hg_topic_path(path, sizeof path, "", name, "") < 0 ||
	    hg_topic_path(tmp, sizeof tmp, ".", name, suffix) < 0 ||
	    hg_topic_path(wake_path, sizeof wake_path, ".", name, ".wake") < 0)
		return NULL;

	hg_pub *p = calloc(1, sizeof *p);
	if (!p)
		return NULL;
	p->stride = stride_for(type->payload_bytes);
	p->mask = op.slots - 1;
	p->payload_bytes = type->payload_bytes;
	p->map_len = HEADER_BYTES + (size_t)op.slots * p->stride;
	p->topic_id = hg_fnv1a32(name);
	p->mirror = mirror;
	snprintf(p->name, sizeof p->name, "%s", name);
	if (op.line) {
		for (int i = 0; i < op.line->n_inputs; i++)
			link_init(&p->inputs[p->n_inputs++], op.line->inputs[i]);
		for (int i = 0; i < op.line->n_sources; i++)
			link_init(&p->sources[p->n_sources++], op.line->sources[i]);
	}

	/* An existing file continues write_seq, unless it is a mirror of
	 * another boot of the sender: then the numbers start again at the
	 * bridge's first seq, and the readers start a new epoch. */
	struct layout ol;
	struct hg_header *old = map_old_rw(path, &ol);
	uint64_t w = 0;
	if (mirror)
		w = first_seq ? first_seq - 1 : 0;
	if (old) {
		int same_boot = mirror ? memcmp(ol.origin_boot, origin_boot, 16) == 0 : 1;
		if (same_boot)
			w = atomic_load_explicit(&old->write_seq, memory_order_acquire);
	}

	int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (fd < 0 && errno == EEXIST && unlink(tmp) == 0)
		fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (fd < 0)
		goto fail;
	/* Reserved now, so a full /dev/shm fails here and not mid-run. */
	int err = posix_fallocate(fd, 0, (off_t)p->map_len);
	if (err) {
		errno = err;
		goto fail_tmp;
	}
	void *m = mmap(NULL, p->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED)
		goto fail_tmp;
	p->h = m;
	p->slots = (unsigned char *)m + HEADER_BYTES;

	struct hg_header *h = p->h;
	memcpy(h->magic, MAGIC, 8);
	h->version = 1;
	h->header_bytes = HEADER_BYTES;
	h->generation = random_u64();
	h->slot_count = op.slots;
	h->slot_header_bytes = SLOT_HEADER_BYTES;
	h->payload_bytes = type->payload_bytes;
	h->slot_stride = p->stride;
	h->slots_offset = HEADER_BYTES;
	h->topic_id = p->topic_id;
	h->rate_mhz = op.rate_mhz;
	h->type_hash = type->hash;
	memcpy(h->name, name, strlen(name));
	snprintf(h->writer, sizeof h->writer, "%s", module_name);
	h->writer_pid = (int32_t)getpid();
	h->schema_bytes = type->schema_bytes;
	h->created_real_ns = real_now_ns();
	h->first_seq = w;
	h->stale_after_ns = op.stale_after_ns;
	atomic_store_explicit(&h->write_seq, w, memory_order_relaxed);
	if (mirror)
		memcpy(h->origin_boot, origin_boot, 16);
	memcpy(h->schema, type->schema, type->schema_bytes);
	p->seq = w;

	p->wake = map_wake(wake_path, 1);
	if (!p->wake)
		goto fail_map;
	(void)dir_word(); /* created before the rename, so readers can find it */

	uint32_t old_flags = 0;
	if (old) {
		old_flags = atomic_fetch_or_explicit(&old->flags, HG_FLAG_REPLACED, memory_order_release);
		announce(old);
	}
	if (rename(tmp, path) < 0) {
		/* Readers of the old file would wait for a new one forever. */
		err = errno;
		if (old && !(old_flags & HG_FLAG_REPLACED)) {
			atomic_fetch_and_explicit(&old->flags, ~HG_FLAG_REPLACED, memory_order_release);
			announce(old);
		}
		errno = err;
		goto fail_wake;
	}
	bump_dir_word();
	if (old)
		munmap(old, HEADER_BYTES);
	close(fd);
	reg_pubs[reg_npubs++] = p;
	return p;

fail_wake:
	munmap(p->wake, WAKE_BYTES);
fail_map:
	munmap(p->h, p->map_len);
fail_tmp:
	err = errno;
	close(fd);
	unlink(tmp);
	errno = err;
fail:
	err = errno;
	if (old)
		munmap(old, HEADER_BYTES);
	free(p);
	errno = err;
	return NULL;
}

hg_pub *hg_pub_open(const char *name, const struct hg_type *type, const struct hg_pub_opts *opts)
{
	return pub_open(name, type, 0, NULL, 0, opts);
}

hg_pub *hg_pub_open_mirror(const char *name, const struct hg_type *type,
			   const unsigned char origin_boot[16], uint64_t first_seq,
			   const struct hg_pub_opts *opts)
{
	if (!origin_boot) {
		errno = EINVAL;
		return NULL;
	}
	return pub_open(name, type, 1, origin_boot, first_seq, opts);
}

uint64_t hg_pub_next_seq(const hg_pub *p)
{
	return p->seq + 1;
}

uint32_t hg_pub_topic_id(const hg_pub *p)
{
	return p->topic_id;
}

/* Publishing message n into its slot, steps 1 to 4 of docs/01 "Publishing". */
static void publish_slot(hg_pub *p, uint64_t n, uint64_t stamp_ns, uint64_t now, uint32_t flags,
			 const struct hg_input *in, uint32_t n_in, const void *payload, size_t len)
{
	struct hg_header *h = p->h;
	struct hg_slot *s = (struct hg_slot *)(p->slots + ((n - 1) & p->mask) * p->stride);

	/* 1. The slot is being written: an odd seq. */
	atomic_store_explicit(&s->seq, 2 * n - 1, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);

	/* 2. The slot header and the payload, as relaxed atomic words. */
	atomic_store_explicit(&s->stamp_ns, stamp_ns, memory_order_relaxed);
	atomic_store_explicit(&s->publish_ns, now, memory_order_relaxed);
	atomic_store_explicit(&s->n_inputs, n_in, memory_order_relaxed);
	atomic_store_explicit(&s->flags, flags, memory_order_relaxed);
	for (uint32_t i = 0; i < HG_MAX_REFS; i++) {
		const struct hg_input z = { 0, 0, 0 };
		const struct hg_input *r = i < n_in ? &in[i] : &z;
		atomic_store_explicit(&s->inputs[i].topic_id, r->topic_id, memory_order_relaxed);
		atomic_store_explicit(&s->inputs[i].flags, r->flags, memory_order_relaxed);
		atomic_store_explicit(&s->inputs[i].seq, r->seq, memory_order_relaxed);
	}
	store_words(s->payload, payload, len);

	/* 3. The message is complete; then the topic has one more. write_seq is
	 * seq_cst, not release: this store and the flag exchange below, against
	 * the reader's flag store and write_seq load in hg_wait, are a
	 * store-buffering pattern, and only with all four seq_cst does C11 rule
	 * out both sides missing each other (a lost wake-up). AArch64 emits the
	 * same STLR; x86 an XCHG instead of a MOV. */
	atomic_store_explicit(&s->seq, 2 * n, memory_order_release);
	atomic_store_explicit(&h->write_seq, n, memory_order_seq_cst);
	atomic_store_explicit(&h->last_publish_ns, now, memory_order_relaxed);
	p->seq = n;
	p->last_flags = flags;

	/* 4. Take the flag, then change the word, then wake if the flag was set.
	 * With the increment first, a reader that read the new word, set its
	 * flag and found nothing new (it already had message n) would have its
	 * fresh flag taken by this exchange while not yet asleep, and every
	 * later publish would find the flag clear: asleep until its timeout.
	 * Exchanged first, a flag set before the exchange is woken, and one set
	 * after it belongs to a reader whose re-check sees message n. */
	uint32_t waiting = atomic_exchange_explicit(&p->wake->wait_flag, 0, memory_order_seq_cst);
	atomic_fetch_add_explicit(&h->futex_word, 1, memory_order_seq_cst);
	if (waiting)
		hg_futex_wake_all(&h->futex_word);
}

/* The payload is the type's C size rounded up to 8: a module may hand the
 * struct's own size or the rounded one; store_words zero-pads the rest. */
static int len_ok(const hg_pub *p, size_t len)
{
	return len <= p->payload_bytes && len + 7 >= p->payload_bytes;
}

/* Finds the subscription or output a link names, once. */
static void link_resolve(struct link *l)
{
	if (l->sub || l->pub || l->remote || l->glob)
		return;
	for (int i = 0; i < reg_nsubs; i++)
		if (!reg_subs[i]->glob && strcmp(reg_subs[i]->name, l->name) == 0) {
			l->sub = reg_subs[i];
			return;
		}
	for (int i = 0; i < reg_npubs; i++)
		if (strcmp(reg_pubs[i]->name, l->name) == 0) {
			l->pub = reg_pubs[i];
			return;
		}
}

/* The slot flags of message seq, if it is still in s's ring. */
static int peek_slot_flags(const hg_sub *s, uint64_t seq, uint32_t *flags)
{
	if (!s->h || seq == 0)
		return 0;
	struct hg_slot *sl = (struct hg_slot *)(s->slots + ((seq - 1) & s->mask) * s->stride);
	uint64_t s1 = atomic_load_explicit(&sl->seq, memory_order_acquire);
	if (s1 != 2 * seq)
		return 0;
	uint32_t f = atomic_load_explicit(&sl->flags, memory_order_relaxed);
	atomic_thread_fence(memory_order_acquire);
	if (atomic_load_explicit(&sl->seq, memory_order_relaxed) != s1)
		return 0;
	*flags = f;
	return 1;
}

/* Fills the reference for a subscription: seq given (an override or hg_use)
 * or the most recent one, with the reused, stale and absent flags, and says
 * whether the output must carry slot flag bit 0 because of it. */
static void ref_from_sub(struct link *l, hg_sub *s, uint64_t now, int have_seq, uint64_t seq,
			 struct hg_input *out, int *stale_out)
{
	out->topic_id = l->topic_id ? l->topic_id : s->topic_id;
	out->flags = 0;
	uint32_t mflags = 0;
	int know_mflags = 0;
	if (!have_seq) {
		if (s->use_set) {
			seq = s->use_seq;
			mflags = s->use_flags;
			know_mflags = 1;
		} else {
			seq = s->last;
			mflags = s->last_flags;
			know_mflags = 1;
		}
	} else if (seq == s->last) {
		mflags = s->last_flags;
		know_mflags = 1;
	} else if (s->use_set && seq == s->use_seq) {
		mflags = s->use_flags;
		know_mflags = 1;
	}
	out->seq = seq;
	if (l->ever && seq == l->last_ref_seq)
		out->flags |= HG_REF_REUSED;
	if (!s->h) {
		out->flags |= HG_REF_ABSENT;
		*stale_out = 1;
	} else {
		uint32_t hf = atomic_load_explicit(&s->h->flags, memory_order_acquire);
		uint64_t lp = atomic_load_explicit(&s->h->last_publish_ns, memory_order_relaxed);
		int stale = (hf & (HG_FLAG_CLOSED | HG_FLAG_DIED)) != 0;
		if (s->rate_mhz && s->stale_after_ns && now > lp && now - lp > s->stale_after_ns)
			stale = 1;
		if (stale) {
			out->flags |= HG_REF_STALE;
			*stale_out = 1;
		}
		if (!know_mflags)
			know_mflags = peek_slot_flags(s, seq, &mflags);
		if (know_mflags && (mflags & HG_SLOT_STALE))
			*stale_out = 1; /* transitive */
	}
	l->last_ref_seq = seq;
	l->ever = 1;
}

/* A reference to another of the module's own outputs: its newest message. */
static void ref_from_pub(struct link *l, const hg_pub *o, int have_seq, uint64_t seq,
			 struct hg_input *out, int *stale_out)
{
	if (!have_seq)
		seq = o->seq;
	out->topic_id = o->topic_id;
	out->flags = 0;
	out->seq = seq;
	if (l->ever && seq == l->last_ref_seq)
		out->flags |= HG_REF_REUSED;
	if (seq == o->seq && (o->last_flags & HG_SLOT_STALE))
		*stale_out = 1;
	l->last_ref_seq = seq;
	l->ever = 1;
}

/* The link of p that a reference to topic_id belongs to: an Inputs entry, a
 * Sources entry, or a Sources glob matched by an open subscription of that
 * id. NULL if the reference is allowed by none. */
static struct link *link_for(hg_pub *p, uint32_t topic_id, hg_sub **sub_out)
{
	*sub_out = NULL;
	for (int i = 0; i < p->n_inputs; i++)
		if (p->inputs[i].topic_id == topic_id)
			return &p->inputs[i];
	for (int i = 0; i < p->n_sources; i++) {
		struct link *l = &p->sources[i];
		if (l->glob) {
			for (int k = 0; k < reg_nsubs; k++) {
				hg_sub *s = reg_subs[k];
				if (!s->glob && s->topic_id == topic_id && hg_glob_match(l->name, s->name)) {
					*sub_out = s;
					return l;
				}
			}
		} else if (l->topic_id == topic_id) {
			return l;
		}
	}
	return NULL;
}

static int publish_common(hg_pub *p, const void *payload, size_t len, uint64_t stamp_ns,
			  unsigned flags, const struct hg_input *refs, int n_refs)
{
	if (!len_ok(p, len) || n_refs < 0 || n_refs > HG_MAX_REFS || (n_refs && !refs) ||
	    (flags & ~(HG_STALE | HG_NO_REFS))) {
		errno = EINVAL;
		return -1;
	}
	uint64_t now = hg_now_ns();
	struct hg_input out[HG_MAX_REFS];
	uint32_t n_out = 0;
	int stale = (flags & HG_STALE) != 0;

	if (!(flags & HG_NO_REFS)) {
		/* Every given reference must be of an Inputs or Sources topic, and
		 * one per topic: checked before anything is written. */
		struct link *rl[HG_MAX_REFS];
		hg_sub *rs[HG_MAX_REFS];
		for (int i = 0; i < n_refs; i++) {
			rl[i] = link_for(p, refs[i].topic_id, &rs[i]);
			if (!rl[i]) {
				errno = EINVAL;
				return -1;
			}
			for (int k = 0; k < i; k++)
				if (refs[k].topic_id == refs[i].topic_id) {
					errno = EINVAL;
					return -1;
				}
		}
		/* Inputs first, each overridden by a given reference if any. */
		for (int i = 0; i < p->n_inputs; i++) {
			struct link *l = &p->inputs[i];
			int have = 0;
			uint64_t seq = 0;
			for (int k = 0; k < n_refs; k++)
				if (rl[k] == l) {
					have = 1;
					seq = refs[k].seq;
				}
			struct hg_input *o = &out[n_out++];
			if (l->remote) {
				/* Of the other side: recorded as given, never checked. */
				o->topic_id = l->topic_id;
				o->flags = 0;
				o->seq = have ? seq : 0;
				continue;
			}
			link_resolve(l);
			if (l->sub)
				ref_from_sub(l, l->sub, now, have, seq, o, &stale);
			else if (l->pub)
				ref_from_pub(l, l->pub, have, seq, o, &stale);
			else {
				/* Declared but not open in this module: nothing read from it. */
				o->topic_id = l->topic_id;
				o->flags = HG_REF_ABSENT;
				o->seq = have ? seq : 0;
				stale = 1;
			}
		}
		/* Then the Sources named for this message. */
		for (int k = 0; k < n_refs; k++) {
			struct link *l = rl[k];
			int is_input = l >= p->inputs && l < p->inputs + p->n_inputs;
			if (is_input)
				continue;
			if (n_out == HG_MAX_REFS) {
				errno = EINVAL;
				return -1;
			}
			struct hg_input *o = &out[n_out++];
			hg_sub *s = rs[k];
			if (!s) {
				link_resolve(l);
				s = l->sub;
			}
			if (s) {
				ref_from_sub(l, s, now, 1, refs[k].seq, o, &stale);
			} else if (l->pub) {
				ref_from_pub(l, l->pub, 1, refs[k].seq, o, &stale);
			} else {
				o->topic_id = refs[k].topic_id;
				o->flags = HG_REF_ABSENT;
				o->seq = refs[k].seq;
				stale = 1;
			}
		}
	}
	publish_slot(p, p->seq + 1, stamp_ns, now, stale ? HG_SLOT_STALE : 0, out, n_out, payload, len);
	return 0;
}

int hg_publish(hg_pub *p, const void *payload, size_t len, uint64_t stamp_ns)
{
	return publish_common(p, payload, len, stamp_ns, 0, NULL, 0);
}

int hg_publish_with(hg_pub *p, const void *payload, size_t len, uint64_t stamp_ns, unsigned flags,
		    const struct hg_input *refs, int n_refs)
{
	return publish_common(p, payload, len, stamp_ns, flags, refs, n_refs);
}

int hg_publish_seq(hg_pub *p, uint64_t seq, uint32_t slot_flags, const void *payload, size_t len,
		   uint64_t stamp_ns, const struct hg_input *refs, int n_refs)
{
	if (!p->mirror) {
		errno = EPERM;
		return -1;
	}
	if (!len_ok(p, len) || n_refs < 0 || n_refs > HG_MAX_REFS || (n_refs && !refs)) {
		errno = EINVAL;
		return -1;
	}
	if (seq <= p->seq) {
		errno = ERANGE;
		return -1;
	}
	publish_slot(p, seq, stamp_ns, hg_now_ns(), slot_flags, refs, (uint32_t)n_refs, payload, len);
	return 0;
}

void hg_pub_close(hg_pub *p)
{
	if (!p)
		return;
	atomic_fetch_or_explicit(&p->h->flags, HG_FLAG_CLOSED, memory_order_release);
	announce(p->h);
	munmap(p->wake, WAKE_BYTES);
	munmap(p->h, p->map_len);
	for (int i = 0; i < reg_npubs; i++)
		if (reg_pubs[i] == p)
			reg_pubs[i] = reg_pubs[--reg_npubs];
	/* Links of other outputs to this one must not dangle. */
	for (int i = 0; i < reg_npubs; i++) {
		hg_pub *o = reg_pubs[i];
		for (int k = 0; k < o->n_inputs; k++)
			if (o->inputs[k].pub == p)
				o->inputs[k].pub = NULL;
		for (int k = 0; k < o->n_sources; k++)
			if (o->sources[k].pub == p)
				o->sources[k].pub = NULL;
	}
	free(p);
}

/* ---- Reader ------------------------------------------------------------ */

struct mapping {
	struct hg_header *h;
	size_t len;
	dev_t dev;
	ino_t ino;
	struct layout l;
};

/* Maps the topic file at path read-only. -1 with errno on failure: ENOENT if
 * there is none, EPROTO if it is not a valid topic. */
static int map_topic_ro(const char *path, struct mapping *m)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct stat st;
	int rc = -1;
	if (fstat(fd, &st) < 0)
		goto out;
	if (st.st_size < (off_t)HEADER_BYTES) {
		errno = EPROTO;
		goto out;
	}
	void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		goto out;
	if (read_layout(p, (uint64_t)st.st_size, &m->l) < 0) {
		munmap(p, (size_t)st.st_size);
		goto out;
	}
	m->h = p;
	m->len = (size_t)st.st_size;
	m->dev = st.st_dev;
	m->ino = st.st_ino;
	rc = 0;
out:;
	int e = errno;
	close(fd);
	errno = e;
	return rc;
}

/* Switches s to mapping m. Only the validated copy of the header is used,
 * never the shared one. A different origin_boot starts a new epoch. If the
 * hg_wait_fd helper holds the old mapping, it is told and the old mapping
 * kept until it has acknowledged. */
static void use_mapping(hg_sub *s, const struct mapping *m)
{
	struct hg_header *old = s->h;
	size_t old_len = s->map_len;
	int new_epoch = s->h && memcmp(s->origin, m->l.origin_boot, 16) != 0;
	fd_lock(s);
	s->h = m->h;
	s->map_len = m->len;
	s->dev = m->dev;
	s->ino = m->ino;
	s->slots = (unsigned char *)m->h + HEADER_BYTES;
	s->stride = m->l.stride;
	s->mask = m->l.slot_count - 1;
	s->payload_bytes = m->l.payload_bytes;
	s->rate_mhz = m->l.rate_mhz;
	s->stale_after_ns = m->l.stale_after_ns;
	s->first = m->l.first_seq;
	memcpy(s->origin, m->l.origin_boot, 16);
	s->seen_flags = 0;
	s->reopen_since = 0;
	s->type_bad = s->has_expect && m->l.type_hash != s->expect_hash;
	s->type_bad_reported = 0;
	s->attach_err = 0;
	if (new_epoch) {
		s->epoch++;
		s->last = s->first;
		s->use_set = 0;
	}
	fd_unlock(s);
	if (s->in_fd_set)
		fd_set_changed();
	if (old)
		munmap(old, old_len);
}

/* Attaches an absent subscription. 0 on success, -1 with errno (ENOENT: still
 * absent). announce: the topic was absent and appeared, which is news once;
 * a topic present when the subscription was opened is not. */
static int attach(hg_sub *s, int announce)
{
	struct mapping m;
	if (map_topic_ro(s->path, &m) < 0) {
		if (errno != ENOENT && s->attach_err != errno) {
			s->attach_err = errno;
			s->attach_err_reported = 0;
		}
		return -1;
	}
	if (!s->wake && !(s->wake = map_wake(s->wake_path, 0))) {
		int e = errno;
		munmap(m.h, m.len);
		errno = e;
		return -1;
	}
	use_mapping(s, &m);
	if (!s->started) {
		/* A fresh subscription starts at the newest message, or, asked
		 * for, at the oldest still safely in the ring (W-K+2 onward). */
		uint64_t w = atomic_load_explicit(&s->h->write_seq, memory_order_acquire);
		uint64_t k = (uint64_t)s->mask + 1;
		if (s->start == HG_OLDEST)
			s->last = w - s->first > k - 1 ? w - k + 1 : s->first;
		else
			s->last = w > s->first ? w - 1 : s->first; /* the newest message, once */
		s->started = 1;
	}
	if (announce) {
		s->appeared = 1;
		s->appeared_reported = 0;
	}
	return 0;
}

/* After flag bit 2: switches to the file now at the path. 1 if it did; 0 if
 * the path still holds the old file, or none (the manager flags the old
 * file before it renames the new one into place); -1 with errno on error. */
static int reopen(hg_sub *s, uint32_t flags)
{
	struct mapping m;
	if (map_topic_ro(s->path, &m) == 0) {
		if (m.dev != s->dev || m.ino != s->ino) {
			use_mapping(s, &m);
			s->reopens++;
			return 1;
		}
		munmap(m.h, m.len);
	} else if (errno != ENOENT) {
		return -1;
	}
	if (!s->reopen_since) {
		s->reopen_since = hg_now_ns();
		s->reopen_word = atomic_load_explicit(&s->h->futex_word, memory_order_acquire);
	}
	s->seen_flags = flags;
	return 0;
}

static struct hg_slot *slot_at(const hg_sub *s, uint64_t n)
{
	return (struct hg_slot *)(s->slots + ((n - 1) & s->mask) * s->stride);
}

static void copy_slot(struct hg_slot *sl, void *buf, size_t len, struct hg_info *info)
{
	info->stamp_ns = atomic_load_explicit(&sl->stamp_ns, memory_order_relaxed);
	info->publish_ns = atomic_load_explicit(&sl->publish_ns, memory_order_relaxed);
	info->flags = atomic_load_explicit(&sl->flags, memory_order_relaxed);
	uint32_t ni = atomic_load_explicit(&sl->n_inputs, memory_order_relaxed);
	info->n_inputs = ni > HG_MAX_REFS ? HG_MAX_REFS : ni;
	for (uint32_t i = 0; i < HG_MAX_REFS; i++) {
		info->inputs[i].topic_id = atomic_load_explicit(&sl->inputs[i].topic_id, memory_order_relaxed);
		info->inputs[i].flags = atomic_load_explicit(&sl->inputs[i].flags, memory_order_relaxed);
		info->inputs[i].seq = atomic_load_explicit(&sl->inputs[i].seq, memory_order_relaxed);
	}
	/* A writer overtaking this reader makes a mix of two messages; the
	 * caller's second load of seq, after an acquire fence, discards it. */
	load_words(buf, sl->payload, len);
}

/* Reads message n from its slot if the slot holds it whole. 1 on a copy, 0
 * if the slot holds another seq (n is not in the ring), -1 if the writer
 * overtook the copy (the caller decides whether to retry). */
static int read_slot(hg_sub *s, uint64_t n, void *buf, size_t buf_len, struct hg_info *info)
{
	struct hg_slot *sl = slot_at(s, n);
	uint64_t s1 = atomic_load_explicit(&sl->seq, memory_order_acquire);
	if (s1 != 2 * n)
		return 0;
	/* A buffer sized to the C struct, below the payload rounded up to 8,
	 * gets the bytes it has room for; the rest is the rounding's padding. */
	copy_slot(sl, buf, buf_len < s->payload_bytes ? buf_len : s->payload_bytes, info);
	atomic_thread_fence(memory_order_acquire);
	uint64_t s2 = atomic_load_explicit(&sl->seq, memory_order_relaxed);
	return s2 == s1 ? 1 : -1;
}

static void fill_common(hg_sub *s, struct hg_info *info, uint64_t now)
{
	info->epoch = s->epoch;
	if (s->h) {
		uint64_t lp = atomic_load_explicit(&s->h->last_publish_ns, memory_order_relaxed);
		info->age_ns = lp && now > lp ? now - lp : 0;
	}
}

static enum hg_result finish(hg_sub *s, struct hg_info *info, enum hg_result r)
{
	info->result = r;
	fill_common(s, info, hg_now_ns());
	return r;
}

static void returned(hg_sub *s, uint64_t n, const struct hg_info *info)
{
	s->last = n;
	s->last_flags = info->flags;
}

/* A replacing writer flags the old file, then renames its own into place.
 * When no new file has appeared a second later, that writer failed: report
 * the topic as closed until one does. */
#define REOPEN_GIVE_UP_NS 1000000000u

/* Nothing new in the mapped file: follow a replacement or report a close. */
static enum hg_result at_end(hg_sub *s, uint32_t flags, int *switched)
{
	flags &= HG_FLAG_CLOSED | HG_FLAG_DIED | HG_FLAG_REPLACED;
	*switched = 0;
	if (flags & HG_FLAG_REPLACED) {
		int r = reopen(s, flags);
		if (r > 0) {
			*switched = 1;
			return HG_NONE;
		}
		if (r < 0)
			return HG_ERROR;
		return hg_now_ns() - s->reopen_since >= REOPEN_GIVE_UP_NS ? HG_CLOSED : HG_NONE;
	}
	s->reopen_since = 0;
	s->seen_flags = flags;
	return flags ? HG_CLOSED : HG_NONE;
}

static hg_sub *sub_new(const char *name, const struct hg_type *type, enum hg_start from, int glob)
{
	hg_sub *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	snprintf(s->name, sizeof s->name, "%s", name);
	s->topic_id = hg_fnv1a32(name);
	s->start = from;
	s->glob = glob;
	if (type) {
		s->expect_hash = type->hash;
		s->has_expect = 1;
	}
	if (!glob && (hg_topic_path(s->path, sizeof s->path, "", name, "") < 0 ||
		      hg_topic_path(s->wake_path, sizeof s->wake_path, ".", name, ".wake") < 0)) {
		int e = errno;
		free(s);
		errno = e;
		return NULL;
	}
	if (reg_nsubs < REG_MAX)
		reg_subs[reg_nsubs++] = s;
	return s;
}

hg_sub *hg_sub_open_from(const char *name, const struct hg_type *type, enum hg_start from)
{
	if (ensure_init() < 0)
		return NULL;
	int glob = is_glob(name);
	if (!valid_segments(name, strlen(name), glob)) {
		errno = EINVAL;
		return NULL;
	}
	if (wiring) {
		if (glob) {
			if (!hg_wiring_find_glob(wiring, name)) {
				errno = EACCES;
				return NULL;
			}
		} else {
			const struct hg_wiring_sub *l = hg_wiring_find_sub(wiring, name);
			if (!l && !hg_wiring_glob_matches(wiring, name)) {
				errno = EACCES;
				return NULL;
			}
			if (l && l->hash && type && type->hash != l->hash) {
				errno = EPROTOTYPE;
				return NULL;
			}
		}
	}
	/* The directory word is needed as soon as a topic is absent; mapping it
	 * now keeps the wait free of file opens that could fail. */
	if (!dir_word())
		return NULL;
	hg_sub *s = sub_new(name, type, from, glob);
	if (!s)
		return NULL;
	if (glob) {
		(void)hg_glob_poll(s);
	} else {
		(void)attach(s, 0); /* absent or failing: reported by the first read */
	}
	return s;
}

hg_sub *hg_sub_open(const char *name, const struct hg_type *type)
{
	return hg_sub_open_from(name, type, HG_NEWEST);
}

void hg_sub_close(hg_sub *s)
{
	if (!s)
		return;
	if (s->in_fd_set) {
		pthread_mutex_lock(&FD.lock);
		for (int i = 0; i < FD.n; i++)
			if (FD.subs[i] == s)
				FD.subs[i] = FD.subs[--FD.n];
		s->in_fd_set = 0;
		pthread_mutex_unlock(&FD.lock);
		fd_set_changed();
	}
	for (int i = 0; i < s->n_kids; i++)
		hg_sub_close(s->kids[i]);
	free(s->kids);
	if (s->h)
		munmap(s->h, s->map_len);
	if (s->wake)
		munmap(s->wake, WAKE_BYTES);
	for (int i = 0; i < reg_nsubs; i++)
		if (reg_subs[i] == s)
			reg_subs[i] = reg_subs[--reg_nsubs];
	for (int i = 0; i < reg_npubs; i++) {
		hg_pub *o = reg_pubs[i];
		for (int k = 0; k < o->n_inputs; k++)
			if (o->inputs[k].sub == s)
				o->inputs[k].sub = NULL;
		for (int k = 0; k < o->n_sources; k++)
			if (o->sources[k].sub == s)
				o->sources[k].sub = NULL;
	}
	free(s);
}

uint32_t hg_sub_payload_bytes(const hg_sub *s)
{
	return s->h ? s->payload_bytes : 0;
}

uint64_t hg_sub_reopens(const hg_sub *s)
{
	return s->reopens;
}

const char *hg_sub_name(const hg_sub *s)
{
	return s->name;
}

uint32_t hg_sub_topic_id(const hg_sub *s)
{
	return s->topic_id;
}

const char *hg_sub_schema(const hg_sub *s, uint32_t *len)
{
	if (!s->h)
		return NULL;
	struct layout l;
	if (read_layout(s->h, s->map_len, &l) < 0)
		return NULL;
	if (len)
		*len = l.schema_bytes;
	return s->h->schema;
}

int hg_sub_meta(const hg_sub *s, struct hg_topic_meta *out)
{
	if (!s->h) {
		errno = ENOENT;
		return -1;
	}
	/* The fields written once at creation: a copy, atomic words, as
	 * read_layout takes them. */
	struct hg_header c;
	load_words(&c, (const _Atomic uint64_t *)(const void *)s->h, offsetof(struct hg_header, futex_word));
	memset(out, 0, sizeof *out);
	memcpy(out->name, c.name, 48);
	memcpy(out->writer, c.writer, 32);
	out->generation = c.generation;
	out->type_hash = c.type_hash;
	out->first_seq = c.first_seq;
	out->stale_after_ns = c.stale_after_ns;
	out->created_real_ns = c.created_real_ns;
	out->slot_count = c.slot_count;
	out->payload_bytes = c.payload_bytes;
	out->topic_id = c.topic_id;
	out->rate_mhz = c.rate_mhz;
	out->writer_pid = c.writer_pid;
	memcpy(out->origin_boot, s->origin, 16);
	return 0;
}

void hg_use(hg_sub *s, const struct hg_info *info)
{
	s->use_set = 1;
	s->use_seq = info->seq;
	s->use_flags = info->flags;
}

/* HG_MSG if attached and readable; else what the read returns. */
static enum hg_result ensure_attached(hg_sub *s, size_t buf_len)
{
	if (!s->h && attach(s, 1) < 0) {
		if (errno == ENOENT)
			return HG_ABSENT;
		s->attach_err_reported = 1;
		return HG_ERROR;
	}
	s->appeared_reported = 1;
	if (s->type_bad) {
		s->type_bad_reported = 1;
		errno = EPROTOTYPE;
		return HG_ERROR;
	}
	/* The payload is the type's size rounded up to 8; a buffer of the
	 * unrounded size is fine, one shorter is not. */
	if (buf_len + 7 < s->payload_bytes) {
		errno = EMSGSIZE;
		return HG_ERROR;
	}
	return HG_MSG;
}

enum hg_result hg_latest(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info)
{
	memset(info, 0, sizeof *info);
	if (s->glob) {
		errno = EINVAL;
		return finish(s, info, HG_ERROR);
	}
	for (int tries = 0;;) {
		enum hg_result a = ensure_attached(s, buf_len);
		if (a != HG_MSG)
			return finish(s, info, a);
		struct hg_header *h = s->h;
		uint32_t flags = atomic_load_explicit(&h->flags, memory_order_acquire);
		uint64_t n = atomic_load_explicit(&h->write_seq, memory_order_acquire);
		if (n <= s->last || n <= s->first) {
			int switched;
			enum hg_result r = at_end(s, flags, &switched);
			if (switched)
				continue;
			return finish(s, info, r);
		}
		int r = read_slot(s, n, buf, buf_len, info);
		if (r > 0) {
			returned(s, n, info);
			info->seq = n;
			return finish(s, info, HG_MSG);
		}
		if (++tries >= 8)
			return finish(s, info, HG_BUSY);
	}
}

enum hg_result hg_next(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info)
{
	memset(info, 0, sizeof *info);
	if (s->glob) {
		errno = EINVAL;
		return finish(s, info, HG_ERROR);
	}
	/* Seqs stepped over on the way to a message: lost, unless that message
	 * says a rate-limited mirror skipped them on purpose (flag bit 1). */
	uint64_t pending = 0;
	for (;;) {
		enum hg_result a = ensure_attached(s, buf_len);
		if (a != HG_MSG)
			return finish(s, info, a);
		struct hg_header *h = s->h;
		uint64_t k = (uint64_t)s->mask + 1;
		uint32_t flags = atomic_load_explicit(&h->flags, memory_order_acquire);
		uint64_t W = atomic_load_explicit(&h->write_seq, memory_order_acquire);
		if (s->last < s->first) {
			/* Messages before this file's first are in no file we can read. */
			pending += s->first - s->last;
			s->last = s->first;
		}
		uint64_t w = s->last + 1;
		if (w > W) {
			int switched;
			enum hg_result r = at_end(s, flags, &switched);
			if (switched)
				continue;
			info->gap += pending;
			return finish(s, info, r);
		}
		if (W - w >= k - 1) {
			/* w <= W-K+1: gone, or in the slot the writer fills next. */
			uint64_t nw = W - k + 2;
			pending += nw - w;
			w = nw;
			s->last = w - 1;
		}
		int r = read_slot(s, w, buf, buf_len, info);
		if (r > 0) {
			if (info->flags & HG_SLOT_DECIMATED)
				info->skipped += pending;
			else
				info->gap += pending;
			returned(s, w, info);
			info->seq = w;
			return finish(s, info, HG_MSG);
		}
		if (r < 0)
			continue; /* overwritten while copying: lost on the next pass */
		/* Odd (being rewritten, or its writer died mid-publish), newer,
		 * older or zero: message w is not in the ring. Never retry it. */
		pending++;
		s->last = w;
	}
}

enum hg_result hg_read_seq(hg_sub *s, uint64_t seq, void *buf, size_t buf_len, struct hg_info *info)
{
	memset(info, 0, sizeof *info);
	if (s->glob) {
		errno = EINVAL;
		return finish(s, info, HG_ERROR);
	}
	enum hg_result a = ensure_attached(s, buf_len);
	if (a != HG_MSG)
		return finish(s, info, a);
	uint64_t W = atomic_load_explicit(&s->h->write_seq, memory_order_acquire);
	if (seq == 0 || seq <= s->first || seq > W)
		return finish(s, info, HG_GONE);
	for (int tries = 0; tries < 8; tries++) {
		int r = read_slot(s, seq, buf, buf_len, info);
		if (r > 0) {
			info->seq = seq;
			return finish(s, info, HG_MSG);
		}
		if (r == 0)
			return finish(s, info, HG_GONE);
	}
	return finish(s, info, HG_BUSY);
}

enum hg_result hg_find_stamp(hg_sub *s, uint64_t t_ns, void *buf, size_t buf_len, struct hg_info *info)
{
	memset(info, 0, sizeof *info);
	if (s->glob) {
		errno = EINVAL;
		return finish(s, info, HG_ERROR);
	}
	enum hg_result a = ensure_attached(s, buf_len);
	if (a != HG_MSG)
		return finish(s, info, a);
	/* From the newest seq back to the oldest that can still be in the ring,
	 * passing over seqs not in it; stamps rise with seqs, so the first one
	 * at or before t is the answer. */
	uint64_t W = atomic_load_explicit(&s->h->write_seq, memory_order_acquire);
	uint64_t k = (uint64_t)s->mask + 1;
	/* Down to W-K+1, the slot the writer fills next: the seq check tells
	 * whether it still holds that message, as hg_read_seq's does. */
	uint64_t lo = W - s->first >= k ? W - k + 1 : s->first + 1;
	for (uint64_t n = W; n >= lo && n > 0; n--) {
		int r = read_slot(s, n, buf, buf_len, info);
		if (r > 0 && info->stamp_ns <= t_ns) {
			info->seq = n;
			return finish(s, info, HG_MSG);
		}
		/* r < 0: the writer is overwriting this slot, so its message is
		 * the oldest in the ring and about to go; nothing older remains. */
		if (r < 0)
			break;
	}
	return finish(s, info, HG_GONE);
}

/* ---- Glob subscriptions ------------------------------------------------ */

static int glob_has(const hg_sub *g, const char *name)
{
	for (int i = 0; i < g->n_kids; i++)
		if (strcmp(g->kids[i]->name, name) == 0)
			return 1;
	return 0;
}

int hg_glob_poll(hg_sub *g)
{
	if (!g->glob) {
		errno = EINVAL;
		return -1;
	}
	/* The directory word says whether anything was created since the last
	 * scan; nothing to do while it stands. */
	struct hg_wake *d = dir_word();
	uint32_t word = d ? atomic_load_explicit(&d->wait_flag, memory_order_acquire) : 0;
	if (g->scanned && word == g->scan_word)
		return 0;
	const char *dir = getenv("HANGAR_TOPIC_DIR");
	if (!dir) {
		errno = EINVAL;
		return -1;
	}
	DIR *dp = opendir(dir);
	if (!dp)
		return -1;
	int added = 0;
	struct dirent *e;
	while ((e = readdir(dp))) {
		if (e->d_name[0] == '.' || !hg_valid_name(e->d_name) ||
		    !hg_glob_match(g->name, e->d_name) || glob_has(g, e->d_name))
			continue;
		if (g->n_kids == g->cap_kids) {
			int nc = g->cap_kids ? g->cap_kids * 2 : 8;
			hg_sub **nk = realloc(g->kids, (size_t)nc * sizeof *nk);
			if (!nk)
				break;
			g->kids = nk;
			g->cap_kids = nc;
		}
		hg_sub *k = sub_new(e->d_name, g->has_expect ? &(struct hg_type){ .hash = g->expect_hash } : NULL,
				    g->start, 0);
		if (!k)
			break;
		(void)attach(k, 1); /* a match found after the open: news */
		g->kids[g->n_kids++] = k;
		added++;
	}
	closedir(dp);
	g->scan_word = word;
	g->scanned = 1;
	return added;
}

int hg_glob_count(const hg_sub *g)
{
	return g->glob ? g->n_kids : 0;
}

hg_sub *hg_glob_at(const hg_sub *g, int i)
{
	return g->glob && i >= 0 && i < g->n_kids ? g->kids[i] : NULL;
}

/* ---- Waiting ----------------------------------------------------------- */

/* Set by hg_wait_interrupt(). Process-private: hg_wait() also sleeps on it. */
static _Atomic uint32_t interrupt_word;

void hg_wait_interrupt(void)
{
	int e = errno;
	atomic_store_explicit(&interrupt_word, 1, memory_order_seq_cst);
	futex_wake_private(&interrupt_word);
	errno = e;
}

/* Whether s has news: a message above the last one returned, or header
 * flags that differ from those last reported, or a type mismatch or attach
 * error not yet reported. The loads are seq_cst: the flag store before them
 * and the write_seq load, against the writer's write_seq store and flag
 * exchange, are a store-buffering pattern (an acquire load could be done
 * before the flag store, LDAPR on ARMv8.3+). */
static int has_news(const hg_sub *s)
{
	if (!s->h)
		return s->attach_err && !s->attach_err_reported;
	if (s->type_bad)
		return !s->type_bad_reported;
	if (s->appeared && !s->appeared_reported)
		return 1;
	uint64_t w = atomic_load_explicit(&s->h->write_seq, memory_order_seq_cst);
	uint32_t f = atomic_load_explicit(&s->h->flags, memory_order_seq_cst) &
		     (HG_FLAG_CLOSED | HG_FLAG_DIED | HG_FLAG_REPLACED);
	return w > s->last || f != s->seen_flags;
}

#define GLOB_POLL_NS 10000000u /* glob matches are polled, every 10 ms at most */

unsigned hg_wait_ready(hg_sub *const *subs, int n, int timeout_ms, unsigned char *ready)
{
	if (n <= 0 || n > HG_MAX_WAIT) {
		errno = EINVAL;
		return (unsigned)-1;
	}
	uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : hg_now_ns() + (uint64_t)timeout_ms * 1000000u;
	struct hg_futex_waitv wv[HG_FUTEX_WAITV_MAX];
	unsigned char local[HG_MAX_WAIT];
	if (!ready)
		ready = local;

	for (;;) {
		/* Taken before the sleep: an interrupt after this changes the
		 * word the sleep compares with 0, so it fails at once or is woken. */
		if (atomic_exchange_explicit(&interrupt_word, 0, memory_order_seq_cst))
			return HG_READY_INTERRUPT;
		wv[0] = (struct hg_futex_waitv){ .val = 0, .uaddr = (uint64_t)(uintptr_t)&interrupt_word,
						 .flags = HG_FUTEX_32_PRIVATE };
		int nw = 1, nready = 0, need_dir = 0, have_glob = 0;

		/* The directory word first, then the retries of every absent
		 * topic: one created after this read has changed the word, so
		 * the sleep on this value returns at once. */
		struct hg_wake *d = dir_word();
		uint32_t dir_val = d ? atomic_load_explicit(&d->wait_flag, memory_order_acquire) : 0;

		for (int i = 0; i < n; i++) {
			hg_sub *s = subs[i];
			ready[i] = 0;
			if (s->glob) {
				have_glob = 1;
				(void)hg_glob_poll(s);
				for (int k = 0; k < s->n_kids; k++)
					if (!s->kids[k]->h)
						(void)attach(s->kids[k], 1);
				continue;
			}
			if (!s->h) {
				if (attach(s, 1) < 0)
					need_dir = 1; /* created later: the word says so */
				continue;
			}
			if (!s->reopen_since)
				continue;
			/* Replaced, the new file not yet at the path. */
			uint32_t f = atomic_load_explicit(&s->h->flags, memory_order_acquire);
			if (!(f & HG_FLAG_REPLACED)) {
				s->reopen_since = 0; /* the replacing writer backed out */
				continue;
			}
			if (reopen(s, f) == 0)
				need_dir = 1; /* the rename bumps the word */
		}
		/* The futexes: every attached topic's word. With n <= 127 there
		 * is room for the interrupt word and, since an absent topic has
		 * no futex, for the directory word as well. */
		for (int i = 0; i < n; i++) {
			hg_sub *s = subs[i];
			if (s->glob || !s->h || nw == HG_FUTEX_WAITV_MAX)
				continue;
			wv[nw++] = (struct hg_futex_waitv){
				.val = atomic_load_explicit(&s->h->futex_word, memory_order_acquire),
				.uaddr = (uint64_t)(uintptr_t)&s->h->futex_word, .flags = HG_FUTEX_32 };
		}
		if (need_dir && d && nw < HG_FUTEX_WAITV_MAX)
			wv[nw++] = (struct hg_futex_waitv){ .val = dir_val,
							    .uaddr = (uint64_t)(uintptr_t)&d->wait_flag,
							    .flags = HG_FUTEX_32 };
		/* The flags, then the re-check (see has_news for the ordering). */
		for (int i = 0; i < n; i++)
			if (!subs[i]->glob && subs[i]->h)
				atomic_store_explicit(&subs[i]->wake->wait_flag, 1, memory_order_seq_cst);
		for (int i = 0; i < n; i++) {
			hg_sub *s = subs[i];
			if (s->glob) {
				for (int k = 0; k < s->n_kids; k++)
					if (has_news(s->kids[k]))
						ready[i] = 1;
			} else if (has_news(s)) {
				ready[i] = 1;
			}
			nready += ready[i];
		}
		if (nready)
			return HG_READY_TOPICS;

		uint64_t now = hg_now_ns();
		if (now >= deadline)
			return 0;
		uint64_t until = deadline;
		if (have_glob && now + GLOB_POLL_NS < until)
			until = now + GLOB_POLL_NS;
		struct timespec ts = { .tv_sec = (time_t)(until / 1000000000u),
				       .tv_nsec = (long)(until % 1000000000u) };
		long r = syscall(SYS_futex_waitv, wv, (unsigned)nw, 0u, until == UINT64_MAX ? NULL : &ts,
				 CLOCK_MONOTONIC);
		if (r < 0 && errno == ETIMEDOUT && until == deadline) {
			/* A timeout, even if news came in meanwhile: a wake-up that
			 * never came must show. */
			memset(ready, 0, (size_t)n);
			return 0;
		}
		if (r < 0 && errno == EINTR)
			return HG_READY_INTERRUPT;
		if (r < 0 && errno != EAGAIN && errno != ETIMEDOUT)
			return (unsigned)-1;
	}
}

unsigned hg_wait(hg_sub *const *subs, int n, int timeout_ms)
{
	return hg_wait_ready(subs, n, timeout_ms, NULL);
}

/* ---- hg_wait_fd -------------------------------------------------------- */

static void *fd_helper(void *arg)
{
	(void)arg;
	struct hg_futex_waitv wv[HG_FUTEX_WAITV_MAX];
	struct hg_wake *wakes[HG_MAX_WAIT_FD];
	for (;;) {
		/* The helper word is read before the set: a change made after
		 * this read makes the sleep below fail at once, and one made
		 * before it is in the set this pass reads. */
		uint32_t hv = atomic_load_explicit(&FD.helper_word, memory_order_seq_cst);
		if (atomic_load_explicit(&FD.stop, memory_order_acquire))
			break;
		int nw = 0, nwake = 0, need_dir = 0;
		wv[nw++] = (struct hg_futex_waitv){ .val = hv, .uaddr = (uint64_t)(uintptr_t)&FD.helper_word,
						    .flags = HG_FUTEX_32_PRIVATE };
		pthread_mutex_lock(&FD.lock);
		uint32_t g = atomic_load_explicit(&FD.gen, memory_order_acquire);
		for (int i = 0; i < FD.n; i++) {
			hg_sub *s = FD.subs[i];
			if (s->glob)
				continue; /* polled by the module itself */
			if (!s->h) {
				need_dir = 1;
				continue;
			}
			/* The snapshot: taken before the eventfd is signalled, so a
			 * message published while the module reads changes a word
			 * the next sleep compares against. */
			wv[nw++] = (struct hg_futex_waitv){
				.val = atomic_load_explicit(&s->h->futex_word, memory_order_acquire),
				.uaddr = (uint64_t)(uintptr_t)&s->h->futex_word, .flags = HG_FUTEX_32 };
			wakes[nwake++] = s->wake;
		}
		pthread_mutex_unlock(&FD.lock);
		struct hg_wake *d = need_dir ? dir_word() : NULL;
		if (d)
			wv[nw++] = (struct hg_futex_waitv){
				.val = atomic_load_explicit(&d->wait_flag, memory_order_acquire),
				.uaddr = (uint64_t)(uintptr_t)&d->wait_flag, .flags = HG_FUTEX_32 };
		/* Acknowledged after the snapshot: the module keeps old mappings
		 * until here, and this pass's addresses are all still mapped. */
		atomic_store_explicit(&FD.ack, g, memory_order_release);
		futex_wake_private(&FD.ack);
		for (int i = 0; i < nwake; i++)
			atomic_store_explicit(&wakes[i]->wait_flag, 1, memory_order_seq_cst);
		uint64_t one = 1;
		if (write(FD.efd, &one, sizeof one) < 0 && errno == EINTR)
			(void)write(FD.efd, &one, sizeof one);
		long r = syscall(SYS_futex_waitv, wv, (unsigned)nw, 0u, NULL, CLOCK_MONOTONIC);
		(void)r; /* EAGAIN: a word moved already; EINTR: look again */
	}
	return NULL;
}

int hg_wait_fd(hg_sub *const *subs, int n)
{
	if (FD.running) {
		errno = EBUSY;
		return -1;
	}
	if (n <= 0 || n > HG_MAX_WAIT_FD) {
		errno = E2BIG;
		return -1;
	}
	int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (efd < 0)
		return -1;
	pthread_mutex_lock(&FD.lock);
	FD.efd = efd;
	FD.n = n;
	for (int i = 0; i < n; i++) {
		FD.subs[i] = subs[i];
		subs[i]->in_fd_set = 1;
	}
	atomic_store_explicit(&FD.stop, 0, memory_order_relaxed);
	pthread_mutex_unlock(&FD.lock);
	int rc = pthread_create(&FD.thread, NULL, fd_helper, NULL);
	if (rc) {
		for (int i = 0; i < n; i++)
			subs[i]->in_fd_set = 0;
		FD.n = 0;
		close(efd);
		FD.efd = -1;
		errno = rc;
		return -1;
	}
	FD.running = 1;
	return efd;
}

void hg_wait_fd_close(void)
{
	if (!FD.running)
		return;
	atomic_store_explicit(&FD.stop, 1, memory_order_release);
	atomic_fetch_add_explicit(&FD.helper_word, 1, memory_order_seq_cst);
	futex_wake_private(&FD.helper_word);
	pthread_join(FD.thread, NULL);
	FD.running = 0;
	pthread_mutex_lock(&FD.lock);
	for (int i = 0; i < FD.n; i++)
		FD.subs[i]->in_fd_set = 0;
	FD.n = 0;
	pthread_mutex_unlock(&FD.lock);
	close(FD.efd);
	FD.efd = -1;
}
