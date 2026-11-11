/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The speaker-safety half of PianoAudio's output path (docs/drivers/audio.md 10.2): no speaker protection runs on piano, so every
 * period the kernel hands the DSP goes through this chain, below whatever the HAL does:
 *   stereo (or mono) S16 -> the four slots FL FR LS RS (top left, top right, bottom left, bottom right in landscape)
 *   -> DC blocker (one-pole high-pass, -3 dB near 38 Hz) -> digital gain (<= 0 dB) -> fade in/out -> clamp to +-ceiling
 * with the ceiling at most -1 dBFS (AUDIO_CEILING_MAX, compile time) and -20 dBFS (AUDIO_CEILING_TEST) until level 8; the test tone
 * (1 kHz, <= -20 dBFS, <= 3 s, 20 ms ramps); the stream guard (a 5 s watchdog for tests, a DSP error, a late period or an amplifier
 * status change stops the stream: zeros, graph stop, amplifiers powered down); and the PCM ring between the macOS engine and the DSP.
 * New code, integer only (no floating point in the kernel), portable C: host-tested in tests/pcm_host.c. */
#ifndef PIANO_AUDIO_SAFETY_H
#define PIANO_AUDIO_SAFETY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_RATE 48000u
#define AUDIO_Q15_ONE 32767
#define AUDIO_CEILING_MAX 29204          /* -1 dBFS: no setting goes above it */
#define AUDIO_CEILING_TEST 3276          /* -20 dBFS: the first tones and every level below 8 */
#define AUDIO_TEST_PEAK 3276             /* the test tone's peak: -20 dBFS */
#define AUDIO_TEST_MAX_FRAMES (3u * AUDIO_RATE)   /* 3 s */
#define AUDIO_WATCHDOG_MS 5000u
#define AUDIO_FADE_FRAMES 960u           /* 20 ms */
#define AUDIO_MAX_CHANNELS 4u

/* ---- layout: interleaved S16, the DSP stream is four channels in slot order */
void audio_stereo_to_quad(const int16_t *stereo, int16_t *quad, uint32_t frames);
void audio_mono_to_quad(const int16_t *mono, int16_t *quad, uint32_t frames);

/* ---- the chain */
typedef struct {
    int32_t x1[AUDIO_MAX_CHANNELS];
    int64_t y1[AUDIO_MAX_CHANNELS];      /* the previous output, with 15 more fractional bits */
} AudioDcBlocker;
void audio_dc_init(AudioDcBlocker *d);
void audio_dc_run(AudioDcBlocker *d, int16_t *frames, uint32_t count, unsigned channels);

enum {AudioFadeSteady = 0, AudioFadeIn = 1, AudioFadeOut = -1};
typedef struct {
    unsigned channels;
    AudioDcBlocker dc;
    int32_t gain_q15;                    /* digital gain, 0..32767 (<= 0 dB) */
    int32_t ceiling;                     /* |sample| <= ceiling <= AUDIO_CEILING_MAX */
    uint32_t fade_frames, fade_pos;      /* fade length; position 0 (silent) .. fade_frames (full) */
    int fade;                            /* AudioFadeIn, AudioFadeOut or steady */
    uint32_t clamped;                    /* samples the clamp changed */
} AudioChain;
/* Starts silent and fading in (start muted), at the test ceiling and unity digital gain. 0, or -1 for a bad channel count. */
int audio_chain_init(AudioChain *c, unsigned channels);
/* The ceiling (capped at AUDIO_CEILING_MAX; returns what applies) and the digital gain (capped at unity). */
int32_t audio_chain_set_ceiling(AudioChain *c, int32_t ceiling);
int32_t audio_chain_set_gain(AudioChain *c, int32_t gain_q15);
void audio_chain_fade(AudioChain *c, int direction);
/* 1 when a fade-out has reached silence (the owner may stop then). */
int audio_chain_silent(const AudioChain *c);
/* One period in place. */
void audio_chain_run(AudioChain *c, int16_t *frames, uint32_t count);

/* ---- the test tone: 1 kHz (48 samples a cycle at 48 kHz), 20 ms ramps at both ends, then zeros */
typedef struct {
    int32_t peak;
    uint32_t total, done, phase;
} AudioTone;
/* 0, or -1 (nothing set) when peak > AUDIO_TEST_PEAK or frames > AUDIO_TEST_MAX_FRAMES or frames < two ramps. */
int audio_tone_init(AudioTone *t, int32_t peak, uint32_t frames);
/* Fills count frames of `channels` (the same sample on each); returns the frames of tone (the rest are zeros). */
uint32_t audio_tone_fill(AudioTone *t, int16_t *frames, uint32_t count, unsigned channels);

/* ---- the stream guard: the owner checks it every period; any reason but AudioGuardOk means stop now */
enum {AudioGuardOk = 0, AudioGuardWatchdog, AudioGuardDsp, AudioGuardLate, AudioGuardAmplifier, AudioGuardOwner};
typedef struct {
    uint32_t start_ms, limit_ms;         /* limit 0: no time limit (daily use) */
    int running, reason;
} AudioGuard;
void audio_guard_start(AudioGuard *g, uint32_t now_ms, uint32_t limit_ms);
/* dsp_error: a refused command or a DSP fault; late: the pump's late period; amplifiers: fs19xx_group_check's bits. Latches. */
int audio_guard_check(AudioGuard *g, uint32_t now_ms, int dsp_error, int late, unsigned amplifiers);
void audio_guard_trip(AudioGuard *g, int reason);
const char *audio_guard_reason(int reason);

/* ---- the PCM ring: one producer (the macOS engine's thread), one consumer (the DSP pump); frames of `channels` S16. Capacity a power
 * of two. A write that does not fit is cut (counted); a read short of frames is completed with zeros (counted). */
typedef struct {
    int16_t *data;
    uint32_t frames, channels;           /* capacity in frames (power of two) */
    uint32_t head, tail;                 /* written / read positions in frames (wrapping counters) */
    uint32_t overruns, underruns;
} AudioRing;
int audio_ring_init(AudioRing *r, int16_t *storage, uint32_t frames, uint32_t channels);
uint32_t audio_ring_fill(const AudioRing *r);
uint32_t audio_ring_write(AudioRing *r, const int16_t *frames, uint32_t count);
uint32_t audio_ring_read(AudioRing *r, int16_t *frames, uint32_t count);
void audio_ring_reset(AudioRing *r);

/* ---- one output period of the macOS engine for the DSP: stereo -> quad -> the chain -> the ring. Returns frames queued. */
uint32_t audio_output_period(AudioChain *c, AudioRing *r, const int16_t *stereo, uint32_t frames, int16_t *scratch_quad);

#ifdef __cplusplus
}
#endif
#endif
