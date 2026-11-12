// SPDX-License-Identifier: BSD-2-Clause-Patent
// The multimedia votes several drivers share on apps DRV-2: the rails MXC ("mxc.lvl") and MMCX ("mmcx.lvl") and the multimedia
// NoC's BCM MM1 ("MM1"). One RPMh vote per resource per DRV: a DRV-2 vote replaces that DRV's earlier one, so two drivers voting the
// same resource from DRV-2 overwrite each other, and one's release would drop the other's need. This helper reference-counts them:
// every client records the level it needs of each resource, the vote sent is the maximum over the clients (with the display's floor
// for a rail), and only what changes is sent. Include it as <PianoMmVotes.h> (tooling/k27/build-native-kext.py puts package/include on
// every kext's include path). docs/drivers/camera.md §4.3.1 is the reference.
//
// Who votes what (2026-11-12): PianoVideo (V1-V3: MXC svs, MMCX low_svs, MM1 on while the core is powered) and PianoCamera (levels
// 2-3: MXC low_svs for CAMCC, MMCX no need of its own (the boot display's vote covers CAMCC's low_svs) but its floor declared, MM1 on
// for the camera NoC's qnm_camnoc_hf while a level 3 stream runs). The display has no
// driver that votes (the boot framebuffer: ABL left MMCX at NOM_L1, and nothing natively lowers it): its need is the MMCX floor every
// client declares (PianoVideo's piano-video-mmcx-floor, PianoCamera's piano-camera-mmcx-floor, both 0x140 by default); the floor word
// keeps the highest floor any client declared, so no client's vote ever goes below another's floor. PianoA830 votes CX, MX, the DDR
// BCMs (SH0, MC0, ACV) and CN0 from DRV-2, none of these three: it only defines the words. A client slot is reserved for the display
// and one for the GPU, for when either votes one of these resources.
//
// The words (defined by PianoA830 in board/PianoA830.cpp next to sm8750_scm_owner and the WCN rails' words, for the same reason:
// PianoA830 links before every user, whose descriptors name com.unixnu.driver.PianoA830 as a library, so the KC resolves every import
// to that one copy):
//   piano_mm_votes_owner      0 free, else the holder's tag (PianoMmTagVideo "VIDE", PianoMmTagCamera "CAM1": their DRV-2 tags)
//   piano_mm_votes[32]        need[r][c] at 4 r + c: client c's need of resource r (a rail: the vlvl, 0 = none; MM1: the BCM command
//                             word, 0 = none); floor[r] at 12 + r (a rail's floor vlvl, the highest declared); sent[r] at 16 + r (a
//                             rail: the hlvl DRV-2 holds from us; MM1: the word sent) with word 20's bit r saying sent[r] is valid;
//                             21 updates, 22 transactions, 23 the last error, 24 the last client's tag. Changed only with the lock held.
//
// The protocol (this file implements it; nothing else writes the words):
//   lock     compare-exchange 0 -> tag, 1 ms between tries (sleep_ms), at most bound_ms (default 5000); past it the update answers
//            PianoMmBusy and changes nothing. Lock order: this lock, then the DRV-2 lock (taken inside ops->send). Never held across
//            anything but one update.
//   update   for the resources in `mask`: the client's needs replaced; the floor raised to the client's; each resource's vote is the
//            maximum (a rail: max(floor, every need), as its first hlvl reaching it; MM1: valid when any client needs it, x the sum
//            and y the maximum of the needs' x/y, commit set; off = commit only) and is sent when it differs from what was sent, or
//            when nothing was sent yet and some client needs the resource (a resource never voted by us is not voted only to say
//            "nothing": the boot's vote stays until a client needs it). All the changed resources go in one transaction. A failed
//            transaction leaves the client's needs as they were before the call (and sent[] unchanged): the words describe what
//            DRV-2 was last told, successfully.
//   one user the vote is exactly what that client alone would send: its need with its floor, only what changes, nothing for a
//            resource it never needed (the rule PianoVideo's VideoRpmh.h had before this helper).
#ifndef PIANO_MM_VOTES_H
#define PIANO_MM_VOTES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PIANO_MM_MXC = 0,                         // "mxc.lvl" (an ARC: vlvl corners)
    PIANO_MM_MMCX = 1,                        // "mmcx.lvl" (an ARC)
    PIANO_MM_MM1 = 2,                         // "MM1" (a BCM: enable mask BIT(0); qnm_camnoc_*, qnm_video_*, qns_mem_noc_sf)
    PIANO_MM_RESOURCES = 3,
    PIANO_MM_ALL = (1u << PIANO_MM_RESOURCES) - 1,
    PIANO_MM_VIDEO = 0,                       // clients
    PIANO_MM_CAMERA = 1,
    PIANO_MM_DISPLAY = 2,                     // reserved
    PIANO_MM_GPU = 3,                         // reserved
    PIANO_MM_CLIENTS = 4,
    PIANO_MM_WORDS = 32,
};
enum {
    PianoMmTagVideo = 0x56494445u,            // "VIDE" (PianoVideo's DRV-2 tag)
    PianoMmTagCamera = 0x43414d31u,           // "CAM1" (PianoCamera's)
    PianoMmDefaultBoundMs = 5000,
};
// The BCM command word (Linux bcm-voter.c's encoding): bit 30 commit, bit 29 valid, x (average) 27:14, y (peak) 13:0.
#define PIANO_MM_BCM_COMMIT 0x40000000u
#define PIANO_MM_BCM_VALID 0x20000000u
#define PIANO_MM_BCM_ON (PIANO_MM_BCM_COMMIT | PIANO_MM_BCM_VALID | 1u)   // MM1 on: x 0, y 1 (its enable mask)
#define PIANO_MM_BCM_OFF PIANO_MM_BCM_COMMIT                              // valid 0
static inline uint32_t piano_mm_bcm_x(uint32_t w) { return (w >> 14) & 0x3fffu; }
static inline uint32_t piano_mm_bcm_y(uint32_t w) { return w & 0x3fffu; }

// Word indexes.
static inline unsigned piano_mm_need_index(unsigned r, unsigned c) { return 4u * r + c; }
static inline unsigned piano_mm_floor_index(unsigned r) { return 12u + r; }
static inline unsigned piano_mm_sent_index(unsigned r) { return 16u + r; }
enum { PIANO_MM_FLOOR0 = 12, PIANO_MM_SENT0 = 16, PIANO_MM_SENT_VALID = 20, PIANO_MM_UPDATES = 21, PIANO_MM_TRANSACTIONS = 22,
       PIANO_MM_LAST_ERROR = 23, PIANO_MM_LAST_TAG = 24 };

enum {
    PianoMmOk = 0,
    PianoMmArgument = -2,
    PianoMmNoLevel = -3,                      // no level of the rail's cmd-db table reaches the need (nothing sent)
    PianoMmNoResource = -4,                   // the resource has no cmd-db address (nothing sent)
    PianoMmSend = -5,                         // the transaction was refused or did not complete
    PianoMmBusy = -16,                        // the lock stayed held past the bound
};

typedef struct PianoMmCmd { uint32_t addr, data; } PianoMmCmd;

// What the report hook is told: (event, a, b).
enum {
    PianoMmLockWaiting = 1,                   // a = the holder's tag
    PianoMmLockGot = 2,                       // a = the former holder, b = ms waited
    PianoMmLockBusy = 3,                      // a = the holder, b = ms waited
    PianoMmLockLost = 4,                      // a = the tag found when giving back
    PianoMmSent = 10,                         // a = the resource mask sent, b = the command count
    PianoMmUnchanged = 11,                    // a = the mask asked: nothing to send
    PianoMmFailed = 12,                       // a = the mask, b = the error
};

typedef struct PianoMmVotesOps {
    void *context;
    // The resource's RPMh address (cmd-db); 0 when absent.
    uint32_t (*address)(void *context, unsigned resource);
    // A rail: the first hlvl whose vlvl reaches `vlvl` (vlvl 0: hlvl 0). 0, or negative (no level reaches it).
    int (*arc_level)(void *context, unsigned resource, uint32_t vlvl, uint32_t *hlvl);
    // One transaction of `count` (1..3) commands through an idle ACTIVE TCS of apps DRV-2, every command waited for; the callee
    // takes the DRV-2 lock (sm8750_rpmh_drv2_owner) around it. 0, or negative.
    int (*send)(void *context, const PianoMmCmd *cmds, unsigned count);
    void (*sleep_ms)(void *context, uint32_t ms);           // blocks (thread context)
    void (*report)(void *context, uint32_t event, uint32_t a, uint32_t b);   // optional
} PianoMmVotesOps;

typedef struct PianoMmVotes {
    volatile uint32_t *words;                 // piano_mm_votes (host tests: words of their own)
    volatile uint32_t *lock;                  // &piano_mm_votes_owner
    uint32_t tag;                             // this kext's, nonzero
    unsigned client;                          // PIANO_MM_VIDEO, PIANO_MM_CAMERA, ...
    uint32_t bound_ms;                        // 0: PianoMmDefaultBoundMs
    uint32_t floor[PIANO_MM_RESOURCES];       // the rails' floors this client declares (vlvl; MM1's is ignored)
    const PianoMmVotesOps *ops;
    // The last update, as this kext saw it:
    PianoMmCmd cmds[PIANO_MM_RESOURCES];
    unsigned count;                           // commands sent (0: nothing changed)
    uint32_t sentMask;                        // the resources those commands were for
    uint32_t vote[PIANO_MM_RESOURCES];        // each resource's vote after the update (a rail: the vlvl voted for, before its hlvl)
    uint32_t level[PIANO_MM_RESOURCES];       // a rail: its hlvl; MM1: the command word
    uint32_t others[PIANO_MM_RESOURCES];      // the other clients' highest need (a rail: vlvl; MM1: nonzero when any needs it)
    int error;
    // Counts:
    uint32_t updates, transactions, unchanged, failed, busy, lost, waits, max_wait_ms;
} PianoMmVotes;

static inline uint32_t piano_mm_load(volatile uint32_t *w) { return __atomic_load_n(w, __ATOMIC_ACQUIRE); }
static inline void piano_mm_store(volatile uint32_t *w, uint32_t v) { __atomic_store_n(w, v, __ATOMIC_RELEASE); }
static inline int piano_mm_valid(const PianoMmVotes *v)
{
    return v && v->words && v->lock && v->tag && v->client < PIANO_MM_CLIENTS && v->ops && v->ops->address && v->ops->arc_level &&
           v->ops->send && v->ops->sleep_ms;
}
static inline void piano_mm_report(const PianoMmVotes *v, uint32_t event, uint32_t a, uint32_t b)
{
    if (v->ops->report) v->ops->report(v->ops->context, event, a, b);
}

// Any context: client c's recorded need of resource r; the clients with a need of r (bit c).
static inline uint32_t piano_mm_need(const PianoMmVotes *v, unsigned r, unsigned c)
{
    return v && v->words && r < PIANO_MM_RESOURCES && c < PIANO_MM_CLIENTS ? piano_mm_load(&v->words[piano_mm_need_index(r, c)]) : 0;
}
static inline uint32_t piano_mm_holders(const PianoMmVotes *v, unsigned r)
{
    uint32_t m = 0;
    for (unsigned c = 0; c < PIANO_MM_CLIENTS; ++c)
        if (piano_mm_need(v, r, c)) m |= 1u << c;
    return m;
}

// ----------------------------------------------------------------------------------------------------------------- the lock
static inline int piano_mm_lock(PianoMmVotes *v)
{
    const uint32_t bound = v->bound_ms ? v->bound_ms : (uint32_t)PianoMmDefaultBoundMs;
    uint32_t waited = 0, other = 0;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(v->lock, &expected, v->tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        other = expected;
        if (waited >= bound) {
            v->busy++;
            piano_mm_report(v, PianoMmLockBusy, other, waited);
            return PianoMmBusy;
        }
        if (!waited) {
            v->waits++;
            piano_mm_report(v, PianoMmLockWaiting, other, 0);
        }
        v->ops->sleep_ms(v->ops->context, 1);
        waited++;
    }
    if (waited) piano_mm_report(v, PianoMmLockGot, other, waited);
    if (waited > v->max_wait_ms) v->max_wait_ms = waited;
    return PianoMmOk;
}
static inline void piano_mm_unlock(PianoMmVotes *v)
{
    uint32_t expected = v->tag;
    if (!__atomic_compare_exchange_n(v->lock, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        v->lost++;
        piano_mm_report(v, PianoMmLockLost, expected, 0);
    }
}

// ------------------------------------------------------------------------------------------------------------ aggregation
// MM1's vote from the needs: off (commit only) when none; else commit | valid, x the sum (saturated), y the maximum.
static inline uint32_t piano_mm_bcm_vote(const uint32_t *needs, unsigned n)
{
    uint32_t x = 0, y = 0, any = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (!needs[i]) continue;
        any = 1;
        x += piano_mm_bcm_x(needs[i]);
        if (piano_mm_bcm_y(needs[i]) > y) y = piano_mm_bcm_y(needs[i]);
    }
    if (!any) return PIANO_MM_BCM_OFF;
    if (x > 0x3fffu) x = 0x3fffu;
    return PIANO_MM_BCM_COMMIT | PIANO_MM_BCM_VALID | x << 14 | y;
}

// ----------------------------------------------------------------------------------------------------------------- update
// For each resource in `mask`, this client's need becomes need[r] (a rail: a vlvl, 0 none; MM1: a command word, 0 none); then every
// resource of `mask` whose vote changed is sent, in one transaction. 0, or one of the errors above (nothing changed in the words).
static inline int piano_mm_votes_update(PianoMmVotes *v, uint32_t mask, const uint32_t need[PIANO_MM_RESOURCES])
{
    if (!piano_mm_valid(v) || !need || !mask || (mask & ~(uint32_t)PIANO_MM_ALL)) return PianoMmArgument;
    for (unsigned r = 0; r < PIANO_MM_RESOURCES; ++r)
        if ((mask & (1u << r)) && r == PIANO_MM_MM1 && need[r] && (need[r] & 0x90000000u)) return PianoMmArgument;
    v->count = 0;
    v->sentMask = 0;
    v->error = 0;
    int rc = piano_mm_lock(v);
    if (rc) {
        v->error = rc;
        return rc;
    }
    volatile uint32_t *w = v->words;
    uint32_t before[PIANO_MM_RESOURCES] = {0, 0, 0};
    for (unsigned r = 0; r < PIANO_MM_RESOURCES; ++r) {
        before[r] = piano_mm_load(&w[piano_mm_need_index(r, v->client)]);
        if (mask & (1u << r)) piano_mm_store(&w[piano_mm_need_index(r, v->client)], need[r]);
        if ((mask & (1u << r)) && r != PIANO_MM_MM1 && v->floor[r] > piano_mm_load(&w[PIANO_MM_FLOOR0 + r]))
            piano_mm_store(&w[PIANO_MM_FLOOR0 + r], v->floor[r]);
    }
    const uint32_t valid = piano_mm_load(&w[PIANO_MM_SENT_VALID]);
    for (unsigned r = 0; r < PIANO_MM_RESOURCES && !rc; ++r) {
        if (!(mask & (1u << r))) continue;
        uint32_t needs[PIANO_MM_CLIENTS], highest = 0, others = 0;
        for (unsigned c = 0; c < PIANO_MM_CLIENTS; ++c) {
            needs[c] = piano_mm_load(&w[piano_mm_need_index(r, c)]);
            if (r == PIANO_MM_MM1) {
                if (needs[c] && c != v->client) others = 1;
            } else {
                if (needs[c] > highest) highest = needs[c];
                if (c != v->client && needs[c] > others) others = needs[c];
            }
        }
        v->others[r] = others;
        uint32_t level = 0, vote = 0;
        const int needed = r == PIANO_MM_MM1 ? piano_mm_bcm_vote(needs, PIANO_MM_CLIENTS) != PIANO_MM_BCM_OFF : highest != 0;
        if (r == PIANO_MM_MM1) {
            vote = level = piano_mm_bcm_vote(needs, PIANO_MM_CLIENTS);
        } else {
            const uint32_t floor = piano_mm_load(&w[PIANO_MM_FLOOR0 + r]);
            vote = highest > floor ? highest : floor;
            if (v->ops->arc_level(v->ops->context, r, vote, &level)) rc = PianoMmNoLevel;
        }
        v->vote[r] = vote;
        v->level[r] = level;
        if (rc) break;
        const int was = (valid >> r) & 1u;
        if (was ? piano_mm_load(&w[PIANO_MM_SENT0 + r]) == level : !needed) continue;
        const uint32_t addr = v->ops->address(v->ops->context, r);
        if (!addr) {
            rc = PianoMmNoResource;
            break;
        }
        v->cmds[v->count].addr = addr;
        v->cmds[v->count].data = level;
        v->count++;
        v->sentMask |= 1u << r;
    }
    if (!rc && v->count) {
        piano_mm_store(&w[PIANO_MM_LAST_TAG], v->tag);
        if (v->ops->send(v->ops->context, v->cmds, v->count)) rc = PianoMmSend;
        else {
            for (unsigned i = 0, r = 0; r < PIANO_MM_RESOURCES; ++r)
                if (v->sentMask & (1u << r)) piano_mm_store(&w[PIANO_MM_SENT0 + r], v->cmds[i++].data);
            piano_mm_store(&w[PIANO_MM_SENT_VALID], valid | v->sentMask);
            piano_mm_store(&w[PIANO_MM_TRANSACTIONS], piano_mm_load(&w[PIANO_MM_TRANSACTIONS]) + 1);
            v->transactions++;
        }
    }
    if (rc) {   // the needs as they were: the words keep saying what DRV-2 was last told
        for (unsigned r = 0; r < PIANO_MM_RESOURCES; ++r)
            if (mask & (1u << r)) piano_mm_store(&w[piano_mm_need_index(r, v->client)], before[r]);
        piano_mm_store(&w[PIANO_MM_LAST_ERROR], (uint32_t)rc);
        v->failed++;
        v->error = rc;
    }
    piano_mm_store(&w[PIANO_MM_UPDATES], piano_mm_load(&w[PIANO_MM_UPDATES]) + 1);
    v->updates++;
    if (!rc && !v->count) v->unchanged++;
    piano_mm_unlock(v);
    if (rc) piano_mm_report(v, PianoMmFailed, mask, (uint32_t)rc);
    else if (v->count) piano_mm_report(v, PianoMmSent, v->sentMask, v->count);
    else piano_mm_report(v, PianoMmUnchanged, mask, 0);
    return rc;
}

// Shorthands: this client's needs of the two rails (one transaction), and of MM1 (on/off).
static inline int piano_mm_votes_rails(PianoMmVotes *v, uint32_t mxcVlvl, uint32_t mmcxVlvl)
{
    const uint32_t need[PIANO_MM_RESOURCES] = {mxcVlvl, mmcxVlvl, 0};
    return piano_mm_votes_update(v, 1u << PIANO_MM_MXC | 1u << PIANO_MM_MMCX, need);
}
static inline int piano_mm_votes_bus(PianoMmVotes *v, int on)
{
    const uint32_t need[PIANO_MM_RESOURCES] = {0, 0, on ? PIANO_MM_BCM_ON : 0u};
    return piano_mm_votes_update(v, 1u << PIANO_MM_MM1, need);
}

// ------------------------------------------------------------------------------------------------- cmd-db, for the rails
// The AOP command database (Linux drivers/soc/qcom/cmd-db.c's layout: a 144-byte header with eight 16-byte slave records, 24-byte
// entries, aux data from the data area) read for a client that has no reader of its own: a rail's address and its level table
// (u16 vlvl per hlvl, zero padded; hlvl 0 may be 0), and a BCM's address. As PianoVideo's VideoRpmh.h reads it.
typedef struct PianoMmArc {
    uint32_t addr;
    uint16_t vlvl[16];
    unsigned count;
} PianoMmArc;
enum { PIANO_MM_SLAVE_ARC = 3, PIANO_MM_SLAVE_BCM = 5 };
static inline uint32_t piano_mm_le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
// 0 and the entry's address/aux, -1 not a cmd-db, -2 no such entry, -3 corrupt (or of another slave type, or not unique).
static inline int piano_mm_cmd_db(const uint8_t *db, uint32_t bytes, const char *name, unsigned slave, uint32_t *addr,
                                  uint32_t *auxOffset, uint32_t *auxLen)
{
    if (!db || bytes < 144 || (piano_mm_le16(db + 4) | piano_mm_le16(db + 6) << 16) != 0x0c0330dbu) return -1;
    uint8_t key[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    unsigned length = 0;
    while (length < 8 && name[length]) {
        key[length] = (uint8_t)name[length];
        ++length;
    }
    if (!length || name[length]) return -2;
    unsigned matches = 0;
    uint32_t a = 0, s = 0, ao = 0, al = 0;
    for (unsigned k = 0; k < 8; ++k) {
        const uint8_t *h = db + 8 + 16 * k;
        const uint32_t id = piano_mm_le16(h), table = 144 + piano_mm_le16(h + 2), data = 144 + piano_mm_le16(h + 4),
                       count = piano_mm_le16(h + 6);
        if (!id) break;
        if (table > bytes || count > (bytes - table) / 24 || data > bytes) return -3;
        for (uint32_t n = 0; n < count; ++n) {
            const uint8_t *p = db + table + 24 * n;
            int same = 1;
            for (unsigned i = 0; i < 8 && same; ++i) same = p[i] == key[i];
            if (!same) continue;
            ++matches;
            s = id;
            a = piano_mm_le16(p + 16) | piano_mm_le16(p + 18) << 16;
            al = piano_mm_le16(p + 20);
            ao = data + piano_mm_le16(p + 22);
        }
    }
    if (!matches) return -2;
    if (matches != 1 || s != slave || (a >> 16) != slave || (a & 3) || ao > bytes || al > bytes - ao) return -3;
    *addr = a;
    if (auxOffset) *auxOffset = ao;
    if (auxLen) *auxLen = al;
    return 0;
}
static inline int piano_mm_cmd_db_arc(const uint8_t *db, uint32_t bytes, const char *name, PianoMmArc *arc)
{
    uint32_t addr = 0, ao = 0, al = 0;
    const int rc = piano_mm_cmd_db(db, bytes, name, PIANO_MM_SLAVE_ARC, &addr, &ao, &al);
    if (rc) return rc;
    const uint32_t entries = al / 2;
    if (!entries || entries > 16) return -3;
    arc->addr = addr;
    for (unsigned i = 0; i < 16; ++i) arc->vlvl[i] = i < entries ? (uint16_t)piano_mm_le16(db + ao + 2 * i) : 0;
    arc->count = 1;
    while (arc->count < entries && arc->vlvl[arc->count]) arc->count++;
    return arc->count < 2 ? -3 : 0;   // a dummy ARC: nothing to vote
}
// The first hlvl whose vlvl reaches the request (vlvl 0: hlvl 0). 0, or -3 when every level is lower.
static inline int piano_mm_arc_level(const PianoMmArc *arc, uint32_t vlvl, uint32_t *hlvl)
{
    if (!vlvl) {
        *hlvl = 0;
        return 0;
    }
    for (uint32_t h = 0; h < arc->count; ++h)
        if (arc->vlvl[h] >= vlvl) {
            *hlvl = h;
            return 0;
        }
    return -3;
}

#ifdef __cplusplus
}
#endif

#endif
