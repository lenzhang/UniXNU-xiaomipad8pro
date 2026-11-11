/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The ambient light chain: see AutoLight.h. */
#include "AutoLight.h"

void pal_init(PalLight *l, const PasConfig *c)
{
    pas_init(&l->stream, c);
    ps_als_init(&l->als);
    l->enabled = l->display_on = 0;
    l->lid = -1;
    l->tick_at = 0;
    l->target = -1;
    l->lux = 0;
    l->samples = l->targets = l->restarts = 0;
}

int pal_settled(const PalLight *l)
{
    const PsAls *a = &l->als;
    return !a->has || (a->smooth_q16 == a->goal << 16 && a->published == a->goal);
}

static int32_t took(PalLight *l, int32_t target, uint64_t now)
{
    l->tick_at = pal_settled(l) ? 0 : now + PS_ALS_PUBLISH_MS;
    if (target >= 0) {
        l->target = target;
        ++l->targets;
    }
    return target;
}

void pal_conditions(PalLight *l, int enabled, int display_on, int lid, uint64_t now_ms)
{
    l->enabled = enabled != 0;
    l->display_on = display_on != 0;
    l->lid = lid == 0 || lid == 1 ? lid : -1;
    const int want = l->enabled && l->display_on && l->lid != 0;
    pas_want(&l->stream, want, now_ms);
    if (!want) l->tick_at = 0;
}

int32_t pal_event(PalLight *l, uint32_t lux_bits, uint64_t now_ms)
{
    const int e = pas_event(&l->stream, now_ms);
    if (e == PAS_EV_DROP) return -1;
    if (e == PAS_EV_FIRST) {
        if (l->samples) ++l->restarts;
        ps_als_init(&l->als);                     /* a new session: start from its first sample */
    }
    ++l->samples;
    const int32_t milli = sfx_milli(lux_bits);
    l->lux = milli <= 0 ? 0 : (uint32_t)((milli + 500) / 1000);
    return took(l, ps_als_step(&l->als, 1, milli, now_ms), now_ms);
}

int32_t pal_tick(PalLight *l, uint64_t now_ms)
{
    pas_tick(&l->stream, now_ms);
    if (!l->tick_at || now_ms < l->tick_at) return -1;
    if (!l->stream.want || l->stream.state != PAS_ON) {
        l->tick_at = 0;
        return -1;
    }
    return took(l, ps_als_step(&l->als, 0, 0, now_ms), now_ms);
}

uint64_t pal_deadline(const PalLight *l, uint64_t now_ms)
{
    const uint64_t s = pas_deadline(&l->stream, now_ms), t = l->tick_at;
    const uint64_t best = !s ? t : !t ? s : s < t ? s : t;
    return best && best < now_ms ? now_ms : best;
}
