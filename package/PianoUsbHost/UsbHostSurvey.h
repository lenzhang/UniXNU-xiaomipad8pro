// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoUsbHost level 1: what the survey reads, when it may read it, and what the readings say. Portable (host tests in
// tests/usbhost_host.cpp): every MMIO access goes through Load, a function of a physical address that returns the 32-bit word
// there. There is no store anywhere in this file, so the survey cannot write to the hardware. (The PMIC Type-C block and the eUSB2
// repeater are read by the kext through the SPMI observer channel, package/PianoPower/SpmiObserver.h, whose only store is the
// channel's read command; their decoders are here.)
//
// What is read, and the rule for each (docs/drivers/usb-host.md section 8):
//   always      the TLMM words of gpio61 (the Type-C orientation line), gpio44/45 (the DIO4485's I2C bus): always-on block;
//               the GCC words of the USB30 and USB3 PHY power domains and branch clocks, the TCSR clock references: always-on
//   core on     (USB30 GDSC on, master, cfg_noc and aggre AXI clocks running: PianoDwc3's rule) the DWC3 globals (GCTL with
//               PRTCAPDIR, GSTS, GUSB2PHYCFG, GUSB3PIPECTL, ...), the device-mode DCTL/DSTS and the QSCRATCH glue words; the eight
//               xHCI capability words (constants of the core)
//   host mode   (core on and PRTCAPDIR host or OTG, or piano-usbhost-xhci=2) the xHCI operational words, the extended-capability
//               list and each root port's PORTSC
//   PHY on      (USB3_PHY_GDSC on, aux and com_aux clocks running) the QMP combo PHY's DP_COM (Type-C mux: mode, orientation
//               select, resets), its USB3 PCS status and the DP PHY mode words
// A load from an unclocked or unpowered block can stall the bus or reset the SoC (the Ubuntu hazard K:7dffd78b342d was a store,
// but nothing here relies on a load being safer): every gated block is loaded only after its gate read true in the same pass.
#pragma once
#include <stdint.h>
#include "UsbHostHw.h"

namespace pianousbhost {

// ------------------------------------------------------------------------------------------------------------- TLMM pins
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

struct PinWords {
  uint32_t cfg = 0, io = 0;
};
struct Pins {
  PinWords orientation, sda, scl;
};
template <class Load> Pins surveyPins(Load &&load)
{
  Pins p;
  p.orientation = {load(tlmmCfg(kGpioOrientation)), load(tlmmIo(kGpioOrientation))};
  p.sda = {load(tlmmCfg(kGpioMuxSda)), load(tlmmIo(kGpioMuxSda))};
  p.scl = {load(tlmmCfg(kGpioMuxScl)), load(tlmmIo(kGpioMuxScl))};
  return p;
}

// ---------------------------------------------------------------------------------------------------------- clocks (GCC)
struct Clocks {
  uint32_t usb30Gdsc = 0, master = 0, sleep = 0, mockUtmi = 0, cfgNoc = 0, aggre = 0;
  uint32_t phyGdsc = 0, aux = 0, comAux = 0, pipe = 0, clkrefUsb2 = 0, clkrefUsb3 = 0;
  bool coreOn = false, phyOn = false;
};
inline bool gdscOn(uint32_t v) { return (v & pianohw::kGdscPwrOn) != 0; }
inline bool branchOn(uint32_t v) { return (v & pianohw::kCbcrClkOff) == 0; }
template <class Load> Clocks surveyClocks(Load &&load)
{
  using namespace pianohw;
  Clocks c;
  c.usb30Gdsc = load(kGcc + kGccUsb30Gdsc);
  c.master = load(kGcc + kGccUsb30Master);
  c.sleep = load(kGcc + kGccUsb30Sleep);
  c.mockUtmi = load(kGcc + kGccUsb30MockUtmi);
  c.cfgNoc = load(kGcc + kGccCfgNocUsb3Axi);
  c.aggre = load(kGcc + kGccAggreUsb3Axi);
  c.phyGdsc = load(kGcc + kGccUsb3PhyGdsc);
  c.aux = load(kGcc + kGccUsb3PhyAux);
  c.comAux = load(kGcc + kGccUsb3PhyComAux);
  c.pipe = load(kGcc + kGccUsb3PhyPipe);
  c.clkrefUsb2 = load(kTcsrUsb2ClkRef);
  c.clkrefUsb3 = load(kTcsrUsb3ClkRef);
  // PianoDwc3's rule for its own probe (clocksOn()): the power domain and the three clocks of the register interface.
  c.coreOn = gdscOn(c.usb30Gdsc) && branchOn(c.master) && branchOn(c.cfgNoc) && branchOn(c.aggre);
  c.phyOn = gdscOn(c.phyGdsc) && branchOn(c.aux) && branchOn(c.comAux);
  return c;
}

// ------------------------------------------------------------------------------------------------------------- DWC3 core
struct Core {
  bool read = false;
  uint32_t gsnpsid = 0, gversion = 0, gctl = 0, gsts = 0, guctl = 0, gsbuscfg0 = 0, gusb2phycfg = 0, gusb3pipectl = 0;
  uint32_t dctl = 0, dsts = 0, qsGeneral = 0, qsHsPhy = 0, qsSsPhy = 0;
};
template <class Load> Core surveyCore(const Clocks &c, Load &&load)
{
  using namespace pianohw;
  Core k;
  if (!c.coreOn) return k;
  const auto r = [&](uint32_t off) { return load(kDwc3 + off); };
  k.read = true;
  k.gsnpsid = r(0xc120);
  k.gversion = r(0xc1a0);
  k.gctl = r(0xc110);
  k.gsts = r(0xc118);
  k.guctl = r(0xc12c);
  k.gsbuscfg0 = r(0xc100);
  k.gusb2phycfg = r(0xc200);
  k.gusb3pipectl = r(0xc2c0);
  k.dctl = r(0xc704);
  k.dsts = r(0xc70c);
  k.qsGeneral = load(kQscratch + kQsGeneralCfg);
  k.qsHsPhy = load(kQscratch + kQsHsPhyCtrl);
  k.qsSsPhy = load(kQscratch + kQsSsPhyCtrl);
  return k;
}
inline uint32_t prtcap(uint32_t gctl) { return (gctl & kGctlPrtcapMask) >> kGctlPrtcapShift; }
inline const char *prtcapName(uint32_t p)
{
  return p == kPrtcapHost ? "host" : p == kPrtcapDevice ? "device" : p == kPrtcapOtg ? "OTG (DRD)" : "none";
}
inline const char *ipName(uint32_t gsnpsid)
{
  const uint32_t id = gsnpsid >> 16;
  return id == 0x5533 ? "DWC_usb3" : id == 0x3331 ? "DWC_usb31" : id == 0x3332 ? "DWC_usb32" : "unknown";
}
// The QSCRATCH VBUS/session override (dwc3-qcom's vbus_override_enable): HS_PHY_CTRL bits 20 + 28 and SS_PHY_CTRL bit 24.
// Set: the core believes VBUS is valid (device role); clear: the host role. 0 none, 1 partial, 2 all three bits.
inline unsigned vbusOverride(const Core &k)
{
  using namespace pianohw;
  return unsigned((k.qsHsPhy & kQsUtmiOtgVbusValid) != 0) + unsigned((k.qsHsPhy & kQsSwSessvldSel) != 0) +
         unsigned((k.qsSsPhy & kQsLane0PwrPresent) != 0);
}
// GENERAL_CFG bits 0 + 3: the UTMI clock stands in for the PIPE clock (no SuperSpeed PHY needed: high speed only).
inline bool utmiAsPipe(const Core &k)
{
  using namespace pianohw;
  return (k.qsGeneral & kQsPipeUtmiClkSel) && (k.qsGeneral & kQsPipe3PhystatusSw);
}

// ------------------------------------------------------------------------------------------------------------------ xHCI
struct Protocol {
  uint8_t major = 0, minor = 0, portOffset = 0, portCount = 0;
  uint32_t name = 0;   // "USB " = 0x20425355
};
struct Xhci {
  bool caps = false, sane = false, walked = false;
  uint32_t cap[8] = {};
  uint32_t capLength = 0, hciVersion = 0, maxSlots = 0, maxIntrs = 0, maxPorts = 0, xecp = 0, dbOff = 0, rtsOff = 0;
  bool ac64 = false, csz = false, ppc = false;
  uint32_t usbCmd = 0, usbSts = 0, pageSize = 0, config = 0;
  unsigned xcapCount = 0, protocolCount = 0, portCount = 0;
  uint8_t xcapIds[kXcapMax] = {};
  Protocol protocols[4];
  uint32_t portsc[16] = {};
  const char *why = "";   // why the walk stopped or was not done
};
// The capability words are checked before any pointer in them is followed (a window full of zeroes or ones is not an xHCI).
inline bool capsSane(const Xhci &x)
{
  return x.capLength >= 0x20 && x.capLength <= 0xff && (x.capLength & 3) == 0 && x.hciVersion >= 0x0090 && x.hciVersion <= 0x0120 &&
         x.maxPorts >= 1 && x.maxPorts <= 16 && x.dbOff && x.dbOff < kXhciBytes && !(x.dbOff & 3) && x.rtsOff &&
         x.rtsOff < kXhciBytes && !(x.rtsOff & 0x1f) && x.xecp && x.xecp * 4 < kXhciBytes;
}
inline void decodeCaps(Xhci &x)
{
  x.capLength = x.cap[0] & 0xffu;
  x.hciVersion = x.cap[0] >> 16;
  x.maxSlots = x.cap[1] & 0xffu;
  x.maxIntrs = (x.cap[1] >> 8) & 0x7ffu;
  x.maxPorts = x.cap[1] >> 24;
  x.ac64 = x.cap[4] & 1u;
  x.csz = (x.cap[4] >> 2) & 1u;
  x.ppc = (x.cap[4] >> 3) & 1u;
  x.xecp = x.cap[4] >> 16;
  x.dbOff = x.cap[5] & ~3u;
  x.rtsOff = x.cap[6] & ~0x1fu;
  x.sane = capsSane(x);
}
// level: piano-usbhost-xhci (0 none, 1 capabilities when clocked + the rest in host/OTG mode, 2 the rest in any mode)
template <class Load> Xhci surveyXhci(const Clocks &c, const Core &k, unsigned level, Load &&load)
{
  Xhci x;
  if (!level) {
    x.why = "piano-usbhost-xhci=0";
    return x;
  }
  if (!c.coreOn || !k.read) {
    x.why = "the core is not powered/clocked";
    return x;
  }
  for (unsigned i = 0; i < 8; ++i) x.cap[i] = load(kXhci + 4 * i);
  x.caps = true;
  decodeCaps(x);
  if (!x.sane) {
    x.why = "the capability words are not an xHCI's";
    return x;
  }
  const uint32_t mode = prtcap(k.gctl);
  if (level < 2 && mode != kPrtcapHost && mode != kPrtcapOtg) {
    x.why = "the core is in device mode (piano-usbhost-xhci=2 walks anyway)";
    return x;
  }
  const uint64_t op = kXhci + x.capLength;
  x.usbCmd = load(op + kOpUsbCmd);
  x.usbSts = load(op + kOpUsbSts);
  x.pageSize = load(op + kOpPageSize);
  x.config = load(op + kOpConfig);
  // Extended capabilities: bounded walk inside the window, each entry 4-byte aligned and below the window's end.
  uint32_t at = x.xecp * 4;
  x.why = "";
  while (x.xcapCount < kXcapMax) {
    if (at + 16 > kXhciBytes) {
      x.why = "an extended capability points outside the window";
      break;
    }
    const uint32_t head = load(kXhci + at);
    const uint8_t id = uint8_t(head & 0xffu);
    x.xcapIds[x.xcapCount++] = id;
    if (id == kXcapProtocol && x.protocolCount < 4) {
      Protocol &p = x.protocols[x.protocolCount++];
      p.minor = uint8_t(head >> 16);
      p.major = uint8_t(head >> 24);
      p.name = load(kXhci + at + 4);
      const uint32_t ports = load(kXhci + at + 8);
      p.portOffset = uint8_t(ports & 0xffu);
      p.portCount = uint8_t((ports >> 8) & 0xffu);
    }
    const uint32_t next = (head >> 8) & 0xffu;
    if (!next) break;
    at += next * 4;
  }
  if (x.xcapCount == kXcapMax) x.why = "more extended capabilities than listed";
  const unsigned ports = x.maxPorts < 16 ? x.maxPorts : 16;
  for (unsigned i = 0; i < ports; ++i) x.portsc[i] = load(op + kOpPortBase + kOpPortStride * i);
  x.portCount = ports;
  x.walked = true;
  return x;
}
inline const char *xcapName(uint8_t id)
{
  switch (id) {
    case kXcapLegacy: return "legacy";
    case kXcapProtocol: return "protocol";
    case kXcapPower: return "power";
    case kXcapMsi: return "msi";
    case kXcapDebug: return "debug";
    default: return id >= 192 ? "vendor" : "other";
  }
}
// PORTSC speed IDs (default protocol speed IDs): 1 full, 2 low, 3 high, 4 SuperSpeed, 5 SuperSpeedPlus
inline const char *speedName(uint32_t s)
{
  return s == 1 ? "full" : s == 2 ? "low" : s == 3 ? "high" : s == 4 ? "super" : s == 5 ? "super+" : s == 0 ? "-" : "?";
}

// ------------------------------------------------------------------------------------------------------------- QMP PHY
struct Qmp {
  bool read = false;
  uint32_t phyMode = 0, swReset = 0, powerDown = 0, swi = 0, typec = 0, typecPwrdn = 0, resetOvrd = 0;
  uint32_t pcsSwReset = 0, pcsStatus1 = 0, pcsPowerDown = 0, pcsStart = 0, dpPdCtl = 0, dpMode = 0;
};
template <class Load> Qmp surveyQmp(const Clocks &c, bool enabled, Load &&load)
{
  Qmp q;
  if (!enabled || !c.phyOn) return q;
  q.read = true;
  q.phyMode = load(kQmp + kDpComPhyMode);
  q.swReset = load(kQmp + kDpComSwReset);
  q.powerDown = load(kQmp + kDpComPowerDown);
  q.swi = load(kQmp + kDpComSwi);
  q.typec = load(kQmp + kDpComTypec);
  q.typecPwrdn = load(kQmp + kDpComTypecPwrdn);
  q.resetOvrd = load(kQmp + kDpComResetOvrd);
  q.pcsSwReset = load(kQmp + kPcs + kPcsSwReset);
  q.pcsStatus1 = load(kQmp + kPcs + kPcsStatus1);
  q.pcsPowerDown = load(kQmp + kPcs + kPcsPowerDown);
  q.pcsStart = load(kQmp + kPcs + kPcsStart);
  q.dpPdCtl = load(kQmp + kDpPhy + kDpPhyPdCtl);
  q.dpMode = load(kQmp + kDpPhy + kDpPhyMode);
  return q;
}
inline const char *qmpModeName(uint32_t phyMode)
{
  switch (phyMode & 3u) {
    case 1: return "USB3 only";
    case 2: return "DP only (4 lanes)";
    case 3: return "USB3 + DP (2 lanes)";
    default: return "neither";
  }
}
// The lane orientation the PHY was set to: 0 not software-selected (the PHY's own CC input), 1 normal, 2 reverse.
inline unsigned qmpOrientation(uint32_t typec)
{
  if (!(typec & kTypecPortSelectMux)) return 0;
  return (typec & kTypecPortSelectVal) ? 2 : 1;
}

// ------------------------------------------------------------------------------------------------- PMIC Type-C (decoders)
struct Typec {
  bool ok = false;
  uint8_t snk = 0, src = 0, smState = 0, smStatus = 0, misc = 0, legacy = 0, uusb = 0, rt = 0, mode = 0, vconn = 0, ccout = 0;
};
enum class Partner : uint8_t { none, source, usbDevice, poweredCableDevice, audio, debug, raOpen, unknown };
struct TypecView {
  bool attached = false, sourceMode = false, vbus = false, vbusError = false, debounced = false, water = false;
  unsigned cc = 0;               // 1 or 2 when attached
  Partner partner = Partner::none;
  unsigned rpMa = 0;             // the source's advertised current when we are the sink (0: unknown / none)
};
inline TypecView decodeTypec(const Typec &t)
{
  TypecView v;
  if (!t.ok) return v;
  v.attached = t.misc & kMiscAttached;
  v.sourceMode = t.misc & kMiscSourceMode;
  v.vbus = t.misc & kMiscVbus;
  v.vbusError = t.misc & kMiscVbusError;
  v.debounced = t.misc & kMiscDebounced;
  v.water = t.misc & kMiscWater;
  if (!v.attached) return v;
  v.cc = (t.misc & kMiscCc2) ? 2 : 1;
  if (v.sourceMode) {
    const uint8_t s = t.src & 0x1f;
    v.partner = (s & kSrcDebug)   ? Partner::debug
                : (s & kSrcRdOpen) ? Partner::usbDevice
                : (s & kSrcRdRa)   ? Partner::poweredCableDevice
                : (s & kSrcRaRa)   ? Partner::audio
                : (s & kSrcRaOpen) ? Partner::raOpen
                                   : Partner::unknown;
  } else {
    v.partner = Partner::source;
    v.rpMa = (t.snk & kSnkRp3p0) ? 3000 : (t.snk & kSnkRp1p5) ? 1500 : (t.snk & kSnkRpStd) ? 500 : 0;
  }
  return v;
}
inline const char *partnerName(Partner p)
{
  switch (p) {
    case Partner::none: return "nothing";
    case Partner::source: return "a source (Rp: charger, computer or dock; we sink)";
    case Partner::usbDevice: return "a USB device (Rd: we source)";
    case Partner::poweredCableDevice: return "a USB device behind a powered cable (Rd/Ra: we source, VCONN)";
    case Partner::audio: return "an audio accessory (Ra/Ra)";
    case Partner::debug: return "a debug accessory";
    case Partner::raOpen: return "Ra only (a powered cable, no device)";
    default: return "an unknown termination";
  }
}
inline const char *portModeName(uint8_t mode)
{
  if (mode & kModeDisabled) return "disabled";
  if (mode & kModeSinkOnly) return "sink only";
  if (mode & kModeSourceOnly) return "source only";
  return (mode & kModeTrySnk) ? "dual role, try-sink" : (mode & kModeTrySrc) ? "dual role, try-source" : "dual role";
}

// ---------------------------------------------------------------------------------------------------------- orientation
// Each witness: 0 unknown, 1 normal (CC1), 2 reverse (CC2). The PMIC's CC bit (1 = CC2), gpio61 (high = reverse, as ucsi_glink reads
// it) and the PHY's software select should agree once a cable is in; gpio61 means nothing without one.
struct Orientation {
  unsigned pmic = 0, gpio = 0, qmp = 0;
  bool agree = false, known = false;
};
inline Orientation orientation(const TypecView &v, const Pins &p, const Qmp &q)
{
  Orientation o;
  o.pmic = v.attached ? v.cc : 0;
  o.gpio = v.attached ? (decodePin(p.orientation.cfg, p.orientation.io).in ? 2 : 1) : 0;
  o.qmp = q.read ? qmpOrientation(q.typec) : 0;
  const unsigned seen[3] = {o.pmic, o.gpio, o.qmp};
  unsigned first = 0;
  o.agree = true;
  for (unsigned s : seen) {
    if (!s) continue;
    if (!first) first = s;
    else if (s != first) o.agree = false;
  }
  o.known = first != 0;
  return o;
}
inline const char *orientationName(unsigned o) { return o == 1 ? "normal" : o == 2 ? "reverse" : "-"; }

// ---------------------------------------------------------------------------------------------------------- role policy
// The controller has one role at a time (docs/drivers/usb-host.md section 6.2). This is the decision a role manager would take
// from what is known; level 1 only logs it. UCSI's partner type wins when the DSP's port manager answered (docks: the power role
// and the data role differ after a PD data-role swap); else the PMIC's terminations.
enum class Role : uint8_t { none, device, host };
inline const char *roleName(Role r) { return r == Role::host ? "host" : r == Role::device ? "device" : "none"; }
enum UcsiPartner : uint8_t { kUcsiNone = 0, kUcsiDfp = 1, kUcsiUfp = 2, kUcsiCable = 3, kUcsiCableUfp = 4, kUcsiDebug = 5,
                             kUcsiAudio = 6 };
struct RoleInput {
  bool typecValid = false;
  TypecView typec;
  bool ucsiValid = false, ucsiConnected = false;
  uint8_t ucsiPartner = kUcsiNone;
  Role forced = Role::none;    // piano-usbhost-role (a test override)
};
struct RoleDecision {
  Role role = Role::none;
  const char *why = "";
};
inline RoleDecision decideRole(const RoleInput &in)
{
  if (in.forced != Role::none) return {in.forced, "forced by piano-usbhost-role"};
  if (in.ucsiValid) {
    if (!in.ucsiConnected) return {Role::device, "UCSI: nothing connected (device mode stays armed for the debug cable)"};
    switch (in.ucsiPartner) {
      case kUcsiUfp:
      case kUcsiCableUfp: return {Role::host, "UCSI: the partner is a UFP (a peripheral, a hub or a dock's data side)"};
      case kUcsiDfp: return {Role::device, "UCSI: the partner is a DFP (a computer)"};
      case kUcsiDebug: return {Role::device, "UCSI: a debug accessory"};
      case kUcsiAudio: return {Role::none, "UCSI: an audio accessory (no USB data)"};
      default: return {Role::device, "UCSI: no USB partner on the data lines"};
    }
  }
  if (!in.typecValid) return {Role::device, "no Type-C state: device mode (the debug cable's default)"};
  const TypecView &v = in.typec;
  if (!v.attached) return {Role::device, "nothing attached: device mode stays armed"};
  switch (v.partner) {
    case Partner::usbDevice:
    case Partner::poweredCableDevice: return {Role::host, "a sink's Rd: an OTG adapter or a bus-powered device"};
    case Partner::source:
      return {Role::device, "a source's Rp: a computer, a charger or a dock (a dock needs UCSI's data role to become host)"};
    case Partner::audio: return {Role::none, "an audio accessory"};
    case Partner::debug: return {Role::device, "a debug accessory"};
    default: return {Role::device, "an unknown termination: device mode"};
  }
}

}  // namespace pianousbhost
