/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioDspLink.h"

static void zero(void *p, unsigned n) {for (unsigned i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}

void adg_init(AudioDspGuard *g, uint32_t settle_ms)
{
    zero(g, sizeof *g);
    g->settle_ms = settle_ms;
    g->state = AdgWait;
}

void adg_trip(AudioDspGuard *g, int reason)
{
    if (g->state == AdgOff) return;
    g->state = AdgOff;
    g->reason = reason ? reason : AdgReasonOwner;
}

int adg_observe(AudioDspGuard *g, const AdgObservation *o, uint32_t now_ms)
{
    g->observations++;
    if (g->state == AdgOff) return AdgOff;
    if (!o->contract_ok) {adg_trip(g, AdgReasonContract); return AdgOff;}
    if (!g->have_base) {g->have_base = 1; g->fatal0 = o->fatal_reports; g->link0 = o->link_generation; g->charger0 = o->charger_generation;}
    if (o->fatal || o->fatal_reports != g->fatal0) {adg_trip(g, AdgReasonFatal); return AdgOff;}
    if (o->crash && o->crash[0]) {adg_trip(g, AdgReasonCrash); return AdgOff;}
    if (g->started) {
        if (o->link_generation != g->link0) {adg_trip(g, AdgReasonLink); return AdgOff;}
        if (o->charger_generation != g->charger0 || o->charger_pd != ADG_CHARGER_UP) {adg_trip(g, AdgReasonCharger); return AdgOff;}
        if (!o->dsp_up || !o->channel_up) {adg_trip(g, AdgReasonLink); return AdgOff;}
        if (o->quiet) {g->holds++; g->state = AdgWait; return AdgWait;}
        g->state = AdgRun;
        return AdgRun;
    }
    /* Not started yet: wait for the DSP, our channel and the charger domain, settled in its current generation. */
    if (!o->dsp_up || !o->channel_up || o->charger_pd != ADG_CHARGER_UP) {g->up_seen = 0; g->state = AdgWait; return AdgWait;}
    if (!g->up_seen || o->charger_generation != g->charger0 || o->link_generation != g->link0) {
        g->up_seen = 1; g->up_since = now_ms; g->charger0 = o->charger_generation; g->link0 = o->link_generation;
    }
    if (o->quiet || now_ms - g->up_since < g->settle_ms) {if (o->quiet) g->holds++; g->state = AdgWait; return AdgWait;}
    g->started = 1;
    g->state = AdgRun;
    return AdgRun;
}

const char *adg_reason(int reason)
{
    switch (reason) {
    case AdgReasonNone: return "none";
    case AdgReasonFatal: return "the DSP reported a fatal error";
    case AdgReasonCrash: return "the DSP left a crash reason";
    case AdgReasonCharger: return "the charger domain restarted or went down";
    case AdgReasonLink: return "the link to the DSP went down";
    case AdgReasonError: return "a command of ours failed";
    case AdgReasonContract: return "PianoPower's health is another version";
    case AdgReasonOwner: return "stopped by the owner";
    default: return "?";
    }
}

void audio_inbox_init(AudioInbox *b) {b->head = b->tail = 0; b->dropped = b->too_long = b->taken = 0;}

int audio_inbox_put(AudioInbox *b, const uint8_t *data, uint32_t length)
{
    if (length > AUDIO_INBOX_BYTES) {__atomic_fetch_add(&b->too_long, 1, __ATOMIC_RELAXED); return -1;}
    const uint32_t head = b->head, tail = __atomic_load_n(&b->tail, __ATOMIC_ACQUIRE);
    if (head - tail >= AUDIO_INBOX_SLOTS) {__atomic_fetch_add(&b->dropped, 1, __ATOMIC_RELAXED); return -1;}
    const uint32_t slot = head % AUDIO_INBOX_SLOTS;
    for (uint32_t i = 0; i < length; ++i) b->data[slot][i] = data[i];
    b->length[slot] = length;
    __atomic_store_n(&b->head, head + 1, __ATOMIC_RELEASE);
    return 0;
}

uint32_t audio_inbox_get(AudioInbox *b, uint8_t *out, uint32_t capacity)
{
    const uint32_t tail = b->tail, head = __atomic_load_n(&b->head, __ATOMIC_ACQUIRE);
    if (head == tail) return 0;
    const uint32_t slot = tail % AUDIO_INBOX_SLOTS;
    uint32_t n = b->length[slot];
    if (n > capacity) n = capacity;
    for (uint32_t i = 0; i < n; ++i) out[i] = b->data[slot][i];
    b->taken++;
    __atomic_store_n(&b->tail, tail + 1, __ATOMIC_RELEASE);
    return n;
}

static const AudioPorts kPorts[AUDIO_PORT_SETS] = {
    {"Ubuntu", "Ubuntu's piano kernel: the DT's service ids, dynamic data ports", 1u, 2u, 0x10000000u},
    {"sheng", "UniXNU-sheng on SM8550: 0x2001, PRM from its service id", 0x2001u, 2u, 0x2001u},
    {"SunUEFI", "Project-SunUEFI's piano note: the downstream service ids", 3u, 7u, 0x10000000u},
};

const AudioPorts *audio_ports(int set) {return set >= 0 && set < AUDIO_PORT_SETS ? &kPorts[set] : 0;}

void audio_link_init(AudioLink *l, uint32_t port, uint32_t max_queries, uint32_t settle_ms)
{
    zero(l, sizeof *l);
    adg_init(&l->guard, settle_ms);
    gpr_spf_init(&l->spf, port, max_queries);
    l->state = AlWaiting;
    l->answer_ms = 1000;
    l->retry_ms = 500;
    l->set = -1;
    l->max_queries = max_queries;
}

void audio_link_init_ports(AudioLink *l, int set, uint32_t try_mask, uint32_t max_queries, uint32_t settle_ms)
{
    if (!audio_ports(set)) set = AudioPortsUbuntu;
    audio_link_init(l, kPorts[set].apm, max_queries, settle_ms);
    l->set = set;
    l->try_mask = try_mask & ((1u << AUDIO_PORT_SETS) - 1u);
    l->tried = 1u << set;
}

/* Out of tries on this set and nobody answered: the next set allowed takes over (1), or there is none (0). */
static int next_set(AudioLink *l)
{
    if (l->set < 0 || l->spf.answers || l->spf.errors) return 0;
    for (int i = 1; i < AUDIO_PORT_SETS; ++i) {
        const int s = (l->set + i) % AUDIO_PORT_SETS;
        if (!(l->try_mask >> s & 1u) || (l->tried >> s & 1u)) continue;
        l->set = s;
        l->tried |= 1u << s;
        l->switches++;
        gpr_spf_init(&l->spf, kPorts[s].apm, l->max_queries);
        l->state = AlWaiting;
        l->retry_armed = 0;
        return 1;
    }
    return 0;
}

static int finished(const AudioLink *l) {return l->state == AlReady || l->state == AlGaveUp || l->state == AlOff;}

uint32_t audio_link_tick(AudioLink *l, const AdgObservation *o, uint32_t now_ms, uint8_t out[GPR_HEADER_BYTES])
{
    const int g = adg_observe(&l->guard, o, now_ms);
    if (g == AdgOff) {l->state = AlOff; return 0;}
    if (finished(l)) return 0;
    if (l->spf.state == SpfWaiting) {
        if (now_ms - l->sent_at >= l->answer_ms) {
            if (gpr_spf_timeout(&l->spf) == SpfDone && !next_set(l)) {l->state = AlGaveUp; return 0;}
            l->retry_at = now_ms + l->retry_ms; l->retry_armed = 1;
        }
        return 0;
    }
    if (l->spf.state == SpfGaveUp && !next_set(l)) {l->state = AlGaveUp; return 0;}
    if (g != AdgRun || (l->retry_armed && (int32_t)(now_ms - l->retry_at) < 0)) return 0;
    const uint32_t n = gpr_spf_next(&l->spf, out);
    if (!n) {l->state = AlGaveUp; return 0;}
    l->retry_armed = 0;
    l->sent_at = now_ms;
    if (l->set >= 0) l->queries[l->set]++;
    if (!l->first_sent) l->first_sent = now_ms ? now_ms : 1;
    l->state = AlQuerying;
    return n;
}

void audio_link_unsent(AudioLink *l)
{
    if (l->spf.state != SpfWaiting) return;
    l->spf.state = SpfIdle;
    l->spf.queries--;
    if (l->set >= 0 && l->queries[l->set]) l->queries[l->set]--;
}

void audio_link_receive(AudioLink *l, const uint8_t *data, uint32_t length, uint32_t now_ms)
{
    if (finished(l)) return;
    switch (gpr_spf_receive(&l->spf, data, length)) {
    case SpfIsReady: l->state = AlReady; l->ready_at = now_ms; break;
    case SpfNotReady: case SpfError: l->retry_at = now_ms + l->retry_ms; l->retry_armed = 1; break;
    case SpfDone: l->state = AlGaveUp; break;
    default: break;
    }
}

const char *audio_link_state(int state)
{
    switch (state) {
    case AlWaiting: return "waiting";
    case AlQuerying: return "querying";
    case AlReady: return "ready";
    case AlGaveUp: return "gave up";
    case AlOff: return "off";
    default: return "?";
    }
}
