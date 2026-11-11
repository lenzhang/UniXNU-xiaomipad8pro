/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The touch screen's coordinates under a display rotation: what PianoTouch applies to each contact before its HID report
 * (docs/drivers/sensors.md section 5a). Header only, C and C++, integer only, no C library (kernel code, -mgeneral-regs-only).
 *
 * The rotation is the one CoreGraphics reports for the panel's display, CGDisplayRotation() (the public CGDisplayConfiguration.h:
 * "the rotation angle of a display in degrees clockwise. A display rotation of 90 degrees implies the display is rotated clockwise
 * 90 degrees, such that what was the physical bottom of the display is now the left side, and what was the physical top is now the
 * right side"). Its source is piano-rotated, which reads it back after every change (it never tells PianoTouch a rotation that was
 * only asked for).
 *
 * Panel coordinates are PianoTouch's own (TouchMatrix.h): x 0..w-1 left to right, y 0..h-1 top to bottom, the panel's top edge being
 * the framebuffer's first row (the edge that is up with the keyboard cover docked: SensorsPolicy.h's PS_EDGE_TOP). The result is the
 * contact in the rotated desktop, w x h at 0 and 180 degrees, h x w at 90 and 270; the HID report scales x over (width - 1) and y over
 * (height - 1) of that size (TouchHid.h tabletReport), and macOS stretches an absolute pointer's logical range over the display:
 *
 *   rotation   the panel edge that is up   desktop x       desktop y       desktop size
 *   0          top                         x               y               w x h
 *   90         left                        h - 1 - y       x               h x w
 *   180        bottom                      w - 1 - x       h - 1 - y       w x h
 *   270        right                       y               w - 1 - x       h x w
 *
 * A new rotation takes effect at the next touch-down (pat_latch), never in the middle of a drag: a held contact keeps the mapping it
 * started with, so a rotation during a drag cannot make the pointer jump across the screen. */
#ifndef PIANO_AUTO_TOUCH_ROTATION_H
#define PIANO_AUTO_TOUCH_ROTATION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The names PianoTouch publishes (docs/drivers/sensors.md section 5a, "PianoTouch"). */
#define PAT_FUNCTION "PianoTouchRotation"        /* callPlatformFunction: param1 = degrees (uintptr_t), param2 = the caller (C string) */
#define PAT_PROPERTY "piano-touch-rotation"      /* setProperties (root or the console user): 0, 90, 180, 270; also published */

typedef struct {
    int32_t x, y;                 /* the contact in the rotated desktop */
    int32_t width, height;        /* the rotated desktop's size */
} PatPoint;

static inline int pat_valid(uint32_t degrees)
{
    return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

static inline int32_t pat_clamp(int32_t v, int32_t size)
{
    return v < 0 ? 0 : v > size - 1 ? size - 1 : v;
}

/* Panel (x, y) of a w x h panel to the desktop shown at degrees (anything but 0/90/180/270 counts as 0). */
static inline PatPoint pat_rotate(uint32_t degrees, int32_t x, int32_t y, int32_t w, int32_t h)
{
    PatPoint p;
    x = pat_clamp(x, w);
    y = pat_clamp(y, h);
    switch (pat_valid(degrees) ? degrees : 0) {
    case 90:
        p.x = h - 1 - y; p.y = x; p.width = h; p.height = w;
        break;
    case 180:
        p.x = w - 1 - x; p.y = h - 1 - y; p.width = w; p.height = h;
        break;
    case 270:
        p.x = y; p.y = w - 1 - x; p.width = h; p.height = w;
        break;
    default:
        p.x = x; p.y = y; p.width = w; p.height = h;
        break;
    }
    return p;
}

/* The inverse (desktop at degrees -> panel), for tests and logs. */
static inline PatPoint pat_unrotate(uint32_t degrees, int32_t dx, int32_t dy, int32_t w, int32_t h)
{
    PatPoint p;
    p.width = w;
    p.height = h;
    switch (pat_valid(degrees) ? degrees : 0) {
    case 90:
        p.x = pat_clamp(dy, w); p.y = h - 1 - pat_clamp(dx, h);
        break;
    case 180:
        p.x = w - 1 - pat_clamp(dx, w); p.y = h - 1 - pat_clamp(dy, h);
        break;
    case 270:
        p.x = w - 1 - pat_clamp(dy, w); p.y = pat_clamp(dx, h);
        break;
    default:
        p.x = pat_clamp(dx, w); p.y = pat_clamp(dy, h);
        break;
    }
    return p;
}

/* The rotation PianoTouch's thread uses. requested is written by callPlatformFunction / setProperties (any thread, one aligned 32-bit
 * store), shown only by the frame thread: pat_latch at each contact's report, with down = 1 for the touch-down. */
typedef struct {
    volatile uint32_t requested;
    uint32_t shown;
    uint32_t changes;
} PatRotation;

static inline void pat_init(PatRotation *r)
{
    r->requested = 0;
    r->shown = 0;
    r->changes = 0;
}

/* 1 when taken, 0 for a value that is not a rotation (ignored). */
static inline int pat_request(PatRotation *r, uint32_t degrees)
{
    if (!pat_valid(degrees)) return 0;
    r->requested = degrees;
    return 1;
}

static inline uint32_t pat_latch(PatRotation *r, int down)
{
    if (down) {
        const uint32_t want = r->requested;
        if (pat_valid(want) && want != r->shown) {
            r->shown = want;
            ++r->changes;
        }
    }
    return r->shown;
}

#ifdef __cplusplus
}
#endif
#endif
