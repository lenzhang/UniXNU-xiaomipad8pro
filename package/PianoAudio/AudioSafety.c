/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioSafety.h"

#define DC_POLE 32604   /* 0.995 in Q15 */

static int16_t sat16(int64_t v) {return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);}

void audio_stereo_to_quad(const int16_t *stereo, int16_t *quad, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t l = stereo[2 * i], r = stereo[2 * i + 1];
        quad[4 * i] = l; quad[4 * i + 1] = r; quad[4 * i + 2] = l; quad[4 * i + 3] = r;   /* FL FR LS RS: TL TR BL BR */
    }
}

void audio_mono_to_quad(const int16_t *mono, int16_t *quad, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i) quad[4 * i] = quad[4 * i + 1] = quad[4 * i + 2] = quad[4 * i + 3] = mono[i];
}

void audio_dc_init(AudioDcBlocker *d)
{
    for (unsigned i = 0; i < AUDIO_MAX_CHANNELS; ++i) {d->x1[i] = 0; d->y1[i] = 0;}
}

void audio_dc_run(AudioDcBlocker *d, int16_t *frames, uint32_t count, unsigned channels)
{
    if (channels > AUDIO_MAX_CHANNELS) return;
    for (uint32_t i = 0; i < count; ++i)
        for (unsigned c = 0; c < channels; ++c) {
            const int32_t x = frames[i * channels + c];
            const int64_t acc = ((int64_t)(x - d->x1[c]) << 15) + ((d->y1[c] * DC_POLE) >> 15);
            d->x1[c] = x;
            d->y1[c] = acc;
            frames[i * channels + c] = sat16((acc + (1 << 14)) >> 15);
        }
}

int audio_chain_init(AudioChain *c, unsigned channels)
{
    for (unsigned i = 0; i < sizeof *c; ++i) ((uint8_t *)c)[i] = 0;
    if (!channels || channels > AUDIO_MAX_CHANNELS) return -1;
    c->channels = channels;
    audio_dc_init(&c->dc);
    c->gain_q15 = AUDIO_Q15_ONE;
    c->ceiling = AUDIO_CEILING_TEST;
    c->fade_frames = AUDIO_FADE_FRAMES;
    c->fade_pos = 0;
    c->fade = AudioFadeIn;
    return 0;
}

int32_t audio_chain_set_ceiling(AudioChain *c, int32_t ceiling)
{
    c->ceiling = ceiling < 0 ? 0 : ceiling > AUDIO_CEILING_MAX ? AUDIO_CEILING_MAX : ceiling;
    return c->ceiling;
}

int32_t audio_chain_set_gain(AudioChain *c, int32_t gain_q15)
{
    c->gain_q15 = gain_q15 < 0 ? 0 : gain_q15 > AUDIO_Q15_ONE ? AUDIO_Q15_ONE : gain_q15;
    return c->gain_q15;
}

void audio_chain_fade(AudioChain *c, int direction) {c->fade = direction > 0 ? AudioFadeIn : direction < 0 ? AudioFadeOut : AudioFadeSteady;}
int audio_chain_silent(const AudioChain *c) {return c->fade_pos == 0 && c->fade != AudioFadeIn;}

void audio_chain_run(AudioChain *c, int16_t *frames, uint32_t count)
{
    audio_dc_run(&c->dc, frames, count, c->channels);
    for (uint32_t i = 0; i < count; ++i) {
        for (unsigned ch = 0; ch < c->channels; ++ch) {
            int64_t s = frames[i * c->channels + ch];
            s = s * c->gain_q15 / AUDIO_Q15_ONE;
            s = s * (int64_t)c->fade_pos / (int64_t)c->fade_frames;
            if (s > c->ceiling) {s = c->ceiling; c->clamped++;}
            else if (s < -c->ceiling) {s = -c->ceiling; c->clamped++;}
            frames[i * c->channels + ch] = (int16_t)s;
        }
        if (c->fade == AudioFadeIn && c->fade_pos < c->fade_frames && ++c->fade_pos == c->fade_frames) c->fade = AudioFadeSteady;
        else if (c->fade == AudioFadeOut && c->fade_pos > 0) --c->fade_pos;
    }
}

/* sin(2 pi k / 48) in Q15 */
static const int16_t kSine[48] = {0, 4277, 8481, 12539, 16383, 19947, 23170, 25996, 28377, 30273, 31650, 32487, 32767, 32487, 31650,
    30273, 28377, 25996, 23170, 19947, 16383, 12539, 8481, 4277, 0, -4277, -8481, -12539, -16383, -19947, -23170, -25996, -28377,
    -30273, -31650, -32487, -32767, -32487, -31650, -30273, -28377, -25996, -23170, -19947, -16383, -12539, -8481, -4277};

int audio_tone_init(AudioTone *t, int32_t peak, uint32_t frames)
{
    if (peak < 0 || peak > AUDIO_TEST_PEAK || frames > AUDIO_TEST_MAX_FRAMES || frames < 2 * AUDIO_FADE_FRAMES) return -1;
    t->peak = peak; t->total = frames; t->done = 0; t->phase = 0;
    return 0;
}

uint32_t audio_tone_fill(AudioTone *t, int16_t *frames, uint32_t count, unsigned channels)
{
    uint32_t tone = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t s = 0;
        if (t->done < t->total) {
            const uint32_t n = t->done, left = t->total - 1 - n;
            uint32_t ramp = n < left ? n : left;
            if (ramp > AUDIO_FADE_FRAMES) ramp = AUDIO_FADE_FRAMES;
            s = (int32_t)((int64_t)kSine[t->phase] * t->peak / AUDIO_Q15_ONE * ramp / AUDIO_FADE_FRAMES);
            t->phase = (t->phase + 1) % 48u;
            t->done++;
            tone++;
        }
        for (unsigned c = 0; c < channels; ++c) frames[i * channels + c] = (int16_t)s;
    }
    return tone;
}

void audio_guard_start(AudioGuard *g, uint32_t now_ms, uint32_t limit_ms)
{
    g->start_ms = now_ms; g->limit_ms = limit_ms; g->running = 1; g->reason = AudioGuardOk;
}

void audio_guard_trip(AudioGuard *g, int reason)
{
    if (!g->reason) g->reason = reason ? reason : AudioGuardOwner;
    g->running = 0;
}

int audio_guard_check(AudioGuard *g, uint32_t now_ms, int dsp_error, int late, unsigned amplifiers)
{
    if (g->reason) return g->reason;
    if (dsp_error) audio_guard_trip(g, AudioGuardDsp);
    else if (late) audio_guard_trip(g, AudioGuardLate);
    else if (amplifiers) audio_guard_trip(g, AudioGuardAmplifier);
    else if (g->limit_ms && now_ms - g->start_ms >= g->limit_ms) audio_guard_trip(g, AudioGuardWatchdog);
    return g->reason;
}

const char *audio_guard_reason(int reason)
{
    switch (reason) {
    case AudioGuardOk: return "ok";
    case AudioGuardWatchdog: return "watchdog";
    case AudioGuardDsp: return "DSP error";
    case AudioGuardLate: return "late period";
    case AudioGuardAmplifier: return "amplifier status";
    case AudioGuardOwner: return "stopped by the owner";
    default: return "?";
    }
}

int audio_ring_init(AudioRing *r, int16_t *storage, uint32_t frames, uint32_t channels)
{
    for (unsigned i = 0; i < sizeof *r; ++i) ((uint8_t *)r)[i] = 0;
    if (!storage || frames < 2 || (frames & (frames - 1)) || !channels || channels > AUDIO_MAX_CHANNELS) return -1;
    r->data = storage; r->frames = frames; r->channels = channels;
    return 0;
}

uint32_t audio_ring_fill(const AudioRing *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

uint32_t audio_ring_write(AudioRing *r, const int16_t *frames, uint32_t count)
{
    const uint32_t head = r->head, tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    const uint32_t space = r->frames - (head - tail);
    const uint32_t n = count < space ? count : space;
    for (uint32_t i = 0; i < n; ++i) {
        int16_t *to = r->data + ((head + i) & (r->frames - 1)) * r->channels;
        for (uint32_t c = 0; c < r->channels; ++c) to[c] = frames[i * r->channels + c];
    }
    r->overruns += count - n;
    __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
    return n;
}

uint32_t audio_ring_read(AudioRing *r, int16_t *frames, uint32_t count)
{
    const uint32_t tail = r->tail, head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    const uint32_t fill = head - tail;
    const uint32_t n = count < fill ? count : fill;
    for (uint32_t i = 0; i < n; ++i) {
        const int16_t *from = r->data + ((tail + i) & (r->frames - 1)) * r->channels;
        for (uint32_t c = 0; c < r->channels; ++c) frames[i * r->channels + c] = from[c];
    }
    for (uint32_t i = n * r->channels; i < count * r->channels; ++i) frames[i] = 0;
    r->underruns += count - n;
    __atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
    return n;
}

void audio_ring_reset(AudioRing *r)
{
    __atomic_store_n(&r->tail, __atomic_load_n(&r->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

uint32_t audio_output_period(AudioChain *c, AudioRing *r, const int16_t *stereo, uint32_t frames, int16_t *scratch_quad)
{
    if (c->channels != 4 || r->channels != 4) return 0;
    audio_stereo_to_quad(stereo, scratch_quad, frames);
    audio_chain_run(c, scratch_quad, frames);
    return audio_ring_write(r, scratch_quad, frames);
}
