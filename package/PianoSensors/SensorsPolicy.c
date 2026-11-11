/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* Piano's sensor policies: see SensorsPolicy.h. */
#include "SensorsPolicy.h"

/* Battery first (the owner's priority, backlight.md): a dark room near the panel's floor, an office (300-500 lux) around 40 %, full
 * scale only in daylight. Owner-tunable later through PianoBacklight's offset (the brightness keys shift the curve). */
const PsCurvePoint ps_als_curve[] = {
    {0, 40},     {5, 60},     {20, 110},   {50, 170},   {100, 240},  {200, 320},
    {400, 420},  {800, 540},  {1500, 660}, {3000, 800}, {6000, 920}, {10000, 1000},
};
const unsigned ps_als_curve_points = sizeof ps_als_curve / sizeof ps_als_curve[0];

uint32_t ps_als_curve_permille(uint32_t lux)
{
    if (lux <= ps_als_curve[0].lux) return ps_als_curve[0].permille;
    for (unsigned i = 1; i < ps_als_curve_points; ++i) {
        const PsCurvePoint a = ps_als_curve[i - 1], b = ps_als_curve[i];
        if (lux <= b.lux) {
            const uint64_t span = b.lux - a.lux, at = lux - a.lux;
            return a.permille + (uint32_t)(((uint64_t)(b.permille - a.permille) * at + span / 2) / span);
        }
    }
    return ps_als_curve[ps_als_curve_points - 1].permille;
}

void ps_als_init(PsAls *a)
{
    a->has = 0;
    a->goal = 0;
    a->smooth_q16 = 0;
    a->published = 0xffffffffu;
    a->last_ms = a->published_ms = 0;
}

static uint32_t distance(uint32_t a, uint32_t b) {return a > b ? a - b : b - a;}

int32_t ps_als_step(PsAls *a, int have_sample, int32_t milli_lux, uint64_t now_ms)
{
    if (have_sample) {
        const uint32_t lux = milli_lux <= 0 ? 0 : (uint32_t)((milli_lux + 500) / 1000);
        const uint32_t target = ps_als_curve_permille(lux);
        if (!a->has) {
            a->has = 1;
            a->goal = target;
            a->smooth_q16 = target << 16;
            a->last_ms = a->published_ms = now_ms;
            a->published = target;
            return (int32_t)target;
        }
        if (distance(target, a->goal) >= PS_ALS_HYSTERESIS) a->goal = target;
    }
    if (!a->has) return -1;
    if (now_ms > a->last_ms) {
        const uint64_t dt = now_ms - a->last_ms;
        const uint64_t tau = (a->goal << 16) > a->smooth_q16 ? PS_ALS_UP_MS : PS_ALS_DOWN_MS;
        const int64_t alpha = (int64_t)((dt << 16) / (tau + dt));            /* 0 .. 65536 */
        const int64_t gap = (int64_t)((uint64_t)a->goal << 16) - (int64_t)a->smooth_q16;
        int64_t next = (int64_t)a->smooth_q16 + gap * alpha / 65536;
        const int64_t left = ((int64_t)a->goal << 16) - next;
        if (left > -(2 << 16) && left < (2 << 16)) next = (int64_t)a->goal << 16;   /* within two permille: there */
        a->smooth_q16 = (uint32_t)next;
        a->last_ms = now_ms;
    }
    const uint32_t value = (a->smooth_q16 + 0x8000u) >> 16;
    if (value == a->published || now_ms - a->published_ms < PS_ALS_PUBLISH_MS) return -1;
    if (distance(value, a->published) < PS_ALS_STEP && value != a->goal) return -1;
    a->published = value;
    a->published_ms = now_ms;
    return (int32_t)value;
}

int ps_edge_of(int sno_state)
{
    switch (sno_state) {
    case SNO_PORTRAIT: return PS_EDGE_TOP;            /* in the panel frame: gravity along -y, the camera edge up (docked) */
    case SNO_LANDSCAPE_LEFT: return PS_EDGE_LEFT;
    case SNO_UPSIDE_DOWN: return PS_EDGE_BOTTOM;
    case SNO_LANDSCAPE_RIGHT: return PS_EDGE_RIGHT;
    }
    return PS_EDGE_NONE;
}

const char *ps_edge_name(int edge)
{
    switch (edge) {
    case PS_EDGE_TOP: return "top";
    case PS_EDGE_LEFT: return "left";
    case PS_EDGE_BOTTOM: return "bottom";
    case PS_EDGE_RIGHT: return "right";
    }
    return "none";
}

int ps_accel_sample(SnoState *s, const SfxAxes *axes, const uint32_t bits[3], uint64_t now_ms)
{
    int32_t in[3], out[3];
    for (unsigned i = 0; i < 3; ++i) in[i] = sfx_accel_ios(bits[i]);
    sfx_axes_apply(axes, in, out);
    return sno_update(s, out[0], out[1], out[2], now_ms);
}

void ps_rotation_defaults(PsRotationConfig *c)
{
    c->degrees[PS_EDGE_TOP] = 0;
    c->degrees[PS_EDGE_LEFT] = 90;
    c->degrees[PS_EDGE_BOTTOM] = 180;
    c->degrees[PS_EDGE_RIGHT] = 270;
    c->settle_ms = 600;
    c->min_interval_ms = 1000;
}

void ps_rotation_init(PsRotation *r)
{
    r->rotation = -1;
    r->edge = PS_EDGE_NONE;
    r->edge_since = r->changed_ms = 0;
}

int ps_rotation_step(PsRotation *r, const PsRotationConfig *c, int edge, int attached, int locked, uint64_t now_ms)
{
    if (attached) {
        r->edge = PS_EDGE_NONE;          /* after a detach the edge must settle afresh */
        if (r->rotation == 0) return -1;
        r->rotation = 0;
        r->changed_ms = now_ms;
        return 0;
    }
    if (edge < PS_EDGE_TOP || edge > PS_EDGE_RIGHT) {r->edge = PS_EDGE_NONE; return -1;}
    if (edge != r->edge) {
        r->edge = edge;
        r->edge_since = now_ms;
        return -1;
    }
    if (locked || now_ms - r->edge_since < c->settle_ms) return -1;
    const int want = c->degrees[edge];
    if (want == r->rotation) return -1;
    if (r->rotation >= 0 && now_ms - r->changed_ms < c->min_interval_ms) return -1;
    r->rotation = want;
    r->changed_ms = now_ms;
    return want;
}
