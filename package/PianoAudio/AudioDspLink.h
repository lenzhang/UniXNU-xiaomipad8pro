/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* PianoAudio's side of the shared DSP (docs/drivers/audio.md 10.1; PianoPower owns the DSP, package/include/PianoAdspContract.h):
 *   the guard      when audio may send anything at all: the DSP up with its charger domain up and settled, PianoPower not asking for
 *                  quiet; and off for the rest of the boot at the first sign of trouble (the fatal bit or a fatal report, a crash
 *                  reason, after we started: a new link or charger generation; or an error of ours)
 *   the inbox      where the DSP thread's receive callback copies each packet (it must not block), for the audio thread
 *   the link       level 2's one exchange: is the APM ready? (sheng's GprSpfProbe: one query in flight, an answer timer, a retry
 *                  pause, a limit on tries); nothing else is sent
 * The model is PianoSensors' SensorsGuard (sensors.md section 7). Portable C, no allocation, no clock: the glue (AudioDsp.cpp) feeds
 * PianoPower's health and its own channel state each tick with the time, and sends what it is given. */
#ifndef PIANO_AUDIO_DSP_LINK_H
#define PIANO_AUDIO_DSP_LINK_H
#include <stdint.h>
#include "PianoGpr.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {AdgWait = 0, AdgRun = 1, AdgOff = 2};
enum {AdgReasonNone = 0, AdgReasonFatal, AdgReasonCrash, AdgReasonCharger, AdgReasonLink, AdgReasonError, AdgReasonContract,
      AdgReasonOwner};
#define ADG_CHARGER_UP 0x1fffffffu
#define ADG_SETTLE_MS 20000u

/* One read-only observation: PianoPower's piano_adsp_health() and our adsp_apps channel's state. */
typedef struct {
    int contract_ok;                     /* the health structure's version is the one this was built for */
    int dsp_up, fatal;
    uint32_t fatal_reports, link_generation, charger_pd, charger_generation, quiet;
    const char *crash;                   /* null or empty: none */
    int channel_up;
} AdgObservation;

typedef struct {
    uint32_t settle_ms;
    int state, reason;
    int started;                         /* AdgRun reached once: from then on any change of generation is trouble */
    int up_seen;
    uint32_t up_since;
    int have_base;
    uint32_t fatal0, link0, charger0;    /* at the first observation / when started */
    uint32_t observations, holds;
} AudioDspGuard;
void adg_init(AudioDspGuard *g, uint32_t settle_ms);
int adg_observe(AudioDspGuard *g, const AdgObservation *o, uint32_t now_ms);
/* An error of ours (a refused command, a late period, a DSP that stopped answering): off for the rest of the boot. */
void adg_trip(AudioDspGuard *g, int reason);
const char *adg_reason(int reason);

/* ---- the inbox: single producer (the DSP thread's callback), single consumer (the audio thread); a full inbox drops, counted */
#define AUDIO_INBOX_SLOTS 16u
#define AUDIO_INBOX_BYTES 512u           /* the receive intents offered for adsp_apps are 512 bytes (the DT's qcom,intents) */
typedef struct {
    uint8_t data[AUDIO_INBOX_SLOTS][AUDIO_INBOX_BYTES];
    uint32_t length[AUDIO_INBOX_SLOTS];
    uint32_t head, tail;
    uint32_t dropped, too_long, taken;
} AudioInbox;
void audio_inbox_init(AudioInbox *b);
int audio_inbox_put(AudioInbox *b, const uint8_t *data, uint32_t length);   /* 0, or -1 (full or too long: counted) */
uint32_t audio_inbox_get(AudioInbox *b, uint8_t *out, uint32_t capacity);  /* a packet's length, or 0 */

/* ---- the GPR source ports (docs/drivers/audio.md 10.10): the ports our APM commands, PRM requests and data commands leave from (the
 * DSP answers to the port a command came from). Three stacks that ran on this DSP family disagree, so the set is a boot argument
 * (piano-audio-ports=N, Ubuntu's by default) and level 2's readiness query can walk the others while nobody answers:
 *   0 Ubuntu   APM 1, PRM 2, data from 0x10000000: Ubuntu's piano kernel (Linux gpr: the DT's service ids, dynamic data ports), the
 *              one stack that plays on this tablet
 *   1 sheng    APM and data 0x2001, PRM 2: UniXNU-sheng on SM8550 (hardware-verified there; its first PRM vote from 0x2001 was
 *              refused with status 1, from 2 it was granted); the K90's SM8750 port used 0x2001 for the APM
 *   2 SunUEFI  APM 3, PRM 7, data from 0x10000000: Project-SunUEFI's note that its piano ADSP answered only the static ports of the
 *              downstream service ids and dropped port 1 */
enum {AudioPortsUbuntu = 0, AudioPortsSheng = 1, AudioPortsSun = 2, AUDIO_PORT_SETS = 3};
typedef struct {
    const char *name, *source;
    uint32_t apm, prm, data;
} AudioPorts;
/* The set, or null for an unknown one. */
const AudioPorts *audio_ports(int set);

/* ---- level 2: the APM readiness exchange under the guard */
enum {AlWaiting = 0, AlQuerying, AlReady, AlGaveUp, AlOff};
typedef struct {
    AudioDspGuard guard;
    GprSpfProbe spf;
    int state;
    uint32_t answer_ms, retry_ms;        /* 1000, 500 */
    uint32_t sent_at, retry_at, ready_at, first_sent;
    int retry_armed;
    int set;                             /* the port set in use (audio_ports), -1 for a bare port */
    uint32_t try_mask, tried;            /* sets that may be tried while nobody answers / sets tried (bit per set) */
    uint32_t max_queries, switches;
    uint32_t queries[AUDIO_PORT_SETS];   /* readiness queries sent from each set */
} AudioLink;
void audio_link_init(AudioLink *l, uint32_t port, uint32_t max_queries, uint32_t settle_ms);
/* The same with a port set: the query leaves from the set's APM port; when max_queries of them go unanswered (no answer and no
 * refusal: nobody listens on that port), the next set of try_mask after the current one (each once) takes over. An answer of any
 * kind (ready, not ready, refused) keeps the set. */
void audio_link_init_ports(AudioLink *l, int set, uint32_t try_mask, uint32_t max_queries, uint32_t settle_ms);
/* One tick: returns the packet to queue now (out holds it), or 0. */
uint32_t audio_link_tick(AudioLink *l, const AdgObservation *o, uint32_t now_ms, uint8_t out[GPR_HEADER_BYTES]);
/* The packet the tick returned was refused by the queue: it is asked again at a later tick. */
void audio_link_unsent(AudioLink *l);
/* A packet from the inbox. */
void audio_link_receive(AudioLink *l, const uint8_t *data, uint32_t length, uint32_t now_ms);
const char *audio_link_state(int state);

#ifdef __cplusplus
}
#endif
#endif
