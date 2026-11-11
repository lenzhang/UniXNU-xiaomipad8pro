/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The display rotation agent's policy: see AutoRotate.h. */
#include "AutoRotate.h"

void par_defaults(ParConfig *c)
{
    ps_rotation_defaults(&c->rot);
    c->detach_hold_ms = 1500;
    c->verify_ms = 2000;
    c->retry_first_ms = 2000;
    c->retry_max_ms = 60000;
    c->give_up = 5;
    c->touch_retry_ms = 5000;
    c->cover_lock = PAR_LOCK_HOLD;
    c->unknown_cover_attached = 0;
}

static int attached_value(const ParAgent *a, int cover)
{
    return cover == PAR_COVER_ATTACHED || (cover == PAR_COVER_UNKNOWN && a->cfg.unknown_cover_attached);
}

void par_init(ParAgent *a, const ParConfig *c)
{
    a->cfg = *c;
    ps_rotation_init(&a->rot);
    a->cover = PAR_COVER_UNKNOWN;
    a->hold_until = 0;
    a->force = a->cfg.cover_lock != PAR_LOCK_IGNORE && attached_value(a, PAR_COVER_UNKNOWN);
    a->edge = PS_EDGE_NONE;
    a->locked = 0;
    a->shown = a->asked = -1;
    a->asked_ms = 0;
    a->manual = 0;
    a->manual_edge = PS_EDGE_NONE;
    a->failures = a->backoff_ms = 0;
    a->retry_at = 0;
    a->gave_up = 0;
    a->touch = -1;
    a->touch_retry_at = 0;
    a->applies = a->confirmed = a->refused = a->unconfirmed = a->manual_changes = 0;
}

int par_valid_degrees(int degrees)
{
    return degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270;
}

int par_edge_of_degrees(const ParConfig *c, int degrees)
{
    for (int e = PS_EDGE_TOP; e <= PS_EDGE_RIGHT; ++e)
        if (c->rot.degrees[e] == degrees) return e;
    return PS_EDGE_NONE;
}

/* The cover now: 1 attached (0 degrees wanted), 2 detached a moment ago (the sensor does not count yet), 0 not in the way. */
static int cover_now(const ParAgent *a, uint64_t now)
{
    if (a->cfg.cover_lock == PAR_LOCK_IGNORE) return 0;
    if (attached_value(a, a->cover)) return 1;
    if (a->hold_until && now < a->hold_until) return 2;
    return 0;
}

void par_cover(ParAgent *a, int cover, uint64_t now_ms)
{
    if (cover != PAR_COVER_ATTACHED && cover != PAR_COVER_DETACHED) cover = PAR_COVER_UNKNOWN;
    if (cover == a->cover) return;
    const int was = attached_value(a, a->cover), is = attached_value(a, cover);
    a->cover = cover;
    if (is && !was) {
        a->force = a->cfg.cover_lock != PAR_LOCK_IGNORE;
        a->hold_until = 0;
    } else if (was && !is) {
        a->force = 0;
        a->hold_until = now_ms + a->cfg.detach_hold_ms;
    }
    a->rot.edge = PS_EDGE_NONE;          /* after any change of the cover an edge settles afresh */
}

void par_edge(ParAgent *a, int edge, uint64_t now_ms)
{
    (void)now_ms;
    if (edge < PS_EDGE_TOP || edge > PS_EDGE_RIGHT) edge = PS_EDGE_NONE;
    a->edge = edge;
    if (a->manual && edge != PS_EDGE_NONE && edge != a->manual_edge) a->manual = 0;   /* turned to another edge: automatic again */
}

void par_lock(ParAgent *a, int locked)
{
    a->locked = locked != 0;
}

static void failed(ParAgent *a, uint64_t now)
{
    ++a->failures;
    if (!a->backoff_ms) a->backoff_ms = a->cfg.retry_first_ms;
    else a->backoff_ms = a->backoff_ms > a->cfg.retry_max_ms / 2 ? a->cfg.retry_max_ms : a->backoff_ms * 2;
    a->retry_at = now + a->backoff_ms;
    if (a->failures >= a->cfg.give_up) a->gave_up = 1;
    a->rot.rotation = a->shown;
}

void par_observed(ParAgent *a, int degrees, uint64_t now_ms)
{
    if (!par_valid_degrees(degrees)) return;
    if (a->asked >= 0) {
        if (degrees != a->asked) return;   /* still the old rotation (or one on the way): the verify deadline decides */
        a->asked = -1;
        a->shown = degrees;
        a->rot.rotation = degrees;
        a->failures = a->backoff_ms = 0;
        a->retry_at = 0;
        a->manual = 0;
        ++a->confirmed;
        if (degrees == 0 && a->cfg.cover_lock == PAR_LOCK_AT_ATTACH) a->force = 0;
        return;
    }
    if (degrees == a->shown) return;
    if (a->shown >= 0) {                  /* not ours: System Settings, or another program */
        a->manual = 1;
        a->manual_edge = a->edge;
        ++a->manual_changes;
        a->rot.changed_ms = now_ms;
        if (a->cfg.cover_lock == PAR_LOCK_AT_ATTACH) a->force = 0;
    }
    a->shown = degrees;
    a->rot.rotation = degrees;
}

void par_applied(ParAgent *a, int degrees, int ok, uint64_t now_ms)
{
    if (ok && par_valid_degrees(degrees)) {
        a->asked = degrees;
        a->asked_ms = now_ms;
        ++a->applies;
        a->rot.rotation = degrees;
        a->rot.changed_ms = now_ms;
        return;
    }
    ++a->refused;
    failed(a, now_ms);
}

void par_touch_done(ParAgent *a, int degrees, int ok, uint64_t now_ms)
{
    if (ok) a->touch = degrees;
    else a->touch_retry_at = now_ms + a->cfg.touch_retry_ms;
}

ParDecision par_step(ParAgent *a, uint64_t now_ms, int can_apply)
{
    ParDecision d = {-1, -1, -1, "nothing to do"};
    if (a->shown >= 0 && a->touch != a->shown && now_ms >= a->touch_retry_at) d.touch = a->shown;
    if (a->asked >= 0) {
        if (now_ms - a->asked_ms < a->cfg.verify_ms) {
            d.why = "waiting for the rotation to show";
            return d;
        }
        a->asked = -1;
        ++a->unconfirmed;
        failed(a, now_ms);
        d.why = "the rotation asked for did not show: backing off";
        return d;
    }
    if (a->gave_up) {
        d.why = "automatic rotation given up after repeated failures";
        return d;
    }
    if (a->shown < 0) {
        d.why = "the rotation shown is not known yet";
        return d;
    }
    const int cover = cover_now(a, now_ms);
    const uint64_t changed = a->rot.changed_ms;   /* a rotation wanted but not applied now does not start the minimum interval */
    if (cover == 1) {
        a->rot.edge = PS_EDGE_NONE;
        d.why = "the keyboard cover is attached";
        if (!a->force || a->shown == 0) {
            if (a->shown == 0 && a->cfg.cover_lock == PAR_LOCK_AT_ATTACH) a->force = 0;
            return d;
        }
        d.want = 0;
        if (now_ms - a->rot.changed_ms < a->cfg.rot.min_interval_ms) {
            d.why = "the keyboard cover is attached: 0 after the minimum interval";
            return d;
        }
    } else if (cover == 2) {
        a->rot.edge = PS_EDGE_NONE;
        d.why = "the keyboard cover was just detached";
        return d;
    } else {
        const int r = ps_rotation_step(&a->rot, &a->cfg.rot, a->edge, 0, a->locked || a->manual, now_ms);
        if (r < 0 || r == a->shown) {
            a->rot.rotation = a->shown;
            d.why = a->locked ? "rotation locked" : a->manual ? "a rotation chosen by hand holds until the tablet turns"
                  : a->edge == PS_EDGE_NONE ? "no edge up (flat, or no sensor)" : "nothing to do";
            return d;
        }
        d.want = r;
        d.why = "the tablet turned";
    }
    if (!can_apply || now_ms < a->retry_at) {
        a->rot.rotation = a->shown;
        a->rot.changed_ms = changed;
        d.why = !can_apply ? "wanted, but not now (session, display asleep, or observing only)" : "wanted, backing off after a failure";
        return d;
    }
    d.apply = d.want;
    return d;
}

uint64_t par_deadline(const ParAgent *a, uint64_t now_ms)
{
    uint64_t best = 0;
#define TAKE(t) do { const uint64_t t_ = (t); if (t_ && (!best || t_ < best)) best = t_; } while (0)
    if (a->shown >= 0 && a->touch != a->shown) TAKE(a->touch_retry_at > now_ms ? a->touch_retry_at : now_ms);
    if (a->asked >= 0) {
        const uint64_t verify = a->asked_ms + a->cfg.verify_ms;   /* read it back every 250 ms until then */
        TAKE(now_ms + 250 < verify ? now_ms + 250 : verify);
    } else if (!a->gave_up && a->shown >= 0) {
        const int cover = cover_now(a, now_ms);
        uint64_t at = 0;
        if (cover == 1) {
            if (a->force && a->shown != 0) at = a->rot.changed_ms + a->cfg.rot.min_interval_ms;
        } else if (cover == 2) {
            at = a->hold_until;
        } else if (!a->locked && !a->manual && a->edge != PS_EDGE_NONE && a->cfg.rot.degrees[a->edge] != a->shown) {
            const uint64_t settle = a->rot.edge == a->edge ? a->rot.edge_since + a->cfg.rot.settle_ms : now_ms;
            const uint64_t interval = a->rot.rotation >= 0 ? a->rot.changed_ms + a->cfg.rot.min_interval_ms : 0;
            at = settle > interval ? settle : interval;
        }
        if (at) TAKE(at > a->retry_at ? at : a->retry_at);
    }
#undef TAKE
    return best && best < now_ms ? now_ms : best;
}
