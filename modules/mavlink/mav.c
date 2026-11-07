// SPDX-License-Identifier: GPL-3.0-or-later
/* MAVLink 2 framing, CRC and signing: see mav.h for the specification each
 * part follows. */
#define _GNU_SOURCE
#include "mav.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mavlink_gen.h"
#include "sha256.h"

uint16_t mav_crc_accumulate(uint8_t b, uint16_t crc)
{
	/* CRC-16/MCRF4XX as mavlink's checksum.h writes it (X25_INIT_CRC 0xffff). */
	uint8_t tmp = (uint8_t)(b ^ (crc & 0xff));
	tmp = (uint8_t)(tmp ^ (tmp << 4));
	return (uint16_t)((crc >> 8) ^ ((uint16_t)tmp << 8) ^ ((uint16_t)tmp << 3) ^ (tmp >> 4));
}

uint16_t mav_crc(const uint8_t *p, size_t n, uint16_t crc)
{
	for (size_t i = 0; i < n; i++)
		crc = mav_crc_accumulate(p[i], crc);
	return crc;
}

uint64_t mav_timestamp_now(const struct mav_signing *s)
{
	if (s && s->fake_now)
		return s->fake_now;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	if ((uint64_t)ts.tv_sec < MAV_TIMESTAMP_EPOCH_S)
		return 0;
	return ((uint64_t)ts.tv_sec - MAV_TIMESTAMP_EPOCH_S) * 100000ull + (uint64_t)ts.tv_nsec / 10000u;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

int mav_signing_load_key(struct mav_signing *s, const char *path, char *err, size_t errlen)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	unsigned char buf[256];
	ssize_t n = read(fd, buf, sizeof buf);
	close(fd);
	if (n == 32) {
		memcpy(s->key, buf, 32);
		return 0;
	}
	/* 64 hex digits, blanks between and a newline after allowed. */
	int k = 0, hi = -1;
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t')
			continue;
		int v = hexval((char)buf[i]);
		if (v < 0 || k == 32) {
			snprintf(err, errlen, "%s: not a 32-byte key (raw, or 64 hex digits)", path);
			return -1;
		}
		if (hi < 0) {
			hi = v;
		} else {
			s->key[k++] = (uint8_t)(hi << 4 | v);
			hi = -1;
		}
	}
	if (k != 32 || hi >= 0) {
		snprintf(err, errlen, "%s: not a 32-byte key (raw, or 64 hex digits)", path);
		return -1;
	}
	return 0;
}

void mav_sign(const uint8_t *key, const uint8_t *frame, size_t n, uint8_t link_id, uint64_t timestamp,
	      uint8_t sig[6])
{
	struct sha256 h;
	uint8_t ts[6];
	for (int i = 0; i < 6; i++)
		ts[i] = (uint8_t)(timestamp >> (8 * i));
	sha256_init(&h);
	sha256_update(&h, key, 32);
	sha256_update(&h, frame, n);
	sha256_update(&h, &link_id, 1);
	sha256_update(&h, ts, 6);
	uint8_t out[32];
	sha256_final(&h, out);
	memcpy(sig, out, 6);
}

size_t mav_encode(uint8_t *out, size_t cap, uint8_t seq, uint8_t sysid, uint8_t compid, uint32_t msgid,
		  const uint8_t *payload, size_t full_len, uint8_t crc_extra, struct mav_signing *sign)
{
	if (full_len == 0 || full_len > MAV_MAX_PAYLOAD || msgid > 0xffffffu)
		return 0;
	size_t n = full_len;
	while (n > 1 && payload[n - 1] == 0)
		n--;
	int signed_ = sign && sign->enabled;
	size_t total = MAV_HEADER_LEN + n + MAV_CRC_LEN + (signed_ ? MAV_SIGNATURE_LEN : 0);
	if (cap < total)
		return 0;
	out[0] = MAV_STX;
	out[1] = (uint8_t)n;
	out[2] = signed_ ? MAV_IFLAG_SIGNED : 0;
	out[3] = 0;
	out[4] = seq;
	out[5] = sysid;
	out[6] = compid;
	out[7] = (uint8_t)msgid;
	out[8] = (uint8_t)(msgid >> 8);
	out[9] = (uint8_t)(msgid >> 16);
	memcpy(out + MAV_HEADER_LEN, payload, n);
	uint16_t crc = mav_crc(out + 1, MAV_HEADER_LEN - 1 + n, 0xffff);
	crc = mav_crc_accumulate(crc_extra, crc);
	size_t p = MAV_HEADER_LEN + n;
	out[p] = (uint8_t)crc;
	out[p + 1] = (uint8_t)(crc >> 8);
	if (signed_) {
		uint64_t ts = mav_timestamp_now(sign);
		if (ts <= sign->last_sent)
			ts = sign->last_sent + 1;
		sign->last_sent = ts;
		out[p + 2] = sign->link_id;
		for (int i = 0; i < 6; i++)
			out[p + 3 + i] = (uint8_t)(ts >> (8 * i));
		mav_sign(sign->key, out, p + 2, sign->link_id, ts, out + p + 9);
	}
	return total;
}

static struct mav_stream *stream_for(struct mav_signing *s, uint8_t sysid, uint8_t compid, uint8_t link,
				     int *is_new)
{
	struct mav_stream *oldest = &s->streams[0];
	for (int i = 0; i < MAV_STREAMS; i++) {
		struct mav_stream *st = &s->streams[i];
		if (st->used && st->sysid == sysid && st->compid == compid && st->link_id == link) {
			*is_new = 0;
			return st;
		}
		if (!st->used)
			oldest = st;
		else if (oldest->used && st->last_seen < oldest->last_seen)
			oldest = st;
	}
	*is_new = 1;
	memset(oldest, 0, sizeof *oldest);
	oldest->used = 1;
	oldest->sysid = sysid;
	oldest->compid = compid;
	oldest->link_id = link;
	return oldest;
}

/* The signature checks of message_signing.html, in order: a signature
 * present, correct, and a timestamp the stream accepts. The stream's
 * timestamp is updated only by a packet that passed every check. */
static int verify(struct mav_signing *s, const uint8_t *buf, struct mav_frame *f, const char **reason)
{
	s->seen++;
	if (!f->is_signed) {
		if (s->accept_unsigned)
			return 1;
		*reason = "unsigned frame on a signed link";
		s->rejected++;
		return 0;
	}
	uint8_t sig[6];
	size_t n = MAV_HEADER_LEN + f->len + MAV_CRC_LEN;
	mav_sign(s->key, buf, n, f->link_id, f->timestamp, sig);
	if (memcmp(sig, f->signature, 6) != 0) {
		*reason = "bad signature";
		s->rejected++;
		return 0;
	}
	uint64_t now = mav_timestamp_now(s);
	if (f->timestamp + MAV_TIMESTAMP_SLACK < now) {
		*reason = "timestamp more than a minute behind";
		s->rejected++;
		return 0;
	}
	int is_new;
	struct mav_stream *st = stream_for(s, f->sysid, f->compid, f->link_id, &is_new);
	if (!is_new && f->timestamp <= st->timestamp) {
		*reason = "timestamp not newer than the stream's last";
		s->rejected++;
		return 0;
	}
	st->timestamp = f->timestamp;
	st->last_seen = ++s->seen;
	/* What we send next is newer than anything seen (the sender's clock may
	 * run ahead of ours). */
	if (f->timestamp > s->last_sent)
		s->last_sent = f->timestamp;
	return 1;
}

size_t mav_decode(const uint8_t *buf, size_t len, struct mav_frame *out, struct mav_signing *vfy,
		  const char **reason)
{
	static const char *const none = "";
	*reason = none;
	memset(out, 0, sizeof *out);
	if (len == 0)
		return 0;
	if (buf[0] != MAV_STX) {
		*reason = "not a MAVLink 2 start byte";
		return 1;
	}
	if (len < MAV_HEADER_LEN + 1 + MAV_CRC_LEN)
		return 0;
	out->len = buf[1];
	out->incompat = buf[2];
	out->compat = buf[3];
	out->seq = buf[4];
	out->sysid = buf[5];
	out->compid = buf[6];
	out->msgid = (uint32_t)buf[7] | (uint32_t)buf[8] << 8 | (uint32_t)buf[9] << 16;
	out->is_signed = (out->incompat & MAV_IFLAG_SIGNED) != 0;
	size_t total = MAV_HEADER_LEN + out->len + MAV_CRC_LEN + (out->is_signed ? MAV_SIGNATURE_LEN : 0);
	if (len < total)
		return 0;
	out->frame_len = total;
	if (out->incompat & ~MAV_IFLAG_SIGNED) {
		*reason = "unknown incompatibility flag";
		return total;
	}
	const struct mav_msg_info *mi = mav_msg_info(out->msgid);
	if (!mi) {
		*reason = "unknown message id";
		return total;
	}
	if (out->len > mi->len) {
		*reason = "payload longer than the message";
		return total;
	}
	memcpy(out->payload, buf + MAV_HEADER_LEN, out->len);
	size_t p = MAV_HEADER_LEN + out->len;
	out->crc = (uint16_t)(buf[p] | (uint16_t)buf[p + 1] << 8);
	uint16_t crc = mav_crc(buf + 1, p - 1, 0xffff);
	crc = mav_crc_accumulate(mi->crc_extra, crc);
	if (crc != out->crc) {
		*reason = "bad CRC";
		return total;
	}
	if (out->is_signed) {
		out->link_id = buf[p + 2];
		out->timestamp = 0;
		for (int i = 0; i < 6; i++)
			out->timestamp |= (uint64_t)buf[p + 3 + i] << (8 * i);
		memcpy(out->signature, buf + p + 9, 6);
	}
	if (vfy && vfy->enabled && !verify(vfy, buf, out, reason))
		return total;
	out->valid = 1;
	return total;
}
