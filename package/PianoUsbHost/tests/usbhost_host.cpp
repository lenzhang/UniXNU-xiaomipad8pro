// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoUsbHost's level 1 survey (UsbHostSurvey.h) against register models:
//   - what is loaded and when: the TLMM and GCC/TCSR words always; DWC3, QSCRATCH and the xHCI capability words only with the core
//     powered and clocked; the xHCI operational/port/extended-capability words only in host or OTG mode (or level 2); the QMP PHY
//     only with its power domain and clocks on; never a reserved GPIO, never outside the listed blocks;
//   - the xHCI decoders and the bounded extended-capability walk (sane and insane capability words, a pointer out of the window);
//   - the QMP DP_COM decode (mode, software lane select, orientation);
//   - the PMIC Type-C decode (a computer's Rp, an OTG device's Rd on CC2, an audio accessory, nothing) with Linux's bit order;
//   - the orientation witnesses and the role policy (PMIC only, UCSI over PMIC, docks).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoDwc3 usbhost_host.cpp -o /tmp/usbhost_host && /tmp/usbhost_host
#include "UsbHostSurvey.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace pianousbhost;
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
  void clear() { loads.clear(); }
  void coreClocks(bool on)
  {
    using namespace pianohw;
    regs[kGcc + kGccUsb30Gdsc] = on ? kGdscPwrOn : 0;
    for (uint32_t b : {kGccUsb30Master, kGccCfgNocUsb3Axi, kGccAggreUsb3Axi, kGccUsb30Sleep, kGccUsb30MockUtmi})
      regs[kGcc + b] = on ? kCbcrClkEnable : kCbcrClkOff;
  }
  void phyClocks(bool on)
  {
    using namespace pianohw;
    regs[kGcc + kGccUsb3PhyGdsc] = on ? kGdscPwrOn : 0;
    for (uint32_t b : {kGccUsb3PhyAux, kGccUsb3PhyComAux, kGccUsb3PhyPipe}) regs[kGcc + b] = on ? kCbcrClkEnable : kCbcrClkOff;
  }
  void gctl(uint32_t prtcapValue) { regs[pianohw::kDwc3 + 0xc110] = prtcapValue << 12; }
  // A DWC_usb31-like xHCI: caplength 0x20, version 1.10, 64 slots, 1 interrupter... 2 ports; protocol caps USB 2.0 port 1 and
  // USB 3.1 port 2, then a vendor capability, then the end.
  void xhci()
  {
    const uint64_t x = kXhci;
    regs[x + 0x00] = 0x01100020;
    regs[x + 0x04] = 0x02000140;   // MaxPorts 2, MaxIntrs 1, MaxSlots 64
    regs[x + 0x10] = 0x0220fe65;   // xECP 0x220 dwords (0x880), AC64 1, CSZ 1
    regs[x + 0x14] = 0x480;
    regs[x + 0x18] = 0x440;
    regs[x + 0x880] = 0x02000402;   // protocol 2.0, next 4 dwords
    regs[x + 0x884] = 0x20425355;
    regs[x + 0x888] = 0x00000101;   // ports 1..1
    regs[x + 0x890] = 0x03100402;   // protocol 3.1, next 4 dwords
    regs[x + 0x894] = 0x20425355;
    regs[x + 0x898] = 0x00000102;   // port 2
    regs[x + 0x8a0] = 0x000000c0;   // vendor, last
    regs[x + 0x20 + 0x04] = 0x1;    // USBSTS halted
    regs[x + 0x20 + 0x400] = 0x00000e03;   // port 1: connected, enabled, power, high speed
    regs[x + 0x20 + 0x410] = 0x000002a0;   // port 2: power, link state 5 (RxDetect)
  }
};
static auto loader(Soc &s)
{
  return [&s](uint64_t pa) { return s.load(pa); };
}

// Every address the survey may ever load: the blocks it names, nothing else.
static bool allowed(uint64_t pa)
{
  using namespace pianohw;
  const uint64_t tlmmPins[3] = {kGpioOrientation, kGpioMuxSda, kGpioMuxScl};
  for (uint64_t g : tlmmPins)
    if (pa == tlmmCfg(unsigned(g)) || pa == tlmmIo(unsigned(g))) return true;
  if (pa >= kGcc + 0x39000 && pa < kGcc + 0x3a000) return true;
  if (pa == kGcc + kGccUsb3PhyGdsc || pa == kTcsrUsb2ClkRef || pa == kTcsrUsb3ClkRef) return true;
  if (pa >= kXhci && pa < kXhci + kXhciBytes) return true;
  if (pa >= kDwc3 + 0xc100 && pa < kDwc3 + 0xd000) return true;
  if (pa >= kQscratch && pa < kQscratch + 0x40) return true;
  if (pa >= kQmp && pa < kQmp + kQmpBytes) return true;
  return false;
}
static bool allAllowed(const Soc &s)
{
  for (uint64_t pa : s.loads) {
    if (!allowed(pa)) {
      std::printf("  load outside the list: %#llx\n", (unsigned long long)pa);
      return false;
    }
    for (unsigned g = 0; g < 216; ++g)
      if (tlmmReserved(g) && (pa == tlmmCfg(g) || pa == tlmmIo(g))) return false;
  }
  return true;
}

static void testGates()
{
  // Everything off: pins and clocks only.
  Soc s;
  s.coreClocks(false);
  s.phyClocks(false);
  s.xhci();
  const Pins p = surveyPins(loader(s));
  const Clocks c = surveyClocks(loader(s));
  CHECK(!c.coreOn && !c.phyOn);
  const Core k = surveyCore(c, loader(s));
  const Xhci x = surveyXhci(c, k, 2, loader(s));
  const Qmp q = surveyQmp(c, true, loader(s));
  CHECK(!k.read && !x.caps && !q.read);
  CHECK(s.loads.size() == 6 + 12);
  CHECK(s.in(pianohw::kDwc3, 0x100000) == 0 && s.in(kQmp, kQmpBytes) == 0);
  CHECK(allAllowed(s));
  (void)p;

  // The core powered, one of its clocks off: still not loaded.
  Soc t;
  t.coreClocks(true);
  t.regs[pianohw::kGcc + pianohw::kGccAggreUsb3Axi] = pianohw::kCbcrClkOff;
  const Clocks c2 = surveyClocks(loader(t));
  CHECK(!c2.coreOn);
  t.clear();
  CHECK(!surveyCore(c2, loader(t)).read && t.loads.empty());

  // The core on, device mode: globals, glue and the 8 capability words; no operational/port/extended words at level 1.
  Soc d;
  d.coreClocks(true);
  d.phyClocks(false);
  d.gctl(kPrtcapDevice);
  d.xhci();
  const Clocks cd = surveyClocks(loader(d));
  CHECK(cd.coreOn && !cd.phyOn);
  d.clear();
  const Core kd = surveyCore(cd, loader(d));
  CHECK(kd.read && prtcap(kd.gctl) == kPrtcapDevice);
  CHECK(d.loads.size() == 13);
  d.clear();
  const Xhci xd = surveyXhci(cd, kd, 1, loader(d));
  CHECK(xd.caps && xd.sane && !xd.walked);
  CHECK(d.loads.size() == 8 && d.in(kXhci, 0x20) == 8);
  // Level 0: nothing.
  d.clear();
  CHECK(!surveyXhci(cd, kd, 0, loader(d)).caps && d.loads.empty());
  // Level 2: the walk in device mode too.
  d.clear();
  const Xhci x2 = surveyXhci(cd, kd, 2, loader(d));
  CHECK(x2.walked && x2.protocolCount == 2);
  CHECK(allAllowed(d));
  // The PHY off: no QMP load.
  d.clear();
  CHECK(!surveyQmp(cd, true, loader(d)).read && d.loads.empty());
}

static void testXhci()
{
  Soc s;
  s.coreClocks(true);
  s.gctl(kPrtcapHost);
  s.xhci();
  const Clocks c = surveyClocks(loader(s));
  const Core k = surveyCore(c, loader(s));
  CHECK(prtcap(k.gctl) == kPrtcapHost);
  s.clear();
  const Xhci x = surveyXhci(c, k, 1, loader(s));
  CHECK(x.sane && x.walked);
  CHECK(x.capLength == 0x20 && x.hciVersion == 0x110 && x.maxSlots == 64 && x.maxIntrs == 1 && x.maxPorts == 2);
  CHECK(x.ac64 && x.csz && x.xecp == 0x220 && x.dbOff == 0x480 && x.rtsOff == 0x440);
  CHECK(x.xcapCount == 3 && x.xcapIds[0] == kXcapProtocol && x.xcapIds[2] == 0xc0);
  CHECK(std::strcmp(xcapName(x.xcapIds[2]), "vendor") == 0);
  CHECK(x.protocolCount == 2);
  CHECK(x.protocols[0].major == 2 && x.protocols[0].minor == 0 && x.protocols[0].portOffset == 1 && x.protocols[0].portCount == 1);
  CHECK(x.protocols[1].major == 3 && x.protocols[1].minor == 0x10 && x.protocols[1].portOffset == 2);
  CHECK(x.protocols[0].name == 0x20425355);
  CHECK(x.portCount == 2);
  CHECK((x.portsc[0] & kPortCcs) && (x.portsc[0] & kPortPed) && portSpeed(x.portsc[0]) == 3);
  CHECK(std::strcmp(speedName(portSpeed(x.portsc[0])), "high") == 0);
  CHECK(!(x.portsc[1] & kPortCcs) && portLinkState(x.portsc[1]) == 5);
  CHECK(x.usbSts & kUsbStsHalted);
  CHECK(allAllowed(s));

  // OTG (DRD) counts as host for the walk.
  Soc o;
  o.coreClocks(true);
  o.gctl(kPrtcapOtg);
  o.xhci();
  const Clocks co = surveyClocks(loader(o));
  CHECK(surveyXhci(co, surveyCore(co, loader(o)), 1, loader(o)).walked);

  // Insane capability words (an unpowered window reading all ones, or zeroes): nothing followed.
  Soc b;
  b.coreClocks(true);
  b.gctl(kPrtcapHost);
  for (unsigned i = 0; i < 8; ++i) b.regs[kXhci + 4 * i] = 0xffffffffu;
  const Clocks cb = surveyClocks(loader(b));
  const Core kb = surveyCore(cb, loader(b));
  b.clear();
  const Xhci xb = surveyXhci(cb, kb, 2, loader(b));
  CHECK(xb.caps && !xb.sane && !xb.walked && b.loads.size() == 8);
  Soc z;
  z.coreClocks(true);
  z.gctl(kPrtcapHost);
  const Clocks cz = surveyClocks(loader(z));
  CHECK(!surveyXhci(cz, surveyCore(cz, loader(z)), 2, loader(z)).sane);

  // An extended capability whose next pointer leaves the window: the walk stops inside it.
  Soc e;
  e.coreClocks(true);
  e.gctl(kPrtcapHost);
  e.xhci();
  e.regs[kXhci + 0x10] = 0x1fc00065;    // xECP at 0x7f00
  e.regs[kXhci + 0x7f00] = 0x000040c0;  // a vendor capability whose next one would start at 0x8000
  const Clocks ce = surveyClocks(loader(e));
  const Core ke = surveyCore(ce, loader(e));
  e.clear();
  const Xhci xe = surveyXhci(ce, ke, 1, loader(e));
  CHECK(xe.walked && xe.xcapCount == 1 && std::strstr(xe.why, "outside") != nullptr);
  CHECK(e.in(kXhci + 0x8000, 0x100000) == 0 && allAllowed(e));
  // A chain longer than the list: the walk stops after kXcapMax entries.
  Soc l;
  l.coreClocks(true);
  l.gctl(kPrtcapHost);
  l.xhci();
  for (unsigned i = 0; i < 40; ++i) l.regs[kXhci + 0x8a0 + 16 * i] = 0x000004c0;
  const Clocks cl = surveyClocks(loader(l));
  const Xhci xl = surveyXhci(cl, surveyCore(cl, loader(l)), 1, loader(l));
  CHECK(xl.xcapCount == kXcapMax && std::strstr(xl.why, "more") != nullptr);
}

static void testQmp()
{
  Soc s;
  s.phyClocks(true);
  s.regs[kQmp + kDpComPhyMode] = 0x3;
  s.regs[kQmp + kDpComTypec] = kTypecPortSelectMux | kTypecPortSelectVal;
  s.regs[kQmp + kPcs + kPcsStatus1] = kPcsPhyStatus;
  const Clocks c = surveyClocks(loader(s));
  CHECK(c.phyOn);
  s.clear();
  const Qmp q = surveyQmp(c, true, loader(s));
  CHECK(q.read && s.loads.size() == 13 && s.in(kQmp, kQmpBytes) == 13);
  CHECK(std::strcmp(qmpModeName(q.phyMode), "USB3 + DP (2 lanes)") == 0);
  CHECK(qmpOrientation(q.typec) == 2);
  CHECK(qmpOrientation(kTypecPortSelectMux) == 1 && qmpOrientation(0) == 0 && qmpOrientation(kTypecPortSelectVal) == 0);
  CHECK(std::strcmp(qmpModeName(1), "USB3 only") == 0 && std::strcmp(qmpModeName(2), "DP only (4 lanes)") == 0);
  s.clear();
  CHECK(!surveyQmp(c, false, loader(s)).read && s.loads.empty());   // piano-usbhost-qmp=0
  // A PHY powered without its com_aux clock: not loaded.
  Soc h;
  h.phyClocks(true);
  h.regs[pianohw::kGcc + pianohw::kGccUsb3PhyComAux] = pianohw::kCbcrClkOff;
  CHECK(!surveyClocks(loader(h)).phyOn);
}

static void testTypec()
{
  // The Mac's port (measured by PianoPower's level 1: snk 0x02 on CC1): Rp 3.0 A in Linux's bit order.
  Typec mac;
  mac.ok = true;
  mac.snk = kSnkRp3p0;
  mac.misc = kMiscAttached | kMiscVbus | kMiscDebounced;
  TypecView v = decodeTypec(mac);
  CHECK(v.attached && !v.sourceMode && v.cc == 1 && v.partner == Partner::source && v.rpMa == 3000 && v.vbus);
  Typec def = mac;
  def.snk = kSnkRpStd;
  CHECK(decodeTypec(def).rpMa == 500);
  def.snk = kSnkRp1p5;
  CHECK(decodeTypec(def).rpMa == 1500);
  // An OTG adapter with a device, plugged the other way round: we source, a sink's Rd on CC2.
  Typec otg;
  otg.ok = true;
  otg.src = kSrcRdOpen;
  otg.misc = kMiscAttached | kMiscCc2 | kMiscSourceMode | kMiscVbus;
  v = decodeTypec(otg);
  CHECK(v.attached && v.sourceMode && v.cc == 2 && v.partner == Partner::usbDevice && v.rpMa == 0);
  otg.src = kSrcRdRa;
  CHECK(decodeTypec(otg).partner == Partner::poweredCableDevice);
  otg.src = kSrcRaRa;
  CHECK(decodeTypec(otg).partner == Partner::audio);
  otg.src = kSrcDebug | kSrcRdOpen;
  CHECK(decodeTypec(otg).partner == Partner::debug);
  Typec none;
  none.ok = true;
  CHECK(!decodeTypec(none).attached && decodeTypec(none).partner == Partner::none && decodeTypec(none).cc == 0);
  Typec unread;
  CHECK(!decodeTypec(unread).attached);
  CHECK(std::strcmp(portModeName(0x02), "sink only") == 0);   // SUN: piano entering UEFI
  CHECK(std::strcmp(portModeName(0x00), "dual role") == 0 && std::strcmp(portModeName(0x04), "source only") == 0);
  CHECK(std::strcmp(portModeName(kModeTrySnk), "dual role, try-sink") == 0);
}

static void testOrientation()
{
  Typec t;
  t.ok = true;
  t.misc = kMiscAttached | kMiscCc2 | kMiscSourceMode;
  t.src = kSrcRdOpen;
  const TypecView v = decodeTypec(t);
  Pins p;
  p.orientation.io = 1;   // gpio61 high: reverse
  Qmp q;
  q.read = true;
  q.typec = kTypecPortSelectMux | kTypecPortSelectVal;
  Orientation o = orientation(v, p, q);
  CHECK(o.known && o.agree && o.pmic == 2 && o.gpio == 2 && o.qmp == 2);
  p.orientation.io = 0;
  o = orientation(v, p, q);
  CHECK(o.known && !o.agree);
  // No cable: gpio61 means nothing and is not a witness; the PHY's select alone is.
  TypecView none;
  q.read = false;
  o = orientation(none, p, q);
  CHECK(!o.known && o.agree && o.gpio == 0);
}

static void testRole()
{
  RoleInput in;
  CHECK(decideRole(in).role == Role::device);   // nothing known: the debug cable's default
  in.typecValid = true;
  CHECK(decideRole(in).role == Role::device);   // detached
  Typec t;
  t.ok = true;
  t.misc = kMiscAttached | kMiscSourceMode;
  t.src = kSrcRdOpen;
  in.typec = decodeTypec(t);
  CHECK(decideRole(in).role == Role::host);      // OTG adapter
  t.src = kSrcRaRa;
  in.typec = decodeTypec(t);
  CHECK(decideRole(in).role == Role::none);      // audio accessory
  t.misc = kMiscAttached;
  t.snk = kSnkRp3p0;
  in.typec = decodeTypec(t);
  CHECK(decideRole(in).role == Role::device);    // a computer, a charger or a dock (PMIC alone cannot tell a dock)
  // UCSI wins: a powered dock (we sink power, the dock's data side is a UFP after its data-role swap) is host.
  in.ucsiValid = true;
  in.ucsiConnected = true;
  in.ucsiPartner = kUcsiUfp;
  CHECK(decideRole(in).role == Role::host);
  in.ucsiPartner = kUcsiCableUfp;
  CHECK(decideRole(in).role == Role::host);
  in.ucsiPartner = kUcsiDfp;
  CHECK(decideRole(in).role == Role::device);
  in.ucsiPartner = kUcsiAudio;
  CHECK(decideRole(in).role == Role::none);
  in.ucsiConnected = false;
  CHECK(decideRole(in).role == Role::device);
  in.forced = Role::host;
  CHECK(decideRole(in).role == Role::host && std::strstr(decideRole(in).why, "forced") != nullptr);
}

static void testCoreDecode()
{
  using namespace pianohw;
  Core k;
  k.read = true;
  k.qsHsPhy = kQsUtmiOtgVbusValid | kQsSwSessvldSel;
  k.qsSsPhy = kQsLane0PwrPresent;
  CHECK(vbusOverride(k) == 3);
  k.qsSsPhy = 0;
  CHECK(vbusOverride(k) == 2);
  k.qsGeneral = kQsPipeUtmiClkSel | kQsPipe3PhystatusSw;
  CHECK(utmiAsPipe(k));
  k.qsGeneral = kQsPipeUtmiClkSel;
  CHECK(!utmiAsPipe(k));
  CHECK(std::strcmp(ipName(0x33313130), "DWC_usb31") == 0 && std::strcmp(ipName(0x5533330a), "DWC_usb3") == 0);
  CHECK(std::strcmp(prtcapName(prtcap(2u << 12)), "device") == 0 && std::strcmp(prtcapName(prtcap(1u << 12)), "host") == 0);
  CHECK(tlmmReserved(36) && tlmmReserved(51) && tlmmReserved(74) && !tlmmReserved(61) && !tlmmReserved(44) && !tlmmReserved(45));
}

int main()
{
  testGates();
  testXhci();
  testQmp();
  testTypec();
  testOrientation();
  testRole();
  testCoreDecode();
  if (failures) {
    std::printf("usbhost_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("usbhost_host: all passed\n");
  return 0;
}
