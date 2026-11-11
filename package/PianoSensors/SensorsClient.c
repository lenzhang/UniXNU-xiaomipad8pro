/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* Level 3's SNS client: see SensorsClient.h. */
#include "SensorsClient.h"
#include "SensorsText.h"

const char *const sc_type_name[SC_TYPES] = {"registry", "accel", "gyro", "mag", "ambient_light", "proximity"};

static uint64_t now(ScClient *c) {return c->ops->now_ms(c->ops->context);}

static void trace(ScClient *c, const char *what, int type, const char *key, int64_t value)
{
    SntText t;
    snt_start(&t, c->line, sizeof c->line);
    snt_str(&t, what);
    if (type >= 0 && type < SC_TYPES) snt_ks(&t, "type", sc_type_name[type]);
    if (key) snt_kd(&t, key, value);
    if (c->ops->trace) c->ops->trace(c->ops->context, c->line);
}

static void changed(ScClient *c)
{
    if (c->ops->changed) c->ops->changed(c->ops->context);
}

static void reset(ScClient *c)
{
    c->pending = 0;
    c->pending_type = -1;
    c->has_client = 0;
    c->discovery_done = 0;
    for (unsigned i = 0; i < SC_TYPES; ++i) {
        ScSensor *s = &c->sensor[i];
        s->lookup = SC_LOOKUP_TODO;
        s->lookup_tries = 0;
        s->lookup_deadline = 0;
        s->lookup_gave_up = 0;
        s->found = s->more = 0;
        s->suid.low = s->suid.high = 0;
        s->attr = SC_ATTR_NONE;
        s->attr_tries = 0;
        s->attr_deadline = 0;
        ssc_attrs_clear(&s->attrs);
    }
}

void sc_init(ScClient *c, const ScOps *ops)
{
    uint8_t *p = (uint8_t *)c;
    for (uint32_t i = 0; i < sizeof *c; ++i) p[i] = 0;
    c->ops = ops;
    c->next_txn = 1;
    reset(c);
}

void sc_service(ScClient *c, int up, uint32_t generation)
{
    if (up && c->up && generation == c->generation) return;
    const int was = c->up;
    c->up = up ? 1 : 0;
    c->generation = generation;
    reset(c);
    trace(c, up ? (was ? "sns: the service moved: starting over" : "sns: the service is there") : "sns: the service went away", -1,
          "generation", generation);
    changed(c);
}

int sc_settled(const ScClient *c)
{
    for (unsigned i = 0; i < SC_TYPES; ++i) {
        const ScSensor *s = &c->sensor[i];
        if (s->lookup != SC_LOOKUP_ANSWERED) return 0;
        if (s->found && s->attr != SC_ATTR_READ && s->attr != SC_ATTR_GAVE_UP) return 0;
    }
    return 1;
}

static int send_request(ScClient *c, int type, int lookup)
{
    uint8_t payload[64], message[160], qmi[200];
    uint32_t n = 0, m;
    if (lookup) {
        n = ssc_build_suid_req(payload, sizeof payload, sc_type_name[type], 1, 1);
        m = ssc_build_request(message, sizeof message, &ssc_suid_lookup, SSC_MSG_SUID_REQ, SSC_DELIVERY_WAKEUP, payload, n, -1);
    } else {
        m = ssc_build_request(message, sizeof message, &c->sensor[type].suid, SSC_MSG_ATTR_REQ, SSC_DELIVERY_WAKEUP, 0, 0, -1);
    }
    if (!m || (lookup && !n)) return -1;
    const uint16_t txn = c->next_txn;
    c->next_txn = (uint16_t)(c->next_txn == 0xffff ? 1 : c->next_txn + 1);
    const uint32_t q = ssc_build_qmi_request(qmi, sizeof qmi, txn, message, m, -1);
    if (!q) return -1;
    if (c->ops->send(c->ops->context, qmi, q)) {
        c->refused++;
        return -2;
    }
    c->requests++;
    c->pending = 1;
    c->pending_txn = txn;
    c->pending_type = type;
    c->pending_lookup = lookup;
    c->sent_ms = now(c);
    return 0;
}

/* A request that failed (refused at the port, refused by the hub, unanswered): its sensor goes back to be asked again. */
static void request_failed(ScClient *c, int type, int lookup)
{
    if (type < 0 || type >= SC_TYPES) return;
    ScSensor *s = &c->sensor[type];
    if (lookup) {
        if (s->lookup != SC_LOOKUP_SENT) return;
        if (++s->lookup_tries < SC_ATTR_TRIES) {s->lookup = SC_LOOKUP_TODO; return;}
        s->lookup = SC_LOOKUP_ANSWERED;
        s->lookup_gave_up = 1;
        trace(c, "sns: lookup given up (no SUID event)", type, 0, 0);
        changed(c);
    } else if (s->attr == SC_ATTR_SENT) {
        s->attr = ++s->attr_tries < SC_ATTR_TRIES ? SC_ATTR_TODO : SC_ATTR_GAVE_UP;
        if (s->attr == SC_ATTR_GAVE_UP) {trace(c, "sns: attributes given up", type, 0, 0); changed(c);}
    }
}

void sc_tick(ScClient *c)
{
    if (!c->up) return;
    const uint64_t t = now(c);
    if (c->pending && t - c->sent_ms >= SC_RESPONSE_MS) {
        c->pending = 0;
        c->timeouts++;
        trace(c, "sns: no response", c->pending_type, "ms", (int64_t)(t - c->sent_ms));
        request_failed(c, c->pending_type, c->pending_lookup);
        if (c->ops->result) c->ops->result(c->ops->context, 0);
    }
    for (unsigned i = 0; i < SC_TYPES; ++i) {   /* SUID and attribute events that never came */
        ScSensor *s = &c->sensor[i];
        const int waiting = c->pending && c->pending_type == (int)i;
        if (s->lookup == SC_LOOKUP_SENT && t >= s->lookup_deadline && !(waiting && c->pending_lookup)) {
            trace(c, "sns: no SUID event", (int)i, "try", s->lookup_tries + 1);
            request_failed(c, (int)i, 1);
        }
        if (s->attr == SC_ATTR_SENT && t >= s->attr_deadline && !(c->pending && c->pending_type == (int)i && !c->pending_lookup)) {
            trace(c, "sns: no attribute event", (int)i, "try", s->attr_tries + 1);
            request_failed(c, (int)i, 0);
        }
    }
    if (c->pending) return;
    int type = -1, lookup = 0;
    for (unsigned i = 0; i < SC_TYPES && type < 0; ++i)
        if (c->sensor[i].lookup == SC_LOOKUP_TODO) {type = (int)i; lookup = 1;}
    for (unsigned i = 0; i < SC_TYPES && type < 0; ++i)
        if (c->sensor[i].found && c->sensor[i].attr == SC_ATTR_TODO) type = (int)i;
    if (type < 0) return;
    if (c->ops->may_request && !c->ops->may_request(c->ops->context)) return;
    const int rc = send_request(c, type, lookup);
    if (rc) {
        trace(c, rc == -2 ? "sns: the port refused the request" : "sns: the request did not fit", type, 0, 0);
        if (c->ops->result) c->ops->result(c->ops->context, 0);
        return;
    }
    if (lookup) {
        c->sensor[type].lookup = SC_LOOKUP_SENT;
        c->sensor[type].lookup_deadline = c->sent_ms + SC_ATTR_MS;
    } else {
        c->sensor[type].attr = SC_ATTR_SENT;
        c->sensor[type].attr_deadline = c->sent_ms + SC_ATTR_MS;
    }
}

static int type_of(const char *data_type)
{
    for (unsigned i = 0; i < SC_TYPES; ++i) {
        const char *a = sc_type_name[i], *b = data_type;
        while (*a && *a == *b) {++a; ++b;}
        if (!*a && !*b) return (int)i;
    }
    return -1;
}

static void suid_event(ScClient *c, const SscEvent *ev)
{
    SscSuidEvent se;
    if (ssc_parse_suid_event(ev->payload, ev->length, &se)) {c->malformed++; return;}
    const int type = type_of(se.data_type);
    if (type < 0) {c->unexpected++; return;}
    ScSensor *s = &c->sensor[type];
    const int fresh = s->lookup != SC_LOOKUP_ANSWERED || s->found != se.count;
    s->lookup = SC_LOOKUP_ANSWERED;
    s->lookup_gave_up = 0;
    s->found = se.count;
    s->more = se.more;
    if (se.count) {
        if (!ssc_suid_equal(&s->suid, &se.suid[0])) {   /* a new default sensor: its attributes are asked for (again) */
            s->suid = se.suid[0];
            s->attr = SC_ATTR_TODO;
            s->attr_tries = 0;
            ssc_attrs_clear(&s->attrs);
        }
    } else {
        s->suid.low = s->suid.high = 0;
        s->attr = SC_ATTR_NONE;
    }
    if (fresh) {
        trace(c, "sns: lookup answered", type, "found", se.count);
        changed(c);
    }
}

static void sensor_event(ScClient *c, const SscSuid *suid, const SscEvent *ev)
{
    int type = -1;
    for (unsigned i = 0; i < SC_TYPES && type < 0; ++i)
        if (c->sensor[i].found && ssc_suid_equal(&c->sensor[i].suid, suid)) type = (int)i;
    if (type < 0) {c->unexpected++; return;}
    ScSensor *s = &c->sensor[type];
    if (ev->msg_id == SSC_MSG_ATTR_EVENT) {
        ssc_attrs_clear(&s->attrs);
        if (ssc_parse_attr_event(ev->payload, ev->length, &s->attrs)) {
            c->malformed++;
            request_failed(c, type, 0);
            return;
        }
        s->attr = SC_ATTR_READ;
        trace(c, "sns: attributes read", type, "count", s->attrs.seen);
        changed(c);
    } else if (ev->msg_id == SSC_MSG_ERROR_EVENT) {
        int64_t error = 0;
        ssc_parse_error_event(ev->payload, ev->length, &error);
        c->errors++;
        trace(c, "sns: error event", type, "error", error);
        request_failed(c, type, 0);
        if (c->ops->result) c->ops->result(c->ops->context, 0);
    } else {
        c->unexpected++;
    }
}

void sc_receive(ScClient *c, const uint8_t *data, uint32_t length)
{
    SscQmi m;
    if (ssc_qmi_parse(data, length, &m)) {c->malformed++; return;}
    if (m.type == SSC_QMI_RESPONSE && m.message == SSC_QMI_REQ) {
        SscResponse r;
        if (!c->pending || m.transaction != c->pending_txn) {c->unexpected++; return;}
        c->pending = 0;
        const int bad = ssc_qmi_response(&m, &r);
        if (bad || !ssc_response_ok(&r)) {
            c->refused++;
            trace(c, "sns: the hub refused the request", c->pending_type, "error", bad ? -1 : r.error);
            request_failed(c, c->pending_type, c->pending_lookup);
            if (c->ops->result) c->ops->result(c->ops->context, 0);
            return;
        }
        c->responses++;
        if (r.has_client) {c->has_client = 1; c->client_id = r.client_id;}
        if (c->ops->result) c->ops->result(c->ops->context, 1);
        return;
    }
    if (m.type != SSC_QMI_INDICATION || (m.message != SSC_QMI_REPORT && m.message != SSC_QMI_JUMBO_REPORT)) {c->unexpected++; return;}
    int has_client = 0;
    uint64_t client = 0;
    const uint8_t *message = 0;
    uint32_t n = 0;
    if (ssc_qmi_report(&m, &has_client, &client, &message, &n)) {c->malformed++; return;}
    c->reports++;
    SscEvents events;
    SscEvent ev;
    if (ssc_events_open(&events, message, n) || !events.has_suid) {c->malformed++; return;}
    int rc;
    while ((rc = ssc_events_next(&events, &ev)) == 1) {
        c->events++;
        if (ssc_suid_equal(&events.suid, &ssc_suid_lookup)) {
            if (ev.msg_id == SSC_MSG_SUID_EVENT) suid_event(c, &ev);
            else if (ev.msg_id == SSC_MSG_SUID_DONE_EVENT) {
                if (!c->discovery_done) trace(c, "sns: discovery done", -1, 0, 0);
                c->discovery_done = 1;
            } else c->unexpected++;
        } else {
            sensor_event(c, &events.suid, &ev);
        }
    }
    if (rc < 0) c->malformed++;
}
