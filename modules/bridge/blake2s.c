// SPDX-License-Identifier: GPL-3.0-or-later
/* BLAKE2s, RFC 7693 section 3 and appendix C. */
#include "blake2s.h"

#include <string.h>

static const uint32_t IV[8] = { 0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
				0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u };

static const uint8_t SIGMA[10][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
};

static uint32_t rotr(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static uint32_t load32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define G(a, b, c, d, x, y)                      \
	do {                                     \
		v[a] = v[a] + v[b] + (x);        \
		v[d] = rotr(v[d] ^ v[a], 16);    \
		v[c] = v[c] + v[d];              \
		v[b] = rotr(v[b] ^ v[c], 12);    \
		v[a] = v[a] + v[b] + (y);        \
		v[d] = rotr(v[d] ^ v[a], 8);     \
		v[c] = v[c] + v[d];              \
		v[b] = rotr(v[b] ^ v[c], 7);     \
	} while (0)

static void compress(struct blake2s *s, int last)
{
	uint32_t v[16], m[16];
	for (int i = 0; i < 8; i++) {
		v[i] = s->h[i];
		v[i + 8] = IV[i];
	}
	v[12] ^= s->t[0];
	v[13] ^= s->t[1];
	if (last)
		v[14] = ~v[14];
	for (int i = 0; i < 16; i++)
		m[i] = load32(s->buf + 4 * i);
	for (int r = 0; r < 10; r++) {
		const uint8_t *z = SIGMA[r];
		G(0, 4, 8, 12, m[z[0]], m[z[1]]);
		G(1, 5, 9, 13, m[z[2]], m[z[3]]);
		G(2, 6, 10, 14, m[z[4]], m[z[5]]);
		G(3, 7, 11, 15, m[z[6]], m[z[7]]);
		G(0, 5, 10, 15, m[z[8]], m[z[9]]);
		G(1, 6, 11, 12, m[z[10]], m[z[11]]);
		G(2, 7, 8, 13, m[z[12]], m[z[13]]);
		G(3, 4, 9, 14, m[z[14]], m[z[15]]);
	}
	for (int i = 0; i < 8; i++)
		s->h[i] ^= v[i] ^ v[i + 8];
}

int blake2s_init(struct blake2s *s, size_t outlen, const void *key, size_t keylen)
{
	if (outlen == 0 || outlen > 32 || keylen > 32)
		return -1;
	memset(s, 0, sizeof *s);
	for (int i = 0; i < 8; i++)
		s->h[i] = IV[i];
	s->h[0] ^= 0x01010000u ^ ((uint32_t)keylen << 8) ^ (uint32_t)outlen;
	s->outlen = outlen;
	if (keylen) {
		blake2s_update(s, key, keylen);
		s->c = 64; /* the key is a whole block of its own, zero-padded */
	}
	return 0;
}

void blake2s_update(struct blake2s *s, const void *in, size_t n)
{
	const uint8_t *p = in;
	for (size_t i = 0; i < n; i++) {
		if (s->c == 64) {
			s->t[0] += (uint32_t)s->c;
			if (s->t[0] < s->c)
				s->t[1]++;
			compress(s, 0);
			s->c = 0;
		}
		s->buf[s->c++] = p[i];
	}
}

void blake2s_final(struct blake2s *s, void *out)
{
	s->t[0] += (uint32_t)s->c;
	if (s->t[0] < s->c)
		s->t[1]++;
	while (s->c < 64)
		s->buf[s->c++] = 0;
	compress(s, 1);
	uint8_t *o = out;
	for (size_t i = 0; i < s->outlen; i++)
		o[i] = (uint8_t)(s->h[i >> 2] >> (8 * (i & 3)));
}

int blake2s(void *out, size_t outlen, const void *key, size_t keylen, const void *in, size_t n)
{
	struct blake2s s;
	if (blake2s_init(&s, outlen, key, keylen) < 0)
		return -1;
	blake2s_update(&s, in, n);
	blake2s_final(&s, out);
	return 0;
}
