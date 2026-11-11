/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The charging policy for piano's charger firmware (Xiaomi's MCA properties, served by the audio DSP; PianoMca.h is the codec), as a
 * pure library: every decision is a function of (the configuration, one snapshot of the charger, the gauges and the port, the
 * policy's own state) -> (the new state, the writes to make, the level of the charging-hold assertion). No I/O, no clock, no
 * allocation, no floating point, nothing from libc: the owner (PianoPower) fills a PianoChargeInput after each charger cycle (and on
 * a notification, at a sleep entry, after a wake), calls piano_charge_decide(), sends the planned writes through its MCA codec in
 * the order given, and reports every answer with piano_charge_ack(). Design, the tables and their sources:
 * docs/drivers/charging-policy.md.
 *
 * The values are Ubuntu's (docs/ubuntu-history/device.md §2.5, kernel.md §2.5); the GPL xiaomi_mca driver was read as a
 * specification only (behaviour and numbers), nothing of it is copied. What the policy covers:
 *   - the input limit and charge current by source type at 5 V, 9 V from a fixed PDO (opt-in), 9 V from a PPS APDO (opt-in), the
 *     charge current derated by temperature (500 mA below 15 degC, -300 mA per degC above 38 degC, raised one step per 30 s);
 *   - the float voltage (opt-in): 4480 mV, 4060 mV from 48 degC until below 46 degC, read back from 0x2007 (2 mV - 3600);
 *   - Preserve Battery Health (opt-in): stop at 80 %, resume at 75 % (the higher pack's charge), the charging-hold assertion while
 *     protected charging runs below the limit, the stop read back (0x2003, and the gauges' current);
 *   - the sleep handover (the source type's 5 V limits, the source back at 5 V, a protected charge stopped for the sleep), the
 *     re-send of everything after a wake or a new charger generation, a re-sync when the firmware loses a source the PMIC still
 *     sees (Ubuntu issue #18), the firmware's own values back on detach (100 mA input), and a stop that never outlives the driver;
 *   - the board thermistors PianoThermal publishes, only ever making charging more conservative: the battery's joins the pack
 *     temperatures; a connector at or above 60 degC holds the input at 100 mA until both read below 50; the charger at or above
 *     85 degC holds it at 500 mA at most until below 75;
 *   - the Type-C Rp current as the 5 V input limit of a source without a PD contract (opt-in, cfg.rp), within the ceiling;
 *   - the direct-charging (SC8541) battery-current curve of an authenticated original adapter, as a specification for a later
 *     charge-pump driver (piano_charge_cp_ibat_ma; nothing calls it yet).
 * Every write passes piano_charge_write_allowed() before it is planned; the owner's own allowlist must admit the same set
 * (docs/drivers/charging-policy.md §7). */
#ifndef PIANO_CHARGE_POLICY_H
#define PIANO_CHARGE_POLICY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The properties the policy writes (the same ids as PianoMca.h; repeated so that this header stands alone). */
#define PIANO_CHG_ENABLE 0x2003u     /* 1 byte: charging enabled (0 only from the battery-health stop and the sleep stop) */
#define PIANO_CHG_FCC 0x2005u        /* charge current, mA (reads back 0: the acknowledgement is the only confirmation) */
#define PIANO_CHG_FV 0x2007u         /* float voltage: written in mV, reads 2 x mV - 3600 */
#define PIANO_CHG_ICL 0x2008u        /* input current limit, mA (reads back the requested value, not the one in force) */
#define PIANO_CHG_PPS 0x21017u       /* PPS APDO request: (mV / 20) << 16 | mA / 50 */
#define PIANO_CHG_PD_FIXED 0x21018u  /* fixed PDO request, mV */

/* Charger types as the firmware reports them (0x2000c; xiaomi_mca's enum, kernel.md §2.5). */
enum {
    PIANO_CHG_T_UNKNOWN = 0, PIANO_CHG_T_SDP = 1, PIANO_CHG_T_CDP = 2, PIANO_CHG_T_DCP = 3, PIANO_CHG_T_FLOAT = 4,
    PIANO_CHG_T_HVDCP = 5, PIANO_CHG_T_HVDCP3 = 6, PIANO_CHG_T_HVDCP3_B = 7, PIANO_CHG_T_HVDCP3P5 = 8, PIANO_CHG_T_TYPEC = 9,
    PIANO_CHG_T_PD = 10, PIANO_CHG_T_PD_VERIFY = 11, PIANO_CHG_T_PPS = 12, PIANO_CHG_T_OCP = 15,
};

/* What the policy makes of a type. */
enum {
    PIANO_CHG_C_NONE = 0,   /* detached */
    PIANO_CHG_C_SDP,        /* USB 2.0 host port */
    PIANO_CHG_C_CDP,        /* charging host port */
    PIANO_CHG_C_DCP,        /* dedicated charger, and QC (HVDCP 2/3/3B/3.5) held at 5 V */
    PIANO_CHG_C_FLOAT,      /* data lines floating, and OCP (type 15) */
    PIANO_CHG_C_TYPEC,      /* Type-C current (Rp) only */
    PIANO_CHG_C_PD,         /* USB PD */
    PIANO_CHG_C_PD_PPS,     /* PD verified (11) or PPS (12): the stock "PD_PPS" row */
    PIANO_CHG_C_REFUSED,    /* unknown or reserved (0, 13, 14, 16 and up): no write at all, the firmware's own values */
};

/* The bounds (mA, mV). */
#define PIANO_CHG_ICL_DEFAULT 100u    /* the firmware's power-on input limit; put back on detach */
#define PIANO_CHG_FCC_DEFAULT 500u    /* the fallback charge current; put back on detach */
#define PIANO_CHG_ICL_ABS_MAX 2500u   /* stock in_pps at 9 V: no configuration goes above */
#define PIANO_CHG_FCC_ABS_MAX 3500u   /* stock chg_pps */
#define PIANO_CHG_ICL_CAP 1600u       /* the default ceilings: what PianoMca's allowlist takes today (docs §7, stage A) */
#define PIANO_CHG_FCC_CAP 1500u
#define PIANO_CHG_FV_DEFAULT 4400u    /* the firmware's own float voltage (reads 5200) */
#define PIANO_CHG_FV_FULL 4480u       /* stock normal float voltage */
#define PIANO_CHG_FV_HOT 4060u        /* from 48 degC */
#define PIANO_CHG_FV_FFC 4540u        /* fast-charge float, only within a direct-charging session (not planned by decide) */

typedef struct {
    uint8_t writes;        /* 0: plan nothing (piano-power-write=0); the 5 V decisions are still made, for the log (9 V never) */
    uint8_t hv;            /* 0 5 V only; 1 also a fixed 9 V PDO; 2 also a PPS APDO covering 9 V (piano-power-hv) */
    uint8_t full_charge;   /* the float voltage of the stock tables (piano-power-fv) */
    uint8_t limit;         /* Preserve Battery Health (piano-power-limit, IORegistry) */
    uint8_t stop_pct, resume_pct;
    uint8_t sleep_charge;  /* a demand sleep below the limit: 0 stops charging for the sleep (default), 1 charges on (the limit
                            * is not watched while the system sleeps) */
    uint16_t icl_cap_ma, fcc_cap_ma;   /* the owner's allowlist ceilings (0: the defaults above) */
    uint8_t rp;            /* trust the Type-C Rp current (piano-power-rp): a source without a PD contract that advertises 1.5 A or
                            * 3.0 A gets that as its 5 V input limit, within icl_cap_ma, instead of its class row; only while a
                            * connector temperature reads and is below 50 degC, and not after the bus sagged under such a limit */
} PianoChargeConfig;

/* writes on, hv off, float off, limit off (80/75 kept for when it is turned on), sleep stop, the stage A ceilings. */
void piano_charge_config_default(PianoChargeConfig *c);
/* Preserve Battery Health: on/off and the thresholds (stop 50..100, resume 40..stop-1; resume 0 = stop - 5). Returns 0, or -1 when
 * the values are refused (the configuration is then unchanged). Stop 100 is the same as off. */
int piano_charge_config_limit(PianoChargeConfig *c, unsigned on, unsigned stop_pct, unsigned resume_pct);

/* valid bits of PianoChargeInput */
enum {
    PIANO_CHG_IN_BUS = 1u << 0, PIANO_CHG_IN_TYPE = 1u << 1, PIANO_CHG_IN_ICL = 1u << 2, PIANO_CHG_IN_ENABLE = 1u << 3,
    PIANO_CHG_IN_FV = 1u << 4, PIANO_CHG_IN_VERIFY = 1u << 5, PIANO_CHG_IN_TBAT = 1u << 6, PIANO_CHG_IN_PDOS = 1u << 7,
    PIANO_CHG_IN_GAUGES = 1u << 8, PIANO_CHG_IN_TYPEC = 1u << 9,
    /* the board thermistors PianoThermal publishes (package/include/PianoBoardTemps.h): each only with its bit; a missing one is
     * unknown, never cool */
    PIANO_CHG_IN_NTC = 1u << 10, PIANO_CHG_IN_CHGR = 1u << 11, PIANO_CHG_IN_CONN1 = 1u << 12, PIANO_CHG_IN_CONN2 = 1u << 13,
};
enum {PIANO_CHG_AWAKE = 0, PIANO_CHG_WILL_SLEEP = 1};

typedef struct {
    uint64_t now_ms;        /* monotonic, ms (it stands still while the system is suspended) */
    uint32_t valid;         /* PIANO_CHG_IN_* */
    uint32_t generation;    /* the charger's generation (PianoMca links): a change means the firmware is on its defaults */
    uint32_t wakes;         /* system wakes so far: a change means everything is sent again */
    uint8_t sleep;          /* PIANO_CHG_WILL_SLEEP while the system goes to sleep (repeat until plan.sleep_ready) */
    /* the charger firmware (PianoMca's last cycle) */
    int32_t bus_mv;         /* 0x20002 / 1000 */
    int32_t type;           /* 0x2000c */
    int32_t icl_ma;         /* 0x2008 */
    int32_t enable;         /* 0x2003 */
    int32_t fv_raw;         /* 0x2007, as read (2 x mV - 3600) */
    int32_t verify;         /* 0x21002: non-zero while an adapter authentication runs */
    int32_t tbat_c;         /* 0x202a */
    uint8_t pdos;           /* 0x21011: {min mV, max mV, max mA}, fixed when min == max */
    uint32_t pdo[7][3];
    /* the two gauges (PIANO_CHG_IN_GAUGES: both packs read) */
    int32_t soc_max_pct, soc_min_pct;   /* the higher and the lower pack's charge */
    int32_t vbat_max_mv;                /* the higher pack's voltage */
    int32_t t_warm_dc, t_cool_dc;       /* the warmest and coolest pack, 0.1 degC */
    int32_t ibat_ma;                    /* both packs, + = charging */
    /* the charger PMIC's Type-C block (PianoPower's level 1 view): a source's Rp is seen, and the current it advertises (0 none,
     * 500 the default, 1500, 3000; ChargerPmic.h rpMilliamps) */
    uint8_t typec_source;
    uint32_t rp_ma;
    /* the board thermistors, 0.1 degC: the battery's (joins the pack temperatures), the charger's, the two connectors' */
    int32_t t_ntc_dc, t_chgr_dc, t_conn1_dc, t_conn2_dc;
} PianoChargeInput;

/* the write slots */
enum {PIANO_CHG_S_ENABLE = 0, PIANO_CHG_S_FCC, PIANO_CHG_S_ICL, PIANO_CHG_S_VOLT, PIANO_CHG_S_FV, PIANO_CHG_SLOTS};
enum {PIANO_CHG_HV_OFF = 0, PIANO_CHG_HV_ASKED, PIANO_CHG_HV_ON, PIANO_CHG_HV_BLOCKED};

typedef struct {
    uint8_t started;
    uint32_t generation, wakes, decisions;
    /* the source */
    uint8_t attached, sleeping, hold_awake;
    int32_t type;                 /* -1: none yet */
    uint64_t attach_ms;
    /* what the firmware took since its generation / the last wake, confirmed by an acknowledgement (0: nothing) */
    uint32_t icl_set, fcc_set, fv_set;
    uint8_t enable_set;           /* 0, 1, or 0xff: not known */
    uint32_t volt_set_prop, volt_set_value;     /* the last voltage request taken */
    uint32_t volt_want_prop, volt_want_value;   /* the voltage request wanted (0: none) */
    uint8_t touched;              /* bit per slot: written and taken since the generation (restored on detach) */
    uint8_t pending;              /* bit per slot: planned, no answer yet */
    uint64_t pending_ms[PIANO_CHG_SLOTS];
    uint32_t want[PIANO_CHG_SLOTS];       /* the value the tries count for */
    uint8_t tries[PIANO_CHG_SLOTS];       /* refusals of that value in a row */
    uint8_t mismatch[PIANO_CHG_SLOTS];    /* read-backs in a row that disagree with what was taken */
    uint8_t rewrites[PIANO_CHG_SLOTS];    /* writes repeated because the read-back disagreed */
    /* the charge current's thermal steps */
    uint32_t fcc_full;
    uint64_t fcc_raise_ms;
    /* 9 V */
    uint8_t hv, hv_pps, hv_renegs, hv_checks;
    uint32_t hv_ma;
    uint64_t hv_asked_ms, hv_retry_ms;
    /* Preserve Battery Health */
    uint8_t hold, hot, cold, disabled, soc_lost, stop_noted, fw_noted;
    uint64_t soc_lost_ms, off_since_ms;
    uint8_t charge_ok;
    /* the float voltage */
    uint8_t term_hot;
    /* the firmware losing a source the PMIC still sees (#18) */
    uint8_t wedge_checks, wedge_resyncs, wedge_reported;
    uint64_t wedge_since_ms;
    uint32_t events;              /* from the acknowledgements, handed to the next plan */
    /* the Type-C Rp current (cfg.rp): distrusted for the boot after a sag or two quick detaches under it */
    uint8_t rp_distrust, rp_strikes, rp_noted;
    uint64_t rp_taken_ms;         /* since when an input limit from the Rp is in force (0: none) */
    /* the board thermistors: the connectors' and the charger's hot holds, the battery thermistor apart from the packs */
    uint8_t conn_hot, chgr_hot, ntc_apart;
} PianoChargeState;

#define PIANO_CHG_MAX_WRITES 6
typedef struct {uint32_t property, value; uint8_t bytes, slot;} PianoChargeWrite;

/* plan events (for the owner's log) */
enum {
    PIANO_CHG_EV_ATTACH = 1u << 0, PIANO_CHG_EV_DETACH = 1u << 1, PIANO_CHG_EV_TYPE = 1u << 2,
    PIANO_CHG_EV_HV_ASKED = 1u << 3, PIANO_CHG_EV_HV_ON = 1u << 4, PIANO_CHG_EV_HV_BLOCKED = 1u << 5, PIANO_CHG_EV_HV_RETRY = 1u << 6,
    PIANO_CHG_EV_LIMIT_STOP = 1u << 7, PIANO_CHG_EV_LIMIT_RESUME = 1u << 8, PIANO_CHG_EV_HOT = 1u << 9, PIANO_CHG_EV_COLD = 1u << 10,
    PIANO_CHG_EV_SOC_LOST = 1u << 11,      /* no charge reading for 5 min: the limit cannot be enforced, charging goes on */
    PIANO_CHG_EV_STOP_LEAK = 1u << 12,     /* stopped for a minute, 0x2003 reads 0, and the packs still take > 100 mA */
    PIANO_CHG_EV_FW_DISABLED = 1u << 13,   /* 0x2003 reads 0 although the policy never wrote 0: the firmware's own (not overridden) */
    PIANO_CHG_EV_GAVE_UP = 1u << 14,       /* a value refused 3 times in a row: left until the next attach, wake or generation */
    PIANO_CHG_EV_READBACK = 1u << 15,      /* a value taken 3 times but read back otherwise: left alone */
    PIANO_CHG_EV_REFUSED = 1u << 16,       /* a computed write outside piano_charge_write_allowed (a bug): not planned */
    PIANO_CHG_EV_GENERATION = 1u << 17, PIANO_CHG_EV_WAKE = 1u << 18,
    PIANO_CHG_EV_WEDGE_RESYNC = 1u << 19,  /* the firmware sees no source, the PMIC does: everything sent again (#18) */
    PIANO_CHG_EV_WEDGED = 1u << 20,        /* still so after two re-syncs: replug or reboot */
    PIANO_CHG_EV_SLEEP = 1u << 21,         /* the sleep handover began */
    PIANO_CHG_EV_RP_DISTRUST = 1u << 22,   /* the bus sagged below 4.5 V under an input limit from the Rp, or the source went twice
                                            * within 30 s of one: the class rows for the rest of the boot */
    PIANO_CHG_EV_RP_WITHHELD = 1u << 23,   /* cfg.rp, a 1.5/3.0 A Rp, and no connector temperature below 50 degC: the class row */
    PIANO_CHG_EV_NTC_APART = 1u << 24,     /* the battery thermistor and the packs more than 10 degC apart (the warmer counts) */
    PIANO_CHG_EV_CONN_HOT = 1u << 25,      /* a connector at or above 60 degC: input limit 100 mA */
    PIANO_CHG_EV_CONN_COOL = 1u << 26,     /* both connectors read below 50 degC again */
    PIANO_CHG_EV_CHGR_HOT = 1u << 27,      /* the charger at or above 85 degC: input limit 500 mA at most */
    PIANO_CHG_EV_CHGR_COOL = 1u << 28,     /* the charger reads below 75 degC again */
};
enum {PIANO_CHG_OFF_NONE = 0, PIANO_CHG_OFF_LIMIT, PIANO_CHG_OFF_HOT, PIANO_CHG_OFF_COLD, PIANO_CHG_OFF_SLEEP};
/* where the input limit came from (plan.icl_why) */
enum {PIANO_CHG_ICL_ROW = 0, PIANO_CHG_ICL_RP, PIANO_CHG_ICL_9V, PIANO_CHG_ICL_CONN, PIANO_CHG_ICL_CHGR};

typedef struct {
    PianoChargeWrite write[PIANO_CHG_MAX_WRITES];   /* in this order */
    uint8_t writes;
    /* what the policy wants now (for the log and the registry) */
    uint8_t cls, hv, charge, why_off;
    uint32_t icl_ma, fcc_ma, fv_mv, bus_mv;
    uint8_t hold_awake;    /* the charging-hold assertion: no idle sleep while protected charging runs below the limit */
    uint8_t sleep_ready;   /* PIANO_CHG_WILL_SLEEP: the handover is complete (or nothing is to be handed over) */
    uint8_t resync;        /* send the board configuration and read everything again (PianoMca relink), as a rebind did (#18) */
    uint32_t events;
    uint8_t icl_why;       /* PIANO_CHG_ICL_* */
    uint32_t rp_ma;        /* the Rp current the input limit follows (0: not from the Rp) */
} PianoChargePlan;

void piano_charge_init(PianoChargeState *s);
void piano_charge_decide(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p);
/* The firmware's answer to a planned write (retcode 0 = taken; the codec's -60 for no answer counts as a refusal). */
void piano_charge_ack(PianoChargeState *s, uint32_t property, uint32_t value, int32_t retcode);
/* The driver stops or the system shuts down: a stop of ours is lifted (0x2003 = 1) and the source put back at 5 V; the limits stay
 * as they are (Ubuntu: handing back 100 mA drained a running tablet). Repeat until plan.sleep_ready, as for a sleep. */
void piano_charge_shutdown(const PianoChargeConfig *c, const PianoChargeInput *in, PianoChargeState *s, PianoChargePlan *p);
int piano_charge_write_allowed(const PianoChargeConfig *c, uint32_t property, uint32_t value, uint8_t bytes);

/* The pieces, each a pure function (host-tested). */
int piano_charge_class(int32_t type);
const char *piano_charge_class_name(int cls);
uint32_t piano_charge_icl_5v(int cls, uint32_t pdo5v_ma);    /* pdo5v_ma: 0 when not known */
/* The 5 V limit with the Type-C current trusted: a class without a PD contract (SDP, CDP, DCP, float, Type-C) whose source
 * advertises 1.5 A or 3.0 A gets min(Rp, cap) when that is above its row (USB Type-C's precedence: a PD contract, then the Type-C
 * current, then BC 1.2, then the default USB power); otherwise the row. */
uint32_t piano_charge_icl_rp(int cls, uint32_t row_ma, uint32_t rp_ma, uint32_t cap_ma);
uint32_t piano_charge_icl_9v(int cls, uint32_t source_ma);   /* 0: no 9 V row for the class */
uint32_t piano_charge_fcc_full(int cls, int hv_on);
uint32_t piano_charge_fcc_thermal(uint32_t full, int t_valid, int32_t t_c);
/* The lowest current among the PDOs and APDOs whose range covers 5 V, 0 when none. */
uint32_t piano_charge_pdo_5v_ma(const uint32_t pdo[][3], unsigned n);
/* A fixed 9 V PDO's current (>= 100 mA), else with allow_pps the largest APDO current covering 9 V (>= 500, capped at 3000) and
 * *pps = 1; 0 when the source offers no 9 V. */
uint32_t piano_charge_pdo_9v_ma(const uint32_t pdo[][3], unsigned n, int allow_pps, int *pps);
uint32_t piano_charge_pps_word(uint32_t mv, uint32_t ma);
/* The float voltage target; *term_hot is the hysteresis (no temperature counts as hot). */
uint32_t piano_charge_fv_target(uint8_t *term_hot, int t_valid, int32_t t_warm_c);
static inline int32_t piano_charge_fv_raw(uint32_t mv) {return 2 * (int32_t)mv - 3600;}
/* The battery-health hysteresis: 1 = stopped. */
int piano_charge_soc_hold(int hold, int32_t soc_pct, unsigned stop_pct, unsigned resume_pct);

/* ---- direct charging (SC8541 2:1 from an authenticated original adapter's PPS APDO): the specification of the curve ----
 * The battery current (both cells) a direct-charging session may target, by the cells' voltage (the stock "normal" step table,
 * or the FFC one below 95 % with the adapter verified, 18-47 degC and the float option on), the temperature (start 15-37 degC,
 * leave from 45 degC or below 15; 4.5 A from 40 degC, held 30 s, then +0.5 A per 15 s once at or below 38 degC; 4.5 A below
 * 18 degC) and the source (2 x its APDO current, 6 A at most, within 67 W; 12 A on the battery side). 0: no session (the buck
 * charger carries on at 9 V). Nothing calls it yet: it needs a charge-pump driver and a 500 ms regulation loop (docs §3.4). */
typedef struct {
    uint64_t now_ms;
    uint8_t t_valid, soc_valid, verified, full_charge, running;
    int32_t vbat_max_mv, vbat_min_mv;   /* the higher and the lower pack */
    int32_t t_cool_c, t_warm_c;
    int32_t soc_max_pct;
    int32_t cell_ma_max;                /* the larger pack current, absolute */
    uint32_t apdo_ma, vbus_mv;          /* the APDO's current; the PPS voltage asked */
} PianoChargeCpInput;
typedef struct {uint8_t profile, stage, warm_valid; uint32_t warm_ma; uint64_t warm_ms;} PianoChargeCpState;
enum {PIANO_CHG_CP_NORMAL = 0, PIANO_CHG_CP_FFC = 1};
uint32_t piano_charge_cp_ibat_ma(const PianoChargeCpInput *in, PianoChargeCpState *s);

#ifdef __cplusplus
}
#endif
#endif
