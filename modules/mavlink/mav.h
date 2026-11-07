// SPDX-License-Identifier: GPL-3.0-or-later
/* MAVLink 2 framing, CRC and signing, written from the specification
 * (docs/10-mavlink.md: no pymavlink or libmavlink at run time). The frame
 * format and the CRC are those of https://mavlink.io/en/guide/serialization.html
 * (read 2026-11-07):
 *
 *   0      STX 0xFD
 *   1      len            payload bytes, trailing zeros trimmed (never the first)
 *   2      incompat_flags bit 0 (MAV_IFLAG_SIGNED): a 13-byte signature follows
 *   3      compat_flags
 *   4      seq
 *   5      sysid
 *   6      compid
 *   7..9   msgid, 24 bits little-endian
 *   10..   payload
 *   +0,+1  CRC-16/MCRF4XX over bytes 1..end of payload, then CRC_EXTRA, low byte first
 *   +2..   signature: link id (1), timestamp (6, LE, 10 us since 2015-01-01), 6 bytes
 *
 * The CRC is the X.25 variant of mavlink's checksum.h (init 0xffff; tmp = b ^
 * (crc & 0xff); tmp ^= tmp << 4; crc = crc >> 8 ^ tmp << 8 ^ tmp << 3 ^ tmp >> 4).
 * Signing follows https://mavlink.io/en/guide/message_signing.html (read the
 * same day): signature = sha256(key[32] || header || payload || crc ||
 * link_id || timestamp)[0..5]; a stream is (sysid, compid, link id) and its
 * timestamps must rise; a packet more than a minute (6 000 000 units) behind
 * the local clock is refused, and so is the first packet of a stream that far
 * behind. The per-message structs, lengths and CRC_EXTRA values come from
 * mavlink_gen.h (tools/mavgen). */
#ifndef HANGAR_MAV_H
#define HANGAR_MAV_H

#include <stddef.h>
#include <stdint.h>

#define MAV_STX 0xFDu
#define MAV_IFLAG_SIGNED 0x01u
#define MAV_HEADER_LEN 10u
#define MAV_CRC_LEN 2u
#define MAV_SIGNATURE_LEN 13u
#define MAV_MAX_PAYLOAD 255u
#define MAV_MAX_FRAME (MAV_HEADER_LEN + MAV_MAX_PAYLOAD + MAV_CRC_LEN + MAV_SIGNATURE_LEN)
#define MAV_TIMESTAMP_EPOCH_S 1420070400ull   /* 2015-01-01T00:00:00Z as Unix time */
#define MAV_TIMESTAMP_SLACK 6000000ull        /* one minute in 10 us units */
#define MAV_STREAMS 16

struct mav_frame {
	int valid;             /* 0: rejected; reason says why */
	uint8_t len, incompat, compat, seq, sysid, compid;
	uint32_t msgid;
	uint8_t payload[MAV_MAX_PAYLOAD];
	uint16_t crc;
	int is_signed;
	uint8_t link_id;
	uint64_t timestamp;
	uint8_t signature[6];
	size_t frame_len;      /* bytes the frame took */
};

struct mav_stream {
	int used;
	uint8_t sysid, compid, link_id;
	uint64_t timestamp;
	uint64_t last_seen;    /* for replacement */
};

/* Signing state of one link. With `enabled`, every frame sent is signed and
 * every frame received must be signed and verify; otherwise frames go out
 * unsigned and signed ones arriving are accepted without a check (there is no
 * key to check them with). */
struct mav_signing {
	int enabled;
	uint8_t key[32];
	uint8_t link_id;
	uint64_t last_sent;    /* the last timestamp sent: the next is greater */
	int accept_unsigned;   /* with enabled: also take unsigned frames (never in hangar's modules) */
	struct mav_stream streams[MAV_STREAMS];
	uint64_t seen;         /* frames counted by the verifier */
	uint64_t rejected;     /* frames the verifier refused */
	uint64_t fake_now;     /* tests: a fixed local timestamp instead of the clock, when non-zero */
};

uint16_t mav_crc_accumulate(uint8_t b, uint16_t crc);
uint16_t mav_crc(const uint8_t *p, size_t n, uint16_t crc);

/* The signing timestamp: 10 us units since 2015-01-01 UTC, from CLOCK_REALTIME. */
uint64_t mav_timestamp_now(const struct mav_signing *s);
/* Reads a key file: 32 raw bytes, or 64 hex digits (blanks and a newline
 * allowed). 0, or -1 with one line in err. */
int mav_signing_load_key(struct mav_signing *s, const char *path, char *err, size_t errlen);

/* Builds one frame into out (MAV_MAX_FRAME bytes is always enough): the
 * payload's trailing zeros trimmed, the CRC appended, and the signature when
 * sign is enabled. Returns the frame's length, or 0 if cap is too small or
 * full_len is 0 or over 255. */
size_t mav_encode(uint8_t *out, size_t cap, uint8_t seq, uint8_t sysid, uint8_t compid, uint32_t msgid,
		  const uint8_t *payload, size_t full_len, uint8_t crc_extra, struct mav_signing *sign);

/* Decodes the frame at the start of buf. Returns the bytes consumed: a whole
 * frame (out->valid says whether it passed its checks; reason says why not),
 * or 1 for a byte that does not start a frame, or 0 when len holds less than
 * the frame needs (a truncated datagram: the caller drops it). An unknown
 * message id cannot be CRC-checked and is rejected. verify may be NULL. */
size_t mav_decode(const uint8_t *buf, size_t len, struct mav_frame *out, struct mav_signing *verify,
		  const char **reason);

/* The 6-byte signature of frame bytes [0, n) (header, payload, CRC) under key,
 * link id and timestamp, into sig. */
void mav_sign(const uint8_t *key, const uint8_t *frame, size_t n, uint8_t link_id, uint64_t timestamp,
	      uint8_t sig[6]);

#endif
