/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The ambient light chain of PianoSensors' kext (level S4, docs/drivers/sensors.md section 5a): the hub's ambient_light stream
 * (AutoStream.h: wanted while the consumer is on, the display is on and the cover's lid is open), SensorsPolicy's curve with its
 * smoothing and hysteresis (ps_als_step, unchanged), and the targets for PianoBacklight's automatic mode (the platform function
 * "PianoBacklightAmbient": param1 the target permille, param2 the source, param3 the lux; AmbientBacklight.h).
 *
 * After every off/on of the stream (display sleep, the lid, a re-arm) the light policy starts afresh from the new session's first
 * sample, so the backlight goes straight to the room's level at wake instead of fading from the level before the display slept.
 * The smoothing ticks every PS_ALS_PUBLISH_MS only while the value is still moving, and never while the stream is off: at rest the
 * chain needs no timer at all. Portable C, integer only, no C library; host-tested (tests/stream_tests.h). */
#ifndef PIANO_AUTO_LIGHT_H
#define PIANO_AUTO_LIGHT_H
#include <stdint.h>
#include "AutoStream.h"
#include "SensorsPolicy.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    PasStream stream;
    PsAls als;
    int enabled, display_on, lid;    /* lid: 1 open, 0 closed, -1 unknown (counts as open) */
    uint64_t tick_at;                /* the next smoothing step, 0 when settled or off */
    int32_t target;                  /* the last target handed out (permille), -1 none */
    uint32_t lux;                    /* the last sample's lux */
    uint32_t samples, targets, restarts;
} PalLight;

void pal_init(PalLight *l, const PasConfig *c);
/* enabled: piano-sensors-als (and PianoBacklight takes targets); display_on: NativeGopDisplay's piano-display-power; lid:
 * PianoKeyboard's piano-keyboard-hall (1 open, 0 closed, anything else unknown). */
void pal_conditions(PalLight *l, int enabled, int display_on, int lid, uint64_t now_ms);
/* An ambient_light event of this stream: its first float (lux, IEEE-754 bits). The target permille to hand PianoBacklight now, or -1. */
int32_t pal_event(PalLight *l, uint32_t lux_bits, uint64_t now_ms);
/* The time passes (call at pal_deadline): the stream's timeouts and the smoothing. A target or -1. */
int32_t pal_tick(PalLight *l, uint64_t now_ms);
/* The next time pal_tick (or the stream's pas_action) has something to do, 0 for none. */
uint64_t pal_deadline(const PalLight *l, uint64_t now_ms);
/* The smoothing has reached its goal and published it. */
int pal_settled(const PalLight *l);

#ifdef __cplusplus
}
#endif
#endif
