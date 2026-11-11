// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel-wide secure-call lock: one owner of the secure world's yielding SiP calls (SCM over SMC) at a time, across every native
// kext that makes them (PianoA830: GPU_INIT_REGS, the SMMU aperture, PAS 13 for the ZAP shader, SET_REMOTE_STATE; PianoPower: PAS 36
// and 1 for the DSP, PAS shutdown; PianoVideo from its level 3: PAS 9 and MP_VIDEO_VAR). docs/scm-lock.md is the reference. Include
// it as <PianoScmLock.h> (tooling/k27/build-native-kext.py puts package/include on every kext's include path).
//
// Why: each kext's Sm8750Scm serialises only its own calls (its ops->lock), so two kexts could be in the secure world at once, and
// a PAS load is several calls that belong together (init_image -> mem_setup -> the segments -> auth_and_reset). Linux holds one
// global lock per call (qcom_scm's), and the K90 port never let two secure calls overlap; on piano an overlap of this kind is the
// leading suspect of the whole-system stall of docs/drivers/charging.md §5a.
//
// The word: `sm8750_scm_owner`, one 32-bit word in PianoA830 (board/PianoA830.cpp defines it, as PianoA830 owns the DRV-2 word
// sm8750_rpmh_drv2_owner of adreno830/platform/sm8750/Sm8750Rpmh.h). PianoA830 links before every other user (configs/
// k27-native-kexts.txt), and each user's descriptor names com.unixnu.driver.PianoA830 as a library, so the KC linker resolves every
// import to that single copy. 0 = free; any other value = the holding kext's tag (below).
//
// The protocol (this file implements it; nothing else writes the word):
//   take:  compare-exchange 0 -> tag. A kext already holding it (a sequence of its own, or another of its threads in a call) only
//          counts one more holder: the count lives in the kext and is changed under the kext's own hold mutex, which is held while
//          waiting, so a kext's threads queue behind its first waiter. Waiting sleeps 1 ms per try (callers are in thread context;
//          the SCM yielding calls already sleep), reports the holder at the first wait and every second, and is bounded: after
//          bound_ms (default 30 s) the waiter takes the word over (counted, reported). A holder past 30 s is either stuck in the
//          secure world (then the Gunyah watchdog resets the board soon anyway) or a leaked lock; taking over keeps the system going
//          and never turns a wait into a new failure path for the caller.
//   give:  the last holder of the kext compare-exchanges tag -> 0; finding another tag there (taken over) is counted, not undone.
//   use:   the kext's Sm8750Scm ops->lock/unlock take and give it around every yielding request (after / before the kext's own SCM
//          mutex: lock order SCM mutex -> hold mutex), and a multi-call sequence takes it once around the whole sequence (the calls
//          inside then only count). Fast calls (GET_WQ_CTX in the wait-queue interrupt) take nothing, as before.
#ifndef PIANO_SCM_LOCK_H
#define PIANO_SCM_LOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PianoScmTagA830 = 0x41383330u,    // "A830"
    PianoScmTagPower = 0x504f5752u,   // "POWR"
    PianoScmTagVideo = 0x56494445u,   // "VIDE"
    PianoScmLockDefaultBoundMs = 30000,
    PianoScmLockReportMs = 1000,
};
// take()'s answers.
enum { PianoScmLockOk = 0, PianoScmLockTakenOver = 1, PianoScmLockArgument = -22 };
// What the report hook is told (with the other tag and the wait so far).
enum { PianoScmLockWaiting = 1, PianoScmLockStillWaiting = 2, PianoScmLockGot = 3, PianoScmLockTookOver = 4, PianoScmLockLost = 5,
       PianoScmLockUnbalanced = 6 };

typedef struct PianoScmLockOps {
    void *context;
    void (*sleep_ms)(void *context, uint32_t ms);   // blocks (thread context)
    void (*mutex_lock)(void *context);               // the kext's hold mutex (not its SCM request mutex)
    void (*mutex_unlock)(void *context);
    // Optional: called with the hold mutex held (keep it short: one log line).
    void (*report)(void *context, uint32_t event, uint32_t other, uint32_t waited_ms);
} PianoScmLockOps;

typedef struct PianoScmLock {
    volatile uint32_t *word;   // &sm8750_scm_owner (host tests: a word of their own)
    uint32_t tag;              // this kext's tag: nonzero, its own
    uint32_t bound_ms;         // 0: PianoScmLockDefaultBoundMs
    const PianoScmLockOps *ops;
    // Under the hold mutex:
    uint32_t holds;            // this kext's current holders (sequences and calls)
    uint32_t takes, contended, taken_over, lost, unbalanced, max_wait_ms;
} PianoScmLock;

static inline uint32_t piano_scm_lock_owner(const PianoScmLock *l)
{
    return l && l->word ? __atomic_load_n(l->word, __ATOMIC_ACQUIRE) : 0;
}

static inline void piano_scm_lock_report(PianoScmLock *l, uint32_t event, uint32_t other, uint32_t waited)
{
    if (l->ops->report) l->ops->report(l->ops->context, event, other, waited);
}

// Takes the lock for this kext (nested holds of the same kext only count). Never fails for a valid lock: past the bound it takes the
// word over and answers PianoScmLockTakenOver.
static inline int piano_scm_lock_take(PianoScmLock *l)
{
    if (!l || !l->word || !l->tag || !l->ops || !l->ops->sleep_ms || !l->ops->mutex_lock || !l->ops->mutex_unlock)
        return PianoScmLockArgument;
    l->ops->mutex_lock(l->ops->context);
    if (l->holds) {
        l->holds++;
        l->ops->mutex_unlock(l->ops->context);
        return PianoScmLockOk;
    }
    const uint32_t bound = l->bound_ms ? l->bound_ms : (uint32_t)PianoScmLockDefaultBoundMs;
    uint32_t waited = 0, other = 0;
    int answer = PianoScmLockOk;
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(l->word, &expected, l->tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        if (expected == l->tag) break;   // ours already (a release that found it taken over, then handed back): keep it
        other = expected;
        if (waited >= bound) {
            // Take it over from whoever holds it now (if that changes under us, try again).
            if (__atomic_compare_exchange_n(l->word, &expected, l->tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                l->taken_over++;
                answer = PianoScmLockTakenOver;
                piano_scm_lock_report(l, PianoScmLockTookOver, other, waited);
                break;
            }
            continue;
        }
        if (!waited) {
            l->contended++;
            piano_scm_lock_report(l, PianoScmLockWaiting, other, 0);
        } else if (waited % PianoScmLockReportMs == 0) {
            piano_scm_lock_report(l, PianoScmLockStillWaiting, other, waited);
        }
        l->ops->sleep_ms(l->ops->context, 1);
        waited++;
    }
    if (waited && answer == PianoScmLockOk) piano_scm_lock_report(l, PianoScmLockGot, other, waited);
    if (waited > l->max_wait_ms) l->max_wait_ms = waited;
    l->holds = 1;
    l->takes++;
    l->ops->mutex_unlock(l->ops->context);
    return answer;
}

// Gives one hold back; the kext's last one frees the word.
static inline void piano_scm_lock_give(PianoScmLock *l)
{
    if (!l || !l->word || !l->ops || !l->ops->mutex_lock || !l->ops->mutex_unlock) return;
    l->ops->mutex_lock(l->ops->context);
    if (!l->holds) {
        l->unbalanced++;
        piano_scm_lock_report(l, PianoScmLockUnbalanced, piano_scm_lock_owner(l), 0);
    } else if (!--l->holds) {
        uint32_t expected = l->tag;
        if (!__atomic_compare_exchange_n(l->word, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            l->lost++;
            piano_scm_lock_report(l, PianoScmLockLost, expected, 0);
        }
    }
    l->ops->mutex_unlock(l->ops->context);
}

#ifdef __cplusplus
}
#endif

// ----------------------------------------------------------------------------------------------------- the kernel side (C++ kexts)
// A kext defines PIANO_SCM_LOCK_KERNEL before including this file and declares its lock with PIANO_SCM_KERNEL_LOCK(name, tag,
// "Label"); then piano_scm_kernel_take(&name) / piano_scm_kernel_give(&name). The hold mutex is an IOLock made on first use; the
// report hook writes one IOLog line per event ("Label: secure-call lock ...").
#if defined(PIANO_SCM_LOCK_KERNEL) && defined(__cplusplus)
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

extern "C" volatile uint32_t sm8750_scm_owner;   // PianoA830's (board/PianoA830.cpp)

struct PianoScmKernelLock {
    PianoScmLock lock;
    PianoScmLockOps ops;
    IOLock *mutex;
    const char *label;
};

static inline IOLock *piano_scm_kernel_mutex(PianoScmKernelLock *k)
{
    IOLock *m = __atomic_load_n(&k->mutex, __ATOMIC_ACQUIRE);
    if (m) return m;
    IOLock *made = IOLockAlloc(), *expected = nullptr;
    if (__atomic_compare_exchange_n(&k->mutex, &expected, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return made;
    if (made) IOLockFree(made);
    return expected;
}
static inline void piano_scm_kernel_sleep(void *, uint32_t ms) { IOSleep(ms); }
static inline void piano_scm_kernel_mutex_lock(void *c) { IOLockLock(piano_scm_kernel_mutex(static_cast<PianoScmKernelLock *>(c))); }
static inline void piano_scm_kernel_mutex_unlock(void *c) { IOLockUnlock(static_cast<PianoScmKernelLock *>(c)->mutex); }
static inline void piano_scm_kernel_report(void *c, uint32_t event, uint32_t other, uint32_t waited)
{
    const PianoScmKernelLock *k = static_cast<const PianoScmKernelLock *>(c);
    const char *what = event == PianoScmLockWaiting ? "held by %#x: waiting (%u ms)"
                     : event == PianoScmLockStillWaiting ? "still held by %#x after %u ms"
                     : event == PianoScmLockGot ? "free again (it was %#x's), taken after %u ms"
                     : event == PianoScmLockTookOver ? "TAKEN OVER from %#x after %u ms (its holder never gave it back)"
                     : event == PianoScmLockLost ? "given back, but %#x had taken it over (%u)"
                     : "given back without a hold (owner %#x, %u)";
    char line[160];
    snprintf(line, sizeof line, what, other, waited);
    IOLog("%s: secure-call lock (sm8750_scm_owner, ours %#x): %s\n", k->label, k->lock.tag, line);
}
#define PIANO_SCM_KERNEL_LOCK(name, tag, label)                                                                                  \
    PianoScmKernelLock name = {{&sm8750_scm_owner, (tag), 0, &name.ops, 0, 0, 0, 0, 0, 0, 0},                                    \
                               {&name, piano_scm_kernel_sleep, piano_scm_kernel_mutex_lock, piano_scm_kernel_mutex_unlock,       \
                                piano_scm_kernel_report},                                                                         \
                               nullptr, (label)}
static inline int piano_scm_kernel_take(PianoScmKernelLock *k) { return piano_scm_lock_take(&k->lock); }
static inline void piano_scm_kernel_give(PianoScmKernelLock *k) { piano_scm_lock_give(&k->lock); }
#endif

#endif
