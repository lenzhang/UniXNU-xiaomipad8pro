// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// Host tests of level 4 (SensorsStream.c over PianoAuto's AutoStream.c and AutoLight.c, docs/drivers/sensors.md section 5c) against a
// simulated hub that reads every request as the hub would (QMI 0x20, TLV 0x01, sns_client_request_msg) and answers with the message
// shapes of the sensor client service (libqmi's SSC description, libssc's and Android's sns_client / sns_std_sensor messages, facts
// only): responses with the client id, reports 0x21 carrying sns_client_event_msg with sensor events 1025 (lux; m/s^2 vectors,
// packed and unpacked floats), the physical configuration event 768 and the error event 130. The hub also checks the client's rules
// (one request outstanding per client, no enable on a streaming client). Synthetic values only.
//   bytes     the enable (514 on change, 513 {sample_rate 10 Hz}, with batching) and the disable (10) byte for byte; a report byte for byte
//   inert     no consumer, no service, no sensor: nothing is sent (battery)
//   light     the first sample at once, smoothing, the hysteresis, lux rate limited, darkness; 20 display off/on cycles resume at the
//             room's level (Ubuntu v1.1.1), the first event before the answer, re-arm, a streaming light sensor at its lowest rate
//   accel     10 Hz while the cover is detached and the display on; the four edges, flat; cover, display and sleep teardown
//   guard     no token no request, refusals, missing answers and error events told to the guard; a strike: nothing more, outputs at rest
//   sleep     disables before the sleep; a hub that does not answer: the port closed, a fresh client after the wake
//   service   a new generation and a new default sensor start over; the level-3 list feeding the streams end to end
//   log       the stream log's limiter
#include <math.h>
#include "check.h"
#include "../SensorsStream.h"
#include "../SensorsGuard.h"
#include "../SensorsPb.h"

static uint32_t fb(float f) {return float_bits(f);}
static void wr16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void wr64(uint8_t *p, uint64_t v) {wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));}

static const SscSuid kLight = {0x1111222233334444ull, 0x5555666677778888ull};
static const SscSuid kAccel = {0x0123456789abcdefull, 0xfedcba9876543210ull};

// ---------------------------------------------------------------- the harness: the engine's ops
static SsEngine eng;
static uint64_t clock_ms;
static int allow = 1;
static uint32_t tokens_taken, results_ok, results_bad, publishes, leaves, closes[SS_STREAMS], trace_lines;
static int verbose;
static char last_trace[256];
static SsOutputs out;
static int edges[64];
static unsigned edge_count;

static uint64_t op_now(void *c) {(void)c; return clock_ms;}
static int hub_take(int stream, const uint8_t *data, uint32_t length);
static int port_refuse[SS_STREAMS];
static int op_send(void *c, int stream, const uint8_t *data, uint32_t length)
{
    (void)c;
    if (port_refuse[stream]) return -1;
    return hub_take(stream, data, length);
}
static int op_may(void *c) {(void)c; if (allow) tokens_taken++; return allow;}
static void op_result(void *c, int ok) {(void)c; if (ok) results_ok++; else results_bad++;}
static void op_trace(void *c, const char *line)
{
    (void)c;
    trace_lines++;
    snprintf(last_trace, sizeof last_trace, "%s", line);
    if (verbose) printf("  [%6llu] %s\n", (unsigned long long)clock_ms, line);
}
static void op_publish(void *c, const SsOutputs *o)
{
    (void)c;
    CHECK(o->seq == out.seq + 1 && o->changed);            // the sequence moves with every change, by one
    out = *o;
    publishes++;
    if ((o->changed & SS_OUT_EDGE) && edge_count < 64) edges[edge_count++] = o->edge;
}
static void op_leave(void *c) {(void)c; leaves++;}
static void op_close(void *c, int stream) {(void)c; closes[stream]++;}
static const SsOps kOps = {NULL, op_now, op_send, op_may, op_result, op_trace, op_publish, op_leave, op_close};

// ---------------------------------------------------------------- the simulated hub
typedef struct {
    int enabled;                 // the hub streams to this client
    int pending;                 // a request is waiting for its answer
    uint16_t txn;
    uint32_t msg;
    uint64_t answer_at, next_sample;
    uint32_t rate_bits;          // the 513 asked
    int64_t batch_us;            // -1 none
    int mute, refuse, silent, error_after, first_before, packed;
    uint32_t requests, enables, disables, overlapped, nested, samples, answered;
    uint64_t client;
    SscSuid suid;
} HubPort;
static HubPort hub[SS_STREAMS];
static float hub_lux = 300.0f;
static float pose[3] = {9.81f, 0.0f, 0.0f};
static uint8_t last_req[SS_STREAMS][256];
static uint32_t last_req_length[SS_STREAMS];

typedef struct {
    uint16_t txn;
    SscSuid suid;
    uint32_t msg_id, processor, delivery;
    int has_payload, has_batching;
    uint32_t rate_bits;
    uint64_t batch_us;
} Req;

// The request as the hub reads it: QMI 0x20 {TLV 0x01 u16 n + sns_client_request_msg}, no jumbo TLV 0x10; the message's four fields.
static Req parse_req(const uint8_t *data, uint32_t length)
{
    Req r;
    memset(&r, 0, sizeof r);
    SscQmi m;
    uint32_t size = 0;
    CHECK(!ssc_qmi_parse(data, length, &m) && m.type == SSC_QMI_REQUEST && m.message == SSC_QMI_REQ);
    const uint8_t *tlv = ssc_qmi_tlv(&m, 0x01, &size);
    CHECK(tlv && size >= 2 && (uint32_t)(tlv[0] | tlv[1] << 8) == size - 2 && !ssc_qmi_tlv(&m, 0x10, &size));
    r.txn = m.transaction;
    SpbIn in, sub;
    uint32_t field, wire;
    int seen = 0;
    spb_in(&in, tlv + 2, (uint32_t)(tlv[0] | tlv[1] << 8));
    while (spb_next(&in, &field, &wire) == 1) {
        const uint8_t *b = NULL;
        uint32_t bn = 0, v32 = 0;
        uint64_t v = 0;
        if (field == 1) {
            CHECK(!spb_bytes_value(&in, wire, &b, &bn));
            spb_in(&sub, b, bn);
            while (spb_next(&sub, &field, &wire) == 1) {
                CHECK(!spb_fixed64_value(&sub, wire, &v));
                if (field == 1) r.suid.low = v;
                else r.suid.high = v;
            }
            seen |= 1;
        } else if (field == 2) {
            CHECK(!spb_fixed32_value(&in, wire, &v32));
            r.msg_id = v32;
            seen |= 2;
        } else if (field == 3) {
            CHECK(!spb_bytes_value(&in, wire, &b, &bn));
            spb_in(&sub, b, bn);
            while (spb_next(&sub, &field, &wire) == 1) {
                CHECK(!spb_varint_value(&sub, wire, &v));
                if (field == 1) r.processor = (uint32_t)v;
                else r.delivery = (uint32_t)v;
            }
            seen |= 4;
        } else if (field == 4) {
            CHECK(!spb_bytes_value(&in, wire, &b, &bn));
            spb_in(&sub, b, bn);
            while (spb_next(&sub, &field, &wire) == 1) {
                const uint8_t *pb = NULL;
                uint32_t pn = 0;
                CHECK(!spb_bytes_value(&sub, wire, &pb, &pn));
                SpbIn q;
                spb_in(&q, pb, pn);
                if (field == 1) {          // batching {batch_period}
                    r.has_batching = 1;
                    CHECK(spb_next(&q, &field, &wire) == 1 && field == 1 && !spb_varint_value(&q, wire, &r.batch_us));
                } else if (field == 2) {   // payload: sns_std_sensor_config {sample_rate} (or a SUID request's, on the list port)
                    r.has_payload = 1;
                    if (spb_next(&q, &field, &wire) == 1 && field == 1 && wire == SPB_I32) CHECK(!spb_fixed32_value(&q, wire, &r.rate_bits));
                }
            }
            seen |= 8;
        } else {
            CHECK(!"an unknown field in the request");
        }
    }
    CHECK(seen == 15 && r.processor == SSC_PROC_APSS && r.delivery == SSC_DELIVERY_WAKEUP);   // every required field
    return r;
}

static int hub_take(int stream, const uint8_t *data, uint32_t length)
{
    HubPort *h = &hub[stream];
    CHECK(length <= sizeof last_req[0]);
    memcpy(last_req[stream], data, length);
    last_req_length[stream] = length;
    const Req r = parse_req(data, length);
    CHECK(ssc_suid_equal(&r.suid, stream == SS_LIGHT ? &kLight : &kAccel) || ssc_suid_equal(&r.suid, &h->suid));
    h->requests++;
    if (h->pending) h->overlapped++;                       // a second request before the first's answer: AutoStream forbids it
    if (r.msg_id == SSC_MSG_DISABLE_REQ) {
        h->disables++;
        CHECK(!r.has_payload && !r.has_batching);
    } else {
        CHECK(r.msg_id == SSC_MSG_ON_CHANGE_CONFIG || r.msg_id == SSC_MSG_SENSOR_CONFIG);
        CHECK(r.msg_id == SSC_MSG_SENSOR_CONFIG ? r.has_payload : !r.has_payload && !r.has_batching);
        h->enables++;
        if (h->enabled) h->nested++;                        // an enable on a streaming client, without its disable
        h->rate_bits = r.rate_bits;
        h->batch_us = r.has_batching ? (int64_t)r.batch_us : -1;
    }
    if (h->mute) return 0;                                  // queued, never answered
    h->pending = 1;
    h->txn = r.txn;
    h->msg = r.msg_id;
    h->answer_at = clock_ms + 20;
    return 0;
}

static void deliver(int stream, const uint8_t *data, uint32_t n) {ss_receive(&eng, stream, data, n);}

static void respond(int stream, uint16_t txn, int ok)
{
    uint8_t q[64];
    uint32_t n = 0;
    q[n++] = SSC_QMI_RESPONSE; q[n++] = (uint8_t)txn; q[n++] = (uint8_t)(txn >> 8); q[n++] = 0x20; q[n++] = 0; q[n++] = 18; q[n++] = 0;
    q[n++] = 0x02; q[n++] = 4; q[n++] = 0; q[n++] = ok ? 0 : 1; q[n++] = 0; q[n++] = ok ? 0 : 0x5a; q[n++] = 0;
    q[n++] = 0x10; q[n++] = 8; q[n++] = 0; wr64(q + n, hub[stream].client); n += 8;
    deliver(stream, q, n);
}

// A report 0x21 {TLV 0x01 client id, TLV 0x02 u16 n + sns_client_event_msg {suid, events {msg_id, timestamp, payload}...}}.
typedef struct {uint32_t msg_id; uint8_t payload[96]; uint32_t length;} HubEvent;
static void report(int stream, const SscSuid *suid, const HubEvent *events, unsigned count)
{
    uint8_t body[700], ind[800];
    SpbOut o;
    spb_out(&o, body, sizeof body);
    uint32_t mark = spb_open(&o, 1);
    spb_put_fixed64(&o, 1, suid->low);
    spb_put_fixed64(&o, 2, suid->high);
    spb_close(&o, mark);
    for (unsigned i = 0; i < count; ++i) {
        mark = spb_open(&o, 2);
        spb_put_fixed32(&o, 1, events[i].msg_id);
        spb_put_fixed64(&o, 2, clock_ms * 19200ull);         // QTimer ticks
        spb_put_bytes(&o, 3, events[i].payload, events[i].length);
        spb_close(&o, mark);
    }
    const uint32_t b = spb_done(&o);
    CHECK(b);
    uint32_t n = 0;
    const uint32_t tlvs = 11 + 5 + b;
    ind[n++] = SSC_QMI_INDICATION; ind[n++] = 0; ind[n++] = 0; ind[n++] = 0x21; ind[n++] = 0;
    ind[n++] = (uint8_t)tlvs; ind[n++] = (uint8_t)(tlvs >> 8);
    ind[n++] = 0x01; ind[n++] = 8; ind[n++] = 0; wr64(ind + n, hub[stream].client); n += 8;
    ind[n++] = 0x02; wr16(ind + n, b + 2); n += 2; wr16(ind + n, b); n += 2;
    memcpy(ind + n, body, b);
    n += b;
    deliver(stream, ind, n);
}

// sns_std_sensor_event {data 1 (repeated float, packed or one tag per value), status 2}
static HubEvent sensor_event(const float *v, unsigned count, int status, int packed)
{
    HubEvent ev = {SSC_MSG_SENSOR_EVENT, {0}, 0};
    SpbOut o;
    spb_out(&o, ev.payload, sizeof ev.payload);
    if (packed) {
        uint8_t raw[64];
        for (unsigned i = 0; i < count; ++i) wr32(raw + 4 * i, fb(v[i]));
        spb_put_bytes(&o, 1, raw, 4 * count);
    } else {
        for (unsigned i = 0; i < count; ++i) spb_put_fixed32(&o, 1, fb(v[i]));
    }
    spb_put_varint(&o, 2, (uint64_t)status);
    ev.length = spb_done(&o);
    return ev;
}

// sns_std_sensor_physical_config_event {sample_rate 1, water_mark 2, range 3, resolution 4, operation_mode 5, active_current 6, dri 8}
static HubEvent physical_event(float rate)
{
    HubEvent ev = {SSC_MSG_PHYSICAL_CONFIG_EVENT, {0}, 0};
    SpbOut o;
    spb_out(&o, ev.payload, sizeof ev.payload);
    spb_put_fixed32(&o, 1, fb(rate));
    spb_put_varint(&o, 2, 1);
    spb_put_fixed32(&o, 3, fb(-78.4532f));
    spb_put_fixed32(&o, 3, fb(78.4532f));
    spb_put_fixed32(&o, 4, fb(0.0024f));
    spb_put_string(&o, 5, "NORMAL");
    spb_put_varint(&o, 6, 120);
    spb_put_varint(&o, 8, 1);
    ev.length = spb_done(&o);
    return ev;
}

static HubEvent error_event(int code)
{
    HubEvent ev = {SSC_MSG_ERROR_EVENT, {0}, 0};
    SpbOut o;
    spb_out(&o, ev.payload, sizeof ev.payload);
    spb_put_varint(&o, 1, (uint64_t)code);
    ev.length = spb_done(&o);
    return ev;
}

static void hub_sample(int stream)
{
    HubPort *h = &hub[stream];
    HubEvent ev;
    if (stream == SS_LIGHT) {
        const float v[2] = {hub_lux, hub_lux * 3.0f};       // lux and the raw count, as the ALS drivers send them
        ev = sensor_event(v, 2, 3, h->packed);
    } else {
        ev = sensor_event(pose, 3, 3, h->packed);
    }
    h->samples++;
    report(stream, stream == SS_LIGHT ? &kLight : &kAccel, &ev, 1);
}

static void hub_step(void)
{
    for (int i = 0; i < SS_STREAMS; ++i) {
        HubPort *h = &hub[i];
        if (h->pending && clock_ms >= h->answer_at) {
            const int enable = h->msg != SSC_MSG_DISABLE_REQ;
            h->pending = 0;
            if (enable && h->first_before && !h->silent && !h->refuse) {
                h->enabled = 1;
                hub_sample(i);                               // the first sample overtakes the answer
            }
            respond(i, h->txn, !h->refuse);
            h->answered++;
            if (h->refuse) continue;
            if (!enable) {
                h->enabled = 0;
                continue;
            }
            if (h->error_after) {
                h->error_after--;
                h->enabled = 0;
                const HubEvent ev = error_event(3);
                report(i, i == SS_LIGHT ? &kLight : &kAccel, &ev, 1);
                continue;
            }
            const int was = h->enabled;
            h->enabled = 1;
            if (i == SS_ACCEL) {
                const HubEvent ev = physical_event(12.5f);   // the driver's nearest rate at or above 10 Hz
                report(i, &kAccel, &ev, 1);
                h->next_sample = clock_ms + 80;
            } else if (!h->silent && !(h->first_before && was)) {
                hub_sample(i);                               // an on-change sensor reports its value on enable
            }
        }
        if (i == SS_ACCEL && h->enabled && clock_ms >= h->next_sample) {
            hub_sample(i);
            h->next_sample = clock_ms + 80;                  // 12.5 Hz
        }
    }
}

static void set_lux(float lux)
{
    hub_lux = lux;
    if (hub[SS_LIGHT].enabled) hub_sample(SS_LIGHT);        // on change
}

static void run(uint64_t ms)
{
    for (const uint64_t end = clock_ms + ms; clock_ms < end; clock_ms += 10) {
        hub_step();
        ss_tick(&eng);
    }
}

static SsConditions cond;

static void conditions(int light, int accel, int display, int lid, int attached)
{
    cond = (SsConditions){light, accel, display, lid, attached};
    ss_conditions(&eng, &cond);
}

static void fresh(const SsConfig *cfg)
{
    memset(hub, 0, sizeof hub);
    for (int i = 0; i < SS_STREAMS; ++i) hub[i].client = 0x7700 + (uint64_t)i;
    allow = 1;
    memset(port_refuse, 0, sizeof port_refuse);
    tokens_taken = results_ok = results_bad = publishes = leaves = trace_lines = 0;
    memset(closes, 0, sizeof closes);
    memset(last_req_length, 0, sizeof last_req_length);
    memset(&out, 0, sizeof out);
    edge_count = 0;
    hub_lux = 300.0f;
    pose[0] = 9.81f; pose[1] = 0; pose[2] = 0;
    clock_ms = 1000;
    ss_init(&eng, &kOps, cfg);
    out.target = -1;
    out.edge = PS_EDGE_NONE;
}

static void bring_up(int light_streams)
{
    ss_port_opened(&eng, SS_LIGHT);
    ss_port_opened(&eng, SS_ACCEL);
    ss_service(&eng, SS_LIGHT, 1, 1);
    ss_service(&eng, SS_ACCEL, 1, 1);
    const uint32_t rates[1] = {fb(1.0f)};
    ss_sensor(&eng, SS_LIGHT, 1, &kLight, light_streams ? SSC_STREAM_STREAMING : SSC_STREAM_ON_CHANGE, rates[0]);
    ss_sensor(&eng, SS_ACCEL, 1, &kAccel, SSC_STREAM_STREAMING, fb(12.5f));
}

static unsigned clean(void)
{
    unsigned bad = 0;
    for (int i = 0; i < SS_STREAMS; ++i) bad += hub[i].overlapped + hub[i].nested;
    return bad;
}

// ---------------------------------------------------------------- bytes
static void test_bytes(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    bring_up(0);
    conditions(1, 1, 1, 1, 0);
    ss_tick(&eng);
    // sns_client_request_msg as libssc builds it: suid 1 {low 1, high 2 fixed64}, msg_id 2 fixed32, susp_config 3 {client processor 1 =
    // APSS (1), delivery 2 = wakeup (0)}, request 4 {...}; in QMI 0x20's TLV 0x01 with its u16 length, no TLV 0x10
    uint8_t want[96];
    uint32_t w = 0;
    want[w++] = 0; want[w++] = 1; want[w++] = 0; want[w++] = 0x20; want[w++] = 0; want[w++] = 0; want[w++] = 0;   // header, length below
    want[w++] = 0x01; want[w++] = 0; want[w++] = 0; want[w++] = 0; want[w++] = 0;                                 // TLV 0x01, u16 n
    const uint32_t message = w;
    want[w++] = 0x0a; want[w++] = 18;
    want[w++] = 0x09; wr64(want + w, kLight.low); w += 8;
    want[w++] = 0x11; wr64(want + w, kLight.high); w += 8;
    want[w++] = 0x15; wr32(want + w, 514); w += 4;               // on-change config
    want[w++] = 0x1a; want[w++] = 4; want[w++] = 0x08; want[w++] = 1; want[w++] = 0x10; want[w++] = 0;
    want[w++] = 0x22; want[w++] = 0;                             // request {}: no payload
    wr16(want + 5, w - 7);
    wr16(want + 8, w - message + 2);
    wr16(want + 10, w - message);
    CHECK(same_bytes(last_req[SS_LIGHT], last_req_length[SS_LIGHT], want, w));
    // the accelerometer: 513, request {payload 2 = sns_std_sensor_config {sample_rate 1 = 10.0f}}
    w = 0;
    want[w++] = 0; want[w++] = 1; want[w++] = 0; want[w++] = 0x20; want[w++] = 0; want[w++] = 0; want[w++] = 0;
    want[w++] = 0x01; want[w++] = 0; want[w++] = 0; want[w++] = 0; want[w++] = 0;
    want[w++] = 0x0a; want[w++] = 18;
    want[w++] = 0x09; wr64(want + w, kAccel.low); w += 8;
    want[w++] = 0x11; wr64(want + w, kAccel.high); w += 8;
    want[w++] = 0x15; wr32(want + w, 513); w += 4;
    want[w++] = 0x1a; want[w++] = 4; want[w++] = 0x08; want[w++] = 1; want[w++] = 0x10; want[w++] = 0;
    want[w++] = 0x22; want[w++] = 7; want[w++] = 0x12; want[w++] = 5; want[w++] = 0x0d; wr32(want + w, 0x41200000u); w += 4;
    wr16(want + 5, w - 7);
    wr16(want + 8, w - message + 2);
    wr16(want + 10, w - message);
    CHECK(same_bytes(last_req[SS_ACCEL], last_req_length[SS_ACCEL], want, w));
    // the disable: 10, request {}; the next transaction
    run(100);
    conditions(1, 1, 0, 1, 0);
    ss_tick(&eng);
    w = 0;
    want[w++] = 0; want[w++] = 2; want[w++] = 0; want[w++] = 0x20; want[w++] = 0; want[w++] = 0; want[w++] = 0;
    want[w++] = 0x01; want[w++] = 0; want[w++] = 0; want[w++] = 0; want[w++] = 0;
    want[w++] = 0x0a; want[w++] = 18;
    want[w++] = 0x09; wr64(want + w, kLight.low); w += 8;
    want[w++] = 0x11; wr64(want + w, kLight.high); w += 8;
    want[w++] = 0x15; wr32(want + w, 10); w += 4;
    want[w++] = 0x1a; want[w++] = 4; want[w++] = 0x08; want[w++] = 1; want[w++] = 0x10; want[w++] = 0;
    want[w++] = 0x22; want[w++] = 0;
    wr16(want + 5, w - 7);
    wr16(want + 8, w - message + 2);
    wr16(want + 10, w - message);
    CHECK(same_bytes(last_req[SS_LIGHT], last_req_length[SS_LIGHT], want, w));
    // batching (piano-sensors-accel-batch=200): request {batching 1 {batch_period 1 = 200000 us}, payload 2}
    cfg.accel_batch_ms = 200;
    fresh(&cfg);
    bring_up(0);
    conditions(0, 1, 1, 1, 0);
    ss_tick(&eng);
    static const uint8_t request[] = {0x22, 13, 0x0a, 4, 0x08, 0xc0, 0x9a, 0x0c, 0x12, 5, 0x0d, 0x00, 0x00, 0x20, 0x41};
    CHECK(last_req_length[SS_ACCEL] == 12 + 31 + sizeof request);   // QMI 7 + TLV 3 + u16 2; suid 20, msg_id 5, susp_config 6
    CHECK(same_bytes(last_req[SS_ACCEL] + 43, sizeof request, request, sizeof request));
    CHECK(hub[SS_ACCEL].batch_us == 200000 && hub[SS_ACCEL].rate_bits == fb(10.0f) && hub[SS_LIGHT].requests == 0);
    // a report written byte for byte (QMI 0x21: TLV 0x01 u64 client id, TLV 0x02 u16 n + {suid, events {1025, timestamp, {data
    // 300.0f, status 3}}}), into the light's stream: its first target
    fresh(&cfg);
    bring_up(0);
    conditions(1, 0, 1, 1, 0);
    ss_tick(&eng);
    CHECK(hub[SS_LIGHT].pending);
    hub[SS_LIGHT].pending = 0;
    respond(SS_LIGHT, hub[SS_LIGHT].txn, 1);
    uint8_t ind[96];
    uint32_t n = 0;
    ind[n++] = 4; ind[n++] = 0; ind[n++] = 0; ind[n++] = 0x21; ind[n++] = 0; ind[n++] = 0; ind[n++] = 0;
    ind[n++] = 0x01; ind[n++] = 8; ind[n++] = 0; wr64(ind + n, 0x7700); n += 8;
    ind[n++] = 0x02; ind[n++] = 0; ind[n++] = 0; ind[n++] = 0; ind[n++] = 0;
    const uint32_t body = n;
    ind[n++] = 0x0a; ind[n++] = 18; ind[n++] = 0x09; wr64(ind + n, kLight.low); n += 8; ind[n++] = 0x11; wr64(ind + n, kLight.high); n += 8;
    ind[n++] = 0x12; ind[n++] = 5 + 9 + 2 + 7;
    ind[n++] = 0x0d; wr32(ind + n, 1025); n += 4;
    ind[n++] = 0x11; wr64(ind + n, 192000000ull); n += 8;
    ind[n++] = 0x1a; ind[n++] = 7; ind[n++] = 0x0d; wr32(ind + n, 0x43960000u); n += 4; ind[n++] = 0x10; ind[n++] = 3;   // 300.0f
    wr16(ind + 5, n - 7);
    wr16(ind + body - 4, n - body + 2);
    wr16(ind + body - 2, n - body);
    deliver(SS_LIGHT, ind, n);
    CHECK(out.target == 370 && out.lux == 300 && out.seq == 1 && (out.changed & (SS_OUT_TARGET | SS_OUT_LUX)) == (SS_OUT_TARGET | SS_OUT_LUX));
    printf("PASS bytes: 514, 513 {10 Hz}, batching and 10 as libssc's request messages; a report byte for byte\n");
}

// ---------------------------------------------------------------- inert
static void test_inert(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    conditions(1, 1, 1, 1, 0);                   // consumers, but no port, no service, no sensor
    run(5000);
    CHECK(hub[0].requests == 0 && hub[1].requests == 0 && tokens_taken == 0 && ss_deadline(&eng) == 0);
    ss_port_opened(&eng, SS_LIGHT);
    ss_port_opened(&eng, SS_ACCEL);
    ss_service(&eng, SS_LIGHT, 1, 1);
    ss_service(&eng, SS_ACCEL, 1, 1);
    run(5000);                                   // a service but no sensor yet: nothing, and no timer
    CHECK(hub[0].requests == 0 && hub[1].requests == 0 && tokens_taken == 0 && ss_deadline(&eng) == 0);
    bring_up(0);
    conditions(0, 0, 1, 1, 0);                   // the sensors, but nobody reads them: nothing (battery)
    run(10000);
    CHECK(hub[0].requests == 0 && hub[1].requests == 0 && tokens_taken == 0 && ss_deadline(&eng) == 0 && ss_idle(&eng));
    conditions(1, 1, 0, 1, 0);                   // the display off
    run(5000);
    conditions(1, 1, 1, 0, 1);                   // the lid closed (the light), the cover attached (the accelerometer)
    run(5000);
    CHECK(hub[0].requests == 0 && hub[1].requests == 0 && out.seq == 0);
    char line[300];
    ss_status(&eng, SS_LIGHT, line, sizeof line);
    CHECK(strstr(line, "off (lid closed)") && strstr(line, "on change"));
    ss_status(&eng, SS_ACCEL, line, sizeof line);
    CHECK(strstr(line, "off (cover attached)") && strstr(line, "10.000 Hz asked"));
    printf("PASS inert: no consumer, no service, no sensor, display off, lid closed, cover attached: nothing sent, no timer\n");
}

// ---------------------------------------------------------------- light
static void test_light(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    bring_up(0);
    conditions(1, 0, 1, -1, 0);                  // a consumer, the display on, the lid unknown (open)
    run(100);
    CHECK(hub[SS_LIGHT].enables == 1 && hub[SS_ACCEL].requests == 0);
    CHECK(out.target == 370 && out.lux == 300 && out.seq == 1 && results_ok == 1);   // the first sample at once: 300 lux -> 370
    // brighter: smoothed towards 574 (1000 lux) at most every 250 ms, rising
    set_lux(1000.0f);
    int32_t last = out.target;
    uint64_t last_at = 0;
    unsigned targets = 0;
    for (unsigned k = 0; k < 1000 && out.target != 574; ++k) {
        const uint64_t seq = out.seq;
        run(10);
        if (out.seq != seq && (out.changed & SS_OUT_TARGET)) {
            CHECK(out.target > last && (!last_at || clock_ms - last_at >= PS_ALS_PUBLISH_MS));
            last = out.target;
            last_at = clock_ms;
            targets++;
        }
    }
    CHECK(out.target == 574 && out.lux == 1000 && targets >= 3);
    run(5000);
    CHECK(ss_deadline(&eng) == 0);               // at rest: no timer
    // inside the hysteresis: no target; the lux moves at most every 250 ms
    const uint64_t seq = out.seq;
    set_lux(990.0f);
    CHECK(out.seq == seq + 1 && out.lux == 990 && out.changed == SS_OUT_LUX && out.target == 574);
    run(100);
    set_lux(985.0f);
    CHECK(out.lux == 990 && ss_deadline(&eng) == clock_ms + 150);   // pending
    run(160);
    CHECK(out.lux == 985 && out.seq == seq + 2 && out.target == 574);
    // darkness is valid: 0 lux -> the curve's floor, slowly (4 s towards darker)
    set_lux(0.0f);
    run(30000);
    CHECK(out.target == 40 && out.lux == 0);
    CHECK(clean() == 0 && results_bad == 0);
    printf("PASS light: the first sample at once, smoothing at most every 250 ms, the hysteresis, lux rate limited, darkness\n");
}

static void test_light_cycles(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    bring_up(0);
    conditions(1, 0, 1, 1, 0);
    run(200);
    CHECK(out.target == 370);
    // 20 display sleep/wake cycles (Ubuntu v1.1.1's "light stops after off/on"): the room's level at once after each wake, never a
    // fade from the level before the sleep; samples while the display sleeps are not used
    static const float rooms[] = {0.0f, 5.0f, 20.0f, 100.0f, 400.0f, 1500.0f, 10000.0f, 50.0f, 200.0f, 3000.0f};
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        conditions(1, 0, 0, 1, 0);
        if (cycle % 3 == 1) conditions(1, 0, 1, 1, 0), conditions(1, 0, 0, 1, 0);   // a flicker while the disable goes out
        run(10);
        hub_sample(SS_LIGHT);                    // a late sample of the old session: dropped
        run(200);
        CHECK(!hub[SS_LIGHT].enabled);
        hub_lux = rooms[cycle % 10];
        run(5000);                               // asleep: nothing
        const uint64_t seq = out.seq;
        conditions(1, 0, 1, 1, 0);
        run(100);
        const uint32_t lux = (uint32_t)rooms[cycle % 10];
        CHECK(out.seq > seq && out.lux == lux && out.target == (int32_t)ps_als_curve_permille(lux));
        if (cycle % 4 == 3) {                    // the lid as well
            conditions(1, 0, 1, 0, 0);
            run(200);
            CHECK(!hub[SS_LIGHT].enabled);
            conditions(1, 0, 1, 1, 0);
            run(200);
            CHECK(hub[SS_LIGHT].enabled && out.target == (int32_t)ps_als_curve_permille(lux));
        }
    }
    CHECK(clean() == 0 && results_bad == 0 && eng.ch[SS_LIGHT].dropped >= 20 && eng.light.stream.sessions >= 21);
    CHECK(hub[SS_LIGHT].enables == hub[SS_LIGHT].disables + 1);
    // the first event before the enable's answer is the session's first; no re-arm follows
    fresh(&cfg);
    hub[SS_LIGHT].first_before = 1;
    bring_up(0);
    conditions(1, 0, 1, 1, 0);
    run(5000);
    CHECK(out.target == 370 && eng.light.stream.rearmed == 0 && hub[SS_LIGHT].enables == 1);
    // no first event at all: re-armed twice (disable, enable), then quiet until a sample comes
    fresh(&cfg);
    hub[SS_LIGHT].silent = 1;
    bring_up(0);
    conditions(1, 0, 1, 1, 0);
    run(12000);
    CHECK(eng.light.stream.rearmed == 2 && hub[SS_LIGHT].enables == 3 && hub[SS_LIGHT].disables == 2 && out.seq == 0);
    hub[SS_LIGHT].silent = 0;
    set_lux(400.0f);
    CHECK(out.target == 420 && clean() == 0);
    // a light sensor whose attributes say "streaming": 513 at its lowest advertised rate (1 Hz), samples used the same way
    fresh(&cfg);
    bring_up(1);
    conditions(1, 0, 1, 1, 0);
    run(100);
    CHECK(hub[SS_LIGHT].enables == 1 && hub[SS_LIGHT].rate_bits == fb(1.0f) && out.target == 370);
    printf("PASS light cycles: 20 display/lid off-on cycles resume at the room's level, late samples dropped, the first event before "
           "the answer, re-arm, a streaming light sensor at its lowest rate\n");
}

// ---------------------------------------------------------------- accel
static void hold_pose(float x, float y, float z, uint64_t ms)
{
    pose[0] = x; pose[1] = y; pose[2] = z;
    run(ms);
}

static void test_accel(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    bring_up(0);
    conditions(0, 1, 1, 1, 0);                   // the agent reads the edge, the display is on, the cover detached
    run(100);
    CHECK(hub[SS_ACCEL].enables == 1 && hub[SS_ACCEL].rate_bits == fb(10.0f) && hub[SS_ACCEL].batch_us == -1 && hub[SS_LIGHT].requests == 0);
    CHECK(eng.ch[SS_ACCEL].hub_rate_milli == 12500);
    hold_pose(9.81f, 0, 0, 600);                 // docked pose (SSC +x up): the top edge
    CHECK(out.edge == PS_EDGE_TOP);
    hold_pose(0, 9.81f, 0, 600);
    CHECK(out.edge == PS_EDGE_LEFT);
    hub[SS_ACCEL].packed = 1;                    // floats packed as well
    hold_pose(-9.81f, 0, 0, 600);
    CHECK(out.edge == PS_EDGE_BOTTOM);
    hold_pose(0, -9.81f, 0, 600);
    CHECK(out.edge == PS_EDGE_RIGHT);
    hold_pose(0.3f, -0.2f, 9.8f, 600);           // flat on a table
    CHECK(out.edge == PS_EDGE_NONE);
    hold_pose(7.0f, 0.5f, 6.8f, 600);            // on its stand, tilted back
    CHECK(out.edge == PS_EDGE_TOP);
    static const int order[] = {PS_EDGE_TOP, PS_EDGE_LEFT, PS_EDGE_BOTTOM, PS_EDGE_RIGHT, PS_EDGE_NONE, PS_EDGE_TOP};
    CHECK(edge_count == 6 && !memcmp(edges, order, sizeof order));
    // the cover attached: disabled, the edge none; detached: a fresh session, the edge again after SensorsOrient's hold
    conditions(0, 1, 1, 1, 1);
    run(200);
    CHECK(!hub[SS_ACCEL].enabled && out.edge == PS_EDGE_NONE && eng.accel.state == PAS_OFF);
    const uint32_t samples = hub[SS_ACCEL].samples;
    run(5000);
    CHECK(hub[SS_ACCEL].samples == samples);     // nothing streams while attached
    conditions(0, 1, 1, 1, 0);
    hold_pose(0, 9.81f, 0, 600);
    CHECK(hub[SS_ACCEL].enables == 2 && out.edge == PS_EDGE_LEFT);
    // the display off, then the agent gone (piano-rotate=0): off each time
    conditions(0, 1, 0, 1, 0);
    run(200);
    CHECK(!hub[SS_ACCEL].enabled && out.edge == PS_EDGE_NONE);
    conditions(0, 1, 1, 1, 0);
    run(600);
    CHECK(hub[SS_ACCEL].enabled && out.edge == PS_EDGE_LEFT);
    conditions(0, 0, 1, 1, 0);
    run(200);
    CHECK(!hub[SS_ACCEL].enabled && out.edge == PS_EDGE_NONE && hub[SS_ACCEL].enables == hub[SS_ACCEL].disables);
    CHECK(clean() == 0 && results_bad == 0 && hub[SS_LIGHT].requests == 0);
    printf("PASS accel: 513 at 10 Hz (the hub's 12.5 Hz logged), the four edges, flat, tilted; off with the cover, the display, the agent\n");
}

// ---------------------------------------------------------------- guard and strikes
static void test_guard(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    bring_up(0);
    allow = 0;                                   // the guard says wait (settling, quiet, backoff): nothing goes
    conditions(1, 1, 1, 1, 0);
    run(5000);
    CHECK(hub[0].requests == 0 && hub[1].requests == 0 && ss_deadline(&eng) == clock_ms + 250);
    allow = 1;
    run(100);
    CHECK(hub[0].enables == 1 && hub[1].enables == 1 && tokens_taken == 2 && results_ok == 2);
    // the port refuses a request (PianoPower's queue full): a failure for the guard, asked again later
    conditions(1, 0, 1, 1, 0);
    port_refuse[SS_ACCEL] = 1;
    ss_tick(&eng);
    CHECK(results_bad == 1 && eng.ch[SS_ACCEL].refused_port == 1 && eng.accel.state == PAS_ON);
    port_refuse[SS_ACCEL] = 0;
    run(100);
    CHECK(!hub[SS_ACCEL].enabled && results_bad == 1);
    // the hub refuses the enable: told to the guard, the stream backs off and tries again
    hub[SS_ACCEL].refuse = 1;
    conditions(1, 1, 1, 1, 0);
    run(100);
    CHECK(results_bad == 2 && eng.accel.state == PAS_OFF && eng.accel.retry_at > clock_ms);
    hub[SS_ACCEL].refuse = 0;
    run(1500);
    CHECK(hub[SS_ACCEL].enabled && eng.accel.state == PAS_ON);
    // no answer: after 2 s a failure, the hub's state unknown: a disable first, then an enable
    conditions(1, 0, 1, 1, 0);
    hub[SS_ACCEL].mute = 1;
    run(2100);
    CHECK(results_bad == 3 && eng.ch[SS_ACCEL].timeouts == 1 && eng.accel.dirty);
    hub[SS_ACCEL].mute = 0;
    run(1200);
    CHECK(!eng.accel.dirty && eng.accel.state == PAS_OFF && !hub[SS_ACCEL].enabled);
    // an error event after the enable's answer: a failure, a disable first (after the backoff), then a fresh enable that streams
    hub[SS_LIGHT].error_after = 1;
    conditions(0, 0, 1, 1, 0);
    run(200);
    conditions(1, 0, 1, 1, 0);
    const uint32_t bad = results_bad, enables = hub[SS_LIGHT].enables;
    run(100);
    CHECK(results_bad == bad + 1 && eng.ch[SS_LIGHT].errors == 1 && eng.light.stream.dirty);
    run(1200);
    CHECK(hub[SS_LIGHT].enables == enables + 2 && hub[SS_LIGHT].enabled && eng.light.stream.state == PAS_ON && !eng.light.stream.dirty);
    CHECK(clean() == 0);
    // the hub refuses every enable: the stream fails for the boot; the light's consumer told to leave automatic mode once
    hub[SS_LIGHT].refuse = 1;
    ss_service(&eng, SS_LIGHT, 1, 2);            // a restart of the service: a fresh client, which the hub now refuses
    hub[SS_LIGHT].enabled = 0;
    run(70000);
    CHECK(eng.light.stream.state == PAS_FAILED && leaves == 1 && hub[SS_LIGHT].enables == enables + 2 + 6);
    run(70000);
    CHECK(hub[SS_LIGHT].enables == enables + 8 && leaves == 1);
    // a strike: nothing more is sent, packets are ignored, the outputs at rest; the ports that carried a request are closed when the
    // glue says the DSP may still be told
    fresh(&cfg);
    bring_up(0);
    conditions(1, 1, 1, 1, 0);
    hold_pose(0, 9.81f, 0, 600);
    CHECK(out.edge == PS_EDGE_LEFT && out.target == 370 && hub[0].enabled && hub[1].enabled);
    const uint32_t sent = hub[0].requests + hub[1].requests, s0 = out.seq;
    ss_off(&eng);
    CHECK(out.edge == PS_EDGE_NONE && leaves == 1 && out.seq == s0 + 1);
    run(10000);
    set_lux(5000.0f);
    conditions(1, 1, 0, 1, 1);
    run(1000);
    CHECK(hub[0].requests + hub[1].requests == sent && out.seq == s0 + 1 && out.target == 370 && ss_deadline(&eng) == 0 && ss_idle(&eng));
    CHECK(eng.ch[SS_LIGHT].ignored > 0 && eng.ch[SS_ACCEL].ignored > 0);
    ss_close_all(&eng);
    CHECK(closes[SS_LIGHT] == 1 && closes[SS_ACCEL] == 1);
    ss_close_all(&eng);
    CHECK(closes[SS_LIGHT] == 1);
    // ... a stream that never sent anything keeps its port (nothing to forget at the hub); no target given: nobody to tell
    fresh(&cfg);
    bring_up(0);
    conditions(0, 1, 1, 1, 0);
    run(200);
    ss_off(&eng);
    ss_close_all(&eng);
    CHECK(closes[SS_LIGHT] == 0 && closes[SS_ACCEL] == 1 && leaves == 0);
    // ... and through the real guard: its tokens pace the requests (8 at once, then 4 per second)
    SgGuard g;
    sg_init(&g, NULL);
    SgDsp d = {1, 0, 0, SG_CHARGER_UP, 1, 1, NULL};
    sg_observe(&g, &d, 0);
    CHECK(sg_observe(&g, &d, g.cfg.settle_ms) == SG_RUN);
    unsigned granted = 0;
    for (unsigned k = 0; k < 40; ++k) granted += (unsigned)sg_may_request(&g, g.cfg.settle_ms);
    CHECK(granted == g.cfg.bucket);
    printf("PASS guard: no token no request, port and hub refusals, missing answers and error events told to the guard, failed for the "
           "boot with one leave, a strike: nothing more, outputs at rest, ports closed\n");
}

// ---------------------------------------------------------------- sleep
static void test_sleep(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    bring_up(0);
    conditions(1, 1, 1, 1, 0);
    hold_pose(9.81f, 0, 0, 600);
    CHECK(hub[0].enabled && hub[1].enabled && !ss_idle(&eng));
    // the system goes to sleep: both disabled, idle before the glue's wait ends; nothing closed
    ss_sleep(&eng, 1);
    run(100);
    CHECK(ss_idle(&eng) && !hub[0].enabled && !hub[1].enabled && out.edge == PS_EDGE_NONE);
    ss_force_off(&eng);
    CHECK(closes[0] == 0 && closes[1] == 0);
    conditions(1, 1, 1, 1, 0);                   // the conditions as before, still asleep: nothing
    run(1000);
    CHECK(!hub[0].enabled && !hub[1].enabled);
    ss_sleep(&eng, 0);                           // the wake: fresh sessions, the room's level at once
    hub_lux = 50.0f;
    run(100);
    CHECK(hub[0].enabled && hub[1].enabled && out.target == (int32_t)ps_als_curve_permille(50) && clean() == 0);
    // a hub that does not answer the disables: the wait ends, the ports are closed (the hub forgets the clients), new ones after the wake
    hub[0].mute = hub[1].mute = 1;
    ss_sleep(&eng, 1);
    run(800);
    CHECK(!ss_idle(&eng));
    ss_force_off(&eng);
    CHECK(closes[0] == 1 && closes[1] == 1 && !eng.ch[0].open && !eng.ch[1].open && ss_idle(&eng));
    hub[0].mute = hub[1].mute = 0;
    hub[0].enabled = hub[1].enabled = 0;         // the hub dropped the gone clients' requests
    hub[0].pending = hub[1].pending = 0;
    ss_sleep(&eng, 0);
    run(1000);
    CHECK(hub[0].enables == 2 && hub[1].enables == 2);   // nothing on a closed port
    for (int i = 0; i < SS_STREAMS; ++i) {       // the glue opens new ports
        ss_port_opened(&eng, i);
        ss_service(&eng, i, 1, 1);
    }
    run(700);
    CHECK(hub[0].enabled && hub[1].enabled && out.edge == PS_EDGE_TOP && clean() == 0);
    printf("PASS sleep: disables before the sleep, idle in time; unanswered: ports closed, fresh clients after the wake\n");
}

// ---------------------------------------------------------------- service, the level-3 list end to end
static ScClient list;
static uint8_t list_req[300];
static uint32_t list_req_length, list_sent;
static uint64_t list_now(void *c) {(void)c; return clock_ms;}
static int list_send(void *c, const uint8_t *d, uint32_t n) {(void)c; memcpy(list_req, d, n); list_req_length = n; list_sent++; return 0;}
static int list_may(void *c) {(void)c; return 1;}
static void list_result(void *c, int ok) {(void)c; (void)ok;}
static void list_trace(void *c, const char *l) {(void)c; (void)l;}
static void list_changed(void *c) {(void)c;}

static void list_report(const SscSuid *suid, uint32_t msg_id, const uint8_t *payload, uint32_t length)
{
    HubEvent ev = {msg_id, {0}, length};
    CHECK(length <= sizeof ev.payload);
    memcpy(ev.payload, payload, length);
    uint8_t body[400], ind[500];
    SpbOut o;
    spb_out(&o, body, sizeof body);
    uint32_t mark = spb_open(&o, 1);
    spb_put_fixed64(&o, 1, suid->low);
    spb_put_fixed64(&o, 2, suid->high);
    spb_close(&o, mark);
    mark = spb_open(&o, 2);
    spb_put_fixed32(&o, 1, ev.msg_id);
    spb_put_fixed64(&o, 2, 1);
    spb_put_bytes(&o, 3, ev.payload, ev.length);
    spb_close(&o, mark);
    const uint32_t b = spb_done(&o);
    uint32_t n = 0;
    const uint32_t tlvs = 11 + 5 + b;
    ind[n++] = SSC_QMI_INDICATION; ind[n++] = 0; ind[n++] = 0; ind[n++] = 0x21; ind[n++] = 0;
    ind[n++] = (uint8_t)tlvs; ind[n++] = (uint8_t)(tlvs >> 8);
    ind[n++] = 0x01; ind[n++] = 8; ind[n++] = 0; wr64(ind + n, 0x42); n += 8;
    ind[n++] = 0x02; wr16(ind + n, b + 2); n += 2; wr16(ind + n, b); n += 2;
    memcpy(ind + n, body, b);
    n += b;
    sc_receive(&list, ind, n);
}

// The list port's hub: every lookup and attribute request answered (light on change, accel streaming with its rates).
static void list_serve(void)
{
    for (unsigned r = 0; r < 100 && !sc_settled(&list); ++r) {
        const uint32_t before = list_sent;
        sc_tick(&list);
        if (list_sent == before) {clock_ms += 250; continue;}
        const Req q = parse_req(list_req, list_req_length);
        uint8_t resp[32];
        uint32_t n = 0;
        resp[n++] = SSC_QMI_RESPONSE; resp[n++] = (uint8_t)q.txn; resp[n++] = (uint8_t)(q.txn >> 8); resp[n++] = 0x20; resp[n++] = 0;
        resp[n++] = 7; resp[n++] = 0;
        resp[n++] = 0x02; resp[n++] = 4; resp[n++] = 0; resp[n++] = 0; resp[n++] = 0; resp[n++] = 0; resp[n++] = 0;
        sc_receive(&list, resp, n);
        uint8_t p[300];
        SpbOut o;
        spb_out(&o, p, sizeof p);
        if (q.msg_id == SSC_MSG_SUID_REQ) {
            // which type: the request's payload names it (as SensorsClient.c asks: registry, accel, gyro, mag, ambient_light, proximity)
            const char *type = sc_type_name[list.pending_type >= 0 ? list.pending_type : 0];
            spb_put_string(&o, 1, type);
            const SscSuid *s = !strcmp(type, "accel") ? &kAccel : !strcmp(type, "ambient_light") ? &kLight : NULL;
            if (s) {
                const uint32_t m = spb_open(&o, 2);
                spb_put_fixed64(&o, 1, s->low);
                spb_put_fixed64(&o, 2, s->high);
                spb_close(&o, m);
            }
            list_report(&ssc_suid_lookup, SSC_MSG_SUID_EVENT, p, spb_done(&o));
        } else {
            const int accel = ssc_suid_equal(&q.suid, &kAccel);
            uint32_t a = spb_open(&o, 1);                 // stream type: 0 streaming, 1 on change (sfixed64)
            spb_put_varint(&o, 1, SSC_ATTR_STREAM_TYPE);
            uint32_t v = spb_open(&o, 2), one = spb_open(&o, 1);
            spb_put_fixed64(&o, 4, accel ? 0 : 1);
            spb_close(&o, one);
            spb_close(&o, v);
            spb_close(&o, a);
            a = spb_open(&o, 1);                          // rates: 26, 12.5, 52 Hz (the lowest is not the first)
            spb_put_varint(&o, 1, SSC_ATTR_RATES);
            v = spb_open(&o, 2);
            const float rates[] = {26.0f, 12.5f, 52.0f};
            for (unsigned k = 0; k < 3; ++k) {
                one = spb_open(&o, 1);
                spb_put_fixed32(&o, 3, fb(rates[k]));
                spb_close(&o, one);
            }
            spb_close(&o, v);
            spb_close(&o, a);
            list_report(&q.suid, SSC_MSG_ATTR_EVENT, p, spb_done(&o));
        }
        clock_ms += 10;
    }
}

static void test_service(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    // the level-3 list finds both sensors; the streams take them only once their attributes are read
    static const ScOps lops = {NULL, list_now, list_send, list_may, list_result, list_trace, list_changed};
    sc_init(&list, &lops);
    sc_service(&list, 1, 1);
    ss_port_opened(&eng, SS_LIGHT);
    ss_port_opened(&eng, SS_ACCEL);
    ss_service(&eng, SS_LIGHT, 1, 1);
    ss_service(&eng, SS_ACCEL, 1, 1);
    conditions(1, 1, 1, 1, 0);
    ss_from_list(&eng, &list);
    run(1000);
    CHECK(hub[0].requests == 0 && hub[1].requests == 0);
    list_serve();
    CHECK(sc_settled(&list) && list.sensor[SC_ACCEL].attrs.stream_type == 0 && list.sensor[SC_LIGHT].attrs.stream_type == 1);
    ss_from_list(&eng, &list);
    CHECK(eng.ch[SS_LIGHT].found && eng.ch[SS_LIGHT].on_change && eng.ch[SS_ACCEL].found && eng.ch[SS_ACCEL].rate_bits == fb(10.0f));
    run(700);
    CHECK(hub[0].enabled && hub[1].enabled && out.target == 370 && out.edge == PS_EDGE_TOP);
    ss_from_list(&eng, &list);                   // the same list again: nothing changes
    run(100);
    CHECK(hub[0].enables == 1 && hub[1].enables == 1);
    // the service moves (a new generation: the hub forgot everything): fresh enables
    hub[0].enabled = hub[1].enabled = 0;
    ss_service(&eng, SS_LIGHT, 1, 2);
    ss_service(&eng, SS_ACCEL, 1, 2);
    CHECK(out.edge == PS_EDGE_NONE);
    run(700);
    CHECK(hub[0].enables == 2 && hub[1].enables == 2 && hub[0].disables == 0 && out.edge == PS_EDGE_TOP && clean() == 0);
    // the service goes: nothing is sent, the edge none; it comes back: fresh
    ss_service(&eng, SS_ACCEL, 0, 3);
    hub[1].enabled = 0;
    run(1000);
    CHECK(hub[1].requests == 2 && out.edge == PS_EDGE_NONE);
    ss_service(&eng, SS_ACCEL, 1, 4);
    run(700);
    CHECK(hub[1].enables == 3 && out.edge == PS_EDGE_TOP);
    // a new default sensor while streaming: the old client's port is closed (its request goes with it), a new port starts clean
    const SscSuid other = {0x99, 0x98};
    ss_sensor(&eng, SS_ACCEL, 1, &other, SSC_STREAM_STREAMING, 0);
    CHECK(closes[SS_ACCEL] == 1 && !eng.ch[SS_ACCEL].open);
    hub[1].enabled = 0;
    hub[1].suid = other;
    ss_port_opened(&eng, SS_ACCEL);
    ss_service(&eng, SS_ACCEL, 1, 5);
    run(100);
    CHECK(hub[1].enables == 4 && ssc_suid_equal(&eng.ch[SS_ACCEL].suid, &other));
    // malformed and stray input is counted, never trusted
    static const uint8_t junk[] = {4, 1, 0, 0x21, 0, 3, 0, 0x02, 9, 0};
    const uint32_t m0 = eng.ch[SS_ACCEL].malformed, u0 = eng.ch[SS_ACCEL].unexpected;
    ss_receive(&eng, SS_ACCEL, junk, sizeof junk);
    ss_receive(&eng, SS_ACCEL, junk, 3);
    respond(SS_ACCEL, 0x7777, 1);                // an answer to no request of ours
    const HubEvent ev = sensor_event(pose, 2, 3, 0);
    report(SS_ACCEL, &other, &ev, 1);            // two floats: not an acceleration
    report(SS_ACCEL, &kLight, &ev, 1);           // another sensor's report on this client
    CHECK(eng.ch[SS_ACCEL].malformed >= m0 + 3 && eng.ch[SS_ACCEL].unexpected >= u0 + 2);
    ss_receive(&eng, 7, junk, sizeof junk);      // no such stream
    // an unavailable sensor (attributes: available false) is not taken: its stream restarts without a sensor (nothing is sent, its
    // port stays); the list's accel, kAccel again, replaces "other" while it streams: that client's port is closed as above
    list.sensor[SC_LIGHT].attrs.available = 0;
    const uint32_t light_sent = hub[SS_LIGHT].requests;
    ss_from_list(&eng, &list);
    run(1000);
    CHECK(!eng.ch[SS_LIGHT].found && closes[SS_LIGHT] == 0 && closes[SS_ACCEL] == 2 && hub[SS_LIGHT].requests == light_sent);
    printf("PASS service: the level-3 list feeding both streams, generations, the service gone and back, a new default sensor, an "
           "unavailable one, junk\n");
}

// ---------------------------------------------------------------- the log's limiter
static void test_log(void)
{
    SsConfig cfg;
    ss_defaults(&cfg);
    fresh(&cfg);
    bring_up(0);
    conditions(1, 0, 1, 1, 0);
    run(100);
    trace_lines = 0;
    for (unsigned k = 0; k < 40; ++k) {          // 40 quick display off/on cycles: 160 transitions and first samples in 40 s
        conditions(1, 0, 0, 1, 0);
        run(500);
        conditions(1, 0, 1, 1, 0);
        run(500);
    }
    CHECK(trace_lines >= 1 && trace_lines <= SS_LOG_BURST + 40000 / SS_LOG_REFILL_MS + 1);
    CHECK(strstr(last_trace, "lines left out") || strstr(last_trace, "line left out"));
    run(SS_LOG_BURST * SS_LOG_REFILL_MS);        // the budget refills
    trace_lines = 0;
    conditions(1, 0, 0, 1, 0);
    run(500);
    CHECK(trace_lines >= 1 && strstr(last_trace, "light: off (display off)"));
    char line[300];
    ss_status(&eng, SS_LIGHT, line, sizeof line);
    CHECK(strstr(line, "off (display off)") && strstr(line, "sessions=41") && strstr(line, "samples"));
    printf("PASS log: a burst of %u lines, then one per %u s, the lines left out counted; the status line complete\n", SS_LOG_BURST,
           SS_LOG_REFILL_MS / 1000);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_bytes();
    test_inert();
    test_light();
    test_light_cycles();
    test_accel();
    test_guard();
    test_sleep();
    test_service();
    test_log();
    printf("PASS PianoSensors stream tests: %u checks\n", checks);
    return 0;
}
