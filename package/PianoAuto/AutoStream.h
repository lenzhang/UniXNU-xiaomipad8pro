/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* One sensor stream of the hub, switched on and off as its consumer's conditions change (the ambient light with the display and the
 * cover's lid, the accelerometer with the cover): PianoSensors' kext (level S4, docs/drivers/sensors.md section 5a) keeps one of these
 * per sensor, next to that sensor's QMI client of service 400. It decides which request goes out when; the kext sends it (enable =
 * message 514 on change or 513 at a rate, disable = message 10), after SensorsGuard's sg_may_request gave a token.
 *
 * The rules are Ubuntu's fixes (sensors.md section 3), so that readings resume after any off/on, which the v1.1.1 release had to fix
 * ("a nested close cancelled an open's first report"):
 *   - one request outstanding per client: a change of the conditions while a request is out is kept and acted on after its answer
 *     (the latest wish wins: off and on again while an enable is out sends nothing more);
 *   - an enable is never followed by a disable before its answer, a disable never by an enable;
 *   - the first event of a session is never dropped, also when it comes before the enable's answer; events while a disable is out,
 *     while off, or while the conditions want the stream off are dropped (PAS_EV_DROP);
 *   - the first event of each new session is marked (PAS_EV_FIRST): the consumer starts afresh from it (the light policy jumps to it
 *     instead of smoothing from the value before the display slept);
 *   - an enabled stream with no event within first_event_ms is re-armed (disable, then enable), at most rearms times per period the
 *     stream is wanted (an on-change sensor reports its current value on enable; a missed one would leave the consumer waiting);
 *   - a request unanswered within response_ms has failed and leaves the hub's state unknown: a disable goes out first (a clean state)
 *     before the next enable; a late answer is ignored;
 *   - failures back off (retry_first_ms doubling to retry_max_ms); failures in a row end the stream for the boot (PAS_FAILED), and the
 *     guard (SensorsGuard) still counts each one.
 * Portable C, integer only, no allocation, no C library; host-tested (tests/stream_tests.h). */
#ifndef PIANO_AUTO_STREAM_H
#define PIANO_AUTO_STREAM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {PAS_OFF = 0, PAS_ENABLING = 1, PAS_ON = 2, PAS_DISABLING = 3, PAS_FAILED = 4};
enum {PAS_ACT_NONE = 0, PAS_ACT_ENABLE = 1, PAS_ACT_DISABLE = 2};
enum {PAS_EV_DROP = 0, PAS_EV_USE = 1, PAS_EV_FIRST = 2};

typedef struct {
    uint32_t response_ms;            /* 2000 */
    uint32_t first_event_ms;         /* 3000 */
    uint32_t rearms;                 /* 2 */
    uint32_t retry_first_ms, retry_max_ms;   /* 1000, 32000 */
    uint32_t failures;               /* 6 in a row: PAS_FAILED */
} PasConfig;

typedef struct {
    PasConfig cfg;
    int state;
    int want;                        /* the consumer's conditions want the stream */
    int outstanding;                 /* the request out (PAS_ACT_*), PAS_ACT_NONE when none */
    int rearm;                       /* a re-arm is due (state ON) or under way (its disable out) */
    int dirty;                       /* a request timed out: the hub's state is unknown */
    int got_event;                   /* this session had its first event */
    int waiting;                     /* the first-event watchdog runs (ON, no event yet) */
    uint32_t rearms_left;
    uint32_t fails, backoff_ms;
    uint64_t sent_ms, on_ms, retry_at;
    uint32_t sessions, events, dropped, rearmed, timeouts, errors, silent;
} PasStream;

void pas_defaults(PasConfig *c);
void pas_init(PasStream *s, const PasConfig *c);
/* The consumer's conditions (repeating a value is harmless). */
void pas_want(PasStream *s, int want, uint64_t now_ms);
/* The request to send now (PAS_ACT_*). Ask the guard first; send it and report pas_sent only if the guard gave a token. */
int pas_action(const PasStream *s, uint64_t now_ms);
void pas_sent(PasStream *s, int action, uint64_t now_ms);
/* The answer to the request out (QMI result ok or not). The kext matches answers to requests by their QMI transaction id and passes
 * only the outstanding one's; without a request out (an answer after its timeout) it is ignored. */
void pas_response(PasStream *s, int ok, uint64_t now_ms);
/* An event (an indication with this client's samples): PAS_EV_DROP, PAS_EV_USE, PAS_EV_FIRST. */
int pas_event(PasStream *s, uint64_t now_ms);
/* Time: response timeouts and the first-event watchdog. Call at pas_deadline (and whenever convenient). */
void pas_tick(PasStream *s, uint64_t now_ms);
/* When pas_tick or pas_action has something to do next (absolute ms), 0 when only an input can change anything. */
uint64_t pas_deadline(const PasStream *s, uint64_t now_ms);
const char *pas_state_name(int state);
const char *pas_action_name(int action);

#ifdef __cplusplus
}
#endif
#endif
