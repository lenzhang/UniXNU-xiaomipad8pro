// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WCN7860 PMU's six rails, shared by the chip's two radios, and the lock that orders their power changes: one helper that both
// PianoWifi (WLAN_EN, PCIe0) and PianoBluetooth (BT_EN, the UART) run, so that neither radio's driver withdraws a rail the other
// still needs and a Bluetooth-only boot has rails at all. docs/drivers/bluetooth.md §7.1 is the reference (and §7.2 the change
// PianoWifi makes to use it); this header is its code side. Include it as <PianoWcnRails.h> (tooling/k27/build-native-kext.py puts
// package/include on every kext's include path).
//
// Why: the PMU is one power unit with two enables (the Ubuntu piano kernel's pwrseq-qcom-wcn.c, wcn7860 entry, behaviour only): the
// six rails, then an enable pin high, then 50 ms before that radio answers; the two enable pins are independent. So the rails stay
// up while either radio runs, come up before either enable pin rises, and only the last user may take them down.
//
// The words (32 bits each, defined by PianoA830 in board/PianoA830.cpp next to sm8750_scm_owner, as it owns the DRV-2 and GCC vote
// words: PianoA830 links before both radios, whose descriptors name com.unixnu.driver.PianoA830 as a library, so the KC resolves
// every import to that one copy; the KC linker merges weak definitions only inside one kext, tooling/k27/coalesce-weak.py, so a word
// each user defined for itself would split):
//   piano_wcn_rails_state   bits 7:0 the owners (PIANO_WCN_WLAN, PIANO_WCN_BT); bits 15:8 the rails whose enable this helper added
//                           (they read disabled before its vote; bit 8 + the rail's index); bit 31 the rails are up (voted and read
//                           back). Changed only with piano_wcn_pmu_owner held; read in any context.
//   piano_wcn_pmu_owner     0 free, else the holder's tag (PianoWcnTagWifi "WIFI", PianoWcnTagBt "BTH1").
//
// The protocol (this file implements it; nothing else writes the words):
//   lock    compare-exchange 0 -> tag. A kext already holding it only counts one more hold (its get inside its own enable-pin
//           section); the count is the kext's, under its hold mutex. Waiting sleeps 1 ms a try, is reported at the first wait and
//           every 5 s, and is bounded (bound_ms, default 30 s: six rail transactions behind a DRV-2 lock held by others take at most
//           about 13 s); past the bound the caller gets PianoWcnBusy and does nothing (no take-over: a radio that stays off is the
//           safe outcome).
//   get     under the lock. Rails up (bit 31): every voted rail is read back over SPMI and must read enabled; then the client's bit is
//           added and nothing is sent. Otherwise the vote, the rules of PianoWifi's level 2 (steps 201..203): every rail and the two
//           PHY LDOs read (a voted rail that cannot be read refuses the vote unless `blind`); the six rails in piano's DT supply
//           order, one VRM transaction each: mode NPM, a voltage (the DT minimum, or what the PMIC reports when that is higher and
//           within the DT maximum; no voltage command above the maximum) and the enable; 5 ms; every rail read again: none lower,
//           none that was enabled now disabled, the PHY LDOs (one is the UFS PHY's PLL) never voted. Success sets bit 31, the
//           added-enable bits and the client's bit; a failure adds no owner bit and, when no owner was there, withdraws the enables
//           this attempt added. Idempotent per client.
//   put     under the lock: the client's bit dropped. When no owner is left and the caller passes PIANO_WCN_RELEASE, the enables this
//           helper added are withdrawn (enable 0 only; mode and voltage stay) and bit 31 cleared; otherwise the rails stay voted
//           (PianoWifi's rule: by default nothing is ever withdrawn). Returns the owners left.
//   owners  the word: owner bits and bit 31. Any context.
//   rules   rails before any enable pin rises; raising an enable pin and every rail change happen with the lock held (lowering one's
//           own pin needs none: it never hurts the other radio); a radio never touches the other's pin; SW_CTRL is read by both.
// A kext serialises its own threads around one PianoWcnRails (PianoBluetooth: its bring-up lock); the hold mutex only keeps the
// nesting count consistent.
#ifndef PIANO_WCN_RAILS_H
#define PIANO_WCN_RAILS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PIANO_WCN_WLAN = 1u << 0,
    PIANO_WCN_BT = 1u << 1,
    PIANO_WCN_CLIENTS = 0xffu,
    PIANO_WCN_RELEASE = 1u << 8,              // put(): when no owner is left, withdraw the enables this helper added
    PIANO_WCN_ADDED_SHIFT = 8,
    PIANO_WCN_ADDED = 0xff00u,
};
#define PIANO_WCN_UP 0x80000000u

enum {
    PianoWcnTagWifi = 0x57494649u,            // "WIFI" (PianoWifi's tag in its other lock words too)
    PianoWcnTagBt = 0x42544831u,              // "BTH1"
    PianoWcnDefaultBoundMs = 30000,
    PianoWcnReportMs = 5000,
    PianoWcnSettleMs = 5,
};

// Answers: 0, or one of these (PianoWifi's level 2 numbers where they mean the same: its errorName() names them).
enum {
    PianoWcnOk = 0,
    PianoWcnArgument = -2,
    PianoWcnRailRead = -4,                    // a voted rail's PMIC state could not be read (and not blind)
    PianoWcnRailVote = -5,                    // a VRM transaction was refused or did not complete
    PianoWcnRailDropped = -6,                 // a rail read back lower, or disabled after it was enabled
    PianoWcnBusy = -16,                       // the PMU lock stayed held past the bound
};
// Where get/put stopped (PianoWcnRails::step; negative where it failed), PianoWifi's numbers for the same steps.
enum { PianoWcnStepRead = 201, PianoWcnStepVote = 202, PianoWcnStepCheck = 203, PianoWcnStepVerify = 211, PianoWcnStepRelease = 221 };

// The six PMU rails and the PHY's two LDOs, in piano's DT order (wcn7860-pmu's vdd*-supply; the regulators' cmd-db names and the
// DT's voltage ranges): the same facts as package/PianoWifi/WifiHw.h kRails, from the running Ubuntu DT (out/inputs/piano-boot.dtb)
// and the Ubuntu kernel's sm8750-xiaomi-piano.dts. `order` 1..6 is the vote order; 0: read only, never voted (vdda-phy is the UFS
// refgen's and USB's, vdda-pll the UFS and DSI PHYs' PLL). PPIDs: SMPS n at page 0x9b + 3 (n - 1); LDO n at 0xc0 + n, else
// 0x40 + n - 1 (the SM8550 page rule PianoWifi's survey found necessary to try), checked by the peripheral's TYPE.
typedef struct PianoWcnRail {
    const char *supply, *regulator, *cmd_db;
    uint8_t sid, smps, ldo, order;
    uint16_t min_mv, max_mv;
} PianoWcnRail;
enum { PIANO_WCN_RAIL_COUNT = 8, PIANO_WCN_VOTED_RAILS = 6 };
static const PianoWcnRail piano_wcn_rail_table[PIANO_WCN_RAIL_COUNT] = {
    {"pmu vddio", "vreg_l3f_1p8", "ldof3", 5, 0, 3, 1, 1800, 1800},
    {"pmu vddio1p2", "vreg_l2f_1p2", "ldof2", 5, 0, 2, 2, 1200, 1200},
    {"pmu vddaon", "vreg_s5f_0p88", "smpf5", 5, 5, 0, 3, 876, 1000},
    {"pmu vdddig", "vreg_s4d_0p88", "smpd4", 3, 4, 0, 4, 876, 1036},
    {"pmu vddrfa1p2", "vreg_s7i_1p31", "smpi7", 8, 7, 0, 5, 1312, 1340},
    {"pmu vddrfa1p8", "vreg_s3g_1p86", "smpg3", 6, 3, 0, 6, 1864, 2000},
    {"phy vdda-phy", "vreg_l1f_0p88", "ldof1", 5, 0, 1, 0, 880, 920},
    {"phy vdda-pll", "vreg_l3g_1p2", "ldog3", 6, 0, 3, 0, 1200, 1256},
};
static inline uint8_t piano_wcn_smps_page(unsigned n) { return (uint8_t)(0x9b + 3 * (n - 1)); }
static inline uint8_t piano_wcn_ldo_page(unsigned n) { return (uint8_t)(0xc0 + n); }
static inline uint8_t piano_wcn_ldo_page_alt(unsigned n) { return (uint8_t)(0x40 + n - 1); }
static inline int piano_wcn_type_is_ldo(uint8_t type) { return type == 0x04; }
static inline int piano_wcn_type_is_smps(uint8_t type) { return type == 0x03 || type == 0x1c || type == 0x21 || type == 0x22; }

// A VRM resource: the set point in mV at +0, the enable at +4, the mode at +8 (7 = NPM). Same layout as PianoWifi's VrmCmd.
typedef struct PianoWcnVrmCmd { uint32_t reg, value; } PianoWcnVrmCmd;
enum { PianoWcnVrmVoltage = 0, PianoWcnVrmEnable = 4, PianoWcnVrmMode = 8, PianoWcnVrmModeNpm = 7 };

// What the report hook is told: (event, a, b).
enum {
    PianoWcnLockWaiting = 1,                  // a = the holder's tag
    PianoWcnLockStillWaiting = 2,             // a = the holder, b = ms waited
    PianoWcnLockGot = 3,                      // a = the former holder, b = ms waited
    PianoWcnLockBusy = 4,                     // a = the holder, b = ms waited: given up
    PianoWcnLockLost = 5,                     // a = the tag found when giving back
    PianoWcnLockUnbalanced = 6,
    PianoWcnRailsVerified = 10,               // a = the state word after
    PianoWcnRailsVoted = 11,                  // a = the state word after, b = the rails voted with a voltage
    PianoWcnRailsRevote = 12,                 // a = the rail that did not read back enabled
    PianoWcnRailsFailed = 13,                 // a = the step (negative), b = the error
    PianoWcnRailsPut = 14,                    // a = the state word after, b = the enables withdrawn (rail bits)
    PianoWcnRailsPutUnbalanced = 15,          // a = the client, b = the state word
};

typedef struct PianoWcnRailsOps {
    void *context;
    // One VRM transaction of `count` (1..3) commands for the resource called `resource` in cmd-db, on apps DRV-2 under the DRV-2
    // lock. 0, or negative.
    int (*vrm)(void *context, const char *resource, const PianoWcnVrmCmd *cmds, unsigned count);
    // One regulator over the SPMI observer channel (reads only): STATUS1 (0x08), the set point (0x40/0x41, mV), MODE (0x45), EN_CTL
    // (0x46). Exactly PianoWifi's Hw::pmic. 0, or negative.
    int (*pmic)(void *context, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts);
    // Optional: a peripheral's TYPE (0x04), to find the PPID of a rail the caller left 0. 0, or negative (not in the arbiter's map).
    int (*pmic_type)(void *context, uint16_t ppid, uint8_t *type);
    void (*sleep_ms)(void *context, uint32_t ms);            // blocks (thread context)
    void (*mutex_lock)(void *context);                        // the kext's hold mutex
    void (*mutex_unlock)(void *context);
    void (*report)(void *context, uint32_t event, uint32_t a, uint32_t b);   // optional: one line each
} PianoWcnRailsOps;

typedef struct PianoWcnRailRecord {
    uint16_t ppid;                            // 0: no regulator found
    int rc;                                   // of its VRM transaction (0: none sent, or done)
    uint8_t read, read_after, status, enable, mode, status_after, enable_after, mode_after;
    uint16_t millivolts, millivolts_after, voted;   // voted: the set point sent (0: none)
    uint8_t sent_enable, kept_voltage, added;
} PianoWcnRailRecord;

typedef struct PianoWcnRails {
    volatile uint32_t *state;                 // &piano_wcn_rails_state (host tests: words of their own)
    volatile uint32_t *lock;                  // &piano_wcn_pmu_owner
    uint32_t tag;                             // this kext's: nonzero, its own
    uint32_t bound_ms;                        // 0: PianoWcnDefaultBoundMs
    const PianoWcnRailsOps *ops;
    int blind;                                // vote a rail whose state could not be read (PianoWifi's piano-wifi-blind)
    uint16_t ppid[PIANO_WCN_RAIL_COUNT];      // the caller's survey; 0: found here with ops->pmic_type
    // The last get/put, as this kext saw it:
    PianoWcnRailRecord rail[PIANO_WCN_RAIL_COUNT];
    int step, error;
    unsigned voted, kept;                     // rails sent with / without a voltage
    uint32_t withdrawn;                       // the rail bits a put withdrew
    // Under the hold mutex:
    uint32_t holds;
    uint32_t takes, contended, busy, lost, unbalanced, max_wait_ms;
    // Counts: gets answered by a read-back alone, gets that voted, puts, puts of a client that was not an owner.
    uint32_t verifies, votes, puts, put_unbalanced;
} PianoWcnRails;

static inline int piano_wcn_valid(const PianoWcnRails *r)
{
    return r && r->state && r->lock && r->tag && r->ops && r->ops->vrm && r->ops->pmic && r->ops->sleep_ms && r->ops->mutex_lock &&
           r->ops->mutex_unlock;
}
static inline void piano_wcn_report(const PianoWcnRails *r, uint32_t event, uint32_t a, uint32_t b)
{
    if (r->ops->report) r->ops->report(r->ops->context, event, a, b);
}
static inline uint32_t piano_wcn_load(volatile uint32_t *w) { return __atomic_load_n(w, __ATOMIC_ACQUIRE); }
static inline void piano_wcn_store(volatile uint32_t *w, uint32_t v) { __atomic_store_n(w, v, __ATOMIC_RELEASE); }

// The owner bits and bit 31 (the rails are up). Any context.
static inline uint32_t piano_wcn_rails_owners(const PianoWcnRails *r)
{
    return r && r->state ? piano_wcn_load(r->state) & (PIANO_WCN_CLIENTS | PIANO_WCN_UP) : 0;
}

// ------------------------------------------------------------------------------------------------------------- the PMU lock
// take = 1: take (nested holds of this kext only count); 0: give one hold back. 0, PianoWcnBusy (past the bound: not taken) or
// PianoWcnArgument.
static inline int piano_wcn_pmu_lock(PianoWcnRails *r, int take)
{
    if (!piano_wcn_valid(r)) return PianoWcnArgument;
    r->ops->mutex_lock(r->ops->context);
    if (!take) {
        if (!r->holds) {
            r->unbalanced++;
            piano_wcn_report(r, PianoWcnLockUnbalanced, piano_wcn_load(r->lock), 0);
        } else if (!--r->holds) {
            uint32_t expected = r->tag;
            if (!__atomic_compare_exchange_n(r->lock, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                r->lost++;
                piano_wcn_report(r, PianoWcnLockLost, expected, 0);
            }
        }
        r->ops->mutex_unlock(r->ops->context);
        return PianoWcnOk;
    }
    if (r->holds) {
        r->holds++;
        r->ops->mutex_unlock(r->ops->context);
        return PianoWcnOk;
    }
    const uint32_t bound = r->bound_ms ? r->bound_ms : (uint32_t)PianoWcnDefaultBoundMs;
    uint32_t waited = 0, other = 0;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(r->lock, &expected, r->tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (expected == r->tag) break;   // ours already (a hold count lost by a restart of this kext): keep it
        other = expected;
        if (waited >= bound) {
            r->busy++;
            piano_wcn_report(r, PianoWcnLockBusy, other, waited);
            r->ops->mutex_unlock(r->ops->context);
            return PianoWcnBusy;
        }
        if (!waited) {
            r->contended++;
            piano_wcn_report(r, PianoWcnLockWaiting, other, 0);
        } else if (waited % PianoWcnReportMs == 0) {
            piano_wcn_report(r, PianoWcnLockStillWaiting, other, waited);
        }
        r->ops->sleep_ms(r->ops->context, 1);
        waited++;
    }
    if (waited) piano_wcn_report(r, PianoWcnLockGot, other, waited);
    if (waited > r->max_wait_ms) r->max_wait_ms = waited;
    r->holds = 1;
    r->takes++;
    r->ops->mutex_unlock(r->ops->context);
    return PianoWcnOk;
}

// ------------------------------------------------------------------------------------------------------------- the rails
// The PPID of rail i: the caller's, else found by the page rule when the caller gave ops->pmic_type.
static inline uint16_t piano_wcn_rail_ppid(PianoWcnRails *r, unsigned i)
{
    const PianoWcnRail *t = &piano_wcn_rail_table[i];
    uint8_t type = 0;
    if (r->ppid[i] || !r->ops->pmic_type) return r->ppid[i];
    if (t->smps) {
        const uint16_t p = (uint16_t)(t->sid << 8 | piano_wcn_smps_page(t->smps));
        if (!r->ops->pmic_type(r->ops->context, p, &type) && piano_wcn_type_is_smps(type)) r->ppid[i] = p;
    } else {
        const uint16_t p = (uint16_t)(t->sid << 8 | piano_wcn_ldo_page(t->ldo)), q = (uint16_t)(t->sid << 8 | piano_wcn_ldo_page_alt(t->ldo));
        if (!r->ops->pmic_type(r->ops->context, p, &type) && piano_wcn_type_is_ldo(type)) r->ppid[i] = p;
        else if (!r->ops->pmic_type(r->ops->context, q, &type) && piano_wcn_type_is_ldo(type)) r->ppid[i] = q;
    }
    return r->ppid[i];
}

static inline void piano_wcn_rail_read(PianoWcnRails *r, unsigned i, int after)
{
    PianoWcnRailRecord *x = &r->rail[i];
    uint8_t status = 0, enable = 0, mode = 0;
    uint16_t mv = 0;
    x->ppid = piano_wcn_rail_ppid(r, i);
    const int ok = x->ppid && !r->ops->pmic(r->ops->context, x->ppid, &status, &enable, &mode, &mv);
    if (after) {
        x->read_after = (uint8_t)ok;
        x->status_after = status;
        x->enable_after = enable;
        x->mode_after = mode;
        x->millivolts_after = mv;
    } else {
        x->read = (uint8_t)ok;
        x->status = status;
        x->enable = enable;
        x->mode = mode;
        x->millivolts = mv;
    }
}

// Every voted rail reads back enabled: the rails another driver (or an earlier get) brought up still hold.
static inline int piano_wcn_rails_verify(PianoWcnRails *r, unsigned *bad)
{
    for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i) {
        piano_wcn_rail_read(r, i, 0);
        r->rail[i].read_after = 0;
        if (!piano_wcn_rail_table[i].order) continue;
        if (!r->rail[i].read || !(r->rail[i].enable & 0x80u)) {
            *bad = i;
            return 0;
        }
    }
    return 1;
}

// Withdraws the enable of every rail in `mask` (rail bits); the bits whose transaction went through are returned.
static inline uint32_t piano_wcn_rails_withdraw(PianoWcnRails *r, uint32_t mask)
{
    uint32_t done = 0;
    for (unsigned i = 0; i < PIANO_WCN_VOTED_RAILS; ++i) {
        if (!(mask & (1u << i))) continue;
        const PianoWcnVrmCmd off = {PianoWcnVrmEnable, 0};
        r->rail[i].rc = r->ops->vrm(r->ops->context, piano_wcn_rail_table[i].cmd_db, &off, 1);
        if (!r->rail[i].rc) done |= 1u << i;
    }
    return done;
}

// Steps 201..203. *added gets the rail bits whose enable this vote added. 0, or the error (r->step negative where it failed).
static inline int piano_wcn_rails_vote(PianoWcnRails *r, uint32_t *added)
{
    *added = 0;
    r->voted = r->kept = 0;
    r->step = PianoWcnStepRead;
    for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i) {
        PianoWcnRailRecord *x = &r->rail[i];
        x->rc = 0;
        x->voted = 0;
        x->sent_enable = x->kept_voltage = x->added = 0;
        x->read_after = 0;
        piano_wcn_rail_read(r, i, 0);
    }
    for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i)
        if (piano_wcn_rail_table[i].order && !r->rail[i].read && !r->blind) {
            r->step = -PianoWcnStepRead;
            return r->error = PianoWcnRailRead;
        }
    r->step = PianoWcnStepVote;
    for (unsigned order = 1; order <= PIANO_WCN_VOTED_RAILS; ++order) {
        unsigned i = 0;
        while (i < PIANO_WCN_RAIL_COUNT && piano_wcn_rail_table[i].order != order) ++i;
        if (i == PIANO_WCN_RAIL_COUNT) continue;
        const PianoWcnRail *t = &piano_wcn_rail_table[i];
        PianoWcnRailRecord *x = &r->rail[i];
        uint32_t mv = t->min_mv;
        int voltage = 1;
        if (x->read && x->millivolts > mv) {
            if (x->millivolts <= t->max_mv) mv = x->millivolts;   // keep what it has
            else voltage = 0;                                     // above the range: not ours to lower, and not a value to send
        }
        PianoWcnVrmCmd cmds[3];
        unsigned n = 0;
        cmds[n].reg = PianoWcnVrmMode;
        cmds[n++].value = PianoWcnVrmModeNpm;
        if (voltage) {
            cmds[n].reg = PianoWcnVrmVoltage;
            cmds[n++].value = mv;
        }
        cmds[n].reg = PianoWcnVrmEnable;
        cmds[n++].value = 1;
        x->rc = r->ops->vrm(r->ops->context, t->cmd_db, cmds, n);
        x->voted = voltage ? (uint16_t)mv : 0;
        x->kept_voltage = (uint8_t)!voltage;
        x->sent_enable = (uint8_t)(x->rc == 0);
        x->added = (uint8_t)(x->rc == 0 && x->read && !(x->enable & 0x80u));
        if (x->added) *added |= 1u << i;
        if (voltage) r->voted++;
        else r->kept++;
        if (x->rc) {
            r->step = -PianoWcnStepVote;
            return r->error = PianoWcnRailVote;
        }
    }
    r->ops->sleep_ms(r->ops->context, PianoWcnSettleMs);
    r->step = PianoWcnStepCheck;
    for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i) piano_wcn_rail_read(r, i, 1);
    for (unsigned i = 0; i < PIANO_WCN_RAIL_COUNT; ++i) {
        const PianoWcnRailRecord *x = &r->rail[i];
        if (!x->read || !x->read_after) continue;
        if (x->millivolts_after < x->millivolts || ((x->enable & 0x80u) && !(x->enable_after & 0x80u))) {
            r->step = -PianoWcnStepCheck;
            return r->error = PianoWcnRailDropped;
        }
    }
    return PianoWcnOk;
}

// The client's rails: up, voted or verified, with the client an owner. 0, or an error (nothing of the state changed, no owner bit).
static inline int piano_wcn_rails_get(PianoWcnRails *r, unsigned client)
{
    if (!piano_wcn_valid(r) || !client || (client & ~(unsigned)PIANO_WCN_CLIENTS)) return PianoWcnArgument;
    r->step = 0;
    r->error = 0;
    int rc = piano_wcn_pmu_lock(r, 1);
    if (rc) {
        r->error = rc;
        return rc;
    }
    uint32_t s = piano_wcn_load(r->state);
    if (s & PIANO_WCN_UP) {
        unsigned bad = 0;
        r->step = PianoWcnStepVerify;
        if (piano_wcn_rails_verify(r, &bad)) {
            s |= client;
            piano_wcn_store(r->state, s);
            r->verifies++;
            piano_wcn_report(r, PianoWcnRailsVerified, s, 0);
            piano_wcn_pmu_lock(r, 0);
            return PianoWcnOk;
        }
        piano_wcn_report(r, PianoWcnRailsRevote, bad, s);
    }
    uint32_t added = 0;
    rc = piano_wcn_rails_vote(r, &added);
    if (rc) {
        // Back to what was found: the enables this attempt added are withdrawn unless an owner still runs on the rails (then they
        // are recorded, for the last owner's release). The rails are no longer known to be up either way.
        if (!(s & PIANO_WCN_CLIENTS)) {
            if (added) piano_wcn_rails_withdraw(r, added);
        } else {
            s |= added << PIANO_WCN_ADDED_SHIFT;
        }
        piano_wcn_store(r->state, s & ~PIANO_WCN_UP);
        piano_wcn_report(r, PianoWcnRailsFailed, (uint32_t)r->step, (uint32_t)rc);
        piano_wcn_pmu_lock(r, 0);
        return rc;
    }
    s |= client | PIANO_WCN_UP | (added << PIANO_WCN_ADDED_SHIFT);
    piano_wcn_store(r->state, s);
    r->votes++;
    piano_wcn_report(r, PianoWcnRailsVoted, s, r->voted);
    piano_wcn_pmu_lock(r, 0);
    return PianoWcnOk;
}

// Drops the client's ownership (flags: the client's bit, PIANO_WCN_RELEASE). Returns the owners left. Without the lock (busy past
// the bound) only the owner bit is dropped, atomically, and nothing is withdrawn.
static inline unsigned piano_wcn_rails_put(PianoWcnRails *r, unsigned flags)
{
    const unsigned client = flags & PIANO_WCN_CLIENTS;
    if (!piano_wcn_valid(r) || !client) return 0;
    r->withdrawn = 0;
    r->puts++;
    if (piano_wcn_pmu_lock(r, 1)) {
        const uint32_t s = __atomic_and_fetch(r->state, ~(uint32_t)client, __ATOMIC_ACQ_REL);
        return s & PIANO_WCN_CLIENTS;
    }
    uint32_t s = piano_wcn_load(r->state);
    if (!(s & client)) {
        r->put_unbalanced++;
        piano_wcn_report(r, PianoWcnRailsPutUnbalanced, client, s);
    }
    s &= ~(uint32_t)client;
    if (!(s & PIANO_WCN_CLIENTS) && (flags & PIANO_WCN_RELEASE) && (s & PIANO_WCN_ADDED)) {
        r->step = PianoWcnStepRelease;
        const uint32_t mask = (s & PIANO_WCN_ADDED) >> PIANO_WCN_ADDED_SHIFT;
        r->withdrawn = piano_wcn_rails_withdraw(r, mask);
        // The rails are no longer known to be up: a later get votes again (and finds which ones still are). A withdrawal that did
        // not go through keeps its bit, for the next release.
        s &= ~(r->withdrawn << PIANO_WCN_ADDED_SHIFT);
        if (r->withdrawn) s &= ~PIANO_WCN_UP;
        if (r->withdrawn != mask) r->step = -PianoWcnStepRelease;
    }
    piano_wcn_store(r->state, s);
    piano_wcn_report(r, PianoWcnRailsPut, s, r->withdrawn);
    piano_wcn_pmu_lock(r, 0);
    return s & PIANO_WCN_CLIENTS;
}

#ifdef __cplusplus
}
#endif

// ----------------------------------------------------------------------------------------------------- the kernel side (C++ kexts)
// A kext defines PIANO_WCN_RAILS_KERNEL before including this file, declares PIANO_WCN_KERNEL_RAILS(name, tag, "Label", vrm, pmic,
// pmic_type) and calls piano_wcn_rails_get(&name.rails, ...) and the others on &name.rails. Its vrm/pmic/pmic_type get &name as their
// context (PianoWifi's hwVrm/hwPmic ignore theirs). The hold mutex is an IOLock made on first use; the report hook writes one IOLog
// line per event ("Label: wcn rails ..."). The words are PianoA830's.
#if defined(PIANO_WCN_RAILS_KERNEL) && defined(__cplusplus)
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

extern "C" volatile uint32_t piano_wcn_rails_state;   // PianoA830's (board/PianoA830.cpp)
extern "C" volatile uint32_t piano_wcn_pmu_owner;

struct PianoWcnKernelRails {
    PianoWcnRails rails;
    PianoWcnRailsOps ops;
    IOLock *mutex;
    const char *label;
};

static inline IOLock *piano_wcn_kernel_mutex(PianoWcnKernelRails *k)
{
    IOLock *m = __atomic_load_n(&k->mutex, __ATOMIC_ACQUIRE);
    if (m) return m;
    IOLock *made = IOLockAlloc(), *expected = nullptr;
    if (__atomic_compare_exchange_n(&k->mutex, &expected, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return made;
    if (made) IOLockFree(made);
    return expected;
}
static inline PianoWcnKernelRails *piano_wcn_kernel_of(void *c) { return static_cast<PianoWcnKernelRails *>(c); }
static inline void piano_wcn_kernel_sleep(void *, uint32_t ms) { IOSleep(ms); }
static inline void piano_wcn_kernel_mutex_lock(void *c) { IOLockLock(piano_wcn_kernel_mutex(piano_wcn_kernel_of(c))); }
static inline void piano_wcn_kernel_mutex_unlock(void *c) { IOLockUnlock(piano_wcn_kernel_of(c)->mutex); }
static inline void piano_wcn_kernel_report(void *c, uint32_t event, uint32_t a, uint32_t b)
{
    const PianoWcnKernelRails *k = piano_wcn_kernel_of(c);
    char line[200];
    switch (event) {
    case PianoWcnLockWaiting: snprintf(line, sizeof line, "PMU lock held by %#x: waiting", a); break;
    case PianoWcnLockStillWaiting: snprintf(line, sizeof line, "PMU lock still held by %#x after %u ms", a, b); break;
    case PianoWcnLockGot: snprintf(line, sizeof line, "PMU lock free again (it was %#x's), taken after %u ms", a, b); break;
    case PianoWcnLockBusy: snprintf(line, sizeof line, "PMU lock held by %#x for %u ms: given up, nothing changed", a, b); break;
    case PianoWcnLockLost: snprintf(line, sizeof line, "PMU lock given back, but %#x held it", a); break;
    case PianoWcnLockUnbalanced: snprintf(line, sizeof line, "PMU lock given back without a hold (owner %#x)", a); break;
    case PianoWcnRailsVerified: snprintf(line, sizeof line, "the six rails were up and read back enabled: owners now %#x", a); break;
    case PianoWcnRailsVoted: snprintf(line, sizeof line, "the six rails voted and read back (%u with a voltage): state %#x", b, a); break;
    case PianoWcnRailsRevote: snprintf(line, sizeof line, "rail %u did not read back enabled (state %#x): voting again", a, b); break;
    case PianoWcnRailsFailed: snprintf(line, sizeof line, "the rails FAILED at step %d (error %d): no owner added", (int)a, (int)b); break;
    case PianoWcnRailsPut: snprintf(line, sizeof line, "put: state %#x, enables withdrawn %#x", a, b); break;
    default: snprintf(line, sizeof line, "put of %#x, which was not an owner (state %#x)", a, b); break;
    }
    IOLog("%s: wcn rails (ours %#x): %s\n", k->label, k->rails.tag, line);
}
#define PIANO_WCN_KERNEL_RAILS(name, tag, label, vrm, pmic, pmic_type)                                                           \
    PianoWcnKernelRails name = {{&piano_wcn_rails_state, &piano_wcn_pmu_owner, (tag), 0, &name.ops, 0, {}, {}, 0, 0, 0, 0, 0, 0,  \
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},                                                                   \
                                {&name, (vrm), (pmic), (pmic_type), piano_wcn_kernel_sleep, piano_wcn_kernel_mutex_lock,            \
                                 piano_wcn_kernel_mutex_unlock, piano_wcn_kernel_report},                                         \
                                nullptr, (label)}
#endif

#endif
