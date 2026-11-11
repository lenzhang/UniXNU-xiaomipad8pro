// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo level 1: what the survey reads, in which order, behind which gate, and what the readings say. Portable (host tests in
// tests/video_host.cpp): every hardware access goes through Load, a function of a physical address that returns the 32-bit word
// there. There is no store anywhere in this file, so the survey cannot write to the hardware.
//
// Order: (1) GCC's four video branches (always readable: GCC). (2) VIDEO_CC, only with GCC's video AHB branch on (the clock
// controller's own registers sit behind it): PLL0, the two RCGs and dividers, both GDSCs, every branch. (3) The core's controller
// registers (CPU_CS, WRAPPER, WRAPPER_TZ, AON) only with the controller GDSC (MVS0C) powered and its clocks on (GCC AXI1, MVS0C,
// MVS0C free-running): a load from an unclocked or collapsed block can stall the bus (UniXNU's SM8750 ports hung on exactly that,
// for MDSS). The codec's idle words only with the codec GDSC (MVS0) and its clocks (GCC AXI0, MVS0, MVS0 free-running) on as
// well. Two registers are never loaded: CPU_CS_A2HSOFTINTCLR (write-to-clear) and CPU_IC_SOFTINT (rings the firmware). (4) The
// apps SMMU's entries for streams 0x1940..0x194f and their banks, plus the free stream-match groups and the banks in use (what a
// translating bank for the video core will take). (5) Optionally (carveout level 1), the first words of the firmware carveout:
// memory XNU does not own, loaded through a device mapping.
#pragma once
#include <stdint.h>
#include "VideoHw.h"

namespace pianovideo {

inline bool branchOn(uint32_t cbcr) { return !(cbcr >> 31); }       // CBCR bit 31: CLK_OFF
inline bool gdscOn(uint32_t gdscr) { return (gdscr >> 31) & 1u; }   // GDSCR bit 31: PWR_ON

// ------------------------------------------------------------------------------------------------------------- the clocks
struct Clocks {
  uint32_t gccAhb = 0, gccXo = 0, gccAxi0 = 0, gccAxi1 = 0;
  bool ccRead = false;   // VIDEO_CC was loaded
  uint32_t pllMode = 0, pllOpmode = 0, pllState = 0, pllL = 0, pllAlpha = 0;
  uint32_t mvs0RcgCmd = 0, mvs0RcgCfg = 0, ahbRcgCmd = 0, ahbRcgCfg = 0;
  uint32_t mvs0cGdsc = 0, mvs0cGdscCfg = 0, mvs0Gdsc = 0, mvs0GdscCfg = 0;
  uint32_t mvs0c = 0, mvs0cFreerun = 0, mvs0cDiv2 = 0, mvs0 = 0, mvs0Freerun = 0, mvs0FreerunMem = 0, mvs0Div = 0;
  uint32_t ahb = 0, xo = 0, sleep = 0, mvs0Shift = 0, mvs0cShift = 0;
  bool controller = false;   // the controller's registers may be loaded
  bool codec = false;        // the codec's registers may be loaded
  const char *why = "";
};

// ccLevel 0: VIDEO_CC is not read at all (piano-video-cc=0: the fallback if a load there ever stalls).
template <class Load>
Clocks surveyClocks(Load load, unsigned ccLevel, unsigned *loads)
{
  Clocks c;
  c.gccAhb = load(kGcc + kGccVideoAhb);
  c.gccXo = load(kGcc + kGccVideoXo);
  c.gccAxi0 = load(kGcc + kGccVideoAxi0);
  c.gccAxi1 = load(kGcc + kGccVideoAxi1);
  *loads += 4;
  if (!ccLevel) {
    c.why = "VIDEO_CC not read (piano-video-cc=0): the core's state is unknown";
    return c;
  }
  if (!branchOn(c.gccAhb)) {
    c.why = "GCC video AHB clock off: VIDEO_CC and the core not read (cold, as nothing before XNU uses the video core)";
    return c;
  }
  struct {
    uint32_t off;
    uint32_t *to;
  } const words[] = {
      {kCcPll0, &c.pllMode},           {kCcPll0 + kPllOpmode, &c.pllOpmode}, {kCcPll0 + kPllState, &c.pllState},
      {kCcPll0 + kPllL, &c.pllL},      {kCcPll0 + kPllAlpha, &c.pllAlpha},   {kCcMvs0Rcg, &c.mvs0RcgCmd},
      {kCcMvs0Rcg + 4, &c.mvs0RcgCfg}, {kCcAhbRcg, &c.ahbRcgCmd},           {kCcAhbRcg + 4, &c.ahbRcgCfg},
      {kCcMvs0cGdsc, &c.mvs0cGdsc},    {kCcMvs0cGdsc + kGdscCfg, &c.mvs0cGdscCfg},
      {kCcMvs0Gdsc, &c.mvs0Gdsc},      {kCcMvs0Gdsc + kGdscCfg, &c.mvs0GdscCfg},
      {kCcMvs0cClk, &c.mvs0c},         {kCcMvs0cFreerun, &c.mvs0cFreerun},   {kCcMvs0cDiv2, &c.mvs0cDiv2},
      {kCcMvs0Clk, &c.mvs0},           {kCcMvs0Freerun, &c.mvs0Freerun},     {kCcMvs0FreerunMem, &c.mvs0FreerunMem},
      {kCcMvs0Div, &c.mvs0Div},        {kCcAhbClk, &c.ahb},                  {kCcXoClk, &c.xo},
      {kCcSleepClk, &c.sleep},         {kCcMvs0Shift, &c.mvs0Shift},         {kCcMvs0cShift, &c.mvs0cShift},
  };
  for (const auto &w : words) *w.to = load(kVideoCc + w.off);
  *loads += sizeof words / sizeof words[0];
  c.ccRead = true;
  if (!gdscOn(c.mvs0cGdsc)) {
    c.why = "controller GDSC (MVS0C) collapsed: the core is cold and not read (as expected before any video driver)";
    return c;
  }
  if (!branchOn(c.gccAxi1) || !branchOn(c.mvs0c) || !branchOn(c.mvs0cFreerun)) {
    c.why = "controller GDSC on but its clocks (GCC AXI1, MVS0C, MVS0C free-running) are not all on: the core not read";
    return c;
  }
  c.controller = true;
  c.codec = gdscOn(c.mvs0Gdsc) && branchOn(c.gccAxi0) && branchOn(c.mvs0) && branchOn(c.mvs0Freerun);
  c.why = c.codec ? "controller and codec powered and clocked: both read"
                  : "controller powered and clocked: its registers read; the codec (MVS0) is off or unclocked: not read";
  return c;
}

// PLL0's output (Taycan ELU: XO x (L + alpha / 2^16), 16-bit alpha); 0 with L = 0.
inline uint64_t pllHz(uint32_t l, uint32_t alpha) { return kXoHz * (l & 0xffffu) + kXoHz * (alpha & 0xffffu) / 65536; }
inline bool pllLocked(uint32_t mode) { return mode >> 31; }
inline bool pllRunning(const Clocks &c) { return pllLocked(c.pllMode) && (c.pllOpmode & 7u) == 1; }
struct Rcg {
  bool rootOff, rootEn;
  unsigned source, halfDivider;   // source select; the divider x 2 (CFG 4:0 holds 2 x divider - 1; 0 is bypass, divider 1)
};
inline Rcg rcg(uint32_t cmd, uint32_t cfg)
{
  const unsigned field = cfg & 0x1fu;
  return {bool(cmd >> 31), bool((cmd >> 1) & 1u), (cfg >> 8) & 7u, field ? field + 1 : 2};
}
// The rate mvs0_clk_src produces from its configuration (source 0 XO, 1 PLL0 at the PLL's configured rate), and after the mvs0
// divider (the OPP clock): 0 when the source is unknown.
inline uint64_t mvs0SourceHz(const Clocks &c)
{
  const Rcg r = rcg(c.mvs0RcgCmd, c.mvs0RcgCfg);
  const uint64_t parent = r.source == 0 ? kXoHz : r.source == 1 ? pllHz(c.pllL, c.pllAlpha) : 0;
  return r.halfDivider ? parent * 2 / r.halfDivider : 0;
}
inline uint64_t mvs0Hz(const Clocks &c) { return mvs0SourceHz(c) / ((c.mvs0Div & 0xfu) + 1); }
// The OPP (DT iris_opp_table) a rate is within 2 MHz of, or -1.
inline int oppIndex(uint64_t hz)
{
  for (unsigned i = 0; i < sizeof kOppMHz / sizeof kOppMHz[0]; ++i) {
    const uint64_t mhz = hz / 1000000;
    if (mhz + 2 >= kOppMHz[i] && mhz <= kOppMHz[i] + 2) return int(i);
  }
  return -1;
}
inline const char *gdscState(uint32_t gdscr)
{
  if (gdscOn(gdscr)) return (gdscr >> 1) & 1u ? "on, hardware-controlled" : "on";
  return gdscr & 1u ? "off (software collapse)" : (gdscr >> 1) & 1u ? "off, hardware-controlled" : "off";
}

// ---------------------------------------------------------------------------------------------------------------- the core
struct Core {
  bool controllerRead = false, codecRead = false;
  uint32_t hwVersion = 0, ctrlInit = 0, ctrlStatus = 0, qtblInfo = 0, qtblAddr = 0, sciaArg3 = 0, sfrAddr = 0, ucAddr = 0, ucSize = 0;
  uint32_t h2xIntEn = 0, bridgeReset = 0, x2rpmh = 0;
  uint32_t intrStatus = 0, intrMask = 0, dbgLpiCtl = 0, dbgLpiStatus = 0, cpuNocLpiCtl = 0, cpuNocLpiStatus = 0, spare0 = 0;
  uint32_t powerStatus = 0, clockConfig = 0;
  uint32_t tzCpuStatus = 0, tzAxiClock = 0, tzFifoReset = 0;
  uint32_t aonLpiCtl = 0, aonLpiStatus = 0, aonSwReset = 0, aonClkCtl = 0, aonSpare = 0, aonVideoLpiCtl = 0, aonVideoLpiStatus = 0;
  uint32_t idle[kVppPipes] = {};
};
// The registers the survey loads from the core, each once: offsets from kIris. Never kCpuCsA2hSoftIntClr or kCpuIcSoftInt.
template <class Load>
Core surveyCore(const Clocks &c, Load load, unsigned *loads)
{
  Core r;
  if (!c.controller) return r;
  struct {
    uint32_t off;
    uint32_t *to;
  } const words[] = {
      {kWrapperHwVersion, &r.hwVersion}, {kCtrlInit, &r.ctrlInit},           {kCtrlStatus, &r.ctrlStatus},
      {kQtblInfo, &r.qtblInfo},          {kQtblAddr, &r.qtblAddr},           {kSciaCmdArg3, &r.sciaArg3},
      {kSfrAddr, &r.sfrAddr},            {kUcRegionAddr, &r.ucAddr},         {kUcRegionSize, &r.ucSize},
      {kH2xSoftIntEn, &r.h2xIntEn},      {kAhbBridgeSyncReset, &r.bridgeReset}, {kX2Rpmh, &r.x2rpmh},
      {kWrapperIntrStatus, &r.intrStatus}, {kWrapperIntrMask, &r.intrMask},  {kDebugBridgeLpiCtl, &r.dbgLpiCtl},
      {kDebugBridgeLpiStatus, &r.dbgLpiStatus}, {kCpuNocLpiCtl, &r.cpuNocLpiCtl}, {kCpuNocLpiStatus, &r.cpuNocLpiStatus},
      {kWrapperSpare0, &r.spare0},       {kCorePowerStatus, &r.powerStatus}, {kCoreClockConfig, &r.clockConfig},
      {kTzCpuStatus, &r.tzCpuStatus},    {kTzAxiClockConfig, &r.tzAxiClock}, {kTzQns4pdxFifoReset, &r.tzFifoReset},
      {kAonNocLpiCtl, &r.aonLpiCtl},     {kAonNocLpiStatus, &r.aonLpiStatus}, {kAonNocCoreSwReset, &r.aonSwReset},
      {kAonNocCoreClkCtl, &r.aonClkCtl}, {kAonSpare, &r.aonSpare},           {kAonVideoCtlNocLpiCtl, &r.aonVideoLpiCtl},
      {kAonVideoCtlNocLpiStatus, &r.aonVideoLpiStatus},
  };
  for (const auto &w : words) *w.to = load(kIris + w.off);
  *loads += sizeof words / sizeof words[0];
  r.controllerRead = true;
  if (c.codec) {
    for (unsigned i = 0; i < kVppPipes; ++i) r.idle[i] = load(kIris + kVcodecIdleStatus + 4 * i);
    *loads += kVppPipes;
    r.codecRead = true;
  }
  return r;
}

// What the readings say about the core and its firmware.
enum class CoreState : uint8_t {
  unknown,         // VIDEO_CC not read
  cold,            // the controller GDSC is collapsed (or GCC's video AHB is off)
  unclocked,       // the controller GDSC is on but its clocks are not
  noFirmware,      // powered, CTRL_STATUS 0 and no queue table: no firmware has started its HFI
  firmwareBooted,  // CTRL_STATUS init bit set, no error: a firmware runs (someone before XNU loaded it)
  firmwareError,   // CTRL_STATUS reports an error code
  otherState,      // powered, something written but not a booted firmware
};
inline unsigned ctrlError(uint32_t status) { return (status >> 1) & 0x7fu; }
inline CoreState coreState(const Clocks &c, const Core &r)
{
  if (!c.ccRead) return branchOn(c.gccAhb) ? CoreState::unknown : CoreState::cold;
  if (!gdscOn(c.mvs0cGdsc)) return CoreState::cold;
  if (!r.controllerRead) return CoreState::unclocked;
  if (ctrlError(r.ctrlStatus)) return CoreState::firmwareError;
  if (r.ctrlStatus & 1u) return CoreState::firmwareBooted;
  if (!r.ctrlStatus && !r.qtblInfo && !r.qtblAddr && !r.ucAddr) return CoreState::noFirmware;
  return CoreState::otherState;
}
inline const char *coreStateName(CoreState s)
{
  switch (s) {
  case CoreState::unknown: return "unknown";
  case CoreState::cold: return "cold";
  case CoreState::unclocked: return "unclocked";
  case CoreState::noFirmware: return "powered-no-firmware";
  case CoreState::firmwareBooted: return "firmware-running";
  case CoreState::firmwareError: return "firmware-error";
  case CoreState::otherState: return "powered-other";
  }
  return "?";
}
inline const char *coreVerdict(CoreState s)
{
  switch (s) {
  case CoreState::unknown: return "the core's power state is unknown (VIDEO_CC not read)";
  case CoreState::cold:
    return "the video core is off (GDSC collapsed or its AHB gated): level 2 powers it from cold, as Linux's iris_vpu_power_on does";
  case CoreState::unclocked: return "the controller domain is on but unclocked: someone left it half on (record and report)";
  case CoreState::noFirmware: return "the core is powered with no firmware: level 3's PAS load starts from here";
  case CoreState::firmwareBooted:
    return "a firmware already runs its HFI (unexpected before XNU): level 3 must shut it down (PAS shutdown 9) before loading";
  case CoreState::firmwareError: return "the firmware reports an error in CTRL_STATUS: record the code";
  case CoreState::otherState: return "the core is powered in a state that is neither cold nor a booted firmware: record it";
  }
  return "";
}

// --------------------------------------------------------------------------------------------------------------- the SMMU
// ARM SMMUv2 (MMU-500): SMR valid 31, mask 30:16, id 14:0; S2CR type 17:16 (0 translate through bank 7:0, 1 bypass, 2 fault);
// CBAR (GR1 +4b) type 17:16; CBA2R (GR1 0x800 +4b) bit 0 VA64; a bank's page: SCTLR +0 (M bit 0), TCR2 +0x10, TTBR0 +0x20,
// TCR +0x30, MAIR0 +0x38, FSR +0x58. IDR0 bit 14 CTTW (coherent table walks), 7:0 groups; IDR1 7:0 banks, 30:28 pages - 1 (log2).
// Read as PianoDpu's survey reads the same SMMU (DpuSurvey.h), for the video streams.
struct SmmuBank {
  uint32_t cbar = 0, cba2r = 0, sctlr = 0, tcr2 = 0, ttbr0lo = 0, ttbr0hi = 0, tcr = 0, mair0 = 0, fsr = 0;
  bool read = false;
};
struct SmmuEntry {
  unsigned group = 0;
  uint32_t smr = 0, s2cr = 0;
  SmmuBank bank;
};
struct SmmuSurvey {
  uint32_t scr0 = 0, idr0 = 0, idr1 = 0, groups = 0, banks = 0, pages = 0;
  const char *error = nullptr;
  SmmuEntry found[6];
  unsigned count = 0, more = 0;
  unsigned freeGroups = 0;      // stream-match groups without the valid bit
  unsigned banksReferenced = 0; // distinct banks a valid group translates through
  unsigned loads = 0;
};
inline bool smrCovers(uint32_t smr, uint16_t sid)
{
  const uint32_t mask = (smr >> 16) & 0x7fffu;
  return ((smr ^ sid) & 0x7fffu & ~mask) == 0;
}
inline bool coherentWalks(const SmmuSurvey &s) { return (s.idr0 >> 14) & 1u; }
template <class Read>
SmmuSurvey surveySmmu(unsigned level, Read rd)   // rd(offset from kSmmu)
{
  SmmuSurvey r;
  auto ld = [&](uint32_t off) {
    ++r.loads;
    return rd(off);
  };
  r.scr0 = ld(0x0);
  r.idr0 = ld(0x20);
  r.idr1 = ld(0x24);
  r.groups = r.idr0 & 0xffu;
  if (r.groups > 128) r.groups = 128;   // Qualcomm: groups above 128 do not behave architecturally
  r.banks = r.idr1 & 0xffu;
  r.pages = 1u << (((r.idr1 >> 28) & 7u) + 1);
  if ((r.idr1 >> 31) || !r.groups || !r.banks || r.banks > 128 || r.pages > 128 || uint64_t(r.pages + r.banks) * 4096 > kSmmuBytes) {
    r.error = "unsupported SMMU geometry (64 KiB pages or out of the 1 MiB window)";
    return r;
  }
  uint32_t used[4] = {0, 0, 0, 0};   // a bit per bank (<= 128)
  for (unsigned i = 0; i < r.groups; ++i) {
    const uint32_t smr = ld(0x800 + 4 * i);
    if (!(smr & 0x80000000u)) {
      ++r.freeGroups;
      continue;
    }
    const uint32_t s2cr = ld(0xc00 + 4 * i);
    const unsigned type = (s2cr >> 16) & 3u, bank = s2cr & 0xffu;
    if (type == 0 && bank < r.banks) used[bank / 32] |= 1u << (bank % 32);
    bool hit = false;
    for (uint32_t sid = kStreamFirst; sid <= kStreamScanLast; ++sid) hit |= smrCovers(smr, uint16_t(sid));
    if (!hit) continue;
    if (r.count >= sizeof r.found / sizeof r.found[0]) {
      ++r.more;
      continue;
    }
    SmmuEntry &e = r.found[r.count++];
    e.group = i;
    e.smr = smr;
    e.s2cr = s2cr;
    if (type != 0 || bank >= r.banks) continue;
    e.bank.cbar = ld(0x1000 + 4 * bank);
    e.bank.cba2r = ld(0x1800 + 4 * bank);
    if (level < 2) continue;
    const uint32_t cb = (r.pages + bank) * 4096;
    e.bank.sctlr = ld(cb + 0x00);
    e.bank.tcr2 = ld(cb + 0x10);
    e.bank.ttbr0lo = ld(cb + 0x20);
    e.bank.ttbr0hi = ld(cb + 0x24);
    e.bank.tcr = ld(cb + 0x30);
    e.bank.mair0 = ld(cb + 0x38);
    e.bank.fsr = ld(cb + 0x58);
    e.bank.read = true;
  }
  for (uint32_t w : used)
    for (; w; w &= w - 1) ++r.banksReferenced;
  return r;
}
inline const char *s2crType(uint32_t s2cr)
{
  const unsigned t = (s2cr >> 16) & 3u;
  return t == 0 ? "translate" : t == 1 ? "bypass" : t == 2 ? "fault" : "reserved";
}
inline const char *cbarType(uint32_t cbar)
{
  const unsigned t = (cbar >> 16) & 3u;
  return t == 0 ? "stage 2 only" : t == 1 ? "stage 1, stage 2 bypass" : t == 2 ? "stage 1, stage 2 fault" : "stage 1 + stage 2";
}
// The entry of one stream, or null.
inline const SmmuEntry *entryFor(const SmmuSurvey &s, uint16_t sid)
{
  for (unsigned i = 0; i < s.count; ++i)
    if (smrCovers(s.found[i].smr, sid)) return &s.found[i];
  return nullptr;
}
// One word per stream for the summary: "none", "bypass", "fault", "bank N off|on|?", "other".
inline const char *streamWord(const SmmuSurvey &s, uint16_t sid)
{
  if (s.error || !s.idr1) return "unread";
  const SmmuEntry *e = entryFor(s, sid);
  if (!e) return "none";
  const unsigned type = (e->s2cr >> 16) & 3u;
  if (type == 1) return "bypass";
  if (type != 0) return "fault";
  if (!e->bank.read) return "bank";
  return (e->bank.sctlr & 1u) ? "bank-on" : "bank-off";
}
inline const char *routeVerdict(const SmmuSurvey &s)
{
  if (s.error || !s.idr1) return "SMMU not read";
  const SmmuEntry *a = entryFor(s, kStreamFirst), *b = entryFor(s, kStreamSecond);
  if (!a && !b)
    return "no stream-match entry for 0x1940/0x1947 (the boot stages never ran the video core): level 4 takes a free group per "
           "stream and a free bank of its own, stage 1 with XNU's tables, IOVAs in [0x80000000, 0xe0000000)";
  if ((a && ((a->s2cr >> 16) & 3u) == 1) || (b && ((b->s2cr >> 16) & 3u) == 1))
    return "a video stream bypasses: device addresses would be physical, and no XNU page (all above 4 GiB) is below the core's "
           "0xe0000000 limit: level 4 must give the stream a translating bank instead";
  return "a video stream already has an entry: someone before XNU set it up; level 4 must decide whether to reuse or replace it "
         "(record the bank, its translation and its owner)";
}

// ------------------------------------------------------------------------------------------------------- the carveout (opt-in)
struct Carveout {
  bool read = false;
  uint32_t words[8] = {};
};
template <class Load>
Carveout surveyCarveout(Load load, unsigned *loads)
{
  Carveout c;
  for (unsigned i = 0; i < 8; ++i) c.words[i] = load(kCarveout + 4 * i);
  *loads += 8;
  c.read = true;
  return c;
}
inline const char *carveoutVerdict(const Carveout &c)
{
  if (!c.read) return "not read (piano-video-carveout=1 reads its first words)";
  if (c.words[0] == 0x464c457fu) return "an ELF header: an image is left there from an earlier load";
  bool zero = true, ones = true;
  for (uint32_t w : c.words) {
    zero &= w == 0;
    ones &= w == 0xffffffffu;
  }
  return zero ? "zeros (readable from EL1; nothing loaded)" : ones ? "all ones (unbacked or blocked?)" : "other data (readable)";
}

}  // namespace pianovideo
