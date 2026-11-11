/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* One sensor stream's on/off lifecycle: see AutoStream.h. */
#include "AutoStream.h"

void pas_defaults(PasConfig *c)
{
    c->response_ms = 2000;
    c->first_event_ms = 3000;
    c->rearms = 2;
    c->retry_first_ms = 1000;
    c->retry_max_ms = 32000;
    c->failures = 6;
}

void pas_init(PasStream *s, const PasConfig *c)
{
    s->cfg = *c;
    s->state = PAS_OFF;
    s->want = 0;
    s->outstanding = PAS_ACT_NONE;
    s->rearm = s->dirty = s->got_event = s->waiting = 0;
    s->rearms_left = c->rearms;
    s->fails = s->backoff_ms = 0;
    s->sent_ms = s->on_ms = s->retry_at = 0;
    s->sessions = s->events = s->dropped = s->rearmed = s->timeouts = s->errors = s->silent = 0;
}

static void failure(PasStream *s, uint64_t now)
{
    ++s->errors;
    ++s->fails;
    if (!s->backoff_ms) s->backoff_ms = s->cfg.retry_first_ms;
    else s->backoff_ms = s->backoff_ms > s->cfg.retry_max_ms / 2 ? s->cfg.retry_max_ms : s->backoff_ms * 2;
    s->retry_at = now + s->backoff_ms;
    if (s->fails >= s->cfg.failures) s->state = PAS_FAILED;
}

static void success(PasStream *s)
{
    s->fails = s->backoff_ms = 0;
    s->retry_at = 0;
}

void pas_want(PasStream *s, int want, uint64_t now_ms)
{
    (void)now_ms;
    want = want != 0;
    if (want && !s->want) s->rearms_left = s->cfg.rearms;    /* a new period of wanting it */
    s->want = want;
    if (!want && s->outstanding != PAS_ACT_DISABLE) s->rearm = 0;   /* the disable that goes out now is no re-arm */
}

/* What would go out if nothing waited (the backoff, the guard). */
static int wanted_action(const PasStream *s)
{
    if (s->state == PAS_FAILED || s->outstanding != PAS_ACT_NONE) return PAS_ACT_NONE;
    if (s->dirty) return PAS_ACT_DISABLE;
    if (s->state == PAS_OFF) return s->want ? PAS_ACT_ENABLE : PAS_ACT_NONE;
    if (s->state == PAS_ON) return !s->want || s->rearm ? PAS_ACT_DISABLE : PAS_ACT_NONE;
    return PAS_ACT_NONE;
}

int pas_action(const PasStream *s, uint64_t now_ms)
{
    return now_ms < s->retry_at ? PAS_ACT_NONE : wanted_action(s);
}

void pas_sent(PasStream *s, int action, uint64_t now_ms)
{
    if (action == PAS_ACT_ENABLE) {
        s->state = PAS_ENABLING;
        s->got_event = s->waiting = 0;
        ++s->sessions;
    } else if (action == PAS_ACT_DISABLE) {
        s->state = PAS_DISABLING;
        s->waiting = 0;
    } else {
        return;
    }
    s->outstanding = action;
    s->sent_ms = now_ms;
}

void pas_response(PasStream *s, int ok, uint64_t now_ms)
{
    const int was = s->outstanding;
    if (was == PAS_ACT_NONE) return;           /* late (after its timeout) or not ours */
    s->outstanding = PAS_ACT_NONE;
    if (was == PAS_ACT_ENABLE) {
        if (ok) {
            s->state = PAS_ON;
            s->on_ms = now_ms;
            s->waiting = !s->got_event;          /* the first event may have come before the answer */
            success(s);
        } else {
            s->state = PAS_OFF;
            failure(s, now_ms);
        }
        return;
    }
    s->state = PAS_OFF;                          /* a refused disable counts as off too: its late samples are dropped */
    s->dirty = 0;
    s->rearm = 0;
    if (ok) success(s);
    else failure(s, now_ms);
}

int pas_event(PasStream *s, uint64_t now_ms)
{
    (void)now_ms;
    ++s->events;
    if (!s->want || (s->state != PAS_ENABLING && s->state != PAS_ON)) {
        ++s->dropped;
        return PAS_EV_DROP;
    }
    s->waiting = 0;
    if (s->state == PAS_ON) s->rearm = 0;      /* it is alive after all */
    if (!s->got_event) {
        s->got_event = 1;
        return PAS_EV_FIRST;
    }
    return PAS_EV_USE;
}

void pas_tick(PasStream *s, uint64_t now_ms)
{
    if (s->state == PAS_FAILED) return;
    if (s->outstanding != PAS_ACT_NONE) {
        if (now_ms - s->sent_ms < s->cfg.response_ms) return;
        ++s->timeouts;
        s->outstanding = PAS_ACT_NONE;
        s->state = PAS_OFF;
        s->dirty = 1;
        s->rearm = s->waiting = 0;
        failure(s, now_ms);
        return;
    }
    if (s->state == PAS_ON && s->waiting && now_ms - s->on_ms >= s->cfg.first_event_ms) {
        s->waiting = 0;
        if (s->want && s->rearms_left) {
            --s->rearms_left;
            s->rearm = 1;
            ++s->rearmed;
        } else {
            ++s->silent;                         /* no more re-arms in this period: it stays on, waiting quietly */
        }
    }
}

uint64_t pas_deadline(const PasStream *s, uint64_t now_ms)
{
    if (s->state == PAS_FAILED) return 0;
    if (s->outstanding != PAS_ACT_NONE) return s->sent_ms + s->cfg.response_ms;
    uint64_t best = 0;
    if (s->state == PAS_ON && s->waiting) best = s->on_ms + s->cfg.first_event_ms;
    if (wanted_action(s) != PAS_ACT_NONE) {
        const uint64_t at = s->retry_at > now_ms ? s->retry_at : now_ms;
        if (!best || at < best) best = at;
    }
    return best && best < now_ms ? now_ms : best;
}

const char *pas_state_name(int state)
{
    switch (state) {
    case PAS_OFF: return "off";
    case PAS_ENABLING: return "enabling";
    case PAS_ON: return "on";
    case PAS_DISABLING: return "disabling";
    case PAS_FAILED: return "failed";
    }
    return "?";
}

const char *pas_action_name(int action)
{
    return action == PAS_ACT_ENABLE ? "enable" : action == PAS_ACT_DISABLE ? "disable" : "none";
}
