/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The PCM transport under PianoAudio's macOS devices (PianoAudioDevice.cpp), in place of a virtio-snd transport: each device's
 * engine thread hands it one period at a time, interleaved S16 at 48 kHz (the speaker's stereo out, the microphone's mono in).
 *   null  (AudioPcm.c): takes every output period and gives no input, so the devices run on their own clock into silence (the HAL
 *         still has a default output; nothing reaches any hardware): piano-audio=3..7
 *   dsp   (AudioDsp.cpp, piano-audio=8..10): play() puts the Speaker's periods into AudioLive.h's ring, which the live thread takes
 *         through the safety chain into the DSP's buffers once the speaker plan runs (it starts the plan at the first sound and
 *         takes it down after the device stopped or went silent); capture() gives the Mic's periods from the capture ring (level
 *         10); until the live paths run, or when they are off, it behaves as the null transport
 * running() is called with the engine thread stopped; play() and capture() from that thread only; control() from the HAL's
 * property writes on the Speaker device. */
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
    /* The kernel's master volume (0..100 percent, -1: unchanged) and mute (0, 1; -1: unchanged); may be null (no such control). */
    void (*control)(void *context, int volume_percent, int mute);
} PianoPcmTransport;

typedef struct {
    uint64_t played, captured;
    uint32_t output_starts, input_starts;
    int output, input;
} PianoPcmNullStats;
const PianoPcmTransport *piano_pcm_null(void);
void piano_pcm_null_stats(PianoPcmNullStats *out);
/* AudioDsp.cpp: the DSP transport (levels 8-10); null until piano_audio_live_prepare() made its shared state. */
const PianoPcmTransport *piano_pcm_dsp(void);

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
