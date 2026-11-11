// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu's writing slices in the kernel: B1c refresh (DpuRefresh.h), B1d Night Light (DpuPcc.h), B1e panel sleep (DpuPanel.h);
// docs/drivers/dpu.md §4.3-4.5 (design), §6 (arguments), §8 (test images and expected lines). Each slice is inert without its own
// argument, and all of them need piano-dpu >= 1: the read-only survey (PianoDpu.cpp) runs first and is waited for, so every boot
// that writes also logs what it found. One thread (this file's worker) does every store, one sequence at a time; the platform
// functions below only record a request and wake it.
//   piano-dpu-refresh=plan|60|120|auto   plan: the verdict and the stores a switch would make, nothing written; 60 / 120: switch
//                                        once at the start and stay; auto: the idle policy (DpuRefresh.h) fed by NativeGopDisplay's
//                                        swaps (it calls PianoDpuSwap when it has the same argument)
//   piano-dpu-refresh-db=0|1|2           the DSI timing shadow: 1 SunUEFI's place (default), 2 downstream's, 0 none (TOTAL directly)
//   piano-dpu-refresh-test=N             first N round trips 120 -> 60 -> 120, one second at each rate (stops at an anomaly)
//   piano-dpu-refresh-idle=MS, -burst=N, -window=MS, -dwell=MS   the policy (1000, 2, 250, 200)
//   piano-dpu-pcc=1|2                    1: the PCC as found (the identity, normally) written once at the start, then macOS's colour
//                                        matrices (NativeGopDisplay forwards swap_set_color_matrix); 2: also its gamma tables, fitted
//   piano-dpu-pcc-encode=0               a diagonal as macOS sends it (default: from linear light to the encoded values)
//   piano-dpu-pcc-negative=1             negative coefficients as two's complement S3.15 (default: clamped to 0)
//   piano-dpu-pcc-test=R,G,B             a diagonal in thousandths written as the PCC's values after the start (e.g. 1000,1000,500)
//   piano-dpu-panel=1                    the panel's light sleep with display sleep (NativeGopDisplay calls PianoDpuPanel)
//   piano-dpu-panel-engines=0            DCS only: the timing engines keep running
//   piano-dpu-panel-hs=1                 DCS in high speed (default: low power, the panel's MIPI_DSI_MODE_LPM)
//   piano-dpu-panel-test=N               first N sleep/wake cycles, 5 s asleep and 5 s awake each (stops at a failure)
// Platform functions (IOService::callPlatformFunction on the PianoDpu service, registered when a slice is on; no link dependency):
//   "PianoDpuSwap"          no parameters: a frame was shown
//   "PianoDpuColorMatrix"   p1 = the 72 bytes of an IOMFBColorFixedMatrix (9 uint64_t), p2 = its IOMFBColorMatrixFunction, p3 = the
//                           call's third argument (logged)
//   "PianoDpuGammaTable"    p1 = the table's 32-bit words, p2 = how many (fitted when piano-dpu-pcc=2 and 771 words)
//   "PianoDpuPanel"         p1 = on (0 / 1): NativeGopDisplay's display power, on each change
// Every store goes through DpuSlices.h's guard (the registers of the three slices only: never TLMM, so never the panel's reset or
// supply pins, never the backlight). Loads and stores use this file's own page maps, all made before the first store; nothing is
// mapped while interrupts are off.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include "DpuRefresh.h"
#include "DpuPcc.h"
#include "DpuPanel.h"

using namespace pianodpu;
using namespace pianodpu::slices;

extern "C" {
extern volatile int32_t piano_dpu_stage;          // PianoDpu.cpp's survey: 5 when done (B1b's steps 6..8 follow), negative when failed
volatile int32_t piano_dpu_slices_stage = 0;      // this file's breadcrumb: the step reached, negative when it failed there
}

// The interface PianoDpu.cpp calls (declared there).
bool dpuSlicesWanted();
void dpuSlicesStart(IOService *dpu);
bool dpuSlicesPlatformFunction(const OSSymbol *name, void *p1, void *p2, void *p3, IOReturn *result);

namespace {

constexpr unsigned kRefreshOff = 0, kRefreshAuto = 1, kRefreshPlan = 2;

struct SliceArgs {
  unsigned level = 0;
  unsigned refresh = kRefreshOff, refreshDb = 1, refreshTest = 0;
  refresh::PolicyConfig policy;
  unsigned pcc = 0, pccEncode = 1, pccNegative = 0;
  bool pccTest = false;
  int32_t pccTestMilli[3] = {1000, 1000, 1000};
  unsigned panel = 0, panelEngines = 1, panelHs = 0, panelTest = 0;
};
SliceArgs gA;
bool gArgsRead;

uint32_t argNumber(const char *name, uint32_t fallback, uint32_t lo, uint32_t hi)
{
  uint32_t v = fallback;
  if (!PE_parse_boot_argn(name, &v, sizeof v)) v = fallback;
  return v < lo ? lo : v > hi ? hi : v;
}

void readSliceArgs()
{
  if (gArgsRead) return;
  gArgsRead = true;
  gA.level = argNumber("piano-dpu", 0, 0, 100);
  char s[32] = {};
  if (PE_parse_boot_arg_str("piano-dpu-refresh", s, sizeof s)) {
    if (!strcmp(s, "auto")) gA.refresh = kRefreshAuto;
    else if (!strcmp(s, "plan")) gA.refresh = kRefreshPlan;
    else if (!strcmp(s, "60")) gA.refresh = 60;
    else if (!strcmp(s, "120")) gA.refresh = 120;
  }
  gA.refreshDb = argNumber("piano-dpu-refresh-db", 1, 0, 2);
  gA.refreshTest = argNumber("piano-dpu-refresh-test", 0, 0, 1000);
  gA.policy.idleMs = argNumber("piano-dpu-refresh-idle", 1000, 100, 60000);
  gA.policy.burstSwaps = argNumber("piano-dpu-refresh-burst", 2, 1, 8);
  gA.policy.burstWindowMs = argNumber("piano-dpu-refresh-window", 250, 20, 5000);
  gA.policy.dwellMs = argNumber("piano-dpu-refresh-dwell", 200, 50, 60000);
  gA.pcc = argNumber("piano-dpu-pcc", 0, 0, 2);
  gA.pccEncode = argNumber("piano-dpu-pcc-encode", 1, 0, 1);
  gA.pccNegative = argNumber("piano-dpu-pcc-negative", 0, 0, 1);
  memset(s, 0, sizeof s);
  if (PE_parse_boot_arg_str("piano-dpu-pcc-test", s, sizeof s)) {
    unsigned k = 0, v = 0;
    bool digit = false, ok = true;
    for (const char *c = s;; ++c) {
      if (*c >= '0' && *c <= '9') {
        v = v * 10 + unsigned(*c - '0');
        digit = true;
        ok &= v <= 4000;
      } else if (*c == ',' || !*c) {
        ok &= digit && k < 3;
        if (ok) gA.pccTestMilli[k++] = int32_t(v);
        v = 0;
        digit = false;
        if (!*c) break;
      } else {
        ok = false;
        break;
      }
    }
    gA.pccTest = ok && k == 3;
  }
  gA.panel = argNumber("piano-dpu-panel", 0, 0, 1);
  gA.panelEngines = argNumber("piano-dpu-panel-engines", 1, 0, 1);
  gA.panelHs = argNumber("piano-dpu-panel-hs", 0, 0, 1);
  gA.panelTest = argNumber("piano-dpu-panel-test", 0, 0, 1000);
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...)
{
  char line[480];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoDpu: %s\n", line);
}

uint64_t nowUs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000;
}
uint64_t nowMs() { return nowUs() / 1000; }

// --------------------------------------------------------------------------------------- MMIO: this file's own page maps
inline uint32_t rd32(uintptr_t a)
{
  uint32_t v;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
  return v;
}
inline void wr32(uintptr_t a, uint32_t v) { __asm__ volatile("str %w0, [%1]\n dsb sy" ::"r"(v), "r"(a) : "memory"); }

struct Page {
  uint64_t pa;
  IOMemoryMap *map;
  uintptr_t va;
};
constexpr unsigned kMaxPages = 40;
Page gPages[kMaxPages];
unsigned gPageCount, gMapFailures, gRefused, gIrqOffMisses;
uint64_t gLoads, gStores;

uintptr_t pageVa(uint64_t pa)
{
  const uint64_t base = pa & ~0xfffull;
  for (unsigned i = 0; i < gPageCount; ++i)
    if (gPages[i].pa == base) return gPages[i].va;
  if (!ml_get_interrupts_enabled()) {   // never map with interrupts off (premap() made every page the slices touch)
    ++gIrqOffMisses;
    return 0;
  }
  if (gPageCount == kMaxPages) return 0;
  IODeviceMemory *m = IODeviceMemory::withRange(base, 0x1000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (m) m->release();
  if (!map) return 0;
  gPages[gPageCount] = {base, map, uintptr_t(map->getVirtualAddress())};
  return gPages[gPageCount++].va;
}

struct KernelIo {
  uint32_t rd(uint64_t pa)
  {
    const uintptr_t va = pageVa(pa);
    if (!va) {
      ++gMapFailures;
      return 0xffffffffu;
    }
    ++gLoads;
    return rd32(va + uintptr_t(pa & 0xfff));
  }
  bool wr(uint64_t pa, uint32_t v)
  {
    if (!storeAllowed(pa, v)) {
      if (++gRefused <= 8) note("REFUSED store %#llx = %08x (not a register of the slices)", (unsigned long long)pa, v);
      return false;
    }
    const uintptr_t va = pageVa(pa);
    if (!va) {
      ++gMapFailures;
      return false;
    }
    ++gStores;
    wr32(va + uintptr_t(pa & 0xfff), v);
    return true;
  }
  void delayUs(uint32_t us) { IODelay(us); }
  void sleepMs(uint32_t ms) { IOSleep(ms); }
  uint64_t nowUs() { return ::nowUs(); }
  uintptr_t irqOff() { return uintptr_t(ml_set_interrupts_enabled(FALSE)); }
  void irqRestore(uintptr_t s) { ml_set_interrupts_enabled(boolean_t(s)); }
};
KernelIo gIo;

// Every page the slices load from or store to, mapped once before anything is written.
bool premap()
{
  const uint64_t at[] = {kGcc + kGccDispAhb, kDispCc + kCcMdssGdsc, kDispCc + kCcAhb, kDispCc + kCcNonGdscAhb, kDispCc + kCcMdp,
                         kDispCc + kCcPclk0, kDispCc + kCcMdpRcg, kMdp, intfReg(1, 0), intfReg(2, 0), pccReg(0, 0), pccReg(1, 0),
                         pccReg(2, 0), pccReg(3, 0), dsiReg(0, 0), dsiReg(1, 0)};
  for (uint64_t pa : at)
    if (!pageVa(pa)) return false;
  for (unsigned c = 0; c < kCtlCount; ++c)
    if (!pageVa(ctlReg(c, 0))) return false;
  return true;
}

// --------------------------------------------------------------------------------------------- requests and state
IOService *gService;
IOLock *gLock;
struct Shared {
  bool wake = false;
  refresh::Policy policy;
  pcc::State color;
  bool colorDirty = false;
  uint32_t matrixCalls = 0, matrixRefused = 0, gammaCalls = 0, gammaFits = 0, swapCalls = 0, panelCalls = 0;
  uint64_t lastRaw[9] = {};
  uint32_t lastFunction = 0, lastArg = 0;
  const char *lastMatrixWhy = "";
  bool matrixLog = false, lastMatrixOk = false;
  const char *lastGammaWhy = "";
  uint32_t gammaWorstMilli = 0, gammaWords = 0;
  bool gammaLog = false;
  int panelWant = -1;   // -1 nothing new, 0 off, 1 on
} gS;

// The worker's own state (only it touches these).
LivePath gPath;
bool gAsleep, gEnginesStopped, gRefreshBroken, gPanelBroken, gPccBroken;
unsigned gHz = 120;
uint32_t gSwitches, gSwitchFailures, gSleeps, gPccApplies, gPccFailures;
pcc::Matrix gPccBase = pcc::identity(), gPccLast = pcc::identity();
bool gPccLastValid;
refresh::Options gRefreshOptions;
panel::Options gPanelOptions;

void milliHz(uint64_t mhz, unsigned *whole, unsigned *frac)
{
  *whole = unsigned(mhz / 1000);
  *frac = unsigned(mhz % 1000);
}

void publish()
{
  if (!gService) return;
  gService->setProperty("piano-dpu-refresh-hz", gHz, 32);
  if (gA.panel) gService->setProperty("piano-dpu-panel", gAsleep ? "asleep" : "awake");
  if (gA.pcc && gPccLastValid) {
    char s[96];
    snprintf(s, sizeof s, "%d %d %d", pcc::milli(gPccLast.m[0][0]), pcc::milli(gPccLast.m[1][1]), pcc::milli(gPccLast.m[2][2]));
    gService->setProperty("piano-dpu-pcc", s);
  }
}

// ------------------------------------------------------------------------------------------------------------- B1c
bool reread(const char *after)
{
  const LivePath p = readPath(gIo);
  if (!p.ok) {
    note("path after %s: %s", after, p.why);
    return false;
  }
  gPath = p;
  gHz = p.link[0].vtotal == kVtotal60 ? 60 : 120;
  return true;
}

bool doSwitch(unsigned hz, const char *why)
{
  const unsigned vt = hz == 60 ? kVtotal60 : kVtotal120;
  const uint64_t t0 = nowUs();
  const refresh::Switch s = refresh::switchTo(gIo, gPath, vt, gRefreshOptions);
  const uint32_t us = uint32_t(nowUs() - t0);
  unsigned w, f;
  milliHz(s.rate.milliHz, &w, &f);
  ++gSwitches;
  if (s.ok) {
    if (gSwitches <= 50 || gSwitches % 100 == 0)
      note("refresh: %u -> %u lines (%u Hz, %s): ok in %u us: latched in %d us, %u stores, window try %u; measured %u.%03u Hz (frames "
           "%u/%u in %llu us, lockstep); periods %08x/%08x dsi totals %08x/%08x; switch %u",
           s.from, s.to, hz, why, us, s.latchUs, s.stores, s.tries - 1, w, f, s.rate.frames[0], s.rate.frames[1],
           (unsigned long long)s.rate.us, s.period[0], s.period[1], s.dsiTotal[0], s.dsiTotal[1], gSwitches);
  } else {
    ++gSwitchFailures;
    note("refresh: %u -> %u lines (%u Hz, %s): %s: %s%s; latched %d us, %u stores (%u refused), measured %u.%03u Hz (frames %u/%u), "
         "underrun %08x, periods %08x/%08x dsi totals %08x/%08x",
         s.from, s.to, hz, why, s.reverted ? "ANOMALY" : "refused", s.why,
         s.reverted ? (s.revertOk ? "; switched back, verified" : "; SWITCHED BACK BUT THAT FAILED TOO (a reset brings the boot state)") : "",
         s.latchUs, s.stores, s.refused, w, f, s.rate.frames[0], s.rate.frames[1], s.underrun, s.period[0], s.period[1], s.dsiTotal[0],
         s.dsiTotal[1]);
    if (s.reverted || s.refused) {
      gRefreshBroken = true;
      IOLockLock(gLock);
      gS.policy.disable();
      IOLockUnlock(gLock);
      note("refresh: off for this boot");
    }
  }
  reread("a switch");
  publish();
  return s.ok;
}

void refreshStart()
{
  gRefreshOptions.db = refresh::Db(gA.refreshDb);
  const char *why = refresh::refuse(gPath);
  note("refresh: %s; now %u lines (%u Hz); DSI timing shadow: %s (mode +%#x, flush +%#x); policy idle %u ms, burst %u in %u ms, dwell "
       "%u ms; %s",
       gA.refresh == kRefreshPlan ? "plan only (nothing written)" : gA.refresh == kRefreshAuto ? "auto" : gA.refresh == 60 ? "60 Hz" : "120 Hz",
       gPath.link[0].vtotal, gHz, refresh::dbName(gRefreshOptions.db), refresh::dbWords(gRefreshOptions.db).mode,
       refresh::dbWords(gRefreshOptions.db).flush, gA.policy.idleMs, gA.policy.burstSwaps, gA.policy.burstWindowMs, gA.policy.dwellMs,
       why ? why : "the path takes a VFP switch");
  for (unsigned l = 0; l < 2; ++l)
    note("refresh: link %u: intf%u config %08x hsync %08x period %08x (htotal %u vtotal %u) counters %x; dsi%u total %08x (vtotal %u) "
         "shadow words %08x %08x %08x",
         l, l + 1, gPath.link[l].config, gPath.link[l].hsyncCtl, gPath.link[l].period, gPath.link[l].htotal, gPath.link[l].vtotal,
         gPath.link[l].countEn, l, gPath.link[l].dsiTotal, gPath.link[l].dsiVtotal(), gIo.rd(dsiReg(l, kDsiShadow[0])),
         gIo.rd(dsiReg(l, kDsiShadow[1])), gIo.rd(dsiReg(l, kDsiShadow[2])));
  if (why) {
    gRefreshBroken = true;
    return;
  }
  if (gA.refresh == kRefreshPlan) {
    const refresh::Plan pl = refresh::plan(gPath, gHz == 60 ? kVtotal120 : kVtotal60, gRefreshOptions.db);
    char s[400];
    size_t n = 0;
    for (unsigned k = 0; k < pl.frameCount && n < sizeof s; ++k)
      n += size_t(snprintf(s + n, sizeof s - n, "%s%#llx=%08x", k ? " " : "", (unsigned long long)pl.frame[k].pa, pl.frame[k].v));
    for (unsigned k = 0; k < pl.afterCount && n < sizeof s; ++k)
      n += size_t(snprintf(s + n, sizeof s - n, "%s%#llx=%08x", k ? " " : "; after the latch ", (unsigned long long)pl.after[k].pa, pl.after[k].v));
    note("refresh: plan to %u Hz, inside INTF1 lines %u..%u of one frame: %s", gHz == 60 ? 120 : 60, refresh::kWindowLo,
         refresh::kWindowHi, s);
    gRefreshBroken = true;   // nothing more: plan only
    return;
  }
  if (gA.refreshTest) {
    unsigned ok = 0;
    const uint64_t t0 = nowUs();
    for (unsigned k = 0; k < gA.refreshTest && !gRefreshBroken; ++k) {
      if (doSwitch(60, "test")) ++ok;
      IOSleep(1000);
      if (!gRefreshBroken && doSwitch(120, "test")) ++ok;
      IOSleep(1000);
    }
    note("refresh: test: %u of %u switches ok in %llu s%s", ok, 2 * gA.refreshTest, (unsigned long long)((nowUs() - t0) / 1000000),
         gRefreshBroken ? " (stopped at an anomaly)" : "");
  }
  if (gRefreshBroken) return;
  if (gA.refresh == 60 && gHz != 60) doSwitch(60, "piano-dpu-refresh=60");
  else if (gA.refresh == 120 && gHz != 120) doSwitch(120, "piano-dpu-refresh=120");
  else if (gA.refresh == 60 || gA.refresh == 120) note("refresh: already %u Hz", gHz);
  IOLockLock(gLock);
  gS.policy.applied(gHz, nowMs());
  if (gA.refresh != kRefreshAuto) gS.policy.disable();
  IOLockUnlock(gLock);
  publish();
}

// ------------------------------------------------------------------------------------------------------------- B1d
void logMatrix(const char *what, const pcc::Matrix &m)
{
  note("pcc: %s [%d %d %d | %d %d %d | %d %d %d] (thousandths, [output][input])", what, pcc::milli(m.m[0][0]), pcc::milli(m.m[0][1]),
       pcc::milli(m.m[0][2]), pcc::milli(m.m[1][0]), pcc::milli(m.m[1][1]), pcc::milli(m.m[1][2]), pcc::milli(m.m[2][0]),
       pcc::milli(m.m[2][1]), pcc::milli(m.m[2][2]));
}

bool pccApply(const pcc::Matrix &m, const char *why)
{
  const pcc::Apply a = pcc::apply(gIo, gPath, m, gA.pccNegative != 0);
  ++gPccApplies;
  if (a.ok) {
    gPccLast = m;
    gPccLastValid = true;
  } else {
    ++gPccFailures;
  }
  if (!a.ok || gPccApplies <= 50 || gPccApplies % 100 == 0) {
    note("pcc: %s -> dspp mask %x: %s; latched in %d us, %u stores (%u refused), %u negative clamped, %u saturated; words R %05x %05x %05x "
         "G %05x %05x %05x B %05x %05x %05x; apply %u",
         why, a.dspps, a.ok ? "ok" : a.why, a.latchUs, a.stores, a.refused, a.words.clampedNegative, a.words.saturated, a.words.w[0][0],
         a.words.w[0][1], a.words.w[0][2], a.words.w[1][0], a.words.w[1][1], a.words.w[1][2], a.words.w[2][0], a.words.w[2][1],
         a.words.w[2][2], gPccApplies);
  }
  if (gPccFailures >= 3 && !gPccBroken) {
    gPccBroken = true;
    note("pcc: off for this boot after %u failed writes", gPccFailures);
  }
  publish();
  return a.ok;
}

// The matrix the PCC should have now: the base (what the boot stages left, normally the identity) after macOS's.
pcc::Matrix pccWanted(bool *approximate)
{
  IOLockLock(gLock);
  const pcc::Matrix user = gS.color.pcc(gA.pccEncode != 0, approximate);
  gS.colorDirty = false;
  IOLockUnlock(gLock);
  return pcc::multiply(gPccBase, user);
}

void pccStart()
{
  // What the boot stages left: PCC enable and words on each DSPP of the path.
  pcc::Matrix found[kDsppCount];
  bool enabled[kDsppCount] = {}, sameAll = true;
  int first = -1;
  for (unsigned d = 0; d < kDsppCount; ++d) {
    if (!((gPath.dspps >> d) & 1u)) continue;
    enabled[d] = gIo.rd(pccReg(d, 0)) & 1u;
    for (unsigned o = 0; o < 3; ++o)
      for (unsigned i = 0; i < 3; ++i) {
        int32_t v = int32_t(gIo.rd(pccReg(d, kPccCoef[o][i])) & 0x3ffffu);
        if (v & 0x20000) v -= 0x40000;
        found[d].m[o][i] = enabled[d] ? v * 2 : (o == i ? pcc::kOne : 0);
      }
    if (first < 0) first = int(d);
    else sameAll &= enabled[d] == enabled[first] && pcc::same(found[d], found[first]);
  }
  if (first < 0) {
    note("pcc: off: the path's LMs (%02x) have no DSPP", gPath.lms);
    gPccBroken = true;
    return;
  }
  if (!sameAll) {
    note("pcc: off: the boot stages left different PCCs on the halves (left as found)");
    gPccBroken = true;
    return;
  }
  gPccBase = found[first];
  note("pcc: piano-dpu-pcc=%u, dspp mask %x (LMs %02x), found %s%s; encode %s, negative %s; macOS's matrices go %s it",
       gA.pcc, gPath.dspps, gPath.lms, enabled[first] ? "on" : "off", enabled[first] && !pcc::isIdentity(gPccBase) ? " (not the identity: kept as the base)" : "",
       gA.pccEncode ? "diagonals to the encoded space" : "as sent", gA.pccNegative ? "two's complement" : "clamped to 0",
       pcc::isIdentity(gPccBase) ? "in place of" : "before");
  logMatrix("base", gPccBase);
  if (!pccApply(gPccBase, pcc::isIdentity(gPccBase) ? "the identity (no visible change)" : "the found matrix again (no visible change)"))
    return;
  if (gA.pccTest) {
    const pcc::Matrix t = pcc::multiply(gPccBase, pcc::diagonal(pcc::fromMilli(gA.pccTestMilli[0]), pcc::fromMilli(gA.pccTestMilli[1]),
                                                                 pcc::fromMilli(gA.pccTestMilli[2])));
    logMatrix("test (piano-dpu-pcc-test)", t);
    pccApply(t, "the test diagonal");
  }
}

void pccUpdate(bool force, const char *why)
{
  if (gPccBroken || gAsleep) return;
  bool approximate = false;
  const pcc::Matrix m = pccWanted(&approximate);
  if (!force && gPccLastValid && pcc::same(m, gPccLast)) return;
  if (approximate) note("pcc: the matrix is not a diagonal: applied to the encoded values as sent (an approximation)");
  logMatrix(why, m);
  pccApply(m, why);
}

// ------------------------------------------------------------------------------------------------------------- B1e
void logPanel(const char *what, const panel::Result &r)
{
  note("panel: %s: %s (step %u)%s; dcs %02x %s in %u us%s, %02x %s in %u us; engines intf1 %d us intf2 %d us status %x/%x%s; at %llu/%llu/"
       "%llu/%llu ms",
       what, r.ok ? "ok" : r.why, r.step, r.restored ? ", the panel brought back" : "", r.dcs[0].command, r.dcs[0].ok ? "sent" : "not sent",
       r.dcs[0].doneUs, r.dcs[0].waitedBlanking ? " (front porch)" : "", r.dcs[1].command, r.dcs[1].ok ? "sent" : "not sent", r.dcs[1].doneUs,
       r.engines.us[0], r.engines.us[1], r.engines.status[0], r.engines.status[1], r.engines.rate.milliHz ? ", counting in lockstep" : "",
       (unsigned long long)(r.tUs[0] / 1000), (unsigned long long)(r.tUs[1] / 1000), (unsigned long long)(r.tUs[2] / 1000),
       (unsigned long long)(r.tUs[3] / 1000));
}

bool panelSleep()
{
  if (gAsleep || gPanelBroken) return false;
  const panel::Result r = panel::panelOff(gIo, gPath, gPanelOptions);
  logPanel("sleep (display off 0x28, 20 ms, sleep in 0x10, 100 ms, INTF2 then INTF1 stopped)", r);
  if (r.ok) {
    gAsleep = true;
    gEnginesStopped = r.enginesStopped;
    ++gSleeps;
  } else {
    gPanelBroken = true;
    note("panel: sleep off for this boot");
  }
  publish();
  return r.ok;
}

bool panelWake()
{
  if (!gAsleep) return false;
  const panel::Result r = panel::panelOn(gIo, gPath, gPanelOptions, gEnginesStopped);
  logPanel("wake (INTF flush, INTF1 + INTF2 on, sleep out 0x11, 120 ms, display on 0x29)", r);
  gAsleep = false;
  gEnginesStopped = false;
  if (!r.ok) {
    gPanelBroken = true;
    note("panel: WAKE FAILED: the panel may stay dark until a reset (piano-reset-after); sleep off for this boot");
  }
  reread("the wake");
  if (r.ok && gA.pcc) pccUpdate(true, "re-applied at the wake");
  publish();
  return r.ok;
}

void panelStart()
{
  gPanelOptions.lowPower = !gA.panelHs;
  gPanelOptions.engines = gA.panelEngines != 0;
  for (unsigned l = 0; l < 2; ++l) {
    const uint32_t trig = gIo.rd(dsiReg(l, kDsiTrigCtrl));
    note("panel: dsi%u command path: ctrl %08x status %08x fifo %08x cmd-dma %08x dma1 %08x length %08x trig %08x (dma trigger %u%s, %s) "
         "intr %08x tpg %08x ack %08x",
         l, gIo.rd(dsiReg(l, kDsiCtrl)), gIo.rd(dsiReg(l, kDsiStatus0)), gIo.rd(dsiReg(l, kDsiFifoStatus)), gIo.rd(dsiReg(l, kDsiCmdDmaCtrl)),
         gIo.rd(dsiReg(l, kDsiDmaCtrl1)), gIo.rd(dsiReg(l, kDsiDmaLength)), trig, trig & 7u, (trig & 7u) == 4 ? " software" : " NOT software",
         (trig >> 12) & 1u ? "DMA blocked within the frame" : "no block: triggered in the front porch", gIo.rd(dsiReg(l, kDsiIntrCtrl)),
         gIo.rd(dsiReg(l, kDsiTpgCtrl)), gIo.rd(dsiReg(l, kDsiAckErr)));
  }
  note("panel: piano-dpu-panel=1: sleep with the display (%s DCS, %s); nothing of the panel's reset or supplies is touched",
       gPanelOptions.lowPower ? "low-power" : "high-speed", gPanelOptions.engines ? "then the timing engines stopped" : "the engines keep running");
  for (unsigned k = 0; k < gA.panelTest && !gPanelBroken; ++k) {
    if (!panelSleep()) break;
    IOSleep(5000);
    if (!panelWake()) break;
    IOSleep(5000);
    note("panel: test cycle %u of %u done", k + 1, gA.panelTest);
  }
}

// ------------------------------------------------------------------------------------------------------------- the worker
void serve()
{
  uint64_t nextAlive = nowMs() + 600000;
  for (;;) {
    IOLockLock(gLock);
    uint64_t due = nextAlive;
    const refresh::Policy::Decision d0 = gS.policy.decide(nowMs());
    if (gA.refresh == kRefreshAuto && !gRefreshBroken && !gAsleep && d0.nextMs && d0.nextMs < due) due = d0.nextMs;
    if (!gS.wake && !(gA.refresh == kRefreshAuto && !gRefreshBroken && !gAsleep && d0.hz != gHz)) {
      const uint64_t now = nowMs();
      if (due > now) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(uint32_t(due - now), kMillisecondScale, &deadline);
        IOLockSleepDeadline(gLock, &gS, deadline, THREAD_UNINT);
      }
    }
    gS.wake = false;
    const int want = gS.panelWant;
    gS.panelWant = -1;
    const bool colorDirty = gS.colorDirty, matrixLog = gS.matrixLog;
    uint64_t raw[9];
    memcpy(raw, gS.lastRaw, sizeof raw);
    const uint32_t function = gS.lastFunction, arg = gS.lastArg, calls = gS.matrixCalls;
    const bool matrixOk = gS.lastMatrixOk;
    const char *matrixWhy = gS.lastMatrixWhy;
    gS.matrixLog = false;
    const bool gammaLog = gS.gammaLog;
    const uint32_t gammaCalls = gS.gammaCalls, gammaWords = gS.gammaWords, gammaWorst = gS.gammaWorstMilli;
    const char *gammaWhy = gS.lastGammaWhy;
    gS.gammaLog = false;
    const refresh::Policy::Decision d = gS.policy.decide(nowMs());
    IOLockUnlock(gLock);

    if (matrixLog && (calls <= 30 || !matrixOk))
      note("pcc: matrix call %u: function %u arg %#x: %016llx %016llx %016llx | %016llx %016llx %016llx | %016llx %016llx %016llx: %s",
           calls, function, arg, (unsigned long long)raw[0], (unsigned long long)raw[1], (unsigned long long)raw[2],
           (unsigned long long)raw[3], (unsigned long long)raw[4], (unsigned long long)raw[5], (unsigned long long)raw[6],
           (unsigned long long)raw[7], (unsigned long long)raw[8], matrixOk ? "taken" : matrixWhy);
    if (gammaLog && gammaCalls <= 30)
      note("pcc: gamma call %u: %u words: %s (worst deviation from scaled ramps %u/1000)", gammaCalls, gammaWords, gammaWhy, gammaWorst);
    if (want == 0 && gA.panel) panelSleep();
    if (want == 1 && gA.panel) panelWake();
    if (colorDirty && gA.pcc) pccUpdate(false, "macOS's matrices");
    if (gA.refresh == kRefreshAuto && !gRefreshBroken && !gAsleep && d.hz != gHz) {
      const bool ok = doSwitch(d.hz, d.hz == 60 ? "idle" : "swaps");
      IOLockLock(gLock);
      if (ok) gS.policy.applied(gHz, nowMs());
      IOLockUnlock(gLock);
    }
    if (nowMs() >= nextAlive) {
      nextAlive = nowMs() + 600000;
      IOLockLock(gLock);
      const uint64_t swaps = gS.policy.swaps();
      const uint32_t mc = gS.matrixCalls, gc = gS.gammaCalls, pc = gS.panelCalls;
      IOLockUnlock(gLock);
      note("slices: alive: refresh %u Hz%s (%u switches, %u failed, %llu swaps); pcc %u applies (%u failed, %u matrix calls, %u gamma "
           "calls); panel %s (%u sleeps, %u calls); %llu loads %llu stores %u refused %u map failures",
           gHz, gRefreshBroken ? " (off)" : "", gSwitches, gSwitchFailures, (unsigned long long)swaps, gPccApplies, gPccFailures, mc, gc,
           gAsleep ? "asleep" : "awake", gSleeps, pc, (unsigned long long)gLoads, (unsigned long long)gStores, gRefused, gMapFailures);
    }
  }
}

void worker(void *, wait_result_t)
{
  thread_set_thread_name(current_thread(), "piano-dpu-slices");
  piano_dpu_slices_stage = 1;
  // The survey first (<= 120 s): its stages 1..5; B1b (piano-dpu=2) goes on with 6..8 (negative when a flip step failed: not ours).
  for (unsigned i = 0; i < 1200 && piano_dpu_stage >= 0 && piano_dpu_stage < 5; ++i) IOSleep(100);
  if (piano_dpu_stage < 0 && piano_dpu_stage > -6) {
    piano_dpu_slices_stage = -1;
    note("slices: off: the survey failed at stage %d", -piano_dpu_stage);
    return;
  }
  piano_dpu_slices_stage = 2;
  if (!premap()) {
    piano_dpu_slices_stage = -2;
    note("slices: off: a register page did not map (%u mapped)", gPageCount);
    return;
  }
  const LivePath p = readPath(gIo);
  note("slices: path: %s; ctl%d (top %08x intf %x master %x) lms %02x dspps %x; intf1 en %x status %x frame %u, intf2 en %x status %x "
       "frame %u",
       p.why, p.ctl, p.ctlTop, p.intfActive, p.intfMaster, p.lms, p.dspps, p.link[0].en, p.link[0].status, p.link[0].frame,
       p.link[1].en, p.link[1].status, p.link[1].frame);
  if (!p.ok) {
    piano_dpu_slices_stage = -2;
    note("slices: off: %s (nothing written)", p.why);
    return;
  }
  gPath = p;
  gHz = p.link[0].vtotal == kVtotal60 ? 60 : 120;
  if (((gA.refresh && gA.refresh != kRefreshPlan) || gA.panel) && !countersOn(gIo, gPath)) {   // plan mode writes nothing
    note("slices: the INTF frame/line counters stay off (count-en %x/%x): refresh and panel off", gPath.link[0].countEn, gPath.link[1].countEn);
    gRefreshBroken = gPanelBroken = true;
  }
  if (gA.pcc) {
    piano_dpu_slices_stage = 3;
    pccStart();
  }
  if (gA.refresh) {
    piano_dpu_slices_stage = 4;
    refreshStart();
  } else {
    gRefreshBroken = true;
  }
  if (gA.panel) {
    piano_dpu_slices_stage = 5;
    panelStart();
  }
  piano_dpu_slices_stage = 6;
  note("slices: serving (refresh %s, pcc %s, panel %s)", gA.refresh == kRefreshAuto && !gRefreshBroken ? "auto" : gRefreshBroken ? "off" : "fixed",
       gA.pcc && !gPccBroken ? "on" : "off", gA.panel && !gPanelBroken ? "on" : "off");
  publish();
  serve();
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------- the interface
bool dpuSlicesWanted()
{
  readSliceArgs();
  return gA.refresh || gA.pcc || gA.panel;
}

void dpuSlicesStart(IOService *dpu)
{
  if (!dpuSlicesWanted() || gLock) return;
  note("slices: piano-dpu-refresh=%s pcc=%u panel=%u (B1c/B1d/B1e: these write; the survey above stays read only)%s",
       gA.refresh == kRefreshAuto ? "auto" : gA.refresh == kRefreshPlan ? "plan" : gA.refresh == 60 ? "60" : gA.refresh == 120 ? "120" : "0", gA.pcc,
       gA.panel, gA.level ? "" : ": piano-dpu=0, not started");
  if (!gA.level) return;
  gLock = IOLockAlloc();
  thread_t t = nullptr;
  if (!gLock || kernel_thread_start(worker, nullptr, &t) != KERN_SUCCESS) {
    piano_dpu_slices_stage = -1;
    note("slices: FAILED: no lock or no thread; nothing written");
    return;
  }
  thread_deallocate(t);
  gService = dpu;
  dpu->registerService();   // NativeGopDisplay finds the service by its class
}

bool dpuSlicesPlatformFunction(const OSSymbol *name, void *p1, void *p2, void *p3, IOReturn *result)
{
  if (!name || !gLock) return false;
  if (name->isEqualTo("PianoDpuSwap")) {
    IOLockLock(gLock);
    ++gS.swapCalls;
    const bool wake = gS.policy.swap(nowMs());
    if (wake) {
      gS.wake = true;
      IOLockWakeup(gLock, &gS, false);
    }
    IOLockUnlock(gLock);
    *result = kIOReturnSuccess;
    return true;
  }
  if (name->isEqualTo("PianoDpuColorMatrix")) {
    if (!gA.pcc || !p1) {
      *result = kIOReturnUnsupported;
      return true;
    }
    uint64_t raw[9];
    memcpy(raw, p1, sizeof raw);
    const pcc::Decoded d = pcc::decodeS3132(raw);
    const uint32_t function = uint32_t(uintptr_t(p2));
    IOLockLock(gLock);
    ++gS.matrixCalls;
    memcpy(gS.lastRaw, raw, sizeof raw);
    gS.lastFunction = function;
    gS.lastArg = uint32_t(uintptr_t(p3));
    gS.lastMatrixWhy = d.ok && function >= pcc::State::kFunctions ? "the function is above 15" : d.why;
    gS.lastMatrixOk = d.ok && gS.color.setMatrix(function, d.m);
    if (gS.lastMatrixOk) gS.colorDirty = true;
    else ++gS.matrixRefused;
    gS.matrixLog = gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    IOLockUnlock(gLock);
    *result = d.ok ? kIOReturnSuccess : kIOReturnBadArgument;
    return true;
  }
  if (name->isEqualTo("PianoDpuGammaTable")) {
    const uint32_t words = uint32_t(uintptr_t(p2));
    pcc::GammaFit f;
    if (gA.pcc >= 2 && p1 && words >= pcc::kGammaWords) f = pcc::fitGamma(static_cast<const uint32_t *>(p1), words);
    IOLockLock(gLock);
    ++gS.gammaCalls;
    gS.lastGammaWhy = gA.pcc < 2 ? "piano-dpu-pcc=1: logged only" : f.why;
    gS.gammaWorstMilli = f.worstMilli;
    gS.gammaWords = words;
    gS.gammaLog = gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    if (f.ok) {
      ++gS.gammaFits;
      gS.color.setGamma(f.m);
      gS.colorDirty = gS.wake = true;
      IOLockWakeup(gLock, &gS, false);
    }
    IOLockUnlock(gLock);
    *result = f.ok ? kIOReturnSuccess : kIOReturnUnsupported;
    return true;
  }
  if (name->isEqualTo("PianoDpuPanel")) {
    IOLockLock(gLock);
    ++gS.panelCalls;
    gS.panelWant = p1 ? 1 : 0;
    gS.wake = true;
    IOLockWakeup(gLock, &gS, false);
    IOLockUnlock(gLock);
    *result = gA.panel ? kIOReturnSuccess : kIOReturnUnsupported;
    return true;
  }
  return false;
}
