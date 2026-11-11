// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal in the kernel: the TSENS upper/lower threshold ("uplow") interrupts, off unless piano-thermal-irq=1 (and PianoThermal
// runs: piano-thermal >= 1). The design and its state machine are TsensIrq.h's (host-tested by tests/uplow_host.cpp); this file is
// the kernel's side of it: the four GIC SPIs 771-774 through ArmCpu (package/common/PianoIrq.h's SpiLine: level, the storm guard),
// the handler (masks what is up and wakes the poll thread), the mask lock shared with it, the wait PianoThermal's pollWait hands
// over to while the path is live (an interrupt or the system-sleep park request ends it), the wake-up line, the IORegistry
// property UplowIrq. docs/drivers/thermal.md section 13.
//   piano-thermal-irq=1            the path: a proof on each controller at the first poll, then the interrupts; any failure, a
//                                  missed or stormy interrupt: off for the boot, logged once, today's polling
//   piano-thermal-irq-up / -down   the window, C (85 / 70; up 50..104, down 30..up-5)
//   piano-thermal-irq-poll-ms      the base poll at rest (5000; 1000..60000; 0: only the report's polls, piano-thermal-log-s)
//   piano-thermal-irq-hold-ms      the fast tier after an upper interrupt (1000; 100..10000)
//   piano-thermal-irq-test=N       the synthetic test: per controller one sensor's upper threshold N tenths of a degree above its
//                                  reading (1..200), for piano-thermal-irq-test-s (120; 10..600), then the window back
// Writes (TM ranges only): TM_Sn_UPPER_LOWER_THRESHOLD of the armed sensors, TM_UPPER_LOWER_INT_MASK and _CLEAR, TM_INT_EN (to 7,
// only when found otherwise); with the fallback all of them back as found except the mask (every up/low interrupt masked); and
// GICD_ICENABLER (SpiLine: a storm, or the fallback). Never: the critical thresholds and interrupts, SROT.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "PianoThermalIrq.h"
#include "TsensIrq.h"

using namespace pianothermal;

namespace {

uint64_t nowMsBoot()
{
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000ull;
}
uint64_t gStartMs;
uint64_t rel(uint64_t bootMs) { return bootMs > gStartMs ? bootMs - gStartMs : 0; }

void noteV(const char *fmt, va_list ap)
{
  char line[600];
  vsnprintf(line, sizeof line, fmt, ap);
  IOLog("PianoThermal: %s\n", line);
}
void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  noteV(fmt, ap);
  va_end(ap);
}

uint32_t arg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v;
}
uint32_t clampU(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(a) : "memory"); }

// ------------------------------------------------------------------------------------------------------------- state
struct Ctl {
  uintptr_t va;   // the controller's TM range (PianoThermal.cpp's mapping)
  unsigned c;
  pianoirq::SpiLine line;
  volatile uint32_t events;   // bits the handler masked, for the poll thread
  volatile uint64_t entries;
};
Ctl gCtl[kControllers];
IOSimpleLock *gLock;
pianoirq::Waiter gWake;
uplow::Machine gMachine;
IOService *gSvc;
uint32_t gLevel, gSeen;
bool gStarted;
uplow::Phase gPublished = uplow::Phase::off;
pianoirq::WakeStats gWakes;
uint64_t gRestMs;
volatile uint64_t gHandled;   // handler entries that masked something (tells an interrupt's wake-up from a kick)

uint32_t regRead(void *, unsigned c, uint32_t off)
{
  return c < kControllers && gCtl[c].va && off + 4 <= kTmBytes ? rd32(gCtl[c].va + off) : 0xffffffffu;
}
void regWrite(void *, unsigned c, uint32_t off, uint32_t v)
{
  if (c >= kControllers || !gCtl[c].va || off + 4 > kTmBytes) return;
  wr32(gCtl[c].va + off, v);
  __builtin_arm_dsb(0xf);
}
const uplow::Regs kRegs = {nullptr, regRead, regWrite};

// The uplow line of one controller (primary interrupt context, the boot CPU): mask what is up, wake the thread; a stray counts for
// the storm guard, which disables the line at the GIC (the thread then falls back).
void uplowHandler(void *context)
{
  Ctl &k = *static_cast<Ctl *>(context);
  pianoirq::bump(k.entries);
  IOSimpleLockLock(gLock);
  const uplow::Serviced sv = uplow::service(kRegs, k.c, kMap[k.c].count);
  IOSimpleLockUnlock(gLock);
  if (sv.bits) {
    __atomic_or_fetch(&k.events, sv.bits, __ATOMIC_RELEASE);
    pianoirq::bump(gHandled);
    gWake.kick();
    return;
  }
  if (k.line.stray()) gWake.kick();
}

// ------------------------------------------------------------------------------------------------------------- the machine's Env
bool envMapped(void *, unsigned c) { return c < kControllers && gCtl[c].va; }
uintptr_t envLock(void *)
{
  const boolean_t was = ml_set_interrupts_enabled(FALSE);
  IOSimpleLockLock(gLock);
  return uintptr_t(was);
}
void envUnlock(void *, uintptr_t token)
{
  __builtin_arm_dsb(0xf);
  IOSimpleLockUnlock(gLock);
  ml_set_interrupts_enabled(boolean_t(token));
}
uint32_t envTake(void *, unsigned c) { return __atomic_exchange_n(&gCtl[c].events, 0u, __ATOMIC_ACQ_REL); }
bool envWait(void *, unsigned c, uint32_t bits, uint32_t ms)
{
  const uint64_t deadline = nowMsBoot() + ms;
  for (;;) {
    const uint32_t seen = gWake.now();
    if (envTake(nullptr, c) & bits) return true;
    const uint64_t now = nowMsBoot();
    if (now >= deadline) return false;
    gWake.wait(seen, uint32_t(deadline - now));
  }
}
bool envAttach(void *, unsigned c)
{
  Ctl &k = gCtl[c];
  const uint32_t id = pianoirq::intid(uplow::kSpiUplow[c]);
  const IOReturn r = k.line.attach(id, false, uplowHandler, &k);
  const uint32_t rb = k.line.readback;
  note("uplow: tsens%u %s: SPI %u (INTID %u, level) %s (%#x): readback %#x (group 1 %u, enabled %u, edge %u, priority %#x), router %#llx",
       c, kMap[c].name, uplow::kSpiUplow[c], id, r == kIOReturnSuccess ? "attached" : "NOT attached", r, rb, rb & 1, (rb >> 1) & 1,
       (rb >> 2) & 1, (rb >> 8) & 0xff, (unsigned long long)k.line.router);
  return r == kIOReturnSuccess;
}
bool envLineLive(void *, unsigned c) { return gCtl[c].line.live(); }
void envDisable(void *, unsigned c)
{
  if (gCtl[c].line.up && !gCtl[c].line.tripped) gCtl[c].line.disable();
}
uint64_t envCount(void *, unsigned c) { return gCtl[c].line.count(); }
uint64_t envNow(void *) { return rel(nowMsBoot()); }
void envNote(void *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void envNote(void *, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  noteV(fmt, ap);
  va_end(ap);
}
const uplow::Env kEnv = {nullptr, kRegs, envMapped, envLock, envUnlock, envTake, envWait, envAttach, envLineLive, envDisable,
                         envCount, envNow, envNote};

void publish()
{
  const uplow::Phase p = gMachine.phase();
  if (!gSvc || p == gPublished) return;
  gPublished = p;
  char text[200];
  if (p == uplow::Phase::failed) snprintf(text, sizeof text, "off: %s", gMachine.why());
  else snprintf(text, sizeof text, "%s", uplow::phaseName(p));
  gSvc->setProperty("UplowIrq", text);
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------- the hook
uint32_t pianothermal::irq::hookLevel() { return arg("piano-thermal-irq", 0) ? 1u : 0u; }

void pianothermal::irq::hookStart(IOService *svc, const uintptr_t tm[kControllers], uint32_t baseMs, bool governing, uint32_t logS,
                                  uint64_t startNs)
{
  gLevel = hookLevel();
  if (!gLevel) return;
  gSvc = svc;
  gStartMs = startNs / 1000000ull;
  for (unsigned c = 0; c < kControllers; ++c) {
    gCtl[c].va = tm[c];
    gCtl[c].c = c;
  }
  gLock = IOSimpleLockAlloc();
  if (!gLock) {
    note("uplow: no lock: piano-thermal-irq off");
    gLevel = 0;
    return;
  }
  uplow::Config cfg;
  const uint32_t up = clampU(arg("piano-thermal-irq-up", 85), 50, 104);
  const uint32_t down = clampU(arg("piano-thermal-irq-down", 70), 30, up - 5);
  cfg.upperDeci = int32_t(up) * 10;
  cfg.lowerDeci = int32_t(down) * 10;
  const uint32_t restMs = arg("piano-thermal-irq-poll-ms", 5000);
  cfg.restPollMs = restMs ? clampU(restMs, 1000, 60000) : logS * 1000;
  cfg.holdMs = clampU(arg("piano-thermal-irq-hold-ms", 1000), 100, 10000);
  cfg.testDeci = clampU(arg("piano-thermal-irq-test", 0), 0, 200);
  cfg.testMs = clampU(arg("piano-thermal-irq-test-s", 120), 10, 600) * 1000;
  cfg.governing = governing;
  gMachine.configure(cfg);
  gWakes = pianoirq::wakeStats(pianoirq::wakeLogMs());
  note("uplow: piano-thermal-irq=1: the TSENS upper/lower threshold interrupts (SPI 771-774, level), upper %u C / lower %u C on every "
       "sensor; at rest the base poll is %u ms (%s), the warm tier and the fast-clock rule stand down; an upper interrupt polls every "
       "10 ms for %u ms; a proof on each controller first%s",
       up, down, governing ? baseMs : cfg.restPollMs, governing ? "the governor's tick" : restMs ? "piano-thermal-irq-poll-ms" : "the report period",
       cfg.holdMs, cfg.testDeci ? ", then the synthetic test (piano-thermal-irq-test)" : "");
  publish();
}

void pianothermal::irq::hookPolled(const Monitor &m, uint64_t nowMs)
{
  if (!gLevel) return;
  gSeen = gWake.now();   // before the records are taken: an interrupt from here on ends the next sleep at once
  const uint64_t now = rel(nowMs);
  if (!gStarted) {
    gStarted = true;
    gMachine.start(kEnv, m, now);
  } else {
    gMachine.step(kEnv, m, now);
  }
  publish();
  pianoirq::WakeStats::Window w;
  if (gWakes.due(now, &w)) {
    note("idle: %u wakeups in %u s, irq %u (timer %u, kick %u; uplow %s%s, at rest %llu s; SPI 771-774 counts %llu %llu %llu %llu; %llu "
         "interrupts, %u misses)", w.wakeups, w.seconds, w.irqs, w.timeouts, w.kicks, uplow::phaseName(gMachine.phase()),
         gMachine.rest() ? " at rest" : "",
         (unsigned long long)(gRestMs / 1000), (unsigned long long)gCtl[0].line.count(), (unsigned long long)gCtl[1].line.count(),
         (unsigned long long)gCtl[2].line.count(), (unsigned long long)gCtl[3].line.count(), (unsigned long long)gMachine.events(),
         gMachine.misses());
    gRestMs = 0;
  }
}

Poll pianothermal::irq::hookTier(Poll polled, Poll atRest, uint64_t nowMs)
{
  return gLevel ? gMachine.tier(polled, atRest, rel(nowMs)) : polled;
}
uint32_t pianothermal::irq::hookBase(uint32_t baseMs) { return gLevel ? gMachine.baseMs(baseMs) : baseMs; }
bool pianothermal::irq::hookHolding(uint64_t nowMs) { return gLevel && gMachine.holding(rel(nowMs)); }

bool pianothermal::irq::hookSleep(uint32_t ms, const volatile bool *stop)
{
  if (!gLevel) return false;
  if (!gMachine.live()) {   // fell back: the caller's own wait, counted as a timer wake-up for the line
    if (gMachine.phase() == uplow::Phase::failed) gWakes.count(pianoirq::Woke::timeout);
    return false;
  }
  // gSeen is from before this poll's records were taken: an interrupt or a kick since then ends the wait at once
  if (stop && *stop) return true;
  const bool rest = gMachine.rest();
  const uint64_t t0 = nowMsBoot(), handled = __atomic_load_n(&gHandled, __ATOMIC_ACQUIRE);
  const bool woke = gWake.wait(gSeen, ms);
  if (rest) gRestMs += nowMsBoot() - t0;
  const bool byIrq = __atomic_load_n(&gHandled, __ATOMIC_ACQUIRE) != handled;
  gWakes.count(byIrq ? pianoirq::Woke::irq : woke ? pianoirq::Woke::kick : pianoirq::Woke::timeout);
  return true;
}
void pianothermal::irq::hookKick() { gWake.kick(); }

int pianothermal::irq::hookSuspendPrepare()
{
  if (!gLevel || !gLock) return -1;
  return gMachine.suspendPrepare(kEnv);
}
bool pianothermal::irq::hookSuspendFinish()
{
  if (!gLevel || !gLock) return false;
  return gMachine.suspendFinish(kEnv);
}
