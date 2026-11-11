/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioSha256.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror(uint32_t x, unsigned n) {return x >> n | x << (32 - n);}

static void compress(AudioSha256 *s, const uint8_t *b)
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3, s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], bb = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void audio_sha256_init(AudioSha256 *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (unsigned i = 0; i < 8; ++i) s->h[i] = iv[i];
    s->bytes = 0; s->used = 0;
}

void audio_sha256_update(AudioSha256 *s, const void *data, uint32_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    s->bytes += length;
    while (length--) {
        s->block[s->used++] = *p++;
        if (s->used == 64) {compress(s, s->block); s->used = 0;}
    }
}

void audio_sha256_final(AudioSha256 *s, uint8_t digest[32])
{
    const uint64_t bits = s->bytes * 8;
    s->block[s->used++] = 0x80;
    if (s->used > 56) {while (s->used < 64) s->block[s->used++] = 0; compress(s, s->block); s->used = 0;}
    while (s->used < 56) s->block[s->used++] = 0;
    for (int i = 7; i >= 0; --i) s->block[s->used++] = (uint8_t)(bits >> (8 * i));
    compress(s, s->block);
    for (unsigned i = 0; i < 8; ++i) {
        digest[4 * i] = (uint8_t)(s->h[i] >> 24); digest[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(s->h[i] >> 8); digest[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void audio_sha256(const void *data, uint32_t length, uint8_t digest[32])
{
    AudioSha256 s;
    audio_sha256_init(&s);
    audio_sha256_update(&s, data, length);
    audio_sha256_final(&s, digest);
}

/* ab060b8476065bd789f327f7f8e724a2e17d94e93a85e2e8aa4b4954ce88b90e */
const uint8_t audio_preset_sha256[32] = {0xab, 0x06, 0x0b, 0x84, 0x76, 0x06, 0x5b, 0xd7, 0x89, 0xf3, 0x27, 0xf7, 0xf8, 0xe7, 0x24, 0xa2,
                                         0xe1, 0x7d, 0x94, 0xe9, 0x3a, 0x85, 0xe2, 0xe8, 0xaa, 0x4b, 0x49, 0x54, 0xce, 0x88, 0xb9, 0x0e};

int audio_preset_is_stock(const uint8_t *data, uint32_t size)
{
    uint8_t d[32];
    if (!data || size != AUDIO_PRESET_BYTES) return 0;
    audio_sha256(data, size, d);
    unsigned diff = 0;
    for (unsigned i = 0; i < 32; ++i) diff |= d[i] ^ audio_preset_sha256[i];
    return diff == 0;
}
