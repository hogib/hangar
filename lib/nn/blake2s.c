// SPDX-License-Identifier: LGPL-3.0-or-later
/* BLAKE2s, RFC 7693. See blake2s.h. */
#include "blake2s.h"

#include <string.h>

static const uint32_t blake2s_iv[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};

static const uint8_t blake2s_sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

static uint32_t load32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void store32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t rotr32(uint32_t w, unsigned c) {
  return (w >> c) | (w << (32 - c));
}

static void blake2s_compress(nn_blake2s_state *s, const uint8_t block[64]) {
  uint32_t m[16], v[16];
  int i;
  for (i = 0; i < 16; i++)
    m[i] = load32(block + 4 * i);
  for (i = 0; i < 8; i++)
    v[i] = s->h[i];
  v[8] = blake2s_iv[0];
  v[9] = blake2s_iv[1];
  v[10] = blake2s_iv[2];
  v[11] = blake2s_iv[3];
  v[12] = s->t[0] ^ blake2s_iv[4];
  v[13] = s->t[1] ^ blake2s_iv[5];
  v[14] = s->f[0] ^ blake2s_iv[6];
  v[15] = s->f[1] ^ blake2s_iv[7];

#define G(r, i, a, b, c, d)                                                    \
  do {                                                                         \
    a = a + b + m[blake2s_sigma[r][2 * i + 0]];                                \
    d = rotr32(d ^ a, 16);                                                     \
    c = c + d;                                                                 \
    b = rotr32(b ^ c, 12);                                                     \
    a = a + b + m[blake2s_sigma[r][2 * i + 1]];                                \
    d = rotr32(d ^ a, 8);                                                      \
    c = c + d;                                                                 \
    b = rotr32(b ^ c, 7);                                                      \
  } while (0)
#define ROUND(r)                                                               \
  do {                                                                         \
    G(r, 0, v[0], v[4], v[8], v[12]);                                          \
    G(r, 1, v[1], v[5], v[9], v[13]);                                          \
    G(r, 2, v[2], v[6], v[10], v[14]);                                         \
    G(r, 3, v[3], v[7], v[11], v[15]);                                         \
    G(r, 4, v[0], v[5], v[10], v[15]);                                         \
    G(r, 5, v[1], v[6], v[11], v[12]);                                         \
    G(r, 6, v[2], v[7], v[8], v[13]);                                          \
    G(r, 7, v[3], v[4], v[9], v[14]);                                          \
  } while (0)
  ROUND(0);
  ROUND(1);
  ROUND(2);
  ROUND(3);
  ROUND(4);
  ROUND(5);
  ROUND(6);
  ROUND(7);
  ROUND(8);
  ROUND(9);
#undef G
#undef ROUND
  for (i = 0; i < 8; i++)
    s->h[i] = s->h[i] ^ v[i] ^ v[i + 8];
}

static void increment_counter(nn_blake2s_state *s, uint32_t inc) {
  s->t[0] += inc;
  s->t[1] += (s->t[0] < inc);
}

int nn_blake2s_init_key(nn_blake2s_state *s, size_t outlen, const void *key,
                        size_t keylen) {
  /* Parameter block: digest_length, key_length, fanout=1, depth=1, rest 0. */
  uint32_t p0;
  int i;
  if (outlen == 0 || outlen > NN_BLAKE2S_OUTBYTES)
    return -1;
  if (keylen > NN_BLAKE2S_KEYBYTES || (keylen > 0 && key == NULL))
    return -1;
  memset(s, 0, sizeof *s);
  p0 = (uint32_t)outlen | ((uint32_t)keylen << 8) | (1u << 16) | (1u << 24);
  for (i = 0; i < 8; i++)
    s->h[i] = blake2s_iv[i];
  s->h[0] ^= p0;
  s->outlen = outlen;
  if (keylen > 0) {
    uint8_t block[NN_BLAKE2S_BLOCKBYTES];
    memset(block, 0, sizeof block);
    memcpy(block, key, keylen);
    nn_blake2s_update(s, block, sizeof block);
  }
  return 0;
}

int nn_blake2s_init(nn_blake2s_state *s, size_t outlen) {
  return nn_blake2s_init_key(s, outlen, NULL, 0);
}

void nn_blake2s_update(nn_blake2s_state *s, const void *in, size_t inlen) {
  const uint8_t *p = (const uint8_t *)in;
  if (inlen == 0)
    return;
  /* Keep the last block in the buffer: it is compressed by final with the
   * last-block flag set. */
  if (s->buflen + inlen > NN_BLAKE2S_BLOCKBYTES) {
    size_t fill = NN_BLAKE2S_BLOCKBYTES - s->buflen;
    memcpy(s->buf + s->buflen, p, fill);
    s->buflen = 0;
    increment_counter(s, NN_BLAKE2S_BLOCKBYTES);
    blake2s_compress(s, s->buf);
    p += fill;
    inlen -= fill;
    while (inlen > NN_BLAKE2S_BLOCKBYTES) {
      increment_counter(s, NN_BLAKE2S_BLOCKBYTES);
      blake2s_compress(s, p);
      p += NN_BLAKE2S_BLOCKBYTES;
      inlen -= NN_BLAKE2S_BLOCKBYTES;
    }
  }
  memcpy(s->buf + s->buflen, p, inlen);
  s->buflen += inlen;
}

void nn_blake2s_final(nn_blake2s_state *s, void *out, size_t outlen) {
  uint8_t digest[NN_BLAKE2S_OUTBYTES];
  int i;
  increment_counter(s, (uint32_t)s->buflen);
  s->f[0] = 0xFFFFFFFFu;
  memset(s->buf + s->buflen, 0, NN_BLAKE2S_BLOCKBYTES - s->buflen);
  blake2s_compress(s, s->buf);
  for (i = 0; i < 8; i++)
    store32(digest + 4 * i, s->h[i]);
  if (outlen > s->outlen)
    outlen = s->outlen;
  memcpy(out, digest, outlen);
  memset(s, 0, sizeof *s);
}

int nn_blake2s(void *out, size_t outlen, const void *in, size_t inlen,
               const void *key, size_t keylen) {
  nn_blake2s_state s;
  if (nn_blake2s_init_key(&s, outlen, key, keylen) < 0)
    return -1;
  nn_blake2s_update(&s, in, inlen);
  nn_blake2s_final(&s, out, outlen);
  return 0;
}
