// SPDX-License-Identifier: GPL-3.0-or-later
/* SHA-256 (FIPS 180-4), for MAVLink 2 message signing (mav.h). A plain
 * implementation over a byte stream; nothing else in hangar needs a hash of
 * this family. */
#ifndef HANGAR_SHA256_H
#define HANGAR_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct sha256 {
	uint32_t h[8];
	uint64_t len;        /* bytes fed so far */
	uint8_t buf[64];
	size_t buf_len;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[32]);
/* The whole thing at once. */
void sha256_of(const void *data, size_t len, uint8_t out[32]);

#endif
