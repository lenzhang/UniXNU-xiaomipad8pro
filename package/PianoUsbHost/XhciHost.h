// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoUsbHost level 5 (docs/drivers/usb-host.md section 8, H5): the DWC3 switched to host mode and our own minimal xHCI probe on
// it: halt, HCRST, a command ring and one event ring, a No-Op command that must complete with the right TRB pointer, the root
// ports' power and connection state, an optional USB 2 port reset; and the way back to device mode. Portable (host tests in
// tests/xhcihost_host.cpp against a register model of the core, the glue and the xHCI): every access goes through the Hal.
// The specification is behaviour only: Linux drivers/usb/dwc3/{core.c,drd.c,host.c,dwc3-qcom.c} and drivers/usb/host/xhci*.c
// (GPL-2.0, read, nothing copied) and the xHCI 1.2 specification's programming model; names and code are ours.
//
// The Hal (the kext's KernelHal, the tests' model):
//   uint32_t rd(uint64_t pa); void wr(uint64_t pa, uint32_t v)   32-bit MMIO; the kernel's wr is followed by a dsb
//   void delayUs(uint32_t us); void sleepMs(uint32_t ms)
//   bool glueClocked()     the USB30 GDSC and its four branch clocks (master, sleep, cfg_noc AXI, aggre AXI) re-read, all on: the
//                          gate of every QSCRATCH store (Ubuntu's K:7dffd78b342d: a VBUS-override store with the glue clocks off
//                          reset the SoC) and of the whole switch
//   uint32_t memRd(uint32_t off); void memWr(uint32_t off, uint32_t v)   the DMA region through its non-cacheable CPU mapping
//   void memBarrier()      TRB stores visible to the controller before a doorbell (dsb st); a cycle bit read before the rest (dmb)
//   uint64_t dmaBus()      the region's bus address (physical: stream 0x40 bypasses the SMMU, or translates through an identity map)
//
// Rules this file keeps (and the tests check): nothing is stored unless glueClocked() read true first; a QSCRATCH store only right
// after glueClocked() read true again; no xHCI register is stored before GCTL.PRTCAPDIR reads host; HCRST only while halted; every
// 64-bit register (DCBAAP, CRCR, ERSTBA, ERDP) high half first (Linux's write-64-hi-lo-quirk for every DWC3 host); a PORTSC store
// never carries PED (write-1-to-clear: it would disable the port) and clears only the change bits it means to; the rings stay inside
// the region, 64-byte aligned, the scratchpad pages page aligned, nothing crosses a 64 KiB boundary; no interrupt is enabled (USBCMD
// INTE and IMAN.IE stay 0, GEVNTSIZ masked: H5 polls, SPI 133 stays quiet).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "UsbHostSurvey.h"

namespace pianousbhost {

// ------------------------------------------------------------------------------------------------------------- helpers
// Poll pa until (value & mask) == want, every stepUs, for at most timeoutUs; *waited gets the time spent. True when it matched.
template <class Hal> bool waitBits(Hal &h, uint64_t pa, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t stepUs, uint32_t *waited)
{
  uint32_t t = 0;
  for (;;) {
    if ((h.rd(pa) & mask) == want) {
      if (waited) *waited = t;
      return true;
    }
    if (t >= timeoutUs) break;
    if (stepUs >= 1000) h.sleepMs(stepUs / 1000);
    else h.delayUs(stepUs);
    t += stepUs;
  }
  if (waited) *waited = t;
  return false;
}
// A 64-bit register as two 32-bit stores, the high half first (Linux's write-64-hi-lo-quirk, which every DWC3 host gets; SUN saw
// the same order in Xiaomi's stock kernel).
template <class Hal> void wr64(Hal &h, uint64_t pa, uint64_t v)
{
  h.wr(pa + 4, uint32_t(v >> 32));
  h.wr(pa, uint32_t(v));
}
template <class Hal> uint64_t rd64(Hal &h, uint64_t pa) { return uint64_t(h.rd(pa)) | uint64_t(h.rd(pa + 4)) << 32; }
inline bool utmiAsPipeBits(uint32_t general)
{
  return (general & pianohw::kQsPipeUtmiClkSel) && (general & pianohw::kQsPipe3PhystatusSw);
}

// ---------------------------------------------------------------------------------------------- DWC3: device -> host -> device
struct HostPolicy {
  unsigned coreSoftReset = 2;   // GCTL.CORESOFTRESET on the role change: 0 never, 1 always, 2 as Linux (DWC_usb3, DWC_usb31 < 1.90a)
  bool utmiAsPipe = true;       // QSCRATCH: the UTMI clock as the PIPE clock (high speed only; the QMP PHY stays off: H7)
  bool autoRetry = true;        // GUCTL.HSTINAUTORETRY (Linux sets it for host and OTG)
};
inline bool needsGctlSoftReset(uint32_t gsnpsid, uint32_t gversion)
{
  const uint32_t ip = gsnpsid >> 16;
  return ip == kIpDwc3 || (ip == kIpDwc31 && gversion < kDwc31Version190a);
}
inline bool knownIp(uint32_t gsnpsid)
{
  const uint32_t ip = gsnpsid >> 16;
  return ip == kIpDwc3 || ip == kIpDwc31 || ip == kIpDwc32;
}

struct CoreWords {
  uint32_t gctl = 0, gsts = 0, guctl = 0, gusb2 = 0, gusb3 = 0, gevntsiz = 0, dctl = 0, dsts = 0, qsGeneral = 0, qsHs = 0, qsSs = 0;
};
template <class Hal> CoreWords readCoreWords(Hal &h)
{
  using namespace pianohw;
  CoreWords w;
  w.gctl = h.rd(kDwc3 + kGctl);
  w.gsts = h.rd(kDwc3 + kGsts);
  w.guctl = h.rd(kDwc3 + kGuctl);
  w.gusb2 = h.rd(kDwc3 + kGusb2PhyCfg);
  w.gusb3 = h.rd(kDwc3 + kGusb3PipeCtl);
  w.gevntsiz = h.rd(kDwc3 + kGevntSiz);
  w.dctl = h.rd(kDwc3 + kDctl);
  w.dsts = h.rd(kDwc3 + kDsts);
  w.qsGeneral = h.rd(kQscratch + kQsGeneralCfg);
  w.qsHs = h.rd(kQscratch + kQsHsPhyCtrl);
  w.qsSs = h.rd(kQscratch + kQsSsPhyCtrl);
  return w;
}
constexpr uint32_t kOverrideHs = pianohw::kQsUtmiOtgVbusValid | pianohw::kQsSwSessvldSel, kOverrideSs = pianohw::kQsLane0PwrPresent;

struct SwitchResult {
  bool ok = false;
  const char *why = "";
  uint32_t gsnpsid = 0, gversion = 0;
  CoreWords found, now;
  bool wasRunning = false, devHalted = false, csftrst = false, gctlReset = false, overrideCleared = false, utmiSelected = false,
       hostMode = false;
  uint32_t haltUs = 0, csftrstUs = 0, curmodUs = 0;
  unsigned stores = 0, glueStores = 0;
};

// A QSCRATCH store behind its gate: the clocks re-read on right before it, or nothing is stored.
template <class Hal> bool glueStore(Hal &h, uint32_t off, uint32_t v, unsigned &stores, unsigned &glueStores)
{
  if (!h.glueClocked()) return false;
  h.wr(pianohw::kQscratch + off, v);
  ++stores;
  ++glueStores;
  return true;
}
// The UTMI clock as the PIPE clock (dwc3-qcom's sequence: the PIPE clock off, both selects, the PIPE clock on, 100 us each).
template <class Hal> bool selectUtmiAsPipe(Hal &h, unsigned &stores, unsigned &glueStores)
{
  using namespace pianohw;
  const uint64_t cfg = kQscratch + kQsGeneralCfg;
  if (!glueStore(h, kQsGeneralCfg, h.rd(cfg) | kQsPipeUtmiClkDis, stores, glueStores)) return false;
  h.delayUs(100);
  if (!glueStore(h, kQsGeneralCfg, h.rd(cfg) | kQsPipeUtmiClkSel | kQsPipe3PhystatusSw, stores, glueStores)) return false;
  h.delayUs(100);
  if (!glueStore(h, kQsGeneralCfg, h.rd(cfg) & ~kQsPipeUtmiClkDis, stores, glueStores)) return false;
  return true;
}

// Device (the firmware's, or PianoDwc3's before a timed hand-off) -> host. Order: the device controller stopped and its event
// interrupt masked; the glue (VBUS/session override off: the host role; the UTMI clock as PIPE clock, before the soft reset so the
// PIPE domain has a clock); the device-side soft reset (DCTL.CSFTRST, as Linux's core init from a non-host role); GCTL.CORESOFTRESET
// by the policy; the PHY suspend bits off; PRTCAPDIR = host and HSTINAUTORETRY; GSTS.CURMOD read back as host.
template <class Hal> SwitchResult hostSwitch(Hal &h, const HostPolicy &policy)
{
  using namespace pianohw;
  SwitchResult r;
  auto rd = [&](uint32_t off) { return h.rd(kDwc3 + off); };
  auto wr = [&](uint32_t off, uint32_t v) {
    h.wr(kDwc3 + off, v);
    ++r.stores;
  };
  if (!h.glueClocked()) {
    r.why = "the core is not powered/clocked: nothing written";
    return r;
  }
  r.gsnpsid = rd(kGsnpsid);
  r.gversion = rd(kGversion);
  if (!knownIp(r.gsnpsid)) {
    r.why = "GSNPSID is not a DWC_usb3x core's: nothing written";
    return r;
  }
  r.found = readCoreWords(h);
  const uint32_t mode = prtcap(r.found.gctl);
  if (mode != kPrtcapHost) {
    wr(kGevntSiz, r.found.gevntsiz | kGevntSizIntMask);
    const uint32_t dctl = r.found.dctl & ~kDctlUlstChngReqMask;
    r.wasRunning = (dctl & kDctlRunStop) != 0;
    if (r.wasRunning) {
      wr(kDctl, dctl & ~kDctlRunStop);
      r.devHalted = waitBits(h, kDwc3 + kDsts, kDstsDevCtrlHlt, kDstsDevCtrlHlt, 100000, 100, &r.haltUs);   // Linux goes on regardless
    } else {
      r.devHalted = (rd(kDsts) & kDstsDevCtrlHlt) != 0;
    }
  }
  if ((r.found.qsHs & kOverrideHs) || (r.found.qsSs & kOverrideSs)) {
    if (!glueStore(h, kQsSsPhyCtrl, h.rd(kQscratch + kQsSsPhyCtrl) & ~kOverrideSs, r.stores, r.glueStores) ||
        !glueStore(h, kQsHsPhyCtrl, h.rd(kQscratch + kQsHsPhyCtrl) & ~kOverrideHs, r.stores, r.glueStores)) {
      r.why = "the glue clocks read off before a QSCRATCH store: not written (K:7dffd78b342d)";
      r.now = readCoreWords(h);
      return r;
    }
    r.overrideCleared = true;
  }
  if (policy.utmiAsPipe && !utmiAsPipeBits(r.found.qsGeneral)) {
    if (!selectUtmiAsPipe(h, r.stores, r.glueStores)) {
      r.why = "the glue clocks read off before a QSCRATCH store: not written (K:7dffd78b342d)";
      r.now = readCoreWords(h);
      return r;
    }
    r.utmiSelected = true;
  }
  if (mode != kPrtcapHost) {
    wr(kDctl, (rd(kDctl) & ~(kDctlRunStop | kDctlUlstChngReqMask)) | kDctlCsftrst);
    r.csftrst = waitBits(h, kDwc3 + kDctl, kDctlCsftrst, 0, 250000, 1000, &r.csftrstUs);
    if (!r.csftrst) {
      r.why = "DCTL.CSFTRST did not clear within 250 ms (no PIPE clock?)";
      r.now = readCoreWords(h);
      return r;
    }
    h.sleepMs(50);   // the PHY-domain synchronisation DWC_usb31 1.80a and before need after the soft reset; harmless later
  }
  if (policy.coreSoftReset == 1 || (policy.coreSoftReset == 2 && mode != kPrtcapHost && needsGctlSoftReset(r.gsnpsid, r.gversion))) {
    wr(kGctl, rd(kGctl) | kGctlCoreSoftReset);
    h.sleepMs(100);
    wr(kGctl, rd(kGctl) & ~kGctlCoreSoftReset);
    r.gctlReset = true;
  }
  wr(kGusb2PhyCfg, rd(kGusb2PhyCfg) & ~(kGusb2PhySusphy | kGusb2PhyEnblslpm));
  wr(kGusb3PipeCtl, rd(kGusb3PipeCtl) & ~kGusb3PipeSusphy);
  wr(kGctl, (rd(kGctl) & ~kGctlPrtcapMask) | kPrtcapHost << kGctlPrtcapShift);
  if (policy.autoRetry) wr(kGuctl, rd(kGuctl) | kGuctlHstInAutoRetry);
  r.hostMode = waitBits(h, kDwc3 + kGsts, kGstsCurmodMask, 1, 10000, 100, &r.curmodUs);
  r.now = readCoreWords(h);
  if (prtcap(r.now.gctl) != kPrtcapHost) {
    r.why = "GCTL.PRTCAPDIR does not read host";
    return r;
  }
  if (!r.hostMode) {
    r.why = "GSTS.CURMOD does not read host within 10 ms";
    return r;
  }
  r.ok = true;
  return r;
}

struct BackResult {
  bool ok = false;
  const char *why = "";
  bool gctlReset = false, overrideRestored = false;
  CoreWords now;
  unsigned stores = 0, glueStores = 0;
};
// Host -> device, the xHCI halted first (xhciStop): GCTL.CORESOFTRESET by the policy, PRTCAPDIR = device, the PHY and GUCTL words
// and the VBUS/session override as the switch found them (the glue store behind its gate). The UTMI-as-PIPE selection stays
// (PianoDwc3 selects it for its device mode too). PianoDwc3's takeover then resets and programs the device side itself.
template <class Hal> BackResult backToDevice(Hal &h, const SwitchResult &sw, const HostPolicy &policy)
{
  using namespace pianohw;
  BackResult b;
  auto rd = [&](uint32_t off) { return h.rd(kDwc3 + off); };
  auto wr = [&](uint32_t off, uint32_t v) {
    h.wr(kDwc3 + off, v);
    ++b.stores;
  };
  if (!h.glueClocked()) {
    b.why = "the core is not powered/clocked: nothing written";
    return b;
  }
  if (policy.coreSoftReset == 1 || (policy.coreSoftReset == 2 && needsGctlSoftReset(sw.gsnpsid, sw.gversion))) {
    wr(kGctl, rd(kGctl) | kGctlCoreSoftReset);
    h.sleepMs(100);
    wr(kGctl, rd(kGctl) & ~kGctlCoreSoftReset);
    b.gctlReset = true;
  }
  const uint32_t back = prtcap(sw.found.gctl) == kPrtcapHost ? kPrtcapDevice : prtcap(sw.found.gctl);
  wr(kGctl, (rd(kGctl) & ~kGctlPrtcapMask) | back << kGctlPrtcapShift);
  wr(kGusb2PhyCfg, sw.found.gusb2);
  wr(kGusb3PipeCtl, sw.found.gusb3);
  wr(kGuctl, sw.found.guctl);
  if (sw.overrideCleared) {
    if (!glueStore(h, kQsSsPhyCtrl, h.rd(kQscratch + kQsSsPhyCtrl) | (sw.found.qsSs & kOverrideSs), b.stores, b.glueStores) ||
        !glueStore(h, kQsHsPhyCtrl, h.rd(kQscratch + kQsHsPhyCtrl) | (sw.found.qsHs & kOverrideHs), b.stores, b.glueStores)) {
      b.why = "the glue clocks read off before a QSCRATCH store: the override is not restored (K:7dffd78b342d)";
      b.now = readCoreWords(h);
      return b;
    }
    b.overrideRestored = true;
  }
  b.now = readCoreWords(h);
  b.ok = prtcap(b.now.gctl) == back;
  if (!b.ok) b.why = "GCTL.PRTCAPDIR does not read the device role";
  return b;
}

// ---------------------------------------------------------------------------------------------------------- the xHCI probe
struct XhciAt {
  uint64_t op = 0, rt = 0, db = 0;
};
inline XhciAt xhciAt(const Xhci &x) { return {kXhci + x.capLength, kXhci + x.rtsOff, kXhci + x.dbOff}; }

// The capability words, loaded and checked (the survey's decoder).
template <class Hal> Xhci readCaps(Hal &h)
{
  Xhci x;
  for (unsigned i = 0; i < 8; ++i) x.cap[i] = h.rd(kXhci + 4 * i);
  x.caps = true;
  decodeCaps(x);
  return x;
}

// The DMA region: DCBAA, command ring, event ring segment, ERST, scratchpad array, scratchpad pages (offsets; 64 KiB aligned
// region, so nothing below 0x10000 crosses a 64 KiB boundary and every scratchpad page is page aligned).
struct RingLayout {
  uint32_t dcbaa = 0x0000, cmd = 0x1000, evt = 0x2000, erst = 0x3000, spArray = 0x3040, spPages = 0x4000;
  uint32_t cmdTrbs = 64, evtTrbs = 256;   // 1 KiB and 4 KiB
  uint32_t slots = 0, scratchpads = 0, bytes = 0;
  const char *error = nullptr;
};
constexpr uint32_t kMaxScratchpads = 256;   // the array's room (0x3040..0x3840)
constexpr uint32_t kRegionAlign = 0x10000;
inline RingLayout ringLayout(const Xhci &x, unsigned wantSlots)
{
  RingLayout l;
  if (!x.sane) {
    l.error = "the capability words are not an xHCI's";
    return l;
  }
  l.slots = x.maxSlots < wantSlots ? x.maxSlots : wantSlots;
  if (!l.slots) {
    l.error = "the xHCI has no device slots";
    return l;
  }
  l.scratchpads = hcsScratchpads(x.cap[2]);
  if (l.scratchpads > kMaxScratchpads) {
    l.error = "the xHCI asks for more scratchpad buffers than the region holds";
    return l;
  }
  l.bytes = l.spPages + l.scratchpads * 0x1000;
  return l;
}

struct Rings {
  RingLayout l;
  uint64_t bus = 0;
  unsigned cmdEnq = 0, evtDeq = 0;
  bool cmdCycle = true, evtCycle = true;
};

struct ResetResult {
  bool ok = false;
  const char *why = "";
  uint32_t usbcmd = 0, usbsts = 0, cnrUs = 0, haltUs = 0, resetUs = 0;
  bool wasRunning = false;
  unsigned stores = 0;
};
// Halt (RS 0, HCHalted within 100 ms; the specification allows 16 ms) and HCRST (the bit and CNR clear within 1 s each).
template <class Hal> ResetResult xhciReset(Hal &h, const Xhci &x)
{
  ResetResult r;
  const XhciAt at = xhciAt(x);
  r.usbcmd = h.rd(at.op + kOpUsbCmd);
  r.usbsts = h.rd(at.op + kOpUsbSts);
  if (!waitBits(h, at.op + kOpUsbSts, kUsbStsCnr, 0, 1000000, 1000, &r.cnrUs)) {
    r.why = "USBSTS.CNR stays set (the controller is not ready)";
    return r;
  }
  if (!(h.rd(at.op + kOpUsbSts) & kUsbStsHalted)) {
    r.wasRunning = true;
    h.wr(at.op + kOpUsbCmd, h.rd(at.op + kOpUsbCmd) & ~(kUsbCmdRun | kUsbCmdInte));
    ++r.stores;
    if (!waitBits(h, at.op + kOpUsbSts, kUsbStsHalted, kUsbStsHalted, 100000, 100, &r.haltUs)) {
      r.why = "the controller does not halt within 100 ms";
      return r;
    }
  }
  h.wr(at.op + kOpUsbCmd, (h.rd(at.op + kOpUsbCmd) & ~(kUsbCmdRun | kUsbCmdInte)) | kUsbCmdHcrst);
  ++r.stores;
  h.delayUs(1000);   // no register load right after HCRST (Linux waits on some controllers; harmless here)
  if (!waitBits(h, at.op + kOpUsbCmd, kUsbCmdHcrst, 0, 1000000, 1000, &r.resetUs)) {
    r.why = "USBCMD.HCRST does not clear within 1 s";
    return r;
  }
  uint32_t cnr = 0;
  if (!waitBits(h, at.op + kOpUsbSts, kUsbStsCnr, 0, 1000000, 1000, &cnr)) {
    r.why = "USBSTS.CNR stays set after HCRST";
    return r;
  }
  r.resetUs += cnr;
  r.usbcmd = h.rd(at.op + kOpUsbCmd);
  r.usbsts = h.rd(at.op + kOpUsbSts);
  r.ok = true;
  return r;
}

struct InitResult {
  bool ok = false;
  const char *why = "";
  uint32_t pageSize = 0, config = 0, usbsts = 0;
  uint64_t dcbaap = 0, crcr = 0, erstba = 0, erdp = 0;   // read back (CRCR's pointer reads as 0 by the specification)
  unsigned stores = 0;
};
// The data structures and the registers that point at them, the controller halted after HCRST. No interrupt is enabled.
template <class Hal> InitResult xhciInit(Hal &h, const Xhci &x, Rings &r)
{
  InitResult res;
  const XhciAt at = xhciAt(x);
  const RingLayout &l = r.l;
  res.pageSize = h.rd(at.op + kOpPageSize);
  if (!(res.pageSize & 1u)) {
    res.why = "the xHCI does not offer 4 KiB pages";
    return res;
  }
  if (l.error || !l.bytes || (r.bus & (kRegionAlign - 1))) {
    res.why = l.error ? l.error : "the DMA region is not 64 KiB aligned";
    return res;
  }
  for (uint32_t off = 0; off < l.spPages; off += 4) h.memWr(off, 0);
  for (uint32_t i = 0; i < l.scratchpads; ++i) {
    const uint64_t page = r.bus + l.spPages + uint64_t(i) * 0x1000;
    h.memWr(l.spArray + 8 * i, uint32_t(page));
    h.memWr(l.spArray + 8 * i + 4, uint32_t(page >> 32));
  }
  if (l.scratchpads) {
    h.memWr(l.dcbaa, uint32_t(r.bus + l.spArray));
    h.memWr(l.dcbaa + 4, uint32_t((r.bus + l.spArray) >> 32));
  }
  // The command ring's link TRB back to its start, toggle cycle; its cycle bit is given when the producer reaches it.
  const uint32_t link = l.cmd + 16 * (l.cmdTrbs - 1);
  h.memWr(link, uint32_t(r.bus + l.cmd));
  h.memWr(link + 4, uint32_t((r.bus + l.cmd) >> 32));
  h.memWr(link + 8, 0);
  h.memWr(link + 12, trbTypeField(kTrbLink) | kTrbToggle);
  // One event ring segment.
  h.memWr(l.erst, uint32_t(r.bus + l.evt));
  h.memWr(l.erst + 4, uint32_t((r.bus + l.evt) >> 32));
  h.memWr(l.erst + 8, l.evtTrbs);
  h.memWr(l.erst + 12, 0);
  h.memBarrier();
  r.cmdEnq = r.evtDeq = 0;
  r.cmdCycle = r.evtCycle = true;
  auto wr = [&](uint64_t pa, uint32_t v) {
    h.wr(pa, v);
    ++res.stores;
  };
  wr(at.op + kOpConfig, (h.rd(at.op + kOpConfig) & ~0xffu) | l.slots);
  wr64(h, at.op + kOpDcbaap, r.bus + l.dcbaa);
  wr64(h, at.op + kOpCrcr, (r.bus + l.cmd) | kCrcrRcs);
  const uint64_t ir = at.rt + kRtIr0;
  wr(ir + kIrIman, kImanIp);   // a pending flag cleared, IE off
  wr(ir + kIrErstsz, 1);
  wr64(h, ir + kIrErdp, r.bus + l.evt);
  wr64(h, ir + kIrErstba, r.bus + l.erst);
  res.stores += 8;   // the four 64-bit registers' halves
  res.config = h.rd(at.op + kOpConfig);
  res.dcbaap = rd64(h, at.op + kOpDcbaap);
  res.crcr = rd64(h, at.op + kOpCrcr);
  res.erstba = rd64(h, ir + kIrErstba);
  res.erdp = rd64(h, ir + kIrErdp);
  res.usbsts = h.rd(at.op + kOpUsbSts);
  if ((res.config & 0xffu) != l.slots || res.dcbaap != r.bus + l.dcbaa || (res.erstba & ~0x3full) != r.bus + l.erst) {
    res.why = "the xHCI does not read back CONFIG/DCBAAP/ERSTBA as written";
    return res;
  }
  res.ok = true;
  return res;
}

struct RunResult {
  bool ok = false;
  const char *why = "";
  uint32_t usbcmd = 0, usbsts = 0, waitedUs = 0;
};
// RS = 1 (no INTE: polled), HCHalted clear within 100 ms, no HSE/HCE.
template <class Hal> RunResult xhciRun(Hal &h, const Xhci &x)
{
  RunResult r;
  const XhciAt at = xhciAt(x);
  h.wr(at.op + kOpUsbCmd, (h.rd(at.op + kOpUsbCmd) & ~kUsbCmdInte) | kUsbCmdRun);
  const bool running = waitBits(h, at.op + kOpUsbSts, kUsbStsHalted, 0, 100000, 100, &r.waitedUs);
  r.usbcmd = h.rd(at.op + kOpUsbCmd);
  r.usbsts = h.rd(at.op + kOpUsbSts);
  if (!running) r.why = "USBSTS.HCHalted stays set after RS";
  else if (r.usbsts & kUsbStsHce) r.why = "USBSTS.HCE: host controller error";
  else if (r.usbsts & kUsbStsHse) r.why = "USBSTS.HSE: host system error (a DMA the bus refused: SMMU?)";
  else r.ok = true;
  return r;
}

// One command TRB on the ring (control word with the cycle bit stored last), then doorbell 0. Returns its offset in the region.
template <class Hal> uint32_t submitCommand(Hal &h, const Xhci &x, Rings &r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t control)
{
  const RingLayout &l = r.l;
  const uint32_t off = l.cmd + 16 * r.cmdEnq;
  h.memWr(off, d0);
  h.memWr(off + 4, d1);
  h.memWr(off + 8, d2);
  h.memBarrier();
  h.memWr(off + 12, (control & ~kTrbCycle) | (r.cmdCycle ? kTrbCycle : 0));
  if (++r.cmdEnq == l.cmdTrbs - 1) {   // the link: it gets the producer's cycle, the producer toggles
    const uint32_t link = l.cmd + 16 * (l.cmdTrbs - 1) + 12;
    h.memWr(link, trbTypeField(kTrbLink) | kTrbToggle | (r.cmdCycle ? kTrbCycle : 0));
    r.cmdEnq = 0;
    r.cmdCycle = !r.cmdCycle;
  }
  h.memBarrier();
  h.wr(xhciAt(x).db, 0);   // doorbell 0, target 0: the command ring
  return off;
}

struct Event {
  uint32_t d[4] = {};
  uint32_t type() const { return trbType(d[3]); }
  uint32_t code() const { return trbCompletion(d[2]); }
  uint64_t pointer() const { return uint64_t(d[0]) | uint64_t(d[1]) << 32; }
  uint32_t port() const { return d[0] >> 24; }   // port status change
  uint32_t slot() const { return d[3] >> 24; }   // command completion
};
// The next event if the controller has produced one (its cycle bit is the consumer's), advancing the dequeue index.
template <class Hal> bool nextEvent(Hal &h, Rings &r, Event &e)
{
  const RingLayout &l = r.l;
  const uint32_t off = l.evt + 16 * r.evtDeq;
  const uint32_t control = h.memRd(off + 12);
  if (((control & kTrbCycle) != 0) != r.evtCycle) return false;
  h.memBarrier();
  e.d[0] = h.memRd(off);
  e.d[1] = h.memRd(off + 4);
  e.d[2] = h.memRd(off + 8);
  e.d[3] = control;
  if (++r.evtDeq == l.evtTrbs) {
    r.evtDeq = 0;
    r.evtCycle = !r.evtCycle;
  }
  return true;
}
// ERDP to the consumer's position, EHB cleared (write-1).
template <class Hal> void ackEvents(Hal &h, const Xhci &x, const Rings &r)
{
  wr64(h, xhciAt(x).rt + kRtIr0 + kIrErdp, (r.bus + r.l.evt + 16 * r.evtDeq) | kErdpEhb);
}

struct NoopResult {
  bool ok = false;
  const char *why = "";
  uint64_t trb = 0;      // the No-Op TRB's bus address
  Event completion;
  unsigned other = 0;    // other events seen meanwhile (port status changes)
  uint32_t portEvents = 0;   // bit n: a port status change event for port n
  uint32_t waitedUs = 0, usbsts = 0;
};
// SUN's acceptance test: a No-Op command completes with completion code 1 and the No-Op TRB's address.
template <class Hal> NoopResult noop(Hal &h, const Xhci &x, Rings &r, uint32_t timeoutUs = 100000)
{
  NoopResult n;
  n.trb = r.bus + submitCommand(h, x, r, 0, 0, 0, trbTypeField(kTrbNoopCmd));
  bool done = false;
  for (;;) {
    Event e;
    while (!done && nextEvent(h, r, e)) {
      if (e.type() == kTrbCmdCompletion) {
        n.completion = e;
        done = true;
      } else {
        ++n.other;
        if (e.type() == kTrbPortStatusChange && e.port() < 32) n.portEvents |= 1u << e.port();
      }
    }
    if (done || n.waitedUs >= timeoutUs) break;
    h.delayUs(100);
    n.waitedUs += 100;
  }
  ackEvents(h, x, r);
  n.usbsts = h.rd(xhciAt(x).op + kOpUsbSts);
  if (!done) n.why = "no command completion event within the timeout";
  else if (n.completion.pointer() != n.trb) n.why = "the completion points at another TRB";
  else if (n.completion.code() != kCcSuccess) n.why = "the completion code is not 1 (success)";
  else n.ok = true;
  return n;
}
// Events that came in later (port status changes), drained and acknowledged; returns how many, bits of the ports in *ports.
template <class Hal> unsigned drainEvents(Hal &h, const Xhci &x, Rings &r, uint32_t *ports)
{
  unsigned count = 0;
  Event e;
  while (nextEvent(h, r, e)) {
    ++count;
    if (e.type() == kTrbPortStatusChange && e.port() < 32 && ports) *ports |= 1u << e.port();
  }
  if (count) ackEvents(h, x, r);
  return count;
}

// ------------------------------------------------------------------------------------------------------------- root ports
inline uint32_t portNeutral(uint32_t portsc) { return portsc & kPortNeutral; }
inline uint64_t portscAt(const Xhci &x, unsigned port /* 1-based */) { return xhciAt(x).op + kOpPortBase + kOpPortStride * (port - 1); }
// The protocol of a port from the survey's walk (0 unknown, else the major revision).
inline unsigned portMajor(const Xhci &x, unsigned port)
{
  for (unsigned i = 0; i < x.protocolCount; ++i) {
    const Protocol &p = x.protocols[i];
    if (p.portCount && port >= p.portOffset && port < unsigned(p.portOffset) + p.portCount) return p.major;
  }
  return 0;
}
struct PortsResult {
  unsigned count = 0, powered = 0;
  uint32_t before[16] = {}, after[16] = {};
};
// Each root port powered if the controller has port power switches (PPC) and the port is off; 20 ms for the power to settle.
template <class Hal> PortsResult portsUp(Hal &h, const Xhci &x)
{
  PortsResult p;
  p.count = x.maxPorts < 16 ? x.maxPorts : 16;
  for (unsigned i = 1; i <= p.count; ++i) {
    p.before[i - 1] = h.rd(portscAt(x, i));
    if (x.ppc && !(p.before[i - 1] & kPortPp)) {
      h.wr(portscAt(x, i), portNeutral(p.before[i - 1]) | kPortPp);
      ++p.powered;
    }
  }
  if (p.powered) h.sleepMs(20);
  for (unsigned i = 1; i <= p.count; ++i) p.after[i - 1] = h.rd(portscAt(x, i));
  return p;
}
// Acknowledge (clear) the change bits a PORTSC read shows, nothing else.
template <class Hal> void portAck(Hal &h, const Xhci &x, unsigned port, uint32_t portsc)
{
  if (portsc & kPortChangeBits) h.wr(portscAt(x, port), portNeutral(portsc) | (portsc & kPortChangeBits));
}
struct PortResetResult {
  bool ok = false;
  const char *why = "";
  uint32_t before = 0, after = 0, waitedMs = 0;
};
// A USB 2 port's reset (PR; the reset change within timeoutMs: 50 ms nominal), then enabled and its speed. USB 3 ports reset
// themselves on connection (warm reset is not this test's).
template <class Hal> PortResetResult portReset(Hal &h, const Xhci &x, unsigned port, uint32_t timeoutMs = 500)
{
  PortResetResult r;
  r.before = h.rd(portscAt(x, port));
  if (!(r.before & kPortCcs)) {
    r.why = "nothing connected";
    return r;
  }
  if (portMajor(x, port) != 2) {
    r.why = "not a USB 2 port";
    return r;
  }
  h.wr(portscAt(x, port), portNeutral(r.before) | kPortPr);
  for (;;) {
    r.after = h.rd(portscAt(x, port));
    if ((r.after & kPortPrc) && !(r.after & kPortPr)) break;
    if (r.waitedMs >= timeoutMs) {
      r.why = "no port reset change within the timeout";
      return r;
    }
    h.sleepMs(10);
    r.waitedMs += 10;
  }
  portAck(h, x, port, r.after);
  if (!(r.after & kPortPed)) r.why = "the port is not enabled after the reset";
  else r.ok = true;
  return r;
}

struct StopResult {
  bool ok = false;
  const char *why = "";
  uint32_t usbsts = 0;
};
// RS = 0, HCHalted, then HCRST: the controller forgets the rings before the region could be reused.
template <class Hal> StopResult xhciStop(Hal &h, const Xhci &x)
{
  StopResult s;
  const XhciAt at = xhciAt(x);
  h.wr(at.op + kOpUsbCmd, h.rd(at.op + kOpUsbCmd) & ~(kUsbCmdRun | kUsbCmdInte));
  if (!waitBits(h, at.op + kOpUsbSts, kUsbStsHalted, kUsbStsHalted, 100000, 100, nullptr)) {
    s.why = "the controller does not halt within 100 ms";
    s.usbsts = h.rd(at.op + kOpUsbSts);
    return s;
  }
  h.wr(at.op + kOpUsbCmd, kUsbCmdHcrst);
  h.delayUs(1000);
  if (!waitBits(h, at.op + kOpUsbCmd, kUsbCmdHcrst, 0, 1000000, 1000, nullptr)) {
    s.why = "USBCMD.HCRST does not clear within 1 s";
    s.usbsts = h.rd(at.op + kOpUsbSts);
    return s;
  }
  s.usbsts = h.rd(at.op + kOpUsbSts);
  s.ok = true;
  return s;
}

}  // namespace pianousbhost
