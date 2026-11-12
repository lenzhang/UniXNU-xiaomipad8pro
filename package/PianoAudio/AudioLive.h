/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* PianoAudio's levels 8-10 (docs/drivers/audio.md section 8): the macOS devices on the DSP, after levels 4-6 passed in the same boot
 * (the session hands over the four amplifiers configured and powered down, the TDM pins muxed, the PCM streams through bypass and
 * the audio slice mapped: AudioSessionConfig.keep):
 *   8  the Speaker device's periods (AudioPcm.h's dsp transport: its engine thread writes them into the ring below) played through
 *      the speaker plan: brought up when the device runs with sound, the amplifiers started muted at the playing gain (at most 160
 *      at this level) and unmuted after four silent periods came back, every 10 ms DSP period taken from the ring through the
 *      safety chain (the slots, the DC blocker, the master gain, the fades, the -20 dBFS ceiling of this level), the amplifiers'
 *      group check while they play; taken down (a fade to silence, the amplifiers muted, after idle_ms without sound their power,
 *      the plan undone) when the device stops, is muted or plays only silence
 *   9  also the daily level: the playing gain up to the core's FS19XX_GAIN_CAP (192), the ceiling up to -1 dBFS, the kernel's own
 *      master volume and mute (boot arguments, the Speaker device's properties) ramped without a click; the HAL's own volume works
 *      on the samples before they come here
 *   10 also the Mic device: the capture plan (the VA clocks, the mic graphs, the VA macro's DMIC path between their prepare and
 *      start) brought up while the device runs, the capture pump's two-channel periods mixed to mono into the Mic's ring, the
 *      first 100 ms dropped (the start transient)
 * The rules of AudioSession.h hold here too: one DSP command at a time over both plans, each answer checked, the reverse unwind;
 * nothing new while PianoPower asks for quiet (a running stream stops, and comes back after); at any sign of trouble in PianoPower's
 * health no DSP command any more, the amplifiers muted, powered down and held in reset at once. A refused command or period, or an
 * amplifier whose status changes while it plays, ends that path for the boot; a late period stops it (the third time for the boot).
 * Two clocks: the devices run on the host clock, the DSP on the TDM's; each ring's fill is held near its target by dropping or
 * repeating single frames (counted).
 * Portable C, new code for PianoAudio: the owner ticks it with the time and PianoPower's health, feeds it what arrives and supplies
 * the hardware through the session's AudioSessionOps; the devices' threads only touch AudioLiveShared. Host-tested in
 * tests/live_host.c against models of the DSP, the amplifiers and both devices' engines in simulated time. */
#ifndef PIANO_AUDIO_LIVE_H
#define PIANO_AUDIO_LIVE_H
#include <stdint.h>
#include "AudioSession.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_LIVE_RING 4096u            /* the Speaker's stereo ring: 85 ms */
#define AUDIO_LIVE_TARGET 960u           /* its fill while playing: 20 ms */
#define AUDIO_LIVE_DEPTH 4u              /* speaker periods queued at the DSP while playing: 40 ms */
#define AUDIO_LIVE_IDLE_MS 3000u         /* no sound (or no device) this long: the path goes down */
#define AUDIO_LIVE_IDLE_MAX_MS 600000u
#define AUDIO_LIVE_STATUS_MS 500u        /* the amplifiers' status while they play */
#define AUDIO_LIVE_LATE_STOPS 3u         /* late stops of a path before it is off for the boot */
#define AUDIO_LIVE_MIC_RING 4096u        /* the Mic's mono ring */
#define AUDIO_LIVE_MIC_TARGET 960u
#define AUDIO_LIVE_MIC_DEPTH 4u
#define AUDIO_LIVE_MIC_SETTLE_MS 100u    /* the capture's first periods: zeros */
#define AUDIO_LIVE_SLIP_BAND 240u        /* frames the smoothed fill may stray from where it settled before single frames slip */
#define AUDIO_VOLUME_STEP 1024           /* Q15 per 10 ms period: the master volume's ramp (a full swing in 0.32 s) */
#define AUDIO_LIVE_IDLE_TICK_MS 2000u    /* the tick with both paths down (the transport wakes the owner sooner) */
/* The latency the transport adds, for the devices' registry (AudioPcm.h output_latency/input_latency). */
#define AUDIO_LIVE_OUTPUT_LATENCY (AUDIO_LIVE_DEPTH * AUDIO_PERIOD_FRAMES + AUDIO_LIVE_TARGET)
#define AUDIO_LIVE_INPUT_LATENCY (AUDIO_PERIOD_FRAMES + AUDIO_LIVE_MIC_TARGET)

/* ---- what the devices' engine threads share with the owner's thread (single producer, single consumer per ring) */
typedef struct {
    AudioRing out;                       /* stereo: the Speaker's engine writes, the live thread reads */
    AudioRing in;                        /* mono: the live thread writes, the Mic's engine reads */
    int want_out, want_in;               /* the devices run (the transport's running()) */
    int accepting;                       /* the speaker stream takes the ring now (else play() drops: nothing would read it) */
    int capturing;                       /* the Mic's ring is fed now */
    int sound;                           /* a period with a non-zero sample came while the speaker path was down */
    uint32_t sound_at;                   /* the time of the last such period (ms) */
    int have_sound;
    int32_t volume_q15;                  /* level 9: the master volume (0..32767), from the device's property or the boot argument */
    int muted;                           /* level 9: the master mute */
    uint64_t offered, taken, dropped, captured, controls;
} AudioLiveShared;
/* The rings' storage: AUDIO_LIVE_RING_BYTES and AUDIO_LIVE_MIC_RING_BYTES (the owner's, kept as long as sh). */
#define AUDIO_LIVE_RING_BYTES (AUDIO_LIVE_RING * 2u * 2u)
#define AUDIO_LIVE_MIC_RING_BYTES (AUDIO_LIVE_MIC_RING * 2u)
void audio_live_shared_init(AudioLiveShared *sh, int16_t *out_storage, int16_t *in_storage, int32_t volume_q15);
/* The devices' side (AudioPcm.h's dsp transport, from their engine threads; running() with the engine stopped). play: the frames
 * taken (all of them: what does not fit, or comes while the path is down, is dropped and counted); *wake 1 when the owner's thread
 * should run now (the first sound while the speaker path is down). capture: the mono frames given (the rest is silence). */
void audio_live_running(AudioLiveShared *sh, int output, int input);
uint32_t audio_live_play(AudioLiveShared *sh, const int16_t *stereo, uint32_t frames, uint32_t now_ms, int *wake);
uint32_t audio_live_capture(AudioLiveShared *sh, int16_t *mono, uint32_t frames);
/* Level 9: the master volume in percent (0 silent, 100 unity, -0.5 dB a step below) and the mute; out of range: unchanged. */
void audio_live_control(AudioLiveShared *sh, int volume_percent, int mute);
int32_t audio_volume_q15(int percent);
/* The ceiling for an attenuation of db dB below full scale (1..40; others: the level 8 ceiling, -20 dBFS). */
int32_t audio_ceiling_db(unsigned db);

/* ---- the live paths */
typedef struct {
    uint32_t level;                      /* 8, 9 or 10 (others are clamped) */
    unsigned gain;                       /* the amplifiers' gain while playing: at most 160 at level 8, FS19XX_GAIN_CAP from 9 */
    int32_t ceiling;                     /* the chain's ceiling: AUDIO_CEILING_TEST at level 8, at most AUDIO_CEILING_MAX from 9 */
    uint32_t idle_ms;                    /* no sound or no device this long: the path goes down */
    uint32_t status_ms;                  /* the amplifiers' status while they play */
    uint32_t answer_ms;                  /* each DSP command's answer */
    int mic;                             /* level 10: the Mic's capture path (0: the Mic stays silent) */
} AudioLiveConfig;
void audio_live_defaults(AudioLiveConfig *c, uint32_t level);

enum {LiveDown = 0, LiveStarting, LiveRunning, LiveStopping, LiveOff};
enum {SpkPriming = 0, SpkPlaying, SpkFading, SpkMuting, SpkQuiet};

typedef struct {
    int32_t avg, setpoint;               /* 64ths of a frame */
    uint32_t samples;
} AudioLiveDrift;

typedef struct {
    ArSeq seq;
    int active, in_flight, pending_step, pending_undo;
    uint32_t unsent, sent_at;
    const char *name;
    uint8_t pkt[GPR_MAX_PACKET];
} AudioLivePlan;

typedef struct {
    AudioLiveConfig cfg;
    AudioSessionOps ops;
    AudioDspGuard *guard;
    const AudioPorts *ports;
    AudioAmps *amps;
    AudioLiveShared *sh;
    int finished, off, held, was_held, aborting;
    uint32_t started_at, now;
    /* ---- the speaker path */
    AudioLivePlan splan;
    ArSpeakerPlan speaker;
    int spk_state, spk_mode, spk_reason, spk_ending;
    ArPump pump;
    AudioChain chain;
    AudioGuard sguard;
    int unmuted, dsp_error, stream_unsent, hold_from_sound;
    unsigned amp_bad, late_stops;
    uint32_t stream_at, next_status, quiet_at, stream_len, mute_after;
    int32_t gain_now;                    /* the master gain as ramped */
    AudioLiveDrift spk_drift;
    uint8_t spkt[GPR_MAX_PACKET];
    int16_t stereo[AUDIO_PERIOD_FRAMES * 2];
    int16_t quad[AUDIO_PERIOD_FRAMES * 4];
    int32_t peak;
    uint32_t spk_starts, spk_stops, periods, sound_periods, underruns, skipped, drops, repeats;
    /* ---- the mic path */
    AudioLivePlan mplan;
    ArMicPlan micplan;
    int mic_state, mic_reason, mic_ending, cap_unsent, cap_error;
    unsigned mic_late_stops;
    ArPump cap;
    uint8_t cpkt[GPR_MAX_PACKET];
    uint32_t cap_len, mic_at, mic_idle_at;
    AudioLiveDrift mic_drift;
    int16_t mono[AUDIO_PERIOD_FRAMES + 1];
    uint32_t mic_starts, mic_periods, mic_drops, mic_repeats, mic_full;
    int va_on;
    /* ---- the exchange */
    uint32_t sends, queue_full, received, stray, timeouts;
} AudioLive;

/* After audio_session_tick finished with the hardware kept (audio_session_kept): the session's amplifiers, ops, guard and ports.
 * 0, or -1 (nothing set) when the session did not hand over. */
int audio_live_init(AudioLive *l, const AudioLiveConfig *c, const AudioSession *session, AudioLiveShared *sh);
/* One tick at now_ms with PianoPower's health (o: the observation; null keeps the guard's state). Returns the milliseconds until
 * the next tick wants to run (5 while a stream runs, 2 while a plan moves, AUDIO_LIVE_IDLE_TICK_MS with both paths down: the
 * transport wakes the owner at the first sound), 0 once finished (the guard went off: everything is down and the amplifiers are
 * held in reset). */
uint32_t audio_live_tick(AudioLive *l, const AdgObservation *o, uint32_t now_ms);
void audio_live_receive(AudioLive *l, const uint8_t *packet, uint32_t length, uint32_t now_ms);
/* The owner's stop: both paths down (hard: without DSP commands), the amplifiers held in reset, the TDM pins given back, then
 * finished. */
void audio_live_abort(AudioLive *l, int hard, uint32_t now_ms);
const char *audio_live_state(int state);

#ifdef __cplusplus
}
#endif
#endif
