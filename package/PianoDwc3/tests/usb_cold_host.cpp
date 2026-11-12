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
//   the role (C6, docs/forensics-2026-11-11.md section 7.6): the model knows piano's reset values (C7), what a collapse with
//            retention keeps (C6), the host role's FIFO sizes, GSTS.CURMOD, the xHCI's run/halt; it flags a device register touched
//            outside the device role, an xHCI register outside the host role, a role change under a running xHCI, a block reset of a
//            running controller, GCTL.CORESOFTRESET with the PHY suspended, and RUN_STOP with the host role's FIFO sizes. sim 4, sim
//            31 and host boot paths (xHCI halted, running, never halting, not answering), against each unknown FIFO behaviour, end
//            in the device role; then the gadget (Dwc3Gadget.h) replays the takeover to RUN_STOP on the same model. C6's own order
//            is flagged (the check would have caught it).
//   the arguments' parsing, the voted set point, the link watch's schedule, the reasons' text.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. usb_cold_host.cpp -o /tmp/usb_cold_host && /tmp/usb_cold_host [v]
#include "UsbCold.h"
#include "Dwc3Gadget.h"
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
  // The core's role behaviour. What the hardware has not shown yet is a switch, and every combination must end in the device role
  // with a device FIFO layout: whether a PRTCAPDIR write loads the new role's FIFO sizes, whether DCTL.CSFTRST (in the device role)
  // loads the device ones. Shown on piano: a reset (a collapse with retention, the block reset) loads the sizes of the role GCTL
  // holds (C6, C7); DCTL.CSFTRST in the host role left the host sizes (C6).
  bool roleWriteLoadsFifos = false, csftrstLoadsFifos = false;
  bool xhciStuck = false;          // an xHCI that never reports HCHalted: still executing (its DMA may go on), whatever RS reads
  uint32_t gversion = 0x3230302a;  // piano: DWC_usb31 2.00a
  unsigned runStops = 0, csftrsts = 0, gctlResets = 0, blockResets = 0, hostRoleTouches = 0;

  static constexpr uint32_t kCoreSnps = 0x33313130;
  // piano's TX/RX FIFO sizes: the device role's (C7, after the block reset; the fastboot path reads the same) and the host role's
  // (C6, after a collapse in the host role and a DCTL.CSFTRST there)
  static constexpr uint32_t kDevTx[8] = {0x00000043, 0x0043030d, 0x03500083, 0x03d3030d, 0x06e00083, 0x07630105, 0x08680083, 0x08eb0083};
  static constexpr uint32_t kHostTx[8] = {0x00000025, 0x00258082, 0x00a700c4, 0x016b8103, 0x026e028b, 0x04f9828b, 0x07840000, 0x07848000};
  static constexpr uint32_t kDevRx = 0x00000313, kHostRx = 0x00000028;
  static constexpr uint32_t kCap = 0x20;   // the xHCI's CAPLENGTH: USBCMD at 0x20, USBSTS at 0x24

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
    coreWord[dwc3::GEVNTSIZ] = 0x80001000;
    coreWord[dwc3::DEVTEN] = 0x1277;
    coreWord[dwc3::DALEPENA] = 0xf;
    coreWord[dwc3::DCTL] = 0x10f00000;
    coreWord[dwc3::DCFG] = 0x0048080c;
    coreWord[dwc3::GUCTL1] = 0x81601808;
    coreWord[dwc3::GUSB3PIPECTL] = 0x0b081402;
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
  void coreReset()   // the core's registers after the block reset (C7's probe after USB30_PRIM_BCR) or a collapse without retention
  {
    using namespace dwc3;
    coreWord.clear();
    coreWord[GSNPSID] = kCoreSnps;
    coreWord[GVERSION] = gversion;
    coreWord[GCTL] = 0x00102004;   // the device role
    coreWord[GSTS] = 0x7e800000;
    coreWord[GUCTL1] = 0x00001808;
    coreWord[GSBUSCFG0] = 0x2222000e;
    coreWord[GSBUSCFG1] = 0x00007f00;
    coreWord[GUSB2PHYCFG] = 0x00102500;
    coreWord[GUSB3PIPECTL] = 0x030c1002;
    coreWord[GHWPARAMS0] = 0x4020400a;
    coreWord[GHWPARAMS1] = 0x01092486;
    coreWord[GHWPARAMS3] = 0x90420086;
    coreWord[GHWPARAMS6] = 0x1835003f;
    coreWord[GHWPARAMS7] = 0x08a00d86;
    coreWord[DCFG] = 0x00080805;
    coreWord[DCTL] = 0x00f00000;
    coreWord[0] = 0x01200020;   // xHCI 1.20, CAPLENGTH 0x20 (a hardware constant)
    loadFifos(ROLE_DEVICE);
  }
  void loadFifos(uint32_t role)
  {
    for (unsigned n = 0; n < 8; ++n) coreWord[dwc3::GTXFIFOSIZ0 + 4 * n] = role == dwc3::ROLE_HOST ? kHostTx[n] : kDevTx[n];
    coreWord[dwc3::GRXFIFOSIZ0] = role == dwc3::ROLE_HOST ? kHostRx : kDevRx;
  }
  // A collapse with RETAIN_FF (C6, sim 31): the retention flops keep GCTL (the role), GUCTL1, the PHY words, DCFG, DCTL, DEVTEN,
  // DALEPENA, GEVNTSIZ; the event buffer address and the xHCI's operational words are lost; the FIFO sizes are the kept role's.
  void collapse(bool retain)
  {
    using namespace dwc3;
    if (!retain) {
      coreReset();
      return;
    }
    std::map<uint32_t, uint32_t> kept;
    for (uint32_t off : {GCTL, GUCTL1, GUSB2PHYCFG, GUSB3PIPECTL, GSBUSCFG0, GSBUSCFG1, DCFG, DCTL, DEVTEN, DALEPENA, GEVNTSIZ})
      kept[off] = coreWord[off];
    coreReset();
    for (auto &k : kept) coreWord[k.first] = k.second;
    coreWord[DCTL] &= ~DCTL_RUN_STOP;
    loadFifos(roleOf(coreWord[GCTL]));
  }
  uint32_t role() { return dwc3::roleOf(coreWord[dwc3::GCTL]); }
  bool xhciRunning() const
  {
    auto c = coreWord.find(kCap);
    return xhciStuck || (c != coreWord.end() && (c->second & 1u));
  }
  // The FIFO sizes now are a device layout (UsbCold.h's own check, piano's RAM).
  bool deviceLayout()
  {
    uint32_t tx[kDeviceFifos];
    for (unsigned n = 0; n < kDeviceFifos; ++n) tx[n] = coreWord[dwc3::GTXFIFOSIZ0 + 4 * n];
    return deviceFifos(tx, coreWord[dwc3::GRXFIFOSIZ0], 3462, true) == kFifosOk;
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
    auto d = coreWord.find(dwc3::DCTL);
    return (d != coreWord.end() && (d->second & dwc3::DCTL_RUN_STOP)) || xhciRunning();
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
        collapse((v & kGdscRetainFf) != 0);   // the core keeps its retention flops at most (QSCRATCH nothing)
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
      xhciStuck = false;   // the block reset stops it for good
      ++blockResets;
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
  // The role rule: a device register only in the device role, an xHCI register only in the host role.
  void roleCheck(uint32_t off, const char *what)
  {
    using namespace dwc3;
    char line[160];
    if (isDeviceReg(off) && role() != ROLE_DEVICE) {
      if (role() == ROLE_HOST) ++hostRoleTouches;
      std::snprintf(line, sizeof line, "a device register %s (%#x) in the %s role", what, off, roleName(role()));
      violation(line);
    }
    if (isXhciReg(off) && role() != ROLE_HOST) {
      std::snprintf(line, sizeof line, "an xHCI register %s (%#x) in the %s role", what, off, roleName(role()));
      violation(line);
    }
  }
  uint32_t core(uint32_t off)
  {
    using namespace dwc3;
    if (!clocked()) { violation("a core read with the glue unclocked"); return 0xffffffffu; }
    roleCheck(off, "read");
    if (off == DSTS) return (coreWord[DCTL] & DCTL_RUN_STOP) ? 0x00020000 : 0x00d34554;
    if (off == GSTS) return (coreWord[GSTS] & ~GSTS_CURMOD_MASK) | (role() == ROLE_HOST ? GSTS_CURMOD_HOST : GSTS_CURMOD_DEVICE);
    if (off == kCap + 4) return xhciRunning() ? 0 : 1;   // USBSTS.HCH
    return coreWord[off];
  }
  void coreWrite(uint32_t off, uint32_t v)
  {
    using namespace dwc3;
    if (!clocked()) { violation("a core write with the glue unclocked"); return; }
    roleCheck(off, "written");
    if (off == GCTL) {
      const uint32_t was = roleOf(coreWord[GCTL]), now = roleOf(v);
      if (now != was && xhciRunning()) violation("the role changed under a running xHCI");
      if ((v & GCTL_CORESOFTRESET) && !(coreWord[GCTL] & GCTL_CORESOFTRESET)) {
        ++gctlResets;
        if (coreWord[GUSB3PIPECTL] & GUSB3PIPECTL_SUSPHY) violation("GCTL.CORESOFTRESET with GUSB3PIPECTL.SUSPHY set");
      }
      coreWord[GCTL] = v;
      if (now != was && roleWriteLoadsFifos) loadFifos(now);
      return;
    }
    if (off == DCTL) {
      if (v & DCTL_CSFTRST) {   // self-clearing
        ++csftrsts;
        v &= ~DCTL_CSFTRST;
        if (csftrstLoadsFifos) loadFifos(ROLE_DEVICE);
      }
      if ((v & DCTL_RUN_STOP) && !(coreWord[DCTL] & DCTL_RUN_STOP)) {
        ++runStops;
        if (!deviceLayout()) violation("RUN_STOP with the FIFO sizes of the host role (C6)");
        if (coreWord[GCTL] & GCTL_CORESOFTRESET) violation("RUN_STOP with GCTL.CORESOFTRESET set");
      }
      coreWord[DCTL] = v;
      return;
    }
    if (off >= 0xc800 && off < 0xca00 && (off & 0xf) == 0xc) {   // DEPCMD: the command completes at once, status 0
      coreWord[off] = v & ~DEPCMD_CMDACT & ~0xf000u;
      return;
    }
    coreWord[off] = v;   // USBCMD: RS 0 halts the xHCI at once, unless xhciStuck (HCHalted never comes)
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
                  m.m31Byte[kM31Common0] == 0x6b && m.m31Byte[kM31HsCtrl2] == 0x07 && m.m31Byte[kM31Override] == 0 && !m.coreRunning() &&
                  m.role() == dwc3::ROLE_DEVICE && m.deviceLayout();
  if (!ok) std::printf("FAIL: %s: the cold start's end state is not the fastboot path's (role %u, device FIFO layout %u)\n", what, m.role(),
                       m.deviceLayout() ? 1 : 0);
  if (!ok) ++failures;
  if (!m.violations.empty()) {
    std::printf("FAIL: %s: %zu forbidden accesses, the first: %s\n", what, m.violations.size(), m.violations[0].c_str());
    ++failures;
  }
}

// The gadget core (Dwc3Gadget.h) on the same model: the takeover's own register work after the cold start.
struct TakeoverHal {
  Model &m;
  std::vector<uint8_t> mem = std::vector<uint8_t>(Layout::kBytes);
  explicit TakeoverHal(Model &model) : m(model) {}
  uint32_t rd(uint32_t off) { return m.core(off); }
  void wr(uint32_t off, uint32_t v) { m.coreWrite(off, v); }
  void barrier() {}
  void udelay(unsigned n) { m.us += n; }
  uint64_t nowUs() { return m.us; }
  uint8_t *dma() { return mem.data(); }
  uint64_t dmaPhys() { return 0x8de966000ull; }
  unsigned txFill(unsigned, uint8_t *, unsigned) { return 0; }
  unsigned rxSpace(unsigned) { return 1u << 20; }
  void rxData(unsigned, const uint8_t *, unsigned) {}
  void lineState(unsigned, uint16_t) {}
  unsigned netTxNext(uint8_t *, unsigned) { return 0; }
  unsigned netRxSpace() { return 0; }
  bool netRxFrame(const uint8_t *, unsigned) { return false; }
  void netRxDone() {}
  void netLink(bool, uint32_t) {}
  __attribute__((format(printf, 2, 3))) void note(const char *fmt, ...)
  {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    m.note("%s", line);
  }
};

// PianoDwc3.cpp's takeover from its gate to RUN_STOP (stages 12-19; the DMA allocation and the notes are the kernel's): the gate
// (ensureDevice), the halt, the UTMI clock as PIPE clock, the soft reset, the device init, the VBUS/session override, RUN_STOP.
// gate false: the takeover without its gate (the gadget's own refusals are then all that guards it).
static bool replayTakeover(Model &m, bool gate = true)
{
  if (gate) {
    RoleSwitch rs;
    if (!ensureDevice(m, rs)) return false;
  }
  TakeoverHal th(m);
  Gadget<TakeoverHal> g;
  g.bind(&th, 3, 0x0001);
  (void)g.halt(100);
  m.qsWrite(kQsGeneralCfg, m.qs(kQsGeneralCfg) | kQsPipeUtmiClkSel | kQsPipe3PhystatusSw);
  uint32_t waited = 0;
  if (!g.softReset(true, &waited) || !g.init()) return false;
  m.qsWrite(kQsSsPhyCtrl, m.qs(kQsSsPhyCtrl) | kQsLane0PwrPresent);
  m.qsWrite(kQsHsPhyCtrl, m.qs(kQsHsPhyCtrl) | kQsUtmiOtgVbusValid | kQsSwSessvldSel);
  return g.run(50);
}

// A boot path that ran the core as a host and halted its xHCI: the host role, the host role's FIFO sizes, no session override.
static void hostBootPath(Model &m)
{
  m.coreWord[dwc3::GCTL] = 0x00101005;
  m.loadFifos(dwc3::ROLE_HOST);
  m.coreWord[dwc3::GEVNTADRLO] = 0;
  m.coreWord[dwc3::DEVTEN] = 0;
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
    // after the cold start only "the firmware never ran it" may stay (the takeover's own init sets the event buffer): never the role
    // (c2 leaves the device role), a power, clock, rail, repeater or PHY reason
    const uint32_t after = reasons(inspect(m, true));
    if (after & ~kWhyNeverRan) {
      std::printf("FAIL %s: reasons after the cold start %#x\n", what, after);
      ++failures;
    }
    CHECK(c.reset == ((mask & kSimClocks) != 0));   // the block reset only after a collapse here (sim 4 alone switches softly)
    CHECK(m.hostRoleTouches == 0);
    if (!replayTakeover(m) || m.runStops != 1 || !m.violations.empty()) {
      std::printf("FAIL %s: the takeover after the cold start did not reach RUN_STOP cleanly (%u, %s)\n", what, m.runStops,
                  m.violations.empty() ? "no violation" : m.violations[0].c_str());
      ++failures;
    }
    CHECK(m.us < 2000000);
  }
}

// The FIFO layout check against piano's numbers: the device role's pass, the host role's fail.
static void testFifoLayout()
{
  uint32_t tx[kDeviceFifos];
  for (unsigned n = 0; n < kDeviceFifos; ++n) tx[n] = Model::kDevTx[n];
  CHECK(deviceFifos(tx, Model::kDevRx, 3462, true) == kFifosOk);
  for (unsigned n = 0; n < kDeviceFifos; ++n) tx[n] = Model::kHostTx[n];
  CHECK(deviceFifos(tx, Model::kHostRx, 3462, true) == 1);   // TXFRAMNUM on FIFO 1
  for (unsigned n = 0; n < kDeviceFifos; ++n) tx[n] = Model::kHostTx[n] & ~0x8000u;
  CHECK(deviceFifos(tx, Model::kHostRx, 3462, true) == 6);   // without it: FIFO 6 has no depth
  for (unsigned n = 0; n < kDeviceFifos; ++n) tx[n] = Model::kDevTx[n];
  CHECK(deviceFifos(tx, 0, 3462, true) == kFifoRx);
  CHECK(deviceFifos(tx, Model::kDevRx, 0x800, true) == 5);   // past the RAM's end
  tx[3] = 0x03d4030d;                                        // a gap
  CHECK(deviceFifos(tx, Model::kDevRx, 3462, true) == 3);
}

// The host role: from sim 4, from what a host-mode boot path leaves (with the xHCI halted or running), from sim 31; each against
// every way the FIFO sizes may load. The cold start ends in the device role with a device FIFO layout; no device register is touched
// in the host role (nor an xHCI register outside it); the soft switch is taken when shown, else the block reset; then the takeover's
// replay reaches RUN_STOP.
static void testHostRole()
{
  struct Start { const char *what; void (*make)(Model &); bool hostSizes, running, collapse; };
  const Start starts[] = {
    {"sim 4", [](Model &m) { simulate(m, kSimHost); }, false, false, false},
    {"a host boot path, the xHCI halted", hostBootPath, true, false, false},
    {"a host boot path, the xHCI running", [](Model &m) { hostBootPath(m); m.coreWord[Model::kCap] = 1; }, true, true, false},
    {"sim 31", [](Model &m) { simulate(m, kSimAll); }, true, false, true},
  };
  for (const Start &st : starts)
    for (unsigned flags = 0; flags < 4; ++flags) {
      Model m;
      m.roleWriteLoadsFifos = flags & 1;
      m.csftrstLoadsFifos = flags & 2;
      st.make(m);
      char what[160];
      std::snprintf(what, sizeof what, "%s (role write loads %u, CSFTRST loads %u)", st.what, flags & 1, flags >> 1);
      const Inherited s = inspect(m, true, st.collapse);
      CHECK(!s.devRead);
      if (!st.collapse) CHECK(s.xhciRead && (reasons(s) & kWhyRole) && (s.usbcmd & 1u) == (st.running ? 1u : 0u));
      report(m, s, Mode::automatic, true);
      const Result r = coldStart(m, kStepsDefault);
      const bool hostSizesStay = st.hostSizes && !(flags & 1) && !(flags & 2);
      const bool wantBcr = st.collapse || hostSizesStay;
      if (!r.ok() || r.reset != wantBcr || r.role.found != dwc3::ROLE_HOST || r.role.soft == st.collapse ||
          r.role.proven != !wantBcr || r.role.xhci != (st.running ? 2 : 1)) {
        std::printf("FAIL %s: ok %u (c%d %s) block reset %u (want %u) found %u soft %u proven %u xhci %d\n", what, r.ok() ? 1 : 0, r.failed,
                    r.why ? r.why : "", r.reset ? 1 : 0, wantBcr ? 1 : 0, r.role.found, r.role.soft ? 1 : 0, r.role.proven ? 1 : 0, r.role.xhci);
        ++failures;
      }
      if (hostSizesStay && !st.collapse)
        CHECK(r.role.afterSoft.read && r.role.afterSoft.fail == 1 && m.logged("NOT a device layout (FIFO 1 fails)"));
      if (st.collapse) CHECK(!r.role.afterSoft.read && r.role.afterBcr.read && r.role.afterBcr.fail == kFifosOk);
      checkReady(m, what);
      CHECK(m.hostRoleTouches == 0 && m.gctlResets == 0);   // DWC_usb31 2.00a: no GCTL.CORESOFTRESET
      if (!replayTakeover(m) || m.runStops != 1 || !m.violations.empty()) {
        std::printf("FAIL %s: the takeover's replay did not reach RUN_STOP cleanly (%s)\n", what,
                    m.violations.empty() ? "no violation" : m.violations[0].c_str());
        ++failures;
      }
    }
}

// The edges of the host role: an xHCI that never halts (c2 fails, nothing more written, and the takeover's gate refuses too), an xHCI
// whose registers do not answer, the OTG and no role, DWC_usb31 1.80a (GCTL.CORESOFTRESET, the PHYs awake first).
static void testHostRoleEdges()
{
  using namespace dwc3;
  {
    Model m;
    hostBootPath(m);
    m.coreWord[Model::kCap] = 1;
    m.xhciStuck = true;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(!r.ok() && r.failed == 2 && m.crumb == -2 && r.role.xhci == -2 && !r.reset && m.blockResets == 0);
    CHECK(m.role() == ROLE_HOST && m.violations.empty() && m.logged("the xHCI does not halt within 100 ms"));
    CHECK(!replayTakeover(m) && m.runStops == 0 && m.violations.empty());   // the gate stops it: no device register touched
    CHECK(m.us < 1000000);
  }
  {   // RS already 0 but HCHalted never comes (a stop not done): still executing, so the same as running: c2 fails, nothing reset
    Model m;
    hostBootPath(m);
    m.xhciStuck = true;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(!r.ok() && r.failed == 2 && r.role.xhci == -2 && !r.reset && m.blockResets == 0 && m.role() == ROLE_HOST && m.violations.empty());
  }
  {
    Model m;
    hostBootPath(m);
    m.coreWord[0] = 0xffffffffu;   // CAPLENGTH 0xff
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && r.role.xhci == -1 && r.reset && !r.role.soft && m.violations.empty());
    checkReady(m, "the xHCI's registers do not answer");
  }
  for (uint32_t gctl : {0x00103005u, 0x00100005u}) {   // OTG, none
    Model m;
    m.coreWord[GCTL] = gctl;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && r.reset && r.role.xhci == 0 && !r.role.soft && m.violations.empty());
    checkReady(m, gctl == 0x00103005u ? "the OTG role" : "no role");
  }
  {
    Model m;
    m.gversion = 0x3138302a;   // DWC_usb31 1.80a: the programming model's GCTL.CORESOFTRESET
    m.coreWord[GVERSION] = m.gversion;
    hostBootPath(m);
    m.coreWord[GUSB3PIPECTL] |= GUSB3PIPECTL_SUSPHY;
    m.csftrstLoadsFifos = true;
    const Result r = coldStart(m, kStepsDefault);
    CHECK(r.ok() && r.role.gctlReset && r.role.proven && !r.reset && m.gctlResets == 1 && m.violations.empty());
    checkReady(m, "DWC_usb31 1.80a");
  }
}

// The takeover's gate without a cold start (piano-usb-cold=0, a hand-off's reclaim): sim 4, then the takeover. With the gate the
// role is switched first and RUN_STOP comes with a device layout; without it the gadget refuses everything and touches nothing.
static void testTakeoverGate()
{
  {
    Model m;
    simulate(m, kSimHost);
    CHECK(replayTakeover(m) && m.runStops == 1 && m.violations.empty() && m.role() == dwc3::ROLE_DEVICE);
    CHECK(m.logged("takeover: role: found role 1 (host"));
  }
  {
    Model m;
    simulate(m, kSimHost);
    CHECK(!replayTakeover(m, false) && m.runStops == 0 && m.csftrsts == 0 && m.violations.empty() && m.role() == dwc3::ROLE_HOST);
    CHECK(m.logged("soft reset refused"));
  }
  {   // the device role: the gate does nothing at all
    Model m;
    RoleSwitch rs;
    CHECK(ensureDevice(m, rs) && rs.found == dwc3::ROLE_DEVICE && !rs.blockReset && !rs.soft && m.log.empty());
  }
}

// C6's own order on C6's state (the old c2 and takeover: the device soft reset in the host role, PRTCAPDIR written afterwards by
// the gadget's init, RUN_STOP): the model flags both, so the checks above would have caught it.
static void testC6OrderCaught()
{
  using namespace dwc3;
  Model m;
  simulate(m, kSimAll);
  Result res;
  CHECK(stepPower(m, kStepsDefault, res) && m.role() == ROLE_HOST && !m.deviceLayout());
  m.violations.clear();
  m.coreWrite(DCTL, (m.core(DCTL) & ~DCTL_RUN_STOP) | DCTL_CSFTRST);
  m.coreWrite(GCTL, (m.core(GCTL) & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_DEVICE | GCTL_DSBLCLKGTNG);
  m.coreWrite(DCTL, m.core(DCTL) | DCTL_RUN_STOP);
  CHECK(m.hostRoleTouches >= 2 && m.violations.size() >= 3);
  bool runStopFlagged = false;
  for (const std::string &v : m.violations) runStopFlagged |= v.find("RUN_STOP with the FIFO sizes of the host role") != std::string::npos;
  CHECK(runStopFlagged);
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
  testFifoLayout();
  testHostRole();
  testHostRoleEdges();
  testTakeoverGate();
  testC6OrderCaught();
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
  std::printf("usb_cold_host: inspect (the fastboot path, 11 leftovers, unknowns), 8 test-hook masks undone by the cold start and the "
              "takeover replayed to RUN_STOP, the host role (sim 4, host boot paths, sim 31 x 4 FIFO behaviours; no device register in "
              "the host role), its edges, the takeover's gate, C6's order flagged, hard and soft failures, stop and block reset, "
              "arguments, the link watch: all passed\n");
  return 0;
}
