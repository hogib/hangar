/* hangar topics: shared-memory ring files, seqlocked slots, futex wake-ups.
 * The layout and the algorithms are those of docs/01-topics.md. */
#define _GNU_SOURCE
#include "hg_topic.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

struct hg_header {
	char magic[8];                   /*   0 */
	uint32_t version;                /*   8 */
	uint32_t header_bytes;           /*  12 */
	uint64_t generation;             /*  16 */
	uint32_t slot_count;             /*  24 */
	uint32_t slot_header_bytes;      /*  28 */
	uint32_t payload_bytes;          /*  32 */
	_Atomic uint32_t flags;          /*  36 */
	uint64_t slot_stride;            /*  40 */
	uint64_t slots_offset;           /*  48 */
	uint32_t topic_id;               /*  56 */
	uint32_t rate_mhz;               /*  60 */
	uint64_t type_hash;              /*  64 */
	char name[48];                   /*  72 */
	char writer[32];                 /* 120 */
	int32_t writer_pid;              /* 152 */
	uint32_t schema_bytes;           /* 156 */
	uint64_t created_real_ns;        /* 160 */
	uint64_t first_seq;              /* 168 */
	uint64_t stale_after_ns;         /* 176 */
	uint64_t reserved0;              /* 184 */
	_Atomic uint32_t futex_word;     /* 192 */
	uint32_t reserved1;              /* 196 */
	_Atomic uint64_t write_seq;      /* 200 */
	_Atomic uint64_t last_publish_ns;/* 208 */
	unsigned char rest[HEADER_BYTES - 216];
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
	_Atomic uint64_t seq;            /*   0 */
	_Atomic uint64_t stamp_ns;       /*   8 */
	_Atomic uint64_t publish_ns;     /*  16 */
	_Atomic uint32_t n_inputs;       /*  24 */
	_Atomic uint32_t flags;          /*  28 */
	unsigned char reserved[32];      /*  32 */
	struct hg_slot_input inputs[4];  /*  64 */
	_Atomic uint64_t payload[];      /* 128, payload_bytes rounded up to 8 */
};

struct hg_wake {
	_Atomic uint32_t wait_flag;
	unsigned char rest[WAKE_BYTES - 4];
};

#define CHECK_OFFSET(type, field, off) \
	_Static_assert(offsetof(struct type, field) == (off), #field " offset")
CHECK_OFFSET(hg_header, version, 8);
CHECK_OFFSET(hg_header, generation, 16);
CHECK_OFFSET(hg_header, slot_count, 24);
CHECK_OFFSET(hg_header, payload_bytes, 32);
CHECK_OFFSET(hg_header, flags, 36);
CHECK_OFFSET(hg_header, slot_stride, 40);
CHECK_OFFSET(hg_header, slots_offset, 48);
CHECK_OFFSET(hg_header, topic_id, 56);
CHECK_OFFSET(hg_header, name, 72);
CHECK_OFFSET(hg_header, first_seq, 168);
CHECK_OFFSET(hg_header, futex_word, 192);
CHECK_OFFSET(hg_header, write_seq, 200);
CHECK_OFFSET(hg_header, last_publish_ns, 208);
CHECK_OFFSET(hg_slot, stamp_ns, 8);
CHECK_OFFSET(hg_slot, publish_ns, 16);
CHECK_OFFSET(hg_slot, n_inputs, 24);
CHECK_OFFSET(hg_slot, flags, 28);
CHECK_OFFSET(hg_slot, inputs, 64);
CHECK_OFFSET(hg_slot, payload, 128);
_Static_assert(sizeof(struct hg_header) == HEADER_BYTES, "header size");
_Static_assert(sizeof(struct hg_slot) == SLOT_HEADER_BYTES, "slot header size");
_Static_assert(sizeof(struct hg_wake) == WAKE_BYTES, "wake file size");

/* ---- Helpers ----------------------------------------------------------- */

uint64_t hg_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint32_t hg_fnv1a32(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}
	return h;
}

int hg_futex_wake_all(void *addr)
{
	return (int)syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static uint64_t stride_for(uint32_t payload_bytes)
{
	uint64_t p = ((uint64_t)payload_bytes + 7) & ~(uint64_t)7;
	return (SLOT_HEADER_BYTES + p + 63) & ~(uint64_t)63;
}

int hg_valid_name(const char *name)
{
	size_t len = strlen(name);
	if (len == 0 || len > HG_NAME_MAX || name[0] == '.' || name[len - 1] == '.')
		return 0;
	for (size_t i = 0; i < len; i++) {
		char c = name[i];
		if (c == '.') {
			if (name[i + 1] == '.')
				return 0;
		} else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
			return 0;
		}
	}
	return 1;
}

/* Builds "<dir>/<prefix><name><suffix>" from $HANGAR_TOPIC_DIR. */
static int topic_path(char *out, size_t len, const char *prefix, const char *name,
		      const char *suffix)
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

/* The fields of a header that a reader relies on, validated. */
struct layout {
	uint64_t stride;
	uint64_t first_seq;
	uint32_t slot_count;
	uint32_t payload_bytes;
};

/* Fills *out from one copy of the header, taken before it is validated, so
 * that a writer changing the shared header cannot get unchecked sizes past
 * the check. 0 on success, -1 with errno EPROTO if it is not a valid topic. */
static int read_layout(const struct hg_header *h, uint64_t file_size, struct layout *out)
{
	struct hg_header c; /* only the fields before futex_word are copied */
	if (file_size < HEADER_BYTES)
		goto bad;
	load_words(&c, (const _Atomic uint64_t *)(const void *)h,
		   offsetof(struct hg_header, futex_word));
	if (memcmp(c.magic, MAGIC, 8) != 0 || c.version != 1 || c.header_bytes != HEADER_BYTES ||
	    c.slot_header_bytes != SLOT_HEADER_BYTES || c.slots_offset != HEADER_BYTES)
		goto bad;
	uint32_t k = c.slot_count;
	if (k < 2 || k > 65536 || (k & (k - 1)) != 0 || c.payload_bytes == 0 ||
	    c.slot_stride != stride_for(c.payload_bytes) ||
	    file_size < HEADER_BYTES + (uint64_t)k * c.slot_stride)
		goto bad;
	*out = (struct layout){ .stride = c.slot_stride, .first_seq = c.first_seq,
				.slot_count = k, .payload_bytes = c.payload_bytes };
	return 0;
bad:
	errno = EPROTO;
	return -1;
}

static struct hg_wake *map_wake(const char *path, int create)
{
	int fd = open(path, O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0660);
	if (fd < 0)
		return NULL;
	struct stat st;
	struct hg_wake *w = NULL;
	if (fstat(fd, &st) < 0)
		goto out;
	if (st.st_size < (off_t)WAKE_BYTES) {
		if (!create) {
			errno = EPROTO;
			goto out;
		}
		if (ftruncate(fd, WAKE_BYTES) < 0)
			goto out;
	}
	if (create)
		(void)fchmod(fd, 0660);
	void *m = mmap(NULL, WAKE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m != MAP_FAILED)
		w = m;
out:;
	int e = errno;
	close(fd);
	errno = e;
	return w;
}

/* ---- Writer ------------------------------------------------------------ */

struct hg_pub {
	struct hg_header *h;
	size_t map_len;
	unsigned char *slots;
	struct hg_wake *wake;
	uint64_t seq;          /* messages published (== write_seq) */
	uint64_t stride;
	uint32_t mask;
	uint32_t payload_bytes;
};

/* Tells readers of h that its flags changed. */
static void announce(struct hg_header *h)
{
	atomic_fetch_add_explicit(&h->futex_word, 1, memory_order_seq_cst);
	hg_futex_wake_all(&h->futex_word);
}

/* The old file, read-write, if it is a valid topic. */
static struct hg_header *map_old_rw(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct stat st;
	struct hg_header *h = NULL;
	struct layout l;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)HEADER_BYTES) {
		void *m = mmap(NULL, HEADER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m != MAP_FAILED) {
			/* Only the header is mapped; the size check uses the file's. */
			if (read_layout(m, (uint64_t)st.st_size, &l) == 0)
				h = m;
			else
				munmap(m, HEADER_BYTES);
		}
	}
	close(fd);
	return h;
}

static uint64_t random_u64(void)
{
	uint64_t v;
	if (getrandom(&v, sizeof v, 0) == (ssize_t)sizeof v)
		return v;
	return hg_now_ns() ^ ((uint64_t)getpid() << 32);
}

hg_pub *hg_pub_open(const char *name, uint32_t payload_bytes, uint32_t slot_count)
{
	char path[PATH_MAX], tmp[PATH_MAX], wake_path[PATH_MAX], suffix[32];
	if (!hg_valid_name(name) || payload_bytes == 0 || slot_count < 2 || slot_count > 65536 ||
	    (slot_count & (slot_count - 1)) != 0) {
		errno = EINVAL;
		return NULL;
	}
	snprintf(suffix, sizeof suffix, ".tmp.%ld", (long)getpid());
	if (topic_path(path, sizeof path, "", name, "") < 0 ||
	    topic_path(tmp, sizeof tmp, ".", name, suffix) < 0 ||
	    topic_path(wake_path, sizeof wake_path, ".", name, ".wake") < 0)
		return NULL;

	hg_pub *p = calloc(1, sizeof *p);
	if (!p)
		return NULL;
	p->stride = stride_for(payload_bytes);
	p->mask = slot_count - 1;
	p->payload_bytes = payload_bytes;
	p->map_len = HEADER_BYTES + (size_t)slot_count * p->stride;

	struct hg_header *old = map_old_rw(path);
	uint64_t w = old ? atomic_load_explicit(&old->write_seq, memory_order_acquire) : 0;

	int fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (fd < 0 && errno == EEXIST && unlink(tmp) == 0)
		fd = open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	if (fd < 0)
		goto fail;
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
	h->slot_count = slot_count;
	h->slot_header_bytes = SLOT_HEADER_BYTES;
	h->payload_bytes = payload_bytes;
	h->slot_stride = p->stride;
	h->slots_offset = HEADER_BYTES;
	h->topic_id = hg_fnv1a32(name);
	memcpy(h->name, name, strlen(name));
	h->writer_pid = (int32_t)getpid();
	h->first_seq = w;
	atomic_store_explicit(&h->write_seq, w, memory_order_relaxed);
	p->seq = w;

	p->wake = map_wake(wake_path, 1);
	if (!p->wake)
		goto fail_map;

	uint32_t old_flags = 0;
	if (old) {
		old_flags = atomic_fetch_or_explicit(&old->flags, HG_FLAG_REPLACED,
						     memory_order_release);
		announce(old);
	}
	if (rename(tmp, path) < 0) {
		/* Readers of the old file would wait for a new one forever. */
		err = errno;
		if (old && !(old_flags & HG_FLAG_REPLACED)) {
			atomic_fetch_and_explicit(&old->flags, ~HG_FLAG_REPLACED,
						  memory_order_release);
			announce(old);
		}
		errno = err;
		goto fail_wake;
	}
	if (old)
		munmap(old, HEADER_BYTES);
	close(fd);
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

uint64_t hg_pub_next_seq(const hg_pub *p)
{
	return p->seq + 1;
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

int hg_publish(hg_pub *p, const void *payload, size_t len, uint64_t stamp_ns)
{
	if (len != p->payload_bytes) {
		errno = EINVAL;
		return -1;
	}
	struct hg_header *h = p->h;
	uint64_t n = p->seq + 1;
	struct hg_slot *s = (struct hg_slot *)(p->slots + ((n - 1) & p->mask) * p->stride);

	atomic_store_explicit(&s->seq, 2 * n - 1, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);

	uint64_t now = hg_now_ns();
	atomic_store_explicit(&s->stamp_ns, stamp_ns, memory_order_relaxed);
	atomic_store_explicit(&s->publish_ns, now, memory_order_relaxed);
	atomic_store_explicit(&s->n_inputs, 0, memory_order_relaxed);
	store_words(s->payload, payload, len);

	atomic_store_explicit(&s->seq, 2 * n, memory_order_release);
	/* seq_cst, not release as SPEC.md has it: this store and the flag
	 * exchange below, against the reader's flag store and write_seq load in
	 * hg_wait(), are a store-buffering pattern. Only with all four seq_cst
	 * does C11 rule out both sides missing each other (the exchange reading
	 * a clear flag while the reader reads the old write_seq: a lost
	 * wake-up). AArch64: the same STLR; x86: XCHG instead of MOV. */
	atomic_store_explicit(&h->write_seq, n, memory_order_seq_cst);
	atomic_store_explicit(&h->last_publish_ns, now, memory_order_relaxed);
	p->seq = n;

	/* The flag is taken before futex_word changes, not after as SPEC.md and
	 * docs/01-topics.md order it. Taken after, this interleaving loses a
	 * wake-up: the writer stores write_seq; the reader returns it; the
	 * writer increments futex_word; the reader reads futex_word, sets the
	 * flag, finds nothing new; the writer takes the flag and wakes nobody
	 * (the reader is not asleep yet); the reader sleeps on the current
	 * value, and every later publish finds the flag clear. Taken first, a
	 * publish that takes a reader's flag changes futex_word after that
	 * reader read it: its sleep either fails at once or is woken. */
	uint32_t waiting = atomic_exchange_explicit(&p->wake->wait_flag, 0, memory_order_seq_cst);
	atomic_fetch_add_explicit(&h->futex_word, 1, memory_order_seq_cst);
	if (waiting)
		hg_futex_wake_all(&h->futex_word);
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
	free(p);
}

int hg_topic_mark_died(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct stat st;
	int rc = -1;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)HEADER_BYTES) {
		struct hg_header *h =
			mmap(NULL, HEADER_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (h != MAP_FAILED) {
			if (memcmp(h->magic, MAGIC, 8) == 0) {
				atomic_fetch_or_explicit(&h->flags, HG_FLAG_DIED, memory_order_release);
				announce(h);
				rc = 0;
			} else {
				errno = EINVAL;
			}
			munmap(h, HEADER_BYTES);
		}
	}
	int e = errno;
	close(fd);
	errno = e;
	return rc;
}

/* ---- Reader ------------------------------------------------------------ */

struct hg_sub {
	char path[PATH_MAX];
	char wake_path[PATH_MAX];
	struct hg_header *h;        /* NULL while absent */
	size_t map_len;
	unsigned char *slots;
	struct hg_wake *wake;
	dev_t dev;
	ino_t ino;
	uint64_t last;              /* last seq returned */
	uint64_t first;             /* first_seq of the mapped file */
	uint64_t stride;
	uint64_t reopens;
	uint64_t reopen_since;      /* replaced, but no new file at the path since then; 0 if not */
	uint32_t reopen_word;       /* the old file's futex_word as of reopen_since */
	uint32_t mask;
	uint32_t payload_bytes;
	uint32_t seen_flags;        /* header flags already reported */
	int started;                /* attached at least once */
};

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

/* Only the validated copy of the header is used, never the shared one. */
static void use_mapping(hg_sub *s, const struct mapping *m)
{
	if (s->h)
		munmap(s->h, s->map_len);
	s->h = m->h;
	s->map_len = m->len;
	s->dev = m->dev;
	s->ino = m->ino;
	s->slots = (unsigned char *)m->h + HEADER_BYTES;
	s->stride = m->l.stride;
	s->mask = m->l.slot_count - 1;
	s->payload_bytes = m->l.payload_bytes;
	s->first = m->l.first_seq;
	s->seen_flags = 0;
	s->reopen_since = 0;
}

/* Attaches an absent subscription. 0 on success, -1 with errno (ENOENT: still
 * absent). */
static int attach(hg_sub *s)
{
	struct mapping m;
	if (map_topic_ro(s->path, &m) < 0)
		return -1;
	if (!s->wake && !(s->wake = map_wake(s->wake_path, 0))) {
		munmap(m.h, m.len);
		return -1;
	}
	use_mapping(s, &m);
	if (!s->started) {
		/* Start at the oldest message still safely in the ring. */
		uint64_t w = atomic_load_explicit(&s->h->write_seq, memory_order_acquire);
		uint64_t k = (uint64_t)s->mask + 1;
		s->last = w - s->first > k - 1 ? w - k + 1 : s->first;
		s->started = 1;
	}
	return 0;
}

/* After flag bit 2: switches to the file now at the path. 1 if it did; 0 if
 * the path still holds the old file, or none (the writer flags the old file
 * before it renames the new one into place); -1 with errno on an error. */
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
	info->n_inputs = ni > 4 ? 4 : ni;
	for (uint32_t i = 0; i < info->n_inputs; i++) {
		info->inputs[i].topic_id =
			atomic_load_explicit(&sl->inputs[i].topic_id, memory_order_relaxed);
		info->inputs[i].flags = atomic_load_explicit(&sl->inputs[i].flags, memory_order_relaxed);
		info->inputs[i].seq = atomic_load_explicit(&sl->inputs[i].seq, memory_order_relaxed);
	}
	/* A writer overtaking this reader makes a mix of two messages; the
	 * caller's second load of seq, after an acquire fence, discards it. */
	load_words(buf, sl->payload, len);
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

hg_sub *hg_sub_open(const char *name)
{
	if (!hg_valid_name(name)) {
		errno = EINVAL;
		return NULL;
	}
	hg_sub *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	if (topic_path(s->path, sizeof s->path, "", name, "") < 0 ||
	    topic_path(s->wake_path, sizeof s->wake_path, ".", name, ".wake") < 0) {
		free(s);
		return NULL;
	}
	(void)attach(s); /* absent or failing: reported by the first read */
	return s;
}

void hg_sub_close(hg_sub *s)
{
	if (!s)
		return;
	if (s->h)
		munmap(s->h, s->map_len);
	if (s->wake)
		munmap(s->wake, WAKE_BYTES);
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

/* HG_MSG if attached; else what the read returns. */
static enum hg_result ensure_attached(hg_sub *s)
{
	if (s->h || attach(s) == 0)
		return HG_MSG;
	return errno == ENOENT ? HG_ABSENT : HG_ERROR;
}

enum hg_result hg_latest(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info)
{
	info->gap = 0;
	enum hg_result a = ensure_attached(s);
	if (a != HG_MSG)
		return a;
	for (int tries = 0;;) {
		if (buf_len < s->payload_bytes) {
			errno = EMSGSIZE;
			return HG_ERROR;
		}
		struct hg_header *h = s->h;
		uint32_t flags = atomic_load_explicit(&h->flags, memory_order_acquire);
		uint64_t n = atomic_load_explicit(&h->write_seq, memory_order_acquire);
		if (n <= s->last || n <= s->first) {
			int switched;
			enum hg_result r = at_end(s, flags, &switched);
			if (switched)
				continue;
			return r;
		}
		struct hg_slot *sl = slot_at(s, n);
		uint64_t s1 = atomic_load_explicit(&sl->seq, memory_order_acquire);
		if (s1 == 2 * n) {
			copy_slot(sl, buf, s->payload_bytes, info);
			atomic_thread_fence(memory_order_acquire);
			uint64_t s2 = atomic_load_explicit(&sl->seq, memory_order_relaxed);
			if (s2 == s1) {
				s->last = n;
				info->seq = n;
				return HG_MSG;
			}
		}
		if (++tries >= 8)
			return HG_BUSY;
	}
}

enum hg_result hg_next(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info)
{
	info->gap = 0;
	enum hg_result a = ensure_attached(s);
	if (a != HG_MSG)
		return a;
	for (;;) {
		if (buf_len < s->payload_bytes) {
			errno = EMSGSIZE;
			return HG_ERROR;
		}
		struct hg_header *h = s->h;
		uint64_t k = (uint64_t)s->mask + 1;
		uint32_t flags = atomic_load_explicit(&h->flags, memory_order_acquire);
		uint64_t W = atomic_load_explicit(&h->write_seq, memory_order_acquire);
		if (s->last < s->first) {
			/* Messages before this file's first are in no file we can read. */
			info->gap += s->first - s->last;
			s->last = s->first;
		}
		uint64_t w = s->last + 1;
		if (w > W) {
			int switched;
			enum hg_result r = at_end(s, flags, &switched);
			if (switched)
				continue;
			return r;
		}
		if (W - w >= k - 1) {
			/* w <= W-K+1: gone, or in the slot the writer fills next. */
			uint64_t nw = W - k + 2;
			info->gap += nw - w;
			w = nw;
			s->last = w - 1;
		}
		struct hg_slot *sl = slot_at(s, w);
		uint64_t s1 = atomic_load_explicit(&sl->seq, memory_order_acquire);
		if (s1 == 2 * w) {
			copy_slot(sl, buf, s->payload_bytes, info);
			atomic_thread_fence(memory_order_acquire);
			uint64_t s2 = atomic_load_explicit(&sl->seq, memory_order_relaxed);
			if (s2 == s1) {
				s->last = w;
				info->seq = w;
				return HG_MSG;
			}
			continue; /* overwritten while copying: lost on the next pass */
		}
		/* Odd (being rewritten, or its writer died mid-publish) or newer:
		 * message w is lost. Never retry the same target. */
		info->gap++;
		s->last = w;
	}
}

#define POLL_ABSENT_NS 10000000u        /* no futex to wait on for an absent topic */
#define POLL_REOPEN_MIN_NS 1000000u     /* replaced, the new file not renamed in yet: */
#define POLL_REOPEN_MAX_NS 1000000000u  /* retry after 1 ms, backing off to 1 s */

/* Set by hg_wait_interrupt(). Process-private: hg_wait() also sleeps on it. */
static _Atomic uint32_t interrupt_word;

void hg_wait_interrupt(void)
{
	int e = errno;
	atomic_store_explicit(&interrupt_word, 1, memory_order_seq_cst);
	syscall(SYS_futex, &interrupt_word, FUTEX_WAKE | HG_FUTEX_PRIVATE_FLAG, INT_MAX, NULL, NULL, 0);
	errno = e;
}

static uint64_t min_poll(uint64_t poll_ns, uint64_t ns)
{
	return poll_ns && poll_ns < ns ? poll_ns : ns;
}

int hg_wait(hg_sub *const *subs, int n, int timeout_ms, unsigned char *ready)
{
	if (n <= 0 || n > HG_MAX_WAIT) {
		errno = EINVAL;
		return -1;
	}
	uint64_t deadline =
		timeout_ms < 0 ? UINT64_MAX : hg_now_ns() + (uint64_t)timeout_ms * 1000000u;
	struct hg_futex_waitv wv[1 + HG_MAX_WAIT];

	for (;;) {
		/* Taken before the sleep: an interrupt after this changes the
		 * word the sleep compares with 0, so it fails at once or is woken. */
		if (atomic_exchange_explicit(&interrupt_word, 0, memory_order_seq_cst)) {
			errno = EINTR;
			return -1;
		}
		wv[0] = (struct hg_futex_waitv){ .val = 0,
						 .uaddr = (uint64_t)(uintptr_t)&interrupt_word,
						 .flags = HG_FUTEX_32_PRIVATE };
		uint64_t poll_ns = 0;
		int nready = 0, nw = 1;

		for (int i = 0; i < n; i++) {
			hg_sub *s = subs[i];
			ready[i] = 0;
			if (!s->h && attach(s) < 0) {
				if (errno == ENOENT)
					poll_ns = min_poll(poll_ns, POLL_ABSENT_NS);
				else
					ready[i] = 1; /* its read reports the error */
				continue;
			}
			if (!s->reopen_since)
				continue;
			uint32_t f = atomic_load_explicit(&s->h->flags, memory_order_acquire);
			uint32_t word = atomic_load_explicit(&s->h->futex_word, memory_order_acquire);
			if (!(f & HG_FLAG_REPLACED)) {
				s->reopen_since = 0; /* the replacing writer backed out */
				continue;
			}
			if (word != s->reopen_word) {
				/* Announced again (another writer starting): retry soon. */
				s->reopen_word = word;
				s->reopen_since = hg_now_ns();
			}
			if (reopen(s, f) != 0) {
				ready[i] = 1; /* switched, or its read reports the error */
			} else {
				uint64_t age = hg_now_ns() - s->reopen_since;
				poll_ns = min_poll(poll_ns, age < POLL_REOPEN_MIN_NS ? POLL_REOPEN_MIN_NS
							: age > POLL_REOPEN_MAX_NS ? POLL_REOPEN_MAX_NS
										   : age);
			}
		}
		for (int i = 0; i < n; i++) {
			if (!subs[i]->h)
				continue;
			struct hg_header *h = subs[i]->h;
			wv[nw].val = atomic_load_explicit(&h->futex_word, memory_order_acquire);
			wv[nw].uaddr = (uint64_t)(uintptr_t)&h->futex_word;
			wv[nw].flags = HG_FUTEX_32;
			wv[nw].reserved = 0;
			nw++;
		}
		for (int i = 0; i < n; i++)
			if (subs[i]->h)
				atomic_store_explicit(&subs[i]->wake->wait_flag, 1, memory_order_seq_cst);
		/* The flag store above and the write_seq load below, against
		 * hg_publish()'s write_seq store and flag exchange, are a
		 * store-buffering pattern: all four are seq_cst, so at least one
		 * side sees the other's store. (An acquire load here could be
		 * done before the flag store, e.g. LDAPR on ARMv8.3+.) */
		for (int i = 0; i < n; i++) {
			hg_sub *s = subs[i];
			if (s->h && !ready[i]) {
				uint64_t w = atomic_load_explicit(&s->h->write_seq, memory_order_seq_cst);
				uint32_t f = atomic_load_explicit(&s->h->flags, memory_order_seq_cst) &
					     (HG_FLAG_CLOSED | HG_FLAG_DIED | HG_FLAG_REPLACED);
				if (w > s->last || f != s->seen_flags)
					ready[i] = 1;
			}
			nready += ready[i];
		}
		if (nready)
			return nready;

		uint64_t now = hg_now_ns();
		if (now >= deadline)
			return 0;
		uint64_t until = deadline;
		if (poll_ns && now + poll_ns < until)
			until = now + poll_ns;
		struct timespec ts = { .tv_sec = (time_t)(until / 1000000000u),
				       .tv_nsec = (long)(until % 1000000000u) };
		long r = syscall(SYS_futex_waitv, wv, (unsigned)nw, 0u,
				 until == UINT64_MAX ? NULL : &ts, CLOCK_MONOTONIC);
		if (r < 0 && errno == ETIMEDOUT && until == deadline) {
			/* A timeout, even if news came in meanwhile: a wake-up that
			 * never came must show (bench lost_wake counts these). */
			memset(ready, 0, (size_t)n);
			return 0;
		}
		if (r < 0 && errno != EAGAIN && errno != ETIMEDOUT)
			return -1; /* EINTR included */
	}
}
