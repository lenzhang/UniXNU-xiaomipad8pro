// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// Included by auto_host.c:
//   stream   AutoStream.c: one request outstanding, no nested close, the first event never dropped (also before the enable's answer),
//            readings resume after every off/on (Ubuntu v1.1.1's bug, 20 cycles), re-arm on a missing first event, timeouts and late
//            answers, the hub's error events, backoff and giving up, deadlines
//   light    AutoLight.c over SensorsPolicy's ps_als_step: the first sample at once, smoothing to the curve with ticks only while moving,
//            the stream off with the display or the lid, and the fresh start at wake (no fade from the level before the sleep)
#ifndef PIANO_AUTO_STREAM_TESTS_H
#define PIANO_AUTO_STREAM_TESTS_H
#include "AutoStream.h"
#include "AutoLight.h"

static PasStream st;

// What the kext does: the action the stream asks for, sent at once (the guard always gives a token here).
static int send_now(PasStream *s, uint64_t t)
{
    const int act = pas_action(s, t);
    if (act != PAS_ACT_NONE) pas_sent(s, act, t);
    return act;
}

static void test_stream(void)
{
    PasConfig c;
    pas_defaults(&c);
    // on: one request at a time; the first event marked, then used
    pas_init(&st, &c);
    CHECK(send_now(&st, 0) == PAS_ACT_NONE && pas_deadline(&st, 0) == 0);
    pas_want(&st, 1, 0);
    CHECK(send_now(&st, 0) == PAS_ACT_ENABLE && st.state == PAS_ENABLING);
    CHECK(send_now(&st, 10) == PAS_ACT_NONE && pas_deadline(&st, 10) == 2000);
    pas_response(&st, 1, 50);
    CHECK(st.state == PAS_ON && st.waiting && pas_deadline(&st, 50) == 3050);
    CHECK(pas_event(&st, 60) == PAS_EV_FIRST && pas_event(&st, 70) == PAS_EV_USE && pas_deadline(&st, 70) == 0);
    // off and on again (the v1.1.1 case): no enable while the disable is out, late samples of the old session dropped, the new
    // session's first sample marked
    pas_want(&st, 0, 100);
    CHECK(pas_event(&st, 101) == PAS_EV_DROP);              // off is wanted: not used even before the disable goes out
    CHECK(send_now(&st, 100) == PAS_ACT_DISABLE && st.state == PAS_DISABLING);
    CHECK(pas_event(&st, 110) == PAS_EV_DROP);
    pas_want(&st, 1, 120);
    CHECK(send_now(&st, 120) == PAS_ACT_NONE);
    CHECK(pas_event(&st, 130) == PAS_EV_DROP);
    pas_response(&st, 1, 150);
    CHECK(st.state == PAS_OFF && send_now(&st, 150) == PAS_ACT_ENABLE);
    pas_response(&st, 1, 200);
    CHECK(pas_event(&st, 210) == PAS_EV_FIRST && st.sessions == 2 && st.dropped == 3);

    // off and on while the enable is out: nothing more is sent (no nested close), the session's first event arrives
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    CHECK(send_now(&st, 0) == PAS_ACT_ENABLE);
    pas_want(&st, 0, 5);
    pas_want(&st, 1, 6);
    CHECK(send_now(&st, 7) == PAS_ACT_NONE);
    pas_response(&st, 1, 40);
    CHECK(send_now(&st, 41) == PAS_ACT_NONE && pas_event(&st, 50) == PAS_EV_FIRST && st.sessions == 1);
    // off while the enable is out: the disable follows its answer
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    CHECK(send_now(&st, 0) == PAS_ACT_ENABLE);
    pas_want(&st, 0, 5);
    CHECK(send_now(&st, 6) == PAS_ACT_NONE);
    pas_response(&st, 1, 40);
    CHECK(st.state == PAS_ON && pas_event(&st, 41) == PAS_EV_DROP && send_now(&st, 42) == PAS_ACT_DISABLE);
    pas_response(&st, 1, 80);
    CHECK(st.state == PAS_OFF && send_now(&st, 81) == PAS_ACT_NONE);

    // the first event before the enable's answer is the session's first, not lost; no watchdog afterwards
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    send_now(&st, 0);
    CHECK(pas_event(&st, 20) == PAS_EV_FIRST);
    pas_response(&st, 1, 30);
    CHECK(!st.waiting && pas_event(&st, 40) == PAS_EV_USE);
    pas_tick(&st, 10000);
    CHECK(st.rearmed == 0 && send_now(&st, 10000) == PAS_ACT_NONE);

    // no first event: re-armed twice (disable, enable), then it stays on quietly until the next period
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    send_now(&st, 0);
    pas_response(&st, 1, 10);
    pas_tick(&st, 3009);
    CHECK(st.rearm == 0 && send_now(&st, 3009) == PAS_ACT_NONE);
    pas_tick(&st, 3010);
    CHECK(st.rearm == 1 && st.rearmed == 1 && pas_deadline(&st, 3010) == 3010);
    CHECK(send_now(&st, 3010) == PAS_ACT_DISABLE);
    pas_response(&st, 1, 3050);
    CHECK(send_now(&st, 3050) == PAS_ACT_ENABLE && st.sessions == 2);
    pas_response(&st, 1, 3100);
    pas_tick(&st, 6100);
    CHECK(send_now(&st, 6100) == PAS_ACT_DISABLE);
    pas_response(&st, 1, 6110);
    CHECK(send_now(&st, 6110) == PAS_ACT_ENABLE && st.sessions == 3);
    pas_response(&st, 1, 6120);
    pas_tick(&st, 9120);
    CHECK(st.rearmed == 2 && st.silent == 1 && send_now(&st, 9120) == PAS_ACT_NONE && pas_deadline(&st, 9120) == 0);
    CHECK(pas_event(&st, 20000) == PAS_EV_FIRST);
    // an event that comes after the re-arm was decided cancels it
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    send_now(&st, 0);
    pas_response(&st, 1, 0);
    pas_tick(&st, 3000);
    CHECK(st.rearm == 1 && pas_event(&st, 3001) == PAS_EV_FIRST && st.rearm == 0 && send_now(&st, 3002) == PAS_ACT_NONE);

    // a request unanswered: failed, the hub's state unknown -> a disable first, after the backoff; its late answer ignored
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    send_now(&st, 0);
    pas_tick(&st, 1999);
    CHECK(st.state == PAS_ENABLING);
    pas_tick(&st, 2000);
    CHECK(st.timeouts == 1 && st.dirty && st.state == PAS_OFF && st.retry_at == 3000);
    pas_response(&st, 1, 2100);
    CHECK(st.state == PAS_OFF && send_now(&st, 2500) == PAS_ACT_NONE && pas_deadline(&st, 2500) == 3000);
    CHECK(send_now(&st, 3000) == PAS_ACT_DISABLE);
    pas_response(&st, 1, 3010);
    CHECK(!st.dirty && st.fails == 0 && send_now(&st, 3010) == PAS_ACT_ENABLE);
    pas_response(&st, 1, 3020);
    CHECK(pas_event(&st, 3030) == PAS_EV_FIRST);

    // an error event of the hub while on: a failure, its events dropped, a disable after the backoff, then a fresh session
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    send_now(&st, 0);
    pas_response(&st, 1, 10);
    CHECK(pas_event(&st, 20) == PAS_EV_FIRST);
    pas_error(&st, 30);
    CHECK(st.state == PAS_OFF && st.dirty && st.fails == 1 && st.retry_at == 1030 && pas_event(&st, 40) == PAS_EV_DROP);
    CHECK(send_now(&st, 1029) == PAS_ACT_NONE && pas_deadline(&st, 500) == 1030 && send_now(&st, 1030) == PAS_ACT_DISABLE);
    pas_response(&st, 1, 1040);
    CHECK(!st.dirty && st.state == PAS_OFF && send_now(&st, 1040) == PAS_ACT_ENABLE);
    pas_response(&st, 1, 1050);
    CHECK(pas_event(&st, 1060) == PAS_EV_FIRST && st.sessions == 2);
    // ... while the enable is out: the answer ends it, its success keeps the error's backoff, the disable follows after it
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    send_now(&st, 0);
    CHECK(pas_event(&st, 5) == PAS_EV_FIRST);
    pas_error(&st, 10);
    CHECK(st.state == PAS_ENABLING && st.outstanding == PAS_ACT_ENABLE && pas_event(&st, 15) == PAS_EV_DROP);
    pas_response(&st, 1, 20);
    CHECK(st.state == PAS_ON && st.dirty && st.fails == 1 && !st.waiting && send_now(&st, 21) == PAS_ACT_NONE);
    CHECK(send_now(&st, 1010) == PAS_ACT_DISABLE);
    pas_response(&st, 1, 1020);
    CHECK(send_now(&st, 1020) == PAS_ACT_ENABLE);
    // errors in a row end it; a late answer then changes nothing
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    for (unsigned i = 0; i < c.failures; ++i) pas_error(&st, 0);
    CHECK(st.state == PAS_FAILED);
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    send_now(&st, 0);
    for (unsigned i = 0; i < c.failures; ++i) pas_error(&st, 0);
    pas_response(&st, 1, 10);
    CHECK(st.state == PAS_FAILED && st.outstanding == PAS_ACT_NONE && send_now(&st, 100000) == PAS_ACT_NONE);

    // refused enables: backoff 1, 2, 4, 8, 16 s, then failed for the boot
    pas_init(&st, &c);
    pas_want(&st, 1, 0);
    uint64_t t = 0;
    for (uint32_t i = 0, backoff = 1000; i < 6; ++i, backoff *= 2) {
        CHECK(send_now(&st, t) == PAS_ACT_ENABLE);
        pas_response(&st, 0, t);
        if (i < 5) CHECK(st.state == PAS_OFF && st.retry_at == t + backoff && send_now(&st, t + backoff - 1) == PAS_ACT_NONE);
        t = st.retry_at;
    }
    CHECK(st.state == PAS_FAILED && st.errors == 6 && send_now(&st, t + 100000) == PAS_ACT_NONE && pas_deadline(&st, t) == 0);
    CHECK(pas_event(&st, t) == PAS_EV_DROP);

    // 20 off/on cycles (the S4 test on the host): every session's readings resume, each first sample marked
    pas_init(&st, &c);
    t = 0;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        pas_want(&st, 1, t);
        CHECK(send_now(&st, t) == PAS_ACT_ENABLE);
        pas_response(&st, 1, t + 20);
        CHECK(pas_event(&st, t + 30) == PAS_EV_FIRST && pas_event(&st, t + 40) == PAS_EV_USE);
        pas_want(&st, 0, t + 50);
        if (cycle % 2) pas_want(&st, 1, t + 51), pas_want(&st, 0, t + 52);   // a flicker of the conditions
        CHECK(send_now(&st, t + 60) == PAS_ACT_DISABLE);
        pas_response(&st, 1, t + 70);
        CHECK(st.state == PAS_OFF);
        t += 1000;
    }
    CHECK(st.sessions == 20 && st.errors == 0 && st.rearmed == 0);
    CHECK(!strcmp(pas_state_name(PAS_DISABLING), "disabling") && !strcmp(pas_action_name(PAS_ACT_ENABLE), "enable"));
    printf("PASS stream: one request at a time, no nested close, first events kept, 20 off/on cycles resume, re-arm, timeouts, "
           "error events, backoff and failure\n");
}

static PalLight light;

static void light_enable(uint64_t t)
{
    CHECK(send_now(&light.stream, t) == PAS_ACT_ENABLE);
    pas_response(&light.stream, 1, t + 10);
}

static void test_light(void)
{
    PasConfig c;
    pas_defaults(&c);
    pal_init(&light, &c);
    pal_conditions(&light, 1, 0, -1, 0);
    CHECK(pas_action(&light.stream, 0) == PAS_ACT_NONE);               // the display is off: no stream
    pal_conditions(&light, 1, 1, -1, 0);                                // on, lid unknown (counts as open)
    light_enable(0);
    // the first sample applies at once: 300 lux -> the curve's 370 permille
    CHECK(pal_event(&light, float_bits(300.0f), 100) == 370 && light.lux == 300 && pal_settled(&light) && light.tick_at == 0);
    CHECK(pal_deadline(&light, 100) == 0);                              // at rest: no timer
    // brighter: smoothed up to 574 (1000 lux), at most one target per 250 ms, rising, only while moving
    int32_t out = pal_event(&light, float_bits(1000.0f), 200);
    CHECK(out == -1 && light.tick_at == 450 && pal_deadline(&light, 200) == 450);
    int32_t last = 370;
    uint64_t t = 450, last_t = 100;
    unsigned published = 0;
    while (light.tick_at) {
        CHECK(pal_deadline(&light, t) == light.tick_at);
        t = light.tick_at;
        out = pal_tick(&light, t);
        if (out >= 0) {
            CHECK(out > last && t - last_t >= PS_ALS_PUBLISH_MS);
            last = out;
            last_t = t;
            ++published;
        }
        CHECK(t < 30000);
    }
    CHECK(last == 574 && published >= 3 && pal_settled(&light) && pal_deadline(&light, t) == 0);
    // a small change (under the 3 % hysteresis) moves nothing
    CHECK(pal_event(&light, float_bits(990.0f), t + 1000) == -1 && pal_settled(&light));
    // the display sleeps: the stream goes off, samples are dropped, no ticks
    pal_conditions(&light, 1, 0, -1, t + 2000);
    CHECK(send_now(&light.stream, t + 2000) == PAS_ACT_DISABLE);
    CHECK(pal_event(&light, float_bits(5.0f), t + 2001) == -1);
    pas_response(&light.stream, 1, t + 2010);
    CHECK(pal_tick(&light, t + 3000) == -1 && light.tick_at == 0);
    // wake in a dark room: the new session's first sample (0 lux, valid darkness) applies at once, no fade from 574
    pal_conditions(&light, 1, 1, 1, 60000);
    light_enable(60000);
    CHECK(pal_event(&light, float_bits(0.0f), 60100) == 40 && light.restarts == 1 && pal_settled(&light));
    // the lid closed: off as with the display; opened: on, fresh again
    pal_conditions(&light, 1, 1, 0, 61000);
    CHECK(send_now(&light.stream, 61000) == PAS_ACT_DISABLE);
    pas_response(&light.stream, 1, 61010);
    pal_conditions(&light, 1, 1, 1, 62000);
    light_enable(62000);
    CHECK(pal_event(&light, float_bits(10000.0f), 62100) == 1000 && light.restarts == 2);
    // a re-armed stream (no first event) also starts afresh from its first sample
    pal_conditions(&light, 1, 0, 1, 63000);
    send_now(&light.stream, 63000);
    pas_response(&light.stream, 1, 63010);
    pal_conditions(&light, 1, 1, 1, 64000);
    light_enable(64000);
    CHECK(pal_tick(&light, 67010) == -1 && light.stream.rearm == 1);
    CHECK(send_now(&light.stream, 67010) == PAS_ACT_DISABLE);
    pas_response(&light.stream, 1, 67020);
    light_enable(67020);
    CHECK(pal_event(&light, float_bits(100.0f), 67100) == 240 && light.restarts == 3);
    // the consumer off: the stream off
    pal_conditions(&light, 0, 1, 1, 70000);
    CHECK(send_now(&light.stream, 70000) == PAS_ACT_DISABLE);
    printf("PASS light: first sample at once, smoothing only while moving, off with the display and the lid, a fresh start at wake\n");
}

static void stream_tests(void)
{
    test_stream();
    test_light();
}
#endif
