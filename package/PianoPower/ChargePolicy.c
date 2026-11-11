/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The charging policy (ChargePolicy.h; docs/drivers/charging-policy.md). Written fresh from the behaviour and the numbers of the
 * Ubuntu history pages and the GPL xiaomi_mca driver read as a specification; no code taken. */
#include "ChargePolicy.h"

#define ONLINE_MV 4000           /* a bus above 4 V: a source is attached */
#define HV_MV 9000u
#define HV_MIN_MV 8000           /* 9 V counts as reached in 8.0-9.6 V */
#define HV_MAX_MV 9600
#define HV_5V_MAX_MV 6000        /* below: the bus is at 5 V */
#define HV_WAIT_MS 15000u        /* after an attach, a wake or a new generation (stock waits 15 s for the authentication) */
#define HV_SETTLE_MS 15000u      /* the bus has this long (and three decisions) to reach 9 V */
#define HV_SETTLE_DECISIONS 3
#define HV_RENEG_MS 10000u       /* a source back at 5 V by itself is asked again after this, */
#define HV_RENEG_MAX 3           /* this often per attach */
#define HV_RETRY_MS 60000u       /* a detach while 9 V was asked (a hard reset in answer looks like one) */
#define HV_FIXED_MIN_MA 100u
#define HV_PPS_MIN_MA 500u
#define HV_PPS_MAX_MA 3000u
#define FCC_TBAT_MIN 15
#define FCC_DERATE_FROM 38
#define FCC_DERATE_STEP 300u
#define FCC_RAISE_MS 30000u
#define TERM_HOT_C 48
#define TERM_COOL_C 46
#define LIMIT_HOT_C 55           /* the battery-health mode's own stop window, inside the firmware's 0-55 degC JEITA */
#define LIMIT_HOT_RELEASE_C 53
#define LIMIT_COLD_C 0
#define LIMIT_COLD_RELEASE_C 2
#define SOC_LOST_MS 300000u
#define STOP_CHECK_MS 60000u
#define STOP_LEAK_MA 100
#define PENDING_MS 10000u        /* a planned write with no answer is planned again after this (the owner dropped it) */
#define TRIES 3
#define MISMATCHES 2
#define REWRITES 3
#define ICL_SLACK_MA 50
#define WEDGE_MS 30000u
#define WEDGE_DECISIONS 3
#define WEDGE_RESYNCS 2
#define UNKNOWN_ENABLE 0xffu

static void zero(void *p, unsigned n) {uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;}
static uint32_t umin(uint32_t a, uint32_t b) {return a < b ? a : b;}
static uint32_t umax(uint32_t a, uint32_t b) {return a > b ? a : b;}
static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi) {return v < lo ? lo : v > hi ? hi : v;}
static int32_t floor_c(int32_t dc) {return dc >= 0 ? dc / 10 : -((-dc + 9) / 10);}
static int is_pd(int cls) {return cls == PIANO_CHG_C_PD || cls == PIANO_CHG_C_PD_PPS;}
static uint32_t icl_cap(const PianoChargeConfig *c) {return c->icl_cap_ma ? umin(c->icl_cap_ma, PIANO_CHG_ICL_ABS_MAX) : PIANO_CHG_ICL_CAP;}
static uint32_t fcc_cap(const PianoChargeConfig *c) {return c->fcc_cap_ma ? umin(c->fcc_cap_ma, PIANO_CHG_FCC_ABS_MAX) : PIANO_CHG_FCC_CAP;}

/* ---------------------------------------------------------------------------------------------------------------- configuration */
void piano_charge_config_default(PianoChargeConfig *c)
{
    zero(c, sizeof *c);
    c->writes = 1;
    c->stop_pct = 80;
    c->resume_pct = 75;
    c->icl_cap_ma = PIANO_CHG_ICL_CAP;
    c->fcc_cap_ma = PIANO_CHG_FCC_CAP;
}

int piano_charge_config_limit(PianoChargeConfig *c, unsigned on, unsigned stop_pct, unsigned resume_pct)
{
    if (!on) {c->limit = 0; return 0;}
    if (!resume_pct && stop_pct >= 45) resume_pct = stop_pct - 5;
    if (stop_pct < 50 || stop_pct > 100 || resume_pct < 40 || resume_pct >= stop_pct) return -1;
    c->limit = 1;
    c->stop_pct = (uint8_t)stop_pct;
    c->resume_pct = (uint8_t)resume_pct;
    return 0;
}

/* ----------------------------------------------------------------------------------------------------------------- the tables */
int piano_charge_class(int32_t type)
{
    switch (type) {
    case PIANO_CHG_T_SDP: return PIANO_CHG_C_SDP;
    case PIANO_CHG_T_CDP: return PIANO_CHG_C_CDP;
    case PIANO_CHG_T_DCP: case PIANO_CHG_T_HVDCP: case PIANO_CHG_T_HVDCP3: case PIANO_CHG_T_HVDCP3_B: case PIANO_CHG_T_HVDCP3P5:
        return PIANO_CHG_C_DCP;   /* stock maps QC to DCP at 5 V */
    case PIANO_CHG_T_FLOAT: case PIANO_CHG_T_OCP: return PIANO_CHG_C_FLOAT;
    case PIANO_CHG_T_TYPEC: return PIANO_CHG_C_TYPEC;
    case PIANO_CHG_T_PD: return PIANO_CHG_C_PD;
    case PIANO_CHG_T_PD_VERIFY: case PIANO_CHG_T_PPS: return PIANO_CHG_C_PD_PPS;
    default: return PIANO_CHG_C_REFUSED;
    }
}

const char *piano_charge_class_name(int cls)
{
    static const char *const names[] = {"none", "SDP", "CDP", "DCP/QC", "float/OCP", "Type-C", "PD", "PD PPS", "refused"};
    return cls >= 0 && cls <= PIANO_CHG_C_REFUSED ? names[cls] : "?";
}

uint32_t piano_charge_icl_5v(int cls, uint32_t pdo5v_ma)
{
    switch (cls) {
    case PIANO_CHG_C_SDP: return 500;
    case PIANO_CHG_C_CDP: return 900;
    case PIANO_CHG_C_DCP: return 1500;
    case PIANO_CHG_C_FLOAT: case PIANO_CHG_C_TYPEC: return 500;
    case PIANO_CHG_C_PD: case PIANO_CHG_C_PD_PPS: return pdo5v_ma ? clampu(pdo5v_ma, PIANO_CHG_ICL_DEFAULT, 1500) : 500;
    default: return 0;
    }
}

uint32_t piano_charge_icl_9v(int cls, uint32_t source_ma)
{
    const uint32_t ceiling = cls == PIANO_CHG_C_PD ? 1600u : cls == PIANO_CHG_C_PD_PPS ? 2500u : 0u;
    return ceiling && source_ma ? clampu(source_ma, PIANO_CHG_ICL_DEFAULT, ceiling) : 0;
}

uint32_t piano_charge_fcc_full(int cls, int hv_on)
{
    switch (cls) {
    case PIANO_CHG_C_SDP: return 500;
    case PIANO_CHG_C_CDP: return 900;
    case PIANO_CHG_C_DCP: return 2000;
    case PIANO_CHG_C_FLOAT: return 1000;
    case PIANO_CHG_C_TYPEC: return 500;
    case PIANO_CHG_C_PD: return hv_on ? 3000 : 2000;
    case PIANO_CHG_C_PD_PPS: return hv_on ? 3500 : 2000;
    default: return 0;
    }
}

uint32_t piano_charge_fcc_thermal(uint32_t full, int t_valid, int32_t t_c)
{
    if (!full) return 0;
    if (!t_valid || t_c < FCC_TBAT_MIN) return PIANO_CHG_FCC_DEFAULT;
    if (t_c <= FCC_DERATE_FROM || full <= PIANO_CHG_FCC_DEFAULT) return full;
    const uint32_t degrees = (uint32_t)(t_c - FCC_DERATE_FROM);
    if (degrees >= 20) return PIANO_CHG_FCC_DEFAULT;
    const uint32_t derate = degrees * FCC_DERATE_STEP;
    return derate < full - PIANO_CHG_FCC_DEFAULT ? full - derate : PIANO_CHG_FCC_DEFAULT;
}

uint32_t piano_charge_pdo_5v_ma(const uint32_t pdo[][3], unsigned n)
{
    uint32_t ma = 0;
    for (unsigned i = 0; i < n && i < 7; ++i)
        if (pdo[i][0] <= 5000 && pdo[i][1] >= 5000 && pdo[i][2] && (!ma || pdo[i][2] < ma)) ma = pdo[i][2];
    return ma;
}

uint32_t piano_charge_pdo_9v_ma(const uint32_t pdo[][3], unsigned n, int allow_pps, int *pps)
{
    uint32_t apdo = 0;
    *pps = 0;
    for (unsigned i = 0; i < n && i < 7; ++i) {
        if (pdo[i][0] == HV_MV && pdo[i][1] == HV_MV && pdo[i][2] >= HV_FIXED_MIN_MA) return pdo[i][2];
        if (pdo[i][0] < pdo[i][1] && pdo[i][0] <= HV_MV && pdo[i][1] >= HV_MV) apdo = umax(apdo, pdo[i][2]);
    }
    if (!allow_pps || apdo < HV_PPS_MIN_MA) return 0;
    *pps = 1;
    return umin(apdo, HV_PPS_MAX_MA);
}

uint32_t piano_charge_pps_word(uint32_t mv, uint32_t ma) {return (mv / 20) << 16 | (ma / 50);}

uint32_t piano_charge_fv_target(uint8_t *term_hot, int t_valid, int32_t t_warm_c)
{
    if (!t_valid || t_warm_c >= TERM_HOT_C) *term_hot = 1;
    else if (t_warm_c < TERM_COOL_C) *term_hot = 0;
    return *term_hot ? PIANO_CHG_FV_HOT : PIANO_CHG_FV_FULL;
}

int piano_charge_soc_hold(int hold, int32_t soc_pct, unsigned stop_pct, unsigned resume_pct)
{
    if (stop_pct >= 100) return 0;
    if (soc_pct >= (int32_t)stop_pct) return 1;
    if (soc_pct <= (int32_t)resume_pct) return 0;
    return hold;
}

int piano_charge_write_allowed(const PianoChargeConfig *c, uint32_t property, uint32_t value, uint8_t bytes)
{
    switch (property) {
    case PIANO_CHG_ENABLE: return bytes == 1 && value <= 1;
    case PIANO_CHG_FCC: return bytes == 4 && value >= 100 && value <= fcc_cap(c);
    case PIANO_CHG_ICL: return bytes == 4 && value >= PIANO_CHG_ICL_DEFAULT && value <= icl_cap(c);
    case PIANO_CHG_FV:
        return bytes == 4 && (value == PIANO_CHG_FV_DEFAULT || (c->full_charge && (value == PIANO_CHG_FV_FULL || value == PIANO_CHG_FV_HOT)));
    case PIANO_CHG_PD_FIXED: return bytes == 4 && (value == 5000 || (c->hv && value == HV_MV));
    case PIANO_CHG_PPS: {
        const uint32_t mv = (value >> 16) * 20, ma = (value & 0xffffu) * 50;
        return bytes == 4 && c->hv >= 2 && mv == HV_MV && ma >= HV_PPS_MIN_MA && ma <= HV_PPS_MAX_MA;
    }
    default: return 0;
    }
}

/* ------------------------------------------------------------------------------------------------------------------ the state */
static int slot_of(uint32_t property)
{
    switch (property) {
    case PIANO_CHG_ENABLE: return PIANO_CHG_S_ENABLE;
    case PIANO_CHG_FCC: return PIANO_CHG_S_FCC;
    case PIANO_CHG_ICL: return PIANO_CHG_S_ICL;
    case PIANO_CHG_PD_FIXED: case PIANO_CHG_PPS: return PIANO_CHG_S_VOLT;
    case PIANO_CHG_FV: return PIANO_CHG_S_FV;
    default: return -1;
    }
}

static void reset_tries(PianoChargeState *s)
{
    for (unsigned i = 0; i < PIANO_CHG_SLOTS; ++i) s->tries[i] = s->mismatch[i] = s->rewrites[i] = 0;
}

/* The firmware is on its defaults (a new generation) or has to be told everything again (a wake): forget what it took. What is
 * about the battery (the battery-health hysteresis, a stop of ours that may still be in force) is kept. */
static void forget(PianoChargeState *s, uint64_t now)
{
    s->icl_set = s->fcc_set = s->fv_set = 0;
    s->enable_set = UNKNOWN_ENABLE;
    s->volt_set_prop = s->volt_set_value = s->volt_want_prop = s->volt_want_value = 0;
    s->touched = s->pending = 0;
    reset_tries(s);
    s->fcc_full = 0;
    s->hv = PIANO_CHG_HV_OFF;
    s->hv_renegs = s->hv_checks = 0;
    if (s->hv_retry_ms < now + HV_WAIT_MS) s->hv_retry_ms = now + HV_WAIT_MS;
    s->attached = 0;
    s->type = -1;
    s->wedge_checks = 0;
    s->wedge_since_ms = 0;
}

void piano_charge_init(PianoChargeState *s)
{
    zero(s, sizeof *s);
    s->type = -1;
    s->enable_set = UNKNOWN_ENABLE;
    s->charge_ok = 1;
}

void piano_charge_ack(PianoChargeState *s, uint32_t property, uint32_t value, int32_t retcode)
{
    const int slot = slot_of(property);
    if (slot < 0) return;
    s->pending &= (uint8_t)~(1u << slot);
    if (retcode) {
        if (s->tries[slot] < 255) s->tries[slot]++;
        if (s->tries[slot] == TRIES) s->events |= PIANO_CHG_EV_GAVE_UP;
        return;
    }
    s->tries[slot] = 0;
    s->mismatch[slot] = 0;
    s->touched |= (uint8_t)(1u << slot);
    switch (slot) {
    case PIANO_CHG_S_ENABLE:
        s->enable_set = (uint8_t)value;
        if (value) s->disabled = 0;
        break;
    case PIANO_CHG_S_FCC: s->fcc_set = value; break;
    case PIANO_CHG_S_ICL: s->icl_set = value; break;
    case PIANO_CHG_S_VOLT: s->volt_set_prop = property; s->volt_set_value = value; break;
    case PIANO_CHG_S_FV:
        s->fv_set = value;
        if (value == PIANO_CHG_FV_DEFAULT) s->touched &= (uint8_t)~(1u << PIANO_CHG_S_FV);   /* the firmware's own again */
        break;
    }
}

/* One write into the plan, if it is not in flight, not given up, allowed and there is room. */
static void plan_write(const PianoChargeConfig *c, PianoChargeState *s, PianoChargePlan *p, uint64_t now, int slot,
                       uint32_t property, uint32_t value, uint8_t bytes)
{
    if (!c->writes) return;
    if (s->want[slot] != value) {s->want[slot] = value; s->tries[slot] = 0; s->rewrites[slot] = 0;}
    if (s->pending & (1u << slot)) return;
    if (s->tries[slot] >= TRIES) return;
    if (!piano_charge_write_allowed(c, property, value, bytes)) {p->events |= PIANO_CHG_EV_REFUSED; return;}
    if (p->writes >= PIANO_CHG_MAX_WRITES) return;
    PianoChargeWrite *w = &p->write[p->writes++];
    w->property = property;
    w->value = value;
    w->bytes = bytes;
    w->slot = (uint8_t)slot;
    s->pending |= (uint8_t)(1u << slot);
    s->pending_ms[slot] = now;
    if (slot == PIANO_CHG_S_ENABLE && !value) s->disabled = 1;   /* a disable may reach the firmware: ours to lift */
}

/* A value the firmware took, held against its read-back: two disagreeing read-backs in a row forget it (it is written again),
 * at most REWRITES times; then the slot is left alone. */
static int still_taken(PianoChargeState *s, PianoChargePlan *p, int slot, int readback_valid, int agrees)
{
    if (!readback_valid || agrees) {s->mismatch[slot] = 0; return 1;}
    if (++s->mismatch[slot] < MISMATCHES) return 1;
    s->mismatch[slot] = 0;
    if (++s->rewrites[slot] >= REWRITES) {
        if (s->tries[slot] < TRIES) p->events |= PIANO_CHG_EV_READBACK;
        s->tries[slot] = TRIES;
        return 1;
    }
    return 0;
}

static int volt_raised(const PianoChargeState *s)
{
    const int want = s->volt_want_prop && !(s->volt_want_prop == PIANO_CHG_PD_FIXED && s->volt_want_value == 5000);
    const int set = s->volt_set_prop && !(s->volt_set_prop == PIANO_CHG_PD_FIXED && s->volt_set_value == 5000);
    return want || set || s->hv == PIANO_CHG_HV_ASKED || s->hv == PIANO_CHG_HV_ON;
}

static void want_volt(PianoChargeState *s, uint32_t property, uint32_t value)
{
    s->volt_want_prop = property;
    s->volt_want_value = value;
}

static void want_5v(PianoChargeState *s)
{
    if (volt_raised(s)) want_volt(s, PIANO_CHG_PD_FIXED, 5000);
    s->hv = s->hv == PIANO_CHG_HV_BLOCKED ? PIANO_CHG_HV_BLOCKED : PIANO_CHG_HV_OFF;
}

static void plan_volt(const PianoChargeConfig *c, PianoChargeState *s, PianoChargePlan *p, uint64_t now)
{
    if (!s->volt_want_prop) return;
    if (s->volt_set_prop == s->volt_want_prop && s->volt_set_value == s->volt_want_value) return;
    plan_write(c, s, p, now, PIANO_CHG_S_VOLT, s->volt_want_prop, s->volt_want_value, 4);
}

/* The temperatures: the warmest and the coolest of the firmware's pack reading and the two gauges. */
typedef struct {int valid; int32_t warm, cool;} Temps;
static Temps temps(const PianoChargeInput *in)
{
    Temps t = {0, 0, 0};
    if ((in->valid & PIANO_CHG_IN_TBAT) && in->tbat_c > -20 && in->tbat_c < 80) {t.valid = 1; t.warm = t.cool = in->tbat_c;}
    if ((in->valid & PIANO_CHG_IN_GAUGES) && in->t_warm_dc > -400 && in->t_warm_dc < 900 && in->t_cool_dc > -400 &&
        in->t_cool_dc <= in->t_warm_dc) {
        const int32_t w = floor_c(in->t_warm_dc), k = floor_c(in->t_cool_dc);
        if (!t.valid || w > t.warm) t.warm = w;
        if (!t.valid || k < t.cool) t.cool = k;
        t.valid = 1;
    }
    return t;
}

static int soc_valid(const PianoChargeInput *in)
{
    return (in->valid & PIANO_CHG_IN_GAUGES) && in->soc_max_pct >= 0 && in->soc_max_pct <= 100;
}

/* The 9 V step (Ubuntu's hv_charge machine; one step per decision). Returns the source's current at 9 V while the bus is there. */
static uint32_t hv_step(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                        uint64_t now, int cls, int charge_ok)
{
    const int32_t bus = in->bus_mv;
    const int at_9v = bus >= HV_MIN_MV && bus <= HV_MAX_MV;
    int pps = 0;
    uint32_t ma = 0;
    if (is_pd(cls) && (in->valid & PIANO_CHG_IN_PDOS)) ma = piano_charge_pdo_9v_ma(in->pdo, in->pdos, c->hv >= 2, &pps);
    else if (is_pd(cls) && (s->hv == PIANO_CHG_HV_ASKED || s->hv == PIANO_CHG_HV_ON)) {ma = s->hv_ma; pps = s->hv_pps;}   /* a failed
                                                                                          * PDO read is no change of offer */
    /* the source has to keep offering 9 V the way it was asked for */
    if (s->hv != PIANO_CHG_HV_OFF && s->hv != PIANO_CHG_HV_BLOCKED && pps != s->hv_pps) ma = 0;
    if (!c->hv || !charge_ok) {
        if (volt_raised(s)) want_5v(s);
        return 0;
    }
    switch (s->hv) {
    case PIANO_CHG_HV_OFF:
        if (!ma) return 0;
        if (at_9v && !s->volt_want_prop) {
            /* already at 9 V without a request of ours (a wake without a handover): take it as asked */
            s->hv = PIANO_CHG_HV_ON;
            s->hv_pps = (uint8_t)pps;
            s->hv_ma = ma;
            p->events |= PIANO_CHG_EV_HV_ON;
            return ma;
        }
        if (now < s->hv_retry_ms || now < s->attach_ms + HV_WAIT_MS || bus >= HV_5V_MAX_MV) return 0;
        if (!(in->valid & PIANO_CHG_IN_VERIFY) || in->verify) return 0;   /* never in the middle of an authentication (#20) */
        s->hv = PIANO_CHG_HV_ASKED;
        s->hv_pps = (uint8_t)pps;
        s->hv_ma = ma;
        s->hv_checks = 0;
        s->hv_asked_ms = now;
        want_volt(s, pps ? PIANO_CHG_PPS : PIANO_CHG_PD_FIXED, pps ? piano_charge_pps_word(HV_MV, ma) : HV_MV);
        p->events |= PIANO_CHG_EV_HV_ASKED;
        return 0;
    case PIANO_CHG_HV_ASKED:
        if (ma && at_9v) {
            s->hv = PIANO_CHG_HV_ON;
            p->events |= PIANO_CHG_EV_HV_ON;
            return ma;
        }
        if (s->hv_checks < 255) s->hv_checks++;
        if (ma && (now < s->hv_asked_ms + HV_SETTLE_MS || s->hv_checks < HV_SETTLE_DECISIONS)) return 0;
        break;
    case PIANO_CHG_HV_ON:
        if (ma && at_9v) return ma;
        if (ma && bus < HV_5V_MAX_MV && s->hv_renegs < HV_RENEG_MAX) {
            /* back at 5 V by itself (a Xiaomi charger does after its authentication): asked again after a pause */
            s->hv_renegs++;
            s->hv = PIANO_CHG_HV_OFF;
            s->volt_want_prop = s->volt_want_value = 0;
            s->volt_set_prop = s->volt_set_value = 0;
            s->hv_retry_ms = now + HV_RENEG_MS;
            p->events |= PIANO_CHG_EV_HV_RETRY;
            return 0;
        }
        break;
    default:
        return 0;
    }
    /* not reached, dropped or refused: 5 V until the source is detached */
    want_volt(s, PIANO_CHG_PD_FIXED, 5000);
    s->hv = PIANO_CHG_HV_BLOCKED;
    p->events |= PIANO_CHG_EV_HV_BLOCKED;
    return 0;
}

/* The charge current with its thermal steps: down at once, up one step (300 mA) per 30 s with a degree to spare. */
static uint32_t fcc_target(PianoChargeState *s, uint32_t full, const Temps *t, uint64_t now)
{
    uint32_t fcc = piano_charge_fcc_thermal(full, t->valid, t->warm);
    if (s->fcc_set && full == s->fcc_full && fcc > s->fcc_set) {
        if (now < s->fcc_raise_ms) return s->fcc_set;
        fcc = umin(piano_charge_fcc_thermal(full, t->valid, t->warm + 1), s->fcc_set + FCC_DERATE_STEP);
        if (fcc < s->fcc_set) fcc = s->fcc_set;
    }
    return fcc;
}

static void plan_fcc(const PianoChargeConfig *c, PianoChargeState *s, PianoChargePlan *p, uint64_t now, uint32_t fcc, uint32_t full)
{
    if (!fcc || s->fcc_set == fcc) return;
    const uint8_t before = p->writes;
    plan_write(c, s, p, now, PIANO_CHG_S_FCC, PIANO_CHG_FCC, fcc, 4);
    if (p->writes != before) {s->fcc_full = full; s->fcc_raise_ms = now + FCC_RAISE_MS;}
}

static void plan_icl(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                     uint64_t now, uint32_t icl)
{
    if (!icl) return;
    if (s->icl_set == icl) {
        const int32_t rb = in->icl_ma;
        const int agrees = rb <= (int32_t)icl + ICL_SLACK_MA && rb + ICL_SLACK_MA >= (int32_t)icl;
        if (still_taken(s, p, PIANO_CHG_S_ICL, (in->valid & PIANO_CHG_IN_ICL) != 0, agrees)) return;
        s->icl_set = 0;
    }
    plan_write(c, s, p, now, PIANO_CHG_S_ICL, PIANO_CHG_ICL, icl, 4);
}

static void plan_enable(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                        uint64_t now, uint32_t value)
{
    if (s->enable_set == value) {
        const int valid = (in->valid & PIANO_CHG_IN_ENABLE) != 0;
        if (still_taken(s, p, PIANO_CHG_S_ENABLE, valid, valid && in->enable == (int32_t)value)) return;
        s->enable_set = UNKNOWN_ENABLE;
    }
    plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, value, 1);
}

static void plan_fv(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                    uint64_t now, const Temps *t)
{
    uint32_t target = 0;
    if (c->full_charge) target = piano_charge_fv_target(&s->term_hot, t->valid, t->warm);
    else if (s->touched & (1u << PIANO_CHG_S_FV)) target = PIANO_CHG_FV_DEFAULT;   /* the option went off: the firmware's own */
    p->fv_mv = target;
    if (!target) return;
    const int valid = (in->valid & PIANO_CHG_IN_FV) != 0;
    const int agrees = valid && in->fv_raw == piano_charge_fv_raw(target);
    if (agrees && s->fv_set != target) {
        /* already in force (the firmware kept it across a wake): nothing to send */
        s->fv_set = target;
        if (target != PIANO_CHG_FV_DEFAULT) s->touched |= 1u << PIANO_CHG_S_FV;
        else s->touched &= (uint8_t)~(1u << PIANO_CHG_S_FV);
    }
    if (s->fv_set == target) {
        if (still_taken(s, p, PIANO_CHG_S_FV, valid, agrees)) return;
        s->fv_set = 0;
    }
    plan_write(c, s, p, now, PIANO_CHG_S_FV, PIANO_CHG_FV, target, 4);
}

static void expire_pending(PianoChargeState *s, uint64_t now)
{
    for (unsigned i = 0; i < PIANO_CHG_SLOTS; ++i)
        if ((s->pending & (1u << i)) && now >= s->pending_ms[i] + PENDING_MS) s->pending &= (uint8_t)~(1u << i);
}

static int limit_on(const PianoChargeConfig *c) {return c->limit && c->stop_pct < 100;}

/* Battery health: the charge hysteresis, the mode's hot/cold stops, and what happens without a charge reading. Returns charge_ok. */
static int protection(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                      const Temps *t, uint64_t now)
{
    if (!limit_on(c)) {s->hold = s->hot = s->cold = 0; s->soc_lost = 0; return 1;}
    if (soc_valid(in)) {
        const uint8_t hold = (uint8_t)piano_charge_soc_hold(s->hold, in->soc_max_pct, c->stop_pct, c->resume_pct);
        if (hold != s->hold) p->events |= hold ? PIANO_CHG_EV_LIMIT_STOP : PIANO_CHG_EV_LIMIT_RESUME;
        s->hold = hold;
        s->soc_lost = 0;
    } else {
        /* No charge reading: the decision stands for 5 min; then charging goes on (a tablet that cannot charge because a gauge
         * read fails is worse than one charged past the limit). */
        if (!s->soc_lost) {s->soc_lost = 1; s->soc_lost_ms = now;}
        else if (now >= s->soc_lost_ms + SOC_LOST_MS && s->hold) {s->hold = 0; p->events |= PIANO_CHG_EV_SOC_LOST;}
    }
    if (t->valid) {
        if (t->warm >= LIMIT_HOT_C) {if (!s->hot) p->events |= PIANO_CHG_EV_HOT; s->hot = 1;}
        else if (t->warm < LIMIT_HOT_RELEASE_C) s->hot = 0;
        if (t->cool < LIMIT_COLD_C) {if (!s->cold) p->events |= PIANO_CHG_EV_COLD; s->cold = 1;}
        else if (t->cool >= LIMIT_COLD_RELEASE_C) s->cold = 0;
    }
    p->why_off = s->hold ? PIANO_CHG_OFF_LIMIT : s->hot ? PIANO_CHG_OFF_HOT : s->cold ? PIANO_CHG_OFF_COLD : PIANO_CHG_OFF_NONE;
    return !(s->hold || s->hot || s->cold);
}

/* The firmware says no source while the PMIC's Type-C block sees one (Ubuntu #18: after a resume the firmware reported online 0,
 * VBUS 0, ignored replugs; a rebind, which sends everything again, recovered it). */
static void wedge_check(const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, uint64_t now)
{
    if (!(in->valid & PIANO_CHG_IN_TYPEC) || !in->typec_source) {
        s->wedge_since_ms = 0;
        s->wedge_checks = 0;
        if (in->valid & PIANO_CHG_IN_TYPEC) s->wedge_resyncs = s->wedge_reported = 0;
        return;
    }
    if (!s->wedge_since_ms) {s->wedge_since_ms = now ? now : 1; s->wedge_checks = 0;}
    if (s->wedge_checks < 255) s->wedge_checks++;
    if (now < s->wedge_since_ms + WEDGE_MS || s->wedge_checks < WEDGE_DECISIONS) return;
    if (s->wedge_resyncs < WEDGE_RESYNCS) {
        s->wedge_resyncs++;
        p->resync = 1;
        p->events |= PIANO_CHG_EV_WEDGE_RESYNC;
        s->wedge_since_ms = now ? now : 1;
        s->wedge_checks = 0;
    } else if (!s->wedge_reported) {
        s->wedge_reported = 1;
        p->events |= PIANO_CHG_EV_WEDGED;
    }
}

static void detached(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p, uint64_t now)
{
    if (s->attached) {
        p->events |= PIANO_CHG_EV_DETACH;
        if (s->hv == PIANO_CHG_HV_ASKED && s->hv_retry_ms < now + HV_RETRY_MS) s->hv_retry_ms = now + HV_RETRY_MS;
        reset_tries(s);
    }
    s->attached = 0;
    s->type = -1;
    s->hv = PIANO_CHG_HV_OFF;
    s->hv_renegs = 0;
    s->volt_set_prop = s->volt_set_value = s->volt_want_prop = s->volt_want_value = 0;   /* the next source starts at 5 V */
    s->fcc_full = 0;
    s->stop_noted = s->fw_noted = 0;
    /* The firmware keeps the last values for the next source (a weak port would get them): its own back, only where we wrote. A
     * stop of ours is lifted too: the next attach decides again within a cycle, and a stop must never outlive us. */
    const int rb_raised = (in->valid & PIANO_CHG_IN_ICL) && in->icl_ma > (int32_t)PIANO_CHG_ICL_DEFAULT + ICL_SLACK_MA;
    if (((s->touched & (1u << PIANO_CHG_S_ICL)) || rb_raised) && s->icl_set != PIANO_CHG_ICL_DEFAULT)
        plan_write(c, s, p, now, PIANO_CHG_S_ICL, PIANO_CHG_ICL, PIANO_CHG_ICL_DEFAULT, 4);
    if ((s->touched & (1u << PIANO_CHG_S_FCC)) && s->fcc_set != PIANO_CHG_FCC_DEFAULT)
        plan_write(c, s, p, now, PIANO_CHG_S_FCC, PIANO_CHG_FCC, PIANO_CHG_FCC_DEFAULT, 4);
    if (s->disabled) plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 1, 1);
    p->icl_ma = PIANO_CHG_ICL_DEFAULT;
    p->fcc_ma = PIANO_CHG_FCC_DEFAULT;
    p->charge = 1;
    wedge_check(in, s, p, now);
}

/* The sleep handover (Ubuntu v1.1.0): the attached type's 5 V limits (not 100 mA), the source back at 5 V, and with battery health
 * on and the limit not reached, charging stopped for the sleep (the limit cannot be watched while the system is suspended). */
static void handover(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p,
                     const Temps *t, uint64_t now)
{
    if (!s->sleeping) {s->sleeping = 1; p->events |= PIANO_CHG_EV_SLEEP;}
    s->hold_awake = 0;
    const int cls = s->attached && s->type >= 0 ? piano_charge_class(s->type) : PIANO_CHG_C_NONE;
    p->cls = (uint8_t)cls;
    if (c->writes && cls != PIANO_CHG_C_NONE && cls != PIANO_CHG_C_REFUSED) {
        const unsigned pdos = (in->valid & PIANO_CHG_IN_PDOS) ? in->pdos : 0;
        const uint32_t icl = umin(piano_charge_icl_5v(cls, piano_charge_pdo_5v_ma(in->pdo, pdos)), icl_cap(c));
        const uint32_t fcc = piano_charge_fcc_thermal(umin(piano_charge_fcc_full(cls, 0), fcc_cap(c)), t->valid, t->warm);
        const int stop = limit_on(c) && (s->hold || s->hot || s->cold || !c->sleep_charge);
        if (stop) {
            p->why_off = s->hold ? PIANO_CHG_OFF_LIMIT : s->hot ? PIANO_CHG_OFF_HOT : s->cold ? PIANO_CHG_OFF_COLD : PIANO_CHG_OFF_SLEEP;
            if (s->enable_set != 0) plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 0, 1);
        }
        if ((s->touched & (1u << PIANO_CHG_S_FCC)) && s->fcc_set != fcc) plan_write(c, s, p, now, PIANO_CHG_S_FCC, PIANO_CHG_FCC, fcc, 4);
        if ((s->touched & (1u << PIANO_CHG_S_ICL)) && s->icl_set != icl) plan_write(c, s, p, now, PIANO_CHG_S_ICL, PIANO_CHG_ICL, icl, 4);
        want_5v(s);
        plan_volt(c, s, p, now);
        p->icl_ma = icl;
        p->fcc_ma = fcc;
        p->charge = (uint8_t)!stop;
        p->bus_mv = 5000;
    }
    /* ready once nothing is left to send; a value refused three times is not waited for */
    p->sleep_ready = p->writes == 0 && s->pending == 0;
}

void piano_charge_decide(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p)
{
    zero(p, sizeof *p);
    const uint64_t now = in->now_ms;
    s->decisions++;
    p->events = s->events;
    s->events = 0;
    if (!s->started) {
        s->started = 1;
        s->generation = in->generation;
        s->wakes = in->wakes;
        forget(s, now);
    }
    if (in->generation != s->generation) {
        s->generation = in->generation;
        forget(s, now);
        p->events |= PIANO_CHG_EV_GENERATION;
    }
    if (in->wakes != s->wakes) {
        s->wakes = in->wakes;
        forget(s, now);
        s->sleeping = 0;
        s->wedge_resyncs = s->wedge_reported = 0;
        p->resync = 1;
        p->events |= PIANO_CHG_EV_WAKE;
    }
    expire_pending(s, now);
    const Temps t = temps(in);
    if (in->sleep == PIANO_CHG_WILL_SLEEP) {handover(c, in, s, p, &t, now); return;}
    s->sleeping = 0;
    p->hold_awake = s->hold_awake;
    p->charge = s->charge_ok;
    if (!(in->valid & PIANO_CHG_IN_BUS)) return;   /* no bus reading is neither a detach nor an attach: nothing changes */
    if (in->bus_mv < ONLINE_MV) {
        detached(c, in, s, p, now);
        s->hold_awake = p->hold_awake = 0;
        return;
    }
    s->wedge_since_ms = 0;
    s->wedge_checks = s->wedge_resyncs = s->wedge_reported = 0;
    if (!s->attached) {
        s->attached = 1;
        s->attach_ms = now;
        s->type = -1;
        s->hv = PIANO_CHG_HV_OFF;
        s->hv_renegs = s->hv_checks = 0;
        s->fcc_full = 0;
        s->stop_noted = s->fw_noted = 0;
        reset_tries(s);
        p->events |= PIANO_CHG_EV_ATTACH;
    }
    /* the type: a failed read changes nothing; a new type starts its limits afresh */
    if (in->valid & PIANO_CHG_IN_TYPE) {
        if (in->type != s->type) {
            const int was = s->type >= 0 ? piano_charge_class(s->type) : PIANO_CHG_C_NONE;
            const int now_cls = piano_charge_class(in->type);
            if (s->type >= 0) p->events |= PIANO_CHG_EV_TYPE;
            /* a PD classification back after an Unknown sample must not stay blocked: the same retry budget */
            if (is_pd(now_cls) && s->type >= 0 && !is_pd(was) && s->hv == PIANO_CHG_HV_BLOCKED && s->hv_renegs < HV_RENEG_MAX) {
                s->hv_renegs++;
                s->hv = PIANO_CHG_HV_OFF;
                if (s->hv_retry_ms < now + HV_RENEG_MS) s->hv_retry_ms = now + HV_RENEG_MS;
            }
            s->type = in->type;
            reset_tries(s);
            s->fcc_full = 0;
        }
    }
    if (s->type < 0) return;
    const int cls = piano_charge_class(s->type);
    p->cls = (uint8_t)cls;
    if (cls == PIANO_CHG_C_REFUSED) {   /* the firmware's own values: nothing is written for an unknown source */
        s->hold_awake = p->hold_awake = 0;
        return;
    }
    const int charge_ok = protection(c, in, s, p, &t, now);
    if (charge_ok != s->charge_ok) {s->charge_ok = (uint8_t)charge_ok; s->off_since_ms = now; s->stop_noted = 0;}
    p->charge = (uint8_t)charge_ok;
    const uint32_t hv_ma = hv_step(c, in, s, p, now, cls, charge_ok);
    const int hv_on = s->hv == PIANO_CHG_HV_ON && hv_ma;
    p->hv = s->hv;
    const unsigned pdos = (in->valid & PIANO_CHG_IN_PDOS) ? in->pdos : 0;
    const uint32_t icl = umin(hv_on ? piano_charge_icl_9v(cls, hv_ma) : piano_charge_icl_5v(cls, piano_charge_pdo_5v_ma(in->pdo, pdos)),
                              icl_cap(c));
    const uint32_t full = umin(piano_charge_fcc_full(cls, hv_on), fcc_cap(c));
    const uint32_t fcc = fcc_target(s, full, &t, now);
    p->icl_ma = icl;
    p->fcc_ma = fcc;
    p->bus_mv = s->hv == PIANO_CHG_HV_ASKED || s->hv == PIANO_CHG_HV_ON ? HV_MV : 5000;
    /* the writes, in this order: a stop first, the currents, the bus voltage, a resume, the float voltage */
    if (!charge_ok) plan_enable(c, in, s, p, now, 0);
    plan_fcc(c, s, p, now, fcc, full);
    plan_icl(c, in, s, p, now, icl);
    plan_volt(c, s, p, now);
    if (charge_ok) {
        if (s->disabled) plan_enable(c, in, s, p, now, 1);
        else if ((in->valid & PIANO_CHG_IN_ENABLE) && !in->enable && !s->fw_noted) {s->fw_noted = 1; p->events |= PIANO_CHG_EV_FW_DISABLED;}
    }
    plan_fv(c, in, s, p, now, &t);
    /* the stop's read-back: 0x2003 reads 0 and yet the packs take current a minute later */
    if (!charge_ok && !s->stop_noted && s->enable_set == 0 && (in->valid & PIANO_CHG_IN_ENABLE) && !in->enable &&
        (in->valid & PIANO_CHG_IN_GAUGES) && in->ibat_ma > STOP_LEAK_MA && now >= s->off_since_ms + STOP_CHECK_MS) {
        s->stop_noted = 1;
        p->events |= PIANO_CHG_EV_STOP_LEAK;
    }
    /* the charging hold: protected charging below the limit keeps the system out of idle sleep, so the stop comes on time */
    s->hold_awake = p->hold_awake = (uint8_t)(c->writes && limit_on(c) && charge_ok && soc_valid(in));
}

void piano_charge_shutdown(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p)
{
    zero(p, sizeof *p);
    const uint64_t now = in->now_ms;
    p->events = s->events;
    s->events = 0;
    expire_pending(s, now);
    s->hold_awake = 0;
    if (s->disabled) plan_write(c, s, p, now, PIANO_CHG_S_ENABLE, PIANO_CHG_ENABLE, 1, 1);
    if (s->attached && volt_raised(s)) {want_5v(s); plan_volt(c, s, p, now);}
    p->sleep_ready = p->writes == 0 && s->pending == 0;
}

/* --------------------------------------------------------------------------------------------------- the direct-charging curve */
#define CP_VBAT_MIN_MV 3450
#define CP_VBAT_NORMAL_MV 4473     /* the normal table's last step (with the float option on) */
#define CP_VBAT_DEFAULT_MV 4387    /* the firmware's 4400 mV float less 13 mV of hysteresis (float option off) */
#define CP_VBAT_FFC_MV 4540
#define CP_VBAT_HYS_MV 13
#define CP_CELL_OCP_MA 7000
#define CP_TBAT_MIN 15
#define CP_TBAT_FULL 18
#define CP_TBAT_WARM 40
#define CP_TBAT_EXIT 45
#define CP_TBAT_HYS 2
#define CP_HOLD_MS 30000u
#define CP_RAMP_MS 15000u
#define CP_RAMP_MA 500u
#define CP_REDUCED_MA 4500u
#define CP_IBAT_MAX_MA 12000u      /* the 2:1 stage's battery side */
#define CP_SOURCE_MAX_MA 6000u
#define CP_POWER_MW 67000u
#define CP_FFC_SOC 95

static const struct {uint32_t mv, ma;} kCpNormal[] = {{4150, 8000}, {4473, 7230}};
static const struct {uint32_t mv, ma;} kCpFfc[] = {{4144, 12400}, {4300, 10650}, {4475, 9040}, {4540, 7230}};

uint32_t piano_charge_cp_ibat_ma(const PianoChargeCpInput *in, PianoChargeCpState *s)
{
    if (!in->verified || !in->t_valid) return 0;
    const uint8_t profile = in->full_charge && in->soc_valid && in->soc_max_pct < CP_FFC_SOC && in->t_cool_c >= CP_TBAT_FULL &&
                            in->t_warm_c < 48 ? PIANO_CHG_CP_FFC : PIANO_CHG_CP_NORMAL;
    if (profile != s->profile) {s->profile = profile; s->stage = 0;}
    const int32_t vbat_max = profile == PIANO_CHG_CP_FFC ? CP_VBAT_FFC_MV : in->full_charge ? CP_VBAT_NORMAL_MV : CP_VBAT_DEFAULT_MV;
    const int32_t cell = in->cell_ma_max < 0 ? -in->cell_ma_max : in->cell_ma_max;
    if (in->running) {
        if (in->t_cool_c < CP_TBAT_MIN || in->t_warm_c >= CP_TBAT_EXIT || in->vbat_max_mv >= vbat_max || cell >= CP_CELL_OCP_MA) return 0;
    } else if (in->vbat_min_mv < CP_VBAT_MIN_MV || in->vbat_max_mv >= vbat_max - CP_VBAT_HYS_MV || cell >= CP_CELL_OCP_MA ||
               in->t_cool_c < CP_TBAT_MIN || in->t_warm_c >= CP_TBAT_WARM - CP_TBAT_HYS) {
        return 0;
    }
    /* the step table: the stage only moves on while the voltage passes each step */
    const unsigned steps = profile == PIANO_CHG_CP_FFC ? 4 : 2;
    while (s->stage + 1u < steps && in->vbat_max_mv >= (int32_t)(profile == PIANO_CHG_CP_FFC ? kCpFfc[s->stage].mv : kCpNormal[s->stage].mv))
        s->stage++;
    uint32_t ma = umin(CP_IBAT_MAX_MA, profile == PIANO_CHG_CP_FFC ? kCpFfc[s->stage].ma : kCpNormal[s->stage].ma);
    /* the source: twice its APDO current on the battery side, within 67 W at the voltage asked */
    uint32_t source = umin(in->apdo_ma, CP_SOURCE_MAX_MA);
    source = umin(source, CP_POWER_MW * 1000u / umax(in->vbus_mv, 6000));
    ma = umin(ma, umin(source, CP_IBAT_MAX_MA / 2) * 2);
    /* warm: 4.5 A from 40 degC, held 30 s, then back 0.5 A per 15 s once 2 degC cooler */
    if (!s->warm_valid) {s->warm_valid = 1; s->warm_ma = CP_IBAT_MAX_MA; s->warm_ms = in->now_ms;}
    if (in->t_warm_c >= CP_TBAT_WARM) {
        s->warm_ma = CP_REDUCED_MA;
        s->warm_ms = in->now_ms;
    } else if (s->warm_ma < CP_IBAT_MAX_MA && in->t_warm_c <= CP_TBAT_WARM - CP_TBAT_HYS &&
               in->now_ms >= s->warm_ms + (s->warm_ma == CP_REDUCED_MA ? CP_HOLD_MS : CP_RAMP_MS)) {
        s->warm_ma = umin(s->warm_ma + CP_RAMP_MA, CP_IBAT_MAX_MA);
        s->warm_ms = in->now_ms;
    }
    ma = umin(ma, s->warm_ma);
    if (in->t_cool_c < CP_TBAT_FULL) ma = umin(ma, CP_REDUCED_MA);
    return ma;
}
