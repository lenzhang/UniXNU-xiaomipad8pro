/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* The safety governor of piano's sensors: see SensorsGuard.h. */
#include "SensorsGuard.h"

void sg_defaults(SgConfig *c)
{
    c->settle_ms = 20000;
    c->backoff_first_ms = 1000;
    c->backoff_max_ms = 64000;
    c->strikes = 6;
    c->window_ms = 600000;
    c->bucket = 8;
    c->refill_per_s = 4;
    c->file_calls_per_min = 3000;
}

void sg_init(SgGuard *g, const SgConfig *config)
{
    uint8_t *p = (uint8_t *)g;
    for (uint32_t i = 0; i < sizeof *g; ++i) p[i] = 0;
    if (config) g->cfg = *config;
    else sg_defaults(&g->cfg);
    if (g->cfg.strikes < 1) g->cfg.strikes = 1;
    if (g->cfg.strikes > 16) g->cfg.strikes = 16;
    if (g->cfg.bucket < 1) g->cfg.bucket = 1;
    if (g->cfg.backoff_first_ms < 1) g->cfg.backoff_first_ms = 1;
    if (g->cfg.backoff_max_ms < g->cfg.backoff_first_ms) g->cfg.backoff_max_ms = g->cfg.backoff_first_ms;
    g->state = SG_WAIT;
    g->tokens_milli = g->cfg.bucket * 1000u;
}

static void off(SgGuard *g, int reason)
{
    if (g->state == SG_OFF) return;
    g->state = SG_OFF;
    g->reason = reason;
}

/* RUN or BACKOFF by the clock (only once started and not quiet). */
static int running_state(const SgGuard *g, uint64_t now_ms)
{
    return now_ms < g->backoff_until ? SG_BACKOFF : SG_RUN;
}

int sg_observe(SgGuard *g, const SgDsp *d, uint64_t now_ms)
{
    if (g->state == SG_OFF) return SG_OFF;
    /* A dead or dying DSP is never touched, before or after we started. */
    if (d->fatal) {off(g, SG_REASON_FATAL); return SG_OFF;}
    if (d->crash && d->crash[0]) {off(g, SG_REASON_CRASH); return SG_OFF;}
    if (g->started) {
        if (!d->dsp_up || d->link_generation != g->link0) {off(g, SG_REASON_LINK); return SG_OFF;}
        if (d->charger_state != SG_CHARGER_UP || d->charger_ups != g->charger_ups0) {off(g, SG_REASON_CHARGER); return SG_OFF;}
        g->state = d->quiet ? SG_WAIT : running_state(g, now_ms);
        return g->state;
    }
    if (!d->dsp_up || d->charger_state != SG_CHARGER_UP || d->quiet) {
        g->up_seen = 0;
        g->state = SG_WAIT;
        return SG_WAIT;
    }
    /* the settling counts from the charger's newest generation (a restart we did not see as a down starts it over) */
    if (!g->up_seen || d->charger_ups != g->charger_ups0) {
        g->up_seen = 1;
        g->up_since = now_ms;
        g->charger_ups0 = d->charger_ups;
    }
    if (now_ms - g->up_since < g->cfg.settle_ms) {
        g->state = SG_WAIT;
        return SG_WAIT;
    }
    g->started = 1;
    g->link0 = d->link_generation;
    g->refill_at = now_ms;
    g->state = running_state(g, now_ms);
    return g->state;
}

int sg_may_request(SgGuard *g, uint64_t now_ms)
{
    if (g->state == SG_BACKOFF && now_ms >= g->backoff_until) g->state = SG_RUN;
    if (g->state != SG_RUN) {g->refused++; return 0;}
    if (now_ms > g->refill_at) {
        const uint64_t add = (now_ms - g->refill_at) * g->cfg.refill_per_s;   /* thousandths of a token */
        const uint64_t full = (uint64_t)g->cfg.bucket * 1000u, sum = g->tokens_milli + add;
        g->tokens_milli = (uint32_t)(sum < full ? sum : full);
    }
    g->refill_at = now_ms;
    if (g->tokens_milli < 1000u) {g->refused++; return 0;}
    g->tokens_milli -= 1000u;
    g->requests++;
    return 1;
}

void sg_result(SgGuard *g, int ok, uint64_t now_ms)
{
    if (g->state == SG_OFF) return;
    if (ok) {g->backoff_ms = 0; return;}
    g->failures++;
    const uint32_t n = g->cfg.strikes;
    g->strike_at[g->strike_next] = now_ms;
    g->strike_next = (g->strike_next + 1) % n;
    if (g->strike_count < n) g->strike_count++;
    /* the slot to be written next holds the oldest of the last n failures */
    if (g->strike_count == n && now_ms - g->strike_at[g->strike_next % n] <= g->cfg.window_ms) {off(g, SG_REASON_ERRORS); return;}
    const uint32_t doubled = g->backoff_ms ? g->backoff_ms * 2u : g->cfg.backoff_first_ms;
    g->backoff_ms = doubled > g->cfg.backoff_max_ms || doubled < g->backoff_ms ? g->cfg.backoff_max_ms : doubled;
    g->backoff_until = now_ms + g->backoff_ms;
    if (g->state == SG_RUN) g->state = SG_BACKOFF;
}

void sg_file_call(SgGuard *g, uint64_t now_ms)
{
    g->file_calls++;
    if (g->state == SG_OFF) return;
    if (!g->minute_calls || now_ms - g->minute_start >= 60000u) {
        g->minute_start = now_ms;
        g->minute_calls = 0;
    }
    if (++g->minute_calls > g->cfg.file_calls_per_min) off(g, SG_REASON_FILE_STORM);
}

int sg_serve_files(const SgGuard *g)
{
    return g->state != SG_OFF;
}

void sg_disable(SgGuard *g)
{
    off(g, SG_REASON_DISABLED);
}

const char *sg_state_name(int state)
{
    switch (state) {
    case SG_WAIT: return "wait";
    case SG_RUN: return "run";
    case SG_BACKOFF: return "backoff";
    case SG_OFF: return "off";
    }
    return "?";
}

const char *sg_reason_name(int reason)
{
    switch (reason) {
    case SG_REASON_NONE: return "none";
    case SG_REASON_FATAL: return "the DSP's fatal bit";
    case SG_REASON_CRASH: return "a DSP crash reason";
    case SG_REASON_CHARGER: return "the charger domain restarted";
    case SG_REASON_LINK: return "the DSP link went down";
    case SG_REASON_ERRORS: return "repeated request failures";
    case SG_REASON_FILE_STORM: return "the DSP asks for files in a loop";
    case SG_REASON_DISABLED: return "disabled by the owner";
    }
    return "?";
}

static int contains(const char *text, const char *word)
{
    for (; *text; ++text) {
        unsigned i = 0;
        while (word[i] && text[i] == word[i]) ++i;
        if (!word[i]) return 1;
    }
    return 0;
}

int sg_crash_is_sensors(const char *crash)
{
    if (!crash) return 0;
    return contains(crash, "qsh_process") || contains(crash, "sns_") || contains(crash, "sensor");
}
