/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* PianoAudio's levels 4-7 (docs/drivers/audio.md section 8) as one state machine over the pieces below it, run after level 2's DSP
 * link reached "APM ready" (or, without the DSP, level 5 alone):
 *   4  the LPASS clocks through the DSP's resource manager (PRM): the TDM bit clock requested and released; the VA macro's two votes
 *      and its mclk requested, its registers and the LPI DMIC pins read while they hold (the owner's loads), all released
 *   5  the four amplifiers (AudioAmps.h): out of reset, probed, configured from the owner's preset, powered down and muted
 *   6  the TDM pins muxed, the DSP's streams through bypass, the speaker plan up with the amplifiers left down and a silent stream
 *      pumped for a while (the amplifiers' status sampled), then all of it undone; the microphone graphs opened, prepared, closed
 *   7  only with piano-audio-tone=1: the speaker plan with the amplifiers started muted at the test gain, unmuted after four silent
 *      periods, the tone walk (AudioSafety.h: <= -20 dBFS, <= 3 s of tone) through the safety chain, the stream guard (5 s watchdog,
 *      the amplifiers' group check every 100 ms, a DSP error or a late period); then zeros, the amplifiers down, the graphs undone
 * The rules kept here whatever the levels ask (audio.md 10.1, 10.2): every DSP command one at a time with its answer checked, the
 * first error undoes what was done in reverse (AudioSequence.h); nothing new is sent while PianoPower asks for quiet; at any sign of
 * trouble in PianoPower's health (the guard off: AudioDspLink.h) no DSP command goes out any more and the amplifiers are muted,
 * powered down and held in reset at once; the DSP is never started, stopped or restarted (nothing here could). At the end the
 * amplifiers are held in reset and the TDM pins are given back as found.
 * Portable C, new code for PianoAudio: the owner sends the packets, feeds back what arrives, ticks it with the time and PianoPower's
 * health, and supplies the hardware through AudioSessionOps (host-tested against a model of the DSP and of the amplifiers). */
#ifndef PIANO_AUDIO_SESSION_H
#define PIANO_AUDIO_SESSION_H
#include <stdint.h>
#include "AudioAmps.h"
#include "AudioDspLink.h"
#include "AudioSafety.h"
#include "AudioSequence.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PERIOD_FRAMES 480u                                   /* 10 ms at 48 kHz */
#define AUDIO_SPK_PERIOD_BYTES (AUDIO_PERIOD_FRAMES * 4u * 2u)     /* four channels of S16 (FL FR LS RS): 3840 */
#define AUDIO_SPK_SLOTS 8u
#define AUDIO_SPK_DEPTH 6u                                         /* 60 ms queued at the DSP */
#define AUDIO_MIC_PERIOD_BYTES (AUDIO_PERIOD_FRAMES * 2u * 2u)     /* two channels of S16: 1920 */
#define AUDIO_MIC_SLOTS 8u
/* The buffers in PianoAudio's slice of the DSP-addressable memory (PianoDspMemory.h PIANO_DSP_MEMORY_AUDIO, 16 MiB at 0x9da00000):
 * the speaker periods at +0, the capture periods at +64 KiB; the kext maps the first AUDIO_DMA_BYTES of the slice. */
#define AUDIO_DMA_SPEAKER 0x00000u
#define AUDIO_DMA_MIC 0x10000u
#define AUDIO_DMA_BYTES 0x20000u
#define AUDIO_LATE_MS 250u                 /* a period out longer than this stops the stream */
#define AUDIO_ANSWER_MS 2000u              /* each DSP command's answer (sheng's) */
#define AUDIO_SILENCE_MAX_MS 10000u
#define AUDIO_STATUS_MS 100u               /* the amplifiers' status while a stream runs */
#define AUDIO_UNMUTE_PERIODS 4u            /* silent periods returned before the amplifiers are unmuted */

typedef struct {
    uint32_t level;          /* the highest level to run, 4..7 */
    int dsp;                 /* level 2 reached "APM ready" in this boot: levels 4, 6 and 7 need it */
    int ports;               /* the GPR port set (audio_ports) the link answered on */
    int lpass_read;          /* level 4: the VA macro's and the LPI pins' registers read while the votes hold */
    int preset_any;          /* level 5: a valid preset that is not the stock file is taken */
    int mic;                 /* level 6: the microphone graphs opened, prepared and closed too */
    uint32_t silence_ms;     /* level 6: the silent stream (<= AUDIO_SILENCE_MAX_MS) */
    int tone;                /* level 7: 1 plays the walk; 0 (the default) does not run level 7 */
    uint32_t tone_mask, tone_ms, gap_ms;   /* the walk: slots, each slot's tone, the silence between (AudioWalk's limits) */
    unsigned tone_gain;      /* the amplifiers' gain for the walk, at most AUDIO_AMPS_TEST_GAIN (160) */
    uint32_t answer_ms;      /* each DSP command's answer */
} AudioSessionConfig;
void audio_session_defaults(AudioSessionConfig *c);

typedef struct {
    void *ctx;
    void (*log)(void *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    /* One packet to the DSP: 0 queued; otherwise the queue is full and the same packet is offered again at the next tick. */
    int (*send)(void *ctx, const uint8_t *packet, uint32_t length);
    /* Level 4: the VA macro's and the LPI pins' registers, read and logged by the owner (loads only); null: not read. */
    void (*lpass_read)(void *ctx);
    /* Level 5: the amplifiers' bus and reset lines (amps.bus null: no bus), and the preset as read from the ESP (null: none, why). */
    AudioAmpsIo amps;
    /* When no amplifier answered at all: swap SDA and SCL once and return 1 (the probe runs again), or 0. May be null. */
    int (*bus_swap)(void *ctx);
    const uint8_t *preset;
    uint32_t preset_bytes;
    const char *preset_why;
    /* Level 6: the i2s1 function on the TDM pins (1), or the pins back as found (0); the DSP's PCM streams through bypass (0 or an
     * error); the mapped part of the audio slice (DSP-addressable: below 4 GiB, 4 KiB aligned, AUDIO_DMA_BYTES); a barrier that makes
     * a period's stores visible to the DSP before its WRITE goes out. */
    int (*tdm_pins)(void *ctx, int on);
    int (*smmu)(void *ctx);
    uint8_t *dma;
    uint64_t dma_phys;
    uint32_t dma_bytes;
    void (*publish)(void *ctx);
} AudioSessionOps;

enum {AudioLevelNotRun = 0, AudioLevelPassed = 1, AudioLevelFailed = 2, AudioLevelSkipped = 3};

typedef struct {
    AudioSessionConfig cfg;
    AudioSessionOps ops;
    AudioDspGuard *guard;                /* level 2's guard, already running */
    const AudioPorts *ports;
    int phase, finished, off, held, was_held;
    uint32_t phase_at, started_at;
    /* the plan in progress */
    ArSeq seq;
    int seq_active, in_flight, pending_step, pending_undo, plan_level;
    const char *plan;
    uint32_t unsent, sent_at;
    uint8_t pkt[GPR_MAX_PACKET];
    ArSpeakerPlan speaker;
    ArMicPlan mic;
    /* the amplifiers */
    AudioAmps amps;
    int amps_ready;                      /* level 5 passed: configured, down */
    uint16_t last_status[AUDIO_AMPS];
    int have_last;
    int pins_on, smmu_done, speaker_ok;
    /* the stream */
    ArPump pump;
    AudioChain chain;
    AudioWalk walk;
    AudioGuard sguard;
    int pump_live, streaming, stopping, ending, tone_stream, unmuted, dsp_error;
    unsigned amp_bad, tail;
    uint32_t stream_at, next_status, unmuted_at, tone_frames, periods_sent, stop_reason;
    int stream_unsent;
    uint32_t stream_len;
    uint8_t spkt[GPR_MAX_PACKET];
    int16_t quad[AUDIO_PERIOD_FRAMES * 4];
    int32_t peak;                        /* the largest |sample| handed to the DSP */
    /* results, by level (4..7) */
    int result[8];
    const char *why[8];
    const char *failed_step[8];
    uint32_t failed_status[8];
    uint32_t sends, queue_full, received, stray, timeouts;
} AudioSession;

/* guard: level 2's (AudioLink.guard), null without the DSP. */
void audio_session_init(AudioSession *s, const AudioSessionConfig *c, const AudioSessionOps *ops, AudioDspGuard *guard);
/* One tick at now_ms with PianoPower's health (o: null without the DSP). Returns the milliseconds until the next tick wants to run
 * (2 while a stream runs, 10 otherwise), 0 once finished. */
uint32_t audio_session_tick(AudioSession *s, const AdgObservation *o, uint32_t now_ms);
/* A packet from the DSP (the inbox). */
void audio_session_receive(AudioSession *s, const uint8_t *packet, uint32_t length, uint32_t now_ms);
/* The owner's stop (its time budget ran out, the module goes away): the amplifiers muted, powered down and held at once, the plan
 * undone (hard: without DSP commands), then finished. */
void audio_session_abort(AudioSession *s, int hard, uint32_t now_ms);
const char *audio_session_phase(int phase);
const char *audio_level_result(int result);

#ifdef __cplusplus
}
#endif
#endif
