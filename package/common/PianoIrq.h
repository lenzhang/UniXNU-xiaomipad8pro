// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoIrq.h: interrupts for piano's native kexts (SM8750, k27 XNU at EL1 under Gunyah), and the adaptive polling that stands in
// when there is none. Shared by PianoDwc3, PianoInput and PianoTouch; PianoKeyboard's change is described in docs/power-irq.md.
// Facts and the reasoning behind them: docs/power-irq.md. Written for UniXNU-piano; Linux drivers (pinctrl-msm, qcom-pdc,
// spmi-pmic-arb, dwc3) were read as the specification of the hardware only, nothing is taken from their code.
//
// What the kernel offers (verified in kernel-k27-0059-native, the native KC's kernel, by its symbol table and disassembly):
// UniXNU's ArmCpu, linked into the kernel, drives the GICv3 and exports
//   IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router)
//   uint64_t armcpu_spi_count(uint32_t intid)
// attach: INTID 32..1019 (SPI + 32), Group 1, priority 0xa0, edge or level, routed to the boot CPU (GICD_IROUTER), enabled; one
// handler per INTID (a second attach: kIOReturnExclusiveAccess), 16 slots, no detach and no mask (this kernel has no
// armcpu_spi_disable/enable/mask). The handler runs in primary interrupt context before the EOI and must deassert a level source.
// The platform registers no IOInterruptController for SPIs (only ArmCpu's per-CPU one), so IOService::registerInterrupt with
// IOInterruptSpecifiers/IOInterruptControllers on a nub reaches nothing; the stock IOKit workloop shape is kept by a provider-less
// IOInterruptEventSource that the SPI handler triggers (workLoopSource below).
//
// Parts: the facts (INTIDs, the PDC map, the TLMM interrupt registers and their INTR_CFG encoding); the pure state machines the host
// tests run (Backoff, StormGuard, MissGuard, WakeStats); and with KERNEL: a wake-up word (Waiter), one SPI line with a storm guard
// that disables it at the distributor (SpiLine), and TLMM GPIO interrupts through the summary SPI shared by every kext (tlmm::Pin).
// Everything has internal linkage (an anonymous namespace): the KC's native linker does not coalesce weak definitions, and the
// summary dispatcher's state must be the owning kext's own.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pianoirq {
namespace {

// ------------------------------------------------------------------------------------------------------------------- facts
// INTID of a GIC SPI (the DT's <0 N type> is SPI N).
constexpr uint32_t intid(uint32_t spi) { return spi + 32; }

// The GICv3 distributor (DT interrupt-controller@16000000; ADT /arm-io/intc): GICD_ICENABLER for the storm guard.
constexpr uint64_t kGicd = 0x16000000;
constexpr uint32_t kGicdBytes = 0x10000;
constexpr uint32_t gicdIcenabler(uint32_t id) { return 0x180 + 4 * (id / 32); }
constexpr uint32_t gicdBit(uint32_t id) { return 1u << (id % 32); }

// SPIs from piano's DT (all level-high).
constexpr uint32_t kSpiDwc3 = 133;          // usb@a600000 "dwc_usb3": the core's event-buffer interrupt
constexpr uint32_t kSpiDwc3PwrEvent = 130;  // "pwr_event" (QSCRATCH)
constexpr uint32_t kSpiDwc3HsPhy = 131;     // "hs_phy_irq"
constexpr uint32_t kSpiTlmmSummary = 208;   // pinctrl@f100000: every TLMM GPIO whose INTR_CFG targets the apps processor (KPSS)
constexpr uint32_t kSpiQup1Se2 = 355;       // spi@a88000 (the touch controller's serial engine)
constexpr uint32_t kSpiQup1Se6 = 363;       // i2c@a98000 (the keyboard cover's serial engine; PianoKeyboard bit-bangs the pins)

// PDC (interrupt-controller@b220000, qcom,pdc-ranges <pin spi count>): a PDC input pin becomes GIC SPI `spi + (pin - first)`,
// as level-high or rising edge (the PDC inverts falling edges and active-low levels).
struct PdcRange {
  uint16_t pin, spi, count;
};
constexpr PdcRange kPdcRanges[] = {{0, 745, 51}, {51, 527, 47}, {98, 609, 32}, {130, 717, 12}, {142, 251, 5}, {147, 796, 16}};
constexpr int pdcSpi(unsigned pin)
{
  for (const PdcRange &r : kPdcRanges)
    if (pin >= r.pin && pin < unsigned(r.pin) + r.count) return int(r.spi + (pin - r.pin));
  return -1;
}
constexpr unsigned kPdcSpmiArbiter = 1;   // spmi@c400000 "periph_irq" (the PMIC arbiter's summary): PDC pin 1, level-high
// TLMM GPIOs with a PDC input (the SoC's wake map), the ones piano's drivers use: table hall, lid hall, keyboard status (its
// wake request), keyboard data, touch (docs/ubuntu-history/kernel.md §2.3, §3.3).
struct TlmmWake {
  uint16_t gpio, pdcPin;
};
constexpr TlmmWake kTlmmWake[] = {{11, 76}, {80, 122}, {95, 128}, {97, 100}, {162, 109}};
// PDC registers (its DRV region at 0x0b220000), for a later direct route; nothing here programs the PDC: IRQ_i_CFG at
// 0x110 + 4 i, type bits 2:0 (level low 0, falling 2, level high 4, rising 6, both 7), enable bit 3 from version 3.2 on (VERSION
// at 0x1000), before that IRQ_ENABLE_BANK at 0x10 + 4 (i / 32) bit i % 32.
constexpr uint64_t kPdc = 0x0b220000;
constexpr uint32_t pdcIrqCfg(unsigned pin) { return 0x110 + 4 * pin; }
// The PDC's "SPI interface" region (the DT's second reg, 0x64 bytes): one bit per GIC SPI below 800, 1 level, 0 edge, kept by
// firmware; Ubuntu's early wakes on GPIO80/95 came from its level bits (their fix was withdrawn). The summary route below never
// needs it; a direct PDC route would have to read it first.
constexpr uint64_t kPdcSpiCfg = 0x164400f0;
constexpr uint32_t kPdcSpiCfgBytes = 0x64;

namespace tlmm {
// pinctrl@f100000 (sm8750-tlmm, 216 GPIOs, 0x1000 per GPIO): CTL, IO, INTR_CFG, INTR_STATUS.
constexpr uint64_t kBase = 0x0f100000;
constexpr uint32_t kBytes = 0x102000, kStride = 0x1000, kGpios = 216;
constexpr uint32_t kCtl = 0x0, kIo = 0x4, kIntrCfg = 0x8, kIntrStatus = 0xc;
// INTR_CFG: bit 0 enable (to the target's line), 1 polarity, 3:2 detection (0 level, 1 rising, 2 falling, 3 both; every edge
// mode with the polarity bit set), 4 raw status (latch into INTR_STATUS even while not enabled), 6 wakeup present (read only),
// 7 wakeup enable (the PDC route), 10:8 target (3: the apps processor, whose line is the summary SPI). INTR_STATUS bit 0 is the
// latch, cleared by writing 0. Bits not named here are kept as found; the summary route also clears wakeup enable (bit 7), as
// Linux does for a GPIO it does not take through the PDC: the pin then reaches only the summary line, never the PDC (so it is
// no wake source; keyboard data gpio97 must not be one).
constexpr uint32_t kCfgEnable = 1u << 0, kCfgPolarity = 1u << 1, kCfgDetectShift = 2, kCfgDetectMask = 3u << 2, kCfgRawStatus = 1u << 4,
                   kCfgWakeupPresent = 1u << 6, kCfgWakeupEnable = 1u << 7, kCfgTargetShift = 8, kCfgTargetMask = 7u << 8,
                   kTargetKpss = 3;
constexpr uint32_t kCfgOwned = kCfgEnable | kCfgPolarity | kCfgDetectMask | kCfgRawStatus | kCfgWakeupEnable | kCfgTargetMask;
enum class Trigger : uint8_t { levelLow, levelHigh, rising, falling, both };
constexpr bool isEdge(Trigger t) { return t == Trigger::rising || t == Trigger::falling || t == Trigger::both; }
constexpr uint32_t detection(Trigger t)
{
  return t == Trigger::rising ? 1u : t == Trigger::falling ? 2u : t == Trigger::both ? 3u : 0u;
}
// The INTR_CFG word for `t`, from the word found: latched (raw status), targeting the apps processor, not to the PDC, enabled or
// not.
constexpr uint32_t intrCfg(uint32_t found, Trigger t, bool enable)
{
  const bool polarity = t != Trigger::levelLow;   // every edge mode and level-high: polarity 1
  return (found & ~kCfgOwned) | (enable ? kCfgEnable : 0) | (polarity ? kCfgPolarity : 0) | detection(t) << kCfgDetectShift |
         kCfgRawStatus | kTargetKpss << kCfgTargetShift;
}
constexpr bool targetsKpss(uint32_t cfg) { return ((cfg & kCfgTargetMask) >> kCfgTargetShift) == kTargetKpss; }
// The DT's gpio-reserved-ranges <36 4>, <48 4>, <74 1>: not the HLOS's, never touched (not even read).
constexpr bool reserved(unsigned gpio) { return (gpio >= 36 && gpio < 40) || (gpio >= 48 && gpio < 52) || gpio == 74; }
// A GPIO whose interrupt, if enabled to the apps processor, would hold the summary line up for a driver that does not own it.
constexpr bool foreignEnabled(uint32_t cfg) { return (cfg & kCfgEnable) && targetsKpss(cfg); }
}  // namespace tlmm

// -------------------------------------------------------------------------------------------------- the pure state machines
// Activity-driven polling period: fastMs while the last activity is less than holdMs old, then midMs until idleAfterMs more have
// passed, then idleMs. Before the first activity: idleMs.
struct Backoff {
  uint32_t fastMs, midMs, idleMs, holdMs, idleAfterMs;
  uint64_t lastMs;
  bool seen;
  void activity(uint64_t nowMs)
  {
    lastMs = nowMs;
    seen = true;
  }
  unsigned tier(uint64_t nowMs) const   // 0 fast, 1 mid, 2 idle
  {
    if (!seen) return 2;
    const uint64_t since = nowMs >= lastMs ? nowMs - lastMs : 0;
    return since < holdMs ? 0 : since < uint64_t(holdMs) + idleAfterMs ? 1 : 2;
  }
  uint32_t periodMs(uint64_t nowMs) const
  {
    const unsigned t = tier(nowMs);
    return t == 0 ? fastMs : t == 1 ? midMs : idleMs;
  }
};
constexpr Backoff backoff(uint32_t fastMs, uint32_t midMs, uint32_t idleMs, uint32_t holdMs, uint32_t idleAfterMs)
{
  return Backoff{fastMs, midMs, idleMs, holdMs, idleAfterMs, 0, false};
}

// Interrupt-context storm guard: more than `limit` entries that found nothing to do within `window` (clock ticks) trips it; the
// caller then disables the line at the distributor and everyone polls. Single writer: the handler runs on the boot CPU only.
struct StormGuard {
  uint64_t window;
  uint32_t limit;
  uint64_t start;
  uint32_t count;
  bool tripped;
  bool stray(uint64_t now)
  {
    if (tripped) return true;
    if (!count || now - start > window) {
      start = now;
      count = 0;
    }
    if (++count > limit) tripped = true;
    return tripped;
  }
};
constexpr StormGuard stormGuard(uint64_t window, uint32_t limit) { return StormGuard{window, limit, 0, 0, false}; }

// Client-side check of an interrupt that never comes: a wait that timed out while the source was armed and its status pending is
// a miss; `limit` misses in a row declare the interrupt dead (the client polls from then on). A delivered interrupt resets the run.
struct MissGuard {
  uint32_t limit, run, total;
  bool dead;
  void delivered() { run = 0; }
  bool miss()
  {
    ++total;
    if (++run >= limit) dead = true;
    return dead;
  }
};
constexpr MissGuard missGuard(uint32_t limit) { return MissGuard{limit, 0, 0, false}; }

// Loop wake-ups per window (default 60 s), for the rate-limited "<driver>: idle: N wakeups in 60 s, irq M" line.
enum class Woke : uint8_t { timeout, irq, kick };
struct WakeStats {
  uint32_t windowMs;
  uint64_t startMs;
  bool started;
  uint32_t wakeups, irqs, kicks, timeouts;
  uint64_t totalWakeups, totalIrqs;
  struct Window {
    uint32_t seconds, wakeups, irqs, kicks, timeouts;
  };
  void count(Woke why)
  {
    ++wakeups;
    ++totalWakeups;
    if (why == Woke::irq) { ++irqs; ++totalIrqs; }
    else if (why == Woke::kick) ++kicks;
    else ++timeouts;
  }
  // True once a window has ended at nowMs: *out gets its counts and a new window starts. windowMs 0: never.
  bool due(uint64_t nowMs, Window *out)
  {
    if (!windowMs) return false;
    if (!started) {
      started = true;
      startMs = nowMs;
      return false;
    }
    if (nowMs < startMs || nowMs - startMs < windowMs) return false;
    *out = Window{uint32_t((nowMs - startMs + 500) / 1000), wakeups, irqs, kicks, timeouts};
    startMs = nowMs;
    wakeups = irqs = kicks = timeouts = 0;
    return true;
  }
};
constexpr WakeStats wakeStats(uint32_t windowMs) { return WakeStats{windowMs, 0, false, 0, 0, 0, 0, 0, 0}; }

}  // namespace
}  // namespace pianoirq

// ------------------------------------------------------------------------------------------------------------------ kernel
#ifdef KERNEL
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                                      uint64_t *router);
extern "C" uint64_t armcpu_spi_count(uint32_t intid);

namespace pianoirq {
namespace {

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory"); }
inline void dsb() { __asm__ volatile("dsb sy" ::: "memory"); }
inline void bump(volatile uint64_t &counter) { __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED); }
inline uint64_t nowMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}
inline uint32_t bootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
// piano-wake-log=S: the period of every converted driver's wake-up line, in seconds (default 60, 0 none).
inline uint32_t wakeLogMs() { return bootArg("piano-wake-log", 60) * 1000; }

// One mapping of device memory (uncached), kept for the life of the kext.
inline uintptr_t mapDevice(uint64_t pa, uint64_t bytes)
{
  const uint64_t base = pa & ~0xfffull, length = ((pa + bytes + 0xfff) & ~0xfffull) - base;
  IODeviceMemory *m = IODeviceMemory::withRange(base, length);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  return map ? uintptr_t(map->getVirtualAddress()) + uintptr_t(pa - base) : 0;
}

// A wake-up word: an interrupt handler, or a thread with work for the waiter, moves the generation and wakes; the waiter sleeps
// until the generation moves or its timeout ends (timeouts of 20 ms and more with 1/8 leeway, for the kernel's timer coalescing).
inline uint32_t generation(const volatile uint32_t *gen) { return __atomic_load_n(gen, __ATOMIC_ACQUIRE); }
inline void kick(volatile uint32_t *gen)   // any context, also primary interrupt context
{
  __atomic_add_fetch(gen, 1, __ATOMIC_RELEASE);
  thread_wakeup_prim((event_t)(uintptr_t)gen, FALSE, THREAD_AWAKENED);
}
// True: woken (the generation is not `seen` any more); false: the timeout ended.
inline bool waitFor(volatile uint32_t *gen, uint32_t seen, uint32_t ms)
{
  if (generation(gen) != seen) return true;
  const uint32_t leeway = ms >= 20 ? ms / 8 : 0;
  assert_wait_timeout_with_leeway((event_t)(uintptr_t)gen, THREAD_UNINT,
                                  leeway ? TIMEOUT_URGENCY_SYS_NORMAL | TIMEOUT_URGENCY_LEEWAY : TIMEOUT_URGENCY_SYS_NORMAL, ms ? ms : 1,
                                  leeway, NSEC_PER_MSEC);
  if (generation(gen) != seen) {
    clear_wait(current_thread(), THREAD_AWAKENED);
    return true;
  }
  const wait_result_t r = thread_block(THREAD_CONTINUE_NULL);
  return r == THREAD_AWAKENED || generation(gen) != seen;
}
struct Waiter {
  volatile uint32_t gen;
  uint32_t now() const { return generation(&gen); }
  void kick() { pianoirq::kick(&gen); }
  bool wait(uint32_t seen, uint32_t ms) { return waitFor(&gen, seen, ms); }
};

// One GIC SPI through ArmCpu. The owner's handler reports entries that found nothing to deassert (stray()); a storm of them
// disables the line at the distributor (GICD_ICENABLER, write-one-to-clear: no read-modify-write against ArmCpu's own writes),
// after which live() is false and the owner polls.
struct SpiLine {
  uint32_t id;
  bool edge;
  volatile bool up, tripped;
  IOReturn result;
  uint32_t readback;   // ArmCpu's readback: bit 0 Group 1, bit 1 enabled, bit 2 edge, bits 15:8 priority
  uint64_t router;
  uintptr_t gicd;
  StormGuard guard;
  volatile uint64_t strays;

  bool live() const { return up && !tripped; }
  uint64_t count() const { return up ? armcpu_spi_count(id) : 0; }
  IOReturn attach(uint32_t intidValue, bool edgeValue, void (*handler)(void *), void *context)
  {
    id = intidValue;
    edge = edgeValue;
    uint64_t window = 0;
    nanoseconds_to_absolutetime(100 * 1000 * 1000ull, &window);   // more than 2000 strays in 100 ms
    guard = stormGuard(window, 2000);
    if (!gicd) gicd = mapDevice(kGicd, kGicdBytes);
    if (!gicd) return result = kIOReturnNoMemory;   // no guard, no line
    result = armcpu_spi_attach(id, edge, handler, context, &readback, &router);
    up = result == kIOReturnSuccess;
    return result;
  }
  void disable()
  {
    tripped = true;
    if (gicd) {
      wr32(gicd + gicdIcenabler(id), gicdBit(id));
      dsb();
    }
  }
  // From the handler: an entry with nothing to deassert. True once the line has been disabled.
  bool stray()
  {
    bump(strays);
    if (tripped) return true;
    if (guard.stray(mach_absolute_time())) disable();
    return tripped;
  }
};

// The stock workloop shape on a raw SPI: an IOInterruptEventSource without a provider on the owner's work loop; the SPI handler calls
// source->interruptOccurred(nullptr, nullptr, 0) (safe in primary interrupt context), and the action runs on the work-loop thread
// with IOKit's own counting (several interrupts before it runs: one call, their count). For drivers built around a work loop;
// the thread-based drivers here use Waiter.
inline IOInterruptEventSource *workLoopSource(OSObject *owner, IOWorkLoop *loop, IOInterruptEventSource::Action action)
{
  IOInterruptEventSource *source = IOInterruptEventSource::interruptEventSource(owner, action, nullptr, 0);
  if (source && loop->addEventSource(source) != kIOReturnSuccess) {
    source->release();
    source = nullptr;
  }
  return source;
}

// ----------------------------------------------------------------------------------- TLMM GPIO interrupts (the summary SPI)
// Every TLMM GPIO enabled with target 3 feeds one level-high summary line, SPI 208, and an INTID has one handler: the first kext
// whose armcpu_spi_attach succeeds owns the line and a table in an OSData published as the resource "PianoTlmmIrq" (its bytes
// are the table: counters and GPIO numbers, no pointer); the others find it and claim a slot. The owner's handler checks each
// claimed GPIO: enabled and latched -> it writes the slot's masked INTR_CFG (the enable bit off: the pin stops holding the line)
// and moves the slot's wake-up word. The client consumes the latch as with polling (INTR_STATUS <- 0) and arms again.
// Before attaching, the owner reads INTR_CFG of every GPIO the HLOS may touch: one enabled to the apps processor that no client
// owns would hold the line, so then nothing is attached (logged; the clients poll).
namespace tlmm {
constexpr uint32_t kMagic = 0x50544951u /* 'PTIQ' */, kVersion = 1;
constexpr unsigned kSlots = 8;
constexpr const char *kResource = "PianoTlmmIrq";
enum : uint32_t { kStateNone = 0, kStateUp = 1, kStateStorm = 2, kStateRefused = 3 };
struct Shared {
  uint32_t magic, version, intid, foreign;   // foreign: the first GPIO found enabled to the apps processor + 1 (0 none)
  volatile uint32_t state;
  volatile uint32_t claimed;            // slot bitmap
  volatile uint32_t gpio1[kSlots];      // the slot's GPIO + 1 once it is set up (0: free)
  volatile uint32_t masked[kSlots];     // its INTR_CFG with the enable bit off, written by the handler
  volatile uint32_t gen[kSlots];        // its wake-up word: handler entries that found it pending
  volatile uint64_t entries, strays;
  char owner[16];
};

// The owner's side (only the owning kext's copy is ever used by the handler).
struct Owner {
  uintptr_t va;   // the whole TLMM block
  SpiLine line;
};
inline Owner &owner()
{
  static Owner o;   // zero-initialized: no guard
  return o;
}

inline void summaryHandler(void *context)
{
  Shared *s = static_cast<Shared *>(context);
  Owner &o = owner();
  bump(s->entries);
  bool ours = false;
  for (unsigned i = 0; i < kSlots; ++i) {
    const uint32_t g = __atomic_load_n(&s->gpio1[i], __ATOMIC_ACQUIRE);
    if (!g) continue;
    const uintptr_t page = o.va + uintptr_t(g - 1) * kStride;
    if (!(rd32(page + kIntrCfg) & kCfgEnable) || !(rd32(page + kIntrStatus) & 1)) continue;
    wr32(page + kIntrCfg, s->masked[i]);
    ours = true;
    kick(&s->gen[i]);
  }
  dsb();
  if (ours) return;
  bump(s->strays);
  if (o.line.stray()) {
    s->state = kStateStorm;
    for (unsigned i = 0; i < kSlots; ++i) kick(&s->gen[i]);
  }
}

// The table, found or made (the owner attaches the line). Null: none can be had (why says it).
inline Shared *shared(const char *who, char *why, size_t whySize)
{
  IOService *resources = IOService::getResourceService();
  if (!resources) {
    snprintf(why, whySize, "no IOResources");
    return nullptr;
  }
  for (unsigned attempt = 0;; ++attempt) {
    if (OSData *d = OSDynamicCast(OSData, resources->getProperty(kResource))) {
      Shared *s = d->getLength() == sizeof(Shared) ? static_cast<Shared *>(const_cast<void *>(d->getBytesNoCopy())) : nullptr;
      if (!s || s->magic != kMagic || s->version != kVersion) {
        snprintf(why, whySize, "%s has another layout", kResource);
        return nullptr;
      }
      return s;
    }
    if (attempt) {   // another kext is attaching: its table appears within a few milliseconds
      if (attempt > 300) {
        snprintf(why, whySize, "INTID %u is taken but %s never appeared", intid(kSpiTlmmSummary), kResource);
        return nullptr;
      }
      IOSleep(10);
      continue;
    }
    Shared zero = {};
    OSData *d = OSData::withBytes(&zero, sizeof zero);
    if (!d) {
      snprintf(why, whySize, "no memory");
      return nullptr;
    }
    Shared *s = static_cast<Shared *>(const_cast<void *>(d->getBytesNoCopy()));
    s->magic = kMagic;
    s->version = kVersion;
    s->intid = intid(kSpiTlmmSummary);
    strlcpy(s->owner, who, sizeof s->owner);
    Owner &o = owner();
    if (!o.va) o.va = mapDevice(kBase, kBytes);
    if (!o.va) {
      d->release();
      snprintf(why, whySize, "the TLMM block does not map");
      return nullptr;
    }
    for (unsigned g = 0; g < kGpios && !s->foreign; ++g)
      if (!reserved(g) && foreignEnabled(rd32(o.va + g * kStride + kIntrCfg))) s->foreign = g + 1;
    // A table published while this one was being made (its owner's clients arm only after it is published, so a GPIO they
    // armed shows up here only with their table already there): take that one.
    if (resources->getProperty(kResource)) {
      d->release();
      continue;
    }
    if (s->foreign) {
      s->state = kStateRefused;   // published anyway, so the next client does not scan again
    } else {
      const IOReturn r = o.line.attach(s->intid, false, summaryHandler, s);
      if (r == kIOReturnExclusiveAccess) {   // another kext won the line: use its table
        d->release();
        continue;
      }
      s->state = r == kIOReturnSuccess ? kStateUp : kStateRefused;
      if (r != kIOReturnSuccess) snprintf(why, whySize, "armcpu_spi_attach(%u): %#x", s->intid, r);
    }
    IOService::publishResource(kResource, d);
    d->release();
    return s;
  }
}

// One TLMM GPIO's interrupt. The caller has the GPIO's page mapped (pageVa: CTL at +0) and keeps consuming its latch; open() sets
// INTR_CFG to latched-not-enabled (what the polled drivers did) and claims a slot; arm() enables it, the handler masks it again.
struct Pin {
  Shared *s;
  unsigned slot, gpio;
  uintptr_t page;
  uint32_t armed, masked;
  bool ok;

  bool live() const { return ok && s->state == kStateUp; }
  uint32_t gen() const { return ok ? generation(&s->gen[slot]) : 0; }
  bool latched() const { return rd32(page + kIntrStatus) & 1; }
  void clear()
  {
    wr32(page + kIntrStatus, 0);
    dsb();
  }
  void arm()
  {
    wr32(page + kIntrCfg, armed);
    dsb();
  }
  void mask()
  {
    wr32(page + kIntrCfg, masked);
    dsb();
  }
  // True: the handler fired for this pin (or the line went away: live() then says which).
  bool wait(uint32_t seen, uint32_t ms) { return ok && waitFor(&s->gen[slot], seen, ms); }
  // Latched, not enabled, as before, also without the interrupt path.
  void latchOnly(uintptr_t pageVa, Trigger t)
  {
    page = pageVa;
    masked = intrCfg(rd32(pageVa + kIntrCfg), t, false);
    armed = masked | kCfgEnable;
    mask();
    clear();
  }
  // false: no interrupt (why says it); the pin is then still latched-not-enabled, for polling.
  bool open(const char *who, unsigned gpioValue, uintptr_t pageVa, Trigger t, char *why, size_t whySize)
  {
    ok = false;
    gpio = gpioValue;
    if (whySize) why[0] = 0;
    latchOnly(pageVa, t);
    if (gpio >= kGpios || reserved(gpio)) {
      snprintf(why, whySize, "gpio%u is not the HLOS's", gpio);
      return false;
    }
    s = shared(who, why, whySize);
    if (!s) return false;
    if (s->state != kStateUp) {
      if (s->foreign) snprintf(why, whySize, "gpio%u is enabled to the apps processor by someone else: the summary line stays off", s->foreign - 1);
      else if (!why[0]) snprintf(why, whySize, "the summary line is %s", s->state == kStateStorm ? "disabled (storm)" : "not attached");
      return false;
    }
    for (unsigned i = 0; i < kSlots; ++i) {
      const uint32_t bit = 1u << i, before = __atomic_fetch_or(&s->claimed, bit, __ATOMIC_ACQ_REL);
      if (before & bit) continue;
      slot = i;
      s->masked[i] = masked;
      __atomic_store_n(&s->gpio1[i], gpio + 1, __ATOMIC_RELEASE);
      ok = true;
      return true;
    }
    snprintf(why, whySize, "no free slot (%u in use)", kSlots);
    return false;
  }
};
}  // namespace tlmm

}  // namespace
}  // namespace pianoirq
#endif  // KERNEL
