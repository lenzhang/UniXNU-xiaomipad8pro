// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoWifi's level 1 survey (WifiSurvey.h) against register models:
//   - the gates: with the PCIe0 power domain off, a block reset asserted, an interface clock stopped (or only hardware-idle without
//     piano-wifi-hwcg=1), the PHY domain off, not one load reaches the PARF, the DBI, the iATU or the PHY; the endpoint's
//     configuration window is never loaded in any case;
//   - the reads when everything runs: the PCI Express capability walk, the link decode, the PHY status;
//   - the TLMM decode, the SMMU stream scan, the cmd-db lookup;
//   - the PMIC survey over an SPMI arbiter model: the LDO page choice, and no store other than the observer read commands.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. wifi_host.cpp -o /tmp/wifi_host && /tmp/wifi_host
#include "WifiSurvey.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace pianowifi;
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
  void gcc(uint32_t off, uint32_t v) { regs[kGcc + off] = v; }
  // every branch stopped (CLK_OFF), the domains off: the expected state at hand-over
  void allOff()
  {
    for (const Branch &b : kBranches) gcc(b.cbcr, kCbcrClkOff);
    gcc(kGccPcie0Gdsc, 0);
    gcc(kGccPcie0PhyGdsc, 0);
  }
  // the controller and the PHY powered and clocked, the link in L0 at Gen2 x1, the PCIe capability at 0x70 after a PM capability at 0x40
  void allOn()
  {
    for (const Branch &b : kBranches) gcc(b.cbcr, 1);
    gcc(kGccPcie0Gdsc, kGdscPwrOn);
    gcc(kGccPcie0PhyGdsc, kGdscPwrOn);
    gcc(kGccVote8, 0x7f);
    gcc(kGccVote0, 1u << 11 | 1u << 12 | 1u << 19 | 1u << 20 | 1u << 22);
    regs[kParf + kParfLtssm] = kParfLtssmEnable | 0x11;
    regs[kDbi + kDbiId] = 0x011517cb;
    regs[kDbi + kDbiCapPtr] = 0x40;
    regs[kDbi + 0x40] = 0x00007001;            // PM capability, next 0x70
    regs[kDbi + 0x70] = 0x00420010;            // PCI Express capability, last
    regs[kDbi + 0x70 + 0x10] = 0x00120000;     // link status: Gen2, x1
    regs[kDbi + kPortDebug0] = 0x11;
    regs[kDbi + kPortDebug1] = kPortDebug1LinkUp;
    regs[kPhy + kPcs + kPcsStatus1] = 0;       // PHYSTATUS clear: ready
    regs[kPhy + kPcs + kPcsPowerDown] = 3;
    regs[kPhy + kPcs + kPcsStart] = 3;
  }
};

static void testHandoverOff()
{
  Soc m;
  m.allOff();
  m.regs[kTlmm + 16 * kTlmmStride] = 0x201;   // WLAN_EN: GPIO, pull-down, 2 mA, output enabled, driven low
  m.regs[kTlmm + 102 * kTlmmStride] = 0x201;
  Survey s;
  survey(s, [&](uint64_t pa) { return m.load(pa); });
  CHECK(!s.controller.parfRead && !s.controller.dbiRead && !s.phy.read);
  CHECK(s.controller.skipped && !std::strcmp(s.controller.skipped, "PCIe0 power domain off"));
  CHECK(s.phy.skipped && !std::strcmp(s.phy.skipped, "PCIe0 PHY power domain off"));
  CHECK(m.in(kParf, kParfBytes) == 0 && m.in(kDbi, 0x100000) == 0 && m.in(kPhy, kPhyBytes) == 0 && m.in(kConfig, 0x100000) == 0);
  CHECK(s.clocks.offMask == (1u << kBranchCount) - 1);
  CHECK(s.loads == m.loads.size());
  // GCC: 2 GDSCs x 2 + collapse + resets + 2 votes + branches + 5 RCG/mux words; TCSR 1; TLMM 2 per pin
  CHECK(m.in(kGcc, 0x200000) == 5 + kResetCount + 2 + kBranchCount + 5);
  CHECK(m.in(kTcsrPcie0ClkRef, 4) == 1);
  CHECK(m.in(kTlmm, 0x102000) == 2 * kPinCount);
  const PinDecode d = decodePin(s.pins[kPinWlanEn].cfg, s.pins[kPinWlanEn].io);
  CHECK(d.pull == 1 && d.function == 0 && d.driveMa == 2 && d.outputEnable && !d.out && !d.in);
  CHECK(!std::strcmp(pullName(d.pull), "down"));
}

static void testGates()
{
  {   // every clock reads running but the controller's domain is off (left-over branch state): nothing behind the domain is read
    Soc m;
    m.allOn();
    m.gcc(kGccPcie0Gdsc, 0);
    Survey s;
    survey(s, [&](uint64_t pa) { return m.load(pa); }, true);
    CHECK(!s.controller.parfRead && m.in(kParf, kParfBytes) == 0 && m.in(kDbi, 0x100000) == 0 && s.phy.read);
  }
  {   // the domain on, the controller's block reset asserted
    Soc m;
    m.allOn();
    m.gcc(kResets[kResetPcie0].bcr, kBcrAssert);
    Survey s;
    survey(s, [&](uint64_t pa) { return m.load(pa); });
    CHECK(!s.controller.parfRead && m.in(kParf, kParfBytes) == 0 && m.in(kDbi, 0x100000) == 0);
    CHECK(s.phy.read);   // the PHY has its own domain and reset
  }
  {   // cfg_ahb stopped, not hardware gated: skipped with or without hwcg
    Soc m;
    m.allOn();
    m.gcc(kBranches[kCfgAhb].cbcr, kCbcrClkOff);
    m.gcc(kGccVote8, 0x7f & ~(1u << kBranches[kCfgAhb].bit));
    for (bool hwcg : {false, true}) {
      m.loads.clear();
      Survey s;
      survey(s, [&](uint64_t pa) { return m.load(pa); }, hwcg);
      CHECK(!s.controller.parfRead && !s.phy.read && m.in(kParf, kParfBytes) == 0 && m.in(kPhy, kPhyBytes) == 0);
    }
  }
  {   // cfg_ahb idle under hardware control while voted: read only with hwcg
    Soc m;
    m.allOn();
    m.gcc(kBranches[kCfgAhb].cbcr, kCbcrClkOff | kCbcrHwCtl | 1);
    Survey a;
    survey(a, [&](uint64_t pa) { return m.load(pa); }, false);
    CHECK(!a.controller.parfRead && m.in(kParf, kParfBytes) == 0);
    CHECK(a.clocks.idleMask == 1u << kCfgAhb);
    m.loads.clear();
    Survey b;
    survey(b, [&](uint64_t pa) { return m.load(pa); }, true);
    CHECK(b.controller.parfRead && b.controller.dbiRead && b.phy.read);
  }
  {   // aux is not hardware gated: CLK_OFF with HW_CTL set still means stopped
    Soc m;
    m.allOn();
    m.gcc(kBranches[kAux].cbcr, kCbcrClkOff | kCbcrHwCtl | 1);
    Survey s;
    survey(s, [&](uint64_t pa) { return m.load(pa); }, true);
    CHECK(!s.controller.parfRead && !s.phy.read && s.clocks.idleMask == 0);
  }
  {   // the slave path stopped: PARF read, DBI and iATU not
    Soc m;
    m.allOn();
    m.gcc(kBranches[kCnocSfAxi].cbcr, kCbcrClkOff);
    m.gcc(kGccVote8, 0x3f);
    Survey s;
    survey(s, [&](uint64_t pa) { return m.load(pa); });
    CHECK(s.controller.parfRead && !s.controller.dbiRead && m.in(kDbi, 0x100000) == 0 && m.in(kAtu, kAtuBytes) == 0);
    CHECK(s.controller.skipped && !std::strcmp(s.controller.skipped, "PCIe0 slave path clocks off"));
  }
  {   // the PHY domain off: the controller is read, the PHY is not
    Soc m;
    m.allOn();
    m.gcc(kGccPcie0PhyGdsc, 0);
    Survey s;
    survey(s, [&](uint64_t pa) { return m.load(pa); });
    CHECK(s.controller.dbiRead && !s.phy.read && m.in(kPhy, kPhyBytes) == 0);
  }
}

static void testLinkUp()
{
  Soc m;
  m.allOn();
  Survey s;
  survey(s, [&](uint64_t pa) { return m.load(pa); });
  CHECK(s.controller.parfRead && s.controller.dbiRead && s.phy.read);
  CHECK(s.controller.expCap == 0x70 && linkUp(s.controller) && linkSpeed(s.controller) == 2 && linkWidth(s.controller) == 1);
  CHECK(!std::strcmp(ltssmName(s.controller.debug0), "L0"));
  CHECK(!(s.phy.status1 & kPcsPhyStatus) && s.phy.powerDown == 3 && s.phy.start == 3);
  CHECK(m.in(kConfig, 0x100000) == 0);   // never, even with the link up
  CHECK(s.clocks.voteMask == (1u << kBranchCount) - 1 && s.clocks.offMask == 0);
  // a capability list that loops is cut off
  m.regs[kDbi + 0x70] = 0x00007005;   // id 5 pointing at itself
  m.loads.clear();
  Survey t;
  survey(t, [&](uint64_t pa) { return m.load(pa); });
  CHECK(t.controller.expCap == 0 && m.in(kDbi + 0x70, 4) <= 16);
}

static void testSmmu()
{
  std::map<uint32_t, uint32_t> r;
  r[0x20] = 0x40000000u | 64;                 // 64 stream-match groups
  r[0x24] = 0x60000053u;                      // piano's IDR1: 83 banks, 128 pages
  r[0x800 + 4 * 3] = 0x80000000u | 0x1401;    // the endpoint, exact, translated through bank 5
  r[0xc00 + 4 * 3] = 5;
  r[0x800 + 4 * 9] = 0x80000000u | 0x007f0000u | 0x1400;   // a masked group covering 0x1400..0x147f
  r[0xc00 + 4 * 9] = 1u << 16;                // bypass
  r[0x800 + 4 * 10] = 0x1400;                 // not valid
  r[(128 + 5) * 4096] = 1;                    // bank 5 translates
  const uint16_t streams[2] = {kStreamRoot, kStreamEndpoint};
  const SmmuStreams s = surveySmmu(streams, kSmmuBytes, [&](uint32_t off) { return r[off]; });
  CHECK(!s.error && s.groups == 64 && s.banks == 0x53 && s.pages == 128);
  CHECK(s.count[0] == 1 && s.found[0][0].group == 9 && !std::strcmp(s2crType(s.found[0][0].s2cr), "bypass"));
  CHECK(s.count[1] == 2 && s.found[1][0].group == 3 && s.found[1][0].sctlr == 1 && s.found[1][1].group == 9);
  std::map<uint32_t, uint32_t> bad;
  bad[0x20] = 8;
  bad[0x24] = 0;   // no banks
  CHECK(surveySmmu(streams, kSmmuBytes, [&](uint32_t off) { return bad[off]; }).error != nullptr);
}

static void testCmdDb()
{
  std::vector<uint8_t> db(4096, 0);
  auto put16 = [&](size_t at, uint32_t v) { db[at] = uint8_t(v); db[at + 1] = uint8_t(v >> 8); };
  auto put32 = [&](size_t at, uint32_t v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); };
  put32(4, kCmdDbMagic);
  put16(8, 3);        // slave 0: VRM
  put16(10, 0);       // header offset (from byte 144)
  put16(14, 2);       // two entries
  const char *names[2] = {"ldof3", "smpi7"};
  for (unsigned e = 0; e < 2; ++e) {
    std::memcpy(&db[144 + 24 * e], names[e], std::strlen(names[e]));
    put32(144 + 24 * e + 16, 0x30000 + 0x100 * e);
  }
  uint32_t address = 0;
  CHECK(cmdDbFind(db.data(), db.size(), "ldof3", &address) == 0 && address == 0x30000);
  CHECK(cmdDbFind(db.data(), db.size(), "smpi7", &address) == 0 && address == 0x30100);
  CHECK(cmdDbFind(db.data(), db.size(), "smpg3", &address) == -2);

  // A second slave (BCM, id 5) with SN4 and its aux data: the data area of that slave at +0x200, the entry's aux at +0x10.
  put16(24, 5);        // slave 1
  put16(26, 0x100);    // its entries at 144 + 0x100
  put16(28, 0x200);    // its data at 144 + 0x200
  put16(30, 2);
  std::memcpy(&db[144 + 0x100], "SN4", 3);
  put32(144 + 0x100 + 16, 0x50020);
  put16(144 + 0x100 + 20, 8);       // 8 bytes of aux
  put16(144 + 0x100 + 22, 0x10);
  const size_t aux = 144 + 0x200 + 0x10;
  put32(aux, 1000);                 // unit
  put16(aux + 4, 16);               // width
  db[aux + 6] = 4;                  // clock domain
  std::memcpy(&db[144 + 0x100 + 24], "SN7", 3);
  put32(144 + 0x100 + 24 + 16, 0x50030);
  put16(144 + 0x100 + 24 + 20, 4);  // too short to carry unit, width and domain
  put16(144 + 0x100 + 24 + 22, 0x20);
  BcmAux bcm;
  CHECK(cmdDbBcm(db.data(), db.size(), "SN4", &address, bcm) == 0 && address == 0x50020 && bcm.unit == 1000 && bcm.width == 16 &&
        bcm.domain == 4);
  CHECK(cmdDbFind(db.data(), db.size(), "SN4", &address) == 0 && address == 0x50020);
  CHECK(cmdDbBcm(db.data(), db.size(), "SN7", &address, bcm) == -4 && address == 0x50030 && bcm.unit == 0);
  CHECK(cmdDbBcm(db.data(), db.size(), "SN9", &address, bcm) == -2);
  put16(144 + 0x100 + 20, 0x4000);  // aux past the copy: no aux
  uint64_t at = 1;
  uint32_t length = 1;
  CHECK(cmdDbEntry(db.data(), db.size(), "SN4", &address, &at, &length) == 0 && at == 0 && length == 0);
  CHECK(cmdDbBcm(db.data(), db.size(), "SN4", &address, bcm) == -4);

  // The vote: Gen2 x2 (1 000 000 kB/s) over a bus 8 bytes wide, aux width 16, unit 1000: 2 000 000 x 1000 / 1000, clamped.
  CHECK(pcieLaneKbps(1) == 250000 && pcieLaneKbps(2) == 500000 && pcieLaneKbps(3) == 984500);
  CHECK(bcmVote(2 * pcieLaneKbps(2), 1000, 16, kNocBusWidth) == kBcmVoteMask);
  CHECK(bcmVote(2 * pcieLaneKbps(2), 2000000, 4, kNocBusWidth) == 250);   // 500 000 x 1000 / 2 000 000
  CHECK(bcmVote(1, 0xffffffffu, 1, kNocBusWidth) == 1);                     // a small vote is never lost
  CHECK(bcmVote(0, 1000, 16, kNocBusWidth) == 0 && bcmVote(1000, 0, 16, kNocBusWidth) == 0);
  CHECK(bcmCommand(0, 250) == (kBcmCommit | kBcmValid | 250u));
  CHECK(bcmCommand(0, 0) == kBcmCommit);                                     // the vote withdrawn: commit, not valid
  CHECK(bcmCommand(0x5000, 0x5000) == (kBcmCommit | kBcmValid | kBcmVoteMask << kBcmVoteXShift | kBcmVoteMask));
  CHECK(!(bcmCommand(kBcmVoteMask, kBcmVoteMask) & 0x90000000u));            // bits 31 and 28 clear (PianoA830's BCM rule)

  put32(4, 0);
  CHECK(cmdDbFind(db.data(), db.size(), "ldof3", &address) == -1);
}

// ------------------------------------------------------------------------------------------------------- the SPMI arbiter model
struct Arb {
  std::map<uint32_t, uint32_t> core, cnfg, obsv;
  std::map<uint32_t, uint8_t> regs;   // PPID << 8 | register
  std::map<uint32_t, uint32_t> apidPpid;
  unsigned commands = 0, otherWrites = 0, next = 10;
  Arb()
  {
    core[0] = 0x70010000;
    core[4] = 500;
  }
  void place(uint32_t ppid, uint8_t type, uint8_t subtype)
  {
    const uint32_t apid = next++;
    core[kSpmiApidMap + 4 * apid] = 1u << 31 | ppid << 8;
    cnfg[4 * apid] = 0;
    apidPpid[apid] = ppid;
    regs[ppid << 8 | 0x04] = type;
    regs[ppid << 8 | 0x05] = subtype;
  }
  void reg(uint32_t ppid, uint8_t r, uint8_t v) { regs[ppid << 8 | r] = v; }
  static uint32_t rd(void *c, Window w, uint32_t o)
  {
    Arb &a = *static_cast<Arb *>(c);
    return (w == Window::core ? a.core : w == Window::cnfg ? a.cnfg : a.obsv)[o];
  }
  static void wr(void *c, Window w, uint32_t o, uint32_t v)
  {
    Arb &a = *static_cast<Arb *>(c);
    if (w != Window::obsv || o % kObsvStride != kObsvCmd) { ++a.otherWrites; return; }
    ++a.commands;
    const uint32_t apid = o / kObsvStride, opcode = v >> 27, reg = (v >> 4) & 0xff, count = (v & 7) + 1;
    CHECK(opcode == kOpExtReadL);
    uint32_t lo = 0, hi = 0;
    for (uint32_t b = 0; b < count; ++b) {
      const uint32_t byte = a.regs[a.apidPpid[apid] << 8 | (reg + b)];
      (b < 4 ? lo : hi) |= byte << (8 * (b & 3));
    }
    a.obsv[o + kObsvStatus] = kStatusDone;
    a.obsv[o + kObsvData0] = lo;
    a.obsv[o + kObsvData1] = hi;
  }
  static void delay(void *, unsigned) {}
  Io io() { return {this, rd, wr, delay}; }
};

static void testPmic()
{
  Arb m;
  // ldof3 at 0xc3 (the SM8750 rule) on, 1800 mV, ready; ldof2 only at the SM8550 guess 0x41; smpd4 a SMPS, off
  m.place(0x5c3, 0x04, 0x52);
  m.reg(0x5c3, 0x08, 0x80);
  m.reg(0x5c3, 0x40, 1800 & 0xff);
  m.reg(0x5c3, 0x41, 1800 >> 8);
  m.reg(0x5c3, 0x46, 0x80);
  m.place(0x541, 0x04, 0x52);
  m.reg(0x541, 0x40, 1200 & 0xff);
  m.reg(0x541, 0x41, 1200 >> 8);
  m.place(0x3a4, 0x22, 0x10);
  m.reg(0x3a4, 0x40, 500 & 0xff);
  m.reg(0x3a4, 0x41, 500 >> 8);
  m.place(0x58a, 0x10, 0x11);   // BT_EN pin
  m.reg(0x58a, 0x46, 0x80);
  m.place(0x8ad, 0x09, 0x00);   // something that is not a regulator where smpi7 should be
  PmicSurvey s;
  surveyPmic(m.io(), s);
  CHECK(s.arbiter.v7);
  CHECK(s.rails[0].chosen == 0 && s.rails[0].reg()->read && s.rails[0].reg()->millivolts() == 1800);
  CHECK(s.rails[0].reg()->enabled() && s.rails[0].reg()->ready());
  CHECK(s.rails[1].chosen == 1 && s.rails[1].reg()->ppid == 0x541 && s.rails[1].reg()->millivolts() == 1200 && !s.rails[1].reg()->enabled());
  CHECK(s.rails[3].chosen == 0 && s.rails[3].reg()->ppid == 0x3a4 && s.rails[3].reg()->millivolts() == 500);
  CHECK(s.rails[4].chosen == -1 && s.rails[4].primary.present && s.rails[4].primary.type == 0x09 && !s.rails[4].reg());
  CHECK(s.rails[2].chosen == -1 && !s.rails[2].primary.present);
  CHECK(s.pins[0].read && (s.pins[0].regs40[6] & 0x80) && !s.pins[1].present);
  CHECK(m.otherWrites == 0 && m.commands == s.commands);   // the only stores are observer read commands
  Arb old;
  old.core[0] = 0x50000000;
  PmicSurvey t;
  surveyPmic(old.io(), t);
  CHECK(!t.arbiter.v7 && t.commands == 0 && old.commands == 0);
}

int main()
{
  testHandoverOff();
  testGates();
  testLinkUp();
  testSmmu();
  testCmdDb();
  testPmic();
  if (failures) {
    std::printf("wifi_host: %d failures\n", failures);
    return 1;
  }
  std::printf("wifi_host: all passed\n");
  return 0;
}
