// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoDp's first slices against models (docs/drivers/displayport.md D1, D2, D6):
//   - D1 (DpSurvey.h) on a register model: nothing of DISPCC without GCC's display AHB; nothing of the DP controller while the MDSS
//     gate is closed or its AUX clock is off, its link block only with LINK and LINK_INTF on, p0 only with PIXEL0 too; the AUX FIFO
//     and the HPD acknowledge register never; nothing of the QMP PHY while its power domain or aux/com_aux clock is off; the DPU's DP
//     side only behind the MDSS gate; every load inside the blocks the survey names; the decoders (version, HPD state, RCG, lanes,
//     PD_CTL) and the DPU path verdict (the panel on CTL0/LM0-1/MERGE_3D_0: CTL1 + LM2/LM3 + MERGE_3D_1 + INTF0 free);
//   - D2 (DpPort.h): the notification decoder on struct usbc_notify's layout, the pin-assignment table (4K60 needs pins C/E, which
//     leave USB 2 only), the link budget at piano's HBR2 cap, the DIO4485 words per pin and orientation, and the per-port HPD state
//     machine on plug sequences (a USB stick, a monitor plugged and unplugged, a dock, flips, IRQ_HPD, repeats, mismatches);
//   - D6 (DpFake.h): the arguments into a configuration and the plug schedule.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoDpu -I../../PianoUsbHost -I../../PianoDwc3 dp_host.cpp -o /tmp/dp_host
#include "DpSurvey.h"
#include "DpPort.h"
#include "DpFake.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace pianodp;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---------------------------------------------------------------------------------------------------------- the MMIO model
// Unset registers read 0: a branch CBCR of 0 is "on" (CLK_OFF clear) and a GDSC of 0 "off", so the model's default is "GCC display
// AHB on, MDSS GDSC off" and every test states the clocks it means.
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
  bool loaded(uint64_t pa) const
  {
    for (uint64_t p : loads)
      if (p == pa) return true;
    return false;
  }
  void set(uint64_t pa, uint32_t v) { regs[pa] = v; }
  void mdssOn()
  {
    set(pianodpu::kGcc + pianodpu::kGccDispAhb, 1);
    set(pianodpu::kDispCc + pianodpu::kCcMdssGdsc, 0x80000000);
    set(pianodpu::kDispCc + pianodpu::kCcAhb, 1);
    set(pianodpu::kDispCc + pianodpu::kCcMdp, 1);
  }
  void dpClocks(bool aux, bool link, bool linkIntf, bool pixel0)
  {
    set(pianodpu::kDispCc + kCcDpAux, aux ? 1 : 0x80000000);
    set(pianodpu::kDispCc + kCcDpLink, link ? 1 : 0x80000000);
    set(pianodpu::kDispCc + kCcDpLinkIntf, linkIntf ? 1 : 0x80000000);
    set(pianodpu::kDispCc + kCcDpPixel0, pixel0 ? 1 : 0x80000000);
    set(pianodpu::kDispCc + kCcDpPixel1, 0x80000000);
    set(pianodpu::kDispCc + kCcDpCrypto, 0x80000000);
    set(pianodpu::kDispCc + kCcDpRouterLinkIntf, 0x80000000);
  }
  void qmp(bool gdsc, bool aux, bool comAux)
  {
    using namespace pianohw;
    set(kGcc + kGccUsb3PhyGdsc, gdsc ? 0x80000000 : 0);
    set(kGcc + kGccUsb3PhyAux, aux ? 1 : 0x80000000);
    set(kGcc + kGccUsb3PhyComAux, comAux ? 1 : 0x80000000);
  }
};

static Survey run(Soc &soc)
{
  Survey s;
  survey(s, [&](uint64_t pa) { return soc.load(pa); });
  return s;
}

constexpr uint64_t kQmpWindow = pianousbhost::kQmp, kQmpBytes = pianousbhost::kQmpBytes;
constexpr uint64_t kDpWindow = kDpFirst, kDpBytes = kDpEnd - kDpFirst;

// Every load in a block the survey names: GCC, TCSR's two clock references, DISPCC, the DP controller, the QMP PHY, the MDP, three pins.
static bool loadsAllowed(const Soc &soc)
{
  using namespace pianodpu;
  for (uint64_t pa : soc.loads) {
    const bool ok = (pa >= kGcc && pa < kGcc + 0x1f0000) || pa == pianohw::kTcsrUsb2ClkRef || pa == pianohw::kTcsrUsb3ClkRef ||
                    (pa >= kDispCc && pa < kDispCc + 0x20000) || (pa >= kDpWindow && pa < kDpWindow + kDpBytes) ||
                    (pa >= kQmpWindow && pa < kQmpWindow + kQmpBytes) || (pa >= kMdp && pa < kMdp + kMdpBytes) ||
                    pa == pianousbhost::tlmmCfg(pianousbhost::kGpioOrientation) || pa == pianousbhost::tlmmIo(pianousbhost::kGpioOrientation) ||
                    pa == pianousbhost::tlmmCfg(pianousbhost::kGpioMuxSda) || pa == pianousbhost::tlmmIo(pianousbhost::kGpioMuxSda) ||
                    pa == pianousbhost::tlmmCfg(pianousbhost::kGpioMuxScl) || pa == pianousbhost::tlmmIo(pianousbhost::kGpioMuxScl);
    if (!ok) {
      std::printf("unexpected load %#llx\n", (unsigned long long)pa);
      return false;
    }
  }
  return true;
}
static bool neverSideEffects(const Soc &soc) { return !soc.loaded(kDpAux + kAuxDataNeverRead) && !soc.loaded(kDpAux + kAuxHpdAckNeverRead); }

static void testGates()
{
  using namespace pianodpu;
  {  // GCC's display AHB off: nothing of DISPCC, the controller or the DPU; the QMP's own gate closed too
    Soc soc;
    soc.set(kGcc + kGccDispAhb, 0x80000000);
    soc.qmp(false, true, true);
    const pianodp::Survey s = run(soc);
    CHECK(!s.mdss.readable && !s.clocks.read && !s.ctrl.core && !s.phy.read && !s.path.read);
    CHECK(soc.in(kDispCc, 0x20000) == 0);
    CHECK(soc.in(kDpWindow, kDpBytes) == 0);
    CHECK(soc.in(kMdp, kMdpBytes) == 0);
    CHECK(soc.in(kQmpWindow, kQmpBytes) == 0);
    CHECK(loadsAllowed(soc));
    CHECK(s.loads == soc.loads.size());
  }
  {  // MDSS on, every DP clock off (what native boots will show): DISPCC's DP words, the DPU's DP side; no DP controller load
    Soc soc;
    soc.mdssOn();
    soc.dpClocks(false, false, false, false);
    soc.qmp(false, false, false);
    const pianodp::Survey s = run(soc);
    CHECK(s.mdss.readable && s.clocks.read && !s.ctrl.core && s.path.read);
    CHECK(soc.loaded(kDispCc + kCcDpAux) && soc.loaded(kDispCc + kCcDpPixel0Rcg + kRcgN));
    CHECK(soc.in(kDpWindow, kDpBytes) == 0);
    CHECK(soc.in(kQmpWindow, kQmpBytes) == 0);
    CHECK(soc.loaded(kMdp + kIntfBase + kIntfStride * 0 + kIntfEn) && soc.loaded(kMdp + kIntfBase + kIntfStride * 3 + kIntfEn));
    CHECK(soc.loaded(kMdp + kCtlBase + kCtlStride * 1 + kCtlIntfActive) && soc.loaded(kMdp + kLmBase + kLmStride * 2 + kLmOutSize));
    CHECK(!soc.loaded(kMdp + kIntfBase + kIntfStride * 1 + kIntfEn));   // the panel's INTFs are PianoDpu's, not read here
    CHECK(loadsAllowed(soc));
  }
  {  // the AUX clock on but the MDSS GDSC off: the controller stays unread (its parent is not active)
    Soc soc;
    soc.mdssOn();
    soc.set(kDispCc + kCcMdssGdsc, 0);
    soc.dpClocks(true, true, true, true);
    const pianodp::Survey s = run(soc);
    CHECK(!s.mdss.readable && !s.ctrl.core && !s.path.read);
    CHECK(soc.in(kDpWindow, kDpBytes) == 0);
    CHECK(soc.in(kMdp, kMdpBytes) == 0);
  }
  {  // core clocks on, link off: ahb + aux only
    Soc soc;
    soc.mdssOn();
    soc.dpClocks(true, false, true, true);
    soc.set(kDpAhb + kAhbHwVersion, 0x10050001);
    soc.set(kDpAux + kAuxHpdIntStatus, 0x40000000);
    soc.set(kDpAux + kAuxHpdCtrl, 1);
    const pianodp::Survey s = run(soc);
    CHECK(s.ctrl.core && !s.ctrl.link && !s.ctrl.stream);
    CHECK(soc.in(kDpAhb, kDpAhbBytes) > 0);
    CHECK(soc.in(kDpLink, kDpLinkBytes) == 0 && soc.in(kDpP0, kDpP0Bytes) == 0);
    CHECK(neverSideEffects(soc));
    const Version v = hwVersion(s.ctrl.hwVersion);
    CHECK(v.major == 1 && v.minor == 5 && v.step == 1);
    CHECK(hpdState(s.ctrl.hpdIntStatus) == 2 && !std::strcmp(hpdStateName(2), "connected"));
  }
  {  // link on, pixel0 off: no p0
    Soc soc;
    soc.mdssOn();
    soc.dpClocks(true, true, true, false);
    soc.set(kDpLink + kLinkConfigCtrl, 0x30 | 0x100);
    const pianodp::Survey s = run(soc);
    CHECK(s.ctrl.core && s.ctrl.link && !s.ctrl.stream);
    CHECK(soc.in(kDpLink, kDpLinkBytes) > 0 && soc.in(kDpP0, kDpP0Bytes) == 0);
    CHECK(configuredLanes(s.ctrl.configCtrl) == 4);
  }
  {  // everything clocked: all four blocks, still no FIFO or acknowledge read; nothing of p1 (MST)
    Soc soc;
    soc.mdssOn();
    soc.dpClocks(true, true, true, true);
    soc.qmp(true, true, true);
    soc.set(kQmpWindow + kQmpDpPhy + kDpPhyPdCtl, 0x67);
    soc.set(kQmpWindow + kQmpDpPhy + kDpPhyRevision0, 0x00);
    soc.set(kQmpWindow + kQmpDpPhy + kDpPhyRevision0 + 12, 0x80);
    soc.set(kQmpWindow + pianousbhost::kDpComPhyMode, 3);
    const pianodp::Survey s = run(soc);
    CHECK(s.ctrl.core && s.ctrl.link && s.ctrl.stream && s.phy.read);
    CHECK(soc.in(kDpP0, kDpP0Bytes) > 0 && soc.in(kDpP1, 0x9c) == 0);
    CHECK(neverSideEffects(soc));
    CHECK(soc.loaded(kQmpWindow + kQmpDpSerdes + kSerdesCReadyStatus) && soc.loaded(kQmpWindow + kQmpDpPhy + kDpPhyStatus));
    CHECK(s.phy.pdCtl == 0x67 && s.phy.revision[3] == 0x80 && s.phy.com.phyMode == 3);
    CHECK(loadsAllowed(soc));
  }
  {  // the QMP gate alone: GDSC on, com_aux off: not read
    Soc soc;
    soc.mdssOn();
    soc.qmp(true, true, false);
    const pianodp::Survey s = run(soc);
    CHECK(!s.phy.read && soc.in(kQmpWindow, kQmpBytes) == 0);
  }
}

static void testDecoders()
{
  CHECK(hwVersion(0x10020000).major == 1 && hwVersion(0x10020000).minor == 2 && hwVersion(0x10020000).step == 0);
  CHECK(hpdState(0x00000000) == 0 && hpdState(0x20000000) == 1 && hpdState(0x40000000) == 2);
  const Rcg r = rcg(0x80000000, 0x0000010b);
  CHECK(!r.on && r.source == 1 && r.halfDivider == 0xb && r.mode == 0);
  CHECK(rcg(0x2, 0x2000).on && rcg(0x2, 0x2000).mode == 2);
  CHECK(configuredLanes(0x00) == 1 && configuredLanes(0x10) == 2 && configuredLanes(0x30) == 4);
  char pd[64];
  pdCtlText(0x67, pd, sizeof pd);
  CHECK(!std::strcmp(pd, "pwrdn|psr|aux|pll|clamp"));
  pdCtlText(0, pd, sizeof pd);
  CHECK(!std::strcmp(pd, "none"));
  CHECK(!std::strcmp(pdCtlVerdict(0x02), "msm's powered-off value (PSR only)"));
  CHECK(!std::strcmp(pdCtlVerdict(0x67), "powered, AUX on, no lanes (msm's AUX-initialised state)"));
  CHECK(!std::strcmp(pdCtlVerdict(0x7f), "powered with lanes (a link was up)"));
}

static void testPath()
{
  using namespace pianodpu;
  SecondPath p;
  p.read = true;
  // The panel as PianoDpu's model has it: CTL0 video mode, INTF1 + INTF2, LM0/LM1, VIG0/VIG1, MERGE_3D_0.
  p.ctls[0].intfActive = 0x6;
  p.ctls[0].layerActive = 0x3;
  p.ctls[0].merge3dActive = 0x1;
  p.ctls[0].pipeActive = (1u << 16) | (1u << 17);
  PathView v = pianodp::viewPath(p);
  CHECK(v.panelCtl == 0 && v.panelLms == 0x3 && v.panelMerge3d == 0x1 && v.panelIntfs == 0x6);
  CHECK(v.dpCtl == 1 && v.dpLmPair == 1 && v.merge3d1Free && v.intf0Free && !v.intf0Running && v.fourK);
  // CTL1 taken with LM2: the next CTL and LM pair, no 4K pair.
  p.ctls[1].layerActive = 0x4;
  p.ctls[1].intfActive = 0;
  v = pianodp::viewPath(p);
  CHECK(v.dpCtl == 2 && v.dpLmPair == 2 && !v.fourK);
  // INTF0's timing engine running (something lit DP): not free.
  p.ctls[1] = {};
  p.intf[0].en = 1;
  v = pianodp::viewPath(p);
  CHECK(v.intf0Running && !v.intf0Free && !v.fourK);
  // MERGE_3D_1 held by the panel (a 4-LM panel path): no 4K pair.
  p.intf[0] = {};
  p.ctls[0].merge3dActive = 0x3;
  v = pianodp::viewPath(p);
  CHECK(!v.merge3d1Free && !v.fourK);
}

// ----------------------------------------------------------------------------------------------------------------- D2
static std::vector<uint8_t> notify(uint8_t port, uint8_t orientation, uint8_t mux, uint16_t svid, uint8_t ext)
{
  std::vector<uint8_t> m(32, 0);
  const uint32_t owner = kPanOwner, type = 2, opcode = kPanNotify | uint32_t(svid) << 16;
  std::memcpy(&m[0], &owner, 4);
  std::memcpy(&m[4], &type, 4);
  std::memcpy(&m[8], &opcode, 4);
  m[12] = port;
  m[13] = orientation;
  m[14] = mux;
  m[18] = uint8_t(svid);
  m[19] = uint8_t(svid >> 8);
  m[20] = ext;
  return m;
}
static uint8_t dpExt(uint8_t pin, bool hpd, bool irq) { return uint8_t(pin | (hpd ? 0x40 : 0) | (irq ? 0x80 : 0)); }
static PortEvent dec(const std::vector<uint8_t> &m) { return decodePortEvent(m.data(), uint32_t(m.size())); }

static void testDecoder()
{
  PortEvent e = dec(notify(0, 1, kMuxUsb3Dp, kSvidDp, dpExt(4, true, false)));
  CHECK(e.valid && e.port == 0 && e.orientation == 1 && e.mux == kMuxUsb3Dp && e.dp && e.pin == 4 && e.hpd && !e.irq);
  CHECK(e.svid == kSvidDp && e.svidPayload == kSvidDp && e.vid == 0);
  e = dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(3, true, true)));
  CHECK(e.valid && e.pin == 3 && e.hpd && e.irq);
  // a non-DP SVID: the extended byte is not a DP word
  e = dec(notify(0, 0, kMuxTunnel, kSvidTbt, 0xc5));
  CHECK(e.valid && !e.dp && e.pin == 0 && !e.hpd && !e.irq && e.ext == 0xc5);
  // Linux's checks: exactly 32 bytes, a port below 3, the owner and opcode
  auto m = notify(0, 0, kMuxUsb3, 0, 0);
  CHECK(!decodePortEvent(m.data(), 31).valid && !decodePortEvent(m.data(), 13).valid);
  m.push_back(0);
  CHECK(!decodePortEvent(m.data(), uint32_t(m.size())).valid);
  CHECK(!dec(notify(3, 0, kMuxUsb3, 0, 0)).valid);
  m = notify(0, 0, kMuxUsb3, 0, 0);
  m[0] = 0x0a;   // owner 0x800a (the charger's)
  CHECK(!dec(m).valid);
  m = notify(0, 0, kMuxUsb3, 0, 0);
  m[8] = 0x15;   // a write answer, not a notification
  CHECK(!dec(m).valid);
  CHECK(!decodePortEvent(nullptr, 32).valid);
  CHECK(!std::strcmp(orientationName(2), "none") && !std::strcmp(muxName(kMuxDp4), "DP 4 lanes"));
}

static void testPins()
{
  // 4K60 needs pins C/E (four lanes at HBR2), which carry no USB 3: USB 2 only.
  for (uint8_t pin = 1; pin <= 6; ++pin) {
    const PinInfo p = pinInfo(pin);
    const bool fourK = p.supported && (fittingModes(p.lanes) & 1u);
    CHECK(fourK == (p.letter == 'C' || p.letter == 'E'));
    if (fourK) CHECK(!p.usb3);
  }
  CHECK(!pinInfo(1).supported && !pinInfo(2).supported && !pinInfo(0).supported && !pinInfo(7).supported);
  CHECK(pinInfo(3).lanes == 4 && pinInfo(4).lanes == 2 && pinInfo(5).lanes == 4 && pinInfo(6).lanes == 2);
  CHECK(pinInfo(3).qmpMode == 2 && pinInfo(4).qmpMode == 3);
  // the DIO4485 words of displayport.md's table (fsa4480_set): C/E 0xe0 / 0x00 (0x60 reversed), D/F 0xf8 / 0x18 (0x78), USB 0x98 / 0x18
  CHECK(pinInfo(3).dioEnable == 0xe0 && dioSelect(pinInfo(3), false) == 0x00 && dioSelect(pinInfo(3), true) == 0x60);
  CHECK(pinInfo(4).dioEnable == 0xf8 && dioSelect(pinInfo(4), false) == 0x18 && dioSelect(pinInfo(4), true) == 0x78);
  CHECK(kDioUsbEnable == 0x98 && kDioUsbSelect == 0x18);
  // the budget: 4 x HBR2 = 17.28 Gbit/s, 2 x HBR2 = 8.64; 4K60 (12.8) only on four lanes, 4K30 and 1440p60 on two
  CHECK(payloadMbps(4, kRateHbr2) == 17280 && payloadMbps(2, kRateHbr2) == 8640);
  CHECK(fittingModes(4) == 0xf);
  CHECK(fittingModes(2) == 0xe);
  CHECK(fittingModes(1) == 0x8);   // one lane: 1080p60 (3.56 of 4.32 Gbit/s)
  CHECK(!(fittingModes(4, kRateHbr) & 1u));   // at HBR, four lanes (8.64) do not carry 4K60
  CHECK(modeFits(533250, 24, 4, kRateHbr2) && !modeFits(533250, 30, 2, kRateHbr2) && !modeFits(533250, 24, 2, kRateHbr2));
}

static void testStateMachine()
{
  {  // a USB stick: attach USB3 only, detach
    PortState s;
    PortStep r = portStep(s, dec(notify(0, 0, kMuxUsb3, 0, 0)));
    CHECK(r.changes == kChAttach && s.attached && !s.dp && !s.hpd);
    r = portStep(s, dec(notify(0, 0, kMuxNone, 0, 0)));
    CHECK(r.changes == kChDetach && !s.attached);
    CHECK(!plugPlan(s).drive);
  }
  {  // a monitor (pin C, normal): entered with HPD low, then HPD high, IRQ_HPD, HPD low, then the safe state
    PortState s;
    PortStep r = portStep(s, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(3, false, false))));
    CHECK(r.changes == (kChAttach | kChDpEnter) && s.dp && !s.hpd && s.pin == 3);
    const PlugPlan p = plugPlan(s);
    CHECK(p.drive && p.lanes == 4 && !p.usb3 && p.qmpMode == 2 && p.dioEnable == 0xe0 && p.dioSelect == 0x00 && (p.modes & 1u));
    r = portStep(s, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(3, true, false))));
    CHECK(r.changes == kChHpdUp && s.hpd && s.plugs == 1);
    r = portStep(s, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(3, true, true))));
    CHECK(r.changes == kChIrqHpd && s.irqs == 1);
    r = portStep(s, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(3, false, false))));
    CHECK(r.changes == kChHpdDown && !s.hpd && s.unplugs == 1);
    // the safe state: SVID DP with "no connection" (Linux: pmic_glink_altmode_safe and an HPD-low report)
    r = portStep(s, dec(notify(0, 0, kMuxNone, kSvidDp, dpExt(0, false, false))));
    CHECK(r.changes == (kChDetach | kChDpExit) && !s.attached && !s.dp);
  }
  {  // a dock (pin D, reverse): USB3 + DP, HPD high at once, then the cable turned over (a new attach, normal)
    PortState s;
    PortStep r = portStep(s, dec(notify(0, 1, kMuxUsb3Dp, kSvidDp, dpExt(4, true, false))));
    CHECK(r.changes == (kChAttach | kChDpEnter | kChHpdUp));
    const PlugPlan p = plugPlan(s);
    CHECK(p.lanes == 2 && p.usb3 && p.qmpMode == 3 && p.dioEnable == 0xf8 && p.dioSelect == 0x78 && !(p.modes & 1u) && (p.modes & 2u));
    r = portStep(s, dec(notify(0, 0, kMuxNone, 0, 0)));
    CHECK(r.changes == (kChDetach | kChDpExit | kChHpdDown));
    r = portStep(s, dec(notify(0, 0, kMuxUsb3Dp, kSvidDp, dpExt(4, true, false))));
    CHECK((r.changes & (kChAttach | kChDpEnter | kChHpdUp)) == (kChAttach | kChDpEnter | kChHpdUp));
    CHECK(!(r.changes & kChOrientation));   // a fresh attach, not a flip while attached
    CHECK(plugPlan(s).dioSelect == 0x18);
  }
  {  // an orientation change while attached, a pin change C -> D with the mux, a repeat
    PortState s;
    portStep(s, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(3, true, false))));
    PortStep r = portStep(s, dec(notify(0, 1, kMuxDp4, kSvidDp, dpExt(3, true, false))));
    CHECK(r.changes == kChOrientation && r.before.orientation == 0 && r.after.orientation == 1);
    r = portStep(s, dec(notify(0, 1, kMuxUsb3Dp, kSvidDp, dpExt(4, true, false))));
    CHECK(r.changes == (kChPin | kChMux) && r.before.pin == 3 && s.pin == 4);
    r = portStep(s, dec(notify(0, 1, kMuxUsb3Dp, kSvidDp, dpExt(4, true, false))));
    CHECK(r.changes == kChRepeat && s.events == 4);
  }
  {  // mismatches and unsupported pins are flagged, not driven
    PortState s;
    PortStep r = portStep(s, dec(notify(0, 0, kMuxUsb3Dp, kSvidDp, dpExt(3, false, false))));   // pin C with USB3 + DP
    CHECK(r.changes & kChPinMuxMismatch);
    PortState a;
    r = portStep(a, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(1, true, false))));   // pin A
    CHECK((r.changes & kChUnsupportedPin) && !plugPlan(a).drive);
    PortState z;
    r = portStep(z, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(0, false, false))));   // HPD_OUT: no pin assignment
    CHECK(r.changes & kChUnsupportedPin);
    PortState q;
    r = portStep(q, dec(notify(0, 0, kMuxDp4, kSvidDp, dpExt(3, false, true))));   // IRQ_HPD with HPD low
    CHECK((r.changes & kChIrqLow) && !(r.changes & kChIrqHpd) && q.irqs == 0);
  }
  {  // tunnelling (USB4/TBT) and other SVIDs: attached, never DP, no HPD
    PortState s;
    PortStep r = portStep(s, dec(notify(0, 0, kMuxTunnel, kSvidTbt, 0xff)));
    CHECK(r.changes == kChAttach && !s.dp && !s.hpd);
  }
}

// ----------------------------------------------------------------------------------------------------------------- D6
static void testFake()
{
  FakeArgs a;
  CHECK(!fakeConfig(a).armed);   // piano-dp=0
  a.dp = 1;
  CHECK(!fakeConfig(a).armed);   // no piano-dp-fake
  a.plug = 60;
  FakeConfig c = fakeConfig(a);
  CHECK(c.armed && !c.atBoot && c.plugS == 60 && c.unplugS == 0 && c.cycles == 1);
  CHECK(c.width == 1920 && c.height == 1080 && c.hz == 60 && c.widthMm == 527 && c.heightMm == 296 && c.primary == 0);
  CHECK(plugEvents(c) == 1);
  PlugStep s = plugStep(c, 0);
  CHECK(s.valid && s.connect && s.atS == 60 && s.plugNumber == 1);
  CHECK(!plugStep(c, 1).valid);
  // alternating: three plugs, ending plugged
  a.unplug = 30;
  a.cycles = 3;
  c = fakeConfig(a);
  CHECK(plugEvents(c) == 5);
  const uint32_t at[5] = {60, 90, 120, 150, 180};
  for (uint32_t k = 0; k < 5; ++k) {
    s = plugStep(c, k);
    CHECK(s.valid && s.atS == at[k] && s.connect == (k % 2 == 0));
  }
  CHECK(plugStep(c, 4).plugNumber == 3 && !plugStep(c, 5).valid);
  // connected from the start: the first event is the unplug
  a.boot = 1;
  a.unplug = 10;
  a.cycles = 2;
  c = fakeConfig(a);
  CHECK(c.armed && c.atBoot && plugEvents(c) == 2);
  CHECK(!plugStep(c, 0).connect && plugStep(c, 0).atS == 10 && plugStep(c, 1).connect && plugStep(c, 1).atS == 20 &&
        plugStep(c, 1).plugNumber == 2);
  a.unplug = 0;
  c = fakeConfig(a);
  CHECK(c.armed && plugEvents(c) == 0 && c.cycles == 1);   // fixed from boot
  // bounds
  FakeArgs b;
  b.dp = 1;
  b.plug = 3601;
  CHECK(!fakeConfig(b).armed);
  b.plug = 20;
  b.unplug = 3;
  c = fakeConfig(b);
  CHECK(c.armed && c.unplugS == 0 && *c.note);
  b.unplug = 0;
  b.cycles = 7;
  CHECK(fakeConfig(b).cycles == 1);   // cycles only with an unplug period
  b.mode = "2560x1440@60";
  b.mm = "597x336";
  c = fakeConfig(b);
  CHECK(c.width == 2560 && c.height == 1440 && c.hz == 60 && c.widthMm == 597 && c.heightMm == 336);
  b.mode = "3840x2160";
  CHECK(fakeConfig(b).hz == 60 && fakeConfig(b).width == 3840);
  for (const char *bad : {"1921x1080@60", "1920x1080@200", "1920*1080", "x1080", "1920x1080@", "1920x1080@60x", "99999999x1"}) {
    b.mode = bad;
    c = fakeConfig(b);
    CHECK(c.width == 1920 && c.height == 1080 && c.hz == 60 && *c.note);
  }
  b.mode = nullptr;
  b.mm = "50x50";
  CHECK(fakeConfig(b).widthMm == 527);
  b.mm = nullptr;
  b.primary = 1;
  CHECK(fakeConfig(b).primary == 1);
  uint32_t w = 0, h = 0, hz = 0;
  CHECK(parseMode("640x480@24", w, h, hz) && w == 640 && h == 480 && hz == 24);
  CHECK(!parseMode(nullptr, w, h, hz) && !parseSize("1x", w, h));
  CHECK(std::strlen(kFakeUuid) == 36 && std::strcmp(kFakeUuid, "4E474F50-4449-5350-0000-000000008750") != 0);
}

int main()
{
  testGates();
  testDecoders();
  testPath();
  testDecoder();
  testPins();
  testStateMachine();
  testFake();
  std::printf("PianoDp host tests: %s\n", failures ? "FAILED" : "passed");
  return failures ? 1 : 0;
}
