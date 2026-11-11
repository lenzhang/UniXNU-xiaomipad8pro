// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the speaker-safety chain and the PCM ring (AudioSafety.c): the slot mapping, the DC blocker's response (DC gone,
// 1 kHz kept, -3 dB near 38 Hz), the ceiling that no input crosses, start muted and the fades, the test tone's level, length and
// shape, the stream guard, the single-producer ring with its overrun and underrun handling.
//   cc -std=c11 -Wall -Wextra -Werror pcm_host.c ../AudioSafety.c -lm && ./a.out
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioSafety.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static int16_t buf[48000 * 4], quad[48000 * 4];

static void testMapping(void)
{
    const int16_t stereo[6] = {1, -2, 300, -400, 32767, -32768};
    audio_stereo_to_quad(stereo, quad, 3);
    const int16_t want[12] = {1, -2, 1, -2, 300, -400, 300, -400, 32767, -32768, 32767, -32768};
    CHECK(!memcmp(quad, want, sizeof want));   // FL FR LS RS = TL TR BL BR: left on the left pair, right on the right pair
    const int16_t mono[2] = {7, -9};
    audio_mono_to_quad(mono, quad, 2);
    CHECK(quad[0] == 7 && quad[3] == 7 && quad[4] == -9 && quad[7] == -9);
}

// The peak of the last `frames` frames of channel 0 after running `total` frames of a sine of freq Hz and amplitude a.
static double response(double freq, double a, uint32_t total, uint32_t frames)
{
    AudioDcBlocker d;
    audio_dc_init(&d);
    for (uint32_t i = 0; i < total; ++i) buf[i] = (int16_t)lrint(a * sin(2 * M_PI * freq * i / 48000.0));
    audio_dc_run(&d, buf, total, 1);
    double peak = 0;
    for (uint32_t i = total - frames; i < total; ++i) peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
    return peak / a;
}

static void testDcBlocker(void)
{
    AudioDcBlocker d;
    audio_dc_init(&d);
    for (uint32_t i = 0; i < 24000; ++i) buf[i] = 10000;   // a DC step
    audio_dc_run(&d, buf, 24000, 1);
    CHECK(buf[0] == 10000);                                  // the step passes, then decays
    int ok = 1;
    for (uint32_t i = 4800; i < 24000; ++i) ok &= buf[i] >= -1 && buf[i] <= 1;
    CHECK(ok);
    CHECK(response(1000, 10000, 9600, 4800) > 0.99);         // 1 kHz: within 0.1 dB
    const double r38 = response(38, 10000, 48000, 24000);
    CHECK(r38 > 0.66 && r38 < 0.75);                         // about -3 dB at 38 Hz
    CHECK(response(20, 10000, 48000, 24000) < 0.55);         // 20 Hz attenuated
    CHECK(response(100, 30000, 48000, 24000) > 0.92);
    // channels are independent: DC on channel 1 only
    audio_dc_init(&d);
    for (uint32_t i = 0; i < 4800; ++i) {buf[2 * i] = (int16_t)lrint(8000 * sin(2 * M_PI * 1000 * i / 48000.0)); buf[2 * i + 1] = -20000;}
    audio_dc_run(&d, buf, 4800, 2);
    CHECK(buf[2 * 4799 + 1] == 0 && abs(buf[2 * 4799]) < 8100);
}

static void testChain(void)
{
    AudioChain c;
    CHECK(audio_chain_init(&c, 0) == -1 && audio_chain_init(&c, 5) == -1);
    CHECK(audio_chain_init(&c, 4) == 0 && c.ceiling == AUDIO_CEILING_TEST && c.fade == AudioFadeIn && c.fade_pos == 0);
    CHECK(audio_chain_set_ceiling(&c, 40000) == AUDIO_CEILING_MAX && audio_chain_set_ceiling(&c, -5) == 0);
    CHECK(audio_chain_set_gain(&c, 70000) == AUDIO_Q15_ONE && audio_chain_set_gain(&c, -1) == 0);
    // whatever comes in, nothing above the ceiling goes out (random full-scale noise, one second, at every ceiling tried)
    const int32_t ceilings[4] = {AUDIO_CEILING_TEST, 12000, AUDIO_CEILING_MAX, 99999};
    uint32_t seed = 12345;
    for (int k = 0; k < 4; ++k) {
        audio_chain_init(&c, 4);
        audio_chain_set_ceiling(&c, ceilings[k]);
        for (uint32_t i = 0; i < 48000 * 4; ++i) {seed = seed * 1103515245u + 12345u; buf[i] = (int16_t)(seed >> 16);}
        audio_chain_run(&c, buf, 48000);
        int32_t peak = 0;
        for (uint32_t i = 0; i < 48000 * 4; ++i) peak = abs(buf[i]) > peak ? abs(buf[i]) : peak;
        CHECK(peak <= c.ceiling && c.ceiling <= AUDIO_CEILING_MAX && c.clamped > 0);
    }
    // start muted: the first frame is silent, the fade-in is monotone over 20 ms, then steady
    audio_chain_init(&c, 1);
    audio_chain_set_ceiling(&c, AUDIO_CEILING_MAX);
    for (uint32_t i = 0; i < 2000; ++i) buf[i] = (int16_t)((i & 1) ? 20000 : -20000);   // 24 kHz square: the DC blocker passes it
    audio_chain_run(&c, buf, 2000);
    CHECK(buf[0] == 0 && c.fade == AudioFadeSteady && c.fade_pos == AUDIO_FADE_FRAMES);
    int mono = 1;
    for (uint32_t i = 2; i < AUDIO_FADE_FRAMES; ++i) mono &= abs(buf[i]) + 60 >= abs(buf[i - 2]);
    CHECK(mono && abs(buf[480]) < 11000 && abs(buf[1500]) > 19000);
    // fade out to silence, then zeros
    audio_chain_fade(&c, -1);
    CHECK(!audio_chain_silent(&c));
    for (uint32_t i = 0; i < 2000; ++i) buf[i] = (int16_t)((i & 1) ? 20000 : -20000);
    audio_chain_run(&c, buf, 2000);
    CHECK(audio_chain_silent(&c) && buf[1999] == 0 && buf[1000] == 0 && abs(buf[100]) > 15000);
    // digital gain: half
    audio_chain_init(&c, 1);
    audio_chain_set_ceiling(&c, AUDIO_CEILING_MAX);
    audio_chain_set_gain(&c, 16384);
    for (uint32_t i = 0; i < 4000; ++i) buf[i] = (int16_t)((i & 1) ? 20000 : -20000);
    audio_chain_run(&c, buf, 4000);
    CHECK(abs(buf[3999]) > 9800 && abs(buf[3999]) < 10200);
}

static void testTone(void)
{
    AudioTone t;
    CHECK(audio_tone_init(&t, AUDIO_TEST_PEAK + 1, 48000) == -1);            // above -20 dBFS
    CHECK(audio_tone_init(&t, AUDIO_TEST_PEAK, AUDIO_TEST_MAX_FRAMES + 1) == -1);   // above 3 s
    CHECK(audio_tone_init(&t, AUDIO_TEST_PEAK, 1000) == -1);                 // shorter than its two ramps
    CHECK(20 * log10(AUDIO_TEST_PEAK / 32767.0) <= -20.0);
    CHECK(audio_tone_init(&t, AUDIO_TEST_PEAK, 48000) == 0);
    static int16_t q[48000 * 4];
    uint32_t n = 0;
    for (int p = 0; p < 110; ++p) n += audio_tone_fill(&t, q + (uint32_t)p * 480 * 4 % (48000 * 4), 480, 4);   // 10 ms periods, 1.1 s
    CHECK(n == 48000);
    // regenerate in one go to look at it
    audio_tone_init(&t, AUDIO_TEST_PEAK, 48000);
    CHECK(audio_tone_fill(&t, q, 48000, 4) == 48000 && audio_tone_fill(&t, q, 480, 4) == 0 && q[0] == 0 && q[479 * 4] == 0);
    audio_tone_init(&t, AUDIO_TEST_PEAK, 48000);
    audio_tone_fill(&t, q, 48000, 4);
    int32_t peak = 0;
    unsigned crossings = 0;
    for (uint32_t i = 0; i < 48000; ++i) {
        peak = abs(q[4 * i]) > peak ? abs(q[4 * i]) : peak;
        CHECK(q[4 * i] == q[4 * i + 1] && q[4 * i] == q[4 * i + 3]);
        if (i && q[4 * (i - 1)] < 0 && q[4 * i] >= 0) crossings++;
    }
    CHECK(peak <= AUDIO_TEST_PEAK && peak >= 3200);
    CHECK(crossings >= 998 && crossings <= 1000);   // 1 kHz for 1 s
    CHECK(q[0] == 0 && abs(q[4 * 12]) < 60);        // the ramp starts at zero (12 samples in: a quarter cycle, 1.25 % of the ramp)
    int32_t mid = 0;
    for (uint32_t i = 470; i < 490; ++i) mid = abs(q[4 * i]) > mid ? abs(q[4 * i]) : mid;
    CHECK(mid > 1500 && mid < 1800);               // half way up the 20 ms ramp
    CHECK(abs(q[4 * 47999]) < 60);                 // and back down at the end
}

static void testGuard(void)
{
    AudioGuard g;
    audio_guard_start(&g, 1000, AUDIO_WATCHDOG_MS);
    CHECK(audio_guard_check(&g, 3000, 0, 0, 0) == AudioGuardOk && audio_guard_check(&g, 5999, 0, 0, 0) == AudioGuardOk);
    CHECK(audio_guard_check(&g, 6000, 0, 0, 0) == AudioGuardWatchdog && !g.running);
    CHECK(audio_guard_check(&g, 6001, 1, 0, 0) == AudioGuardWatchdog);   // latched: the first reason stays
    audio_guard_start(&g, 0xfffff000u, AUDIO_WATCHDOG_MS);                // across the millisecond counter's wrap
    CHECK(audio_guard_check(&g, 0x00000100u, 0, 0, 0) == AudioGuardOk && audio_guard_check(&g, 0x00000389u, 0, 0, 0) == AudioGuardWatchdog);
    audio_guard_start(&g, 0, 0);   // daily use: no time limit
    CHECK(audio_guard_check(&g, 0x7fffffff, 0, 0, 0) == AudioGuardOk);
    CHECK(audio_guard_check(&g, 1, 0, 0, 0x4) == AudioGuardAmplifier);
    audio_guard_start(&g, 0, 0);
    CHECK(audio_guard_check(&g, 1, 0, 1, 0) == AudioGuardLate);
    audio_guard_start(&g, 0, 0);
    CHECK(audio_guard_check(&g, 1, 1, 1, 1) == AudioGuardDsp);   // a DSP error comes first
    audio_guard_start(&g, 0, 0);
    audio_guard_trip(&g, 0);
    CHECK(g.reason == AudioGuardOwner && !strcmp(audio_guard_reason(AudioGuardWatchdog), "watchdog") && !strcmp(audio_guard_reason(9), "?"));
}

static void testRing(void)
{
    static int16_t store[64 * 4], in[200 * 4], out[200 * 4];
    AudioRing r;
    CHECK(audio_ring_init(&r, store, 48, 4) == -1 && audio_ring_init(&r, store, 64, 5) == -1 && audio_ring_init(&r, 0, 64, 4) == -1);
    CHECK(audio_ring_init(&r, store, 64, 4) == 0 && audio_ring_fill(&r) == 0);
    for (int i = 0; i < 200 * 4; ++i) in[i] = (int16_t)i;
    CHECK(audio_ring_write(&r, in, 40) == 40 && audio_ring_fill(&r) == 40);
    CHECK(audio_ring_read(&r, out, 30) == 30 && !memcmp(out, in, 30 * 4 * 2));
    CHECK(audio_ring_write(&r, in + 40 * 4, 50) == 50 && audio_ring_fill(&r) == 60);   // wraps
    CHECK(audio_ring_read(&r, out, 60) == 60 && !memcmp(out, in + 30 * 4, 60 * 4 * 2));
    // overrun: a write past the capacity is cut and counted
    CHECK(audio_ring_write(&r, in, 100) == 64 && r.overruns == 36 && audio_ring_fill(&r) == 64);
    // underrun: the missing frames are zeros
    CHECK(audio_ring_read(&r, out, 70) == 64 && r.underruns == 6 && out[64 * 4] == 0 && out[69 * 4 + 3] == 0 && out[63 * 4 + 3] == 63 * 4 + 3);
    audio_ring_write(&r, in, 10);
    audio_ring_reset(&r);
    CHECK(audio_ring_fill(&r) == 0);
    // a long run of odd-sized writes and reads keeps every frame in order
    uint32_t written = 0, read = 0;
    int ordered = 1;
    for (uint32_t k = 0; k < 5000; ++k) {
        static int16_t w[13 * 4], o[11 * 4];
        const uint32_t wn = 1 + k % 13, rn = 1 + (k * 7) % 11;
        for (uint32_t i = 0; i < wn; ++i) for (int c = 0; c < 4; ++c) w[i * 4 + c] = (int16_t)(written + i + c);
        written += audio_ring_write(&r, w, wn);
        const uint32_t got = audio_ring_read(&r, o, rn);
        for (uint32_t i = 0; i < got; ++i) ordered &= o[i * 4] == (int16_t)(read + i) && o[i * 4 + 3] == (int16_t)(read + i + 3);
        read += got;
    }
    CHECK(ordered && written - read == audio_ring_fill(&r));
}

static void testOutputPeriod(void)
{
    static int16_t store[2048 * 4], stereo[480 * 2], scratch[480 * 4], out[480 * 4];
    AudioRing r;
    AudioChain c;
    CHECK(audio_ring_init(&r, store, 2048, 4) == 0 && audio_chain_init(&c, 4) == 0);
    for (int i = 0; i < 480; ++i) {stereo[2 * i] = (int16_t)((i & 1) ? 32767 : -32768); stereo[2 * i + 1] = 0;}   // left only, full scale
    for (int p = 0; p < 3; ++p) CHECK(audio_output_period(&c, &r, stereo, 480, scratch) == 480);
    CHECK(audio_ring_fill(&r) == 1440);
    audio_ring_read(&r, out, 480);   // the first period: the fade-in from silence
    CHECK(out[0] == 0 && out[1] == 0);
    audio_ring_read(&r, out, 480);
    audio_ring_read(&r, out, 480);
    int left = 1, right = 1;
    for (int i = 0; i < 480; ++i) {
        left &= abs(out[4 * i]) <= AUDIO_CEILING_TEST && out[4 * i] == out[4 * i + 2];   // TL and BL carry left, clamped to -20 dBFS
        right &= out[4 * i + 1] == 0 && out[4 * i + 3] == 0;                            // TR and BR silent
    }
    CHECK(left && right && abs(out[4 * 479]) == AUDIO_CEILING_TEST);
    AudioChain two;
    audio_chain_init(&two, 2);
    CHECK(audio_output_period(&two, &r, stereo, 480, scratch) == 0);   // only a four-channel chain feeds the DSP
}

// Level 7's walk: one slot at a time in slot order, each at most -20 dBFS, gaps of zeros, the limits refused.
static int16_t walk[4 * AUDIO_RATE * 4];
static void testWalk(void)
{
    AudioWalk w;
    CHECK(audio_walk_init(&w, 0, 600, 150, AUDIO_TEST_PEAK) == -1);                  // nothing to play
    CHECK(audio_walk_init(&w, 0x10, 600, 150, AUDIO_TEST_PEAK) == -1);               // a fifth slot
    CHECK(audio_walk_init(&w, 0xf, 600, 150, AUDIO_TEST_PEAK + 1) == -1);            // above -20 dBFS
    CHECK(audio_walk_init(&w, 0xf, 800, 0, AUDIO_TEST_PEAK) == -1);                  // 3.2 s of tone in all
    CHECK(audio_walk_init(&w, 0xf, 30, 0, AUDIO_TEST_PEAK) == -1);                   // shorter than the two ramps
    CHECK(audio_walk_init(&w, 0x3, 1500, 1100, AUDIO_TEST_PEAK) == -1);              // 4.1 s with the gap
    CHECK(audio_walk_init(&w, 0x1, 3000, 0, AUDIO_TEST_PEAK) == 0);                  // one slot, the full 3 s
    CHECK(audio_walk_init(&w, 0xf, 600, 150, AUDIO_TEST_PEAK) == 0 && w.count == 4); // the default: 2.85 s
    const uint32_t total = 4 * 600 * 48 + 3 * 150 * 48;
    uint32_t tone = 0, at = 0;
    while (at < total + 4800) {   // in 10 ms periods, and past the end
        CHECK(at / 480 < (total + 4800) / 480 + 1);
        tone += audio_walk_fill(&w, walk + at * 4, 480);
        at += 480;
    }
    CHECK(tone == 4 * 600 * 48 && w.tone_total == tone && audio_walk_slot(&w) == -1 && w.done);
    // each frame: at most one slot carries sound, the slot of its segment; never above -20 dBFS
    int peak[4] = {0, 0, 0, 0};
    for (uint32_t f = 0; f < at; ++f) {
        const uint32_t seg = f / (750 * 48), within = f % (750 * 48);
        const int expect = f < total && within < 600 * 48 ? (int)seg : -1;
        for (int c = 0; c < 4; ++c) {
            const int v = abs(walk[f * 4 + c]);
            CHECK(v <= AUDIO_TEST_PEAK);
            if (c != expect) CHECK(v == 0);
            if (v > peak[c]) peak[c] = v;
        }
    }
    for (int c = 0; c < 4; ++c) CHECK(peak[c] > AUDIO_TEST_PEAK * 99 / 100);   // each slot heard at the test level
    // a mask of two (TR, BR) walks only those, in slot order
    CHECK(audio_walk_init(&w, 0xa, 500, 100, 1000) == 0 && w.slots[0] == 1 && w.slots[1] == 3);
    memset(walk, 0, sizeof walk);
    audio_walk_fill(&w, walk, 1100 * 48);
    for (uint32_t f = 0; f < 1100 * 48; ++f) {
        CHECK(walk[f * 4] == 0 && walk[f * 4 + 2] == 0 && abs(walk[f * 4 + 1]) <= 1000 && abs(walk[f * 4 + 3]) <= 1000);
        if (f < 500 * 48) CHECK(walk[f * 4 + 3] == 0);
        if (f >= 500 * 48) CHECK(walk[f * 4 + 1] == 0);
    }
    CHECK(w.done && audio_walk_slot(&w) == -1);
    // through the chain at the test ceiling: still nothing above -20 dBFS
    AudioChain c;
    audio_chain_init(&c, 4);
    CHECK(audio_walk_init(&w, 0xf, 600, 150, AUDIO_TEST_PEAK) == 0);
    for (uint32_t f = 0; f < total; f += 480) {
        audio_walk_fill(&w, walk, 480);
        audio_chain_run(&c, walk, 480);
        for (int i = 0; i < 480 * 4; ++i) CHECK(abs(walk[i]) <= AUDIO_CEILING_TEST);
    }
}

int main(void)
{
    testMapping();
    testDcBlocker();
    testChain();
    testTone();
    testGuard();
    testRing();
    testOutputPeriod();
    testWalk();
    printf("PASS pcm: %u checks\n", checks);
    return 0;
}
