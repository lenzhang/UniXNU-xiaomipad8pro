// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// Host tests of PianoSensors' own parts and independent cross-checks of the imported codecs (docs/drivers/sensors.md section 4):
//   guard     SensorsGuard.c: wait for the charger, settle, tokens, backoff, strikes, one-strike rules, the file-loop detector
//   policy    SensorsPolicy.c: the light curve, smoothing and hysteresis; SSC accelerometer floats -> panel edge (Ubuntu's mount matrix
//             under iio-sensor-proxy 3.9's rule); the rotation with the keyboard cover's lock
//   qmi/sns   the SNS frames against libqmi's SSC service description (message 0x20/0x21/0x22, TLVs 0x01/0x02/0x10/0x11) and the
//             protobuf against libssc's messages, written here byte by byte from those descriptions (facts only, no code)
//   client    SensorsClient.c against a simulated hub; SensorsHealth.h (PianoPower's report as the guard reads it); the arena check
//   fastrpc   the file service's bring-up against Linux fastrpc.c (40-byte invoke, metadata layout, context bank in bits 32 and up,
//             client id 0 on the attach) and qualcomm/fastrpc's IDL (remotectl 0, adsp_listener 3, method and scalars numbers)
#include <math.h>
#include "check.h"
#include "../SensorsGuard.h"
#include "../SensorsPolicy.h"
#include "../SensorsSsc.h"
#include "../SensorsRpc.h"
#include "../SensorsClient.h"
#include "../SensorsHealth.h"
#include "../SensorsPb.h"
#include "../../PianoPower/QcomQrtr.h"

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t rd64(const uint8_t *p) {return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;}
static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void wr64(uint8_t *p, uint64_t v) {wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));}

// ---------------------------------------------------------------- guard
static SgDsp healthy(void)
{
    SgDsp d = {1, 0, 0, SG_CHARGER_UP, 1, 7, NULL};
    return d;
}

static void guard(void)
{
    SgGuard g;
    SgConfig c;
    sg_defaults(&c);
    sg_init(&g, &c);
    SgDsp d = healthy();
    // nothing before the DSP and its charger domain are up, and not before they settled
    d.dsp_up = 0;
    CHECK(sg_observe(&g, &d, 1000) == SG_WAIT && !sg_may_request(&g, 1000));
    d = healthy();
    d.charger_state = 0;
    CHECK(sg_observe(&g, &d, 2000) == SG_WAIT);
    d = healthy();
    CHECK(sg_observe(&g, &d, 3000) == SG_WAIT && !sg_may_request(&g, 3000));
    d.quiet = 1;   // PianoPower asks for quiet: the settling starts over
    CHECK(sg_observe(&g, &d, 10000) == SG_WAIT);
    d.quiet = 0;
    CHECK(sg_observe(&g, &d, 11000) == SG_WAIT);
    CHECK(sg_observe(&g, &d, 11000 + c.settle_ms - 1) == SG_WAIT);
    CHECK(sg_observe(&g, &d, 11000 + c.settle_ms) == SG_RUN);
    uint64_t t = 11000 + c.settle_ms;
    // the bucket: 8 at once, then 4 per second
    for (unsigned i = 0; i < c.bucket; ++i) CHECK(sg_may_request(&g, t));
    CHECK(!sg_may_request(&g, t));
    CHECK(!sg_may_request(&g, t + 249) && sg_may_request(&g, t + 250) && !sg_may_request(&g, t + 251));
    t += 10000;
    CHECK(sg_observe(&g, &d, t) == SG_RUN && sg_serve_files(&g));
    // backoff: 1, 2, 4 s; a success resets it
    sg_result(&g, 0, t);
    CHECK(g.state == SG_BACKOFF && !sg_may_request(&g, t + 999) && sg_may_request(&g, t + 1000));
    sg_result(&g, 0, t + 1000);
    CHECK(!sg_may_request(&g, t + 2999) && sg_may_request(&g, t + 3000));
    sg_result(&g, 1, t + 3000);
    sg_result(&g, 0, t + 3000);
    CHECK(!sg_may_request(&g, t + 3999) && sg_may_request(&g, t + 4000) && sg_serve_files(&g));
    // strikes: the sixth failure within ten minutes is the end
    sg_init(&g, &c);
    d = healthy();
    sg_observe(&g, &d, 0);
    CHECK(sg_observe(&g, &d, c.settle_ms) == SG_RUN);
    t = c.settle_ms;
    for (unsigned i = 0; i < 5; ++i) {t += 70000; sg_result(&g, 0, t);}
    CHECK(g.state == SG_BACKOFF);
    t += 70000;
    sg_result(&g, 0, t);
    CHECK(g.state == SG_OFF && g.reason == SG_REASON_ERRORS && !sg_serve_files(&g) && !sg_may_request(&g, t + 3600000));
    CHECK(sg_observe(&g, &d, t + 3600000) == SG_OFF);
    // spread out (one every 3 minutes) they never add up; the backoff saturates at 64 s
    sg_init(&g, &c);
    sg_observe(&g, &d, 0);
    sg_observe(&g, &d, c.settle_ms);
    t = c.settle_ms;
    for (unsigned i = 0; i < 40; ++i) {t += 180000; sg_result(&g, 0, t);}
    CHECK(g.state != SG_OFF && g.backoff_ms == c.backoff_max_ms);
    // one strike: the fatal bit, a crash reason, the charger's new generation, its domain down, the link's new generation, link down
    for (int k = 0; k < 6; ++k) {
        sg_init(&g, &c);
        d = healthy();
        sg_observe(&g, &d, 0);
        CHECK(sg_observe(&g, &d, c.settle_ms) == SG_RUN);
        static const char reason[] = "err_qdi.c:1215:EF:qsh_process:0x4:UTIMER_CLIENT_4:0x1000002f:sns_watchdog.c:84";
        if (k == 0) d.fatal = 1;
        if (k == 1) d.crash = reason;
        if (k == 2) d.charger_ups++;
        if (k == 3) d.charger_state = 0;
        if (k == 4) d.link_generation++;
        if (k == 5) d.dsp_up = 0;
        static const int want[] = {SG_REASON_FATAL, SG_REASON_CRASH, SG_REASON_CHARGER, SG_REASON_CHARGER, SG_REASON_LINK, SG_REASON_LINK};
        CHECK(sg_observe(&g, &d, c.settle_ms + 5) == SG_OFF && g.reason == want[k] && !sg_serve_files(&g));
        d = healthy();
        CHECK(sg_observe(&g, &d, c.settle_ms + 600000) == SG_OFF);   // for the rest of the boot
    }
    // a DSP that is already dead is never touched, not even before the settling
    sg_init(&g, &c);
    d = healthy();
    d.fatal = 1;
    CHECK(sg_observe(&g, &d, 0) == SG_OFF && g.reason == SG_REASON_FATAL);
    // the charger may restart before we start (PianoPower's own business): we only begin from its newest generation
    sg_init(&g, &c);
    d = healthy();
    sg_observe(&g, &d, 0);
    d.charger_ups = 2;   // a new generation: the settling starts over
    CHECK(sg_observe(&g, &d, c.settle_ms) == SG_WAIT && sg_observe(&g, &d, 2 * c.settle_ms - 1) == SG_WAIT);
    CHECK(sg_observe(&g, &d, 2 * c.settle_ms) == SG_RUN && g.charger_ups0 == 2);
    // the file-call loop: more than 3000 in one minute
    for (unsigned i = 0; i < 3000; ++i) sg_file_call(&g, 2 * c.settle_ms + 100 + i);
    CHECK(g.state == SG_RUN);
    sg_file_call(&g, 2 * c.settle_ms + 59000);
    CHECK(g.state == SG_OFF && g.reason == SG_REASON_FILE_STORM && !sg_serve_files(&g));
    sg_init(&g, &c);
    sg_observe(&g, &d, 0);
    sg_observe(&g, &d, c.settle_ms);
    for (unsigned m = 0; m < 5; ++m)
        for (unsigned i = 0; i < 2900; ++i) sg_file_call(&g, c.settle_ms + 1 + m * 60000 + i * 20);
    CHECK(g.state == SG_RUN);
    sg_disable(&g);
    CHECK(g.state == SG_OFF && g.reason == SG_REASON_DISABLED);
    CHECK(sg_crash_is_sensors("err_qdi.c:EF:qsh_process:0x4") && sg_crash_is_sensors("sns_watchdog.c:84:thread idx 1"));
    CHECK(!sg_crash_is_sensors("charger_pd: battmngr") && !sg_crash_is_sensors(NULL));
    for (int s = 0; s < 4; ++s) CHECK(strcmp(sg_state_name(s), "?"));
    for (int r = 0; r < 8; ++r) CHECK(strcmp(sg_reason_name(r), "?"));
    printf("PASS guard: waiting, settling, tokens, backoff, strikes, one-strike rules, the file loop\n");
}

// ---------------------------------------------------------------- policy
static int32_t als_run(PsAls *a, int32_t milli, uint64_t *t, uint32_t ms, int32_t *last)
{
    int32_t out = ps_als_step(a, 1, milli, *t);
    if (out >= 0) *last = out;
    int sent = out >= 0;
    for (uint32_t done = 0; done < ms; done += PS_ALS_PUBLISH_MS) {
        *t += PS_ALS_PUBLISH_MS;
        out = ps_als_step(a, 0, 0, *t);
        if (out >= 0) {
            CHECK(*last < 0 || abs(out - *last) >= (int)PS_ALS_STEP || out == (int32_t)a->goal);
            *last = out;
            sent++;
        }
    }
    return sent;
}

static void policy(void)
{
    // the curve: its points, monotonic, flat beyond the ends
    for (unsigned i = 0; i < ps_als_curve_points; ++i) CHECK(ps_als_curve_permille(ps_als_curve[i].lux) == ps_als_curve[i].permille);
    uint32_t prev = 0;
    for (uint32_t lux = 0; lux <= 20000; lux += 7) {
        const uint32_t p = ps_als_curve_permille(lux);
        CHECK(p >= prev && p <= 1000);
        prev = p;
    }
    CHECK(ps_als_curve_permille(0) == 40 && ps_als_curve_permille(150) == 280 && ps_als_curve_permille(1u << 31) == 1000);
    // smoothing (time constants 1 s up, 4 s down): the first sample at once, then about 93 % of a brighter step in 3 s and 63 % of a
    // darker one in 4 s, the goal itself in the end; noise inside the hysteresis does nothing
    PsAls a;
    ps_als_init(&a);
    uint64_t t = 1000;
    int32_t last = -1;
    CHECK(als_run(&a, 300000, &t, 2000, &last) == 1 && last == 370);
    CHECK(als_run(&a, 330000, &t, 5000, &last) == 0);                 // 300 -> 330 lux: 15 permille, inside the hysteresis
    const int sent = als_run(&a, 3000000, &t, 3000, &last);           // to 3000 lux
    CHECK(last >= 750 && last < 800 && sent >= 5 && sent <= 13);
    als_run(&a, 3000000, &t, 6000, &last);
    CHECK(last == 800);
    als_run(&a, 0, &t, 4000, &last);                                  // darkness (0 lux is valid): slower
    CHECK(last > 250 && last < 400);
    als_run(&a, -5, &t, 30000, &last);
    CHECK(last == 40);
    CHECK(ps_als_step(&a, 0, 0, t + 100000) == -1);                   // settled: nothing more
    // the axes: SSC accelerometer floats (m/s^2, the reaction to gravity: +9.81 on the axis that points up) to the edge that is up
    SfxAxes axes, gyro, mag;
    sfx_axes_identity(&axes);
    sfx_axes_identity(&gyro);
    sfx_axes_identity(&mag);
    CHECK(sfx_axes_parse(PS_ACCEL_AXES, &axes, &gyro, &mag) == 0);
    static const struct {float x, y, z; int edge;} poses[] = {
        {9.81f, 0, 0, PS_EDGE_TOP},          // docked: SSC +x up -> iio-sensor-proxy "normal" (Ubuntu's rule)
        {0, 9.81f, 0, PS_EDGE_LEFT},         // SSC +y up -> its X = +y > 0 -> "left-up"
        {-9.81f, 0, 0, PS_EDGE_BOTTOM},
        {0, -9.81f, 0, PS_EDGE_RIGHT},
        {7.0f, 0.5f, 6.8f, PS_EDGE_TOP},     // tilted back on the stand: still the top edge
        {0.3f, -0.2f, 9.8f, PS_EDGE_NONE},   // flat on a table, face up: keep
    };
    for (unsigned i = 0; i < sizeof poses / sizeof poses[0]; ++i) {
        SnoState s;
        sno_init(&s);
        const uint32_t bits[3] = {fbits(poses[i].x), fbits(poses[i].y), fbits(poses[i].z)};
        int state = -1;
        for (uint64_t ms = 0; ms <= 400; ms += 40) {
            const int got = ps_accel_sample(&s, &axes, bits, ms);
            if (got >= 0) state = got;
        }
        CHECK(state >= 0 && ps_edge_of(state) == poses[i].edge);
        if (poses[i].edge == PS_EDGE_NONE) CHECK(state == SNO_FACE_UP);
    }
    CHECK(!strcmp(ps_edge_name(PS_EDGE_LEFT), "left") && !strcmp(ps_edge_name(PS_EDGE_NONE), "none"));
    // rotation: the cover forces the normal landscape at once; detached, an edge must settle; the lock keeps; flat keeps
    PsRotationConfig rc;
    PsRotation r;
    ps_rotation_defaults(&rc);
    ps_rotation_init(&r);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_LEFT, 1, 0, 0) == 0);          // attached: normal, whatever the sensor says
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_LEFT, 1, 0, 5000) == -1);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_LEFT, 0, 0, 6000) == -1);      // detached: the edge settles first
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_LEFT, 0, 0, 6599) == -1);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_LEFT, 0, 0, 6600) == 90);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_NONE, 0, 0, 7000) == -1);      // laid flat: keep 90
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_BOTTOM, 0, 0, 7100) == -1);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_BOTTOM, 0, 0, 7700) == 180);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_RIGHT, 0, 0, 7710) == -1);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_RIGHT, 0, 0, 8310) == -1);     // settled, but within a second of the last rotation
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_RIGHT, 0, 0, 8700) == 270);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_TOP, 0, 1, 9000) == -1);       // locked
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_TOP, 0, 1, 20000) == -1);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_TOP, 0, 0, 20001) == 0);       // unlocked: the settled edge applies
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_RIGHT, 0, 0, 30000) == -1);
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_RIGHT, 1, 0, 30700) == -1);    // attached while already normal: nothing to do
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_RIGHT, 0, 0, 30800) == -1);    // detached again: settles afresh
    CHECK(ps_rotation_step(&r, &rc, PS_EDGE_RIGHT, 0, 0, 31400) == 270);
    printf("PASS policy: the light curve and its smoothing, SSC axes to the panel's edges, rotation with the cover and the lock\n");
}

// ---------------------------------------------------------------- QMI and SNS against libqmi / libssc
static void qmi_sns(void)
{
    // SscSuidRequest {data_type 1 = "accel", enable_updates 2 = 1, only_default_values 3 = 1}
    static const uint8_t suid_req[] = {0x0a, 5, 'a', 'c', 'c', 'e', 'l', 0x10, 1, 0x18, 1};
    uint8_t got[256], msg[512], qmi[600];
    uint32_t n = ssc_build_suid_req(got, sizeof got, PS_TYPE_ACCEL, 1, 1);
    CHECK(same_bytes(got, n, suid_req, sizeof suid_req));
    // SscClientRequest {uid 1 {low 1 fixed64, high 2 fixed64}, msg_id 2 fixed32, config 3 {processor 1 = 1 (APSS), suspend_mode 2 = 0},
    // request 4 {msg 2 = the SUID request}} to the SUID sensor 0xabab... with message 512
    uint8_t want[128];
    uint32_t w = 0;
    want[w++] = 0x0a; want[w++] = 18;
    want[w++] = 0x09; for (int i = 0; i < 8; ++i) want[w++] = 0xab;
    want[w++] = 0x11; for (int i = 0; i < 8; ++i) want[w++] = 0xab;
    want[w++] = 0x15; wr32(want + w, 512); w += 4;
    want[w++] = 0x1a; want[w++] = 4; want[w++] = 0x08; want[w++] = 1; want[w++] = 0x10; want[w++] = 0;
    want[w++] = 0x22; want[w++] = (uint8_t)(2 + sizeof suid_req); want[w++] = 0x12; want[w++] = sizeof suid_req;
    memcpy(want + w, suid_req, sizeof suid_req); w += sizeof suid_req;
    const uint32_t m = ssc_build_request(msg, sizeof msg, &ssc_suid_lookup, SSC_MSG_SUID_REQ, SSC_DELIVERY_WAKEUP, got, n, -1);
    CHECK(same_bytes(msg, m, want, w));
    // QMI request 0x0020: type 0 (request), transaction, message id, TLV length; TLV 0x01 "Data" = u16 length + bytes; TLV 0x10
    // "Report Type" u8 (0 small, 1 large) after it
    uint32_t q = ssc_build_qmi_request(qmi, sizeof qmi, 0x1234, msg, m, 1);
    CHECK(q == 7 + 3 + 2 + m + 4 && qmi[0] == 0 && qmi[1] == 0x34 && qmi[2] == 0x12 && qmi[3] == 0x20 && qmi[4] == 0);
    CHECK(qmi[5] == (uint8_t)(q - 7) && qmi[6] == 0 && qmi[7] == 0x01 && qmi[8] == (uint8_t)(m + 2) && qmi[10] == (uint8_t)m);
    CHECK(!memcmp(qmi + 12, msg, m) && qmi[12 + m] == 0x10 && qmi[13 + m] == 1 && qmi[14 + m] == 0 && qmi[15 + m] == 1);
    CHECK(ssc_build_qmi_request(qmi, sizeof qmi, 1, msg, m, -1) == 7 + 3 + 2 + m);
    // ... and PianoPower's own QMI parser reads it the same way
    QcomQmiMessage pm;
    uint32_t size = 0;
    CHECK(!qcom_qmi_parse(qmi, q, &pm) && pm.type == QCOM_QMI_REQUEST && pm.transaction == 1 && pm.message == 0x20);
    const uint8_t *data = qcom_qmi_tlv(&pm, 0x01, &size);
    CHECK(data && size == m + 2 && data[0] == (uint8_t)m);
    // the response: TLV 0x02 result (u16 status, u16 error), 0x10 u64 client id, 0x11 u32 response
    static const uint8_t response[] = {2, 0x34, 0x12, 0x20, 0, 25, 0, 0x02, 4, 0, 0, 0, 0, 0, 0x10, 8, 0, 0x88, 0x77, 0x66, 0x55, 0x44,
                                       0x33, 0x22, 0x11, 0x11, 4, 0, 0, 0, 0, 0};
    SscQmi sq;
    SscResponse sr;
    CHECK(!ssc_qmi_parse(response, sizeof response, &sq) && sq.type == SSC_QMI_RESPONSE && sq.message == SSC_QMI_REQ);
    CHECK(!ssc_qmi_response(&sq, &sr) && ssc_response_ok(&sr) && sr.has_client && sr.client_id == 0x1122334455667788ull);
    CHECK(sr.has_status && sr.status == 0);
    // a refusal: status 1, error 0x5a
    uint8_t refused[sizeof response];
    memcpy(refused, response, sizeof refused);
    refused[10] = 1; refused[12] = 0x5a;
    CHECK(!ssc_qmi_parse(refused, sizeof refused, &sq) && !ssc_qmi_response(&sq, &sr) && !ssc_response_ok(&sr) && sr.error == 0x5a);
    // the indication 0x0022 (large report): TLV 0x01 u64 client id, TLV 0x02 u16 length + an SscClientResponse with one event:
    // {uid 1, response 2 {msg_id 1 fixed32 = 1025, timestamp 2 fixed64, msg 3 = {acceleration 1 floats (unpacked), accuracy 2 = 3}}}
    uint8_t event[64], body[96], ind[160];
    uint32_t e = 0, b = 0, k = 0;
    const float axes[3] = {9.81f, 0.0f, 0.25f};
    for (int i = 0; i < 3; ++i) {event[e++] = 0x0d; wr32(event + e, fbits(axes[i])); e += 4;}
    event[e++] = 0x10; event[e++] = 3;
    body[b++] = 0x0a; body[b++] = 18;
    body[b++] = 0x09; wr64(body + b, 0x0102030405060708ull); b += 8;
    body[b++] = 0x11; wr64(body + b, 0x1112131415161718ull); b += 8;
    body[b++] = 0x12; body[b++] = (uint8_t)(5 + 9 + 2 + e);
    body[b++] = 0x0d; wr32(body + b, 1025); b += 4;
    body[b++] = 0x11; wr64(body + b, 192000000ull); b += 8;
    body[b++] = 0x1a; body[b++] = (uint8_t)e; memcpy(body + b, event, e); b += e;
    ind[k++] = 4; ind[k++] = 9; ind[k++] = 0; ind[k++] = 0x22; ind[k++] = 0; ind[k++] = (uint8_t)(11 + 5 + b); ind[k++] = 0;
    ind[k++] = 0x01; ind[k++] = 8; ind[k++] = 0; wr64(ind + k, 0x1122334455667788ull); k += 8;
    ind[k++] = 0x02; ind[k++] = (uint8_t)(b + 2); ind[k++] = 0; ind[k++] = (uint8_t)b; ind[k++] = 0; memcpy(ind + k, body, b); k += b;
    int has_client = 0;
    uint64_t client = 0;
    const uint8_t *message = NULL;
    uint32_t length = 0;
    CHECK(!ssc_qmi_parse(ind, k, &sq) && sq.type == SSC_QMI_INDICATION && sq.message == SSC_QMI_JUMBO_REPORT);
    CHECK(!ssc_qmi_report(&sq, &has_client, &client, &message, &length) && has_client && client == 0x1122334455667788ull && length == b);
    SscEvents evs;
    SscEvent ev;
    SscSensorEvent se;
    CHECK(!ssc_events_open(&evs, message, length) && evs.has_suid && evs.suid.low == 0x0102030405060708ull);
    CHECK(ssc_events_next(&evs, &ev) == 1 && ev.msg_id == SSC_MSG_SENSOR_EVENT && ev.timestamp == 192000000ull);
    CHECK(!ssc_parse_sensor_event(ev.payload, ev.length, &se) && se.count == 3 && se.status == 3 && se.data[0] == fbits(9.81f));
    CHECK(ssc_events_next(&evs, &ev) == 0);
    // the message ids libssc uses: 512 SUID request, 768 SUID event, 1 attributes, 128 attribute event, 513 continuous, 514 on change,
    // 10 disable, 1025 a measurement, 769 proximity (libssc-common-private.h, libssc-client-private.h)
    CHECK(SSC_MSG_SUID_REQ == 512 && SSC_MSG_SUID_EVENT == 768 && SSC_MSG_ATTR_REQ == 1 && SSC_MSG_ATTR_EVENT == 128);
    CHECK(SSC_MSG_SENSOR_CONFIG == 513 && SSC_MSG_ON_CHANGE_CONFIG == 514 && SSC_MSG_DISABLE_REQ == 10);
    CHECK(SSC_MSG_SENSOR_EVENT == 1025 && SSC_MSG_PROXIMITY_EVENT == 769 && SSC_QMI_SERVICE == PS_QMI_SNS);
    CHECK(ssc_suid_lookup.low == 0xababababababababull && ssc_suid_lookup.high == 0xababababababababull);
    printf("PASS qmi/sns: the request, response and report frames as libqmi describes them, the protobuf as libssc's messages\n");
}

// ---------------------------------------------------------------- FastRPC bring-up against Linux fastrpc.c and qualcomm/fastrpc
#define ARENA_PA 0x9f000000ull
static uint8_t *arena;
static uint8_t bounce[SSR_BOUNCE];
static SsrCore core;
static uint64_t clock_ms;
static uint8_t sent[SSR_MESSAGE];
static int pending, step, done_steps;
static uint32_t listener = 0x77;

static uint64_t ops_now(void *c) {(void)c; return clock_ms;}
static int ops_send(void *c, const uint8_t *m, uint32_t n) {(void)c; CHECK(n == SSR_MESSAGE && !pending); memcpy(sent, m, n); pending = 1; return 0;}
static void ops_trace(void *c, const char *line) {(void)c; (void)line;}
static void ops_publish(void *c) {(void)c;}
static int ops_prepare(void *c) {(void)c; return 0;}
static int ops_pack_size(void *c, uint64_t *size) {(void)c; *size = 0; return SsrPackAbsent;}
static int ops_pack_read(void *c, uint64_t o, void *to, uint32_t n) {(void)c; (void)o; (void)to; (void)n; return 1;}
static void *ops_alloc(void *c, uint32_t n) {(void)c; return calloc(1, n);}
static unsigned admits;
static int ops_admit(void *c) {(void)c; return ++admits > 1;}   // the first call declined (the guard off), the second served

// The bytes a buffer's page list and pv point at, read as the DSP does: the first page's address (bank bits dropped) + pv's page offset.
static uint8_t *dsp_buffer(const uint8_t *meta, uint32_t n, uint32_t i, uint64_t *len)
{
    const uint8_t *list = meta + n * 16, *pages = list + n * 8;
    const uint64_t pv = rd64(meta + i * 16), page = rd64(pages + rd32(list + i * 8 + 4) * 16);
    *len = rd64(meta + i * 16 + 8);
    CHECK(rd32(list + i * 8) == (*len ? 1u : 0u) && rd32(list + i * 8 + 4) == i);
    if (!*len) return NULL;
    CHECK(page >> 32 == PS_CONTEXT_BANK && !(page & 0xfff));
    CHECK(rd64(pages + i * 16 + 8) >= ((pv & 0xfff) + *len + 0xfff) / 0x1000 * 0x1000);
    const uint64_t pa = (page & 0xffffffffull) + (pv & 0xfff);
    CHECK(pa >= ARENA_PA && pa + *len <= ARENA_PA + SSR_ARENA_BYTES);
    return arena + (pa - ARENA_PA);
}

// The DSP's call in next2's out buffers: {ctx, handle 1 (apps_std), sc, length} and the packed in buffers of fopen(name, mode)
// (method 0: in {name length, mode length}, the name, the mode; out {fd}): each in buffer a u32 length, aligned to 8 when not empty,
// its bytes; then one u32 length per out buffer (SensorsRpc.h's reading of the listener's blob).
static void deliver_fopen(const uint8_t *meta, uint32_t n, uint32_t rctx)
{
    static const char name[] = "/vendor/etc/sensors/sns_reg_config", mode[] = "r";
    uint64_t len = 0, cap = 0;
    uint8_t *prim = dsp_buffer(meta, n, 2, &len), *blob = dsp_buffer(meta, n, 3, &cap);
    CHECK(len == 16 && cap >= 256);
    uint32_t at = 0;
    const struct {const void *p; uint32_t n;} in[3] = {{NULL, 8}, {name, sizeof name}, {mode, sizeof mode}};
    for (unsigned i = 0; i < 3; ++i) {
        wr32(blob + at, in[i].n);
        at = (at + 4 + 7) & ~7u;
        if (i == 0) {wr32(blob + at, sizeof name); wr32(blob + at + 4, sizeof mode);}
        else memcpy(blob + at, in[i].p, in[i].n);
        at += in[i].n;
    }
    wr32(blob + at, 4);
    at += 4;
    wr32(prim, rctx);
    wr32(prim + 4, 1);
    wr32(prim + 8, 0x00030100u);
    wr32(prim + 12, at);
}

static void respond(uint64_t ctx, int32_t value)
{
    uint8_t r[16] = {0};
    wr64(r, ctx);
    wr32(r + 8, (uint32_t)value);
    pending = 0;
    ssr_core_receive(&core, r, sizeof r);   // Linux's struct fastrpc_invoke_rsp {u64 ctx; int retval;}, padded
}

// The simulated DSP: checks each invoke against the facts, answers it.
static void dsp(void)
{
    const uint32_t pid = rd32(sent), tid = rd32(sent + 4), handle = rd32(sent + 16), sc = rd32(sent + 20);
    const uint64_t ctx = rd64(sent + 8), addr = rd64(sent + 24), size = rd64(sent + 32);
    const uint32_t n = (sc >> 16 & 0xff) + (sc >> 8 & 0xff) + (sc >> 4 & 0xf) + (sc & 0xf);
    const uint8_t *meta = NULL;
    CHECK((ctx & 0xf) == PS_PD_SENSORS && tid == 5);     // the PD in the low bits (Linux: ctxid | fl->pd)
    if (n) {
        // the metadata: n remote_arg {u64 pv, u64 len}, n invoke_buf {u32 num, u32 pgidx}, n phy_page {u64 addr, u64 size}, 16 u64 fds,
        // 64 u32 crcs (fastrpc_get_meta_size); the message's addr is its first page with the bank in bits 32 and up, size whole pages
        CHECK(addr >> 32 == PS_CONTEXT_BANK && !(addr & 0xfff) && size >= (uint64_t)(40 * n + 128 + 256) && !(size & 0xfff));
        meta = arena + ((addr & 0xffffffffull) - ARENA_PA);
        for (unsigned i = 0; i < 16; ++i) CHECK(!rd64(meta + 40 * n + 8 * i));   // no fds
    } else {
        CHECK(!addr && !size);
    }
    uint64_t len = 0;
    uint8_t *b;
    switch (step) {
    case 0:   // attach: handle 1 (FASTRPC_INIT_HANDLE), method 0 (INIT_ATTACH), 1 in; client id 0 in the message, ours in the argument
        CHECK(pid == 0 && handle == 1 && sc == 0x00010000u && n == 1);
        b = dsp_buffer(meta, n, 0, &len);
        CHECK(len == 4 && rd32(b) == 5);
        break;
    case 1:   // remotectl_open (handle 0, method 0, 2 in 2 out): {name length, error length}, the name; {handle, error}, the error text
        CHECK(pid == 5 && handle == 0 && sc == 0x00020200u && n == 4);
        b = dsp_buffer(meta, n, 0, &len);
        CHECK(len == 8 && rd32(b) == 22 && rd32(b + 4) >= 1);
        b = dsp_buffer(meta, n, 1, &len);
        CHECK(len == 22 && !memcmp(b, "adsp_default_listener", 22));
        b = dsp_buffer(meta, n, 2, &len);
        CHECK(len == 8);
        wr32(b, listener);
        wr32(b + 4, 0);
        break;
    case 2:   // adsp_default_listener_register: the opened handle, method 0, no arguments
        CHECK(handle == listener && sc == 0 && n == 0);
        break;
    case 3:   // remotectl_close (method 1, 1 in 2 out): {handle, error length}
        CHECK(handle == 0 && sc == 0x01010200u && n == 3);
        b = dsp_buffer(meta, n, 0, &len);
        CHECK(len == 8 && rd32(b) == listener);
        break;
    case 4:   // adsp_listener_init2: handle 3, method 3, no arguments
        CHECK(handle == 3 && sc == 0x03000000u && n == 0);
        break;
    case 5:   // adsp_listener_next2: handle 3, method 4, 2 in 2 out; first {ctx 0, result -1, 0 bytes back, our capacity}
        CHECK(handle == 3 && sc == 0x04020200u && n == 4);
        b = dsp_buffer(meta, n, 0, &len);
        CHECK(len == 16 && rd32(b) == 0 && rd32(b + 4) == 0xffffffffu && rd32(b + 8) == 0 && rd32(b + 12) >= 256);
        b = dsp_buffer(meta, n, 3, &len);
        CHECK(len == rd32(dsp_buffer(meta, n, 0, &(uint64_t){0}) + 12));
        deliver_fopen(meta, n, 0x55);     // the DSP's first call: apps_std fopen of a file the (empty) pack does not have
        break;
    case 6:   // the answer to it comes with the next next2: declined by the guard (admit 0), so EUNSUPPORTED and nothing back
        CHECK(handle == 3 && sc == 0x04020200u && n == 4);
        b = dsp_buffer(meta, n, 0, &len);
        CHECK(rd32(b) == 0x55 && rd32(b + 4) == (uint32_t)SSR_EUNSUPPORTED && rd32(b + 8) == 0);
        CHECK(core.status.declined == 1 && core.status.opens == 0);
        deliver_fopen(meta, n, 0x56);     // the same call again, now admitted: served (and failing: the pack is empty)
        break;
    case 7:
        b = dsp_buffer(meta, n, 0, &len);
        CHECK(rd32(b) == 0x56 && rd32(b + 4) != 0 && rd32(b + 4) != (uint32_t)SSR_EUNSUPPORTED && rd32(b + 8) == 0);
        CHECK(core.status.declined == 1 && core.status.missing == 1 && admits == 2);
        done_steps = 1;
        pending = 0;
        ssr_core_channel(&core, 0);       // end the session: the invoke returns, the core goes back to waiting for the channel
        return;
    }
    step++;
    respond(ctx, 0);
}

static void ops_wait(void *c, uint32_t ms)
{
    (void)c;
    clock_ms += ms ? 1 : 0;
    if (pending) dsp();
}

static void fastrpc(void)
{
    static const SsrOps ops = {NULL, ops_now, ops_send, ops_wait, ops_trace, ops_publish, ops_prepare, ops_pack_size, ops_pack_read,
                               ops_alloc, ops_admit};
    arena = aligned_alloc(0x1000, SSR_ARENA_BYTES);
    CHECK(arena);
    memset(arena, 0, SSR_ARENA_BYTES);
    const SsrConfig cfg = {arena, ARENA_PA, SSR_ARENA_BYTES, bounce, 5, PS_CONTEXT_BANK, 0};
    clock_ms = 1;
    ssr_core_init(&core, &ops, &cfg);
    ssr_core_channel(&core, 1);
    for (unsigned i = 0; i < 1000 && !done_steps; ++i) CHECK(ssr_core_step(&core));
    CHECK(done_steps && step == 7);
    CHECK(!strcmp(SSR_CHANNEL, PS_GLINK_CHANNEL) && SSR_PD_SENSORS == PS_PD_SENSORS && SSR_MESSAGE == 40);
    free(arena);
    printf("PASS fastrpc: attach, remotectl open, register, close, init2 and next2 as Linux and the IDL describe them; a declined call "
           "answered at once\n");
}

// ---------------------------------------------------------------- the SNS client against a simulated hub
static uint64_t hub_clock;
static uint8_t hub_last[400];
static uint32_t hub_last_length, hub_sent, hub_results_ok, hub_results_bad;
static int hub_allow = 1, hub_refuse_port;
static ScClient sc;
static unsigned hub_changed;

static uint64_t hub_now(void *c) {(void)c; return hub_clock;}
static int hub_send(void *c, const uint8_t *d, uint32_t n)
{
    (void)c;
    if (hub_refuse_port) return -1;
    CHECK(n <= sizeof hub_last);
    memcpy(hub_last, d, n);
    hub_last_length = n;
    hub_sent++;
    return 0;
}
static int hub_may(void *c) {(void)c; return hub_allow;}
static void hub_result(void *c, int ok) {(void)c; if (ok) hub_results_ok++; else hub_results_bad++;}
static void hub_trace(void *c, const char *line) {(void)c; (void)line;}
static void hub_change(void *c) {(void)c; hub_changed++;}

typedef struct {
    uint16_t txn;
    uint32_t msg_id;
    SscSuid suid;
    char data_type[32];
} HubRequest;

// The last request as the hub reads it: QMI 0x20, TLV 0x01, the protobuf request {suid, msg_id, config, request {payload}}.
static HubRequest hub_request(void)
{
    HubRequest r;
    memset(&r, 0, sizeof r);
    SscQmi m;
    uint32_t size = 0;
    CHECK(!ssc_qmi_parse(hub_last, hub_last_length, &m) && m.type == SSC_QMI_REQUEST && m.message == SSC_QMI_REQ);
    const uint8_t *tlv = ssc_qmi_tlv(&m, 0x01, &size);
    CHECK(tlv && size >= 2 && (uint32_t)(tlv[0] | tlv[1] << 8) == size - 2);
    CHECK(!ssc_qmi_tlv(&m, 0x10, &size));   // no jumbo report asked for
    r.txn = m.transaction;
    SpbIn in, sub;
    uint32_t field, wire;
    spb_in(&in, tlv + 2, (uint32_t)(tlv[0] | tlv[1] << 8));
    while (spb_next(&in, &field, &wire) == 1) {
        const uint8_t *b = NULL;
        uint32_t bn = 0;
        uint64_t v = 0;
        uint32_t v32 = 0;
        if (field == 1) {
            CHECK(!spb_bytes_value(&in, wire, &b, &bn));
            spb_in(&sub, b, bn);
            while (spb_next(&sub, &field, &wire) == 1) {
                CHECK(!spb_fixed64_value(&sub, wire, &v));
                if (field == 1) r.suid.low = v;
                else r.suid.high = v;
            }
        } else if (field == 2) {
            CHECK(!spb_fixed32_value(&in, wire, &v32));
            r.msg_id = v32;
        } else if (field == 4) {
            CHECK(!spb_bytes_value(&in, wire, &b, &bn));
            spb_in(&sub, b, bn);
            while (spb_next(&sub, &field, &wire) == 1) {
                const uint8_t *pl = NULL;
                uint32_t pn = 0;
                if (field != 2) {CHECK(!spb_skip(&sub, wire)); continue;}
                CHECK(!spb_bytes_value(&sub, wire, &pl, &pn));
                SpbIn q;
                spb_in(&q, pl, pn);
                while (spb_next(&q, &field, &wire) == 1) {
                    if (field == 1) {
                        const uint8_t *t = NULL;
                        uint32_t tn = 0;
                        CHECK(!spb_bytes_value(&q, wire, &t, &tn) && tn < sizeof r.data_type);
                        memcpy(r.data_type, t, tn);
                    } else {
                        CHECK(!spb_skip(&q, wire));
                    }
                }
            }
        } else {
            CHECK(!spb_skip(&in, wire));
        }
    }
    return r;
}

static void hub_respond(uint16_t txn, int ok)
{
    uint8_t q[64];
    uint32_t n = 0;
    q[n++] = SSC_QMI_RESPONSE; q[n++] = (uint8_t)txn; q[n++] = (uint8_t)(txn >> 8); q[n++] = 0x20; q[n++] = 0; q[n++] = 18; q[n++] = 0;
    q[n++] = 0x02; q[n++] = 4; q[n++] = 0; q[n++] = ok ? 0 : 1; q[n++] = 0; q[n++] = ok ? 0 : 0x5a; q[n++] = 0;
    q[n++] = 0x10; q[n++] = 8; q[n++] = 0; wr64(q + n, 0x4242ull); n += 8;
    sc_receive(&sc, q, n);
}

// One report (indication 0x21) about suid with one event {msg_id, timestamp, payload}.
static void hub_report(const SscSuid *suid, uint32_t msg_id, const uint8_t *payload, uint32_t length)
{
    uint8_t body[512], ind[600];
    SpbOut o;
    spb_out(&o, body, sizeof body);
    uint32_t mark = spb_open(&o, 1);
    spb_put_fixed64(&o, 1, suid->low);
    spb_put_fixed64(&o, 2, suid->high);
    spb_close(&o, mark);
    mark = spb_open(&o, 2);
    spb_put_fixed32(&o, 1, msg_id);
    spb_put_fixed64(&o, 2, 19200000ull);
    spb_put_bytes(&o, 3, payload, length);
    spb_close(&o, mark);
    const uint32_t b = spb_done(&o);
    CHECK(b);
    uint32_t n = 0;
    ind[n++] = SSC_QMI_INDICATION; ind[n++] = 7; ind[n++] = 0; ind[n++] = 0x21; ind[n++] = 0;
    const uint32_t tlvs = 11 + 5 + b;
    ind[n++] = (uint8_t)tlvs; ind[n++] = (uint8_t)(tlvs >> 8);
    ind[n++] = 0x01; ind[n++] = 8; ind[n++] = 0; wr64(ind + n, 0x4242ull); n += 8;
    ind[n++] = 0x02; ind[n++] = (uint8_t)(b + 2); ind[n++] = (uint8_t)((b + 2) >> 8); ind[n++] = (uint8_t)b; ind[n++] = (uint8_t)(b >> 8);
    memcpy(ind + n, body, b);
    n += b;
    sc_receive(&sc, ind, n);
}

static void hub_suid_event(const char *type, unsigned count, uint64_t base)
{
    uint8_t p[128];
    SpbOut o;
    spb_out(&o, p, sizeof p);
    spb_put_string(&o, 1, type);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t mark = spb_open(&o, 2);
        spb_put_fixed64(&o, 1, base + i);
        spb_put_fixed64(&o, 2, base + 0x100 + i);
        spb_close(&o, mark);
    }
    hub_report(&ssc_suid_lookup, SSC_MSG_SUID_EVENT, p, spb_done(&o));
}

// An attribute event: name, vendor, available, stream type, four rates.
static void hub_attr_event(const SscSuid *suid, const char *name)
{
    uint8_t p[300];
    SpbOut o;
    spb_out(&o, p, sizeof p);
    const struct {int id; int kind; const char *s; uint64_t i;} attrs[] = {
        {SSC_ATTR_NAME, 2, name, 0}, {SSC_ATTR_VENDOR, 2, "vendor", 0}, {SSC_ATTR_AVAILABLE, 5, NULL, 1}, {SSC_ATTR_STREAM_TYPE, 4, NULL, 0}};
    for (unsigned k = 0; k < 4; ++k) {
        const uint32_t a = spb_open(&o, 1);
        spb_put_varint(&o, 1, (uint64_t)attrs[k].id);
        const uint32_t v = spb_open(&o, 2), one = spb_open(&o, 1);
        if (attrs[k].kind == 2) spb_put_string(&o, 2, attrs[k].s);
        else if (attrs[k].kind == 5) spb_put_varint(&o, 5, attrs[k].i);
        else spb_put_fixed64(&o, 4, attrs[k].i);
        spb_close(&o, one);
        spb_close(&o, v);
        spb_close(&o, a);
    }
    const uint32_t a = spb_open(&o, 1);
    spb_put_varint(&o, 1, SSC_ATTR_RATES);
    const uint32_t v = spb_open(&o, 2);
    const float rates[] = {13.0f, 26.0f, 52.0f, 104.0f};
    for (unsigned k = 0; k < 4; ++k) {
        const uint32_t one = spb_open(&o, 1);
        spb_put_fixed32(&o, 3, fbits(rates[k]));
        spb_close(&o, one);
    }
    spb_close(&o, v);
    spb_close(&o, a);
    hub_report(suid, SSC_MSG_ATTR_EVENT, p, spb_done(&o));
}

// Runs the client, the hub answering every request (found: the SUID count per data type).
static void hub_serve(const unsigned found[SC_TYPES], unsigned rounds)
{
    for (unsigned r = 0; r < rounds; ++r) {
        const uint32_t before = hub_sent;
        sc_tick(&sc);
        if (hub_sent == before) {hub_clock += 250; continue;}
        const HubRequest q = hub_request();
        hub_respond(q.txn, 1);
        if (q.msg_id == SSC_MSG_SUID_REQ) {
            CHECK(ssc_suid_equal(&q.suid, &ssc_suid_lookup));
            int t = -1;
            for (unsigned i = 0; i < SC_TYPES; ++i) if (!strcmp(q.data_type, sc_type_name[i])) t = (int)i;
            CHECK(t >= 0);
            hub_suid_event(q.data_type, found[t], 0x1000ull * (unsigned)(t + 1));
        } else {
            CHECK(q.msg_id == SSC_MSG_ATTR_REQ);
            hub_attr_event(&q.suid, "lsm6dso");
        }
        hub_clock += 10;
    }
}

static void client(void)
{
    static const ScOps ops = {NULL, hub_now, hub_send, hub_may, hub_result, hub_trace, hub_change};
    static const unsigned found[SC_TYPES] = {1, 1, 1, 0, 1, 0};   // registry, accel, gyro, no mag, light, no proximity
    hub_clock = 1000;
    sc_init(&sc, &ops);
    sc_tick(&sc);
    CHECK(hub_sent == 0);                                    // nothing before the service is announced
    sc_service(&sc, 1, 1);
    hub_serve(found, 60);
    CHECK(sc_settled(&sc) && hub_sent == SC_TYPES + 4 && hub_results_ok == SC_TYPES + 4 && hub_results_bad == 0);
    CHECK(sc.sensor[SC_ACCEL].found == 1 && sc.sensor[SC_ACCEL].suid.low == 0x2000 && sc.sensor[SC_ACCEL].attr == SC_ATTR_READ);
    CHECK(!strcmp(sc.sensor[SC_ACCEL].attrs.name, "lsm6dso") && !strcmp(sc.sensor[SC_ACCEL].attrs.vendor, "vendor"));
    CHECK(sc.sensor[SC_ACCEL].attrs.available == 1 && sc.sensor[SC_ACCEL].attrs.rate_count == 4 && sc.sensor[SC_ACCEL].attrs.stream_type == 0);
    CHECK(sc.sensor[SC_MAG].found == 0 && sc.sensor[SC_MAG].attr == SC_ATTR_NONE && sc.sensor[SC_PROX].lookup == SC_LOOKUP_ANSWERED);
    CHECK(sc.has_client && sc.client_id == 0x4242 && hub_changed >= SC_TYPES + 4);
    // a late sensor (register_updates): proximity appears; its attributes are asked for
    hub_suid_event("proximity", 1, 0x9000);
    CHECK(!sc_settled(&sc) && sc.sensor[SC_PROX].found == 1);
    hub_serve(found, 20);
    CHECK(sc_settled(&sc) && sc.sensor[SC_PROX].attr == SC_ATTR_READ);
    // a new generation: everything over; while the guard says no, nothing goes out
    sc_service(&sc, 1, 2);
    CHECK(sc.sensor[SC_ACCEL].lookup == SC_LOOKUP_TODO && !sc.sensor[SC_ACCEL].found && !sc_settled(&sc));
    hub_allow = 0;
    const uint32_t sent0 = hub_sent;
    for (unsigned i = 0; i < 40; ++i) {sc_tick(&sc); hub_clock += 250;}
    CHECK(hub_sent == sent0);
    hub_allow = 1;
    // the port refuses: a failure for the guard, the lookup asked again later
    hub_refuse_port = 1;
    sc_tick(&sc);
    CHECK(hub_results_bad == 1 && sc.sensor[SC_REGISTRY].lookup == SC_LOOKUP_TODO && !sc.pending);
    hub_refuse_port = 0;
    // the hub refuses: a failure, asked again
    sc_tick(&sc);
    HubRequest q = hub_request();
    hub_respond(q.txn, 0);
    CHECK(hub_results_bad == 2 && sc.sensor[SC_REGISTRY].lookup == SC_LOOKUP_TODO && sc.refused == 2);   // one at the port, one by the hub
    // no response at all: after 2 s a failure
    sc_tick(&sc);
    hub_clock += SC_RESPONSE_MS;
    sc_tick(&sc);
    CHECK(hub_results_bad == 3 && sc.timeouts == 1);
    // answered, but the SUID event never comes: asked again after 5 s; three failures of any kind and the lookup is given up
    // (counted as none found; two came above)
    unsigned asked = 0;
    while (!sc.sensor[SC_REGISTRY].lookup_gave_up) {
        sc_tick(&sc);
        q = hub_request();
        CHECK(!strcmp(q.data_type, "registry") && ++asked <= SC_ATTR_TRIES);
        hub_respond(q.txn, 1);
        hub_clock += SC_ATTR_MS;
        sc_tick(&sc);   // the event did not come: asked again, or given up (then the next type goes out)
    }
    CHECK(asked == 1 && sc.sensor[SC_REGISTRY].lookup_tries == SC_ATTR_TRIES);
    CHECK(sc.sensor[SC_REGISTRY].lookup == SC_LOOKUP_ANSWERED && !sc.sensor[SC_REGISTRY].found && !sc_settled(&sc));
    // accel found; an error event about its attribute request counts as a failure; then the event never comes: given up
    sc_tick(&sc);
    q = hub_request();
    CHECK(!strcmp(q.data_type, "accel"));
    hub_respond(q.txn, 1);
    hub_suid_event("accel", 1, 0x2000);
    for (unsigned lookups = 0;; ++lookups) {   // the remaining lookups go first (none found), then the attributes
        hub_clock += 10;
        sc_tick(&sc);
        q = hub_request();
        if (q.msg_id == SSC_MSG_ATTR_REQ) break;
        CHECK(lookups < SC_TYPES);
        hub_respond(q.txn, 1);
        hub_suid_event(q.data_type, 0, 0);
    }
    CHECK(q.suid.low == 0x2000);
    hub_respond(q.txn, 1);
    const uint32_t bad = hub_results_bad;
    uint8_t err[8];
    SpbOut o;
    spb_out(&o, err, sizeof err);
    spb_put_varint(&o, 1, 3);
    hub_report(&q.suid, SSC_MSG_ERROR_EVENT, err, spb_done(&o));
    CHECK(hub_results_bad == bad + 1 && sc.errors == 1 && sc.sensor[SC_ACCEL].attr == SC_ATTR_TODO);
    for (unsigned guard_rounds = 0; sc.sensor[SC_ACCEL].attr != SC_ATTR_GAVE_UP && guard_rounds < 400; ++guard_rounds) {
        const uint32_t before = hub_sent;
        sc_tick(&sc);
        if (hub_sent == before) {hub_clock += 250; continue;}
        q = hub_request();
        hub_respond(q.txn, 1);
        if (q.msg_id == SSC_MSG_SUID_REQ) hub_suid_event(q.data_type, 0, 0);   // the other types: none found
    }
    CHECK(sc.sensor[SC_ACCEL].attr == SC_ATTR_GAVE_UP && sc.sensor[SC_ACCEL].attr_tries == SC_ATTR_TRIES);
    // malformed and stray input is counted, never trusted
    static const uint8_t junk[] = {4, 1, 0, 0x21, 0, 3, 0, 0x02, 9, 0};
    sc_receive(&sc, junk, sizeof junk);
    sc_receive(&sc, junk, 3);
    hub_respond(0x7777, 1);
    CHECK(sc.malformed >= 2 && sc.unexpected >= 1);
    sc_service(&sc, 0, 2);
    CHECK(!sc.up);
    const uint32_t sent1 = hub_sent;
    sc_tick(&sc);
    CHECK(hub_sent == sent1);
    printf("PASS sns client: lookups and attributes against a simulated hub, late sensors, generations, the guard, refusals, timeouts, "
           "error events, junk\n");
}

// ---------------------------------------------------------------- health -> guard, the arena
static void health_arena(void)
{
    PianoAdspHealth h;
    memset(&h, 0, sizeof h);
    h.version = PIANO_ADSP_HEALTH_VERSION;
    h.dsp_up = 1;
    h.charger_pd = 0x1fffffff;
    h.charger_generation = 2;
    h.link_generation = 5;
    SgDsp d = sensors_dsp_of(&h);
    CHECK(d.dsp_up && !d.fatal && !d.quiet && d.charger_state == SG_CHARGER_UP && d.charger_ups == 2 && d.link_generation == 5 && !d.crash);
    h.fatal_reports = 1;   // a fatal report in the past is enough
    CHECK(sensors_dsp_of(&h).fatal);
    h.fatal_reports = 0;
    h.quiet = PIANO_ADSP_QUIET_CHARGER;
    CHECK(sensors_dsp_of(&h).quiet);
    h.quiet = 0;
    strcpy(h.crash, "err_qdi.c:EF:qsh_process");
    d = sensors_dsp_of(&h);
    CHECK(d.crash && sg_crash_is_sensors(d.crash));
    h.crash[0] = 0;
    h.version = PIANO_ADSP_HEALTH_VERSION + 1;   // another contract: a fatal sign, never up
    d = sensors_dsp_of(&h);
    CHECK(d.fatal && !d.dsp_up);
    // ... and through the guard: quiet holds, a past fatal report ends it
    SgGuard g;
    sg_init(&g, NULL);
    h.version = PIANO_ADSP_HEALTH_VERSION;
    h.quiet = 1;
    d = sensors_dsp_of(&h);
    CHECK(sg_observe(&g, &d, 0) == SG_WAIT && sg_observe(&g, &d, 60000) == SG_WAIT);
    h.quiet = 0;
    d = sensors_dsp_of(&h);
    sg_observe(&g, &d, 60000);
    CHECK(sg_observe(&g, &d, 80000) == SG_RUN);
    h.fatal_reports = 1;
    d = sensors_dsp_of(&h);
    CHECK(sg_observe(&g, &d, 80001) == SG_OFF && g.reason == SG_REASON_FATAL);
    const char *why = NULL;
    CHECK(ps_arena_check(0xa0000000ull, SSR_ARENA_BYTES, &why) == 0 && !why);
    CHECK(ps_arena_check(0xa0000800ull, SSR_ARENA_BYTES, &why) && !strcmp(why, "not whole 4 KiB pages"));
    CHECK(ps_arena_check(0x40000000ull, SSR_ARENA_BYTES, &why) && !strcmp(why, "below DRAM"));
    CHECK(ps_arena_check(0xfff00000ull, SSR_ARENA_BYTES, &why) && strstr(why, "4 GiB"));
    CHECK(ps_arena_check(0x8c0000000ull, SSR_ARENA_BYTES, &why) && strstr(why, "4 GiB"));
    CHECK(ps_arena_check(0x80800000ull, SSR_ARENA_BYTES, &why) && !strcmp(why, "gunyah-hyp"));
    CHECK(ps_arena_check(0xb8000000ull, SSR_ARENA_BYTES, &why) && strstr(why, "adspslpi"));
    CHECK(ps_arena_check(0xfc000000ull, SSR_ARENA_BYTES, &why) && strstr(why, "vm"));
    CHECK(ps_arena_check(0xfd000000ull, SSR_ARENA_BYTES, &why) && strstr(why, "splash"));
    CHECK(ps_arena_check(0x9ed00000ull, SSR_ARENA_BYTES, &why) && strstr(why, "q6-adsp-dtb"));
    CHECK(ps_arena_check(0xa7000000ull, SSR_ARENA_BYTES, &why) && strstr(why, "firmware volume"));
    for (unsigned i = 1; i < ps_reserved_count; ++i) CHECK(ps_reserved[i - 1].base + ps_reserved[i - 1].bytes <= ps_reserved[i].base);
    printf("PASS health and arena: PianoPower's report as the guard reads it, the arena's refusals\n");
}

int main(void)
{
    guard();
    policy();
    qmi_sns();
    fastrpc();
    client();
    health_arena();
    printf("PASS PianoSensors piano tests: %u checks\n", checks);
    return 0;
}
