// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal's uplow-interrupt hook (PianoThermalIrq.cpp, piano-thermal-irq=1): the calls PianoThermal.cpp makes. Every call is
// today's behaviour unchanged while piano-thermal-irq is 0 (the default). docs/drivers/thermal.md section 13.
#ifndef PIANO_THERMAL_IRQ_HOOK_H
#define PIANO_THERMAL_IRQ_HOOK_H
#include <stdint.h>
#include "Cluster.h"

class IOService;

namespace pianothermal::irq {
// piano-thermal-irq (0 off, the default; 1 the TSENS upper/lower threshold interrupts).
uint32_t hookLevel();
// From the poll thread once the TM windows are mapped (tm[c]: controller c's TM range, 0 when not mapped): the boot arguments, the
// lock; the proof and the arming follow on the first poll. baseMs: the loop's base period; governing: that base is the governor's
// tick (never relaxed); logS: the report period ("off at rest"); startNs: the kext's start, for the [Ns] of the lines.
void hookStart(IOService *svc, const uintptr_t tm[kControllers], uint32_t baseMs, bool governing, uint32_t logS, uint64_t startNs);
// After every poll (nowMs: since boot): the proof and the arming (first call), the handler's records, state changes, misses, the
// test, the wake-up line.
void hookPolled(const Monitor &m, uint64_t nowMs);
// The poll tier: `polled` (today's rules) or, at rest, `atRest` (the same without the warm rule and the fast-clock rule); fast while
// an upper interrupt holds it.
Poll hookTier(Poll polled, Poll atRest, uint64_t nowMs);
// The base period: relaxed at rest (not the governor's tick).
uint32_t hookBase(uint32_t baseMs);
// True while an upper interrupt (or a suspect reading) holds the fast tier.
bool hookHolding(uint64_t nowMs);
// The poll thread's wait while the interrupt is live: up to ms, ended at once by an uplow interrupt or when *stop is set (the
// system-sleep park request, which also calls hookKick). False when the path is not live: the caller waits its own way (today's).
bool hookSleep(uint32_t ms, const volatile bool *stop);
// Ends a hookSleep in progress (any context).
void hookKick();
// The suspend hooks while the interrupt is live: every lower interrupt masked, the upper ones kept armed; the number of controllers,
// -1 when not live (the caller masks as before). Finish: true when the prepare was this path's (its masks are back).
int hookSuspendPrepare();
bool hookSuspendFinish();
}  // namespace pianothermal::irq
#endif
