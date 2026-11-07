// SPDX-License-Identifier: LGPL-3.0-or-later
/* hangar topics: shared-memory ring files, seqlocked slots, futex wake-ups,
 * provenance. The contract is docs/01-topics.md; the wiring and the open
 * protocol are docs/03-manager.md. Stage 1: standalone mode, no manager. */
#ifndef HG_TOPIC_H
#define HG_TOPIC_H

#include <stddef.h>
#include <stdint.h>

#include "hg_type.h"
#include "hg_wiring.h"

/* Results, as in docs/01-topics.md "Reading". HG_ERROR is the spike's
 * addition, now design: a file that is not a compatible topic, a buffer too
 * short, a type that does not match. errno says which. */
enum hg_result {
	HG_MSG = 0,
	HG_NONE,
	HG_ABSENT,
	HG_CLOSED,
	HG_BUSY,
	HG_GONE,
	HG_ERROR,
};

/* Header flags. */
#define HG_FLAG_CLOSED 1u
#define HG_FLAG_DIED 2u
#define HG_FLAG_REPLACED 4u

/* Slot flags. */
#define HG_SLOT_STALE 1u      /* computed from stale inputs */
#define HG_SLOT_DECIMATED 2u  /* a rate-limited mirror skipped seqs before this one */

/* Reference flags (slot inputs[].flags). */
#define HG_REF_REUSED 1u
#define HG_REF_STALE 2u
#define HG_REF_ABSENT 4u

/* hg_publish_with() flags. */
#define HG_STALE 1u
#define HG_NO_REFS 2u

/* hg_wait() ready set. */
#define HG_READY_TOPICS 1u      /* a topic has news */
#define HG_READY_INTERRUPT 2u   /* hg_wait_interrupt() was called, or a signal arrived */

#define HG_NAME_MAX 47
#define HG_MAX_REFS 4
#define HG_MAX_WAIT 127         /* topics by name in one hg_wait(): 128 futexes, one for .dir */
#define HG_MAX_WAIT_FD 126      /* with hg_wait_fd(): one more for the helper word */

struct hg_input {
	uint32_t topic_id;
	uint32_t flags;
	uint64_t seq;
};

struct hg_info {
	enum hg_result result;
	uint64_t seq;
	uint64_t stamp_ns;
	uint64_t publish_ns;
	uint64_t gap;          /* messages lost (hg_next) */
	uint64_t skipped;      /* seqs a rate-limited mirror skipped on purpose (hg_next) */
	uint64_t age_ns;       /* now - the topic's last_publish_ns at the read; 0 if none yet */
	uint32_t epoch;        /* changes when a mirrored topic's sender rebooted */
	uint32_t n_inputs;
	uint32_t flags;        /* slot flags */
	struct hg_input inputs[HG_MAX_REFS];
};

/* A copy of the header fields that never change, for introspection. */
struct hg_topic_meta {
	char name[48];
	char writer[32];
	uint64_t generation;
	uint64_t type_hash;
	uint64_t first_seq;
	uint64_t stale_after_ns;
	uint64_t created_real_ns;
	uint32_t slot_count;
	uint32_t payload_bytes;
	uint32_t topic_id;
	uint32_t rate_mhz;
	int32_t writer_pid;
	unsigned char origin_boot[16];
};

typedef struct hg_pub hg_pub;
typedef struct hg_sub hg_sub;

/* ---- Module ------------------------------------------------------------ */

/* Reads the wiring, from the descriptor $HANGAR_WIRING or the file
 * $HANGAR_WIRING_FILE; with neither the module runs unwired: any topic may be
 * opened, slots and rates come from hg_pub_opts. In stage 1 the module page
 * is a no-op (no manager to hand it over). 0, or -1 with errno; a wiring that
 * does not parse is EINVAL and hg_init_error() says why. */
int hg_init(void);
const char *hg_init_error(void);
/* The wiring in force, or NULL when unwired. */
const struct hg_wiring *hg_wiring(void);
/* The module's name: the wiring's, else $HANGAR_MODULE, else "standalone". */
const char *hg_module_name(void);
/* The module page (docs/03-manager.md): no-ops in standalone mode. */
void hg_ready(void);
void hg_alive(void);
void hg_status(const char *line);

/* ---- Writer ------------------------------------------------------------ */

/* What an unwired module gives in place of the manifest; ignored for a topic
 * the wiring lists. */
struct hg_pub_opts {
	uint32_t slots;            /* 0: 16 (4 for a frame type) */
	uint32_t rate_mhz;         /* 0: event topic */
	uint64_t stale_after_ns;   /* 0 with a rate: three periods */
};

/* Creates the topic (standalone mode: by the manager's steps, in
 * $HANGAR_TOPIC_DIR) and maps it. NULL with errno: EINVAL for a bad name or
 * type, EACCES for a topic the wiring does not list as this module's output,
 * EPROTOTYPE when the wiring declares another type, ENOSPC and friends from
 * the file system. An existing file of that name is replaced and its
 * write_seq continued. */
hg_pub *hg_pub_open(const char *name, const struct hg_type *type, const struct hg_pub_opts *opts);
/* The bridge's open of a mirrored topic: the sender's boot id and the first
 * seq it will publish. An existing file with the same origin_boot has its
 * write_seq continued; any other is replaced with first_seq = first - 1. */
hg_pub *hg_pub_open_mirror(const char *name, const struct hg_type *type,
			   const unsigned char origin_boot[16], uint64_t first_seq,
			   const struct hg_pub_opts *opts);
/* Publishes one message with a reference per Inputs topic. len must equal
 * the payload size: -1 with EINVAL otherwise, nothing published. */
int hg_publish(hg_pub *p, const void *payload, size_t len, uint64_t stamp_ns);
/* References chosen per message: each must be a message of an Inputs or
 * Sources topic (-1, EINVAL otherwise), at most 4 in all with the Inputs; a
 * reference to an Inputs topic overrides its default. flags: HG_STALE,
 * HG_NO_REFS. */
int hg_publish_with(hg_pub *p, const void *payload, size_t len, uint64_t stamp_ns,
		    unsigned flags, const struct hg_input *refs, int n_refs);
/* The bridge only (a pub opened with hg_pub_open_mirror; EPERM otherwise):
 * the sender's seq, slot flags and references, published unchanged. A seq at
 * or below write_seq is refused (-1, ERANGE). */
int hg_publish_seq(hg_pub *p, uint64_t seq, uint32_t slot_flags, const void *payload, size_t len,
		   uint64_t stamp_ns, const struct hg_input *refs, int n_refs);
/* Sequence number the next hg_publish() will use. */
uint64_t hg_pub_next_seq(const hg_pub *p);
uint32_t hg_pub_topic_id(const hg_pub *p);
/* Sets flag bit 0, wakes the readers, unmaps. */
void hg_pub_close(hg_pub *p);

/* ---- Reader ------------------------------------------------------------ */

enum hg_start { HG_NEWEST = 0, HG_OLDEST = 1 };

/* Never blocks. NULL only for an invalid name, a name the wiring does not
 * list (EACCES), a missing $HANGAR_TOPIC_DIR or no memory. An absent topic
 * attaches on a later call. type NULL: any type (introspect with
 * hg_sub_meta/hg_sub_schema); given, a file of another type reads HG_ERROR
 * with errno EPROTOTYPE. A name holding '*' or '?' opens a glob
 * subscription: hg_wait() polls its matches (hg_glob_count/hg_glob_at). */
hg_sub *hg_sub_open(const char *name, const struct hg_type *type);
/* HG_OLDEST starts hg_next at the oldest message still in the ring instead
 * of the newest. */
hg_sub *hg_sub_open_from(const char *name, const struct hg_type *type, enum hg_start from);
/* Opens one of the module's own outputs for reading: a topic the wiring
 * lists as a pub, not a sub (a restarted server reads its own .rep topic
 * for the replies it already gave, docs/04 "While the server restarts").
 * EACCES for a topic that is neither. */
hg_sub *hg_sub_open_own(const char *name, const struct hg_type *type, enum hg_start from);
void hg_sub_close(hg_sub *s);

/* The buffer must hold the payload: HG_ERROR with EMSGSIZE if shorter. */
enum hg_result hg_latest(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info);
enum hg_result hg_next(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info);
/* Random access: HG_GONE when seq is not in the ring. Neither moves hg_next. */
enum hg_result hg_read_seq(hg_sub *s, uint64_t seq, void *buf, size_t buf_len,
			   struct hg_info *info);
enum hg_result hg_find_stamp(hg_sub *s, uint64_t t_ns, void *buf, size_t buf_len,
			     struct hg_info *info);
/* Makes the message info describes the reference the next publishes record
 * for this input, instead of the most recent one, until the next hg_use. */
void hg_use(hg_sub *s, const struct hg_info *info);

/* Waits until a topic has news: a message above the last one returned, or
 * header flags that differ from those last reported (a close, a death, a
 * replacement, each once), or an absent topic that appeared. Returns the
 * ready set: HG_READY_TOPICS, HG_READY_INTERRUPT, or 0 on timeout (even if
 * news came in meanwhile: a wake-up that never came must show). timeout_ms
 * < 0 waits forever. ready[i], if ready is given, is 1 for each topic with
 * news. (unsigned)-1 with errno on a bad argument (EINVAL: n over
 * HG_MAX_WAIT or 0). */
unsigned hg_wait_ready(hg_sub *const *subs, int n, int timeout_ms, unsigned char *ready);
unsigned hg_wait(hg_sub *const *subs, int n, int timeout_ms);
/* Makes the hg_wait() under way in this process, or else the next one,
 * return HG_READY_INTERRUPT. Async-signal-safe. */
void hg_wait_interrupt(void);

/* A helper thread waits on the topics' futexes and signals the returned
 * eventfd, which the module puts in its own epoll set. Only this thread; the
 * module reads its topics itself, without blocking. At most HG_MAX_WAIT_FD
 * topics (-1, E2BIG). The set follows the topics as they attach and reopen.
 * -1 with errno on failure. */
int hg_wait_fd(hg_sub *const *subs, int n);
/* Stops the helper and closes the eventfd. */
void hg_wait_fd_close(void);

/* Glob subscriptions: rescans the directory for new matches (hg_wait does it
 * for the module; a module on hg_wait_fd calls it itself, every 10 ms at
 * most) and gives the matches, each an ordinary subscription. */
int hg_glob_poll(hg_sub *g);
int hg_glob_count(const hg_sub *g);
hg_sub *hg_glob_at(const hg_sub *g, int i);

/* Introspection. 0 while absent for the first two. */
uint32_t hg_sub_payload_bytes(const hg_sub *s);
int hg_sub_meta(const hg_sub *s, struct hg_topic_meta *out);
/* The schema text in the mapped header, and its length; NULL while absent. */
const char *hg_sub_schema(const hg_sub *s, uint32_t *len);
const char *hg_sub_name(const hg_sub *s);
uint32_t hg_sub_topic_id(const hg_sub *s);
/* Times the reader switched to a replacing file. */
uint64_t hg_sub_reopens(const hg_sub *s);
/* until the bridge: 1 if the subscription reads the other root's file through
 * the same-host shim ($HANGAR_PEER_TOPIC_DIR, modules/PHASE1.md): the call
 * library counts such a request topic as mirrored from the other side. */
int hg_sub_is_peer(const hg_sub *s);

/* ---- Shared helpers ---------------------------------------------------- */

int hg_valid_name(const char *name);  /* 1 if a valid topic name */
uint64_t hg_now_ns(void);             /* CLOCK_MONOTONIC */
uint32_t hg_topic_id(const char *name);
int hg_futex_wake_all(void *addr);    /* shared FUTEX_WAKE */
/* "<dir>/<prefix><name><suffix>" under $HANGAR_TOPIC_DIR; -1 with errno
 * EINVAL if it is not set. */
int hg_topic_path(char *out, size_t len, const char *prefix, const char *name,
		  const char *suffix);

#endif
