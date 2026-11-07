// SPDX-License-Identifier: LGPL-3.0-or-later
/* What the manager does to topic files, for the tests: built into the test
 * suites, never into the library. Everything takes a topic name under
 * $HANGAR_TOPIC_DIR and returns 0, or -1 with errno. */
#ifndef HG_MANAGER_STUB_H
#define HG_MANAGER_STUB_H

#include <stdint.h>
#include <sys/types.h>

/* A fresh, empty topic directory under base, made $HANGAR_TOPIC_DIR; the
 * path is written to out. */
int hg_stub_fresh_dir(const char *base, const char *tag, char *out, size_t len);
/* Removes every file of the current $HANGAR_TOPIC_DIR. */
int hg_stub_clear_dir(void);

/* Flag bit 1 (the writer died), with the wake a close makes. */
int hg_stub_mark_died(const char *name);
/* Flag bit 2 (replaced), with the wake, and no new file: what readers see
 * between the manager's flagging and its rename. */
int hg_stub_mark_replaced(const char *name);
int hg_stub_clear_replaced(const char *name);
/* Increments and wakes the directory word, as a manager does after a rename. */
int hg_stub_bump_dir(void);

/* Raw access for the layout tests. */
int hg_stub_read_header(const char *name, unsigned char out[4096]);
int hg_stub_read_slot(const char *name, uint32_t slot, unsigned char *out, size_t len);
/* Stores seq into slot's seq word: an odd value is what a writer that died
 * mid-publish leaves. */
int hg_stub_set_slot_seq(const char *name, uint32_t slot, uint64_t seq);
/* Stores a value into write_seq (seq_cst) without touching the slots. */
int hg_stub_set_write_seq(const char *name, uint64_t seq);

/* Inodes, to check that the wake files and .dir survive a replacement. */
int hg_stub_wake_inode(const char *name, ino_t *out);
int hg_stub_dir_inode(ino_t *out);
int hg_stub_topic_inode(const char *name, ino_t *out);
int hg_stub_unlink_topic(const char *name);

/* A sealed memfd holding text, as the manager hands a module its wiring.
 * Returns the descriptor. */
int hg_stub_memfd(const char *text);

/* The publish protocol taken apart, for the test that guards the order of
 * step 4 (docs/01 "Publishing"): the writer's steps 1 to 3 for the next
 * message with an all-zero payload; the two halves of step 4; the wake. And
 * the reader's side of hg_wait, step by step: read the futex word, set the
 * wake flag, sleep on a value. raw_waitv returns 0 if woken, ETIMEDOUT or
 * EAGAIN otherwise. */
int hg_stub_raw_body(const char *name);
int hg_stub_raw_exchange_flag(const char *name, uint32_t *was);
int hg_stub_raw_increment(const char *name);
int hg_stub_raw_wake(const char *name);
int hg_stub_raw_read_word(const char *name, uint32_t *out);
int hg_stub_raw_set_flag(const char *name);
int hg_stub_raw_waitv(const char *name, uint32_t val, int timeout_ms);

#endif
