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
//            writes, PERST# low for at least 100 ms, the endpoint's capability walk, BAR0's size and placement;
//   level 4: the firmware file's layout (one image, or a boot loader and a second ELF), the DMA arena's alignment, the BHIe vector
//            table, and the boot state machine from PBL through SBL to mission mode;
//   every injected fault (a domain that stays off, a branch that does not start, an RCG update that hangs, PHYSTATUS stuck, no L0,
//            an iATU that will not enable, the wrong endpoint, an impossible BAR mask, a refused rail vote, an unreadable rail, a
//            BHI error, a BHIe error, a system error, a crash dump, a timeout) must make the level fail at that step and leave
//            WLAN_EN low and PERST# asserted.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. bringup_host.cpp -o /tmp/bringup_host && /tmp/bringup_host
#include "WifiBringup.h"
#include "WifiMhi.h"
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

  Device()
  {
    regs[kMhiRegLen] = reglen;
    regs[kMhiBhiOff] = bhi;
    regs[kMhiBhieOff] = bhie;
    regs[kMhiErdbOff] = 0x400;
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
    if (reg == kMhiStatus) return status();
    if (reg == bhi + kBhiExecEnv) return ee;
    if (reg == bhi + kBhiStatus) {
      if (!bhiDoorbells || stuck) return 0;
      return (bhiError ? kMhiXferError : kMhiXferSuccess) << 30;
    }
    if (reg == bhie + kBhieTxVecStatus) {
      if (!bhieDoorbells) return 0;
      return ((bhieError ? kMhiXferError : kMhiXferSuccess) << 30) | (regs[bhie + kBhieTxVecDb] & kMhiSeqMask);
    }
    auto it = regs.find(reg);
    return it == regs.end() ? 0 : it->second;
  }
  void write(uint32_t reg, uint32_t value)
  {
    regs[reg] = value;
    if (reg == bhi + kBhiImgTxDb) {
      ++bhiDoorbells;
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
  if (failures) {
    std::printf("bringup_host: %d failures\n", failures);
    return 1;
  }
  std::printf("bringup_host: all passed\n");
  return 0;
}
