/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* Level 4's streams: see SensorsStream.h. */
#include "SensorsStream.h"
#include "SensorsText.h"

const char *ss_stream_name(int stream)
{
    return stream == SS_LIGHT ? "light" : stream == SS_ACCEL ? "accel" : "?";
}

static uint64_t now(const SsEngine *e) {return e->ops->now_ms(e->ops->context);}
static PasStream *stream_of(SsEngine *e, int i) {return i == SS_LIGHT ? &e->light.stream : &e->accel;}
static const PasStream *cstream_of(const SsEngine *e, int i) {return i == SS_LIGHT ? &e->light.stream : &e->accel;}

void ss_defaults(SsConfig *c)
{
    SfxAxes gyro, mag;
    c->accel_hz = SS_ACCEL_HZ;
    c->accel_batch_ms = 0;
    sfx_axes_identity(&c->axes);
    sfx_axes_identity(&gyro);
    sfx_axes_identity(&mag);
    sfx_axes_parse(PS_ACCEL_AXES, &c->axes, &gyro, &mag);
    pas_defaults(&c->stream);
}

/* ---- the log: a budget per stream (SS_LOG_BURST lines, one more every SS_LOG_REFILL_MS), the lines left out counted ---- */
static int log_allow(SsLog *l, uint64_t t)
{
    const uint64_t full = (uint64_t)SS_LOG_BURST * SS_LOG_REFILL_MS;
    if (t > l->at) {
        const uint64_t sum = l->budget_ms + (t - l->at);
        l->budget_ms = (uint32_t)(sum < full ? sum : full);
    }
    l->at = t;
    if (l->budget_ms < SS_LOG_REFILL_MS) {
        l->left_out++;
        return 0;
    }
    l->budget_ms -= SS_LOG_REFILL_MS;
    return 1;
}

static SntText *line(SsEngine *e, int i, SntText *t)
{
    snt_start(t, e->ch[i].line, sizeof e->ch[i].line);
    snt_str(t, ss_stream_name(i));
    snt_str(t, ": ");
    return t;
}

static void emit(SsEngine *e, int i, SntText *t, uint64_t at, int always)
{
    SsLog *l = &e->ch[i].log;
    if (!always && !log_allow(l, at)) return;
    if (l->left_out) {
        snt_str(t, " (");
        snt_udec(t, l->left_out);
        snt_str(t, l->left_out == 1 ? " line left out)" : " lines left out)");
        l->left_out = 0;
    }
    if (e->ops->trace) e->ops->trace(e->ops->context, e->ch[i].line);
}

/* ---- the outputs ---- */
static void publish(SsEngine *e, unsigned changed)
{
    if (!changed) return;
    e->out.seq++;
    e->out.changed = changed;
    if (e->ops->publish) e->ops->publish(e->ops->context, &e->out);
}

static void set_edge(SsEngine *e, int edge, uint64_t at)
{
    if (edge == e->out.edge) return;
    e->out.edge = edge;
    publish(e, SS_OUT_EDGE);
    SntText t;
    snt_str(line(e, SS_ACCEL, &t), "edge ");
    snt_str(&t, ps_edge_name(edge));
    emit(e, SS_ACCEL, &t, at, 0);
}

static void light_target(SsEngine *e, int32_t target, uint64_t at)
{
    unsigned changed = 0;
    if (target >= 0 && target != e->out.target) {
        e->out.target = target;
        changed |= SS_OUT_TARGET;
    }
    if (target >= 0) e->target_given = 1;
    if (e->light.lux != e->out.lux && (changed || at - e->lux_ms >= SS_LUX_MS || !e->lux_ms)) {
        e->out.lux = e->light.lux;
        e->lux_ms = at ? at : 1;
        changed |= SS_OUT_LUX;
    }
    e->lux_pending = e->light.lux != e->out.lux;
    publish(e, changed);
}

static void leave(SsEngine *e)
{
    if (e->left || !e->target_given) return;
    e->left = 1;
    if (e->ops->leave) e->ops->leave(e->ops->context);
}

/* ---- the conditions -> what each stream wants ---- */
static void apply(SsEngine *e, uint64_t at)
{
    const SsConditions *c = &e->cond;
    const char *held = e->off ? "the guard is off" : e->asleep ? "system sleep" : 0;
    const int light = !held && c->light_consumer;
    pal_conditions(&e->light, light, c->display != 0, c->lid, at);
    e->ch[SS_LIGHT].why = held ? held : !c->light_consumer ? "no consumer" : c->display == 0 ? "display off"
                        : c->lid == 0 ? "lid closed" : 0;
    const int accel = !held && c->accel_consumer && c->display != 0 && c->attached != 1;
    pas_want(&e->accel, accel, at);
    e->ch[SS_ACCEL].why = held ? held : !c->accel_consumer ? "no consumer" : c->display == 0 ? "display off"
                        : c->attached == 1 ? "cover attached" : 0;
    if (!accel) set_edge(e, PS_EDGE_NONE, at);
}

static void restart(SsEngine *e, int i, uint64_t at)
{
    SsChannel *ch = &e->ch[i];
    PasStream *s = stream_of(e, i);
    if (s->sessions || s->outstanding != PAS_ACT_NONE) ch->restarts++;
    pas_init(s, &e->cfg.stream);
    ch->timeouts_seen = 0;
    ch->rearmed_seen = 0;
    ch->last_state = PAS_OFF;
    ch->hub_rate_milli = 0;
    ch->sample_session = 0;
    if (i == SS_ACCEL) {
        sno_init(&e->orient);
        set_edge(e, PS_EDGE_NONE, at);
    }
    apply(e, at);
}

void ss_init(SsEngine *e, const SsOps *ops, const SsConfig *config)
{
    uint8_t *p = (uint8_t *)e;
    for (uint32_t i = 0; i < sizeof *e; ++i) p[i] = 0;
    e->ops = ops;
    if (config) e->cfg = *config;
    else ss_defaults(&e->cfg);
    if (e->cfg.accel_hz < 1) e->cfg.accel_hz = 1;
    if (e->cfg.accel_hz > 100) e->cfg.accel_hz = 100;
    if (e->cfg.accel_batch_ms > 1000) e->cfg.accel_batch_ms = 1000;
    pal_init(&e->light, &e->cfg.stream);
    pas_init(&e->accel, &e->cfg.stream);
    sno_init(&e->orient);
    e->cond.display = e->cond.lid = e->cond.attached = -1;
    e->out.target = -1;
    e->out.edge = PS_EDGE_NONE;
    const uint64_t t = now(e);
    for (int i = 0; i < SS_STREAMS; ++i) {
        SsChannel *ch = &e->ch[i];
        ch->next_txn = 1;
        ch->last_state = PAS_OFF;
        ch->on_change = i == SS_LIGHT;
        ch->log.budget_ms = SS_LOG_BURST * SS_LOG_REFILL_MS;
        ch->log.at = t;
    }
    apply(e, t);
}

void ss_port_opened(SsEngine *e, int i)
{
    if (i < 0 || i >= SS_STREAMS || e->off) return;
    SsChannel *ch = &e->ch[i];
    ch->open = 1;
    ch->up = 0;
    ch->generation = 0;
    ch->ever_sent = 0;
    ch->has_client = 0;
    restart(e, i, now(e));
}

void ss_service(SsEngine *e, int i, int up, uint32_t generation)
{
    if (i < 0 || i >= SS_STREAMS) return;
    SsChannel *ch = &e->ch[i];
    up = up ? 1 : 0;
    if (!ch->open || e->off || (up == ch->up && generation == ch->generation)) return;
    const int was = ch->up;
    ch->up = up;
    ch->generation = generation;
    ch->has_client = 0;
    const uint64_t t = now(e);
    restart(e, i, t);
    SntText s;
    snt_str(line(e, i, &s), up ? (was ? "the service moved: starting over" : "the service is there") : "the service went away");
    snt_kd(&s, "generation", generation);
    emit(e, i, &s, t, 0);
}

static void close_port(SsEngine *e, int i)
{
    SsChannel *ch = &e->ch[i];
    if (!ch->open) return;
    ch->open = 0;
    ch->up = 0;
    ch->closes++;
    if (e->ops->close_port) e->ops->close_port(e->ops->context, i);
}

void ss_sensor(SsEngine *e, int i, int found, const SscSuid *suid, int64_t stream_type, uint32_t lowest_rate_bits)
{
    if (i < 0 || i >= SS_STREAMS || e->off) return;
    SsChannel *ch = &e->ch[i];
    found = found && suid;
    /* the light on change unless its attributes say it streams (then at its lowest rate: the hub's least work); the accelerometer at
     * the rotation's rate */
    const int on_change = i == SS_LIGHT && (stream_type != SSC_STREAM_STREAMING || !sfx_positive(lowest_rate_bits));
    const uint32_t rate = i == SS_ACCEL ? sfx_float_from_ratio(e->cfg.accel_hz, 1) : on_change ? 0 : lowest_rate_bits;
    if (found == ch->found && (!found || ssc_suid_equal(suid, &ch->suid)) && on_change == ch->on_change && rate == ch->rate_bits) return;
    const PasStream *s = stream_of(e, i);
    const int active = ch->found && (s->state != PAS_OFF || s->outstanding != PAS_ACT_NONE || s->dirty);
    ch->found = found;
    if (found) ch->suid = *suid;
    ch->on_change = on_change;
    ch->rate_bits = rate;
    const uint64_t t = now(e);
    SntText txt;
    line(e, i, &txt);
    if (!found) {
        snt_str(&txt, "no sensor in the list");
    } else {
        snt_str(&txt, "sensor ");
        snt_hex(&txt, suid->low);
        snt_str(&txt, on_change ? ", on change" : ", streaming at ");
        if (!on_change) {
            snt_milli(&txt, sfx_milli(rate));
            snt_str(&txt, " Hz");
        }
    }
    emit(e, i, &txt, t, 0);
    if (active) close_port(e, i);   /* the hub keeps the old sensor's request: its client goes, a new port starts clean */
    else restart(e, i, t);
}

void ss_from_list(SsEngine *e, const ScClient *list)
{
    static const int type[SS_STREAMS] = {SC_LIGHT, SC_ACCEL};
    for (int i = 0; i < SS_STREAMS; ++i) {
        const ScSensor *s = &list->sensor[type[i]];
        const int read = s->attr == SC_ATTR_READ;
        const int counts = list->up && s->lookup == SC_LOOKUP_ANSWERED && s->found && (read || s->attr == SC_ATTR_GAVE_UP) &&
                           !(read && s->attrs.available == 0);
        uint32_t lowest = 0;
        for (unsigned k = 0; counts && read && k < s->attrs.rate_count; ++k)   /* positive floats order as their bits */
            if (sfx_positive(s->attrs.rates[k]) && (!lowest || s->attrs.rates[k] < lowest)) lowest = s->attrs.rates[k];
        ss_sensor(e, i, counts, counts ? &s->suid : 0, counts && read ? s->attrs.stream_type : -1, lowest);
    }
}

void ss_conditions(SsEngine *e, const SsConditions *c)
{
    e->cond = *c;
    apply(e, now(e));
}

void ss_sleep(SsEngine *e, int asleep)
{
    e->asleep = asleep ? 1 : 0;
    apply(e, now(e));
}

/* ---- requests ---- */
static uint32_t build(const SsEngine *e, int i, int action, uint16_t txn, uint8_t *out, uint32_t cap)
{
    const SsChannel *ch = &e->ch[i];
    uint8_t payload[16], message[160];
    uint32_t n = 0, msg_id = SSC_MSG_DISABLE_REQ;
    int64_t batch = -1;
    if (action == PAS_ACT_ENABLE) {
        if (i == SS_LIGHT && ch->on_change) {
            msg_id = SSC_MSG_ON_CHANGE_CONFIG;                   /* sns_std_sensor's on-change config: no payload */
        } else {
            msg_id = SSC_MSG_SENSOR_CONFIG;                      /* sns_std_sensor_config {sample_rate} */
            n = ssc_build_sensor_config(payload, sizeof payload, ch->rate_bits);
            if (!n) return 0;
            if (i == SS_ACCEL && e->cfg.accel_batch_ms) batch = (int64_t)e->cfg.accel_batch_ms * 1000;
        }
    }
    const uint32_t m = ssc_build_request(message, sizeof message, &ch->suid, msg_id, SSC_DELIVERY_WAKEUP, payload, n, batch);
    return m ? ssc_build_qmi_request(out, cap, txn, message, m, -1) : 0;
}

static void result(SsEngine *e, int ok)
{
    if (e->ops->result) e->ops->result(e->ops->context, ok);
}

/* Logs the stream's transitions and tells the guard about timeouts; a failed stream's outputs go to rest. */
static void observe(SsEngine *e, int i, uint64_t at)
{
    SsChannel *ch = &e->ch[i];
    PasStream *s = stream_of(e, i);
    SntText t;
    if (s->timeouts != ch->timeouts_seen) {
        ch->timeouts += s->timeouts - ch->timeouts_seen;
        ch->timeouts_seen = s->timeouts;
        result(e, 0);
        snt_str(line(e, i, &t), "no answer within ");
        snt_udec(&t, e->cfg.stream.response_ms);
        snt_str(&t, " ms: the hub's state is unknown (a disable goes first)");
        emit(e, i, &t, at, 0);
    }
    if (s->rearmed != ch->rearmed_seen) {
        ch->rearmed_seen = s->rearmed;
        snt_str(line(e, i, &t), "no first event within ");
        snt_udec(&t, e->cfg.stream.first_event_ms);
        snt_str(&t, " ms: re-armed (disable, then enable)");
        emit(e, i, &t, at, 0);
    }
    if (s->state == ch->last_state) return;
    const int was = ch->last_state;
    ch->last_state = s->state;
    line(e, i, &t);
    if (s->state == PAS_ON) {
        snt_str(&t, "on (");
        if (i == SS_LIGHT && ch->on_change) {
            snt_str(&t, "on change");
        } else {
            snt_milli(&t, sfx_milli(ch->rate_bits));
            snt_str(&t, " Hz asked");
            if (i == SS_ACCEL && e->cfg.accel_batch_ms) snt_kd(&t, "batch_ms", e->cfg.accel_batch_ms);
        }
        snt_str(&t, ")");
        snt_kd(&t, "session", s->sessions);
        emit(e, i, &t, at, 0);
    } else if (s->state == PAS_OFF && was == PAS_DISABLING) {
        snt_str(&t, "off (");
        snt_str(&t, ch->why ? ch->why : "still wanted: re-armed, or a clean state first");
        snt_str(&t, ")");
        snt_kd(&t, "session", s->sessions);
        snt_kd(&t, "samples_total", ch->samples);
        emit(e, i, &t, at, 0);
    } else if (s->state == PAS_FAILED) {
        snt_str(&t, "failed for the boot (");
        snt_udec(&t, s->fails);
        snt_str(&t, " failures in a row); its outputs at rest");
        emit(e, i, &t, at, 1);
        if (i == SS_LIGHT) leave(e);
        else set_edge(e, PS_EDGE_NONE, at);
    }
}

static void send_due(SsEngine *e, int i, uint64_t at)
{
    SsChannel *ch = &e->ch[i];
    PasStream *s = stream_of(e, i);
    const int action = pas_action(s, at);
    if (action == PAS_ACT_NONE || !ch->open || !ch->up || !ch->found) return;
    if (e->ops->may_request && !e->ops->may_request(e->ops->context)) return;
    uint8_t qmi[200];
    const uint16_t txn = ch->next_txn;
    ch->next_txn = (uint16_t)(txn == 0xffff ? 1 : txn + 1);
    const uint32_t n = build(e, i, action, txn, qmi, sizeof qmi);
    if (!n || e->ops->send(e->ops->context, i, qmi, n)) {
        ch->refused_port++;
        result(e, 0);
        SntText t;
        snt_str(line(e, i, &t), action == PAS_ACT_ENABLE ? "the port refused the enable" : "the port refused the disable");
        emit(e, i, &t, at, 0);
        return;
    }
    ch->requests++;
    ch->ever_sent = 1;
    ch->txn = txn;
    pas_sent(s, action, at);
    observe(e, i, at);
}

/* ---- what the hub sends ---- */
static void response(SsEngine *e, int i, const SscQmi *m, uint64_t at)
{
    SsChannel *ch = &e->ch[i];
    PasStream *s = stream_of(e, i);
    if (s->outstanding == PAS_ACT_NONE || m->transaction != ch->txn) {   /* after its timeout, or not ours */
        ch->unexpected++;
        return;
    }
    SscResponse r;
    const int bad = ssc_qmi_response(m, &r);
    const int ok = !bad && ssc_response_ok(&r);
    const int action = s->outstanding;
    if (ok) {
        ch->responses++;
        if (r.has_client) {
            ch->has_client = 1;
            ch->client_id = r.client_id;
        }
    } else {
        ch->refused_hub++;
        SntText t;
        snt_str(line(e, i, &t), action == PAS_ACT_ENABLE ? "the hub refused the enable" : "the hub refused the disable");
        snt_kd(&t, "error", bad ? -1 : (int64_t)r.error);
        emit(e, i, &t, at, 0);
    }
    pas_response(s, ok, at);
    result(e, ok);
}

static void sample(SsEngine *e, int i, const SscSensorEvent *se, uint64_t at)
{
    SsChannel *ch = &e->ch[i];
    PasStream *s = stream_of(e, i);
    SntText t;
    if (i == SS_LIGHT) {
        if (se->count < 1) {ch->malformed++; return;}
        const uint32_t before = e->light.samples;
        const int32_t target = pal_event(&e->light, se->data[0], at);
        if (e->light.samples == before) {ch->dropped++; return;}
        ch->samples++;
        if (se->status == 0) ch->unreliable++;
        if (ch->sample_session != s->sessions) {
            ch->sample_session = s->sessions;
            snt_str(line(e, i, &t), "first sample:");
            snt_kd(&t, "lux", e->light.lux);
            snt_kd(&t, "session", s->sessions);
            emit(e, i, &t, at, 0);
        }
        light_target(e, target, at);
        return;
    }
    if (se->count < 3) {ch->malformed++; return;}
    const int use = pas_event(s, at);
    if (use == PAS_EV_DROP) {ch->dropped++; return;}
    if (use == PAS_EV_FIRST) sno_init(&e->orient);
    ch->samples++;
    if (se->status == 0) ch->unreliable++;
    if (ch->sample_session != s->sessions) {
        ch->sample_session = s->sessions;
        line(e, i, &t);
        snt_str(&t, "first sample:");
        snt_km(&t, "x", sfx_milli(se->data[0]));
        snt_km(&t, "y", sfx_milli(se->data[1]));
        snt_km(&t, "z", sfx_milli(se->data[2]));
        snt_str(&t, " m/s^2");
        snt_kd(&t, "session", s->sessions);
        emit(e, i, &t, at, 0);
    }
    const int state = ps_accel_sample(&e->orient, &e->cfg.axes, se->data, at);
    if (state >= 0) set_edge(e, ps_edge_of(state), at);
}

static void event(SsEngine *e, int i, const SscEvent *ev, uint64_t at)
{
    SsChannel *ch = &e->ch[i];
    PasStream *s = stream_of(e, i);
    SntText t;
    if (ev->msg_id == SSC_MSG_SENSOR_EVENT) {
        SscSensorEvent se;
        if (ssc_parse_sensor_event(ev->payload, ev->length, &se)) {ch->malformed++; return;}
        sample(e, i, &se, at);
    } else if (ev->msg_id == SSC_MSG_PHYSICAL_CONFIG_EVENT) {
        SscPhysicalConfig pc;
        if (ssc_parse_physical_config(ev->payload, ev->length, &pc)) {ch->malformed++; return;}
        ch->physical++;
        const int32_t milli = sfx_milli(pc.sample_rate);
        const uint32_t rate = milli > 0 ? (uint32_t)milli : 0;
        if (rate != ch->hub_rate_milli) {
            ch->hub_rate_milli = rate;
            snt_str(line(e, i, &t), "the hub runs it at ");
            snt_milli(&t, rate);
            snt_str(&t, " Hz");
            emit(e, i, &t, at, 0);
        }
    } else if (ev->msg_id == SSC_MSG_ERROR_EVENT) {
        int64_t error = -1;
        ssc_parse_error_event(ev->payload, ev->length, &error);
        ch->errors++;
        snt_str(line(e, i, &t), "error event");
        snt_kd(&t, "error", error);
        if (s->state == PAS_ENABLING || s->state == PAS_ON) {
            pas_error(s, at);
            result(e, 0);
            snt_str(&t, ": a failure; a disable goes first, then a fresh enable");
        }
        emit(e, i, &t, at, 0);
    } else {
        ch->other++;   /* flush (129), resampler (133), calibration (1022) and the like: not used */
    }
}

void ss_receive(SsEngine *e, int i, const uint8_t *data, uint32_t length)
{
    if (i < 0 || i >= SS_STREAMS) return;
    SsChannel *ch = &e->ch[i];
    if (e->off || !ch->open) {ch->ignored++; return;}
    const uint64_t at = now(e);
    SscQmi m;
    if (ssc_qmi_parse(data, length, &m)) {ch->malformed++; return;}
    if (m.type == SSC_QMI_RESPONSE && m.message == SSC_QMI_REQ) {
        response(e, i, &m, at);
        observe(e, i, at);
        return;
    }
    if (m.type != SSC_QMI_INDICATION || (m.message != SSC_QMI_REPORT && m.message != SSC_QMI_JUMBO_REPORT)) {ch->unexpected++; return;}
    int has_client = 0;
    uint64_t client = 0;
    const uint8_t *message = 0;
    uint32_t n = 0;
    if (ssc_qmi_report(&m, &has_client, &client, &message, &n)) {ch->malformed++; return;}
    ch->reports++;
    SscEvents events;
    SscEvent ev;
    if (ssc_events_open(&events, message, n) || !events.has_suid) {ch->malformed++; return;}
    if (!ch->found || !ssc_suid_equal(&events.suid, &ch->suid)) {ch->unexpected++; return;}
    int rc;
    while ((rc = ssc_events_next(&events, &ev)) == 1 && !e->off) event(e, i, &ev, at);
    if (rc < 0) ch->malformed++;
    observe(e, i, at);
}

void ss_tick(SsEngine *e)
{
    if (e->off) return;
    const uint64_t at = now(e);
    for (int i = 0; i < SS_STREAMS; ++i) {
        if (!e->ch[i].open) continue;
        if (i == SS_LIGHT) {
            const int32_t target = pal_tick(&e->light, at);
            if (target >= 0) light_target(e, target, at);
        } else {
            pas_tick(&e->accel, at);
        }
        observe(e, i, at);
        send_due(e, i, at);
    }
    if (e->lux_pending && at - e->lux_ms >= SS_LUX_MS) light_target(e, -1, at);
}

uint64_t ss_deadline(const SsEngine *e)
{
    if (e->off) return 0;
    const uint64_t at = now(e);
    uint64_t best = 0;
    for (int i = 0; i < SS_STREAMS; ++i) {
        const SsChannel *ch = &e->ch[i];
        if (!ch->open) continue;
        uint64_t d = i == SS_LIGHT ? pal_deadline(&e->light, at) : pas_deadline(&e->accel, at);
        /* due now after a tick: a request that is held (the guard's tokens: try again at the next regular tick; no port or sensor
         * yet: only their arrival changes anything) */
        if (d && d <= at) d = ch->up && ch->found ? at + 250 : 0;
        if (d && (!best || d < best)) best = d;
    }
    if (e->lux_pending) {
        const uint64_t d = e->lux_ms + SS_LUX_MS;
        if (!best || d < best) best = d;
    }
    return best;
}

int ss_idle(const SsEngine *e)
{
    if (e->off) return 1;
    for (int i = 0; i < SS_STREAMS; ++i) {
        const SsChannel *ch = &e->ch[i];
        const PasStream *s = cstream_of(e, i);
        if (!ch->open || !ch->up) continue;
        if (s->outstanding != PAS_ACT_NONE || s->dirty || (s->state != PAS_OFF && s->state != PAS_FAILED)) return 0;
    }
    return 1;
}

void ss_force_off(SsEngine *e)
{
    for (int i = 0; i < SS_STREAMS; ++i) {
        SsChannel *ch = &e->ch[i];
        const PasStream *s = stream_of(e, i);
        if (!ch->open || !ch->up) continue;
        if (s->outstanding == PAS_ACT_NONE && !s->dirty && (s->state == PAS_OFF || s->state == PAS_FAILED)) continue;
        const uint64_t at = now(e);
        SntText t;
        snt_str(line(e, i, &t), "not off in time: its port closed (the hub drops a gone client's requests)");
        emit(e, i, &t, at, 1);
        close_port(e, i);
        restart(e, i, at);
    }
}

void ss_off(SsEngine *e)
{
    if (e->off) return;
    const uint64_t at = now(e);
    e->off = 1;
    apply(e, at);
    set_edge(e, PS_EDGE_NONE, at);
    leave(e);
    for (int i = 0; i < SS_STREAMS; ++i) {
        SntText t;
        snt_str(line(e, i, &t), "the guard is off: nothing more is sent, every packet is ignored, the outputs at rest");
        emit(e, i, &t, at, 1);
    }
}

void ss_close_all(SsEngine *e)
{
    for (int i = 0; i < SS_STREAMS; ++i)
        if (e->ch[i].open && e->ch[i].ever_sent) close_port(e, i);
}

void ss_status(const SsEngine *e, int i, char *out, uint32_t size)
{
    SntText t;
    snt_start(&t, out, size);
    if (i < 0 || i >= SS_STREAMS) return;
    const SsChannel *ch = &e->ch[i];
    const PasStream *s = cstream_of(e, i);
    snt_str(&t, e->off ? "off (the guard)" : !ch->open ? "no port" : !ch->up ? "no service" : !ch->found ? "no sensor yet"
                                                                                               : pas_state_name(s->state));
    if (ch->why && s->state == PAS_OFF) {
        snt_str(&t, " (");
        snt_str(&t, ch->why);
        snt_str(&t, ")");
    }
    snt_str(&t, i == SS_LIGHT && ch->on_change ? ", on change" : ", ");
    if (!(i == SS_LIGHT && ch->on_change)) {
        snt_milli(&t, sfx_milli(ch->rate_bits));
        snt_str(&t, " Hz asked");
    }
    if (ch->hub_rate_milli) snt_km(&t, " hub Hz", ch->hub_rate_milli);
    snt_kd(&t, "sessions", s->sessions);
    snt_kd(&t, "requests", ch->requests);
    snt_kd(&t, "answers", ch->responses);
    snt_kd(&t, "refused", ch->refused_port + ch->refused_hub);
    snt_kd(&t, "unanswered", ch->timeouts);
    snt_kd(&t, "rearmed", s->rearmed);
    snt_kd(&t, "errors", ch->errors);
    snt_kd(&t, "samples", ch->samples);
    snt_kd(&t, "dropped", ch->dropped);
    if (ch->unreliable) snt_kd(&t, "unreliable", ch->unreliable);
    if (ch->malformed || ch->unexpected) {
        snt_kd(&t, "malformed", ch->malformed);
        snt_kd(&t, "unexpected", ch->unexpected);
    }
    if (ch->restarts || ch->closes) {
        snt_kd(&t, "restarts", ch->restarts);
        snt_kd(&t, "ports closed", ch->closes);
    }
}
