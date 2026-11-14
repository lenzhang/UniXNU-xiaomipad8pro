// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 1: what the survey reads, in which order, and the gates that keep it off blocks that cannot answer. Portable (host
// tests in tests/wifi_host.cpp): every hardware access goes through Load, a function of a physical address that returns the 32-bit
// word there. There is no store anywhere in this file, so a level 1 survey cannot write to the hardware.
//
// The gates (SHENG WifiProbe.cpp, hardware-verified on SM8550 for the same controller family: "a read of an unclocked block stalls the
// bus"): the controller's PARF is read only when the PCIe0 power domain is on, its block reset is released and its interface clocks
// (cfg_ahb, aux, master and slave AXI) run; the DBI and the iATU (in the controller's slave address space) also need the slave Q2A
// bridge and the CPU-to-PCIe NoC branch; the PHY is read only when its own power domain is on, its reset is released and cfg_ahb and
// aux run. The endpoint's configuration space is never read at level 1 (an access through an iATU nobody programmed can abort).
#pragma once
#include <stdint.h>
#include "WifiHw.h"
#include "SpmiObserver.h"

namespace pianowifi {

// ---------------------------------------------------------------------------------------------------------------- decoders
inline bool gdscOn(uint32_t gdscr) { return (gdscr & kGdscPwrOn) != 0; }
inline bool branchOff(uint32_t cbcr) { return (cbcr & kCbcrClkOff) != 0; }
inline bool resetAsserted(uint32_t bcr) { return (bcr & kBcrAssert) != 0; }

struct PinDecode {
  unsigned pull, function, driveMa;
  bool outputEnable, in, out;
};
inline PinDecode decodePin(uint32_t cfg, uint32_t io)
{
  PinDecode d;
  d.pull = cfg & 3u;
  d.function = (cfg >> 2) & 0xfu;
  d.driveMa = (((cfg >> 6) & 7u) + 1) * 2;
  d.outputEnable = (cfg >> 9) & 1u;
  d.in = io & 1u;
  d.out = (io >> 1) & 1u;
  return d;
}
inline const char *pullName(unsigned pull) { return pull == 0 ? "none" : pull == 1 ? "down" : pull == 2 ? "keeper" : "up"; }

// The DesignWare LTSSM state (PORT_DEBUG0 bits 5:0); only the states a first link test needs by name.
inline const char *ltssmName(uint32_t state)
{
  switch (state & kLtssmMask) {
  case 0x00: return "detect.quiet";
  case 0x01: return "detect.active";
  case 0x02: return "polling.active";
  case 0x11: return "L0";
  default: return "other";
  }
}

// --------------------------------------------------------------------------------------------------------- the survey record
struct ClockState {
  uint32_t gdsc = 0, gdscCfg = 0, phyGdsc = 0, phyGdscCfg = 0, collapse = 0, vote8 = 0, vote0 = 0;
  uint32_t bcr[kResetCount] = {};
  uint32_t cbcr[kBranchCount] = {};
  uint32_t auxRcg = 0, auxCfg = 0, rchngRcg = 0, rchngCfg = 0, pipeMux = 0, clkref = 0;
  uint32_t offMask = 0;    // bit i: kBranches[i] reports CLK_OFF
  uint32_t voteMask = 0;   // bit i: kBranches[i]'s vote bit is set
  uint32_t idleMask = 0;   // bit i: kBranches[i] reports CLK_OFF but is voted, hardware gated and under HW_CTL (may be only idle)
  bool hwcgRunning = false;   // piano-wifi-hwcg=1: count the idleMask branches as running for the gates
  uint32_t stopped() const { return hwcgRunning ? offMask & ~idleMask : offMask; }
};
struct PinState {
  uint32_t cfg = 0, io = 0;
};
struct ControllerState {
  bool parfRead = false, dbiRead = false;
  const char *skipped = nullptr;   // why the controller was not read
  uint32_t sysCtrl = 0, pmCtrl = 0, pmStts = 0, phyCtrl = 0, ltssm = 0, intAll = 0, status = 0, dbiBase = 0, dbiBaseHi = 0, slvSpace = 0, slvSpaceHi = 0,
           atuBase = 0, atuBaseHi = 0, deviceType = 0, bdfToSid = 0;
  uint32_t id = 0, command = 0, classRev = 0, busNumbers = 0, debug0 = 0, debug1 = 0;
  uint32_t expCap = 0, linkCap = 0, linkCtlSta = 0;   // the PCI Express capability's offset, link capabilities, control | status << 16
  uint32_t atuCtrl1[2] = {}, atuCtrl2[2] = {}, atuBase0[2] = {}, atuLimit[2] = {}, atuTarget[2] = {};
};
struct PhyState {
  bool read = false;
  const char *skipped = nullptr;
  uint32_t swReset = 0, status1 = 0, powerDown = 0, start = 0;
};
struct Survey {
  ClockState clocks;
  PinState pins[kPinCount];
  ControllerState controller;
  PhyState phy;
  unsigned loads = 0;
};

// ------------------------------------------------------------------------------------------------------------- the gates
inline bool controllerReadable(const ClockState &c, const char **why)
{
  if (!gdscOn(c.gdsc)) { *why = "PCIe0 power domain off"; return false; }
  if (resetAsserted(c.bcr[kResetPcie0])) { *why = "PCIe0 block reset asserted"; return false; }
  const uint32_t needed = 1u << kCfgAhb | 1u << kAux | 1u << kMstrAxi | 1u << kSlvAxi;
  if (c.stopped() & needed) { *why = "PCIe0 interface clocks off"; return false; }
  return true;
}
inline bool dbiReadable(const ClockState &c, const char **why)
{
  if (!controllerReadable(c, why)) return false;
  if (c.stopped() & (1u << kSlvQ2a | 1u << kCnocSfAxi)) { *why = "PCIe0 slave path clocks off"; return false; }
  return true;
}
inline bool phyReadable(const ClockState &c, const char **why)
{
  if (!gdscOn(c.phyGdsc)) { *why = "PCIe0 PHY power domain off"; return false; }
  if (resetAsserted(c.bcr[kResetPcie0Phy])) { *why = "PCIe0 PHY block reset asserted"; return false; }
  if (c.stopped() & (1u << kCfgAhb | 1u << kAux)) { *why = "PHY interface clocks off"; return false; }
  return true;
}

// ------------------------------------------------------------------------------------------------------------- the reads
template <class Load>
void surveyClocks(Survey &s, Load load, bool hwcgRunning)
{
  auto g = [&](uint32_t off) { ++s.loads; return load(kGcc + off); };
  ClockState &c = s.clocks;
  c.hwcgRunning = hwcgRunning;
  c.gdsc = g(kGccPcie0Gdsc);
  c.gdscCfg = g(kGccPcie0Gdsc + 4);
  c.phyGdsc = g(kGccPcie0PhyGdsc);
  c.phyGdscCfg = g(kGccPcie0PhyGdsc + 4);
  c.collapse = g(kGccCollapse);
  for (unsigned i = 0; i < kResetCount; ++i) c.bcr[i] = g(kResets[i].bcr);
  c.vote8 = g(kGccVote8);
  c.vote0 = g(kGccVote0);
  for (unsigned i = 0; i < kBranchCount; ++i) {
    c.cbcr[i] = g(kBranches[i].cbcr);
    const uint32_t vote = kBranches[i].vote == kGccVote8 ? c.vote8 : c.vote0;
    const bool voted = (vote >> kBranches[i].bit) & 1u;
    if (voted) c.voteMask |= 1u << i;
    if (!branchOff(c.cbcr[i])) continue;
    c.offMask |= 1u << i;
    if (kBranches[i].hwcg && voted && (c.cbcr[i] & kCbcrHwCtl)) c.idleMask |= 1u << i;
  }
  c.auxRcg = g(kGccPcie0AuxRcg);
  c.auxCfg = g(kGccPcie0AuxRcg + 4);
  c.rchngRcg = g(kGccPcie0RchngRcg);
  c.rchngCfg = g(kGccPcie0RchngRcg + 4);
  c.pipeMux = g(kGccPcie0PipeMux);
  ++s.loads;
  c.clkref = load(kTcsrPcie0ClkRef);
}

template <class Load>
void surveyPins(Survey &s, Load load)
{
  for (unsigned i = 0; i < kPinCount; ++i) {
    const uint64_t page = kTlmm + uint64_t(kPins[i].gpio) * kTlmmStride;
    s.pins[i].cfg = load(page);
    s.pins[i].io = load(page + 4);
    s.loads += 2;
  }
}

template <class Load>
void surveyController(Survey &s, Load load)
{
  ControllerState &r = s.controller;
  if (!controllerReadable(s.clocks, &r.skipped)) return;
  auto p = [&](uint32_t off) { ++s.loads; return load(kParf + off); };
  r.sysCtrl = p(kParfSysCtrl);
  r.pmCtrl = p(kParfPmCtrl);
  r.pmStts = p(kParfPmStts);
  r.phyCtrl = p(kParfPhyCtrl);
  r.ltssm = p(kParfLtssm);
  r.intAll = p(kParfIntAllStatus);
  r.status = p(kParfStatus);
  r.dbiBase = p(kParfDbiBase);
  r.dbiBaseHi = p(kParfDbiBase + 4);
  r.slvSpace = p(kParfSlvSpace);
  r.slvSpaceHi = p(kParfSlvSpace + 4);
  r.atuBase = p(kParfAtuBase);
  r.atuBaseHi = p(kParfAtuBase + 4);
  r.deviceType = p(kParfDeviceType);
  r.bdfToSid = p(kParfBdfToSidCfg);
  r.parfRead = true;
  if (!dbiReadable(s.clocks, &r.skipped)) return;
  auto d = [&](uint32_t off) { ++s.loads; return load(kDbi + off); };
  r.id = d(kDbiId);
  r.command = d(kDbiCommand);
  r.classRev = d(kDbiClass);
  r.busNumbers = d(kDbiBusNumbers);
  r.debug0 = d(kPortDebug0);
  r.debug1 = d(kPortDebug1);
  // the PCI Express capability: the list from 0x34, at most 16 entries, inside the first 256 bytes
  uint32_t cap = d(kDbiCapPtr) & 0xfcu;
  for (unsigned guard = 0; cap >= 0x40 && cap < 0x100 && guard < 16; ++guard) {
    const uint32_t w = d(cap);
    if ((w & 0xffu) == 0x10) {
      r.expCap = cap;
      r.linkCap = d(cap + 0x0c);
      r.linkCtlSta = d(cap + 0x10);
      break;
    }
    cap = (w >> 8) & 0xfcu;
  }
  for (unsigned i = 0; i < 2; ++i) {
    auto a = [&](uint32_t off) { ++s.loads; return load(kAtu + i * kAtuRegion + off); };
    r.atuCtrl1[i] = a(0x00);
    r.atuCtrl2[i] = a(0x04);
    r.atuBase0[i] = a(0x08);
    r.atuLimit[i] = a(0x10);
    r.atuTarget[i] = a(0x14);
  }
  r.dbiRead = true;
}

template <class Load>
void surveyPhy(Survey &s, Load load)
{
  PhyState &r = s.phy;
  if (!phyReadable(s.clocks, &r.skipped)) return;
  auto p = [&](uint32_t off) { ++s.loads; return load(kPhy + kPcs + off); };
  r.swReset = p(kPcsSwReset);
  r.status1 = p(kPcsStatus1);
  r.powerDown = p(kPcsPowerDown);
  r.start = p(kPcsStart);
  r.read = true;
}

// The whole MMIO part of level 1, in the order the kext logs it.
template <class Load>
void survey(Survey &s, Load load, bool hwcgRunning = false)
{
  surveyClocks(s, load, hwcgRunning);
  surveyPins(s, load);
  surveyController(s, load);
  surveyPhy(s, load);
}

// ------------------------------------------------------------------------------------- what the record says, in one word each
inline bool linkUp(const ControllerState &r)
{
  return r.dbiRead && (r.debug1 & kPortDebug1LinkUp) && (r.debug0 & kLtssmMask) == kLtssmL0;
}
// Link status (PCIe capability + 0x12): speed 3:0, width 9:4
inline unsigned linkSpeed(const ControllerState &r) { return (r.linkCtlSta >> 16) & 0xfu; }
inline unsigned linkWidth(const ControllerState &r) { return (r.linkCtlSta >> 20) & 0x3fu; }

// --------------------------------------------------------------------------------- PMIC regulators and pins over SPMI, read only
// Observer reads (SpmiObserver.h): the only stores are the observer channel's read commands, never a PMIC register. Each peripheral:
// TYPE/SUBTYPE (0x04, 2 bytes), STATUS (0x08), then 0x40..0x46 in one 7-byte read (set point lo/hi, 0x42-0x44, MODE, EN_CTL).
struct PmicPeripheral {
  uint16_t ppid = 0;
  int apid = -1;
  uint32_t owner = 0;
  ReadResult result = ReadResult::error;   // of the last read
  bool present = false, read = false;
  uint8_t type = 0, subtype = 0, status = 0, regs40[7] = {};
  uint32_t millivolts() const { return uint32_t(regs40[0]) | uint32_t(regs40[1]) << 8; }
  bool enabled() const { return (regs40[6] & 0x80u) != 0; }
  bool ready() const { return (status & 0x80u) != 0; }
};
inline bool isLdo(uint8_t type) { return type == 0x04; }
inline bool isSmps(uint8_t type) { return type == 0x03 || type == 0x1c || type == 0x21 || type == 0x22; }

// The identity (TYPE/SUBTYPE) is read until two reads in a row agree (at most kIdentityReads): it decides whose registers the next
// reads take for a regulator's, and on the 2026-11-13 01:19 probe (v34p) two identity reads ended "ok" with another register's value
// (ldof3, an LDO, TYPE 0x00; smpi7, an SMPS, 0x04) while other kexts drove the arbiter, so level 2 had no PPID for two rails.
// *unstable counts the pairs that disagreed; a read that fails (through spmiRead's retries) ends it, as before.
constexpr unsigned kIdentityReads = 4;
inline void pmicRead(const Io &io, const Arbiter &a, uint8_t sid, uint8_t page, PmicPeripheral &p, bool full, unsigned *commands,
                     unsigned *unstable = nullptr)
{
  p.ppid = uint16_t(sid << 8 | page);
  const Apid found = spmiFindApid(io, a, p.ppid);
  p.apid = found.apid;
  p.owner = found.owner;
  if (p.apid < 0) return;
  p.present = true;
  uint8_t id[2] = {0, 0}, last[2] = {0, 0};
  bool have = false, agreed = false;
  for (unsigned i = 0; i < kIdentityReads && !agreed; ++i) {
    ++*commands;
    p.result = spmiRead(io, p.apid, 0x04, id, 2);
    if (p.result != ReadResult::ok) return;
    if (have && id[0] == last[0] && id[1] == last[1]) {
      agreed = true;
    } else {
      if (have && unstable) ++*unstable;
      last[0] = id[0];
      last[1] = id[1];
      have = true;
    }
  }
  if (!agreed) {
    p.result = ReadResult::error;   // four answers, no two in a row alike
    return;
  }
  p.type = id[0];
  p.subtype = id[1];
  if (!full) return;
  ++*commands;
  p.result = spmiRead(io, p.apid, 0x08, &p.status, 1);
  if (p.result != ReadResult::ok) return;
  ++*commands;
  p.result = spmiRead(io, p.apid, 0x40, p.regs40, 7);
  p.read = p.result == ReadResult::ok;
}

struct RailState {
  PmicPeripheral primary, alternate;   // LDOs: page 0xc0 + n and 0x40 + n - 1; SMPSs: primary only
  int chosen = -1;                     // 0 primary, 1 alternate, -1 neither is a regulator of the expected kind
  const PmicPeripheral *reg() const { return chosen == 0 ? &primary : chosen == 1 ? &alternate : nullptr; }
  // No regulator chosen although the map has one of the pages and its identity read, as a kind other than expected: the survey
  // takes it again (PianoWifi.cpp's re-survey), as it does a page whose reads failed.
  bool unexpected() const
  {
    return chosen < 0 && ((primary.present && primary.result == ReadResult::ok) ||
                          (alternate.present && alternate.result == ReadResult::ok));
  }
};
struct PmicSurvey {
  Arbiter arbiter;
  RailState rails[kRailCount];
  PmicPeripheral pins[kPmicPinCount];
  unsigned commands = 0;
  unsigned typeRetries = 0, typeUnstable = 0;   // identity reads again for an unexpected kind; identity pairs that disagreed
};
// The kind a rail's page must have: an SMPS (TYPE 0x03, 0x1c, 0x21, 0x22) or an LDO (0x04).
inline bool kindOk(const Rail &r, const PmicPeripheral &p) { return p.result == ReadResult::ok && (r.smps ? isSmps(p.type) : isLdo(p.type)); }
// A page whose identity read, but not as the expected kind, is read again (up to kKindTries reads in all) before it is given up:
// a wrong value with an "ok" status was the failure of v34p.
constexpr unsigned kKindTries = 3;
inline void pmicReadKind(const Io &io, const Arbiter &a, const Rail &r, uint8_t page, PmicPeripheral &p, PmicSurvey &s)
{
  pmicRead(io, a, r.sid, page, p, false, &s.commands, &s.typeUnstable);
  for (unsigned i = 1; i < kKindTries && p.present && p.result == ReadResult::ok && !kindOk(r, p); ++i) {
    ++s.typeRetries;
    io.delayUs(io.ctx, 1000);
    pmicRead(io, a, r.sid, page, p, false, &s.commands, &s.typeUnstable);
  }
}
inline void surveyPmic(const Io &io, PmicSurvey &s)
{
  s.arbiter = spmiArbiter(io);
  if (!s.arbiter.v7) return;
  for (unsigned i = 0; i < kRailCount; ++i) {
    const Rail &r = kRails[i];
    RailState &st = s.rails[i];
    if (r.smps) {
      pmicReadKind(io, s.arbiter, r, smpsPage(r.smps), st.primary, s);
      if (kindOk(r, st.primary)) st.chosen = 0;
    } else {
      pmicReadKind(io, s.arbiter, r, ldoPage(r.ldo), st.primary, s);
      if (kindOk(r, st.primary)) st.chosen = 0;
      if (st.chosen < 0) pmicReadKind(io, s.arbiter, r, ldoPageAlt(r.ldo), st.alternate, s);
      else pmicRead(io, s.arbiter, r.sid, ldoPageAlt(r.ldo), st.alternate, false, &s.commands, &s.typeUnstable);
      if (st.chosen < 0 && kindOk(r, st.alternate)) st.chosen = 1;
    }
    if (st.chosen >= 0) {
      PmicPeripheral &p = st.chosen == 0 ? st.primary : st.alternate;
      pmicRead(io, s.arbiter, r.sid, uint8_t(p.ppid), p, true, &s.commands, &s.typeUnstable);
      // The full read takes the identity again: it must still be the kind chosen, else no regulator (the re-survey runs).
      if (!kindOk(r, p)) st.chosen = -1;
    }
  }
  for (unsigned i = 0; i < kPmicPinCount; ++i)
    pmicRead(io, s.arbiter, kPmicPins[i].sid, kPmicPins[i].page, s.pins[i], true, &s.commands, &s.typeUnstable);
}

// --------------------------------------------------------------------------------------------------- apps SMMU, read only
// Every stream-match group that matches a stream (ARM SMMUv2: SMR valid bit 31, mask 30:16, id 14:0; S2CR type 17:16 (0 translate
// through context bank 7:0, 1 bypass, 2 fault)), with the bank's SCTLR when it translates.
struct SmmuMatch {
  unsigned group = 0;
  uint32_t smr = 0, s2cr = 0, sctlr = 0;
};
struct SmmuStreams {
  uint32_t idr0 = 0, idr1 = 0, groups = 0, banks = 0, pages = 0;
  const char *error = nullptr;
  SmmuMatch found[2][4];
  unsigned count[2] = {0, 0};
};
template <class Read>
SmmuStreams surveySmmu(const uint16_t (&streams)[2], uint64_t windowBytes, Read rd)
{
  SmmuStreams r;
  r.idr0 = rd(0x20);
  r.idr1 = rd(0x24);
  r.groups = r.idr0 & 0xffu;
  if (r.groups > 128) r.groups = 128;   // Qualcomm: groups above 128 do not behave architecturally (PianoDwc3 SmmuStream.h)
  r.banks = r.idr1 & 0xffu;
  r.pages = 1u << (((r.idr1 >> 28) & 7u) + 1);
  if (!r.groups || !r.banks || r.banks > 128 || r.pages > 128 || uint64_t(r.pages + r.banks) * 4096 > windowBytes) {
    r.error = "unsupported SMMU geometry";
    return r;
  }
  for (unsigned i = 0; i < r.groups; ++i) {
    const uint32_t smr = rd(0x800 + 4 * i);
    if (!(smr & 0x80000000u)) continue;
    const uint32_t mask = (smr >> 16) & 0x7fffu;
    for (unsigned k = 0; k < 2; ++k) {
      if (((smr ^ streams[k]) & 0x7fffu & ~mask) != 0 || r.count[k] >= 4) continue;
      SmmuMatch &m = r.found[k][r.count[k]++];
      m.group = i;
      m.smr = smr;
      m.s2cr = rd(0xc00 + 4 * i);
      const unsigned type = (m.s2cr >> 16) & 3u, bank = m.s2cr & 0xffu;
      if (type == 0 && bank < r.banks) m.sctlr = rd((r.pages + bank) * 4096);
    }
  }
  return r;
}
inline const char *s2crType(uint32_t s2cr)
{
  const unsigned t = (s2cr >> 16) & 3u;
  return t == 0 ? "translate" : t == 1 ? "bypass" : t == 2 ? "fault" : "reserved";
}

// ------------------------------------------------------------------------------------------- the AOP command database, read only
// The address of an RPMh resource by name (package/PianoKeyboard/CmdDb.h, same repository, re-expressed here in this namespace).
// Header: u32 version, u32 magic, 8 slave records of 16 bytes (u16 id, header offset, data offset, entry count, ...), u32 checksum,
// u32 reserved, then the data area the offsets count from (byte 144). Entries are 24 bytes: an 8-byte id, two u32 priorities, u32
// address, u16 aux length, u16 aux offset. Returns 0 with the address, -1 not a cmd-db, -2 no such resource, -3 corrupt.
constexpr uint32_t kCmdDbMagic = 0x0c0330db;
// An entry's aux data sits at the data area + the slave's data offset (its record's +4) + the entry's aux offset; `aux`/`auxBytes`
// get where and how long (0 when it has none or it would lie outside the copy).
inline int cmdDbEntry(const uint8_t *db, uint64_t bytes, const char *name, uint32_t *address, uint64_t *aux, uint32_t *auxBytes)
{
  auto u16 = [db](uint64_t at) { return uint32_t(db[at] | db[at + 1] << 8); };
  auto u32 = [&u16](uint64_t at) { return u16(at) | u16(at + 2) << 16; };
  if (aux) *aux = 0;
  if (auxBytes) *auxBytes = 0;
  if (bytes < 144 || u32(4) != kCmdDbMagic) return -1;
  char id[8] = {0};
  for (unsigned i = 0; i < 8 && name[i]; ++i) id[i] = name[i];
  for (unsigned sl = 0; sl < 8; ++sl) {
    const uint64_t slave = 8 + 16 * sl;
    if (!u16(slave)) break;
    const uint64_t heads = 144 + u16(slave + 2), count = u16(slave + 6);
    if (heads > bytes || count * 24 > bytes - heads) return -3;
    for (uint64_t e = 0; e < count; ++e) {
      const uint64_t entry = heads + 24 * e;
      bool same = true;
      for (unsigned i = 0; i < 8 && same; ++i) same = db[entry + i] == uint8_t(id[i]);
      if (!same) continue;
      *address = u32(entry + 16);
      const uint32_t length = u16(entry + 20);
      const uint64_t at = 144 + uint64_t(u16(slave + 4)) + u16(entry + 22);
      if (length && at < bytes && length <= bytes - at) {
        if (aux) *aux = at;
        if (auxBytes) *auxBytes = length;
      }
      return *address ? 0 : -3;
    }
  }
  return -2;
}
inline int cmdDbFind(const uint8_t *db, uint64_t bytes, const char *name, uint32_t *address)
{
  return cmdDbEntry(db, bytes, name, address, nullptr, nullptr);
}

// A bus clock manager's aux data (cmd-db): u32 unit, u16 width, u8 clock domain. 0 with the three, -4 when the entry has fewer than
// the 7 bytes that carry them.
struct BcmAux { uint32_t unit = 0; uint16_t width = 0; uint8_t domain = 0; };
inline int cmdDbBcm(const uint8_t *db, uint64_t bytes, const char *name, uint32_t *address, BcmAux &out)
{
  uint64_t aux = 0;
  uint32_t length = 0;
  renew(out);
  const int rc = cmdDbEntry(db, bytes, name, address, &aux, &length);
  if (rc) return rc;
  if (length < 7) return -4;
  out.unit = uint32_t(db[aux]) | uint32_t(db[aux + 1]) << 8 | uint32_t(db[aux + 2]) << 16 | uint32_t(db[aux + 3]) << 24;
  out.width = uint16_t(db[aux + 4] | db[aux + 5] << 8);
  out.domain = db[aux + 6];
  return 0;
}

}  // namespace pianowifi
