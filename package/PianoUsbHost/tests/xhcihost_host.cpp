// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoUsbHost's level 5 (XhciHost.h) against a register model of the DWC3 core, its QSCRATCH glue, the GCC gates and
// an xHCI that executes a command ring and produces events into its event ring in the model's DMA memory:
//   - the gates: nothing stored with the core unclocked; no QSCRATCH store unless the glue clocks read on right before it (a clock
//     that drops mid-switch stops the switch before the glue store: K:7dffd78b342d); no xHCI store before PRTCAPDIR reads host;
//   - the switch order (device halted, glue, DCTL.CSFTRST, GCTL soft reset by the policy, SUSPHY off, PRTCAPDIR host) and the way back
//     (PRTCAPDIR device, the PHY words and the VBUS/session override as found);
//   - the xHCI: HCRST only while halted, CNR waited for, every 64-bit register high half first, the rings inside the region and aligned,
//     scratchpads handed over, no interrupt enabled, a No-Op completing with code 1 and its TRB's address, the command ring's link
//     after a wrap, event ring wrap and ERDP, a wrong completion code and a silent controller reported;
//   - the ports: power switched on, a connection seen, a USB 2 port reset to enabled + high speed, PED never stored as 1.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoDwc3 xhcihost_host.cpp -o /tmp/xhcihost_host && /tmp/xhcihost_host
#include "XhciHost.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianousbhost;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

using namespace pianohw;

struct Model {
  // GCC gates
  bool gdsc = true, master = true, sleep = true, cfgNoc = true, aggre = true;
  int dropClockAfterStores = -1;   // the aggre clock goes off after this many stores (-1: never)
  // DWC3
  uint32_t gsnpsid = 0x33310000, gversion = 0x3230302a;   // DWC_usb31 2.00a: no GCTL soft reset by Linux's rule
  uint32_t gctl = kPrtcapDevice << kGctlPrtcapShift, gsts = 0, guctl = 0x0c808010, gusb2 = kGusb2PhySusphy | kGusb2PhyEnblslpm | 0x1400,
           gusb3 = kGusb3PipeSusphy | 0x0100000, gevntsiz = 0x1000, dctl = kDctlRunStop, dsts = 0;
  int csftrstReads = 0, haltReads = 0;
  bool pipeClock = true;   // false: DCTL.CSFTRST never clears (no PIPE clock)
  uint32_t qsGeneral = 0, qsHs = kQsUtmiOtgVbusValid | kQsSwSessvldSel | 0x10, qsSs = kQsLane0PwrPresent;
  // xHCI
  uint32_t cap[8] = {0x01100020, 0x02000140, 0x0, 0x0, 0x0220fe6d, 0x480, 0x440, 0x0};   // 64 slots, 1 intr, 2 ports, PPC 1
  uint32_t usbcmd = 0, usbsts = kUsbStsHalted | kUsbStsCnr, pagesize = 1, config = 0, dnctrl = 0;
  int cnrReads = 3, hcrstReads = 0;
  uint64_t crcr = 0, dcbaap = 0, erstba = 0, erdp = 0;
  uint32_t iman = 0, imod = 0, erstsz = 0;
  uint32_t portsc[2] = {0, 0};
  bool deviceOnPort1 = true;
  int prReads = 0;
  uint32_t completionCode = kCcSuccess;
  bool silent = false;   // the controller never answers a doorbell
  // command ring consumer and event ring producer
  uint64_t cmdDeq = 0;
  bool cmdCcs = true;
  uint64_t evtBase = 0;
  uint32_t evtSize = 0, evtEnq = 0;
  bool evtPcs = true;
  // the halves of 64-bit registers: the high half must come first
  std::map<uint64_t, bool> highWritten;
  // memory
  std::vector<uint8_t> mem;
  uint64_t bus = 0x0000000123450000ull;   // above 4 GiB: the high halves matter
  // records
  std::vector<std::string> violations;
  std::vector<std::pair<uint64_t, uint32_t>> stores;
  unsigned glueStores = 0, xhciStores = 0, doorbells = 0;
  uint64_t timeUs = 0;

  Model() : mem(0x4000 + 0x1000 * 4, 0) {}
  bool coreOn() const { return gdsc && master && cfgNoc && aggre; }
  bool glueOn() const { return coreOn() && sleep; }
  void violation(const std::string &s) { violations.push_back(s); }
  bool inMem(uint64_t a, uint64_t n) const { return a >= bus && a + n <= bus + mem.size(); }
  uint32_t mrd(uint64_t a)
  {
    if (!inMem(a, 4)) {
      violation("controller DMA read outside the region");
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, &mem[a - bus], 4);
    return v;
  }
  void mwr(uint64_t a, uint32_t v)
  {
    if (!inMem(a, 4)) {
      violation("controller DMA write outside the region");
      return;
    }
    std::memcpy(&mem[a - bus], &v, 4);
  }
  uint64_t op() const { return kXhci + (cap[0] & 0xff); }
  uint64_t rt() const { return kXhci + (cap[6] & ~0x1fu); }
  uint64_t db() const { return kXhci + (cap[5] & ~3u); }
  uint32_t gccWord(uint32_t off) const
  {
    if (off == kGccUsb30Gdsc) return gdsc ? kGdscPwrOn : 0;
    const bool on = off == kGccUsb30Master ? master : off == kGccUsb30Sleep ? sleep : off == kGccCfgNocUsb3Axi ? cfgNoc
                    : off == kGccAggreUsb3Axi ? aggre : true;
    return on ? kCbcrClkEnable : kCbcrClkOff;
  }
  void postEvent(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t control)
  {
    if (!evtSize) {
      violation("an event with no event ring");
      return;
    }
    const uint64_t at = evtBase + 16ull * evtEnq;
    mwr(at, d0);
    mwr(at + 4, d1);
    mwr(at + 8, d2);
    mwr(at + 12, (control & ~kTrbCycle) | (evtPcs ? kTrbCycle : 0));
    if (++evtEnq == evtSize) {
      evtEnq = 0;
      evtPcs = !evtPcs;
    }
    usbsts |= kUsbStsEint;
    iman |= kImanIp;
  }
  void doorbell()
  {
    ++doorbells;
    if (silent) return;
    if (usbsts & kUsbStsHalted) {
      violation("a doorbell while halted");
      return;
    }
    for (unsigned guard = 0; guard < 300; ++guard) {
      const uint32_t control = mrd(cmdDeq + 12);
      if (((control & kTrbCycle) != 0) != cmdCcs) return;
      const uint32_t type = trbType(control);
      if (type == kTrbLink) {
        const uint64_t next = uint64_t(mrd(cmdDeq)) | uint64_t(mrd(cmdDeq + 4)) << 32;
        if (control & kTrbToggle) cmdCcs = !cmdCcs;
        cmdDeq = next & ~0xfull;
        continue;
      }
      if (type == kTrbNoopCmd) postEvent(uint32_t(cmdDeq), uint32_t(cmdDeq >> 32), completionCode << 24, trbTypeField(kTrbCmdCompletion));
      else postEvent(uint32_t(cmdDeq), uint32_t(cmdDeq >> 32), uint32_t(kCcTrbError) << 24, trbTypeField(kTrbCmdCompletion));
      cmdDeq += 16;
    }
    violation("the command ring never ends");
  }
  void run()
  {
    if (!dcbaap || !evtSize || !cmdDeq) {
      usbsts |= kUsbStsHce;
      violation("RS with the data structures not set");
      return;
    }
    const uint32_t sp = hcsScratchpads(cap[2]);
    if (sp) {
      const uint64_t array = uint64_t(mrd(dcbaap)) | uint64_t(mrd(dcbaap + 4)) << 32;
      if (!inMem(array, 8ull * sp) || (array & 63)) violation("scratchpad array");
      for (uint32_t i = 0; i < sp; ++i) {
        const uint64_t page = uint64_t(mrd(array + 8 * i)) | uint64_t(mrd(array + 8 * i + 4)) << 32;
        if (!inMem(page, 0x1000) || (page & 0xfff)) violation("scratchpad page");
      }
    }
    usbsts &= ~kUsbStsHalted;
    // A device on port 1 shows its connection once the controller runs and the port has power.
    if (deviceOnPort1 && (portsc[0] & kPortPp) && !(portsc[0] & kPortCcs)) {
      portsc[0] |= kPortCcs | kPortCsc;
      postEvent(1u << 24, 0, kCcSuccess << 24, trbTypeField(kTrbPortStatusChange));
    }
  }
  void hcrst()
  {
    if (!(usbsts & kUsbStsHalted)) violation("HCRST while running");
    usbcmd = kUsbCmdHcrst;
    hcrstReads = 2;
    cnrReads = 2;
    usbsts |= kUsbStsCnr;
    crcr = dcbaap = erstba = erdp = 0;
    evtSize = 0;
    cmdDeq = 0;
    config = 0;
    iman = 0;
    erstsz = 0;
    portsc[0] = deviceOnPort1 ? 0 : 0;
    portsc[1] = 0;
  }
  void write64(uint64_t base, uint64_t pa, uint32_t v, uint64_t &reg)
  {
    if (pa == base + 4) {
      highWritten[base] = true;
      reg = (reg & 0xffffffffull) | uint64_t(v) << 32;
      return;
    }
    if (!highWritten[base]) violation("a 64-bit register's low half before its high half");
    highWritten[base] = false;
    reg = (reg & ~0xffffffffull) | v;
  }

  uint32_t rd(uint64_t pa)
  {
    if (pa >= kGcc && pa < kGcc + 0x100000) return gccWord(uint32_t(pa - kGcc));
    if (!coreOn()) violation("a core load with the core unclocked");
    if (pa == kDwc3 + kGsnpsid) return gsnpsid;
    if (pa == kDwc3 + kGversion) return gversion;
    if (pa == kDwc3 + kGctl) return gctl;
    if (pa == kDwc3 + kGsts) return gsts;
    if (pa == kDwc3 + kGuctl) return guctl;
    if (pa == kDwc3 + kGusb2PhyCfg) return gusb2;
    if (pa == kDwc3 + kGusb3PipeCtl) return gusb3;
    if (pa == kDwc3 + kGevntSiz) return gevntsiz;
    if (pa == kDwc3 + kDctl) {
      if ((dctl & kDctlCsftrst) && pipeClock && --csftrstReads <= 0) dctl &= ~kDctlCsftrst;
      return dctl;
    }
    if (pa == kDwc3 + kDsts) {
      if (!(dctl & kDctlRunStop) && --haltReads <= 0) dsts |= kDstsDevCtrlHlt;
      return dsts;
    }
    if (pa == kQscratch + kQsGeneralCfg) return qsGeneral;
    if (pa == kQscratch + kQsHsPhyCtrl) return qsHs;
    if (pa == kQscratch + kQsSsPhyCtrl) return qsSs;
    if (pa >= kXhci && pa < kXhci + 0x20) return cap[(pa - kXhci) / 4];
    const uint64_t o = op(), r = rt();
    if (pa == o + kOpUsbCmd) {
      if ((usbcmd & kUsbCmdHcrst) && --hcrstReads <= 0) usbcmd &= ~kUsbCmdHcrst;
      return usbcmd;
    }
    if (pa == o + kOpUsbSts) {
      if ((usbsts & kUsbStsCnr) && !(usbcmd & kUsbCmdHcrst) && --cnrReads <= 0) usbsts &= ~kUsbStsCnr;
      return usbsts;
    }
    if (pa == o + kOpPageSize) return pagesize;
    if (pa == o + kOpConfig) return config;
    if (pa == o + kOpDnCtrl) return dnctrl;
    if (pa == o + kOpCrcr) return uint32_t(crcr & kCrcrCrr);   // the pointer reads as 0
    if (pa == o + kOpCrcr + 4) return 0;
    if (pa == o + kOpDcbaap) return uint32_t(dcbaap);
    if (pa == o + kOpDcbaap + 4) return uint32_t(dcbaap >> 32);
    for (unsigned i = 0; i < 2; ++i)
      if (pa == o + kOpPortBase + kOpPortStride * i) {
        if ((portsc[i] & kPortPr) && --prReads <= 0) {
          portsc[i] = (portsc[i] & ~kPortPr & ~kPortSpeedMask) | kPortPrc | kPortPed | 3u << 10;
          postEvent((i + 1) << 24, 0, kCcSuccess << 24, trbTypeField(kTrbPortStatusChange));
        }
        return portsc[i];
      }
    if (pa == r + kRtIr0 + kIrIman) return iman;
    if (pa == r + kRtIr0 + kIrImod) return imod;
    if (pa == r + kRtIr0 + kIrErstsz) return erstsz;
    if (pa == r + kRtIr0 + kIrErstba) return uint32_t(erstba);
    if (pa == r + kRtIr0 + kIrErstba + 4) return uint32_t(erstba >> 32);
    if (pa == r + kRtIr0 + kIrErdp) return uint32_t(erdp);
    if (pa == r + kRtIr0 + kIrErdp + 4) return uint32_t(erdp >> 32);
    return 0;
  }
  void wr(uint64_t pa, uint32_t v)
  {
    stores.push_back({pa, v});
    if (dropClockAfterStores >= 0 && int(stores.size()) > dropClockAfterStores) aggre = false;
    if (!coreOn()) {
      violation("a store with the core unclocked");
      return;
    }
    if (pa >= kQscratch && pa < kQscratch + 0x100) {
      ++glueStores;
      if (!glueOn()) violation("a QSCRATCH store with the glue clocks off (K:7dffd78b342d)");
      if (pa == kQscratch + kQsGeneralCfg) qsGeneral = v;
      else if (pa == kQscratch + kQsHsPhyCtrl) qsHs = v;
      else if (pa == kQscratch + kQsSsPhyCtrl) qsSs = v;
      return;
    }
    if (pa == kDwc3 + kGctl) {
      gctl = v;
      gsts = (gsts & ~kGstsCurmodMask) | (prtcap(v) == kPrtcapHost ? 1u : 0u);
      return;
    }
    if (pa == kDwc3 + kGuctl) { guctl = v; return; }
    if (pa == kDwc3 + kGusb2PhyCfg) { gusb2 = v; return; }
    if (pa == kDwc3 + kGusb3PipeCtl) { gusb3 = v; return; }
    if (pa == kDwc3 + kGevntSiz) { gevntsiz = v; return; }
    if (pa == kDwc3 + kDctl) {
      if (v & kDctlUlstChngReqMask) violation("DCTL stored with a link state change request");
      if ((dctl & kDctlRunStop) && !(v & kDctlRunStop)) haltReads = 2;
      if (v & kDctlCsftrst) csftrstReads = 3;
      dctl = v;
      return;
    }
    if (pa >= kXhci && pa < kXhci + 0x8000) {
      ++xhciStores;
      if (prtcap(gctl) != kPrtcapHost) violation("an xHCI store with the core not in host mode");
      const uint64_t o = op(), r = rt();
      if (pa == o + kOpUsbCmd) {
        if (v & (kUsbCmdInte | kUsbCmdHsee)) violation("an interrupt enabled");
        if (v & kUsbCmdHcrst) {
          hcrst();
          return;
        }
        const bool start = (v & kUsbCmdRun) && !(usbcmd & kUsbCmdRun);
        usbcmd = v;
        if (start) run();
        if (!(v & kUsbCmdRun)) usbsts |= kUsbStsHalted;
        return;
      }
      if (pa == o + kOpConfig) { config = v; return; }
      if (pa == o + kOpCrcr || pa == o + kOpCrcr + 4) {
        write64(o + kOpCrcr, pa, v, crcr);
        if (pa == o + kOpCrcr) {
          cmdDeq = crcr & ~0x3full;
          cmdCcs = crcr & kCrcrRcs;
          if (cmdDeq & 63) violation("command ring not 64-byte aligned");
        }
        return;
      }
      if (pa == o + kOpDcbaap || pa == o + kOpDcbaap + 4) {
        write64(o + kOpDcbaap, pa, v, dcbaap);
        if (pa == o + kOpDcbaap && (dcbaap & 63)) violation("DCBAA not 64-byte aligned");
        return;
      }
      for (unsigned i = 0; i < 2; ++i)
        if (pa == o + kOpPortBase + kOpPortStride * i) {
          if (v & kPortPed) violation("PORTSC stored with PED (disables the port)");
          portsc[i] &= ~(v & kPortChangeBits);
          if (v & kPortPp) portsc[i] |= kPortPp;
          if (v & kPortPr) {
            portsc[i] |= kPortPr;
            prReads = 3;
          }
          return;
        }
      if (pa == r + kRtIr0 + kIrIman) {
        if (v & kImanIe) violation("IMAN.IE set");
        iman &= ~(v & kImanIp);
        return;
      }
      if (pa == r + kRtIr0 + kIrImod) { imod = v; return; }
      if (pa == r + kRtIr0 + kIrErstsz) { erstsz = v; return; }
      if (pa == r + kRtIr0 + kIrErstba || pa == r + kRtIr0 + kIrErstba + 4) {
        write64(r + kRtIr0 + kIrErstba, pa, v, erstba);
        if (pa == r + kRtIr0 + kIrErstba) {
          if (erstba & 63) violation("ERST not 64-byte aligned");
          if (!erstsz) violation("ERSTBA before ERSTSZ");
          evtBase = uint64_t(mrd(erstba)) | uint64_t(mrd(erstba + 4)) << 32;
          evtSize = mrd(erstba + 8) & 0xffff;
          evtEnq = 0;
          evtPcs = true;
          if (evtSize < 16 || (evtBase & 63) || !inMem(evtBase, 16ull * evtSize)) violation("event ring segment");
          if ((evtBase >> 16) != ((evtBase + 16ull * evtSize - 1) >> 16)) violation("event ring crosses 64 KiB");
        }
        return;
      }
      if (pa == r + kRtIr0 + kIrErdp || pa == r + kRtIr0 + kIrErdp + 4) {
        const uint64_t before = erdp;
        write64(r + kRtIr0 + kIrErdp, pa, v, erdp);
        if (pa == r + kRtIr0 + kIrErdp) erdp &= ~uint64_t(kErdpEhb);
        (void)before;
        return;
      }
      if (pa == db()) {
        if (v != 0) violation("doorbell 0 with a target");
        doorbell();
        return;
      }
      return;
    }
    violation("a store outside the core, the glue and the xHCI");
  }
};

struct TestHal {
  Model &m;
  uint32_t rd(uint64_t pa) { return m.rd(pa); }
  void wr(uint64_t pa, uint32_t v) { m.wr(pa, v); }
  void delayUs(uint32_t us) { m.timeUs += us; }
  void sleepMs(uint32_t ms) { m.timeUs += 1000ull * ms; }
  bool glueClocked()
  {
    const uint32_t g = m.rd(kGcc + kGccUsb30Gdsc);
    return (g & kGdscPwrOn) && !(m.rd(kGcc + kGccUsb30Master) & kCbcrClkOff) && !(m.rd(kGcc + kGccUsb30Sleep) & kCbcrClkOff) &&
           !(m.rd(kGcc + kGccCfgNocUsb3Axi) & kCbcrClkOff) && !(m.rd(kGcc + kGccAggreUsb3Axi) & kCbcrClkOff);
  }
  uint32_t memRd(uint32_t off)
  {
    if (off + 4 > m.mem.size()) {
      m.violation("CPU read outside the region");
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, &m.mem[off], 4);
    return v;
  }
  void memWr(uint32_t off, uint32_t v)
  {
    if (off + 4 > m.mem.size()) {
      m.violation("CPU write outside the region");
      return;
    }
    std::memcpy(&m.mem[off], &v, 4);
  }
  void memBarrier() {}
  uint64_t dmaBus() const { return m.bus; }
};

bool clean(const Model &m)
{
  for (const std::string &v : m.violations) std::printf("  violation: %s\n", v.c_str());
  return m.violations.empty();
}

// The whole level 5 path on a model; returns the pieces for the checks.
struct Run {
  SwitchResult sw;
  Xhci x;
  RingLayout l;
  ResetResult reset;
  InitResult init;
  RunResult run;
  NoopResult noop;
  PortsResult ports;
};
Run bringUp(Model &m, const HostPolicy &policy = {})
{
  TestHal h{m};
  Run r;
  r.sw = hostSwitch(h, policy);
  if (!r.sw.ok) return r;
  r.x = readCaps(h);
  r.l = ringLayout(r.x, 8);
  Rings rings;
  rings.l = r.l;
  rings.bus = m.bus;
  if (r.l.error || r.l.bytes > m.mem.size()) return r;
  r.reset = xhciReset(h, r.x);
  if (!r.reset.ok) return r;
  r.init = xhciInit(h, r.x, rings);
  if (!r.init.ok) return r;
  r.ports = portsUp(h, r.x);
  r.run = xhciRun(h, r.x);
  if (!r.run.ok) return r;
  r.noop = noop(h, r.x, rings);
  return r;
}

void testSwitch()
{
  // The firmware's device mode (RUN_STOP set, override on, SUSPHY set): to host.
  Model m;
  TestHal h{m};
  const SwitchResult sw = hostSwitch(h, HostPolicy{});
  CHECK(sw.ok);
  CHECK(sw.wasRunning && sw.devHalted && sw.csftrst && !sw.gctlReset && sw.overrideCleared && sw.utmiSelected && sw.hostMode);
  CHECK(prtcap(m.gctl) == kPrtcapHost && (m.gsts & 3) == 1);
  CHECK(!(m.gusb2 & (kGusb2PhySusphy | kGusb2PhyEnblslpm)) && (m.gusb2 & 0x1400) && !(m.gusb3 & kGusb3PipeSusphy));
  CHECK(m.guctl & kGuctlHstInAutoRetry);
  CHECK(!(m.qsHs & kOverrideHs) && (m.qsHs & 0x10) && !(m.qsSs & kOverrideSs));
  CHECK(utmiAsPipeBits(m.qsGeneral) && !(m.qsGeneral & kQsPipeUtmiClkDis));
  CHECK(m.gevntsiz & kGevntSizIntMask);
  CHECK(!(m.dctl & kDctlRunStop));
  CHECK(m.glueStores == 5 && sw.glueStores == 5);
  CHECK(m.xhciStores == 0);
  CHECK(clean(m));
  // Order: the glue before the device soft reset, PRTCAPDIR last among the core words but GUCTL.
  int glue = -1, csftrst = -1, prtcapAt = -1;
  for (size_t i = 0; i < m.stores.size(); ++i) {
    const auto &s = m.stores[i];
    if (s.first >= kQscratch && s.first < kQscratch + 0x100 && glue < 0) glue = int(i);
    if (s.first == kDwc3 + kDctl && (s.second & kDctlCsftrst)) csftrst = int(i);
    if (s.first == kDwc3 + kGctl && prtcap(s.second) == kPrtcapHost) prtcapAt = int(i);
  }
  CHECK(glue >= 0 && csftrst > glue && prtcapAt > csftrst);

  // Back to device: PRTCAPDIR device, the PHY words and the override as found.
  const BackResult b = backToDevice(h, sw, HostPolicy{});
  CHECK(b.ok && b.overrideRestored && !b.gctlReset);
  CHECK(prtcap(m.gctl) == kPrtcapDevice && m.gusb2 == sw.found.gusb2 && m.gusb3 == sw.found.gusb3 && m.guctl == sw.found.guctl);
  CHECK((m.qsHs & kOverrideHs) == kOverrideHs && (m.qsSs & kOverrideSs));
  CHECK(clean(m));

  // DWC_usb31 1.80a: the GCTL soft reset by Linux's rule, there and back; policy 0 never, 1 always.
  Model o;
  o.gversion = 0x3138302a;
  TestHal ho{o};
  const SwitchResult so = hostSwitch(ho, HostPolicy{});
  CHECK(so.ok && so.gctlReset && o.timeUs >= 150000);
  CHECK(backToDevice(ho, so, HostPolicy{}).gctlReset);
  Model p;
  p.gversion = 0x3138302a;
  TestHal hp{p};
  HostPolicy never;
  never.coreSoftReset = 0;
  CHECK(hostSwitch(hp, never).ok && !hostSwitch(hp, never).gctlReset);
  Model a;
  TestHal ha{a};
  HostPolicy always;
  always.coreSoftReset = 1;
  CHECK(hostSwitch(ha, always).gctlReset);
  CHECK(needsGctlSoftReset(0x5533290a, 0) && needsGctlSoftReset(0x33310000, 0x3138302a) && !needsGctlSoftReset(0x33310000, 0x3139302a) &&
        !needsGctlSoftReset(0x33320000, 0));
  // UTMI-as-PIPE already selected: no glue store for it; policy off: none either.
  Model u;
  u.qsGeneral = kQsPipeUtmiClkSel | kQsPipe3PhystatusSw;
  TestHal hu{u};
  CHECK(hostSwitch(hu, HostPolicy{}).ok && u.glueStores == 2);
  Model v;
  TestHal hv{v};
  HostPolicy noUtmi;
  noUtmi.utmiAsPipe = false;
  CHECK(hostSwitch(hv, noUtmi).ok && v.glueStores == 2 && !v.qsGeneral);
  // The firmware left the device controller stopped: no RUN_STOP store, the soft reset still.
  Model s;
  s.dctl = 0;
  s.dsts = kDstsDevCtrlHlt;
  TestHal hs{s};
  const SwitchResult ss = hostSwitch(hs, HostPolicy{});
  CHECK(ss.ok && !ss.wasRunning && ss.devHalted && ss.csftrst);
  CHECK(clean(s));
}

void testGates()
{
  // The core unclocked: nothing stored, nothing of the core loaded.
  Model m;
  m.master = false;
  TestHal h{m};
  const SwitchResult sw = hostSwitch(h, HostPolicy{});
  CHECK(!sw.ok && m.stores.empty() && std::strstr(sw.why, "not powered") != nullptr);
  CHECK(clean(m));
  // The sleep clock off (the glue's): the whole switch refuses (the gate reads all four branches).
  Model s;
  s.sleep = false;
  TestHal hs{s};
  CHECK(!hostSwitch(hs, HostPolicy{}).ok && s.stores.empty());
  // A clock that drops after the first stores: the switch stops before the glue store; the model sees no glue store off-clock.
  Model d;
  d.dropClockAfterStores = 2;   // GEVNTSIZ, DCTL (RUN_STOP off): then aggre off
  TestHal hd{d};
  const SwitchResult sd = hostSwitch(hd, HostPolicy{});
  CHECK(!sd.ok && std::strstr(sd.why, "K:7dffd78b342d") != nullptr);
  CHECK(d.glueStores == 0);
  bool offClockStore = false;
  for (const auto &v : d.violations) offClockStore |= v.find("QSCRATCH") != std::string::npos;
  CHECK(!offClockStore);
  // An unknown IP: nothing stored.
  Model i;
  i.gsnpsid = 0x12345678;
  TestHal hi{i};
  CHECK(!hostSwitch(hi, HostPolicy{}).ok && i.stores.empty());
  // No PIPE clock: the device soft reset never clears; reported, PRTCAPDIR not touched.
  Model n;
  n.pipeClock = false;
  TestHal hn{n};
  HostPolicy noUtmi;
  noUtmi.utmiAsPipe = false;
  const SwitchResult sn = hostSwitch(hn, noUtmi);
  CHECK(!sn.ok && std::strstr(sn.why, "CSFTRST") != nullptr && prtcap(n.gctl) == kPrtcapDevice && n.xhciStores == 0);
  // The back path with the clocks off: nothing.
  Model b;
  TestHal hb{b};
  const SwitchResult sb = hostSwitch(hb, HostPolicy{});
  b.cfgNoc = false;
  const size_t before = b.stores.size();
  CHECK(!backToDevice(hb, sb, HostPolicy{}).ok && b.stores.size() == before);
}

void testXhci()
{
  Model m;
  const Run r = bringUp(m);
  CHECK(r.sw.ok && r.x.sane && !r.l.error);
  CHECK(r.l.slots == 8 && r.l.scratchpads == 0 && r.l.bytes == 0x4000);
  CHECK(r.reset.ok && r.init.ok && r.run.ok);
  CHECK(r.init.dcbaap == m.bus && (r.init.config & 0xff) == 8 && r.init.erstba == m.bus + 0x3000);
  CHECK(r.noop.ok);
  CHECK(r.noop.trb == m.bus + 0x1000 && r.noop.completion.pointer() == r.noop.trb && r.noop.completion.code() == kCcSuccess);
  CHECK(r.noop.completion.type() == kTrbCmdCompletion);
  // The port status change of the device on port 1 came in before the completion.
  CHECK(r.noop.other == 1 && (r.noop.portEvents & 2));
  CHECK(r.ports.powered == 2 && (r.ports.after[0] & kPortPp) && (r.ports.after[1] & kPortPp));
  CHECK(m.erdp == m.bus + 0x2000 + 2 * 16);   // ERDP after the two events
  CHECK(m.doorbells == 1);
  CHECK(clean(m));

  // A USB 2 port reset on port 1 (the walk's protocol table: port 1 USB 2, port 2 USB 3).
  TestHal h{m};
  Xhci x = r.x;
  x.protocolCount = 2;
  x.protocols[0] = {2, 0, 1, 1, 0x20425355};
  x.protocols[1] = {3, 0x10, 2, 1, 0x20425355};
  CHECK(m.portsc[0] & kPortCcs);
  const PortResetResult pr = portReset(h, x, 1);
  CHECK(pr.ok && (pr.after & kPortPed) && portSpeed(pr.after) == 3);
  CHECK(!(m.portsc[0] & kPortPrc));   // acknowledged
  CHECK(!portReset(h, x, 2).ok);      // nothing connected / USB 3
  CHECK(portMajor(x, 1) == 2 && portMajor(x, 2) == 3 && portMajor(x, 3) == 0);
  Rings rings;
  rings.l = r.l;
  rings.bus = m.bus;
  rings.evtDeq = 2;
  uint32_t ports = 0;
  CHECK(drainEvents(h, x, rings, &ports) == 1 && (ports & 2));   // the reset's port status change
  CHECK(clean(m));
  // Stop: halted and reset; then back to device.
  const StopResult st = xhciStop(h, x);
  CHECK(st.ok && (m.usbsts & kUsbStsHalted) && !m.dcbaap);
  CHECK(backToDevice(h, r.sw, HostPolicy{}).ok && prtcap(m.gctl) == kPrtcapDevice);
  CHECK(clean(m));
}

void testXhciEdges()
{
  // Scratchpads: the array and the pages handed over through DCBAA[0].
  Model s;
  s.cap[2] = 2u << 27;   // two scratchpad buffers (Lo)
  const Run rs = bringUp(s);
  CHECK(rs.l.scratchpads == 2 && rs.l.bytes == 0x6000 && rs.noop.ok);
  CHECK(clean(s));
  // Too many scratchpads: refused before anything is allocated.
  Xhci many = rs.x;
  many.cap[2] = 0x1fu << 21 | 0x1fu << 27;
  CHECK(ringLayout(many, 8).error != nullptr);
  // A wrong completion code, a silent controller.
  Model c;
  c.completionCode = kCcTrbError;
  const Run rc = bringUp(c);
  CHECK(!rc.noop.ok && rc.noop.completion.code() == kCcTrbError && std::strstr(rc.noop.why, "code") != nullptr);
  Model q;
  q.silent = true;
  const Run rq = bringUp(q);
  CHECK(!rq.noop.ok && std::strstr(rq.noop.why, "no command completion") != nullptr && rq.noop.waitedUs >= 100000);
  // A controller running when the probe starts: halted before HCRST (the model flags HCRST while running).
  Model g;
  TestHal hg{g};
  CHECK(hostSwitch(hg, HostPolicy{}).ok);
  g.usbsts = 0;
  g.cnrReads = 0;
  g.usbcmd = kUsbCmdRun;
  const Xhci xg = readCaps(hg);
  const ResetResult rg = xhciReset(hg, xg);
  CHECK(rg.ok && rg.wasRunning);
  CHECK(clean(g));
  // CNR that never clears.
  Model n;
  TestHal hn{n};
  CHECK(hostSwitch(hn, HostPolicy{}).ok);
  n.cnrReads = 1 << 30;
  const ResetResult rn = xhciReset(hn, readCaps(hn));
  CHECK(!rn.ok && std::strstr(rn.why, "CNR") != nullptr && n.xhciStores == 0);
  // No 4 KiB page size; a region not 64 KiB aligned.
  Model p;
  p.pagesize = 2;
  const Run rp = bringUp(p);
  CHECK(rp.reset.ok && !rp.init.ok && std::strstr(rp.init.why, "4 KiB") != nullptr);
  Model a;
  a.bus += 0x1000;
  const Run ra = bringUp(a);
  CHECK(!ra.init.ok && std::strstr(ra.init.why, "aligned") != nullptr);
  // Insane capability words: no layout, nothing stored in the xHCI.
  Model z;
  for (uint32_t &w : z.cap) w = 0xffffffffu;
  const Run rz = bringUp(z);
  CHECK(rz.sw.ok && !rz.x.sane && rz.l.error && z.xhciStores == 0);
}

void testRings()
{
  // 70 No-Ops: the command ring wraps through its link TRB (63 usable entries), the consumer follows; the event ring's dequeue
  // index and cycle advance.
  Model m;
  m.deviceOnPort1 = false;
  TestHal h{m};
  CHECK(hostSwitch(h, HostPolicy{}).ok);
  const Xhci x = readCaps(h);
  Rings r;
  r.l = ringLayout(x, 8);
  r.bus = m.bus;
  CHECK(xhciReset(h, x).ok && xhciInit(h, x, r).ok && xhciRun(h, x).ok);
  bool all = true;
  for (unsigned i = 0; i < 70; ++i) all &= noop(h, x, r).ok;
  CHECK(all);
  CHECK(r.cmdEnq == 70 - 63 && !r.cmdCycle);
  CHECK(r.evtDeq == 70 && r.evtCycle);
  // 300 events wrap the event ring (256 entries): the consumer's cycle toggles.
  for (unsigned i = 0; i < 230; ++i) all &= noop(h, x, r).ok;
  CHECK(all && r.evtDeq == 300 - 256 && !r.evtCycle);
  CHECK(m.erdp == m.bus + 0x2000 + 16ull * r.evtDeq);
  CHECK(clean(m));
}

void testNeutral()
{
  // The PORTSC write-back mask: change bits, PED and PR/WPR never carried over.
  const uint32_t v = kPortCcs | kPortPed | kPortPp | 3u << 10 | 5u << 5 | kPortCsc | kPortPrc | kPortPr | kPortWpr | kPortWce;
  const uint32_t n = portNeutral(v);
  CHECK((n & kPortCcs) && (n & kPortPp) && (n & kPortWce) && portSpeed(n) == 3 && portLinkState(n) == 5);
  CHECK(!(n & (kPortPed | kPortCsc | kPortPrc | kPortPr | kPortWpr)));
  CHECK(hcsScratchpads(0x1fu << 27) == 31 && hcsScratchpads(1u << 21) == 32 && hcsErstMax(0x20) == 4);
}

}  // namespace

int main()
{
  testSwitch();
  testGates();
  testXhci();
  testXhciEdges();
  testRings();
  testNeutral();
  if (failures) {
    std::printf("xhcihost_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("xhcihost_host: all passed\n");
  return 0;
}
