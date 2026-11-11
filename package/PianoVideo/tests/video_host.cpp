// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoVideo's level 1 survey (VideoSurvey.h) against register models:
//   - the gate: with GCC's video AHB off nothing of VIDEO_CC or the core is loaded; with piano-video-cc=0 only GCC's four words;
//     with the controller GDSC collapsed (the expected cold state) nothing of the core; with the controller on but a clock off
//     nothing of the core; with the controller on and the codec off, the controller's words but no codec word;
//   - every load lies in GCC's four words, VIDEO_CC, the core's listed registers, the SMMU window or (opt-in) the carveout's first
//     words; the write-to-clear and doorbell registers (A2HSOFTINTCLR, IC_SOFTINT) are never loaded; no word is loaded twice;
//   - the decoders: PLL0 at Linux's configuration (720 MHz), the RCG source/divider, the mvs0 divider and the OPP match (240 MHz),
//     the GDSC states, CTRL_STATUS and the core-state verdicts;
//   - the SMMU scan: groups covering 0x1940..0x194f under a mask, the free-group and banks-in-use counts, the bank registers only
//     at level 2, the stream words and the route verdicts.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. video_host.cpp -o /tmp/video_host && /tmp/video_host
#include "VideoSurvey.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace pianovideo;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---------------------------------------------------------------------------------------------------------- the MMIO model
struct Soc {
  std::map<uint64_t, uint32_t> regs;
  std::vector<uint64_t> loads;
  uint32_t load(uint64_t pa)
  {
    loads.push_back(pa);
    auto it = regs.find(pa);
    return it == regs.end() ? 0 : it->second;
  }
  unsigned in(uint64_t base, uint64_t bytes) const
  {
    unsigned n = 0;
    for (uint64_t pa : loads) n += pa >= base && pa < base + bytes;
    return n;
  }
  bool loaded(uint64_t pa) const
  {
    for (uint64_t a : loads)
      if (a == pa) return true;
    return false;
  }
  bool noRepeats() const { return std::set<uint64_t>(loads.begin(), loads.end()).size() == loads.size(); }
  void set(uint64_t pa, uint32_t v) { regs[pa] = v; }
  void gccOn() { for (uint32_t o : {kGccVideoAhb, kGccVideoXo, kGccVideoAxi0, kGccVideoAxi1}) set(kGcc + o, 1); }
  void gccAhbOnly() { set(kGcc + kGccVideoAhb, 1); for (uint32_t o : {kGccVideoXo, kGccVideoAxi0, kGccVideoAxi1}) set(kGcc + o, 0x80000000); }
  // The controller domain powered and clocked (Linux's iris_vpu35_vpu4x_power_on_controller), PLL0 at Linux's configuration and
  // mvs0 at the lowest OPP (720 MHz / 3); the codec domain (MVS0) collapsed and unclocked until codecOn().
  void controllerOn()
  {
    gccOn();
    set(kVideoCc + kCcPll0, 0x80000007);
    set(kVideoCc + kCcPll0 + kPllOpmode, 1);
    set(kVideoCc + kCcPll0 + kPllL, kPllConfigL);
    set(kVideoCc + kCcPll0 + kPllAlpha, kPllConfigAlpha);
    set(kVideoCc + kCcMvs0Rcg + 4, 1u << 8 | 0);   // PLL0, divider 1
    set(kVideoCc + kCcMvs0Div, 2);                   // divide by 3
    set(kVideoCc + kCcMvs0cGdsc, 0x80000000);
    set(kVideoCc + kCcMvs0cClk, 1);
    set(kVideoCc + kCcMvs0cFreerun, 1);
    set(kVideoCc + kCcMvs0Gdsc, 0x00000002);         // off, hardware-controlled
    set(kVideoCc + kCcMvs0Clk, 0x80000000);
    set(kVideoCc + kCcMvs0Freerun, 0x80000000);
  }
  void codecOn()
  {
    set(kVideoCc + kCcMvs0Gdsc, 0x80000002);
    set(kVideoCc + kCcMvs0Clk, 1);
    set(kVideoCc + kCcMvs0Freerun, 1);
  }
};

// Every address the survey may load: GCC's four words, VIDEO_CC, the core's listed registers and the codec's idle words.
static bool allowed(uint64_t pa, bool carveout)
{
  for (uint32_t o : {kGccVideoAhb, kGccVideoXo, kGccVideoAxi0, kGccVideoAxi1})
    if (pa == kGcc + o) return true;
  if (pa >= kVideoCc && pa < kVideoCc + kVideoCcBytes) return true;
  if (pa >= kSmmu && pa < kSmmu + kSmmuBytes) return true;
  if (carveout && pa >= kCarveout && pa < kCarveout + 32) return true;
  if (pa >= kIris && pa < kIris + kIrisBytes) {
    const uint32_t o = uint32_t(pa - kIris);
    if (o == kCpuCsA2hSoftIntClr || o == kCpuIcSoftInt) return false;
    for (uint32_t r : {kWrapperHwVersion, kCtrlInit, kCtrlStatus, kQtblInfo, kQtblAddr, kSciaCmdArg3, kSfrAddr, kUcRegionAddr,
                       kUcRegionSize, kH2xSoftIntEn, kAhbBridgeSyncReset, kX2Rpmh, kWrapperIntrStatus, kWrapperIntrMask,
                       kDebugBridgeLpiCtl, kDebugBridgeLpiStatus, kCpuNocLpiCtl, kCpuNocLpiStatus, kWrapperSpare0, kCorePowerStatus,
                       kCoreClockConfig, kTzCpuStatus, kTzAxiClockConfig, kTzQns4pdxFifoReset, kAonNocLpiCtl, kAonNocLpiStatus,
                       kAonNocCoreSwReset, kAonNocCoreClkCtl, kAonSpare, kAonVideoCtlNocLpiCtl, kAonVideoCtlNocLpiStatus})
      if (o == r) return true;
    for (unsigned i = 0; i < kVppPipes; ++i)
      if (o == kVcodecIdleStatus + 4 * i) return true;
  }
  return false;
}
static bool allAllowed(const Soc &soc, bool carveout = false, bool quiet = false)
{
  for (uint64_t pa : soc.loads)
    if (!allowed(pa, carveout)) {
      if (!quiet) std::printf("  load outside the allowlist: %#llx\n", (unsigned long long)pa);
      return false;
    }
  return true;
}

struct Run {
  Clocks c;
  Core r;
  unsigned loads = 0;
};
static Run survey(Soc &soc, unsigned cc = 1)
{
  Run x;
  auto ld = [&](uint64_t pa) { return soc.load(pa); };
  x.c = surveyClocks(ld, cc, &x.loads);
  x.r = surveyCore(x.c, ld, &x.loads);
  return x;
}

// ------------------------------------------------------------------------------------------------------------------ gates
static void testGccOff()
{
  Soc soc;
  soc.set(kGcc + kGccVideoAhb, 0x80000000);
  const Run x = survey(soc);
  CHECK(!x.c.ccRead && !x.c.controller && !x.r.controllerRead);
  CHECK(soc.loads.size() == 4 && x.loads == 4);
  CHECK(soc.in(kVideoCc, kVideoCcBytes) == 0 && soc.in(kIris, kIrisBytes) == 0);
  CHECK(coreState(x.c, x.r) == CoreState::cold);
  CHECK(std::strstr(x.c.why, "GCC video AHB clock off"));
}

static void testCcSkipped()
{
  Soc soc;
  soc.controllerOn();
  const Run x = survey(soc, 0);
  CHECK(soc.loads.size() == 4 && !x.c.ccRead && !x.r.controllerRead);
  CHECK(coreState(x.c, x.r) == CoreState::unknown);
}

// The expected state at XNU time: GCC's video AHB on (or off), the controller GDSC collapsed.
static void testCold()
{
  Soc soc;
  soc.gccAhbOnly();
  soc.set(kVideoCc + kCcMvs0cGdsc, 0x00000000);
  soc.set(kVideoCc + kCcMvs0Gdsc, 0x00000002);
  const Run x = survey(soc);
  CHECK(x.c.ccRead && !x.c.controller && !x.c.codec);
  CHECK(soc.in(kIris, kIrisBytes) == 0);
  CHECK(soc.in(kVideoCc, kVideoCcBytes) == 25);
  CHECK(coreState(x.c, x.r) == CoreState::cold);
  CHECK(allAllowed(soc) && soc.noRepeats());
  CHECK(std::strcmp(gdscState(x.c.mvs0cGdsc), "off") == 0);
  CHECK(std::strcmp(gdscState(x.c.mvs0Gdsc), "off, hardware-controlled") == 0);
  CHECK(std::strcmp(gdscState(1), "off (software collapse)") == 0);
}

static void testControllerUnclocked()
{
  for (uint32_t off : {0u, 1u, 2u}) {   // AXI1, MVS0C, MVS0C free-running off in turn
    Soc soc;
    soc.controllerOn();
    if (off == 0) soc.set(kGcc + kGccVideoAxi1, 0x80000000);
    if (off == 1) soc.set(kVideoCc + kCcMvs0cClk, 0x80000000);
    if (off == 2) soc.set(kVideoCc + kCcMvs0cFreerun, 0x80000000);
    const Run x = survey(soc);
    CHECK(!x.c.controller && soc.in(kIris, kIrisBytes) == 0);
    CHECK(coreState(x.c, x.r) == CoreState::unclocked);
  }
}

static void testControllerOnly()
{
  Soc soc;
  soc.controllerOn();
  const Run x = survey(soc);
  CHECK(x.c.controller && !x.c.codec && x.r.controllerRead && !x.r.codecRead);
  CHECK(soc.in(kIris, kIrisBytes) == 31);
  for (unsigned i = 0; i < kVppPipes; ++i) CHECK(!soc.loaded(kIris + kVcodecIdleStatus + 4 * i));
  CHECK(!soc.loaded(kIris + kCpuCsA2hSoftIntClr) && !soc.loaded(kIris + kCpuIcSoftInt));
  CHECK(allAllowed(soc) && soc.noRepeats());
  CHECK(coreState(x.c, x.r) == CoreState::noFirmware);
}

static void testCodecOn()
{
  Soc soc;
  soc.controllerOn();
  soc.codecOn();
  soc.set(kIris + kVcodecIdleStatus + 8, 0x400000);
  const Run x = survey(soc);
  CHECK(x.c.codec && x.r.codecRead && x.r.idle[2] == 0x400000);
  CHECK(soc.in(kIris, kIrisBytes) == 31 + kVppPipes);
  CHECK(allAllowed(soc) && soc.noRepeats());
  // The codec gate needs AXI0 as well.
  Soc noAxi0;
  noAxi0.controllerOn();
  noAxi0.codecOn();
  noAxi0.set(kGcc + kGccVideoAxi0, 0x80000000);
  const Run y = survey(noAxi0);
  CHECK(y.c.controller && !y.c.codec && !y.r.codecRead);
}

// ---------------------------------------------------------------------------------------------------------------- decoders
static void testClockDecoders()
{
  CHECK(pllHz(kPllConfigL, kPllConfigAlpha) == 720000000ull);
  CHECK(pllHz(0, 0) == 0);
  CHECK(pllHz(0x34, 0x4000) == 19200000ull * 52 + 4800000ull);
  Soc soc;
  soc.controllerOn();
  const Run x = survey(soc);
  CHECK(pllLocked(x.c.pllMode) && pllRunning(x.c));
  CHECK(mvs0SourceHz(x.c) == 720000000ull);
  CHECK(mvs0Hz(x.c) == 240000000ull);
  CHECK(oppIndex(mvs0Hz(x.c)) == 0);
  CHECK(oppIndex(533333334) == 4 && oppIndex(630000000) == 6 && oppIndex(19200000) == -1);
  const Rcg r = rcg(0x80000000, 1u << 8 | 3);   // root off, PLL0, divider 2
  CHECK(r.rootOff && !r.rootEn && r.source == 1 && r.halfDivider == 4);
  // An RCG on XO with divider 1, no mvs0 divider: 19.2 MHz.
  Soc xo;
  xo.controllerOn();
  xo.set(kVideoCc + kCcMvs0Rcg + 4, 0);
  xo.set(kVideoCc + kCcMvs0Div, 0);
  CHECK(mvs0Hz(survey(xo).c) == kXoHz);
  // An unknown source yields no rate.
  Soc other;
  other.controllerOn();
  other.set(kVideoCc + kCcMvs0Rcg + 4, 5u << 8);
  CHECK(mvs0Hz(survey(other).c) == 0);
}

static void testCoreStates()
{
  Clocks on;
  on.ccRead = true;
  on.mvs0cGdsc = 0x80000000;
  on.controller = true;
  Core r;
  r.controllerRead = true;
  CHECK(coreState(on, r) == CoreState::noFirmware);
  r.ctrlStatus = 1;
  CHECK(coreState(on, r) == CoreState::firmwareBooted);
  r.ctrlStatus = 0x4 << 1;
  CHECK(coreState(on, r) == CoreState::firmwareError && ctrlError(r.ctrlStatus) == 4);
  r.ctrlStatus = 0;
  r.qtblAddr = 0xdff00000;
  CHECK(coreState(on, r) == CoreState::otherState);
  for (CoreState s : {CoreState::unknown, CoreState::cold, CoreState::unclocked, CoreState::noFirmware, CoreState::firmwareBooted,
                      CoreState::firmwareError, CoreState::otherState})
    CHECK(std::strlen(coreStateName(s)) > 2 && std::strlen(coreVerdict(s)) > 10);
}

// ------------------------------------------------------------------------------------------------------------------- SMMU
struct SmmuModel {
  std::map<uint32_t, uint32_t> regs;
  std::vector<uint32_t> loads;
  uint32_t rd(uint32_t off)
  {
    loads.push_back(off);
    auto it = regs.find(off);
    return it == regs.end() ? 0 : it->second;
  }
};
static SmmuModel smmuModel()
{
  SmmuModel m;
  m.regs[0x20] = 0x00004000u | 100;      // CTTW, 100 groups
  m.regs[0x24] = 0x60000053u;            // 83 banks, 128 pages (as piano's apps SMMU reads in the other surveys)
  // Valid groups: 0 -> 0x800 translate bank 2; 1 -> 0x60 bypass; 2 -> 0x1940 translate bank 5; 3 -> 0x1400 mask 0x1 bank 2.
  m.regs[0x800] = 0x80000800u | 2u << 16;
  m.regs[0xc00] = 2;
  m.regs[0x804] = 0x80000060u;
  m.regs[0xc04] = 1u << 16;
  m.regs[0x808] = 0x80001940u;
  m.regs[0xc08] = 5;
  m.regs[0x80c] = 0x80001400u | 1u << 16;
  m.regs[0xc0c] = 2;
  m.regs[0x1000 + 4 * 5] = 1u << 16;     // CBAR: stage 1, stage 2 bypass
  m.regs[0x1800 + 4 * 5] = 1;
  const uint32_t cb = (128 + 5) * 4096;
  m.regs[cb + 0x00] = 0x1;               // translation on
  m.regs[cb + 0x20] = 0x12345000;
  return m;
}

static void testSmmu()
{
  SmmuModel m = smmuModel();
  const SmmuSurvey s = surveySmmu(2, [&](uint32_t off) { return m.rd(off); });
  CHECK(!s.error && s.groups == 100 && s.banks == 0x53 && s.pages == 128);
  CHECK(coherentWalks(s));
  CHECK(s.freeGroups == 96);
  CHECK(s.banksReferenced == 2);          // banks 2 and 5 (two groups share bank 2; the bypass one counts none)
  CHECK(s.count == 1 && s.found[0].group == 2 && s.found[0].bank.read && s.found[0].bank.sctlr == 1);
  CHECK(std::strcmp(streamWord(s, kStreamFirst), "bank-on") == 0);
  CHECK(std::strcmp(streamWord(s, kStreamSecond), "none") == 0);
  CHECK(std::strstr(routeVerdict(s), "already has an entry"));
  for (uint32_t off : m.loads) CHECK(off < kSmmuBytes);
  // Level 1: the bank's page is not loaded.
  SmmuModel g = smmuModel();
  const SmmuSurvey l1 = surveySmmu(1, [&](uint32_t off) { return g.rd(off); });
  CHECK(l1.count == 1 && !l1.found[0].bank.read && l1.found[0].bank.cbar == 1u << 16);
  for (uint32_t off : g.loads) CHECK(off < 128 * 4096);
  CHECK(std::strcmp(streamWord(l1, kStreamFirst), "bank") == 0);
  // No entry: the expected state; a mask covering the whole range; a bypassing stream.
  SmmuModel none = smmuModel();
  none.regs.erase(0x808);
  const SmmuSurvey n = surveySmmu(2, [&](uint32_t off) { return none.rd(off); });
  CHECK(n.count == 0 && n.freeGroups == 97 && std::strstr(routeVerdict(n), "no stream-match entry"));
  SmmuModel masked = smmuModel();
  masked.regs[0x808] = 0x80001940u | 0xfu << 16;
  masked.regs[0xc08] = 1u << 16;          // bypass
  const SmmuSurvey b = surveySmmu(2, [&](uint32_t off) { return masked.rd(off); });
  CHECK(b.count == 1 && std::strcmp(streamWord(b, kStreamSecond), "bypass") == 0 && std::strstr(routeVerdict(b), "bypasses"));
  // A geometry outside the window is refused before any group is read.
  SmmuModel bad = smmuModel();
  bad.regs[0x24] = 0xe0000053u;
  const SmmuSurvey e = surveySmmu(2, [&](uint32_t off) { return bad.rd(off); });
  CHECK(e.error && bad.loads.size() == 3 && std::strcmp(streamWord(e, kStreamFirst), "unread") == 0);
}

// ---------------------------------------------------------------------------------------------------------------- carveout
static void testCarveout()
{
  Soc soc;
  unsigned loads = 0;
  Carveout c;
  CHECK(std::strstr(carveoutVerdict(c), "not read"));
  c = surveyCarveout([&](uint64_t pa) { return soc.load(pa); }, &loads);
  CHECK(loads == 8 && soc.loads.size() == 8 && allAllowed(soc, true) && !allAllowed(soc, false, true));
  CHECK(std::strstr(carveoutVerdict(c), "zeros"));
  soc.set(kCarveout, 0x464c457f);
  CHECK(std::strstr(carveoutVerdict(surveyCarveout([&](uint64_t pa) { return soc.load(pa); }, &loads)), "ELF"));
}

int main()
{
  testGccOff();
  testCcSkipped();
  testCold();
  testControllerUnclocked();
  testControllerOnly();
  testCodecOn();
  testClockDecoders();
  testCoreStates();
  testSmmu();
  testCarveout();
  if (failures) {
    std::printf("PianoVideo host tests: %d failed\n", failures);
    return 1;
  }
  std::printf("PianoVideo host tests: all passed\n");
  return 0;
}
