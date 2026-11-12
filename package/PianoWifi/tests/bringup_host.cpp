// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoWifi levels 2, 3 and 4 (WifiBringup.h, WifiPhySeq.h, WifiMhi.h) against register, PMIC/RPMh and MHI device
// models. What it holds the code to:
//   level 2: the six PMU rails are voted in piano's DT supply order, each with mode NPM, a voltage and an enable in one
//            transaction; the voltage asked for is piano's DT minimum when the PMIC reads lower, the PMIC's own value when it reads
//            higher inside the range, and no voltage command at all when it reads above the range; no rail is ever disabled or
//            lowered; the PHY's two LDOs (one of them the UFS PHY's PLL) get no command at all; a rail that cannot be read, or has
//            no cmd-db address, stops level 2 before any vote; WLAN_EN goes low, waits 150 ms and goes high after PERST# is low;
//   level 3: the order of the whole sequence (domains, clocks, resets, PARF, the PHY table, BDF-to-SID, root port, PERST# release,
//            LTSSM, L0, the configuration window, BAR0), the pipe mux switched to the PHY between the table's body and its last two
//            writes, PERST# low for at least 100 ms, the endpoint's capability walk, BAR0's size and placement, the root port's
//            unused I/O and prefetchable windows closed in Linux's order without a status bit written;
//   level 4: the firmware file's layout (one image, or a boot loader and a second ELF), the DMA arena's alignment, the BHIe vector
//            table, and the boot state machine from PBL through SBL to mission mode; before the doorbell (WifiPbl.h): the chip's
//            power-on reset in ath12k's order with every value kept (an LTSSM word that takes two writes, a link that does not
//            answer), the receive vector block cleared and nothing else, MSI programmed off-then-on with one vector for 32- and
//            64-bit capabilities and refused where the sink cannot be reached, the RAS DES capability found behind other
//            capabilities and the counters' deltas (an unsupported counter, a wrap), the contexts given only at READY; SHA-256 on
//            the standard vectors, in pieces across block boundaries and through a bounce buffer, and the two pins;
//   every injected fault (a domain that stays off, a branch that does not start, an RCG update that hangs, PHYSTATUS stuck, no L0,
//            an iATU that will not enable, the wrong endpoint, an impossible BAR mask, a refused rail vote, an unreadable rail, a
//            BHI error, a BHIe error, a system error, a crash dump, a timeout) must make the level fail at that step and leave
//            WLAN_EN low and PERST# asserted.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. bringup_host.cpp -o /tmp/bringup_host && /tmp/bringup_host
#include "WifiBringup.h"
#include "WifiMhi.h"
#include "WifiPbl.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianowifi;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static uint64_t key(unsigned win, uint32_t off) { return uint64_t(win) << 32 | off; }

// ------------------------------------------------------------------------------------------------------- the register model
struct Model {
  struct Store { unsigned win; uint32_t off, value; std::string what; uint64_t ms; };
  struct RailModel { uint8_t status = 0x80, enable = 0, mode = 0; uint16_t mv = 0; bool readable = true; };
  struct VrmCall { std::string resource; uint32_t reg, value; };

  std::map<uint64_t, uint32_t> regs;
  std::vector<Store> writes;
  std::map<uint16_t, RailModel> rails;
  std::vector<VrmCall> vrm;
  std::map<std::string, int> vrmFail;
  std::map<uint16_t, uint16_t> dropAfterVotes;   // a rail the model makes sag once the votes are in (an injected fault)
  uint64_t ms = 1000;
  unsigned voteLocks = 0, badAccess = 0, pmicReads = 0, cfgPolls = 0;
  // what the model does
  bool gdscUp = true, branches = true, phyReady = true, trains = true, atuEnables = true, rcgStuck = false;
  unsigned pendingIdPolls = 2, trainAfterMs = 5;
  uint32_t endpointId = uint32_t(kEndpointDevice) << 16 | kEndpointVendor;
  uint32_t barMask = 0xffe00004u;   // 2 MiB, 64-bit memory BAR
  bool barProbe = false, phyStarted = false, ltssmOn = false;
  uint64_t ltssmAt = 0;

  Model()
  {
    // The hand-over state: both domains collapsed, the votes clear, the pins as wlan-en-state / pcie0-default-state leave them.
    regs[key(kWinGcc, kGccCollapse)] = kCollapsePcie0 | kCollapsePcie0Phy;
    regs[key(kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride)] = 0x201;
    regs[key(kWinTlmm, kPins[kPinPerst].gpio * kTlmmStride)] = 0x201;
    regs[key(kWinDbi, kDbiCapPtr)] = 0x70;
    regs[key(kWinDbi, 0x70)] = 0x10;              // the root port's PCI Express capability, last in the list
    regs[key(kWinDbi, 0x70 + kExpLinkCtlSta)] = 0x00120000u;   // Gen2 x1 once trained
    for (unsigned i = 0; i < kRailCount; ++i) rails[uint16_t(0x100 + i)] = RailModel{};
  }
  void rail(unsigned i, uint16_t mv, uint8_t enable) { rails[uint16_t(0x100 + i)] = RailModel{0x80, enable, 7, mv, true}; }

  uint32_t vote(uint32_t word) const
  {
    auto it = regs.find(key(kWinGcc, word));
    return it == regs.end() ? 0 : it->second;
  }
  uint32_t stored(unsigned win, uint32_t off) const
  {
    auto it = regs.find(key(win, off));
    return it == regs.end() ? 0 : it->second;
  }

  uint32_t read(unsigned win, uint32_t off)
  {
    if (off & 3) ++badAccess;
    if (win == kWinGcc) {
      if (off == kGccPcie0Gdsc || off == kGccPcie0PhyGdsc) {
        const uint32_t bit = off == kGccPcie0Gdsc ? kCollapsePcie0 : kCollapsePcie0Phy;
        return (gdscUp && !(stored(kWinGcc, kGccCollapse) & bit)) ? kGdscPwrOn : 0;
      }
      for (unsigned i = 0; i < kBranchCount; ++i) {
        if (off != kBranches[i].cbcr) continue;
        const bool voted = (vote(kBranches[i].vote) >> kBranches[i].bit) & 1u;
        const bool running = branches && voted && (i != kPipe || phyStarted);
        return running ? 0u : kCbcrClkOff;
      }
      if (off == kGccPcie0AuxRcg || off == kGccPcie0RchngRcg) return rcgStuck ? stored(win, off) : stored(win, off) & ~kRcgUpdate;
    }
    if (win == kWinPhy && off == kPcs + kPcsStatus1) return (phyReady && phyStarted) ? 0u : kPcsPhyStatus;
    if (win == kWinDbi && off == kPortDebug0) return (ltssmOn && trains && ms >= ltssmAt + trainAfterMs) ? kLtssmL0 : 0x02u;
    if (win == kWinDbi && off == kPortDebug1)
      return (ltssmOn && trains && ms >= ltssmAt + trainAfterMs) ? kPortDebug1LinkUp : 0u;
    if (win == kWinAtu && (off == kAtuCtrl2 || off == kAtuRegion + kAtuCtrl2))
      return atuEnables ? stored(win, off) : stored(win, off) & ~kAtuCtrl2Enable;
    if (win == kWinCfg) {
      if (off == 0) return ++cfgPolls <= pendingIdPolls ? 0xffffffffu : endpointId;
      if (off == 0x10) return barProbe ? barMask : stored(win, off);
      if (off == 0x34) return 0x40;
      if (off == 0x40) return 0x05u | 0x70u << 8;   // MSI capability, next the PCI Express one
      if (off == 0x70) return 0x10u;
    }
    return stored(win, off);
  }

  void write(unsigned win, uint32_t off, uint32_t value, const char *what)
  {
    if (off & 3) ++badAccess;
    writes.push_back({win, off, value, what ? what : "?", ms});
    if (win == kWinPhy && off == kPcs + kPcsStart && value == 3) phyStarted = true;
    if (win == kWinParf && off == kParfLtssm && (value & kParfLtssmEnable) && !ltssmOn) {
      ltssmOn = true;
      ltssmAt = ms;
    }
    if (win == kWinCfg && off == 0x10) {
      barProbe = value == 0xffffffffu;
      if (!barProbe) regs[key(win, off)] = value;
      return;
    }
    regs[key(win, off)] = value;
  }

  // the last write of a window and offset, or nothing
  const Store *last(unsigned win, uint32_t off) const
  {
    for (size_t i = writes.size(); i-- > 0;)
      if (writes[i].win == win && writes[i].off == off) return &writes[i];
    return nullptr;
  }
  int indexOf(unsigned win, uint32_t off) const
  {
    for (size_t i = 0; i < writes.size(); ++i)
      if (writes[i].win == win && writes[i].off == off) return int(i);
    return -1;
  }
  int lastIndexOf(unsigned win, uint32_t off) const
  {
    for (size_t i = writes.size(); i-- > 0;)
      if (writes[i].win == win && writes[i].off == off) return int(i);
    return -1;
  }
};

static uint32_t mRead(void *ctx, unsigned win, uint32_t off) { return static_cast<Model *>(ctx)->read(win, off); }
static void mWrite(void *ctx, unsigned win, uint32_t off, uint32_t value, const char *what)
{
  static_cast<Model *>(ctx)->write(win, off, value, what);
}
static void mUdelay(void *ctx, unsigned us)
{
  Model &m = *static_cast<Model *>(ctx);
  m.ms += us >= 1000 ? us / 1000 : 0;
}
static void mMsleep(void *ctx, unsigned msec) { static_cast<Model *>(ctx)->ms += msec; }
static uint64_t mNow(void *ctx) { return static_cast<Model *>(ctx)->ms; }
static int mVoteLock(void *ctx, int take)
{
  if (take) ++static_cast<Model *>(ctx)->voteLocks;
  return 0;
}
static int mVrm(void *ctx, const char *resource, const VrmCmd *cmds, unsigned count, uint32_t *address)
{
  Model &m = *static_cast<Model *>(ctx);
  if (address) *address = 0x1000;
  auto fail = m.vrmFail.find(resource);
  if (fail != m.vrmFail.end()) return fail->second;
  for (unsigned i = 0; i < count; ++i) m.vrm.push_back({resource, cmds[i].reg, cmds[i].value});
  // the model's PMIC follows the votes, as RPMh would
  for (unsigned i = 0; i < kRailCount; ++i) {
    if (std::strcmp(kRails[i].cmdDb, resource)) continue;
    Model::RailModel &r = m.rails[uint16_t(0x100 + i)];
    for (unsigned k = 0; k < count; ++k) {
      if (cmds[k].reg == kVrmVoltage) r.mv = uint16_t(cmds[k].value);
      if (cmds[k].reg == kVrmEnable) r.enable = cmds[k].value ? 0x80 : 0;
      if (cmds[k].reg == kVrmMode) r.mode = uint8_t(cmds[k].value);
    }
  }
  for (const auto &drop : m.dropAfterVotes) m.rails[drop.first].mv = drop.second;
  return 0;
}
static int mPmic(void *ctx, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *mv)
{
  Model &m = *static_cast<Model *>(ctx);
  ++m.pmicReads;
  auto it = m.rails.find(ppid);
  if (it == m.rails.end() || !it->second.readable) return -5;
  if (status) *status = it->second.status;
  if (enable) *enable = it->second.enable;
  if (mode) *mode = it->second.mode;
  if (mv) *mv = it->second.mv;
  return 0;
}
static Hw hwOf(Model &m)
{
  Hw hw;
  hw.ctx = &m;
  hw.read = mRead;
  hw.write = mWrite;
  hw.udelay = mUdelay;
  hw.msleep = mMsleep;
  hw.now_ms = mNow;
  hw.vote_lock = mVoteLock;
  hw.vrm = mVrm;
  hw.pmic = mPmic;
  return hw;
}
static void fillState(PowerState &p)
{
  for (unsigned i = 0; i < kRailCount; ++i) {
    p.rails[i].ppid = uint16_t(0x100 + i);
    p.rails[i].address = 0x1000 + 0x10 * i;
  }
}
// After any failure: WLAN_EN driven low and PERST# asserted.
static void checkSafeOff(const Model &m)
{
  const Model::Store *en = m.last(kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride + 4);
  const Model::Store *perst = m.last(kWinTlmm, kPins[kPinPerst].gpio * kTlmmStride + 4);
  CHECK(en && en->value == 0);
  CHECK(perst && perst->value == 0);
}
// No rail is ever disabled or lowered, and the PHY's LDOs are never written at all.
static void checkVotesOnlyAdd(const Model &m, const uint16_t before[kRailCount])
{
  for (const Model::VrmCall &c : m.vrm) {
    CHECK(std::strcmp(c.resource.c_str(), "ldog3") != 0);   // the UFS PHY's PLL
    CHECK(std::strcmp(c.resource.c_str(), "ldof1") != 0);
    if (c.reg == kVrmEnable) CHECK(c.value == 1);
    if (c.reg == kVrmMode) CHECK(c.value == kVrmModeNpm);
    if (c.reg != kVrmVoltage) continue;
    for (unsigned i = 0; i < kRailCount; ++i)
      if (!std::strcmp(kRails[i].cmdDb, c.resource.c_str())) CHECK(c.value >= before[i]);
  }
}

// ----------------------------------------------------------------------------------------------------------------- level 2
static void testPowerUp()
{
  Model m;
  m.rail(0, 1800, 0x80);   // ldof3 already on at its only voltage
  m.rail(1, 0, 0);         // ldof2 off at 0 mV
  m.rail(2, 500, 0);       // smpf5 at the stock minimum: must be raised to 876
  m.rail(3, 1036, 0);      // smpd4 above what the chip needs but inside the range: kept at 1036
  m.rail(4, 1400, 0x80);   // smpi7 above its range: no voltage command at all
  m.rail(5, 0, 0);
  m.rail(6, 880, 0x80);    // the PHY's LDOs, read only
  m.rail(7, 1200, 0x80);
  uint16_t before[kRailCount];
  for (unsigned i = 0; i < kRailCount; ++i) before[i] = m.rails[uint16_t(0x100 + i)].mv;
  const Hw hw = hwOf(m);
  Options opt;
  PowerState p;
  fillState(p);
  CHECK(powerUp(p, hw, opt) == kErrNone);
  CHECK(p.step == kStepWlanEn && p.error == kErrNone);
  checkVotesOnlyAdd(m, before);

  // Every voted rail got mode and enable; the voltages are the ones above.
  unsigned seen = 0;
  for (unsigned i = 0; i < kVotedRails; ++i) {
    bool mode = false, enable = false;
    for (const Model::VrmCall &c : m.vrm) {
      if (std::strcmp(kRails[i].cmdDb, c.resource.c_str())) continue;
      mode = mode || c.reg == kVrmMode;
      enable = enable || c.reg == kVrmEnable;
    }
    CHECK(mode && enable);
    ++seen;
  }
  CHECK(seen == kVotedRails);
  CHECK(p.rails[2].voted == 876 && p.rails[3].voted == 1036 && p.rails[5].voted == 1864);
  CHECK(p.rails[4].voted == 0 && p.rails[4].keptVoltage);          // above its range: left alone
  CHECK(p.voted == 5 && p.kept == 1);
  CHECK(p.rails[1].enabledByUs && !p.rails[0].enabledByUs);

  // The order: ldof3, ldof2, smpf5, smpd4, smpi7, smpg3 (piano's DT supply order).
  std::vector<std::string> order;
  for (const Model::VrmCall &c : m.vrm)
    if (order.empty() || order.back() != c.resource) order.push_back(c.resource);
  const char *expected[kVotedRails] = {"ldof3", "ldof2", "smpf5", "smpd4", "smpi7", "smpg3"};
  CHECK(order.size() == kVotedRails);
  for (unsigned i = 0; i < kVotedRails && i < order.size(); ++i) CHECK(order[i] == expected[i]);

  // PERST# is asserted before WLAN_EN rises, and WLAN_EN is low for at least 150 ms before it goes high.
  const int perst = m.indexOf(kWinTlmm, kPins[kPinPerst].gpio * kTlmmStride + 4);
  const int enFirst = m.indexOf(kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride + 4);
  const int enLast = m.lastIndexOf(kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride + 4);
  CHECK(perst >= 0 && enFirst > perst && enLast > enFirst);
  CHECK(m.writes[size_t(enFirst)].value == 0 && m.writes[size_t(enLast)].value == 2);
  CHECK(m.writes[size_t(enLast)].ms - m.writes[size_t(enFirst)].ms >= opt.wlanEnLowMs);
  CHECK(p.perstAssertedMs && m.ms - p.perstAssertedMs >= opt.wlanEnLowMs + opt.pmuMs);
  // CLKREQ# got its PCIe function and no output enable.
  const Model::Store *clkreq = m.last(kWinTlmm, kPins[kPinClkReq].gpio * kTlmmStride);
  CHECK(clkreq && (clkreq->value & 3u) == 3u && ((clkreq->value >> 2) & 0xfu) == 1u && !(clkreq->value & (1u << 9)));
  CHECK(m.badAccess == 0);
}

static void testPowerFaults()
{
  {   // a rail whose PMIC state cannot be read: nothing is voted at all
    Model m;
    m.rails[uint16_t(0x100 + 4)].readable = false;
    const Hw hw = hwOf(m);
    Options opt;
    PowerState p;
    fillState(p);
    CHECK(powerUp(p, hw, opt) == kErrRailRead);
    CHECK(p.step == -kStepRailsRead && m.vrm.empty());
    checkSafeOff(m);
    // piano-wifi-blind=1 goes ahead
    Model n;
    n.rails[uint16_t(0x100 + 4)].readable = false;
    const Hw hw2 = hwOf(n);
    Options blind;
    blind.blind = 1;
    PowerState q;
    fillState(q);
    CHECK(powerUp(q, hw2, blind) == kErrNone);
    CHECK(!n.vrm.empty());
  }
  {   // no cmd-db address for a rail
    Model m;
    const Hw hw = hwOf(m);
    Options opt;
    PowerState p;
    fillState(p);
    p.rails[3].address = 0;
    CHECK(powerUp(p, hw, opt) == kErrRailVote);
    CHECK(p.step == -kStepRailsRead && m.vrm.empty());
  }
  {   // a refused vote stops level 2 at that rail
    Model m;
    const Hw hw = hwOf(m);
    Options opt;
    PowerState p;
    fillState(p);
    m.vrmFail["smpd4"] = -16;
    CHECK(powerUp(p, hw, opt) == kErrRailVote);
    CHECK(p.step == -kStepRailVote);
    CHECK(m.last(kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride + 4) != nullptr);   // safeOff still drove the pins
    checkSafeOff(m);
    for (const Model::VrmCall &c : m.vrm) CHECK(c.resource != "smpi7" && c.resource != "smpg3");   // nothing after it
  }
  {   // a rail that reads above what the chip needs is kept there, never lowered
    Model m;
    m.rail(2, 900, 0x80);
    const Hw hw = hwOf(m);
    Options opt;
    PowerState p;
    fillState(p);
    CHECK(powerUp(p, hw, opt) == kErrNone);
    CHECK(p.rails[2].voted == 900);
  }
  {   // a rail that sags once the votes are in: the check after them must catch it and stop level 2
    Model m;
    m.rail(5, 1900, 0x80);
    m.dropAfterVotes[uint16_t(0x100 + 5)] = 1700;
    const Hw hw = hwOf(m);
    Options opt;
    PowerState p;
    fillState(p);
    CHECK(powerUp(p, hw, opt) == kErrRailDropped);
    CHECK(p.step == -kStepRailsCheck);
    checkSafeOff(m);
  }
  {   // the UFS PHY's PLL dropping behind our back stops level 2 as well, although we never vote it
    Model m;
    m.rail(7, 1200, 0x80);
    m.dropAfterVotes[uint16_t(0x100 + 7)] = 1100;
    const Hw hw = hwOf(m);
    Options opt;
    PowerState p;
    fillState(p);
    CHECK(powerUp(p, hw, opt) == kErrRailDropped);
    for (const Model::VrmCall &c : m.vrm) CHECK(c.resource != "ldog3");
  }
  {   // piano-wifi-release=1: on a failure the enables this driver added are withdrawn, and nothing else is
    Model m;
    const Hw hw = hwOf(m);
    Options opt;
    opt.release = 1;
    m.vrmFail["smpg3"] = -16;
    PowerState p;
    fillState(p);
    CHECK(powerUp(p, hw, opt) == kErrRailVote);
    unsigned offs = 0;
    for (const Model::VrmCall &c : m.vrm)
      if (c.reg == kVrmEnable && c.value == 0) ++offs;
    CHECK(offs == 5);   // the five rails it enabled before the refused one
    checkSafeOff(m);
  }
  {   // piano-wifi-stop
    Model m;
    const Hw hw = hwOf(m);
    Options opt;
    opt.stopAfter = kStepRailsRead;
    PowerState p;
    fillState(p);
    CHECK(powerUp(p, hw, opt) == kErrStopped && m.vrm.empty());
  }
}

// ----------------------------------------------------------------------------------------------------------------- level 3
static int runLink(Model &m, LinkState &l, const Options &opt = Options{})
{
  const Hw hw = hwOf(m);
  PowerState p;
  fillState(p);
  CHECK(powerUp(p, hw, opt) == kErrNone);
  return linkUp(l, p, hw, opt);
}

static void testLink()
{
  Model m;
  LinkState l;
  CHECK(runLink(m, l) == kErrNone);
  CHECK(l.step == kStepBar && l.error == kErrNone);
  CHECK(gdscOn(l.gdsc) && gdscOn(l.phyGdsc) && !(l.collapseAfter & (kCollapsePcie0 | kCollapsePcie0Phy)));
  CHECK((l.vote8 & kVote8Bits) == kVote8Bits && (l.vote0 & kVote0Bits) == kVote0Bits);
  CHECK(l.clkref & kTcsrClkRefEnable);
  CHECK(l.phyWrites == kPhySequenceCount);
  CHECK(!(l.pcsStatus & kPcsPhyStatus));
  CHECK(l.sidHash == bdfHash(kEndpointRid) && l.sidEntry == (uint32_t(kEndpointRid) << 16 | 1u << 8));
  CHECK(!(l.sidCfg & kParfBdfToSidBypass));
  CHECK(l.perstLowMs >= 100);
  CHECK((l.debug0 & kLtssmMask) == kLtssmL0 && (l.debug1 & kPortDebug1LinkUp));
  CHECK(idIsEndpoint(l.endpointId) && l.endpointPolls == m.pendingIdPolls + 1);
  CHECK(l.endpointCap == 0x70 && l.endpointMsiCap == 0x40);
  // the window starts at 0x40300000, so a 2 MiB BAR lands on the next 2 MiB boundary
  CHECK(l.barBytes == 0x200000 && l.barAddress == barPlace(kMem32, kMem32Bytes, 0x200000) && l.barAddress == 0x40400000);
  CHECK((l.command & (kCommandMemory | kCommandMaster)) == (kCommandMemory | kCommandMaster));
  CHECK(l.memBaseLimit == memBaseLimit(kMem32, kMem32Bytes));
  CHECK(l.expCap == 0x70 && (l.linkCtl2 & 0xf) == kDefaultGen);
  CHECK(m.voteLocks >= 1);
  CHECK(m.badAccess == 0);

  // the order of the sequence
  const int collapse = m.indexOf(kWinGcc, kGccCollapse);
  const int vote = m.indexOf(kWinGcc, kGccVote8);
  const int reset = m.indexOf(kWinGcc, kResets[kResetPcie0].bcr);
  const int parf = m.indexOf(kWinParf, kParfDeviceType);
  const int phyFirst = m.indexOf(kWinPhy, kPhySequence[0].offset);
  const int pipe = m.lastIndexOf(kWinGcc, kGccPcie0PipeMux);
  const int start = m.indexOf(kWinPhy, kPcs + kPcsStart);
  const int sid = m.indexOf(kWinParf, kParfBdfToSidCfg);
  const int buses = m.indexOf(kWinDbi, kDbiBusNumbers);
  const int perstHigh = m.lastIndexOf(kWinTlmm, kPins[kPinPerst].gpio * kTlmmStride + 4);
  const int ltssm = m.indexOf(kWinParf, kParfLtssm);
  const int atu0 = m.indexOf(kWinAtu, kAtuCtrl2);
  const int atu1 = m.indexOf(kWinAtu, kAtuRegion + kAtuCtrl2);
  const int bar = m.lastIndexOf(kWinCfg, 0x10);
  CHECK(collapse < vote && vote < reset && reset < parf && parf < phyFirst);
  CHECK(phyFirst < pipe && pipe < start);                       // the pipe mux goes to the PHY before the start write
  CHECK(start < sid && sid < buses && buses < perstHigh && perstHigh < ltssm && ltssm < atu0 && atu0 < atu1 && atu1 < bar);
  CHECK(m.writes[size_t(perstHigh)].value == 2);
  // the PHY table is applied in its own order, and only inside the PHY window
  unsigned phySeen = 0;
  for (const Model::Store &w : m.writes) {
    if (w.win != kWinPhy) continue;
    CHECK(w.off == kPhySequence[phySeen].offset && w.value == kPhySequence[phySeen].value);
    CHECK(w.off + 4 <= kPhyBytes);
    ++phySeen;
  }
  CHECK(phySeen == kPhySequenceCount);

  // the root port's unused windows closed as Linux's enumeration leaves them, after the memory window and before the command,
  // the I/O range disabled through its upper half while its low half changes, the prefetchable limit's upper word first
  const int mem = m.indexOf(kWinDbi, kDbiMemBaseLimit);
  const int ioUp = m.indexOf(kWinDbi, kDbiIoUpper), io = m.indexOf(kWinDbi, kDbiIoBaseLimit);
  const int ioUpLast = m.lastIndexOf(kWinDbi, kDbiIoUpper), prefHi = m.indexOf(kWinDbi, kDbiPrefLimitHi);
  const int pref = m.indexOf(kWinDbi, kDbiPrefBaseLimit), prefBase = m.indexOf(kWinDbi, kDbiPrefBaseHi);
  const int command = m.indexOf(kWinDbi, kDbiCommand);
  CHECK(mem < ioUp && ioUp < io && io < ioUpLast && ioUpLast < prefHi && prefHi < pref && pref < prefBase && prefBase < command);
  CHECK(m.writes[size_t(ioUp)].value == kIoUpperDisabled && m.writes[size_t(ioUpLast)].value == 0);
  CHECK(m.writes[size_t(io)].value == kIoWindowClosed && !(m.writes[size_t(io)].value >> 16));   // no status bit written
  CHECK(m.writes[size_t(pref)].value == kPrefWindowClosed && m.writes[size_t(prefHi)].value == 0);
  CHECK(l.io == kIoWindowClosed && l.pref == kPrefWindowClosed);
  {   // what the controller had is kept for the log
    Model w;
    w.regs[key(kWinDbi, kDbiPrefBaseLimit)] = 0x0001fff1u;
    w.regs[key(kWinDbi, kDbiIoBaseLimit)] = 0x22000101u;   // secondary status bits above the I/O word
    LinkState lw;
    CHECK(runLink(w, lw) == kErrNone);
    CHECK(lw.prefBefore == 0x0001fff1u && lw.ioBefore == 0x0101u && lw.pref == kPrefWindowClosed);
  }
}

static void testLinkFaults()
{
  struct Case { const char *name; int error; int step; };
  {   // the power domain never comes up
    Model m;
    m.gdscUp = false;
    LinkState l;
    CHECK(runLink(m, l) == kErrGdsc && l.step == -kStepGdsc);
    checkSafeOff(m);
    CHECK(m.indexOf(kWinParf, kParfDeviceType) < 0);   // nothing was written to the controller
  }
  {   // a branch clock does not start
    Model m;
    m.branches = false;
    LinkState l;
    CHECK(runLink(m, l) == kErrBranch && l.step == -kStepClocks);
    checkSafeOff(m);
  }
  {   // an RCG update never finishes
    Model m;
    m.rcgStuck = true;
    LinkState l;
    CHECK(runLink(m, l) == kErrRcg && l.step == -kStepClocks);
  }
  {   // PHYSTATUS stays set
    Model m;
    m.phyReady = false;
    LinkState l;
    CHECK(runLink(m, l) == kErrPhy && l.step == -kStepPhy);
    CHECK(l.phyPolls == 100);          // the poll is bounded
    CHECK(m.indexOf(kWinParf, kParfLtssm) < 0);
    checkSafeOff(m);
  }
  {   // no L0
    Model m;
    m.trains = false;
    Options opt;
    opt.linkMs = 50;
    LinkState l;
    CHECK(runLink(m, l, opt) == kErrLink && l.step == -kStepLink);
    CHECK(l.linkMs >= opt.linkMs && l.linkMs < 4 * opt.linkMs);
    CHECK(m.indexOf(kWinAtu, kAtuCtrl2) < 0);   // the configuration window was never opened
    checkSafeOff(m);
  }
  {   // the iATU does not take its enable
    Model m;
    m.atuEnables = false;
    LinkState l;
    CHECK(runLink(m, l) == kErrAtu && l.step == -kStepEndpoint);
  }
  {   // the wrong endpoint
    Model m;
    m.endpointId = 0x11071a4c;
    LinkState l;
    CHECK(runLink(m, l) == kErrEndpoint && l.step == -kStepEndpoint);
    checkSafeOff(m);
  }
  {   // an endpoint that never answers
    Model m;
    m.pendingIdPolls = 10000;
    LinkState l;
    CHECK(runLink(m, l) == kErrEndpoint && l.endpointPolls == kEndpointAnswerMs / 10);
  }
  {   // a BAR mask that is not a power of two, and one that does not fit the window
    Model m;
    m.barMask = 0xffe00003u;   // an I/O BAR
    LinkState l;
    CHECK(runLink(m, l) == kErrBar && l.step == -kStepBar);
    Model n;
    n.barMask = 0x80000004u;   // 2 GiB: the 32-bit window is 61 MiB
    LinkState l2;
    CHECK(runLink(n, l2) == kErrBar);
    checkSafeOff(n);
  }
  (void)sizeof(Case);
}

static void testBarArithmetic()
{
  CHECK(barSize(0xffe00004u) == 0x200000);
  CHECK(barSize(0xfffff004u) == 0x1000);
  CHECK(barSize(0xffffffffu) == 0);        // unimplemented BAR
  CHECK(barSize(0) == 0);
  CHECK(barSize(0xffe00001u) == 0);        // I/O BAR
  CHECK(barSize(0xffe00003u) == 0);
  CHECK(barIs64(0xffe00004u) && !barIs64(0xffe00000u));
  CHECK(barPlace(0x40300000, 0x3d00000, 0x200000) == 0x40400000);
  CHECK(barPlace(0x40300000, 0x3d00000, 0x100000) == 0x40300000);
  CHECK(barPlace(0x40300000, 0x3d00000, 0x8000000) == 0);     // larger than the window
  CHECK(barPlace(0x40300000, 0x3d00000, 0) == 0);
  CHECK(memBaseLimit(0x40300000, 0x3d00000) == 0x43f04030u);
  CHECK(idPending(0xffffffffu) && idPending(0x11070001u) && !idPending(0x110e17cbu));
  CHECK(idIsEndpoint(0x110e17cbu) && !idIsEndpoint(0x110717cbu));
  CHECK(bdfHash(0) == 0 && bdfHash(kEndpointRid) == 0x15);    // PAGANI's value for 01:00.0
  CHECK(pinConfig(-1, 0, 2, true) == 0x201);                  // the DT's wlan-en-state word
  CHECK(pinConfig(1, 1, 2, false) == 0x007);
}

// ----------------------------------------------------------------------------------------------------------------- level 4
static void testFirmwareLayout()
{
  FwLayout out;
  std::vector<uint8_t> head(kMhiSblBytes + 8, 0);
  // one image (WCN7850): nothing that looks like an ELF after the boot loader
  CHECK(firmwareLayout(head.data(), uint32_t(head.size()), 2 * kMhiSblBytes, kMhiSblBytes, out));
  CHECK(!out.split && out.imageOffset == 0 && out.imageBytes == 2 * kMhiSblBytes && out.sblBytes == kMhiSblBytes);
  // a boot loader and a second ELF (peach)
  head[kMhiSblBytes] = 0x7f;
  head[kMhiSblBytes + 1] = 'E';
  head[kMhiSblBytes + 2] = 'L';
  head[kMhiSblBytes + 3] = 'F';
  CHECK(firmwareLayout(head.data(), uint32_t(head.size()), 6930496, kMhiSblBytes, out));
  CHECK(out.split && out.imageOffset == kMhiSblBytes && out.imageBytes == 6930496 - kMhiSblBytes);
  // a file smaller than the boot loader: one image, no second part
  CHECK(firmwareLayout(head.data(), uint32_t(head.size()), 1024, kMhiSblBytes, out));
  CHECK(!out.split && out.sblBytes == 1024 && out.imageBytes == 1024);
  CHECK(!firmwareLayout(nullptr, 0, 0, kMhiSblBytes, out));
  CHECK(!firmwareLayout(head.data(), 16, 6930496, kMhiSblBytes, out));   // not enough of the head to judge
  // the same from the four bytes alone
  const uint8_t elf[4] = {0x7f, 'E', 'L', 'F'}, other[4] = {0, 0, 0, 0};
  CHECK(firmwareLayoutFromMagic(elf, 6930496, kMhiSblBytes, out) && out.split);
  CHECK(firmwareLayoutFromMagic(other, 6930496, kMhiSblBytes, out) && !out.split);
  CHECK(!firmwareLayoutFromMagic(nullptr, 6930496, kMhiSblBytes, out));
  CHECK(firmwareLayoutFromMagic(nullptr, 1024, kMhiSblBytes, out) && !out.split && out.sblBytes == 1024);
}

static void testArena()
{
  std::vector<uint8_t> memory(4u << 20, 0xaa);
  Arena a;
  arenaInit(a, memory.data(), 0xc0000000, uint32_t(memory.size()));
  uint64_t bus = 0;
  uint8_t *p = arenaAlloc(a, 0x80000, &bus);
  CHECK(p && !(bus & 0x7ffff) && p[0] == 0 && p[0x7ffff] == 0);     // aligned to its size and zeroed
  uint8_t *q = arenaAlloc(a, 100, &bus);
  CHECK(q && !(bus & 63));
  uint8_t *big = arenaAlloc(a, 8u << 20, &bus);
  CHECK(!big && a.failures == 1);
  CHECK(a.allocations == 2);
  Arena empty;
  CHECK(!arenaAlloc(empty, 64, &bus));

  BhieEntry table[8];
  const uint64_t addresses[4] = {0x1000, 0x2000, 0x3000, 0x4000};
  CHECK(bhieTable(table, 8, addresses, 3 * kMhiSegBytes + 7, kMhiSegBytes) == 4);
  CHECK(table[0].size == kMhiSegBytes && table[3].size == 7 && table[3].address == 0x4000);
  CHECK(bhieTable(table, 2, addresses, 3 * kMhiSegBytes, kMhiSegBytes) == 0);   // the table is too small
  CHECK(bhieTable(table, 8, addresses, 0, kMhiSegBytes) == 0);
}

// An MHI device: PBL answers BHI, then READY, then (once M0 is asked for) SBL, then BHIe, then mission mode.
struct Device {
  std::map<uint32_t, uint32_t> regs;
  uint32_t ee = kEePbl, state = kStateBhi;
  bool ready = false, sysErr = false, bhiError = false, bhieError = false, rddm = false, stuck = false;
  unsigned bhiDoorbells = 0, bhieDoorbells = 0, m0 = 0, contextWrites = 0;
  uint32_t bhi = 0x200, bhie = 0x300, reglen = 0x1000;
  // BHI's status register: its power-on value (piano: all ones), whether a host write reaches it, the value the boot loader writes
  // on an error (piano's model: all ones), and whether the device answers at all (a link that went away reads all ones).
  uint32_t bhiStatus = 0, bhiErrorValue = kMhiXferError << 30;
  bool statusWritable = true, gone = false;
  bool echoDebug = false;   // on an error the debug words name the image's address (low, high) and size (wifi-3a, 2026-11-12)
  std::vector<uint32_t> bhiWrites;   // the BHI block's offsets in the order they were written

  Device()
  {
    regs[kMhiRegLen] = reglen;
    regs[kMhiBhiOff] = bhi;
    regs[kMhiBhieOff] = bhie;
    regs[kMhiErdbOff] = 0x400;
  }
  // piano, 2026-11-12: BHI status ffffffff and error code ef0e0003 before anything was written
  void piano()
  {
    bhiStatus = 0xffffffffu;
    bhiErrorValue = 0xffffffffu;
    regs[bhi + kBhiErrCode] = 0xef0e0003u;
    state = kStateReset;
  }
  uint32_t status() const
  {
    uint32_t v = state << kMhiStateShift;
    if (ready) v |= kMhiStatusReady;
    if (sysErr) v |= kMhiStatusSysErr;
    return v;
  }
  uint32_t read(uint32_t reg)
  {
    if (gone) return 0xffffffffu;
    if (reg == kMhiStatus) return status();
    if (reg == bhi + kBhiExecEnv) return ee;
    if (reg == bhi + kBhiStatus) return bhiStatus;
    if (reg == bhie + kBhieTxVecStatus) {
      if (!bhieDoorbells) return 0;
      return ((bhieError ? kMhiXferError : kMhiXferSuccess) << 30) | (regs[bhie + kBhieTxVecDb] & kMhiSeqMask);
    }
    auto it = regs.find(reg);
    return it == regs.end() ? 0 : it->second;
  }
  void write(uint32_t reg, uint32_t value)
  {
    if (gone) return;
    regs[reg] = value;
    if (reg >= bhi && reg < bhi + kBhiDumpBytes) bhiWrites.push_back(reg - bhi);
    if (reg == bhi + kBhiStatus && statusWritable) bhiStatus = value;
    if (reg == bhi + kBhiImgTxDb) {
      ++bhiDoorbells;
      if (!stuck) bhiStatus = bhiError ? bhiErrorValue : kMhiXferSuccess << 30;
      if (!stuck && bhiError && echoDebug) {
        regs[bhi + kBhiErrDbg1] = regs[bhi + kBhiImgAddrLo];
        regs[bhi + kBhiErrDbg2] = regs[bhi + kBhiImgAddrHi];
        regs[bhi + kBhiErrDbg3] = regs[bhi + kBhiImgSize];
      }
      if (!bhiError && !stuck) ready = true;
    }
    if (reg == bhie + kBhieTxVecDb) {
      ++bhieDoorbells;
      if (!bhieError) ee = kEeAmss;
    }
    if (reg == kMhiCtrl) {
      if (value & kMhiCtrlReset) {
        sysErr = false;
        regs[kMhiCtrl] = 0;
      } else if (((value >> kMhiStateShift) & 0xff) == kStateM0) {
        ++m0;
        ee = rddm ? kEeRddm : kEeSbl;
      }
    }
    if (reg == kMhiCcabap) ++contextWrites;
  }
  static uint32_t rd(void *ctx, uint32_t reg) { return static_cast<Device *>(ctx)->read(reg); }
  static void wr(void *ctx, uint32_t reg, uint32_t value) { static_cast<Device *>(ctx)->write(reg, value); }
  MhiIo io() { return {this, rd, wr}; }
};

// Drives the boot the way the kext's loop does, with a step budget instead of a clock.
static uint32_t runBoot(MhiBoot &boot, Device &d, MhiContexts &c, unsigned budget = 200)
{
  uint32_t phase = mhiBegin(boot, 0, 0xc0000000, kMhiSblBytes, 0xc1000000, 4 * sizeof(BhieEntry), 7);
  for (unsigned i = 0; i < budget && phase != kMhiDone && phase != kMhiFailed; ++i) {
    if (phase == kMhiNeedContexts) {
      mhiContextsProgram(c, d.io(), boot.reglen);
      phase = mhiContextsDone(boot);
    } else {
      phase = mhiStep(boot);
      if (phase != kMhiNeedContexts && phase != kMhiDone && phase != kMhiFailed) mhiEventAck(c, d.io(), boot.reglen);
    }
  }
  return phase;
}

static void testMhiBoot()
{
  std::vector<uint8_t> memory(kMhiContextBytes, 0);
  MhiContexts c;
  CHECK(mhiContextsPlan(c, memory.data(), 0xc2000000, kMhiContextBytes));
  CHECK(c.eventBytes[0] == 32 * kMhiElement && c.eventBytes[1] == 256 * kMhiElement);
  CHECK(c.eventBus[0] > 0xc2000000 && c.commandBus > c.eventBus[1]);
  MhiContexts tooSmall;
  CHECK(!mhiContextsPlan(tooSmall, memory.data(), 0xc2000000, 1024));

  Device d;
  MhiBoot boot;
  CHECK(mhiOpen(boot, d.io(), 0x1000));
  CHECK(boot.bhi == 0x200 && boot.bhie == 0x300 && boot.phase == kMhiIdle);
  mhiContextsProgram(c, d.io(), boot.reglen);
  CHECK(d.contextWrites == 1);
  CHECK(runBoot(boot, d, c) == kMhiDone);
  CHECK(boot.error == kMhiErrNone && mhiEeRead(boot) == kEeAmss);
  CHECK(d.bhiDoorbells == 1 && d.bhieDoorbells == 1 && d.m0 == 1);
  CHECK(d.contextWrites >= 2);   // again after the boot loader reset the registers
  CHECK(d.regs[kMhiCtrlLimit] == uint32_t(kMhiHostLimit) && d.regs[kMhiDataLimit] == uint32_t(kMhiHostLimit));

  {   // piano's BAR0: the owner maps 2 MiB (wifi-l4, 2026-11-12: MHIREGLEN 0x100, BHIOFF 0x100, BHIEOFF 0x224)
    Device bar;
    MhiBoot b;
    CHECK(mhiOpen(b, bar.io(), 0x200000) && b.reglen == 0x200000 && b.phase == kMhiIdle);
    MhiBoot huge;
    CHECK(!mhiOpen(huge, bar.io(), 0x2000000) && huge.error == kMhiErrLayout);
  }
  {   // a register layout that makes no sense
    Device bad;
    bad.regs[kMhiBhiOff] = 4;
    MhiBoot b;
    CHECK(!mhiOpen(b, bad.io(), 0x1000) && b.error == kMhiErrLayout && b.phase == kMhiFailed);
  }
  {   // the device is not in its boot loader
    Device amss;
    amss.ee = kEeAmss;
    MhiBoot b;
    CHECK(mhiOpen(b, amss.io(), 0x1000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, amss, k) == kMhiFailed && b.error == kMhiErrNotPbl);
  }
  {   // a system error at the start is reset first, then the boot goes on
    Device err;
    err.sysErr = true;
    MhiBoot b;
    CHECK(mhiOpen(b, err.io(), 0x1000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, err, k) == kMhiDone);
    CHECK(err.regs.count(kMhiCtrl));
  }
  {   // BHI reports an error
    Device bhi;
    bhi.bhiError = true;
    bhi.regs[bhi.bhi + kBhiErrCode] = 0x1234;
    MhiBoot b;
    CHECK(mhiOpen(b, bhi.io(), 0x1000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, bhi, k) == kMhiFailed && b.error == kMhiErrBhi && b.errcode == 0x1234);
    CHECK(b.failedPhase == kMhiSblLoading);
  }
  {   // the order of BHI's writes: the vector, the status cleared, the address high then low, the size, the doorbell last
    Device order;
    MhiBoot b;
    CHECK(mhiOpen(b, order.io(), 0x1000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, order, k) == kMhiDone);
    const std::vector<uint32_t> want = {kBhiIntVec, kBhiIntVec, kBhiStatus, kBhiImgAddrHi, kBhiImgAddrLo, kBhiImgSize, kBhiImgTxDb};
    CHECK(order.bhiWrites == want);
    CHECK(order.regs[order.bhi + kBhiImgSize] == kMhiSblBytes && order.regs[order.bhi + kBhiImgAddrLo] == 0xc0000000u);
    CHECK(order.regs[order.bhi + kBhiImgTxDb] == 7 && b.clearTook && b.statusBefore == 0);
  }
  {   // piano's power-on BHI status (all ones, error code ef0e0003), cleared by the host, and a boot loader that never answers
      // (2026-11-12: the image's DMA never completed): the phase stands until the owner's deadline, with the errors read then
    Device p;
    p.piano();
    p.stuck = true;
    p.regs[p.bhi + kBhiErrDbg1] = 0x11;
    p.regs[p.bhi + kBhiErrDbg3] = 0x33;
    MhiBoot b;
    CHECK(mhiOpen(b, p.io(), 0x200000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, p, k, 50) == kMhiSblLoading);
    CHECK(b.statusBefore == 0xffffffffu && b.statusCleared == 0 && b.clearTook && b.lastStatus == 0);
    CHECK(mhiTimedOut(b) == kMhiFailed && b.error == kMhiErrTimeout && b.errcode == 0xef0e0003u);
    CHECK(b.errdbg[0] == 0x11 && b.errdbg[1] == 0 && b.errdbg[2] == 0x33);
  }
  {   // the same, but the status register keeps its power-on value whatever the host writes: that value is not an error
    Device p;
    p.piano();
    p.stuck = true;
    p.statusWritable = false;
    MhiBoot b;
    CHECK(mhiOpen(b, p.io(), 0x200000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, p, k, 50) == kMhiSblLoading && !b.clearTook && b.lastStatus == 0xffffffffu && b.error == kMhiErrNone);
    p.bhiStatus = 0xc0000123u;   // ... until the boot loader writes an error of its own
    CHECK(mhiStep(b) == kMhiFailed && b.error == kMhiErrBhi && b.errcode == 0xef0e0003u && b.lastStatus == 0xc0000123u);
  }
  {   // piano's model of a refusal: the cleared status turns all ones after the doorbell, MHI_STATUS still answering: an error at
      // once (the old reading took all ones for "not answering" and waited 20 s)
    Device p;
    p.piano();
    p.bhiError = true;
    p.regs[p.bhi + kBhiErrCode] = 0x0000beefu;
    p.regs[p.bhi + kBhiErrDbg2] = 0x22;
    MhiBoot b;
    CHECK(mhiOpen(b, p.io(), 0x200000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, p, k, 3) == kMhiFailed && b.error == kMhiErrBhi && b.failedPhase == kMhiSblLoading);
    CHECK(b.lastStatus == 0xffffffffu && b.errcode == 0xbeefu && b.errdbg[1] == 0x22);
  }
  {   // the device gone (every read all ones, the link down): not a BHI error, the phase stands
    Device p;
    p.piano();
    p.stuck = true;
    MhiBoot b;
    CHECK(mhiOpen(b, p.io(), 0x200000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, p, k, 3) == kMhiSblLoading);
    p.gone = true;
    for (unsigned i = 0; i < 5; ++i) CHECK(mhiStep(b) == kMhiSblLoading);
    CHECK(b.error == kMhiErrNone);
  }
  {   // piano's power-on status and a boot loader that takes the image: the boot completes
    Device p;
    p.piano();
    MhiBoot b;
    CHECK(mhiOpen(b, p.io(), 0x200000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, p, k) == kMhiDone && b.error == kMhiErrNone && b.statusBefore == 0xffffffffu && b.clearTook);
  }
  {   // wifi-3a / 3b (2026-11-12): BHI's status 0 before the clear, then the boot loader's own ffffffff / ef0e0003 with the debug
      // words naming the image's address and size: an error at once, the status before the clear recorded as 0, the words read
    Device p;
    p.state = kStateReset;
    p.bhiError = true;
    p.bhiErrorValue = 0xffffffffu;
    p.echoDebug = true;
    p.regs[p.bhi + kBhiErrCode] = 0xef0e0003u;
    MhiBoot b;
    CHECK(mhiOpen(b, p.io(), 0x200000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0x20708000, kMhiContextBytes));
    CHECK(runBoot(b, p, k, 3) == kMhiFailed && b.error == kMhiErrBhi && b.statusBefore == 0 && b.clearTook);
    CHECK(b.errcode == 0xef0e0003u && b.errdbg[0] == 0xc0000000u && b.errdbg[1] == 0 && b.errdbg[2] == kMhiSblBytes);
    CHECK(p.contextWrites == 0);   // nothing of the contexts reached the device before the refusal
  }
  {   // BHIe reports an error
    Device bhie;
    bhie.bhieError = true;
    MhiBoot b;
    CHECK(mhiOpen(b, bhie.io(), 0x1000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, bhie, k) == kMhiFailed && b.error == kMhiErrBhie);
  }
  {   // the device goes to its crash dump environment
    Device crash;
    crash.rddm = true;
    MhiBoot b;
    CHECK(mhiOpen(b, crash.io(), 0x1000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, crash, k) == kMhiFailed && b.error == kMhiErrRddm);
  }
  {   // a device that never answers: the phase stands and the owner's deadline ends it
    Device slow;
    slow.stuck = true;
    MhiBoot b;
    CHECK(mhiOpen(b, slow.io(), 0x1000));
    MhiContexts k;
    CHECK(mhiContextsPlan(k, memory.data(), 0xc2000000, kMhiContextBytes));
    CHECK(runBoot(b, slow, k, 20) == kMhiSblLoading);
    CHECK(mhiTimedOut(b) == kMhiFailed && b.error == kMhiErrTimeout && b.failedPhase == kMhiSblLoading);
  }
  {   // contexts asked for in the wrong phase
    Device d2;
    MhiBoot b;
    CHECK(mhiOpen(b, d2.io(), 0x1000));
    CHECK(mhiContextsDone(b) == kMhiFailed && b.error == kMhiErrState);
  }
}

// ------------------------------------------------------------------------- level 4 before the doorbell (WifiPbl.h, §10.23)
static std::string hex(const uint8_t d[32])
{
  static const char *digits = "0123456789abcdef";
  std::string s;
  for (unsigned i = 0; i < 32; ++i) {
    s += digits[d[i] >> 4];
    s += digits[d[i] & 15];
  }
  return s;
}

static void testSha256()
{
  struct Vector { const char *text; const char *digest; };
  const Vector vectors[] = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
  };
  for (const Vector &v : vectors) {
    Sha256 s;
    sha256Update(s, v.text, std::strlen(v.text));
    uint8_t d[32];
    sha256Final(s, d);
    CHECK(hex(d) == v.digest);
  }
  // a million 'a', in pieces that straddle the block boundary every way
  std::vector<uint8_t> a(1000000, 'a');
  for (size_t piece : {size_t(1), size_t(63), size_t(64), size_t(65), size_t(4096), size_t(1000000)}) {
    Sha256 s;
    for (size_t at = 0; at < a.size(); at += piece) sha256Update(s, a.data() + at, std::min(piece, a.size() - at));
    uint8_t d[32];
    sha256Final(s, d);
    CHECK(hex(d) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  // the image's parts hashed through a small bounce buffer are the file hashed whole
  std::vector<uint8_t> file(3 * 1000 + 17);
  for (size_t i = 0; i < file.size(); ++i) file[i] = uint8_t(i * 37 + (i >> 5));
  uint8_t whole[32], parts[32];
  {
    Sha256 s;
    sha256Update(s, file.data(), file.size());
    sha256Final(s, whole);
  }
  const Span spans[3] = {{file.data(), 1000}, {file.data() + 1000, 2000}, {file.data() + 3000, 17}};
  uint8_t bounce[96];
  unsigned copies = 0;
  hashSpans(spans, 3, bounce, sizeof bounce, [&](uint8_t *to, const uint8_t *from, uint32_t n) {
    CHECK(n <= sizeof bounce);
    std::memcpy(to, from, n);
    ++copies;
  }, parts);
  CHECK(sha256Same(whole, parts) && copies == 11 + 21 + 1);
  parts[31] ^= 1;
  CHECK(!sha256Same(whole, parts));
  // the pins are the manifest's (xiaomipad-8pro-mainline firmware/piano-wlan.json) and the boot loader's
  CHECK(hex(kAmssStockSha256) == "02bbc028f73229c96ccede7474b230d39d2186dc5d9db7737f69381b8dd33cac");
  CHECK(hex(kAmssStockSblSha256) == "c0e4bb917b2979f57790922860aa01a52ca7772db62e0f9597e0bd56ff79d708");
}

// The chip's registers as chipPowerOnReset reaches them: an LTSSM word that takes a few writes, a reset cause that clears on read.
struct Chip {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  unsigned ltssmTakesAfter = 2, ltssmWrites = 0, sleptMs = 0;
  bool linkDown = false;
  uint32_t read(uint32_t a)
  {
    if (linkDown) return 0xffffffffu;
    if (a == kChipWlaonResetCause) {
      const uint32_t v = regs[a];
      regs[a] = 0;
      return v;
    }
    return regs[a];
  }
  void write(uint32_t a, uint32_t v)
  {
    writes.push_back({a, v});
    if (a == kChipParfLtssm) {
      if (++ltssmWrites < ltssmTakesAfter) return;   // not yet taken
    }
    if (!linkDown) regs[a] = v;
  }
  static uint32_t rd(void *c, uint32_t a) { return static_cast<Chip *>(c)->read(a); }
  static void wr(void *c, uint32_t a, uint32_t v) { static_cast<Chip *>(c)->write(a, v); }
  ChipIo io() { return {this, rd, wr}; }
  int indexOf(uint32_t a, size_t from = 0) const
  {
    for (size_t i = from; i < writes.size(); ++i)
      if (writes[i].first == a) return int(i);
    return -1;
  }
};

static void testChipPowerOn()
{
  Chip c;
  c.regs[kChipParfLtssm] = 0x100;
  c.regs[kChipGccHotReset] = 0x1;
  c.regs[kChipQfpromPwrCtrl] = 0xc;              // VDD4BLOW (bit 2) on, another bit that must survive
  c.regs[kChipWlaonWarmSwEntry] = 0x5a;
  c.regs[kChipWlaonResetCause] = 0x3;
  c.regs[kChipSocGlobalReset] = 0x4;
  c.regs[kChipQ6Cookie] = 0xc0000000u;
  c.regs[kMhiStatus] = 0x4;                      // a SYSERR, as the global reset may leave
  c.regs[0x100 + kBhiExecEnv] = kEePbl;
  ChipReset r;
  chipPowerOnReset(c.io(), [&](unsigned ms) { c.sleptMs += ms; }, 0x100, r);
  CHECK(r.ltssmBefore == 0x100 && r.ltssm == kChipParfLtssmValue && r.ltssmWrites == 2);
  CHECK(r.hotResetBefore == 0x1 && r.hotReset == (0x1 | kChipGccHotResetBit));
  CHECK(r.qfpromBefore == 0xc && r.qfprom == 0x8);
  CHECK(r.cookie == 0xc0000000u && r.warm == 0x5a && r.warmAfter == 0 && r.cause == 3 && c.regs[kChipWlaonResetCause] == 0);
  CHECK(r.globalBefore == 4 && r.globalAfter == 4 && !r.linkLost);
  CHECK(r.mhiStatusBefore == 0x4 && r.ee == kEePbl);
  CHECK(c.sleptMs == kChipLtssmSettleMs + 5 * kChipSocResetMs);
  // ath12k's order: the LTSSM word, the hot reset bit, the interrupts, the QFPROM, the vector words, the warm entry, the global
  // reset set then cleared, MHICTRL's reset last
  const int ltssm = c.indexOf(kChipParfLtssm), hot = c.indexOf(kChipGccHotReset), intr = c.indexOf(kChipPcieIntAllClear);
  const int qf = c.indexOf(kChipQfpromPwrCtrl), txdb = c.indexOf(kChipTxVecDb), txst = c.indexOf(kChipTxVecStatus);
  const int rxdb = c.indexOf(kChipRxVecDb), rxst = c.indexOf(kChipRxVecStatus), warm = c.indexOf(kChipWlaonWarmSwEntry);
  const int gset = c.indexOf(kChipSocGlobalReset), gclr = c.indexOf(kChipSocGlobalReset, size_t(gset) + 1);
  const int ctrl = c.indexOf(kMhiCtrl);
  CHECK(ltssm >= 0 && ltssm < hot && hot < intr && intr < qf && qf < txdb && txdb < txst && txst < rxdb && rxdb < rxst &&
        rxst < warm && warm < gset && gset < gclr && gclr < ctrl && ctrl == int(c.writes.size()) - 1);
  CHECK(c.writes[size_t(intr)].second == 0xffffffffu && c.writes[size_t(warm)].second == 0);
  CHECK(c.writes[size_t(gset)].second == 5 && c.writes[size_t(gclr)].second == 4 && c.writes[size_t(ctrl)].second == kMhiCtrlReset);
  for (int i : {txdb, txst, rxdb, rxst}) CHECK(c.writes[size_t(i)].second == 0);
  {   // the link not answering: the LTSSM word is tried kChipLtssmTries times with a pause each, and the loss is reported
    Chip d;
    d.linkDown = true;
    ChipReset rd;
    chipPowerOnReset(d.io(), [&](unsigned ms) { d.sleptMs += ms; }, 0x100, rd);
    CHECK(rd.ltssmWrites == kChipLtssmTries && rd.ltssm == 0xffffffffu && rd.linkLost);
    CHECK(d.sleptMs == kChipLtssmTries * kChipLtssmSettleMs + kChipLtssmSettleMs + 5 * kChipSocResetMs);
  }
  {   // an LTSSM word already right is not written
    Chip e;
    e.regs[kChipParfLtssm] = kChipParfLtssmValue;
    ChipReset re;
    chipPowerOnReset(e.io(), [](unsigned) {}, 0, re);
    CHECK(re.ltssmWrites == 0 && e.indexOf(kChipParfLtssm) < 0 && re.ee == 0);
  }
}

static void testRxVectorClear()
{
  Device d;
  MhiBoot b;
  CHECK(mhiOpen(b, d.io(), 0x1000));
  for (uint32_t off = kBhieRxVecAddrLo; off <= kBhieRxVecStatus; off += 4) d.regs[d.bhie + off] = 0xdeadbeefu;
  CHECK(mhiClearRxVectors(b) == 7);
  for (uint32_t off = kBhieRxVecAddrLo; off <= kBhieRxVecStatus; off += 4) CHECK(d.regs[d.bhie + off] == 0);
  CHECK(d.regs[d.bhie + kBhieTxVecDb] == 0);   // nothing else written: the transmit vector is the boot's own
  CHECK(d.bhiWrites.empty());
  CHECK(runBoot(b, d, *[] { static std::vector<uint8_t> mem(kMhiContextBytes); static MhiContexts k;
                            mhiContextsPlan(k, mem.data(), 0xc2000000, kMhiContextBytes); return &k; }()) == kMhiDone);
  CHECK(mhiClearRxVectors(b) == 0);            // only before the boot
  Device small;
  small.regs[kMhiBhieOff] = 0x1000 - 0x50;     // BHIe's transmit words fit the mapping, its receive block would not
  MhiBoot s;
  CHECK(mhiOpen(s, small.io(), 0x1000));
  CHECK(mhiClearRxVectors(s) == 0);
}

// The endpoint's configuration space for msiEnable and the RAS DES counters.
struct Config {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint32_t rasAt = 0;
  std::map<uint32_t, uint32_t> counters;   // group << 8 | event -> count
  std::map<uint32_t, bool> enabled;
  uint32_t selected = 0, unsupported = ~0u;   // a counter that never reads as enabled (group << 8 | event)
  uint32_t read(uint32_t off)
  {
    if (rasAt && off == rasAt + kRasDesCtrl) {
      const uint32_t key = selected;
      return ((key >> 8) << 24) | ((key & 0xff) << 16) | (enabled[key] ? kRasDesStatus : 0u);
    }
    if (rasAt && off == rasAt + kRasDesData) return counters[selected];
    auto it = regs.find(off);
    return it == regs.end() ? 0 : it->second;
  }
  void write(uint32_t off, uint32_t v)
  {
    writes.push_back({off, v});
    if (rasAt && off == rasAt + kRasDesCtrl) {
      selected = ((v >> 24) & 0xf) << 8 | ((v >> 16) & 0xff);
      if (((v >> 2) & 7) == 3 && selected != unsupported) enabled[selected] = true;
      return;
    }
    regs[off] = v;
  }
};

static void testMsi()
{
  for (bool is64 : {true, false}) {
    Config c;
    c.regs[0x04] = 0x00100006u;
    c.regs[0x50] = 0x05u | 0x70u << 8 | uint32_t(0x0006u | (is64 ? kMsiCtl64 : 0u) | kMsiCtlMask) << 16;   // 8 vectors capable
    auto rd = [&](uint32_t o) { return c.read(o); };
    auto wr = [&](uint32_t o, uint32_t v) { c.write(o, v); };
    MsiSetup m;
    const uint64_t sink = 0x20708000ull;
    CHECK(msiEnable(m, rd, wr, 0x50, sink, 0x4d53));
    CHECK(m.enabled && m.is64 == is64 && !m.why);
    CHECK((m.ctl & kMsiCtlEnable) && !(m.ctl & kMsiCtlMmeMask));   // one vector
    CHECK(c.regs[0x54] == uint32_t(sink) && c.regs[is64 ? 0x5c : 0x58] == 0x4d53u);
    if (is64) CHECK(c.regs[0x58] == 0);
    CHECK(c.regs[is64 ? 0x60 : 0x5c] == 0);                      // no vector masked
    CHECK(c.regs[0x04] == (0x0006u | kCommandIntxDisable));       // INTx off, no status bit written
    // the capability is off while it is programmed, and switched on last
    CHECK(!c.writes.empty() && c.writes.front().first == 0x50 && !((c.writes.front().second >> 16) & kMsiCtlEnable));
    CHECK(c.writes.back().first == 0x50 && ((c.writes.back().second >> 16) & kMsiCtlEnable));
    CHECK((c.writes.back().second & 0xffffu) == (c.regs[0x50] & 0xffffu));
  }
  {   // a 32-bit capability cannot take a sink above 4 GiB: nothing is written
    Config c;
    c.regs[0x50] = 0x05u | 0x0006u << 16;   // 32-bit addresses only
    MsiSetup m;
    CHECK(!msiEnable(m, [&](uint32_t o) { return c.read(o); }, [&](uint32_t o, uint32_t v) { c.write(o, v); }, 0x50,
                     0x9aa600000ull, 1));
    CHECK(m.why && c.writes.empty());
  }
  {   // no capability, or not an MSI one
    Config c;
    c.regs[0x50] = 0x10u;
    MsiSetup m;
    auto rd = [&](uint32_t o) { return c.read(o); };
    auto wr = [&](uint32_t o, uint32_t v) { c.write(o, v); };
    CHECK(!msiEnable(m, rd, wr, 0, 0x1000, 1) && m.why);
    CHECK(!msiEnable(m, rd, wr, 0x50, 0x1000, 1) && m.why && c.writes.empty());
  }
}

static void testRasDes()
{
  Config c;
  c.regs[0] = 0x011c17cbu;                           // Qualcomm's root port
  c.regs[0x100] = 0x00010001u | 0x148u << 20;        // AER, next 0x148
  c.regs[0x148] = kExtCapVendor | 0x1u << 16 | 0x200u << 20;   // a vendor capability of another kind first
  c.regs[0x14c] = 0x00100001u;
  c.regs[0x200] = kExtCapVendor | 0x1u << 16;        // RAS DES, last in the list
  c.regs[0x204] = kRasDesVsecId | kRasDesVsecRev << 16 | 0x100u << 20;
  c.rasAt = 0x200;
  c.unsupported = 3u << 8 | 0x05;                    // completion timeouts not counted on this one
  c.counters[7u << 8 | 0x0c] = 100;                  // rx MRd before the doorbell
  auto rd = [&](uint32_t o) { return c.read(o); };
  auto wr = [&](uint32_t o, uint32_t v) { c.write(o, v); };
  RasDes r;
  r.at = rasDesFind(rd);
  CHECK(r.at == 0x200);
  rasDesArm(r, rd, wr);
  CHECK(r.at == 0x200 && __builtin_popcount(r.on) == int(kRasEventCount) - 1 && !rasOn(r, 3, 0x05) && rasOn(r, 7, 0x0c));
  c.counters[7u << 8 | 0x0c] += 1024;                // the chip's reads of the image
  c.counters[7u << 8 | 0x07] += 4096;                // the completions with data
  c.counters[3u << 8 | 0x05] += 9;                   // not counted: reads as off
  rasDesRead(r, rd, wr);
  CHECK(rasDelta(r, 7, 0x0c) == 1024 && rasDelta(r, 7, 0x07) == 4096 && rasDelta(r, 7, 0x06) == 0 && rasDelta(r, 3, 0x05) == 0);
  CHECK(rasDelta(r, 9, 9) == 0 && !rasOn(r, 9, 9));
  // no write ever touches the counter data, and every control write selects a known event
  for (const auto &w : c.writes) {
    CHECK(w.first == 0x200 + kRasDesCtrl);
    bool known = false;
    for (const RasEvent &e : kRasEvents) known |= (w.second & 0x0fff0000u) == rasDesSelect(e);
    CHECK(known);
  }
  // a counter that wraps still gives the right delta
  RasDes wrap;
  wrap.at = 0x200;
  c.counters[7u << 8 | 0x0b] = 0xfffffff0u;
  rasDesArm(wrap, rd, wr);
  c.counters[7u << 8 | 0x0b] += 0x20;
  rasDesRead(wrap, rd, wr);
  CHECK(rasDelta(wrap, 7, 0x0b) == 0x20);
  {   // not Qualcomm's, no capability, a broken list: none found, nothing written
    Config o;
    o.regs[0] = 0x12345678u;
    CHECK(rasDesFind([&](uint32_t x) { return o.read(x); }) == 0);
    Config e;
    e.regs[0] = 0x110e17cbu;
    e.regs[0x100] = 0xffffffffu;
    CHECK(rasDesFind([&](uint32_t x) { return e.read(x); }) == 0);
    Config loop;
    loop.regs[0] = 0x110e17cbu;
    loop.regs[0x100] = 0x00010001u | 0x100u << 20;   // points at itself
    CHECK(rasDesFind([&](uint32_t x) { return loop.read(x); }) == 0);
    RasDes none;
    rasDesArm(none, [&](uint32_t x) { return o.read(x); }, [&](uint32_t x, uint32_t v) { o.write(x, v); });
    rasDesRead(none, [&](uint32_t x) { return o.read(x); }, [&](uint32_t x, uint32_t v) { o.write(x, v); });
    CHECK(o.writes.empty() && !none.on);
  }
}

static void testBhiIdentity()
{
  std::map<uint32_t, uint32_t> regs;
  auto rd = [&](uint32_t o) { auto it = regs.find(o); return it == regs.end() ? 0u : it->second; };
  const BhiIdentity empty = bhiIdentity(rd, 0x100);
  CHECK(empty.hwIdNonZero == 0 && empty.pkHashNonZero == 0 && empty.pkHashAllOnes == 0);
  for (uint32_t i = 0; i < kBhiMsmHwIdWords; ++i) regs[0x100 + kBhiMsmHwId + 4 * i] = i ? 0 : 0x001b0e1u;
  for (uint32_t i = 0; i < kBhiOemPkHashWords; ++i) regs[0x100 + kBhiOemPkHash + 4 * i] = i < 12 ? 0x9e3779b9u * (i + 1) : 0;
  regs[0x100 + kBhiOemPkHash + 4 * 23] = 0xffffffffu;
  const BhiIdentity a = bhiIdentity(rd, 0x100), b = bhiIdentity(rd, 0x100);
  CHECK(a.hwIdNonZero == 1 && a.pkHashNonZero == 13 && a.pkHashAllOnes == 1 && a.digest == b.digest && a.digest != empty.digest);
  regs[0x100 + kBhiOemPkHash] ^= 1;   // one bit of the hash changed: another digest
  CHECK(bhiIdentity(rd, 0x100).digest != a.digest);
  // only the 30 words are read, and the serial number (+0x40) is not among them
  std::vector<uint32_t> seen;
  (void)bhiIdentity([&](uint32_t o) { seen.push_back(o); return rd(o); }, 0x100);
  CHECK(seen.size() == kBhiMsmHwIdWords + kBhiOemPkHashWords);
  for (uint32_t o : seen) CHECK(o >= 0x100 + kBhiMsmHwId && o < 0x100 + kBhiOemPkHash + 4 * kBhiOemPkHashWords && o != 0x100 + kBhiSerialNumber);
}

static void testContextsAtReady()
{
  // The MHI host's order: no context register before the doorbell, the contexts given once the device reports READY. The
  // device model takes the image either way; what matters is that the boot completes with exactly the one context write at READY.
  std::vector<uint8_t> memory(kMhiContextBytes, 0);
  MhiContexts c;
  CHECK(mhiContextsPlan(c, memory.data(), 0x20708000, kMhiContextBytes));
  Device d;
  MhiBoot b;
  CHECK(mhiOpen(b, d.io(), 0x200000));
  CHECK(mhiClearRxVectors(b) == 7);
  CHECK(d.contextWrites == 0);
  CHECK(runBoot(b, d, c) == kMhiDone);
  CHECK(d.contextWrites == 1 && c.writes == 1 && d.regs[kMhiCcabap] == 0x20708000u);
}

int main()
{
  testPowerUp();
  testPowerFaults();
  testLink();
  testLinkFaults();
  testBarArithmetic();
  testFirmwareLayout();
  testArena();
  testMhiBoot();
  testSha256();
  testChipPowerOn();
  testRxVectorClear();
  testMsi();
  testRasDes();
  testBhiIdentity();
  testContextsAtReady();
  if (failures) {
    std::printf("bringup_host: %d failures\n", failures);
    return 1;
  }
  std::printf("bringup_host: all passed\n");
  return 0;
}
