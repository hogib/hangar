// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
/*
 * BLAKE2s (RFC 7693), unkeyed or keyed, digest of 1 to 32 bytes. Written
 * from the RFC in the style of the public-domain reference implementation,
 * for the weights file's hash check: the bridge hashes a received file with
 * BLAKE2s-256 and the loader verifies the same digest.
 */
#include <stddef.h>
#include <stdint.h>

#define NN_BLAKE2S_OUTBYTES 32
#define NN_BLAKE2S_BLOCKBYTES 64
#define NN_BLAKE2S_KEYBYTES 32

typedef struct nn_blake2s_state {
  uint32_t h[8];
  uint32_t t[2];
  uint32_t f[2];
  uint8_t buf[NN_BLAKE2S_BLOCKBYTES];
  size_t buflen;
  size_t outlen;
} nn_blake2s_state;

/* -1 on a bad outlen (0 or > 32) or keylen (> 32). */
int nn_blake2s_init(nn_blake2s_state *s, size_t outlen);
int nn_blake2s_init_key(nn_blake2s_state *s, size_t outlen, const void *key,
                        size_t keylen);
void nn_blake2s_update(nn_blake2s_state *s, const void *in, size_t inlen);
void nn_blake2s_final(nn_blake2s_state *s, void *out, size_t outlen);
/* One call: out[outlen] = BLAKE2s(in). */
int nn_blake2s(void *out, size_t outlen, const void *in, size_t inlen,
               const void *key, size_t keylen);
