// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of UsbCold.h, PianoDwc3's cold start (piano-usb-cold), against a model of piano's USB hardware: GCC (the USB30 GDSC, the
// branch clocks, the RCGs, GPLL0's vote word, the two BCRs), the TCSR reference clock, the DWC3 core (device controller and xHCI run
// bits), QSCRATCH, the M31 PHY, the eUSB2 repeater and the three rails behind SPMI and RPMh. The model records every access the
// kernel must never make (the core, QSCRATCH or the M31 with the glue unclocked, the M31 with its vdd off, a block reset with the
// GDSC off or a controller running, a repeater write on an APID not EE 0's, the GCC vote word written without the lock).
//   inspect: the fastboot path's registers (logs/native-runs, 2026-11-11) are a running device controller; each leftover the normal
//            path may have is named; unknown reads are never a reason; the M31 is not read where it would not answer.
//   simulate + coldStart: every piano-usb-cold-sim mask (and all of them) is undone by the default cold start, with no forbidden access;
//            the end state is the fastboot path's (rails on, repeater ready in device mode, the M31 at kM31Init's end state, 200 MHz).
//   failures: a GDSC or a branch that never comes on stops the start at c1 within its bound (no core access after); a repeater APID
//            of another EE, failed votes, a held vote lock are soft (logged, the start goes on, nothing forbidden).
//   the arguments' parsing, the voted set point, the link watch's schedule, the reasons' text.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. usb_cold_host.cpp -o /tmp/usb_cold_host && /tmp/usb_cold_host [v]
#include "UsbCold.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianousb;
using namespace pianousb::cold;
using namespace pianohw;
static int failures;
static bool verbose;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// --------------------------------------------------------------------------------------------------------------- the model
struct Model {
  // GCC
  std::map<uint32_t, uint32_t> gccWord;
  bool gdscOn = true, gdscStuck = false;
  std::map<uint32_t, bool> branchStuck;
  bool otherGpllVote = true;     // GPLL0 runs for others (it feeds most of the SoC)
  bool voteLockHeld = false, voteLockBusy = false;
  unsigned voteWordWrites = 0;
  // TCSR, core, QSCRATCH, M31
  uint32_t clkref = 1;
  std::map<uint32_t, uint32_t> coreWord, qsWord;
  std::map<uint32_t, uint8_t> m31Byte;
  // SPMI peripherals: registers and the APID's owner; rails (RPMh)
  std::map<uint16_t, std::map<uint8_t, uint8_t>> pmic;
  std::map<uint16_t, int> owner;
  bool spmiOff = false;
  unsigned repeaterWrites = 0;
  struct RailModel { uint16_t ppid; bool on; uint16_t mv; } rail[kRails] = {{0x3c2, true, 880}, {0x14e, true, 1800}, {0x144, true, 3100}};
  bool voteFails = false;
  unsigned votes = 0;
  // Gpio61, Type-C
  uint32_t gpioCfg = 0x00000001, gpioIo = 0;
  TypeC typecValue;
  // Time, breadcrumbs, the log, what must never happen
  uint64_t us = 0;
  int crumb = 0;
  std::vector<std::string> log, violations;

  static constexpr uint32_t kCoreSnps = 0x33313130;

  Model() { fastbootPath(); }
  void violation(const std::string &what)
  {
    violations.push_back(what);
    if (verbose) std::printf("  VIOLATION %s\n", what.c_str());
  }
  // The registers PianoDwc3's probe read after ABL fastboot (20261111-201656): a device controller the firmware ran and halted.
  void fastbootPath()
  {
    gccWord.clear();
    gdscOn = true;
    gccWord[kGccUsb30Gdsc] = 0x7a22f800;   // bit 31 from gdscOn
    for (uint32_t b : kBranches) gccWord[b] = 0x08000001;
    gccWord[kGccUsb30MasterRcg] = 0;
    gccWord[kGccUsb30MasterRcg + 4] = 0x00100105;
    gccWord[kGccUsb30MockUtmiRcg] = 0;
    gccWord[kGccUsb30MockUtmiRcg + 4] = 0x00100001;
    gccWord[kGccGpllVote] = 1;
    gccWord[kGccUsb30Bcr] = 0;
    gccWord[kGccQusb2PhyBcr] = 0;
    clkref = 1;
    coreReset();
    coreWord[dwc3::GCTL] = 0x00102005;
    coreWord[dwc3::GEVNTADRLO] = 0xd359f000;
    coreWord[dwc3::DEVTEN] = 0x1277;
    coreWord[dwc3::DCTL] = 0x10f00000;
    coreWord[dwc3::DCFG] = 0x0048080c;
    qsWord.clear();
    m31Byte = {{kM31UtmiCtrl0, 0x01}, {kM31Por, 0x00}, {kM31Common0, 0x6b}, {kM31HsCtrl2, 0x07}, {kM31Override, 0x00}};
    pmic.clear();
    owner.clear();
    for (unsigned i = 0; i < kRails; ++i) {
      rail[i].on = true;
      rail[i].mv = kRailTable[i].minMv;
      owner[rail[i].ppid] = 0;
      pmic[rail[i].ppid][kRegType] = kTypeLdo;
      syncRail(i);
    }
    owner[0x3c2 ^ 0xc2 ^ 0x41] = 0;   // l2d's other page holds a peripheral that is not an LDO (the page search skips it)
    pmic[0x341][kRegType] = 0x10;
    owner[kRepeaterPpid] = 0;
    auto &r = pmic[kRepeaterPpid];
    r[kRepeaterEnable] = 0x80;
    r[kRptForceEn5] = 0;
    r[kRptForceVal5] = 0;
    r[0x51] = 0x0a;
    r[0x54] = 0x03;
    r[0x57] = 0x03;
    syncRepeater();
  }
  void coreReset()   // the core's registers after a block reset or a collapse
  {
    coreWord.clear();
    coreWord[dwc3::GSNPSID] = kCoreSnps;
    coreWord[dwc3::GCTL] = 0x00003000;
    coreWord[0] = 0x01000020;   // xHCI CAPLENGTH 0x20
  }
  void syncRail(unsigned i)
  {
    auto &p = pmic[rail[i].ppid];
    p[kRegStatus] = rail[i].on ? 0x80 : 0;
    p[kRegEnCtl] = rail[i].on ? 0x80 : 0;
    p[kRegMode] = 0x07;
    p[kRegVsetLb] = uint8_t(rail[i].mv);
    p[kRegVsetUb] = uint8_t(rail[i].mv >> 8);
  }
  void syncRepeater()
  {
    auto &r = pmic[kRepeaterPpid];
    r[kRepeaterStatus] = (r[kRepeaterEnable] & 0x80) && rail[kRailL15b].on && rail[kRailL5b].on ? 0x80 : 0;
  }
  bool branchOn(uint32_t b) const
  {
    auto it = gccWord.find(b);
    auto stuck = branchStuck.find(b);
    return gdscOn && it != gccWord.end() && (it->second & kCbcrClkEnable) && !(stuck != branchStuck.end() && stuck->second);
  }
  bool clocked() const
  {
    if (!gdscOn) return false;
    for (uint32_t b : kBranches)
      if (!branchOn(b)) return false;
    return true;
  }
  bool coreRunning() const
  {
    auto d = coreWord.find(dwc3::DCTL), cmd = coreWord.find(0x20);
    return (d != coreWord.end() && (d->second & dwc3::DCTL_RUN_STOP)) || (cmd != coreWord.end() && (cmd->second & 1u));
  }

  // ---- the Hal
  uint32_t gcc(uint32_t off)
  {
    uint32_t v = gccWord[off];
    if (off == kGccUsb30Gdsc) return (v & ~kGdscPwrOn) | (gdscOn ? kGdscPwrOn : 0);
    for (uint32_t b : kBranches)
      if (off == b) return (v & ~kCbcrClkOff) | (branchOn(b) ? 0 : kCbcrClkOff);
    if (off == kGccUsb30MasterRcg || off == kGccUsb30MockUtmiRcg) {
      const bool root = (v & kRcgRootEn) || branchOn(off == kGccUsb30MasterRcg ? kGccUsb30Master : kGccUsb30MockUtmi);
      return (v & ~(kRcgRootOff | kRcgUpdate)) | (root ? 0 : kRcgRootOff);
    }
    if (off == kGccGpll0Mode) return (otherGpllVote || (gccWord[kGccGpllVote] & 1u)) ? kPllLockDet : 0;
    return v;
  }
  void gccWrite(uint32_t off, uint32_t v)
  {
    const uint32_t old = gccWord[off];
    if (off == kGccGpllVote) {
      ++voteWordWrites;
      if (!voteLockHeld && !voteLockBusy) violation("the GCC vote word written without the vote lock");
      if (old & ~v) violation("a GCC vote bit cleared");
    }
    if (off == kGccUsb30Gdsc) {
      if ((v & kGdscSwCollapse) && gdscOn) {
        gdscOn = false;
        coreReset();   // the collapse drops the core's state (and QSCRATCH's)
        qsWord.clear();
      } else if (!(v & kGdscSwCollapse)) {
        gdscOn = !gdscStuck;
      }
    }
    if (off == kGccUsb30Bcr && (v & 1u) && !(old & 1u)) {
      if (!gdscOn) violation("the controller's block reset with the GDSC off");
      if (coreRunning()) violation("the controller's block reset while it runs");
    }
    if (off == kGccUsb30Bcr && !(v & 1u) && (old & 1u)) {
      coreReset();
      qsWord.clear();
    }
    if (off == kGccQusb2PhyBcr && !(v & 1u) && (old & 1u))
      m31Byte = {{kM31UtmiCtrl0, 0x00}, {kM31Por, 0x02}, {kM31Common0, 0x04}, {kM31HsCtrl2, 0x00}, {kM31Override, 0x00}};
    if ((off == kGccUsb30MasterRcg || off == kGccUsb30MockUtmiRcg) && (v & kRcgUpdate) && !(gcc(off) & kRcgRootEn) &&
        !branchOn(off == kGccUsb30MasterRcg ? kGccUsb30Master : kGccUsb30MockUtmi))
      violation("an RCG updated with its root off");
    gccWord[off] = off == kGccUsb30MasterRcg || off == kGccUsb30MockUtmiRcg ? v & ~kRcgUpdate : v;   // UPDATE completes at once
  }
  uint32_t tcsr() { return clkref; }
  void tcsrWrite(uint32_t v) { clkref = v; }
  uint32_t core(uint32_t off)
  {
    if (!clocked()) { violation("a core read with the glue unclocked"); return 0xffffffffu; }
    if (off == dwc3::DSTS) return (coreWord[dwc3::DCTL] & dwc3::DCTL_RUN_STOP) ? 0x00020000 : 0x00d34554;
    if (off == 0x24) return (coreWord[0x20] & 1u) ? 0 : 1;   // USBSTS.HCH
    return coreWord[off];
  }
  void coreWrite(uint32_t off, uint32_t v)
  {
    if (!clocked()) { violation("a core write with the glue unclocked"); return; }
    coreWord[off] = v;
  }
  uint32_t m31(uint32_t off)
  {
    m31Check("read");
    const uint32_t b = m31Byte[off];
    return b | b << 8 | b << 16 | b << 24;
  }
  void m31Write(uint32_t off, uint32_t v)
  {
    m31Check("write");
    m31Byte[off] = uint8_t(v);
  }
  void m31Check(const char *what)
  {
    if (!clocked()) violation(std::string("an M31 ") + what + " with the glue unclocked");
    if (!rail[kRailL2d].on) violation(std::string("an M31 ") + what + " with its vdd off");
  }
  uint32_t qs(uint32_t off)
  {
    if (!clocked()) violation("a QSCRATCH read with the glue unclocked");
    return qsWord[off];
  }
  bool qsWrite(uint32_t off, uint32_t v)
  {
    if (!clocked()) { violation("a QSCRATCH write with the glue unclocked"); return false; }
    qsWord[off] = v;
    return true;
  }
  int spmiRead(uint16_t ppid, uint8_t reg, uint8_t *v)
  {
    if (spmiOff || !owner.count(ppid)) return -19;
    *v = pmic[ppid][reg];
    return 0;
  }
  int spmiWrite(uint16_t ppid, uint8_t reg, uint8_t v)
  {
    if (spmiOff || !owner.count(ppid)) return -19;
    if (owner[ppid] != 0) { violation("an SPMI write to an APID of another EE"); return -1; }
    if (ppid == kRepeaterPpid) ++repeaterWrites;
    pmic[ppid][reg] = v;
    if (ppid == kRepeaterPpid) syncRepeater();
    return 0;
  }
  int spmiOwner(uint16_t ppid)
  {
    if (spmiOff) return -2;
    return owner.count(ppid) ? owner[ppid] : -1;
  }
  int railVote(unsigned i, uint16_t mv, bool enable)
  {
    if (i >= kRails) return -22;
    if (voteFails) return -60;
    ++votes;
    if (enable && (mv < kRailTable[i].minMv || mv > kRailTable[i].maxMv)) violation("a rail voted outside its DT range");
    rail[i].on = enable;
    if (enable) rail[i].mv = mv;
    syncRail(i);
    syncRepeater();
    return 0;
  }
  bool voteLock(bool take)
  {
    if (!take) { voteLockHeld = false; return true; }
    if (voteLockBusy) { us += 50000; return false; }
    voteLockHeld = true;
    return true;
  }
  bool gpio61(uint32_t *cfg, uint32_t *io)
  {
    *cfg = gpioCfg;
    *io = gpioIo;
    return true;
  }
  TypeC typec() { return typecValue; }
  void udelay(unsigned n) { us += n; }
  void cold(int step) { crumb = step; }
  __attribute__((format(printf, 2, 3))) void note(const char *fmt, ...)
  {
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log.push_back(line);
    if (verbose) std::printf("  PianoDwc3: %s\n", line);
  }
  bool logged(const char *needle) const
  {
    for (const std::string &l : log)
      if (l.find(needle) != std::string::npos) return true;
    return false;
  }
};

// The end state the takeover needs: the fastboot path's, as far as the cold start owns it.
static void checkReady(Model &m, const char *what)
{
  const bool ok = m.clocked() && (m.gccWord[kGccUsb30MasterRcg + 4] & kRcgCfgMask) == kRcgMasterCfg &&
                  (m.gccWord[kGccUsb30MockUtmiRcg + 4] & kRcgCfgMask) == kRcgMockUtmiCfg && (m.clkref & 1u) && m.rail[0].on && m.rail[1].on &&
                  m.rail[2].on && (m.pmic[kRepeaterPpid][kRepeaterStatus] & 0x80) && (m.pmic[kRepeaterPpid][kRepeaterEnable] & 0x80) &&
                  !(m.pmic[kRepeaterPpid][kRptForceEn5] & kRptClk19p2) && m.m31Byte[kM31UtmiCtrl0] == 0x01 && m.m31Byte[kM31Por] == 0 &&
                  m.m31Byte[kM31Common0] == 0x6b && m.m31Byte[kM31HsCtrl2] == 0x07 && m.m31Byte[kM31Override] == 0 && !m.coreRunning();
  if (!ok) std::printf("FAIL: %s: the cold start's end state is not the fastboot path's\n", what);
  if (!ok) ++failures;
  if (!m.violations.empty()) {
    std::printf("FAIL: %s: %zu forbidden accesses, the first: %s\n", what, m.violations.size(), m.violations[0].c_str());
    ++failures;
  }
}

// ----------------------------------------------------------------------------------------------------------------- the tests
static void testFastbootPath()
{
  Model m;
  const Inherited s = inspect(m, true);
  CHECK(s.clocked && s.coreRead && s.phyRead && s.rptRead && m31Ready(s.phy));
  CHECK(reasons(s) == 0);
  CHECK(report(m, s, Mode::automatic, true) == 0);
  CHECK(m.logged("taken as it is (the takeover of before)"));
  CHECK(m.logged("orientation normal (CC1)"));
  CHECK(s.rails[kRailL2d].found && s.rails[kRailL2d].ppid == 0x3c2 && s.rails[kRailL5b].ppid == 0x144 && s.rails[kRailL15b].ppid == 0x14e);
  CHECK(m.violations.empty());
  // piano-usb-cold=1 runs it anyway; piano-usb-cold=0 says what it would have found
  Model a;
  report(a, inspect(a, true), Mode::always, true);
  CHECK(a.logged("the cold start runs"));
}

static void testReasons()
{
  struct Case { const char *what; void (*spoil)(Model &); uint32_t want; };
  const Case cases[] = {
    {"GDSC collapsed", [](Model &m) { m.gdscOn = false; }, kWhyGdsc},
    {"master branch off", [](Model &m) { m.gccWord[kGccUsb30Master] &= ~kCbcrClkEnable; }, kWhyClocks},
    {"master RCG on TCXO", [](Model &m) { m.gccWord[kGccUsb30MasterRcg + 4] = 0x00100001; }, kWhyRcg},
    {"usb2 clkref off", [](Model &m) { m.clkref = 0; }, kWhyClkRef},
    {"host role", [](Model &m) { m.coreWord[dwc3::GCTL] = 0x00101005; }, kWhyRole},
    {"never ran", [](Model &m) { m.coreWord[dwc3::GEVNTADRLO] = 0; m.coreWord[dwc3::DEVTEN] = 0; }, kWhyNeverRan},
    {"M31 SIDDQ", [](Model &m) { m.m31Byte[kM31Common0] = 0x6f; }, kWhyPhy},
    {"M31 in POR", [](Model &m) { m.m31Byte[kM31Por] = 0x02; }, kWhyPhy},
    {"repeater disabled", [](Model &m) { m.pmic[kRepeaterPpid][kRepeaterEnable] = 0; m.syncRepeater(); }, kWhyRepeater},
    {"repeater host clock", [](Model &m) { m.pmic[kRepeaterPpid][kRptForceEn5] = 0x40; }, kWhyRepeaterHost},
    {"l5b off", [](Model &m) { m.rail[kRailL5b].on = false; m.syncRail(kRailL5b); m.syncRepeater(); }, kWhyRails | kWhyRepeater},
  };
  for (const Case &c : cases) {
    Model m;
    c.spoil(m);
    const uint32_t r = reasons(inspect(m, true));
    if (r != c.want) {
      std::printf("FAIL reasons: %s: %#x, want %#x\n", c.what, r, c.want);
      ++failures;
    }
    if (!m.violations.empty()) {
      std::printf("FAIL reasons: %s: forbidden access %s\n", c.what, m.violations[0].c_str());
      ++failures;
    }
  }
  {   // unknowns are never a reason: no SPMI at all, the repeater missing from the map
    Model m;
    m.spmiOff = true;
    CHECK(reasons(inspect(m, false)) == 0);
    CHECK(reasons(inspect(m, true)) == 0);
    Model n;
    n.owner.erase(kRepeaterPpid);
    const Inherited s = inspect(n, true);
    CHECK(!s.rptRead && reasons(s) == 0);
  }
  {   // the M31 is not read with its vdd off or its reference clock off (no access that would not answer)
    Model m;
    m.rail[kRailL2d].on = false;
    m.syncRail(kRailL2d);
    const Inherited s = inspect(m, true);
    CHECK(!s.phyRead && (reasons(s) & kWhyRails) && m.violations.empty());
    Model c;
    c.clkref = 0;
    CHECK(!inspect(c, true).phyRead);
    Model w;   // after the rails test hook: the vdd unreadable counts as off
    w.owner.erase(0x3c2);
    w.rail[kRailL2d].on = false;
    CHECK(!inspect(w, true, true).phyRead && w.violations.empty());
  }
}

static void testColdFromFastboot()
{
  Model m;
  const Result r = coldStart(m, kStepsDefault);
  CHECK(r.ok() && r.soft == 0 && r.votes == 3 && r.rptInit && !r.tuned && r.phyInit && !r.reset && m.crumb == 6);
  CHECK(m.pmic[kRepeaterPpid][0x51] == 0x0a);   // the firmware's tuning is kept for a repeater that was on
  checkReady(m, "from the fastboot path");
  CHECK(m.voteWordWrites == 0);                  // GPLL0 was voted already
  Model t;
  const Result rt = coldStart(t, kStepsDefault | kStepTuneAlways);
  CHECK(rt.ok() && rt.tuned && t.pmic[kRepeaterPpid][0x51] == 0x0e && t.pmic[kRepeaterPpid][0x57] == 0x06);
  checkReady(t, "with the tuning always");
}

static void testSimThenCold()
{
  const uint32_t masks[] = {kSimRepeater, kSimPhy, kSimHost, kSimClocks, kSimRails, kSimRepeater | kSimPhy, kSimPhy | kSimClocks, kSimAll};
  for (uint32_t mask : masks) {
    Model m;
    m.otherGpllVote = true;
    simulate(m, mask);
    char what[64];
    std::snprintf(what, sizeof what, "sim %#x", mask);
    if (!m.violations.empty()) {
      std::printf("FAIL %s: the test hook made a forbidden access: %s\n", what, m.violations[0].c_str());
      ++failures;
      continue;
    }
    const Inherited s = inspect(m, true, (mask & kSimRails) != 0);
    const uint32_t r = reasons(s);
    uint32_t want = 0;
    if (mask & kSimRepeater) want |= kWhyRepeater | kWhyRepeaterHost;
    if (mask & kSimPhy) want |= kWhyClkRef;
    if ((mask & kSimHost) && !(mask & kSimClocks)) want |= kWhyRole;
    if (mask & kSimClocks) want |= kWhyGdsc | kWhyRcg;
    if (mask & kSimRails) want |= kWhyRails | kWhyRepeater;
    if ((r & want) != want || !r) {
      std::printf("FAIL %s: reasons %#x, want at least %#x\n", what, r, want);
      ++failures;
    }
    report(m, s, Mode::automatic, true);
    CHECK(m.logged("the cold start runs"));
    const Result c = coldStart(m, kStepsDefault);
    if (!c.ok()) {
      std::printf("FAIL %s: the cold start failed at c%d (%s)\n", what, c.failed, c.why ? c.why : "?");
      ++failures;
    }
    checkReady(m, what);
    if (mask & kSimRepeater) CHECK(c.tuned && m.pmic[kRepeaterPpid][0x53] == 0x07 && m.pmic[kRepeaterPpid][0x52] == 0x01);
    // after the cold start only the role and "the firmware never ran it" may stay (the takeover's own init sets the device role and
    // the event buffer): never a power, clock, rail, repeater or PHY reason
    const uint32_t after = reasons(inspect(m, true));
    if (after & ~(kWhyNeverRan | kWhyRole)) {
      std::printf("FAIL %s: reasons after the cold start %#x\n", what, after);
      ++failures;
    }
    CHECK(m.us < 2000000);
  }
}

static void testHardFailures()
{
  {   // a GDSC that never powers on: c1 fails within its bound and nothing of the core is touched
    Model m;
    m.gdscOn = false;
    m.gdscStuck = true;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(!r.ok() && r.failed == 1 && m.crumb == -1 && m.violations.empty() && m.us <= 20000 && m.logged("cold c1 FAILED"));
  }
  {   // a branch that never comes on
    Model m;
    m.branchStuck[kGccAggreUsb3Axi] = true;
    m.gccWord[kGccAggreUsb3Axi] &= ~kCbcrClkEnable;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(!r.ok() && r.failed == 1 && m.violations.empty());
  }
  {   // a core that does not answer as a DWC3 (c2)
    Model m;
    m.coreWord[dwc3::GSNPSID] = 0;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(!r.ok() && r.failed == 2 && m.crumb == -2);
  }
}

static void testSoftFailures()
{
  {   // the repeater's APID belongs to another EE: nothing written, a soft failure, the rest goes on
    Model m;
    m.owner[kRepeaterPpid] = 2;
    simulate(m, kSimPhy);
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && (r.soft & 1u << 4) && m.repeaterWrites == 0 && m.violations.empty() && r.phyInit);
  }
  {   // every vote fails and the M31's vdd reads off: the M31 is not touched
    Model m;
    simulate(m, kSimRails);
    m.voteFails = true;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && (r.soft & 1u << 3) && (r.soft & 1u << 5) && !r.phyInit && m.violations.empty());
  }
  {   // the vote lock held elsewhere for good: GPLL0's vote bit is set anyway (only ever set), and that is logged
    Model m;
    m.gccWord[kGccGpllVote] = 0;
    m.otherGpllVote = false;
    m.voteLockBusy = true;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && (m.gccWord[kGccGpllVote] & 1u) && m.voteWordWrites == 1 && m.violations.empty() && m.logged("the lock was held"));
  }
  {   // the vote lock free: taken around the write and given back
    Model m;
    m.gccWord[kGccGpllVote] = 0x10;
    m.otherGpllVote = false;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && m.gccWord[kGccGpllVote] == 0x11 && !m.voteLockHeld && m.violations.empty());
  }
  {   // piano-usb-spmi=0: no rail readback, no repeater; votes still go out
    Model m;
    m.spmiOff = true;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && r.votes == 3 && (r.soft & 1u << 4) && r.phyInit && m.violations.empty());
  }
}

static void testStopAndReset()
{
  {   // the firmware left the xHCI running: halted before the block reset
    Model m;
    m.coreWord[dwc3::GCTL] = 0x00101005;
    m.coreWord[0x20] = 1;
    const Result r = coldStart(m, kStepsDefault | kStepBcr);
    CHECK(r.ok() && r.reset && !(m.coreWord[0x20] & 1u) && m.violations.empty() && m.logged("the xHCI the firmware ran is halted"));
  }
  {   // a device controller left running: halted
    Model m;
    m.coreWord[dwc3::DCTL] |= dwc3::DCTL_RUN_STOP;
    const Result r = coldStart(m, kStepsDefault | kStepBcr);
    CHECK(r.ok() && !m.coreRunning() && m.violations.empty() && m.logged("the device controller the firmware ran is halted"));
  }
  {   // no block reset unless asked for
    Model m;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && !r.reset && m.coreWord[dwc3::GEVNTADRLO] == 0xd359f000);
  }
  {   // the steps mask: nothing optional
    Model m;
    simulate(m, kSimRepeater);
    const Result r = coldStart(m, 0);
    CHECK(r.ok() && r.votes == 0 && !r.rptInit && !r.phyInit && m.logged("left as it is (piano-usb-cold-steps)"));
  }
}

static void testArguments()
{
  bool known = false;
  CHECK(parseMode(nullptr, false, &known) == Mode::automatic && known);
  CHECK(parseMode("0", true, &known) == Mode::inherited && known);
  CHECK(parseMode("1", true, &known) == Mode::always && known);
  CHECK(parseMode("auto", true, &known) == Mode::automatic && known);
  CHECK(parseMode("2", true, &known) == Mode::automatic && !known);
  CHECK(parseMode("", true, &known) == Mode::automatic && !known);
  CHECK(parseMode("autox", true, &known) == Mode::automatic && !known);
  CHECK(parseMode("10", true, &known) == Mode::automatic && !known);
  CHECK(std::strcmp(modeName(Mode::inherited), "0") == 0 && std::strcmp(modeName(Mode::automatic), "auto") == 0);
  CHECK(kStepsDefault == 0x17 && !(kStepsDefault & kStepBcr) && !(kStepsDefault & kStepTuneAlways));
  // the set point voted
  RailRead s;
  CHECK(voteMv(kRailTable[kRailL2d], s) == 880);
  s.read = true;
  s.mv = 904;
  CHECK(voteMv(kRailTable[kRailL2d], s) == 904);
  s.mv = 960;
  CHECK(voteMv(kRailTable[kRailL2d], s) == 880);
  s.mv = 3000;
  CHECK(voteMv(kRailTable[kRailL5b], s) == 3100);
  // the board tuning is piano's DT, code 3 for -3000 bp
  CHECK(kRptTuneCount == 5 && kRptTune[0].reg == 0x51 && kRptTune[0].value == 0x0e && kRptTune[3].reg == 0x54 && kRptTune[3].value == 3);
}

static void testWatch()
{
  Watch w;
  w.arm(15000, 1000);
  CHECK(w.armed && w.step(15999, false) == WatchDo::nothing);
  CHECK(w.step(16000, false) == WatchDo::coldStart && w.waitMs == 30000);
  w.colds = 1;
  w.restarted(17000);
  CHECK(w.step(46999, false) == WatchDo::nothing && w.step(47000, false) == WatchDo::coldStart && w.waitMs == 60000);
  w.colds = 2;
  CHECK(w.step(107000, false) == WatchDo::coldStart);
  w.colds = 3;
  CHECK(w.step(10000000, false) == WatchDo::giveUp && !w.armed && w.step(20000000, false) == WatchDo::nothing);
  Watch h;   // a host seen ends it for good, even if later the link goes quiet
  h.arm(15000, 0);
  CHECK(h.step(100, true) == WatchDo::nothing && !h.armed && h.hostSeen);
  h.arm(15000, 200);
  CHECK(!h.armed && h.step(1000000, false) == WatchDo::nothing);
  Watch off;   // piano-usb-cold-wait=0
  off.arm(0, 0);
  CHECK(!off.armed && off.step(1000000, false) == WatchDo::nothing);
  Watch cap;
  cap.arm(200000, 0);
  CHECK(cap.step(200000, false) == WatchDo::coldStart && cap.waitMs == kWatchMaxMs);
}

static void testWhyText()
{
  char buf[480];
  whyText(0, buf, sizeof buf);
  CHECK(std::strcmp(buf, "none") == 0);
  whyText(kWhyRepeater | kWhyRails, buf, sizeof buf);
  CHECK(std::strcmp(buf, "the repeater is not enabled/ready; a rail is off") == 0);
  char small[8];
  whyText(0x7ff, small, sizeof small);
  CHECK(std::strlen(small) == 7);
  whyText(0x7ff, buf, sizeof buf);
  CHECK(std::strlen(buf) < sizeof buf - 1);   // all eleven fit in the kernel's buffer
}

int main(int argc, char **argv)
{
  verbose = argc > 1 && argv[1][0] == 'v';
  testFastbootPath();
  testReasons();
  testColdFromFastboot();
  testSimThenCold();
  testHardFailures();
  testSoftFailures();
  testStopAndReset();
  testArguments();
  testWatch();
  testWhyText();
  if (failures) {
    std::printf("usb_cold_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("usb_cold_host: inspect (the fastboot path, 11 leftovers, unknowns), 8 test-hook masks undone by the cold start, hard and "
              "soft failures, stop and block reset, arguments, the link watch: all passed\n");
  return 0;
}
