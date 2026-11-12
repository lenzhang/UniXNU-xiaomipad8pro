// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera level 1 (piano-camera=1): the read-only survey of the camera hardware as the boot chain left it, and the checks that
// decide whether level 2 may power a sensor. Portable: every access is a load through Loader::rd (the kernel maps the page on
// demand), every PMIC access an observer read through Pmic, the cmd-db lookup a read of the copied database; nothing here stores
// (tooling/k27/build-native-piano-camera.sh greps it). Host tests: tests/camera_host.cpp (a register model that refuses a load from a
// block whose GDSC or clock is off, as the bus would stall).
//
// What is read, in this order (each part can be turned off by its boot argument):
//   GCC      the camera branches (AHB, XO, the MCLK controller's AHB, HF/SF AXI, QMIP), the QUP2 vote word and SE1's branches
//   CAMCC    (piano-camera-cc) the GDSCs, the branches and clock sources of the camera path, the PLLs' MODE/OPMODE/L
//   MCLKCC   (piano-camera-mclkcc) PLL0, MCLK0..7 (source, divider, M/N, branch), the sleep branch
//   CAMSS    (piano-camera-blocks) CCI0's version and state, CSID/TFE versions: only for a block that is powered and clocked
//   QUP2 SE1 the GENI engine's firmware word and protocol, only while its four branches run (piano-camera-qup)
//   TLMM     the ten camera pins (configuration and level)
//   PMIC     (piano-camera-spmi) the eight camera/CSIPHY LDOs over the SPMI observer: type, ready, set point, mode, enable; cmd-db
//            addresses of the voted five
//   SMMU     (piano-camera-smmu) the stream-match groups covering 0x1c00..0x1c1f and their banks
#pragma once
#include <stdint.h>
#include <string.h>
#if __has_include(<new>)
#include <new>
#else
// The kernel has no <new>: XNU's own forward declaration (libkern/c++/safe_allocation.h). The reserved placement form, which the
// compiler emits as its pointer argument (no call, no import).
void *operator new(size_t, void *) noexcept;
#endif
#include "CameraHw.h"

namespace pianocamera {

// A value reset in place. `x = T()` builds the fresh T on the stack first and copies it: with Survey (1.8 KiB), StreamResult
// (1.5 KiB) and level 3's objects on it, the camera thread's 16 KiB kernel stack overflowed (c3rp, 2026-11-13: "Invalid kernel
// stack pointer (probable overflow)" while entering DRV-2's 64 KiB mapping; docs/drivers/camera.md section 8.4).
template <class T>
inline void renew(T &x)
{
  x.~T();
  ::new (static_cast<void *>(&x)) T();
}

struct Loader {
  void *ctx = nullptr;
  uint32_t (*rd)(void *ctx, uint64_t pa) = nullptr;   // one 32-bit load
};
// The PMIC side (observer reads only): the APID of a PPID (and its owner EE), and n (1..8) registers of an APID.
struct Pmic {
  void *ctx = nullptr;
  int (*apid)(void *ctx, uint16_t ppid, int *apid, uint32_t *owner) = nullptr;   // 0, or negative (not in the map)
  int (*read)(void *ctx, int apid, uint8_t reg, uint8_t *value, unsigned n) = nullptr;
  int (*cmdDb)(void *ctx, const char *name, uint32_t *address) = nullptr;   // 0 and the RPMh address, or negative
};

struct SurveyOptions {
  bool cc = true, mclkcc = true, blocks = true, qup = true, spmi = true, smmu = true;
};

// ------------------------------------------------------------------------------------------------------------- decoders
inline bool branchRunning(uint32_t cbcr) { return !(cbcr & kCbcrClkOff); }
inline bool branchEnabled(uint32_t cbcr) { return (cbcr & kCbcrEnable) != 0; }
// A GCC branch with hardware clock gating (HW_CTL, bit 1) reads CLK_OFF set while idle even when enabled (the tablet's camera and
// MCLK-controller AHB branches, c1 2026-11-12: 0x88000002 off; the display's 0x88000003 enabled and idle, dpu.md §2): Linux's rule
// (clk-branch.c skips the halt check in hardware gating mode) counts it on when enabled; otherwise a branch is on when CLK_OFF is clear.
inline bool branchHwGated(uint32_t cbcr) { return (cbcr & kCbcrHwCtl) != 0; }
inline bool gccClockOn(uint32_t cbcr) { return branchRunning(cbcr) || (branchEnabled(cbcr) && branchHwGated(cbcr)); }
inline bool gdscOn(uint32_t gdscr) { return (gdscr & kGdscPwrOn) != 0; }
inline bool gdscRequestedOn(uint32_t gdscr) { return !(gdscr & kGdscCollapse); }
inline bool pllLocked(uint32_t mode) { return (mode & kPllLockDet) != 0; }
inline bool pllRunning(uint32_t mode, uint32_t opmode) { return (opmode & 1u) && (mode & kPllOutctrl) && (mode & kPllLockDet); }
inline bool pinDrivenHigh(uint32_t cfg, uint32_t io) { return pinFunction(cfg) == 0 && (cfg & kTlmmOe) && (io & kTlmmOut); }
inline const char *pullName(uint32_t cfg)
{
  static const char *const names[4] = {"none", "down", "keeper", "up"};
  return names[cfg & kTlmmPullMask];
}

// An RCG's rate from its words: the source (XO only known here: a PLL source reports 0 Hz with the PLL's rate unknown), the half
// divider ((h + 1) / 2, 0 = 1), and the M/N counter when in dual-edge mode (M / N, with N from ~(N - M)).
struct RcgState {
  uint32_t cmd = 0, cfg = 0, m = 0, n = 0, d = 0;
  bool rootOff = true;
  uint64_t hz = 0;   // when the source is the XO
};
inline uint64_t rcgRate(uint64_t parentHz, uint32_t cfg, uint32_t m, uint32_t nReg, unsigned mndBits)
{
  const uint32_t half = rcgHalfDiv(cfg);
  uint64_t hz = half > 1 ? parentHz * 2 / (half + 1) : parentHz;
  if (rcgMode(cfg) == 2 && mndBits) {
    const uint32_t mask = (1u << mndBits) - 1, mm = m & mask, nMinusM = (~nReg) & mask, nn = nMinusM + mm;
    if (mm && nn) hz = hz * mm / nn;
  }
  return hz;
}
inline bool rcgIsXoUndivided(uint32_t cfg) { return rcgSource(cfg) == 0 && rcgHalfDiv(cfg) <= 1 && rcgMode(cfg) == 0; }

// -------------------------------------------------------------------------------------------------------------- results
struct GccState {
  uint32_t cameraAhb = 0, cameraXo = 0, mclkAhb = 0, hfAxi = 0, sfAxi = 0, qmip[3] = {};
  uint32_t qupVote = 0, qup[5] = {};   // m-ahb, s-ahb, core, core2x, s1
  uint32_t s1RcgCmd = 0, s1RcgCfg = 0;
  bool qupSe1Clocked = false;
};
struct GdscState {
  uint32_t gdscr = 0, cfg = 0;
};
struct CcState {
  bool read = false;
  const char *why = "not surveyed";
  GdscState gdsc[kCcGdscCount];
  uint32_t branch[kCcBranchCount] = {};
  RcgState rcg[kCcRcgCount];
  uint32_t pllMode[kCcPllCount] = {}, pllOpmode[kCcPllCount] = {}, pllL[kCcPllCount] = {};
  bool titanOn() const { return gdscOn(gdsc[0].gdscr); }
  uint32_t branchAt(uint32_t off) const
  {
    for (unsigned i = 0; i < kCcBranchCount; ++i)
      if (kCcBranches[i].off == off) return branch[i];
    return kCbcrClkOff;
  }
  uint32_t gdscAt(uint32_t off) const
  {
    for (unsigned i = 0; i < kCcGdscCount; ++i)
      if (kCcGdscs[i].off == off) return gdsc[i].gdscr;
    return 0;
  }
};
struct MclkState {
  bool read = false;
  const char *why = "not surveyed";
  uint32_t pllMode = 0, pllOpmode = 0, pllL = 0, pllUser = 0;
  RcgState rcg[kMclkCount];
  uint32_t branch[kMclkCount] = {}, sleep = 0;
};
struct BlockState {
  bool read = false;
  const char *why = "not surveyed";
  uint32_t version = 0;
};
struct CciState {
  bool read = false;
  const char *why = "not surveyed";
  uint32_t version = 0, irqMask = 0, irqStatus = 0, sclCtl = 0, sdaCtl0 = 0, miscCtl = 0, curCount[2] = {}, readLevel = 0;
};
struct Se1State {
  bool read = false;
  const char *why = "not surveyed";
  uint32_t fwRevision = 0, status = 0;
  unsigned protocol() const { return (fwRevision >> 8) & 0xffu; }
};
struct PinState {
  uint32_t cfg = 0, io = 0;
};
struct RailState {
  bool found = false, read = false, alt = false;
  uint16_t ppid = 0;
  int apid = -1;
  uint32_t owner = 0;
  uint8_t type = 0, subtype = 0, status = 0, mode = 0, en = 0;
  uint16_t mv = 0;
  int cmdDbRc = -1;
  uint32_t address = 0;   // RPMh (cmd-db)
  bool enabled() const { return read && (en & kPmicEnabled); }
  bool ready() const { return read && (status & kPmicReady); }
};
struct SmmuGroup {
  unsigned index = 0;
  uint32_t smr = 0, s2cr = 0, cbar = 0, sctlr = 0;
  bool bankRead = false;
};
struct SmmuState {
  bool read = false;
  const char *why = "not surveyed";
  uint32_t scr0 = 0, idr0 = 0, idr1 = 0;
  unsigned groups = 0, banks = 0, freeGroups = 0;
  SmmuGroup found[6];
  unsigned count = 0;
};

struct Survey {
  GccState gcc;
  CcState cc;
  MclkState mclk;
  BlockState blocks[kBlockCount];
  CciState cci;
  Se1State se1;
  PinState pins[kPinCount];
  RailState rails[kRailCount];
  SmmuState smmu;
  unsigned loads = 0, pmicReads = 0, pmicFailures = 0;
};

// --------------------------------------------------------------------------------------------------------------- reading
class Surveyor {
 public:
  Surveyor(const Loader &l, const Pmic &p, const SurveyOptions &o) : l_(l), p_(p), o_(o) {}

  // Into `s`, renewed in place (no Survey on the stack).
  void run(Survey &s)
  {
    renew(s);
    gcc(s);
    // A clock controller is read only while GCC clocks its AHB interface (Linux keeps both branches on from GCC's probe).
    if (!o_.cc) s.cc.why = "piano-camera-cc=0";
    else if (!gccClockOn(s.gcc.cameraAhb)) s.cc.why = "GCC camera AHB off (its registers are not clocked)";
    else camcc(s);
    if (!o_.mclkcc) s.mclk.why = "piano-camera-mclkcc=0";
    else if (!gccClockOn(s.gcc.mclkAhb)) s.mclk.why = "GCC MCLK-controller AHB off (its registers are not clocked)";
    else mclkcc(s);
    if (o_.blocks) blocks(s);
    if (o_.qup) se1(s);
    pins(s);
    if (o_.spmi) rails(s);
    if (o_.smmu) smmu(s);
    s.loads = loads_;
  }

 private:
  uint32_t ld(uint64_t pa)
  {
    ++loads_;
    return l_.rd(l_.ctx, pa);
  }
  RcgState rcg(uint64_t base, unsigned mndBits)
  {
    RcgState r;
    r.cmd = ld(base);
    r.cfg = ld(base + kRcgCfg);
    if (mndBits) {
      r.m = ld(base + kRcgM);
      r.n = ld(base + kRcgN);
      r.d = ld(base + kRcgD);
    }
    r.rootOff = (r.cmd & kRcgRootOff) != 0;
    r.hz = rcgSource(r.cfg) == 0 ? rcgRate(kXoHz, r.cfg, r.m, r.n, mndBits) : 0;
    return r;
  }

  void gcc(Survey &s)
  {
    GccState &g = s.gcc;
    g.cameraAhb = ld(kGcc + kGccCameraAhb);
    g.cameraXo = ld(kGcc + kGccCameraXo);
    g.mclkAhb = ld(kGcc + kGccMclkAhb);
    g.hfAxi = ld(kGcc + kGccCameraHfAxi);
    g.sfAxi = ld(kGcc + kGccCameraSfAxi);
    g.qmip[0] = ld(kGcc + kGccQmipNrt);
    g.qmip[1] = ld(kGcc + kGccQmipRt);
    g.qmip[2] = ld(kGcc + kGccQmipCmd);
    g.qupVote = ld(kGcc + kGccQup2Vote);
    const uint32_t q[5] = {kGccQup2MAhb, kGccQup2SAhb, kGccQup2Core, kGccQup2Core2x, kGccQup2S1};
    bool all = true;
    for (unsigned i = 0; i < 5; ++i) {
      g.qup[i] = ld(kGcc + q[i]);
      // The AHB branches are gated by hardware while idle: a vote counts for them (their CLK_OFF may read set although voted).
      const bool ahb = i < 2;
      all = all && (branchRunning(g.qup[i]) || (ahb && (g.qupVote & (1u << (i == 0 ? 2 : 1)))));
    }
    g.qupSe1Clocked = all && branchRunning(g.qup[4]) && branchRunning(g.qup[2]);
    g.s1RcgCmd = ld(kGcc + kGccQup2S1Rcg);
    g.s1RcgCfg = ld(kGcc + kGccQup2S1Rcg + kRcgCfg);
  }

  void camcc(Survey &s)
  {
    CcState &c = s.cc;
    for (unsigned i = 0; i < kCcGdscCount; ++i) {
      c.gdsc[i].gdscr = ld(kCamCc + kCcGdscs[i].off);
      c.gdsc[i].cfg = ld(kCamCc + kCcGdscs[i].off + kGdscCfg);
    }
    for (unsigned i = 0; i < kCcBranchCount; ++i) c.branch[i] = ld(kCamCc + kCcBranches[i].off);
    for (unsigned i = 0; i < kCcRcgCount; ++i)
      c.rcg[i] = rcg(kCamCc + kCcRcgs[i].off, kCcRcgs[i].off == kCcCci0Rcg ? 8 : 0);
    for (unsigned n = 0; n < kCcPllCount; ++n) {
      c.pllMode[n] = ld(kCamCc + ccPll(n) + kPllMode);
      c.pllOpmode[n] = ld(kCamCc + ccPll(n) + kPllOpmode);
      c.pllL[n] = ld(kCamCc + ccPll(n) + kPllTaycanL);
    }
    c.read = true;
    c.why = "read";
  }

  void mclkcc(Survey &s)
  {
    MclkState &m = s.mclk;
    m.pllMode = ld(kMclkCc + kMclkPll + kPllMode);
    m.pllOpmode = ld(kMclkCc + kMclkPll + kPllOpmode);
    m.pllL = ld(kMclkCc + kMclkPll + kPllRivianL);
    m.pllUser = ld(kMclkCc + kMclkPll + kPllRivianUserCtl);
    for (unsigned n = 0; n < kMclkCount; ++n) {
      m.rcg[n] = rcg(kMclkCc + mclkRcg(n), 8);
      m.branch[n] = ld(kMclkCc + mclkBranch(n));
    }
    m.sleep = ld(kMclkCc + kMclkSleep);
    m.read = true;
    m.why = "read";
  }

  // The camera AHB path: TITAN_TOP powered and the top AHB branch running (CAMCC read), GCC's camera AHB running.
  const char *ahbGate(const Survey &s)
  {
    if (!s.cc.read) return s.cc.why;
    if (!s.cc.titanOn()) return "TITAN_TOP GDSC off";
    if (!branchRunning(s.cc.branchAt(kCcCamTopAhb))) return "camera top AHB branch off";
    return nullptr;
  }

  void blocks(Survey &s)
  {
    const char *gate = ahbGate(s);
    for (unsigned i = 0; i < kBlockCount; ++i) {
      const Block &b = kBlocks[i];
      BlockState &st = s.blocks[i];
      if (gate) {
        st.why = gate;
      } else if (b.gdsc && !gdscOn(s.cc.gdscAt(b.gdsc))) {
        st.why = "its GDSC is off";
      } else if (!branchRunning(s.cc.branchAt(b.branch))) {
        st.why = "its clock is off";
      } else {
        st.version = ld(b.pa);
        st.read = true;
        st.why = "read";
      }
    }
    CciState &c = s.cci;
    if (gate) {
      c.why = gate;
    } else if (!branchRunning(s.cc.branchAt(kCcCci0))) {
      c.why = "the CCI0 clock is off";
    } else {
      c.version = ld(kCci0 + kCciHwVersion);
      c.irqMask = ld(kCci0 + kCciIrqMask);
      c.irqStatus = ld(kCci0 + kCciIrqStatus);
      c.sclCtl = ld(kCci0 + cciSclCtl(kCciMaster));
      c.sdaCtl0 = ld(kCci0 + cciSdaCtl0(kCciMaster));
      c.miscCtl = ld(kCci0 + cciMiscCtl(kCciMaster));
      c.curCount[0] = ld(kCci0 + cciCurCount(kCciMaster, 0));
      c.curCount[1] = ld(kCci0 + cciCurCount(kCciMaster, 1));
      c.readLevel = ld(kCci0 + cciReadLevel(kCciMaster));
      c.read = true;
      c.why = "read";
    }
  }

  // QUP2 SE1's GENI words (GENI_FW_REVISION_RO 0x68: protocol 15:8 (3 = I2C); GENI_STATUS 0x40): never its FIFO windows.
  void se1(Survey &s)
  {
    Se1State &e = s.se1;
    if (!s.gcc.qupSe1Clocked) {
      e.why = "QUP2 or SE1 not clocked";
      return;
    }
    e.fwRevision = ld(kQup2Se1 + 0x68);
    e.status = ld(kQup2Se1 + 0x40);
    e.read = true;
    e.why = "read";
  }

  void pins(Survey &s)
  {
    for (unsigned i = 0; i < kPinCount; ++i) {
      s.pins[i].cfg = ld(tlmmCfg(kPins[i].gpio));
      s.pins[i].io = ld(tlmmIo(kPins[i].gpio));
    }
  }

  void rails(Survey &s)
  {
    if (!p_.apid || !p_.read) return;
    for (unsigned i = 0; i < kRailCount; ++i) {
      RailState &r = s.rails[i];
      const Rail &t = kRails[i];
      const uint16_t pages[2] = {uint16_t(t.sid << 8 | ldoPage(t.ldo)), uint16_t(t.sid << 8 | ldoPageAlt(t.ldo))};
      for (unsigned k = 0; k < 2 && !r.found; ++k) {
        int apid = -1;
        uint32_t owner = 0;
        if (p_.apid(p_.ctx, pages[k], &apid, &owner) || apid < 0) continue;
        uint8_t two[2] = {};
        ++s.pmicReads;
        if (p_.read(p_.ctx, apid, kPmicType, two, 2)) {
          ++s.pmicFailures;
          continue;
        }
        if (two[0] != kPmicTypeLdo) continue;
        r.found = true;
        r.alt = k == 1;
        r.ppid = pages[k];
        r.apid = apid;
        r.owner = owner;
        r.type = two[0];
        r.subtype = two[1];
      }
      if (!r.found) continue;
      uint8_t vset[2] = {}, me[2] = {};
      s.pmicReads += 3;
      if (p_.read(p_.ctx, r.apid, kPmicStatus1, &r.status, 1) || p_.read(p_.ctx, r.apid, kPmicVset, vset, 2) ||
          p_.read(p_.ctx, r.apid, kPmicMode, me, 2)) {
        ++s.pmicFailures;
        continue;
      }
      r.mv = uint16_t(vset[0] | vset[1] << 8);
      r.mode = me[0];
      r.en = me[1];
      r.read = true;
    }
    if (p_.cmdDb)
      for (unsigned i = 0; i < kRailCount; ++i) s.rails[i].cmdDbRc = p_.cmdDb(p_.ctx, kRails[i].cmdDb, &s.rails[i].address);
  }

  // ARM SMMUv2 (MMU-500): SCR0 +0, IDR0 +0x20, IDR1 +0x24 (31 PAGESIZE 64K, 30:28 NUMPAGENDXB, 7:0 NUMCB), IDR0 7:0 NUMSMRG; SMR n
  // +0x800 + 4 n (31 valid, 30:16 mask, 14:0 id), S2CR n +0xc00 + 4 n (17:16 type: 0 translate via bank 7:0, 1 bypass, 2 fault),
  // CBAR n +0x1000 + 4 n; bank n's SCTLR at (1 << (NUMPAGENDXB + 1)) pages + n pages. As PianoVideo's and PianoDpu's surveys.
  void smmu(Survey &s)
  {
    SmmuState &m = s.smmu;
    m.scr0 = ld(kSmmu);
    m.idr0 = ld(kSmmu + 0x20);
    m.idr1 = ld(kSmmu + 0x24);
    m.groups = m.idr0 & 0xffu;
    m.banks = m.idr1 & 0xffu;
    const unsigned pages = 1u << (((m.idr1 >> 28) & 7u) + 1);
    if ((m.idr1 >> 31) || !m.groups || !m.banks || m.groups > 128 || uint64_t(pages + m.banks) * 4096 > kSmmuBytes) {
      m.why = "unsupported SMMU geometry";
      return;
    }
    for (unsigned i = 0; i < m.groups; ++i) {
      const uint32_t smr = ld(kSmmu + 0x800 + 4 * i);
      if (!(smr & 0x80000000u)) {
        ++m.freeGroups;
        continue;
      }
      const uint32_t mask = (smr >> 16) & 0x7fffu;
      bool hit = false;
      for (uint32_t sid = kCamStream; sid <= kCamStreamScanLast && !hit; ++sid) hit = ((smr ^ sid) & 0x7fffu & ~mask) == 0;
      if (!hit || m.count == sizeof m.found / sizeof m.found[0]) continue;
      SmmuGroup &g = m.found[m.count++];
      g.index = i;
      g.smr = smr;
      g.s2cr = ld(kSmmu + 0xc00 + 4 * i);
      const unsigned type = (g.s2cr >> 16) & 3u, bank = g.s2cr & 0xffu;
      if (type == 0 && bank < m.banks) {
        g.cbar = ld(kSmmu + 0x1000 + 4 * bank);
        g.sctlr = ld(kSmmu + uint64_t(pages + bank) * 4096);
        g.bankRead = true;
      }
    }
    m.read = true;
    m.why = "read";
  }

  const Loader &l_;
  const Pmic &p_;
  const SurveyOptions &o_;
  unsigned loads_ = 0;
};

inline void survey(const Loader &l, const Pmic &p, const SurveyOptions &o, Survey &out) { Surveyor(l, p, o).run(out); }
inline Survey survey(const Loader &l, const Pmic &p, const SurveyOptions &o)   // (the host tests; the kernel's fill their own)
{
  Survey s;
  survey(l, p, o, s);
  return s;
}

// ------------------------------------------------------------------------------------------------------- level 2's checks
// Whether level 2 may power a sensor, from the survey alone. nullptr = yes; else why not (nothing is written for that sensor).
struct BusChoice {
  bool rearCci = true;   // the rear sensor through CCI0 (else software I2C on gpio115/116)
  bool blind = false;    // vote a rail whose PMIC state could not be read
};
inline const char *railRefusal(const Survey &s, unsigned i, bool blind)
{
  const RailState &r = s.rails[i];
  if (!r.address) return "no RPMh address in cmd-db";
  if (!r.read && !blind) return "its PMIC state could not be read";
  // Found enabled at another voltage: something else set it; not ours to change.
  if (r.enabled() && (r.mv + 8 < kRails[i].mv || r.mv > kRails[i].mv + 8)) return "found enabled at another voltage";
  return nullptr;
}
inline const char *sensorRefusal(const Survey &s, Sensor which, const BusChoice &bus)
{
  const bool rear = which == Sensor::rear;
  const SensorFacts &f = kSensors[rear ? 0 : 1];
  if (!s.mclk.read) return s.mclk.why && !strcmp(s.mclk.why, "piano-camera-mclkcc=0") ? "the MCLK controller was not read (piano-camera-mclkcc=0)"
                                                                                       : "the MCLK controller was not read (its GCC AHB is off)";
  if (branchEnabled(s.mclk.branch[f.mclk]) || branchRunning(s.mclk.branch[f.mclk])) return "its MCLK branch is already on";
  const unsigned reset = rear ? kPinRearReset : kPinFrontReset, avdd = rear ? kPinRearAvdd : kPinFrontAvdd;
  if (pinDrivenHigh(s.pins[reset].cfg, s.pins[reset].io)) return "its reset line is driven high (the sensor is in use)";
  if (pinDrivenHigh(s.pins[avdd].cfg, s.pins[avdd].io)) return "its analogue supply enable is driven high";
  const unsigned first = rear ? kRailRearVddd : kRailFrontVddio, count = rear ? 3 : 2;
  for (unsigned i = first; i < first + count; ++i)
    if (const char *why = railRefusal(s, i, bus.blind)) return why;
  if (rear && bus.rearCci) {
    if (!s.cc.read) return "CAMCC was not read (piano-camera-cc=0): use piano-camera-rear-bus=1";
    if (branchEnabled(s.cc.branchAt(kCcCci0)) && s.cci.read && (s.cci.curCount[0] || s.cci.curCount[1]))
      return "CCI0 is running with queued words (someone else drives it)";
  }
  return nullptr;
}

}  // namespace pianocamera
