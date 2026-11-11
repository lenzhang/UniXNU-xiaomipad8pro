// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoAudio's DSP guard, inbox and level-2 exchange (AudioDspLink.c) against PianoPower's health as the contract
// describes it and a model of the APM: nothing is sent before the DSP, our channel and the charger domain are up and settled, nor
// while PianoPower asks for quiet; one query at a time, answered, retried, given up; off for the rest of the boot at a fatal report,
// a crash reason, or (once started) a new link or charger generation.
//   cc -std=c11 -Wall -Wextra -Werror dsplink_host.c ../AudioDspLink.c ../PianoGpr.c && ./a.out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AudioDspLink.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

static AdgObservation healthy(void)
{
    AdgObservation o = {1, 1, 0, 0, 3, ADG_CHARGER_UP, 1, 0, "", 1};
    return o;
}

// The APM's answer to a query: its readiness state (1 ready), or a basic result refusing it.
static uint32_t apm_answer(const uint8_t *query, uint8_t *out, uint32_t state, int refuse)
{
    GprPacket q, p;
    CHECK(gpr_decode(query, GPR_HEADER_BYTES, &q) == GprOk && q.opcode == GPR_OP_APM_GET_SPF_STATE && q.dst_port == GPR_PORT_APM);
    memset(&p, 0, sizeof p);
    p.dst_domain = q.src_domain; p.src_domain = q.dst_domain; p.src_port = q.dst_port; p.dst_port = q.src_port; p.token = q.token;
    if (refuse) {const uint32_t w[2] = {GPR_OP_APM_GET_SPF_STATE, 0x0b}; p.opcode = GPR_OP_BASIC_RESULT; return gpr_encode(out, GPR_MAX_PACKET, &p, w, 8);}
    p.opcode = GPR_OP_APM_SPF_STATE;
    return gpr_encode(out, GPR_MAX_PACKET, &p, &state, 4);
}

static void testGuardWaits(void)
{
    AudioDspGuard g;
    adg_init(&g, ADG_SETTLE_MS);
    AdgObservation o = healthy();
    o.dsp_up = 0;
    CHECK(adg_observe(&g, &o, 0) == AdgWait);
    o = healthy(); o.channel_up = 0;
    CHECK(adg_observe(&g, &o, 100) == AdgWait);
    o = healthy(); o.charger_pd = 0;
    CHECK(adg_observe(&g, &o, 200) == AdgWait);
    o = healthy();
    CHECK(adg_observe(&g, &o, 1000) == AdgWait && adg_observe(&g, &o, 20999) == AdgWait);   // settling 20 s from 1000
    // the charger domain restarts while we still wait: the settling starts over, it is not trouble yet
    o.charger_generation = 2;
    CHECK(adg_observe(&g, &o, 21000) == AdgWait && adg_observe(&g, &o, 40999) == AdgWait);
    o.quiet = 1;
    CHECK(adg_observe(&g, &o, 42000) == AdgWait && g.holds == 1);   // PianoPower configures the charger
    o.quiet = 0;
    CHECK(adg_observe(&g, &o, 42001) == AdgRun && g.started);
    CHECK(adg_observe(&g, &o, 50000) == AdgRun);
    o.quiet = 1;
    CHECK(adg_observe(&g, &o, 50001) == AdgWait);   // started: quiet holds, it is not trouble
    o.quiet = 0;
    CHECK(adg_observe(&g, &o, 50002) == AdgRun);
}

static void testGuardOff(void)
{
    // each sign of trouble turns the guard off for good
    AudioDspGuard g;
    AdgObservation o;
    // the fatal bit, before or after starting
    adg_init(&g, 0); o = healthy(); o.fatal = 1;
    CHECK(adg_observe(&g, &o, 0) == AdgOff && g.reason == AdgReasonFatal);
    o = healthy();
    CHECK(adg_observe(&g, &o, 1) == AdgOff);   // latched
    // a new fatal report
    adg_init(&g, 0); o = healthy(); o.fatal_reports = 2;
    CHECK(adg_observe(&g, &o, 0) == AdgRun);   // the count at the first look is the baseline
    o.fatal_reports = 3;
    CHECK(adg_observe(&g, &o, 1) == AdgOff && g.reason == AdgReasonFatal);
    // a crash reason
    adg_init(&g, 0); o = healthy(); o.crash = "err_fatal: audio_pd";
    CHECK(adg_observe(&g, &o, 0) == AdgOff && g.reason == AdgReasonCrash && !strcmp(adg_reason(g.reason), "the DSP left a crash reason"));
    // once started: a new link generation, a new charger generation, the charger domain or our channel down
    adg_init(&g, 0); o = healthy();
    CHECK(adg_observe(&g, &o, 0) == AdgRun);
    o.link_generation = 1;
    CHECK(adg_observe(&g, &o, 1) == AdgOff && g.reason == AdgReasonLink);
    adg_init(&g, 0); o = healthy();
    adg_observe(&g, &o, 0);
    o.charger_generation = 4;
    CHECK(adg_observe(&g, &o, 1) == AdgOff && g.reason == AdgReasonCharger);
    adg_init(&g, 0); o = healthy();
    adg_observe(&g, &o, 0);
    o.charger_pd = 0;
    CHECK(adg_observe(&g, &o, 1) == AdgOff && g.reason == AdgReasonCharger);
    adg_init(&g, 0); o = healthy();
    adg_observe(&g, &o, 0);
    o.channel_up = 0;
    CHECK(adg_observe(&g, &o, 1) == AdgOff && g.reason == AdgReasonLink);
    // another contract version: off at once
    adg_init(&g, 0); o = healthy(); o.contract_ok = 0;
    CHECK(adg_observe(&g, &o, 0) == AdgOff && g.reason == AdgReasonContract);
    // an error of ours
    adg_init(&g, 0); o = healthy();
    adg_observe(&g, &o, 0);
    adg_trip(&g, AdgReasonError);
    CHECK(adg_observe(&g, &o, 1) == AdgOff && g.reason == AdgReasonError);
    adg_trip(&g, AdgReasonOwner);
    CHECK(g.reason == AdgReasonError && !strcmp(adg_reason(99), "?"));
}

static void testInbox(void)
{
    static AudioInbox b;
    audio_inbox_init(&b);
    uint8_t p[600], out[600];
    for (int i = 0; i < 600; ++i) p[i] = (uint8_t)i;
    CHECK(audio_inbox_get(&b, out, sizeof out) == 0);
    CHECK(audio_inbox_put(&b, p, 513) == -1 && b.too_long == 1);
    for (unsigned i = 0; i < AUDIO_INBOX_SLOTS; ++i) CHECK(audio_inbox_put(&b, p + i, 24 + i) == 0);
    CHECK(audio_inbox_put(&b, p, 24) == -1 && b.dropped == 1);   // full: the newest is dropped, never waited for
    for (unsigned i = 0; i < AUDIO_INBOX_SLOTS; ++i) CHECK(audio_inbox_get(&b, out, sizeof out) == 24 + i && out[0] == (uint8_t)i);
    CHECK(audio_inbox_get(&b, out, sizeof out) == 0);
    for (unsigned k = 0; k < 100; ++k) {   // round and round
        CHECK(audio_inbox_put(&b, p + k % 7, 512) == 0 && audio_inbox_get(&b, out, 10) == 10 && out[0] == (uint8_t)(k % 7));
    }
}

static void testLink(void)
{
    AudioLink l;
    uint8_t q[GPR_HEADER_BYTES], a[GPR_MAX_PACKET];
    audio_link_init(&l, GPR_PORT_APM, 5, ADG_SETTLE_MS);
    AdgObservation o = healthy();
    o.channel_up = 0;
    CHECK(audio_link_tick(&l, &o, 0, q) == 0 && l.state == AlWaiting);
    o.channel_up = 1;
    uint32_t t = 0;
    for (t = 100; t < 20100; t += 100) CHECK(audio_link_tick(&l, &o, t, q) == 0);   // settling: nothing sent
    uint32_t n = audio_link_tick(&l, &o, t, q);
    CHECK(n == 24 && l.state == AlQuerying && le32(q + 20) == GPR_OP_APM_GET_SPF_STATE && le32(q + 8) == GPR_PORT_APM && le32(q + 12) == 1);
    CHECK(audio_link_tick(&l, &o, t + 10, q) == 0);   // one in flight
    // not ready: asked again after the retry pause
    audio_link_receive(&l, a, apm_answer(q, a, 0, 0), t + 20);
    CHECK(audio_link_tick(&l, &o, t + 100, q) == 0 && audio_link_tick(&l, &o, t + 520, q) == 24 && l.spf.queries == 2);
    // the queue was full: the same query again at the next tick
    audio_link_unsent(&l);
    CHECK(l.spf.queries == 1 && audio_link_tick(&l, &o, t + 540, q) == 24 && l.spf.queries == 2);
    // no answer: after a second, retried
    CHECK(audio_link_tick(&l, &o, t + 1539, q) == 0 && audio_link_tick(&l, &o, t + 1540, q) == 0 && l.spf.timeouts == 1);
    CHECK(audio_link_tick(&l, &o, t + 2040, q) == 24);
    // quiet holds the next query, not the answer of the one in flight
    o.quiet = 1;
    audio_link_receive(&l, a, apm_answer(q, a, 0, 0), t + 2050);
    CHECK(audio_link_tick(&l, &o, t + 3000, q) == 0);
    o.quiet = 0;
    n = audio_link_tick(&l, &o, t + 3100, q);
    CHECK(n == 24 && l.spf.queries == 4);
    // ready
    audio_link_receive(&l, a, apm_answer(q, a, 1, 0), t + 3110);
    CHECK(l.state == AlReady && l.ready_at == t + 3110 && !strcmp(audio_link_state(l.state), "ready"));
    CHECK(audio_link_tick(&l, &o, t + 4000, q) == 0);   // nothing more at level 2
    // ...but trouble still turns it off
    o.link_generation = 9;
    audio_link_tick(&l, &o, t + 4100, q);
    CHECK(l.state == AlOff && l.guard.reason == AdgReasonLink);

    // refused, then out of tries: gave up
    audio_link_init(&l, GPR_PORT_APM, 2, 0);
    o = healthy();
    CHECK(audio_link_tick(&l, &o, 0, q) == 24);
    audio_link_receive(&l, a, apm_answer(q, a, 0, 1), 5);   // refused: retried
    CHECK(l.spf.errors == 1 && audio_link_tick(&l, &o, 600, q) == 24);
    audio_link_receive(&l, a, apm_answer(q, a, 0, 0), 610);  // the last try: not ready
    CHECK(l.state == AlGaveUp && audio_link_tick(&l, &o, 2000, q) == 0);

    // a fatal report while a query is in flight: off, the late answer ignored
    audio_link_init(&l, GPR_PORT_APM, 5, 0);
    o = healthy();
    CHECK(audio_link_tick(&l, &o, 0, q) == 24);
    o.fatal = 1;
    CHECK(audio_link_tick(&l, &o, 10, q) == 0 && l.state == AlOff);
    audio_link_receive(&l, a, apm_answer(q, a, 1, 0), 20);
    CHECK(l.state == AlOff);
    // another port's packet and garbage are ignored
    audio_link_init(&l, GPR_PORT_APM, 5, 0);
    o = healthy();
    CHECK(audio_link_tick(&l, &o, 0, q) == 24);
    n = apm_answer(q, a, 1, 0);
    a[12] = 7;
    audio_link_receive(&l, a, n, 1);
    audio_link_receive(&l, a, 9, 1);
    CHECK(l.state == AlQuerying && l.spf.ignored == 2);
}

int main(void)
{
    testGuardWaits();
    testGuardOff();
    testInbox();
    testLink();
    printf("PASS dsplink: %u checks\n", checks);
    return 0;
}
