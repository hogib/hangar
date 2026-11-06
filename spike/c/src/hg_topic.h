/* hangar topics: a subset of docs/01-topics.md (spike/SPEC.md section 1). */
#ifndef HG_TOPIC_H
#define HG_TOPIC_H

#include <stddef.h>
#include <stdint.h>

/* Results, as in docs/01-topics.md "Reading". */
enum hg_result {
	HG_MSG = 0,
	HG_NONE,
	HG_ABSENT,
	HG_TIMEOUT,
	HG_CLOSED,
	HG_BUSY,
	HG_ERROR,
};

/* Header flags. */
#define HG_FLAG_CLOSED   1u
#define HG_FLAG_DIED     2u
#define HG_FLAG_REPLACED 4u

#define HG_NAME_MAX 47
#define HG_MAX_WAIT 126

struct hg_input {
	uint32_t topic_id;
	uint32_t flags;
	uint64_t seq;
};

struct hg_info {
	uint64_t seq;
	uint64_t stamp_ns;
	uint64_t publish_ns;
	uint64_t gap;        /* messages skipped by hg_next() in this call */
	uint32_t n_inputs;
	uint32_t flags;
	struct hg_input inputs[4];
};

typedef struct hg_pub hg_pub;
typedef struct hg_sub hg_sub;

/* Writer. Returns NULL with errno set on failure. */
hg_pub *hg_pub_open(const char *name, uint32_t payload_bytes, uint32_t slot_count);
/* Publishes one message. len must equal the topic's payload_bytes; -1 with
 * errno EINVAL otherwise (nothing is published). */
int hg_publish(hg_pub *p, const void *payload, size_t len, uint64_t stamp_ns);
/* Sequence number the next hg_publish() will use. */
uint64_t hg_pub_next_seq(const hg_pub *p);
void hg_pub_close(hg_pub *p);

/* Reader. Never blocks; NULL only for an invalid name, a missing
 * $HANGAR_TOPIC_DIR or no memory. An absent topic attaches on a later call.
 * HG_ABSENT means only that no file is at the path; a file that cannot be
 * opened or mapped, or is not a compatible topic (errno EPROTO), makes
 * hg_latest()/hg_next() return HG_ERROR with errno set. */
hg_sub *hg_sub_open(const char *name);
void hg_sub_close(hg_sub *s);
/* The buffer must hold hg_sub_payload_bytes(); HG_ERROR if it is shorter.
 * HG_CLOSED also when a replacing writer flagged the file but no new file
 * appeared at the path within a second. */
enum hg_result hg_latest(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info);
enum hg_result hg_next(hg_sub *s, void *buf, size_t buf_len, struct hg_info *info);
/* Waits until any of subs[0..n) may have news. ready[i] is set to 1 for those
 * that may; returns how many, 0 on timeout, -1 with errno on error
 * (EINTR when a signal interrupted it, or hg_wait_interrupt() was called).
 * A wait the kernel timed out returns 0 even if news came in meanwhile.
 * timeout_ms < 0 waits forever. A subscription whose read would report
 * HG_ERROR is ready at once. */
int hg_wait(hg_sub *const *subs, int n, int timeout_ms, unsigned char *ready);
/* Makes the hg_wait() under way in this process, or else the next one, return
 * -1 with errno EINTR. Async-signal-safe: call it from a signal handler that
 * sets the flag the caller checks around hg_wait(), and no signal is missed
 * between the check and the sleep. */
void hg_wait_interrupt(void);
/* 0 while absent. */
uint32_t hg_sub_payload_bytes(const hg_sub *s);
/* Times the reader switched to a replacing file. */
uint64_t hg_sub_reopens(const hg_sub *s);

/* Shared helpers. */
int hg_valid_name(const char *name);             /* 1 if a valid topic name */
uint64_t hg_now_ns(void);                        /* CLOCK_MONOTONIC */
uint32_t hg_fnv1a32(const char *s);
int hg_futex_wake_all(void *addr);               /* shared FUTEX_WAKE */
/* Marks the topic file at path as died (bit 1), as the manager does when its
 * writer exits. 0 on success, -1 with errno. */
int hg_topic_mark_died(const char *path);

#endif
