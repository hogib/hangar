// SPDX-License-Identifier: GPL-3.0-or-later
/* BLAKE2s (RFC 7693), keyed or not, any output length from 1 to 32 bytes:
 * the bridge's datagram tags and key derivation (docs/08-bridge.md "The
 * link"). Written from the RFC; checked against its test vector and against
 * Python's hashlib.blake2s in the bridge's suite. */
#ifndef BLAKE2S_H
#define BLAKE2S_H

#include <stddef.h>
#include <stdint.h>

struct blake2s {
	uint32_t h[8], t[2];
	uint8_t buf[64];
	size_t c, outlen;
};

/* 0, or -1 for an outlen not in 1..32 or a key longer than 32 bytes. */
int blake2s_init(struct blake2s *s, size_t outlen, const void *key, size_t keylen);
void blake2s_update(struct blake2s *s, const void *in, size_t n);
void blake2s_final(struct blake2s *s, void *out);
/* All three at once. */
int blake2s(void *out, size_t outlen, const void *key, size_t keylen, const void *in, size_t n);

#endif
