/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The display rotation agent's policy (piano-rotated, docs/drivers/sensors.md section 5a): the rotation WindowServer should show and
 * the one PianoTouch is told, from the panel edge PianoSensors reports up, the keyboard cover (PianoKeyboard's piano-keyboard-attached),
 * the user's rotation lock and what WindowServer really shows (CGDisplayRotation, read back). SensorsPolicy's ps_rotation_step keeps the
 * settle time, the minimum interval and the lock; this adds what a real display needs around it:
 *   - the cover: attached forces 0 degrees at once (Ubuntu's rule: docked is the normal landscape) and, by default, keeps it there (a
 *     rotation made while attached is turned back, PAR_LOCK_HOLD); detached, the sensor counts again only after detach_hold_ms (a
 *     cover being put on or taken off makes and breaks its contacts) and an edge that settled afresh;
 *   - what is shown: every rotation asked for must be read back within verify_ms; a refused or unconfirmed one backs off (2 s, doubling
 *     to 60 s) and give_up failures in a row end automatic rotation for the session; a rotation made by someone else (System Settings)
 *     is adopted, and automatic rotation then waits until the tablet is turned to another edge;
 *   - touch: PianoTouch is told the rotation shown, never one only asked for, again after every change (TouchRotation.h).
 * Rotations are CGDisplayRotation's degrees (clockwise; 90 = the panel's left edge up). Portable C, integer only, no C library;
 * host-tested (tests/auto_host.c). */
#ifndef PIANO_AUTO_ROTATE_H
#define PIANO_AUTO_ROTATE_H
#include <stdint.h>
#include "SensorsPolicy.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {PAR_COVER_UNKNOWN = -1, PAR_COVER_DETACHED = 0, PAR_COVER_ATTACHED = 1};
enum {
    PAR_LOCK_IGNORE = 0,        /* the cover is not looked at */
    PAR_LOCK_AT_ATTACH = 1,     /* attaching the cover turns the display to 0; a rotation chosen while attached stays */
    PAR_LOCK_HOLD = 2,          /* 0 for as long as the cover is attached (the default) */
};

typedef struct {
    PsRotationConfig rot;          /* degrees per edge, settle_ms (600), min_interval_ms (1000): ps_rotation_defaults */
    uint32_t detach_hold_ms;       /* 1500 */
    uint32_t verify_ms;            /* 2000: a rotation not read back by then has failed */
    uint32_t retry_first_ms, retry_max_ms;   /* 2000, 60000 */
    uint32_t give_up;              /* 5 failures in a row: automatic rotation off until the agent restarts */
    uint32_t touch_retry_ms;       /* 5000: PianoTouch not there or refusing */
    int cover_lock;                /* PAR_LOCK_HOLD */
    int unknown_cover_attached;    /* 0: no PianoKeyboard (or no answer) counts as detached */
} ParConfig;

typedef struct {
    ParConfig cfg;
    PsRotation rot;                /* SensorsPolicy's state; rot.rotation follows what is shown */
    int cover;                     /* PAR_COVER_* as last reported */
    uint64_t hold_until;           /* detached: the sensor counts again from then (0: no hold) */
    int force;                     /* attached: 0 is wanted */
    int edge;                      /* PS_EDGE_* as last reported */
    int locked;                    /* the user's rotation lock */
    int shown;                     /* degrees WindowServer shows, -1 before the first reading */
    int asked;                     /* degrees asked for and not read back yet, -1 none */
    uint64_t asked_ms;
    int manual, manual_edge;       /* someone else rotated: automatic rotation waits for an edge other than manual_edge */
    uint32_t failures, backoff_ms;
    uint64_t retry_at;
    int gave_up;
    int touch;                     /* degrees PianoTouch was told, -1 none */
    uint64_t touch_retry_at;
    uint32_t applies, confirmed, refused, unconfirmed, manual_changes;
} ParAgent;

typedef struct {
    int apply;                     /* -1, or the degrees to ask WindowServer for now (then report par_applied) */
    int want;                      /* -1, or the degrees the policy wants (also when it may not apply them now: logs, dry runs) */
    int touch;                    /* -1, or the degrees to tell PianoTouch now (then report par_touch_done) */
    const char *why;               /* one phrase for the log */
} ParDecision;

void par_defaults(ParConfig *c);
void par_init(ParAgent *a, const ParConfig *c);
/* Inputs, each when it changes (repeating a value is harmless). */
void par_cover(ParAgent *a, int cover, uint64_t now_ms);
void par_edge(ParAgent *a, int edge, uint64_t now_ms);
void par_lock(ParAgent *a, int locked);
/* What WindowServer shows (after a reconfiguration, and while a rotation is awaited). */
void par_observed(ParAgent *a, int degrees, uint64_t now_ms);
/* The backend's answer to an apply: ok = it took the request (the reading back confirms it later). */
void par_applied(ParAgent *a, int degrees, int ok, uint64_t now_ms);
void par_touch_done(ParAgent *a, int degrees, int ok, uint64_t now_ms);
/* What to do now. can_apply: the agent may rotate at all now (its session is on the console, the display is awake, a backend). */
ParDecision par_step(ParAgent *a, uint64_t now_ms, int can_apply);
/* When par_step has something to do next (absolute ms), or 0 when only an input can change anything. */
uint64_t par_deadline(const ParAgent *a, uint64_t now_ms);
/* The edge a rotation corresponds to under the map (PS_EDGE_NONE when none does). */
int par_edge_of_degrees(const ParConfig *c, int degrees);
int par_valid_degrees(int degrees);

#ifdef __cplusplus
}
#endif
#endif
