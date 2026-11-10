/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PianoMca.h"

#define NO_CYCLE 0xffu
#define ONLINE_UV 4000000    /* a bus above 4 V: a source is attached */
#define HV_MIN_UV 8000000    /* the 9 V request took when the bus settles in 8.0-9.6 V */
#define HV_MAX_UV 9600000
#define HV_LOST_UV 7000000
#define HV_ASK_AFTER 3       /* cycles after attach before 9 V is asked for (stock waits 15 s for the authentication) */
#define HV_CHECKS 3          /* cycles the bus has to reach 9 V in */
#define TRIES 3              /* refused writes of one wanted limit in a row, enable writes per attach */

/* What one cycle reads; pd: only when the source is a PD source. */
static const struct {uint32_t property; uint8_t pd;} kFields[McaFields] = {
    [McaBusVoltage] = {PIANO_MCA_BUS_VOLTAGE, 0},
    [McaBusCurrent] = {PIANO_MCA_BUS_CURRENT, 0},
    [McaType] = {PIANO_MCA_REAL_TYPE, 0},
    [McaIcl] = {PIANO_MCA_ICL, 0},
    [McaTbat] = {PIANO_MCA_TBAT, 0},
    [McaChargeEnable] = {PIANO_MCA_CHARGE_ENABLE, 0},
    [McaStatus] = {PIANO_MCA_STATUS, 0},
    [McaPackVoltage] = {PIANO_MCA_PACK_VOLTAGE, 0},
    [McaPackCurrent] = {PIANO_MCA_PACK_CURRENT, 0},
    [McaVerify] = {PIANO_MCA_VERIFY, 1},
    [McaPdos] = {PIANO_MCA_PDOS, 1},
};

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}
static uint32_t min32(uint32_t a, uint32_t b) {return a < b ? a : b;}

static int is_pd(int type) {return type == PIANO_MCA_TYPE_PD || type == PIANO_MCA_TYPE_PD_VERIFY || type == PIANO_MCA_TYPE_PD_PPS;}

void piano_mca_init(PianoMca *m, unsigned policy, int hv_allowed)
{
    zero(m, sizeof *m);
    m->cursor = NO_CYCLE;
    m->policy = policy ? 1 : 0;
    m->hv_allowed = policy && hv_allowed;
}

static void reset_attach(PianoMca *m)
{
    m->hv = McaHvOff;
    m->hv_checks = m->attach_cycles = 0;
    m->icl_tries = m->fcc_tries = m->enable_tries = 0;
    m->icl_want = m->fcc_want = 0;
}

void piano_mca_link(PianoMca *m, int up)
{
    m->cursor = NO_CYCLE;
    m->in_flight = m->refresh_wanted = 0;
    m->queued = m->sent_write = 0;
    m->pan_in_flight = m->pan_pending = 0;
    m->valid = 0;
    m->pdos = 0;
    if (!up) {m->pan_enable = 0; return;}
    m->links++;
    m->icl_set = m->fcc_set = 0;   /* a started firmware is on its defaults */
    m->attached = 0;
    reset_attach(m);
    m->pan_enable = 1;
    piano_mca_refresh(m);
}

static int wanted(const PianoMca *m, unsigned field)
{
    if (!kFields[field].pd) return 1;
    return piano_mca_has(m, McaType) && is_pd(m->value[McaType]);
}

static void skip_to_wanted(PianoMca *m)
{
    while (m->cursor < McaFields && !wanted(m, m->cursor)) m->cursor++;
}

void piano_mca_refresh(PianoMca *m)
{
    if (m->cursor != NO_CYCLE) {m->refresh_wanted = 1; return;}
    m->cursor = 0;
    skip_to_wanted(m);
}

static void header(uint8_t *out, uint32_t owner, uint32_t opcode)
{
    put32(out, owner);
    put32(out + 4, PIANO_MCA_TYPE_REQUEST);
    put32(out + 8, opcode);
}

int piano_mca_write_allowed(uint32_t property, uint32_t value, uint8_t bytes)
{
    switch (property) {
    case PIANO_MCA_ICL: return bytes == 4 && value >= PIANO_MCA_ICL_FLOOR && value <= PIANO_MCA_ICL_MAX_9V;
    case PIANO_MCA_FCC: return bytes == 4 && value >= 100 && value <= PIANO_MCA_FCC_9V;
    case PIANO_MCA_CHARGE_ENABLE: return bytes == 1 && value == 1;
    case PIANO_MCA_PD_FIXED: return bytes == 4 && (value == 5000 || value == 9000);
    default: return 0;
    }
}

uint32_t piano_mca_next(PianoMca *m, uint8_t out[PIANO_MCA_REQUEST_BYTES])
{
    if (m->in_flight) return 0;
    /* Writes go between cycles, so a cycle's readings belong to one setting. */
    while (m->queued && m->cursor == NO_CYCLE) {
        const uint32_t property = m->queue[0].property, value = m->queue[0].value;
        const uint8_t bytes = m->queue[0].bytes;
        if (m->policy && piano_mca_write_allowed(property, value, bytes)) {
            zero(out, PIANO_MCA_REQUEST_BYTES);
            header(out, PIANO_MCA_OWNER, PIANO_MCA_OP_WRITE);
            put32(out + 12, property);
            put32(out + 16, ++m->seq);
            if (bytes == 1) out[20] = (uint8_t)value;
            else put32(out + 20, value);
            m->sent_property = property;
            m->sent_opcode = PIANO_MCA_OP_WRITE;
            m->in_flight = 1;
            m->sent_write = 1;
            return PIANO_MCA_REQUEST_BYTES;
        }
        m->refused++;
        m->events |= McaEvRefused;
        for (unsigned i = 1; i < m->queued; ++i) m->queue[i - 1] = m->queue[i];
        m->queued--;
    }
    if (m->cursor == NO_CYCLE || m->cursor >= McaFields) return 0;
    zero(out, PIANO_MCA_REQUEST_BYTES);
    header(out, PIANO_MCA_OWNER, PIANO_MCA_OP_READ);
    put32(out + 12, kFields[m->cursor].property);
    put32(out + 16, ++m->seq);
    m->sent_property = kFields[m->cursor].property;
    m->sent_opcode = PIANO_MCA_OP_READ;
    m->in_flight = 1;
    return PIANO_MCA_REQUEST_BYTES;
}

uint32_t piano_mca_pan_next(PianoMca *m, uint8_t out[PIANO_MCA_PAN_REQUEST_BYTES])
{
    if (m->pan_in_flight) return 0;
    uint32_t command, argument = 0;
    if (m->pan_enable) {
        command = PIANO_MCA_PAN_ENABLE;
        m->pan_enable = 0;
    } else if (m->pan_pending) {
        while (!((m->pan_pending >> argument) & 1)) argument++;
        m->pan_pending &= (uint8_t)~(1u << argument);
        command = PIANO_MCA_PAN_ACK;
    } else {
        return 0;
    }
    zero(out, PIANO_MCA_PAN_REQUEST_BYTES);
    header(out, PIANO_MCA_OWNER_PAN, PIANO_MCA_PAN_WRITE);
    put32(out + 12, command);
    put32(out + 16, argument);
    m->pan_in_flight = 1;
    return PIANO_MCA_PAN_REQUEST_BYTES;
}

/* ---- the policy ---- */
uint32_t piano_mca_icl_for(int type, uint32_t pdo5v_ma)
{
    switch (type) {
    case PIANO_MCA_TYPE_SDP: return 500;
    case PIANO_MCA_TYPE_CDP: return 900;
    case PIANO_MCA_TYPE_DCP: case PIANO_MCA_TYPE_HVDCP: case PIANO_MCA_TYPE_HVDCP_3: case PIANO_MCA_TYPE_HVDCP_3_B:
    case PIANO_MCA_TYPE_HVDCP_3P5: case PIANO_MCA_TYPE_DCP_B:
        return PIANO_MCA_ICL_MAX_5V;
    case PIANO_MCA_TYPE_PD: case PIANO_MCA_TYPE_PD_VERIFY: case PIANO_MCA_TYPE_PD_PPS:
        /* capped by what the source offers at 5 V; without its PDOs, a computer port's 900 mA */
        return pdo5v_ma >= PIANO_MCA_ICL_FLOOR ? min32(pdo5v_ma, PIANO_MCA_ICL_MAX_5V) : 900;
    default:
        return 500;
    }
}

uint32_t piano_mca_fcc_for(int tbat_valid, int32_t tbat_c, int hv)
{
    if (!tbat_valid || tbat_c < PIANO_MCA_TBAT_LOW || tbat_c >= PIANO_MCA_TBAT_HIGH) return PIANO_MCA_FCC_FLOOR;
    return hv ? PIANO_MCA_FCC_9V : PIANO_MCA_FCC_5V;
}

uint32_t piano_mca_pdo_fixed(const PianoMca *m, uint32_t mv)
{
    for (unsigned i = 0; i < m->pdos; ++i)
        if (m->pdo[i][0] == mv && m->pdo[i][1] == mv) return m->pdo[i][2];
    return 0;
}

static void queue_write(PianoMca *m, uint32_t property, uint32_t value, uint8_t bytes)
{
    for (unsigned i = 0; i < m->queued; ++i)
        if (m->queue[i].property == property) {m->queue[i].value = value; m->queue[i].bytes = bytes; return;}
    if (m->queued >= sizeof m->queue / sizeof m->queue[0]) return;
    m->queue[m->queued].property = property;
    m->queue[m->queued].value = value;
    m->queue[m->queued].bytes = bytes;
    m->queued++;
}

static void hv_step(PianoMca *m, int type)
{
    const int32_t bus = m->value[McaBusVoltage];
    switch (m->hv) {
    case McaHvOff:
        if (m->attach_cycles >= HV_ASK_AFTER && is_pd(type) && piano_mca_pdo_fixed(m, 9000) &&
            !(piano_mca_has(m, McaVerify) && m->value[McaVerify]) && bus < 6000000) {
            queue_write(m, PIANO_MCA_PD_FIXED, 9000, 4);
            m->hv = McaHvAsked;
            m->hv_checks = 0;
            m->events |= McaEvHvAsked;
        }
        break;
    case McaHvAsked:
        if (bus >= HV_MIN_UV && bus <= HV_MAX_UV) {
            m->hv = McaHvOn;
            m->events |= McaEvHvOn;
        } else if (++m->hv_checks >= HV_CHECKS) {
            queue_write(m, PIANO_MCA_PD_FIXED, 5000, 4);
            m->hv = McaHvBlocked;
            m->events |= McaEvHvBlocked;
        }
        break;
    case McaHvOn:
        if (bus < HV_LOST_UV) {m->hv = McaHvBlocked; m->events |= McaEvHvBlocked;}
        break;
    default:
        break;
    }
}

static void policy(PianoMca *m)
{
    if (!m->policy || !piano_mca_has(m, McaBusVoltage)) return;
    const int attached = m->value[McaBusVoltage] > ONLINE_UV;
    if (!attached) {
        if (m->attached) {
            /* The firmware keeps the last values for the next source (a weak port would get them): back to its own. */
            m->events |= McaEvDetach;
            if (m->icl_set > PIANO_MCA_ICL_FLOOR) queue_write(m, PIANO_MCA_ICL, PIANO_MCA_ICL_FLOOR, 4);
            if (m->fcc_set && m->fcc_set != PIANO_MCA_FCC_FLOOR) queue_write(m, PIANO_MCA_FCC, PIANO_MCA_FCC_FLOOR, 4);
        }
        m->attached = 0;
        reset_attach(m);
        return;
    }
    if (!m->attached) {
        m->attached = 1;
        reset_attach(m);
        m->events |= McaEvAttach;
    }
    if (m->attach_cycles < 255) m->attach_cycles++;
    const int type = piano_mca_has(m, McaType) ? m->value[McaType] : PIANO_MCA_TYPE_UNKNOWN;
    if (m->hv_allowed) hv_step(m, type);
    const uint32_t pdo9 = piano_mca_pdo_fixed(m, 9000);
    const uint32_t icl = m->hv == McaHvOn ? min32(pdo9 ? pdo9 : PIANO_MCA_ICL_MAX_9V, PIANO_MCA_ICL_MAX_9V)
                                          : piano_mca_icl_for(type, piano_mca_pdo_fixed(m, 5000));
    /* The firmware's pack temperature when it has one a battery can have, else the gauges'. */
    const int fw_tbat = piano_mca_has(m, McaTbat) && m->value[McaTbat] > -20 && m->value[McaTbat] < 80;
    const uint32_t fcc = piano_mca_fcc_for(fw_tbat || m->ext_tbat_valid, fw_tbat ? m->value[McaTbat] : m->ext_tbat_c, m->hv == McaHvOn);
    if (icl != m->icl_want) {m->icl_want = icl; m->icl_tries = 0;}
    if (fcc != m->fcc_want) {m->fcc_want = fcc; m->fcc_tries = 0;}
    const int icl_off = piano_mca_has(m, McaIcl) ? (m->value[McaIcl] > (int32_t)icl + 50 || m->value[McaIcl] + 50 < (int32_t)icl)
                                                 : m->icl_set != icl;
    if (icl_off && m->icl_tries < TRIES) {queue_write(m, PIANO_MCA_ICL, icl, 4); m->icl_tries++;}
    if (m->fcc_set != fcc && m->fcc_tries < TRIES) {queue_write(m, PIANO_MCA_FCC, fcc, 4); m->fcc_tries++;}
    if (piano_mca_has(m, McaChargeEnable) && !m->value[McaChargeEnable] && m->enable_tries < TRIES) {
        queue_write(m, PIANO_MCA_CHARGE_ENABLE, 1, 1);
        m->enable_tries++;
    }
}

static void advance(PianoMca *m)
{
    m->in_flight = 0;
    m->cursor++;
    skip_to_wanted(m);
}

/* The writes of a cycle go out before the next cycle: a remembered refresh waits for them (writes_done starts it). */
static int finish(PianoMca *m)
{
    if (m->cursor == NO_CYCLE || m->cursor < McaFields) return 0;
    m->cursor = NO_CYCLE;
    m->cycles++;
    policy(m);
    if (m->refresh_wanted && !m->queued) {m->refresh_wanted = 0; piano_mca_refresh(m);}
    return 1;
}

static void drop_write(PianoMca *m)
{
    for (unsigned i = 1; i < m->queued; ++i) m->queue[i - 1] = m->queue[i];
    m->queued--;
    if (!m->queued) {m->refresh_wanted = 0; piano_mca_refresh(m);}  /* read the results back */
}

void piano_mca_timeout(PianoMca *m)
{
    if (!m->in_flight) return;
    m->timeouts++;
    if (m->sent_write) {
        /* the write counts as tried; its result is unknown */
        m->in_flight = m->sent_write = 0;
        m->last_property = m->queue[0].property;
        m->last_value = m->queue[0].value;
        m->last_result = -60;
        m->writes_failed++;
        m->events |= McaEvWrite;
        drop_write(m);
        return;
    }
    m->valid &= ~(1u << m->cursor);
    advance(m);
    finish(m);
}

void piano_mca_pan_timeout(PianoMca *m) {m->pan_in_flight = 0;}

void piano_mca_battery_temperature(PianoMca *m, int valid, int32_t celsius)
{
    m->ext_tbat_valid = valid != 0;
    m->ext_tbat_c = celsius;
}

uint32_t piano_mca_take_events(PianoMca *m)
{
    const uint32_t e = m->events;
    m->events = 0;
    return e;
}

static int receive_pan(PianoMca *m, const uint8_t *data, uint32_t length)
{
    const uint32_t opcode = le32(data + 8) & 0xff;
    if (opcode == PIANO_MCA_PAN_WRITE) {
        if (!m->pan_in_flight) {m->ignored++; return McaIgnored;}
        m->pan_in_flight = 0;
        m->pan_acks++;
        return McaPanDone;
    }
    if (opcode == PIANO_MCA_PAN_NOTIFY && length >= 13 && data[12] < PIANO_MCA_PAN_PORTS) {
        m->pan_notifies++;
        m->pan_pending |= (uint8_t)(1u << data[12]);
        return McaPanNotified;
    }
    m->ignored++;
    return McaIgnored;
}

int piano_mca_receive(PianoMca *m, const uint8_t *data, uint32_t length)
{
    if (!data || length < 12) {m->ignored++; return McaIgnored;}
    const uint32_t owner = le32(data), type = le32(data + 4), opcode = le32(data + 8);
    if (owner == PIANO_MCA_OWNER_PAN) return receive_pan(m, data, length);
    if (owner != PIANO_MCA_OWNER) {m->ignored++; return McaIgnored;}
    if (type == PIANO_MCA_TYPE_NOTIFY) {
        m->notifications++;
        piano_mca_refresh(m);
        return McaNotified;
    }
    if (type != PIANO_MCA_TYPE_REQUEST || length < 24 || !m->in_flight) {m->ignored++; return McaIgnored;}
    const uint32_t property = le32(data + 12), retcode = le32(data + 16), seq = le32(data + 20);
    if (seq != m->seq || property != m->sent_property || opcode != m->sent_opcode) {m->ignored++; return McaIgnored;}
    if (m->sent_write) {
        m->in_flight = m->sent_write = 0;
        const uint32_t value = m->queue[0].value;
        m->last_property = property;
        m->last_value = value;
        m->last_result = (int32_t)retcode;
        if (!retcode) {
            /* A limit the firmware took may be written again if it later reads otherwise; the tries count refusals in a row. The
             * enable bit is not: if the firmware clears it again, it means to. */
            m->writes_ok++;
            if (property == PIANO_MCA_ICL) {m->icl_set = value; m->icl_tries = 0;}
            if (property == PIANO_MCA_FCC) {m->fcc_set = value; m->fcc_tries = 0;}
        } else {
            m->writes_failed++;
        }
        m->events |= McaEvWrite;
        drop_write(m);
        return McaWritten;
    }
    m->answers++;
    const unsigned field = m->cursor;
    const uint8_t *payload = data + 24;
    const uint32_t have = length - 24;
    if (retcode) {
        m->errors++;
        m->valid &= ~(1u << field);
    } else if (field == McaPdos) {
        m->pdos = 0;
        for (unsigned i = 0; i < 7 && 12 * (i + 1) <= have; ++i) {
            const uint32_t lo = le32(payload + 12 * i), hi = le32(payload + 12 * i + 4), ma = le32(payload + 12 * i + 8);
            if (!hi) break;
            m->pdo[m->pdos][0] = lo; m->pdo[m->pdos][1] = hi; m->pdo[m->pdos][2] = ma;
            m->pdos++;
        }
        m->value[field] = m->pdos;
        m->valid |= 1u << field;
    } else if (have >= 4 || (field == McaChargeEnable && have >= 1)) {
        m->value[field] = field == McaChargeEnable ? payload[0] : (int32_t)le32(payload);
        m->valid |= 1u << field;
    } else {
        m->errors++;
        m->valid &= ~(1u << field);
    }
    advance(m);
    return finish(m) ? McaCycleDone : McaAnswer;
}

const char *piano_mca_type_name(int type)
{
    static const char *const names[] = {"unknown", "SDP", "CDP", "DCP", "float", "HVDCP", "HVDCP3", "HVDCP3B", "HVDCP3.5", "Type-C",
                                        "PD", "PD (verified)", "PD PPS", "13", "ACA", "DCP"};
    return type >= 0 && type < (int)(sizeof names / sizeof names[0]) ? names[type] : "other";
}
