// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SPMI transaction lock and the arbiter's channel primitives every native kext uses: one transaction at a time on piano's SPMI
// PMIC arbiter (v7, DT spmi@c400000, EE 0, the application processor) across PianoPower (gauges' charger reads, the complex watch,
// the charger writes, the RTC), PianoThermal (the ADC's conversions), PianoWifi, PianoBluetooth, PianoCamera, PianoDwc3 (the eUSB2
// repeater, the restart reason), PianoTorch, PianoInput and PianoUsbHost. Include it as <PianoSpmi.h> (tooling/k27/build-native-
// kext.py puts package/include on every kext's include path) or as "../include/PianoSpmi.h" from a kext's own header (the host
// tests of the kexts compile with their own directory only). docs/spmi-lock.md is the reference.
//
// Why (2026-11-12/13, RAM boots of the same payload): with charging at level 2 and PianoThermal's ADC on the bus, PianoWifi's level 1
// regulator survey failed at random where ESP v3.2 (no PianoThermal, charging level 1) read every regulator: TYPE reads ending with
// an error status through five tries (v33p, v33c: before the DSP started), and reads that ended "ok" with another register's value
// (v34p: ldof3, an LDO, read TYPE 0x00; smpi7, an SMPS, read 0x04, an LDO's), so level 2 stopped at step -201; PianoThermal's first
// ADC round lost a conversion to a "dropped" write (status 0x9) on two of those boots. Each kext drove its observer and owner
// channels with its own copy of the accessor and nothing shared between them. Linux's spmi-pmic-arb (read for behaviour only) never
// has two commands in flight on one bus: one bus-wide raw spinlock, taken with interrupts off, covers every command from the store
// of the command word through the poll for done to the read of the data, for observer reads and owner-channel writes alike, whatever
// their APIDs (and a masked write's read and write together). Nothing shows that v7's per-APID channels may run concurrently (v1
// had one channel per EE, the "dropped" status says a command was not performed); this lock gives piano Linux's regime.
//
// The words: `piano_spmi_words` (PianoSpmiWords), defined by PianoA830 in board/PianoA830.cpp next to sm8750_scm_owner and the
// other shared words: PianoA830 links before every user and each user's descriptor names com.unixnu.driver.PianoA830 as a library,
// so the KC linker resolves every import to that one copy.
//   owner     0 free, else the holder's tag: four letters of its kext's name after "Piano" (PIANO_KEXT_NAME, which build-native-
//             kext.py defines: "WIFI", "THER", "POWE", "CAME", "DWC3", "TORC", "BLUE", "INPU", "USBH").
//   holder    the holding thread; depth its nesting (a take by the thread that holds it only counts).
//   the rest  every kext's counts together (changed by the holder only): the lines each kext logs show them next to its own.
//
// The protocol (this file implements it; nothing else writes the words):
//   take    interrupts masked on this CPU (ml_set_interrupts_enabled(FALSE): the holder can be neither interrupted nor preempted, so
//           a hold lasts one transaction, and an interrupt handler taking the lock can never spin on its own CPU's holder), then a
//           compare-exchange 0 -> tag. Taken by someone else: interrupts back as they were, a 1 us spin, again. Bounded: a waiter
//           that spins bound_us (default 20000 spins of about 1 us; a hold is at most one transaction, about 6 ms when it times out)
//           while no new hold begins takes the word over (counted and reported: a holder that never gave it back) rather than hang
//           the caller; its own preemption or losing races to other waiters never counts.
//   hold    exactly one transaction: the data and the command stored, the status polled (1000 x 1 us, Linux's bound), the read data
//           read; after a timeout, up to 5 ms more until the channel reports done, so the next holder finds the bus quiet. Never a
//           sleep, a log line, a mapping or an allocation inside (the channel windows are mapped before).
//   give    the outermost give clears the holder, compare-exchanges tag -> 0 and restores the interrupts.
//   retry   piano_spmi read(): a read that failed (failure or dropped) or timed out is tried again up to four times, 500 us apart
//           with the lock free (as the 2026-11-12 SpmiObserver.h retries, a548070/3cb778a); a denied read is final. Writes are not
//           retried here: a caller that knows its write is idempotent may.
#ifndef PIANO_SPMI_H
#define PIANO_SPMI_H

#include <stddef.h>
#include <stdint.h>
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PianoSpmiDefaultBoundUs = 20000,
    PianoSpmiTokenNone = 0,     // no lock (host code without one): nothing to give back
    PianoSpmiTokenOuter = 1,
    PianoSpmiTokenNested = 2,
};

typedef struct PianoSpmiWords {
    volatile uint32_t owner;           // 0 free, else the holder's tag
    volatile uint32_t depth;           // the holder's nesting
    volatile uint64_t holder;          // the holding thread
    // every kext's counts (the holder's to change)
    volatile uint32_t takes, contended, nested, taken_over;
    volatile uint32_t wait_max_us, hold_max_us, last_waiter, last_holder;   // last_*: the latest wait, who waited for whom
    volatile uint64_t wait_total_us;
} PianoSpmiWords;

typedef struct PianoSpmiLockOps {
    void *context;
    uintptr_t (*mask)(void *context);                 // interrupts off on this CPU; answers what unmask needs to restore them
    void (*unmask)(void *context, uintptr_t was);
    uint64_t (*self)(void *context);                  // the calling thread: nonzero, its own
    void (*spin)(void *context);                      // a wait of about 1 us (interrupts as the caller had them)
    uint64_t (*now_us)(void *context);                // optional: a monotonic clock for the wait and hold times
} PianoSpmiLockOps;

typedef struct PianoSpmiLock {
    PianoSpmiWords *words;             // &piano_spmi_words (host tests: words of their own)
    uint32_t tag;                      // this kext's: nonzero
    uint32_t bound_us;                 // 0: PianoSpmiDefaultBoundUs
    const PianoSpmiLockOps *ops;
    // this kext's counts (changed with the word held)
    uint32_t takes, contended, nested, taken_over, lost, unbalanced;
    uint32_t wait_max_us, hold_max_us, last_other;    // last_other: the tag this kext last waited for
    uint64_t wait_total_us;
    // its transactions (the primitives below count them inside the hold): ended with an error status, timed out, tried again,
    // read twice with two different answers (readStable)
    uint32_t errors, timeouts, retries, unstable;
} PianoSpmiLock;

typedef struct PianoSpmiToken {
    uintptr_t was;                     // the interrupt state before the take
    uint64_t since;                    // when the hold began (now_us)
    uint32_t kind;                     // PianoSpmiToken*
} PianoSpmiToken;

static inline int piano_spmi_valid(const PianoSpmiLock *l)
{
    return l && l->words && l->tag && l->ops && l->ops->mask && l->ops->unmask && l->ops->self && l->ops->spin;
}
static inline uint32_t piano_spmi_owner(const PianoSpmiLock *l)
{
    return l && l->words ? __atomic_load_n(&l->words->owner, __ATOMIC_ACQUIRE) : 0;
}
static inline uint64_t piano_spmi_now(const PianoSpmiLock *l) { return l->ops->now_us ? l->ops->now_us(l->ops->context) : 0; }

// Takes the lock for one transaction (or a short sequence of them) of the calling thread. Never fails for a valid lock: past the
// bound it takes the word over (counted). Without a valid lock (null: a host test that has none) it does nothing.
static inline PianoSpmiToken piano_spmi_take(PianoSpmiLock *l)
{
    PianoSpmiToken t = {0, 0, PianoSpmiTokenNone};
    if (!piano_spmi_valid(l)) return t;
    const PianoSpmiLockOps *o = l->ops;
    PianoSpmiWords *w = l->words;
    const uint64_t self = o->self(o->context);
    t.was = o->mask(o->context);
    if (__atomic_load_n(&w->owner, __ATOMIC_ACQUIRE) && __atomic_load_n(&w->holder, __ATOMIC_RELAXED) == self) {
        w->depth = w->depth + 1;       // ours already (only this thread can have stored its own identity there)
        l->nested++;
        w->nested = w->nested + 1;
        t.kind = PianoSpmiTokenNested;
        return t;
    }
    // The bound counts this waiter's own spins (about 1 us each) since the lock last changed hands (the transaction count moved):
    // neither a waiter that was preempted between two spins nor one that keeps losing to other waiters ever takes it over, only
    // one that sees the same hold for bound_us of its own spinning.
    const uint32_t bound = l->bound_us ? l->bound_us : (uint32_t)PianoSpmiDefaultBoundUs;
    const uint64_t start = piano_spmi_now(l);
    uint32_t spins = 0, same = 0, other = 0, over = 0, epoch = __atomic_load_n(&w->takes, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&w->owner, &expected, l->tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        other = expected;
        const uint32_t now = __atomic_load_n(&w->takes, __ATOMIC_RELAXED);
        if (now != epoch) {
            epoch = now;
            same = 0;
        }
        if (same >= bound) {
            // Take it over from whoever holds it now (if that changes under us, try again).
            if (__atomic_compare_exchange_n(&w->owner, &expected, l->tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                over = 1;
                break;
            }
            same = 0;
            continue;
        }
        o->unmask(o->context, t.was);  // wait with the interrupts as the caller had them
        o->spin(o->context);
        spins++;
        same++;
        t.was = o->mask(o->context);
    }
    __atomic_store_n(&w->holder, self, __ATOMIC_RELAXED);
    w->depth = 1;
    t.since = piano_spmi_now(l);
    t.kind = PianoSpmiTokenOuter;
    // The counts, with the word held.
    const uint32_t waited = spins ? (uint32_t)(o->now_us ? t.since - start : spins) : 0;
    l->takes++;
    w->takes = w->takes + 1;
    if (spins) {
        l->contended++;
        l->last_other = other;
        l->wait_total_us += waited;
        if (waited > l->wait_max_us) l->wait_max_us = waited;
        w->contended = w->contended + 1;
        w->wait_total_us = w->wait_total_us + waited;
        if (waited > w->wait_max_us) w->wait_max_us = waited;
        w->last_waiter = l->tag;
        w->last_holder = other;
    }
    if (over) {
        l->taken_over++;
        w->taken_over = w->taken_over + 1;
    }
    return t;
}

// Gives a take back: a nested one only counts down; the outermost frees the word and restores the interrupts.
static inline void piano_spmi_give(PianoSpmiLock *l, PianoSpmiToken t)
{
    if (t.kind == PianoSpmiTokenNone || !piano_spmi_valid(l)) return;
    const PianoSpmiLockOps *o = l->ops;
    PianoSpmiWords *w = l->words;
    if (t.kind == PianoSpmiTokenNested) {
        if (w->depth > 1) w->depth = w->depth - 1;
        else l->unbalanced++;
        o->unmask(o->context, t.was);
        return;
    }
    const uint64_t self = o->self(o->context);
    if (__atomic_load_n(&w->holder, __ATOMIC_RELAXED) != self) {
        l->lost++;                     // taken over while we held it: not ours to free any more
        o->unmask(o->context, t.was);
        return;
    }
    if (o->now_us) {
        const uint64_t held = piano_spmi_now(l) - t.since;
        const uint32_t us = held > 0xffffffffu ? 0xffffffffu : (uint32_t)held;
        if (us > l->hold_max_us) l->hold_max_us = us;
        if (us > w->hold_max_us) w->hold_max_us = us;
    }
    if (w->depth != 1) l->unbalanced++;
    w->depth = 0;
    __atomic_store_n(&w->holder, 0, __ATOMIC_RELAXED);
    uint32_t expected = l->tag;
    if (!__atomic_compare_exchange_n(&w->owner, &expected, 0u, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) l->lost++;
    o->unmask(o->context, t.was);
}

// A tag as text ("WIFI"); 0 as "-".
static inline void piano_spmi_tag_text(uint32_t tag, char out[5])
{
    if (!tag) {
        out[0] = '-';
        out[1] = 0;
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const char c = (char)(tag >> (24 - 8 * i));
        out[i] = c >= 0x20 && c < 0x7f ? c : '?';
    }
    out[4] = 0;
}

// The lock's line for a kext's report (one per report): this kext's transactions and waits ("ours"), then every kext's ("all").
// part: PianoSpmiLineBoth, or one half (for a kext whose log lines are short). About 280 bytes for both; at most `size` written.
enum { PianoSpmiLineBoth = 0, PianoSpmiLineOurs = 1, PianoSpmiLineAll = 2 };
static inline void piano_spmi_format_part(const PianoSpmiLock *l, char *out, size_t size, int part)
{
    if (!out || !size) return;
    if (!l || !l->words) {
        snprintf(out, size, "SPMI lock: none (no shared lock in this build)");
        return;
    }
    const PianoSpmiWords *w = l->words;
    char other[5], waiter[5], holder[5], ours[200] = "", all[128] = "";
    piano_spmi_tag_text(l->last_other, other);
    piano_spmi_tag_text(w->last_waiter, waiter);
    piano_spmi_tag_text(w->last_holder, holder);
    if (part != PianoSpmiLineAll)
        snprintf(ours, sizeof ours,
                 "ours %u tx (err %u, timeout %u, retry %u, unstable %u), waited %u (max %u us, total %llu us, last behind %s), nested %u, "
                 "taken over %u, lost %u, unbalanced %u, max hold %u us",
                 l->takes, l->errors, l->timeouts, l->retries, l->unstable, l->contended, l->wait_max_us,
                 (unsigned long long)l->wait_total_us, other, l->nested, l->taken_over, l->lost, l->unbalanced, l->hold_max_us);
    if (part != PianoSpmiLineOurs)
        snprintf(all, sizeof all, "all kexts %u tx, waited %u (max %u us, last %s behind %s), taken over %u, max hold %u us", w->takes,
                 w->contended, w->wait_max_us, waiter, holder, w->taken_over, w->hold_max_us);
    snprintf(out, size, "SPMI lock: %s%s%s", ours, part == PianoSpmiLineBoth ? "; " : "", all);
}
static inline void piano_spmi_format(const PianoSpmiLock *l, char *out, size_t size)
{
    piano_spmi_format_part(l, out, size, PianoSpmiLineBoth);
}

#ifdef __cplusplus
}
#endif

// ----------------------------------------------------------------------------------------------- the channel primitives (C++)
#ifdef __cplusplus

// The tag of a kext from its name (PIANO_KEXT_NAME): the four letters after "Piano" (or "Native"), upper case, space-padded.
constexpr uint32_t piano_spmi_tag_of(const char *name)
{
    const char *p = name;
    const char piano[] = "Piano", native[] = "Native";
    bool isPiano = true, isNative = true;
    for (int i = 0; i < 5; ++i) isPiano = isPiano && p[i] == piano[i];
    for (int i = 0; i < 6 && isNative; ++i) isNative = p[i] == native[i];
    if (isPiano) p += 5;
    else if (isNative) p += 6;
    uint32_t t = 0;
    bool ended = false;
    for (int i = 0; i < 4; ++i) {
        char c = ended ? ' ' : p[i];
        if (!c) {
            ended = true;
            c = ' ';
        }
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        t = t << 8 | uint8_t(c);
    }
    return t ? t : 0x53504d49u;   // "SPMI"
}

#if defined(KERNEL)
// ------------------------------------------------------------------------------------------------- the kernel side
// Every kext that includes this file uses its own PianoSpmiLock (one per kext: an inline variable, merged inside the kext) on the
// shared words. Nothing to declare: the primitives below take it.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>

extern "C" PianoSpmiWords piano_spmi_words;   // PianoA830's (board/PianoA830.cpp)

#ifndef PIANO_KEXT_NAME
#define PIANO_KEXT_NAME "SPMI"
#endif

inline uintptr_t piano_spmi_kernel_mask(void *) { return uintptr_t(ml_set_interrupts_enabled(FALSE)); }
inline void piano_spmi_kernel_unmask(void *, uintptr_t was) { (void)ml_set_interrupts_enabled(boolean_t(was)); }
inline uint64_t piano_spmi_kernel_self(void *) { return uint64_t(uintptr_t(current_thread())); }
inline void piano_spmi_kernel_spin(void *) { IODelay(1); }
inline uint64_t piano_spmi_kernel_now(void *)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
inline constexpr PianoSpmiLockOps piano_spmi_kernel_ops = {nullptr, piano_spmi_kernel_mask, piano_spmi_kernel_unmask,
                                                           piano_spmi_kernel_self, piano_spmi_kernel_spin, piano_spmi_kernel_now};
constinit inline PianoSpmiLock piano_spmi_kext_lock = {&piano_spmi_words, piano_spmi_tag_of(PIANO_KEXT_NAME), 0,
                                                       &piano_spmi_kernel_ops, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
namespace pianospmi {
inline PianoSpmiLock *lock() { return &piano_spmi_kext_lock; }
}  // namespace pianospmi
#else
// ------------------------------------------------------------------------------------------------- host code
// No lock unless a test installs one (tests/spmi_lock_host.cpp): the kexts' host tests run single-threaded on their models.
inline PianoSpmiLock *piano_spmi_host_lock = nullptr;
namespace pianospmi {
inline PianoSpmiLock *lock() { return piano_spmi_host_lock; }
}  // namespace pianospmi
#endif

namespace pianospmi {

// A channel's registers (observer channels at obsv + 0x20 * APID, owner channels at chnls + 0x1000 * APID: the same layout).
constexpr uint32_t kCmd = 0x00, kStatus = 0x08, kWdata0 = 0x10, kWdata1 = 0x14, kRdata0 = 0x18, kRdata1 = 0x1c;
constexpr uint32_t kDone = 1u << 0, kFailure = 1u << 1, kDenied = 1u << 2, kDropped = 1u << 3, kErrors = 0xeu;
constexpr uint32_t kOpExtWriteL = 0, kOpExtReadL = 1;
constexpr unsigned kPollUs = 1000;      // x 1 us: Linux's PMIC_ARB_TIMEOUT_US
constexpr unsigned kDrainUs = 5000;     // after a timeout, within the hold: the command in flight finishes
constexpr unsigned kRetries = 4, kRetryGapUs = 500;

enum class Result : uint8_t { ok, error, timeout };
inline const char *resultName(Result r) { return r == Result::ok ? "ok" : r == Result::error ? "error" : "timeout"; }

constexpr uint32_t command(uint32_t opcode, uint8_t reg, unsigned bytes) { return opcode << 27 | uint32_t(reg) << 4 | (bytes - 1); }
constexpr bool validRange(uint8_t reg, unsigned bytes) { return bytes >= 1 && bytes <= 8 && unsigned(reg) + bytes <= 0x100; }

// The lock around one transaction (RAII): constructed right before the first store of the command, destroyed after the data.
class Hold {
 public:
  explicit Hold(PianoSpmiLock *l = lock()) : l_(l), t_(piano_spmi_take(l)) {}
  ~Hold() { piano_spmi_give(l_, t_); }
  Hold(const Hold &) = delete;
  Hold &operator=(const Hold &) = delete;
  bool held() const { return t_.kind != PianoSpmiTokenNone; }

 private:
  PianoSpmiLock *l_;
  PianoSpmiToken t_;
};

// The counts of a transaction's end, inside its hold.
inline void count(PianoSpmiLock *l, const Hold &h, Result r, bool retry)
{
  if (!h.held()) return;
  if (r == Result::error) ++l->errors;
  if (r == Result::timeout) ++l->timeouts;
  if (retry) ++l->retries;
}

// The poll of one channel (the caller holds the lock): done, or a timeout and then the drain. *status = the last status word.
template <class Rd, class Delay>
Result poll(Rd &rd, Delay &delayUs, uint32_t *status)
{
  uint32_t st = 0;
  for (unsigned i = 0; i < kPollUs; ++i) {
    st = rd(kStatus);
    if (st & kDone) {
      *status = st;
      return (st & kErrors) ? Result::error : Result::ok;
    }
    delayUs(1u);
  }
  *status = st;
  for (unsigned i = 0; i < kDrainUs && !(rd(kStatus) & kDone); ++i) delayUs(1u);
  return Result::timeout;
}

namespace detail {
template <class Rd, class Wr, class Delay>
Result readOnce(Rd &rd, Wr &wr, Delay &delayUs, uint8_t reg, uint8_t *value, unsigned bytes, uint32_t *status, PianoSpmiLock *l,
                bool retry)
{
  uint32_t st = 0, lo = 0, hi = 0;
  if (!value || !validRange(reg, bytes)) {
    if (status) *status = 0;
    return Result::error;
  }
  Result r;
  {
    Hold hold(l);
    wr(kCmd, command(kOpExtReadL, reg, bytes));
    r = poll(rd, delayUs, &st);
    if (r == Result::ok) {
      lo = rd(kRdata0);
      if (bytes > 4) hi = rd(kRdata1);
    }
    count(l, hold, r, retry);
  }
  if (status) *status = st;
  if (r == Result::ok)
    for (unsigned b = 0; b < bytes; ++b) value[b] = uint8_t((b < 4 ? lo : hi) >> (8 * (b & 3)));
  return r;
}
}  // namespace detail

// One observer read of `bytes` (1..8) registers from `reg`: rd(offset) / wr(offset, value) reach the APID's observer channel (the
// channel's own offsets, kCmd .. kRdata1), delayUs(us) waits. `value` is written only when the answer is ok.
template <class Rd, class Wr, class Delay>
Result readOnce(Rd &&rd, Wr &&wr, Delay &&delayUs, uint8_t reg, uint8_t *value, unsigned bytes, uint32_t *status = nullptr,
                PianoSpmiLock *l = lock())
{
  return detail::readOnce(rd, wr, delayUs, reg, value, bytes, status, l, false);
}

// readOnce, again up to kRetries more times kRetryGapUs apart (the lock free in between) after a failure, a dropped command or a
// timeout; a denied read is final. *retried = the tries after the first.
template <class Rd, class Wr, class Delay>
Result read(Rd &&rd, Wr &&wr, Delay &&delayUs, uint8_t reg, uint8_t *value, unsigned bytes, uint32_t *status = nullptr,
            PianoSpmiLock *l = lock(), unsigned *retried = nullptr)
{
  uint32_t st = 0;
  Result r = detail::readOnce(rd, wr, delayUs, reg, value, bytes, &st, l, false);
  unsigned again = 0;
  while (r != Result::ok && again < kRetries && value && validRange(reg, bytes) && !(r == Result::error && (st & kDenied))) {
    delayUs(kRetryGapUs);
    ++again;
    r = detail::readOnce(rd, wr, delayUs, reg, value, bytes, &st, l, true);
  }
  if (status) *status = st;
  if (retried) *retried = again;
  return r;
}

// For a value whose misreading is dangerous (a peripheral's TYPE/SUBTYPE decides which registers are a regulator's): read() until
// two reads in a row agree, at most `tries` reads (>= 2). ok with `value` = the agreed bytes; error when they never agreed.
template <class Rd, class Wr, class Delay>
Result readStable(Rd &&rd, Wr &&wr, Delay &&delayUs, uint8_t reg, uint8_t *value, unsigned bytes, uint32_t *status = nullptr,
                  PianoSpmiLock *l = lock(), unsigned tries = 4)
{
  uint8_t last[8] = {}, now[8] = {};
  bool have = false;
  Result r = Result::error;
  if (!value || !validRange(reg, bytes)) return Result::error;
  for (unsigned i = 0; i < (tries < 2 ? 2u : tries); ++i) {
    r = read(rd, wr, delayUs, reg, now, bytes, status, l);
    if (r != Result::ok) {
      have = false;
      continue;
    }
    bool same = have;
    for (unsigned b = 0; same && b < bytes; ++b) same = last[b] == now[b];
    if (same) {
      for (unsigned b = 0; b < bytes; ++b) value[b] = now[b];
      return Result::ok;
    }
    if (have) {
      Hold hold(l);   // the count only (inside a hold, as every count)
      if (hold.held()) ++l->unstable;
    }
    for (unsigned b = 0; b < bytes; ++b) last[b] = now[b];
    have = true;
  }
  return r == Result::ok ? Result::error : r;
}

// One owner-channel write of `bytes` (1..8) registers from `reg` (the APID's owner channel: rd/wr at its own offsets): the data,
// then the command, then the poll, all in one hold. Not retried.
template <class Rd, class Wr, class Delay>
Result write(Rd &&rd, Wr &&wr, Delay &&delayUs, uint8_t reg, const uint8_t *value, unsigned bytes, uint32_t *status = nullptr,
             PianoSpmiLock *l = lock())
{
  uint32_t st = 0, lo = 0, hi = 0;
  if (!value || !validRange(reg, bytes)) {
    if (status) *status = 0;
    return Result::error;
  }
  for (unsigned b = 0; b < bytes; ++b) (b < 4 ? lo : hi) |= uint32_t(value[b]) << (8 * (b & 3));
  Result r;
  {
    Hold hold(l);
    wr(kWdata0, lo);
    if (bytes > 4) wr(kWdata1, hi);
    wr(kCmd, command(kOpExtWriteL, reg, bytes));
    r = poll(rd, delayUs, &st);
    count(l, hold, r, false);
  }
  if (status) *status = st;
  return r;
}

// The lock's report line (piano_spmi_format) for this kext: log it once per report.
inline void format(char *out, size_t size, int part = PianoSpmiLineBoth) { piano_spmi_format_part(lock(), out, size, part); }

}  // namespace pianospmi
#endif  // __cplusplus

#endif
