/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The PCM transport under PianoAudio's macOS devices (PianoAudioDevice.cpp), in place of a virtio-snd transport: each device's
 * engine thread hands it one period at a time, interleaved S16 at 48 kHz (the speaker's stereo out, the microphone's mono in).
 *   null  (this file): takes every output period and gives no input, so the devices run on their own clock into silence (the HAL
 *         still has a default output; nothing reaches any hardware)
 *   dsp   (level 7, not written yet): play() is audio_output_period (AudioSafety.h: slots, DC blocker, gain, fade, ceiling) into the
 *         ring the DSP pump (AudioSequence.h ArPump) drains in 10 ms periods; running() starts and stops the speaker plan
 * running() is called with the engine thread stopped; play() and capture() from that thread only. */
#ifndef PIANO_AUDIO_PCM_H
#define PIANO_AUDIO_PCM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct PianoPcmTransport {
    const char *name;
    void *context;
    uint32_t output_latency;     /* frames past the transport (the DSP's queue, the TDM), added to the published output latency */
    uint32_t input_latency;
    void (*running)(void *context, int output, int input);
    /* Takes up to frames stereo frames; returns how many it took. */
    uint32_t (*play)(void *context, const int16_t *stereo, uint32_t frames);
    /* Gives up to frames mono frames; returns how many it gave (the engine fills the rest with silence). */
    uint32_t (*capture)(void *context, int16_t *mono, uint32_t frames);
} PianoPcmTransport;

typedef struct {
    uint64_t played, captured;
    uint32_t output_starts, input_starts;
    int output, input;
} PianoPcmNullStats;
const PianoPcmTransport *piano_pcm_null(void);
void piano_pcm_null_stats(PianoPcmNullStats *out);

/* PianoAudioDevice.cpp: links its classes under IOAudio2Family's (IOAudio2Device, IOAudio2DeviceUserClient) and publishes the
 * "Speaker" and "Mic" devices on the transport. The devices published (bit 0 Speaker, bit 1 Mic), or -1 (the family's classes not
 * found, or neither device published). Once. */
int piano_audio_devices_attach(const PianoPcmTransport *transport);
/* How far it got: 0 not tried, 1 transport set, 2 family loaded, 3 classes linked, 4 published; negative: failed there. */
int piano_audio_devices_stage(void);

#ifdef __cplusplus
}
#endif
#endif
