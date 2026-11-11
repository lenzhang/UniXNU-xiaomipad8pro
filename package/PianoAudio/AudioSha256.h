/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SHA-256 (FIPS 180-4), for one job: telling whether the amplifier preset read from the owner's odm partition is the file Ubuntu
 * runs on piano (its size and digest are published in xiaomipad-8pro-mainline firmware/piano-audio.json: stock build
 * OS3.0.303.0.WPYCNXM). New code; portable C, no allocation. */
#ifndef PIANO_AUDIO_SHA256_H
#define PIANO_AUDIO_SHA256_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    uint32_t used;
} AudioSha256;
void audio_sha256_init(AudioSha256 *s);
void audio_sha256_update(AudioSha256 *s, const void *data, uint32_t length);
void audio_sha256_final(AudioSha256 *s, uint8_t digest[32]);
void audio_sha256(const void *data, uint32_t length, uint8_t digest[32]);

/* The stock preset foursemi/piano/fs19xx.fsm: 4348 bytes, sha256 ab060b84...b90e. */
#define AUDIO_PRESET_BYTES 4348u
extern const uint8_t audio_preset_sha256[32];
/* 1 when data is that file. */
int audio_preset_is_stock(const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif
#endif
