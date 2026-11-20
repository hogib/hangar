// SPDX-License-Identifier: GPL-3.0-or-later
/* What the two MAVLink harnesses share: the input's framing, a topic directory
 * of the process's own, and the bytes on their way to mav_link_receive.
 *
 * Input: one mode byte, then the bytes that arrive on the link.
 *
 *   bit 0  the link is a serial line (a byte stream; else UDP datagrams)
 *   bit 1  signing is on: frames must be signed with the harness's key
 *   bit 2  repair: before the bytes are handed over, every frame in them gets
 *          the CRC its length and message id need, and a signed frame the
 *          signature of the harness's key over its own link id and timestamp.
 *          Mutated bytes almost never keep a CRC-16 or a SHA-256 signature, so
 *          without this the handlers would see nothing but the seeds; with
 *          it, a frame's payload, length, ids and timestamp are the fuzzer's.
 *          The other inputs test the checks themselves.
 *   bit 3  vehicle: any system and component id is taken for ArduSub's
 *   bits 4-7  the bytes are handed over in pieces of this many * 16 + 7
 *          bytes (0: all at once): frames cut between reads or datagrams
 *
 * mav_decode is also run over the raw bytes on its own, with and without a
 * verifier, as tools/sim/pilot.c and the UDP loop call it. */
#ifndef HANGAR_FUZZ_MAVLINK_H
#define HANGAR_FUZZ_MAVLINK_H

#include "fuzz.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "mav.h"
#include "mavlink.h"
#include "mavlink_gen.h"

#define FZM_SERIAL 1u
#define FZM_SIGNING 2u
#define FZM_REPAIR 4u
#define FZM_ANY_ID 8u

/* 2026-01-01 in the signing clock: frames a minute around it pass the age check. */
#define FZM_NOW 34689600000000ull

static char fzm_dir[256];

static inline void fzm_cleanup(void)
{
	DIR *d = opendir(fzm_dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char p[600];
		snprintf(p, sizeof p, "%s/%s", fzm_dir, e->d_name);
		unlink(p);
	}
	if (d)
		closedir(d);
	rmdir(fzm_dir);
}

static inline void fzm_topic_dir(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(fzm_dir, sizeof fzm_dir, "%s/hg-fuzz-mavlink-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(fzm_dir))
		abort();
	setenv("HANGAR_TOPIC_DIR", fzm_dir, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_PEER_TOPIC_DIR");
	atexit(fzm_cleanup);
}

static inline void fzm_key(struct mav_signing *s)
{
	memset(s, 0, sizeof *s);
	s->enabled = 1;
	memset(s->key, 0x4b, sizeof s->key);
	s->link_id = 1;
	s->fake_now = FZM_NOW;
}

/* Gives every frame found in p[0..n) a correct CRC, and a signed one a correct
 * signature. A frame of an unknown id or cut by the end is left as it is. */
static inline void fzm_repair(uint8_t *p, size_t n, const struct mav_signing *key)
{
	size_t off = 0;
	while (off < n) {
		if (p[off] != MAV_STX || n - off < MAV_HEADER_LEN + MAV_CRC_LEN) {
			off++;
			continue;
		}
		uint8_t *f = p + off;
		size_t len = f[1], is_signed = f[2] & MAV_IFLAG_SIGNED;
		size_t total = MAV_HEADER_LEN + len + MAV_CRC_LEN + (is_signed ? MAV_SIGNATURE_LEN : 0);
		uint32_t msgid = (uint32_t)f[7] | (uint32_t)f[8] << 8 | (uint32_t)f[9] << 16;
		const struct mav_msg_info *mi = mav_msg_info(msgid);
		if (!mi || total > n - off) {
			off++;
			continue;
		}
		uint16_t crc = mav_crc(f + 1, MAV_HEADER_LEN - 1 + len, 0xffff);
		crc = mav_crc_accumulate(mi->crc_extra, crc);
		f[MAV_HEADER_LEN + len] = (uint8_t)crc;
		f[MAV_HEADER_LEN + len + 1] = (uint8_t)(crc >> 8);
		if (is_signed) {
			size_t s = MAV_HEADER_LEN + len + MAV_CRC_LEN;
			uint64_t ts = 0;
			for (int i = 0; i < 6; i++)
				ts |= (uint64_t)f[s + 1 + i] << (8 * i);
			mav_sign(key->key, f, s, f[s], ts, f + s + 7);
		}
		off += total;
	}
}

/* mav_decode over the bytes as they came, stepping as the UDP loop steps. */
static inline void fzm_decode_alone(const uint8_t *data, size_t size, int with_key)
{
	struct mav_signing s;
	fzm_key(&s);
	size_t off = 0;
	while (off < size) {
		struct mav_frame f;
		const char *why = NULL;
		size_t used = mav_decode(data + off, size - off, &f, with_key ? &s : NULL, &why);
		FZ_CHECK(why != NULL);
		FZ_CHECK(used <= size - off);
		if (used == 0)
			break;
		if (f.valid) {
			const struct mav_msg_info *mi = mav_msg_info(f.msgid);
			FZ_CHECK(mi && f.len <= mi->len && f.frame_len == used);
		}
		off += used;
	}
	FZ_CHECK(s.rejected <= s.seen);
}

/* The module's link over a socket pair, the input written to the far end in
 * the pieces the mode asks for, each followed by one mav_link_receive. */
static inline void fzm_feed(struct mav_link *l, unsigned mode, const uint8_t *data, size_t size,
		     void (*fn)(void *, const struct mav_frame *), uint64_t *now)
{
	int sp[2];
	int serial = (mode & FZM_SERIAL) != 0;
	if (socketpair(AF_UNIX, (serial ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sp) < 0)
		abort();
	memset(l, 0, sizeof *l);
	l->fd = sp[0];
	l->tlog_fd = -1;
	l->serial = serial;
	l->have_peer = 1; /* answers are sent: into the pair, which nobody reads */
	l->sysid = 255;
	l->compid = 190;
	snprintf(l->line_state, sizeof l->line_state, "open");
	if (mode & FZM_SIGNING)
		fzm_key(&l->signing);
	uint8_t *copy = malloc(size ? size : 1);
	if (!copy)
		abort();
	memcpy(copy, data, size);
	if (mode & FZM_REPAIR) {
		struct mav_signing k;
		fzm_key(&k);
		fzm_repair(copy, size, &k);
	}
	size_t piece = (mode >> 4) ? (size_t)(mode >> 4) * 16 + 7 : size;
	for (size_t off = 0; off < size;) {
		size_t k = size - off < piece ? size - off : piece;
		if (send(sp[1], copy + off, k, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
			break; /* the pair's buffer is full of the module's answers: enough */
		off += k;
		(void)mav_link_receive(l, fn, now);
		(void)mav_link_service(l, *now);
		FZ_CHECK(l->rx_len <= sizeof l->rx);
		FZ_CHECK(l->tx_count >= 0 && l->tx_count <= MAV_TXQ);
		*now += 10000000;
	}
	free(copy);
	FZ_NOTE("link: %llu received, %llu rejected (%s), %llu unknown, %llu garbage, %llu sent", (unsigned long long)l->received,
		(unsigned long long)l->rejected, l->last_reject, (unsigned long long)l->unknown,
		(unsigned long long)l->garbage, (unsigned long long)l->sent);
	if (l->fd >= 0)
		close(l->fd); /* -1: the link took its line for gone and closed its end itself */
	close(sp[1]);
}

#endif
