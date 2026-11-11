// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDp D1: the read-only survey of the DisplayPort path: what is read, behind which gate, and what the readings say. Portable (host
// tests in tests/dp_host.cpp): every hardware access goes through Load, a function of a physical address returning the 32-bit word
// there. There is no store anywhere in this file, so the survey cannot write to the hardware.
//
// Order and gates (docs/drivers/displayport.md D1). A load from an unclocked block can stall the bus or reset the SoC (msm's own DP
// snapshot refuses it, dp_display.c), so every gated block is loaded only after its gate read true in the same pass:
//   (1) PianoDpu's display gate (DpuSurvey.h surveyClocks): GCC's display AHB, then DISPCC's MDSS GDSC, MDSS AHB and MDP clocks;
//   (2) with DISPCC readable (GCC's display AHB on): DISPCC's DP branches and RCGs (DpHw.h);
//   (3) the DP controller, as msm clocks it: ahb + aux with the MDSS gate open and the DPTX0 AUX clock on (its core clocks), link with
//       the LINK and LINK_INTF clocks on as well, p0 with PIXEL0 too; the AUX FIFO and the HPD acknowledge register never;
//   (4) the QMP PHY: PianoUsbHost's GCC/TCSR words (always on), then with the PHY's power domain, aux and com_aux clocks on (its phyOn
//       rule) DP_COM, the USB3 PCS words it reads, and the DP half's version, power-down, mode, status and AUX configuration words;
//   (5) the DPU's DP side with the MDSS gate open: every CTL (so the panel's path is known), every LM, MERGE_3D 0..3, INTF0 and INTF3,
//       the TOP interface and interrupt words (msm's snapshot reads all of them with only the MDSS clocks on);
//   (6) the TLMM words of the Type-C orientation line and the DIO4485's I2C pins (an always-on block); the DIO4485 itself is not read
//       (an I2C read needs the pins taken: writes).
#pragma once
#include <stdint.h>
#include "DpHw.h"
#include "DpuSurvey.h"
#include "UsbHostSurvey.h"

namespace pianodp {

// ------------------------------------------------------------------------------------------------------------- the record
struct DpClocks {
  bool read = false;   // DISPCC was readable (GCC's display AHB on)
  uint32_t link = 0, routerLinkIntf = 0, linkIntf = 0, crypto = 0, pixel0 = 0, pixel1 = 0, aux = 0;
  uint32_t auxRcg[2] = {}, linkRcg[2] = {}, pixel0Rcg[5] = {}, pixel1Rcg[2] = {}, linkDiv = 0;
};
struct DpCtrl {
  bool core = false, link = false, stream = false;   // the blocks read: ahb + aux, link, p0
  const char *why = "";
  uint32_t hwVersion = 0, swReset = 0, phyCtrl = 0, clkCtrl = 0, clkActive = 0, intr[4] = {}, intrMask4 = 0;
  uint32_t hpdCtrl = 0, hpdIntStatus = 0, hpdIntMask = 0, hpdRefTimer = 0, hpdEventTime[2] = {}, auxCtrl = 0, auxTransCtrl = 0,
           auxTimeoutCount = 0, auxLimits = 0, auxStatus = 0;
  uint32_t mainlinkCtrl = 0, stateCtrl = 0, configCtrl = 0, totalHorVer = 0, activeHorVer = 0, misc = 0, laneMap = 0,
           mainlinkReady = 0, levels = 0, tu = 0;
  uint32_t p0Bist = 0, p0TimingEn = 0, p0IntfConfig = 0, p0HsyncCtl = 0, p0VsyncPeriod = 0, p0DisplayHctl = 0, p0Tpg = 0;
};
struct DpPhy {
  bool read = false;
  pianousbhost::Qmp com;   // DP_COM and the USB3 PCS words (PianoUsbHost's surveyQmp)
  uint32_t revision[4] = {}, cfg = 0, cfg1 = 0, pdCtl = 0, mode = 0, auxCfg[kDpPhyAuxCfgCount] = {}, vcoDiv = 0, auxIntStatus = 0,
           status = 0, cmnStatus = 0, cReadyStatus = 0;
};
struct SecondPath {
  bool read = false;
  uint32_t intfSel = 0, intrEn = 0, intrStatus = 0, dpPhyIntfSel = 0;
  pianodpu::CtlRegs ctls[pianodpu::kCtlCount]{};
  pianodpu::LmRegs lms[pianodpu::kLmCount]{};
  uint32_t merge3d[4][2]{};
  pianodpu::IntfRegs intf[2]{};   // INTF0, INTF3
};
struct Survey {
  pianodpu::ClockGate mdss;
  DpClocks clocks;
  DpCtrl ctrl;
  pianousbhost::Clocks usb;
  DpPhy phy;
  SecondPath path;
  pianousbhost::Pins pins;
  unsigned loads = 0;
};

// The controller's gates: msm never touches it unless its parent MDSS is active (GDSC, AHB, MDP) and its own clocks run.
inline bool coreClocksOn(const Survey &s) { return s.mdss.readable && s.clocks.read && pianodpu::branchOn(s.clocks.aux); }
inline bool linkClocksOn(const Survey &s)
{
  return coreClocksOn(s) && pianodpu::branchOn(s.clocks.link) && pianodpu::branchOn(s.clocks.linkIntf);
}
inline bool streamClockOn(const Survey &s) { return linkClocksOn(s) && pianodpu::branchOn(s.clocks.pixel0); }

// ------------------------------------------------------------------------------------------------------------ the reads
template <class Load> void surveyDpClocks(Survey &s, Load &&load)
{
  using namespace pianodpu;
  if (!branchOn(s.mdss.gccDispAhb)) return;   // DISPCC's registers sit behind GCC's display AHB clock
  DpClocks &c = s.clocks;
  c.read = true;
  const uint32_t at[] = {kCcDpLink, kCcDpRouterLinkIntf, kCcDpLinkIntf, kCcDpCrypto, kCcDpPixel0, kCcDpPixel1, kCcDpAux,
                         kCcDpAuxRcg + kRcgCmd, kCcDpAuxRcg + kRcgCfg, kCcDpLinkRcg + kRcgCmd, kCcDpLinkRcg + kRcgCfg,
                         kCcDpPixel0Rcg + kRcgCmd, kCcDpPixel0Rcg + kRcgCfg, kCcDpPixel0Rcg + kRcgM, kCcDpPixel0Rcg + kRcgN,
                         kCcDpPixel0Rcg + kRcgD, kCcDpPixel1Rcg + kRcgCmd, kCcDpPixel1Rcg + kRcgCfg, kCcDpLinkDiv};
  uint32_t *to[] = {&c.link, &c.routerLinkIntf, &c.linkIntf, &c.crypto, &c.pixel0, &c.pixel1, &c.aux, &c.auxRcg[0], &c.auxRcg[1],
                    &c.linkRcg[0], &c.linkRcg[1], &c.pixel0Rcg[0], &c.pixel0Rcg[1], &c.pixel0Rcg[2], &c.pixel0Rcg[3],
                    &c.pixel0Rcg[4], &c.pixel1Rcg[0], &c.pixel1Rcg[1], &c.linkDiv};
  static_assert(sizeof at / sizeof at[0] == sizeof to / sizeof to[0], "DISPCC DP table");
  for (unsigned i = 0; i < sizeof at / sizeof at[0]; ++i) *to[i] = load(kDispCc + at[i]);
}

template <class Load> void surveyDpCtrl(Survey &s, Load &&load)
{
  DpCtrl &d = s.ctrl;
  if (!s.mdss.readable) {
    d.why = "MDSS not powered or its AHB/MDP clocks off: the DP controller is not read";
    return;
  }
  if (!coreClocksOn(s)) {
    d.why = "DPTX0 AUX clock off (the controller's core clocks): not read (nothing natively lights DP)";
    return;
  }
  d.core = true;
  const auto ahb = [&](uint32_t off) { return load(kDpAhb + off); };
  const auto aux = [&](uint32_t off) { return load(kDpAux + off); };
  d.hwVersion = ahb(kAhbHwVersion);
  d.swReset = ahb(kAhbSwReset);
  d.phyCtrl = ahb(kAhbPhyCtrl);
  d.clkCtrl = ahb(kAhbClkCtrl);
  d.clkActive = ahb(kAhbClkActive);
  d.intr[0] = ahb(kAhbIntrStatus);
  d.intr[1] = ahb(kAhbIntrStatus2);
  d.intr[2] = ahb(kAhbIntrStatus3);
  d.intr[3] = ahb(kAhbIntrStatus4);
  d.intrMask4 = ahb(kAhbIntrMask4);
  d.hpdCtrl = aux(kAuxHpdCtrl);
  d.hpdIntStatus = aux(kAuxHpdIntStatus);
  d.hpdIntMask = aux(kAuxHpdIntMask);
  d.hpdRefTimer = aux(kAuxHpdRefTimer);
  d.hpdEventTime[0] = aux(kAuxHpdEventTime0);
  d.hpdEventTime[1] = aux(kAuxHpdEventTime1);
  d.auxCtrl = aux(kAuxCtrl);
  d.auxTransCtrl = aux(kAuxTransCtrl);
  d.auxTimeoutCount = aux(kAuxTimeoutCount);
  d.auxLimits = aux(kAuxLimits);
  d.auxStatus = aux(kAuxStatus);
  if (!linkClocksOn(s)) {
    d.why = "core clocks on; LINK or LINK_INTF clock off: the link and stream blocks are not read";
    return;
  }
  d.link = true;
  const auto link = [&](uint32_t off) { return load(kDpLink + off); };
  d.mainlinkCtrl = link(kLinkMainlinkCtrl);
  d.stateCtrl = link(kLinkStateCtrl);
  d.configCtrl = link(kLinkConfigCtrl);
  d.totalHorVer = link(kLinkTotalHorVer);
  d.activeHorVer = link(kLinkActiveHorVer);
  d.misc = link(kLinkMisc);
  d.laneMap = link(kLinkLaneMap);
  d.mainlinkReady = link(kLinkMainlinkReady);
  d.levels = link(kLinkLevels);
  d.tu = link(kLinkTu);
  if (!streamClockOn(s)) {
    d.why = "core and link clocks on; PIXEL0 off: the stream block is not read";
    return;
  }
  d.stream = true;
  const auto p0 = [&](uint32_t off) { return load(kDpP0 + off); };
  d.p0Bist = p0(kP0BistEnable);
  d.p0TimingEn = p0(kP0TimingEngineEn);
  d.p0IntfConfig = p0(kP0IntfConfig);
  d.p0HsyncCtl = p0(kP0HsyncCtl);
  d.p0VsyncPeriod = p0(kP0VsyncPeriodF0);
  d.p0DisplayHctl = p0(kP0DisplayHctl);
  d.p0Tpg = p0(kP0TpgMain);
  d.why = "every block clocked: all read";
}

template <class Load> void surveyDpPhy(Survey &s, Load &&load)
{
  using pianousbhost::kQmp;
  s.usb = pianousbhost::surveyClocks(load);
  if (!s.usb.phyOn) return;
  DpPhy &p = s.phy;
  p.read = true;
  p.com = pianousbhost::surveyQmp(s.usb, true, load);
  const auto phy = [&](uint32_t off) { return load(kQmp + kQmpDpPhy + off); };
  for (unsigned i = 0; i < 4; ++i) p.revision[i] = phy(kDpPhyRevision0 + 4 * i);
  p.cfg = phy(kDpPhyCfg);
  p.cfg1 = phy(kDpPhyCfg1);
  p.pdCtl = phy(kDpPhyPdCtl);
  p.mode = phy(kDpPhyMode);
  for (unsigned i = 0; i < kDpPhyAuxCfgCount; ++i) p.auxCfg[i] = phy(kDpPhyAuxCfg0 + 4 * i);
  p.vcoDiv = phy(kDpPhyVcoDiv);
  p.auxIntStatus = phy(kDpPhyAuxIntStatus);
  p.status = phy(kDpPhyStatus);
  p.cmnStatus = load(kQmp + kQmpDpSerdes + kSerdesCmnStatus);
  p.cReadyStatus = load(kQmp + kQmpDpSerdes + kSerdesCReadyStatus);
}

template <class Load> void surveySecondPath(Survey &s, Load &&load)
{
  using namespace pianodpu;
  if (!s.mdss.readable) return;
  SecondPath &p = s.path;
  p.read = true;
  p.intfSel = load(kMdp + kTopIntfSel);
  p.intrEn = load(kMdp + kTopIntrEn);
  p.intrStatus = load(kMdp + kTopIntrStatus);
  p.dpPhyIntfSel = load(kMdp + kTopDpPhyIntfSel);
  for (unsigned i = 0; i < kCtlCount; ++i) {
    const uint64_t b = kMdp + kCtlBase + kCtlStride * i;
    CtlRegs &c = p.ctls[i];
    const uint32_t at[] = {kCtlTop, kCtlFlush, kCtlStart, kCtlPrepare, kCtlMerge3dActive, kCtlDscActive, kCtlWbActive, kCtlCwbActive,
                           kCtlIntfActive, kCtlCdmActive, kCtlFetchActive, kCtlMerge3dFlush, kCtlDscFlush, kCtlIntfFlush,
                           kCtlPeriphFlush, kCtlPipeActive, kCtlLayerActive, kCtlIntfMaster};
    uint32_t *to[] = {&c.top, &c.flush, &c.start, &c.prepare, &c.merge3dActive, &c.dscActive, &c.wbActive, &c.cwbActive,
                      &c.intfActive, &c.cdmActive, &c.fetchActive, &c.merge3dFlush, &c.dscFlush, &c.intfFlush, &c.periphFlush,
                      &c.pipeActive, &c.layerActive, &c.intfMaster};
    static_assert(sizeof at / sizeof at[0] == sizeof to / sizeof to[0], "CTL table");
    for (unsigned k = 0; k < sizeof at / sizeof at[0]; ++k) *to[k] = load(b + at[k]);
  }
  for (unsigned i = 0; i < kLmCount; ++i) {
    const uint64_t b = kMdp + kLmBase + kLmStride * i;
    LmRegs &l = p.lms[i];
    l.opMode = load(b + kLmOpMode);
    l.outSize = load(b + kLmOutSize);
    l.bgSrcSel = load(b + kLmBgSrcSel);
    for (unsigned st = 0; st < kLmStages; ++st) {
      l.op[st] = load(b + lmStage(st) + kLmBlendOp);
      l.src[st] = load(b + lmStage(st) + kLmFgSrcSel);
    }
  }
  for (unsigned i = 0; i < 4; ++i) {
    p.merge3d[i][0] = load(kMdp + kMerge3dBase[i]);
    p.merge3d[i][1] = load(kMdp + kMerge3dBase[i] + 4);
  }
  const unsigned intfs[2] = {kIntfDp, kIntfDpMst};
  for (unsigned i = 0; i < 2; ++i) {
    const uint64_t b = kMdp + kIntfBase + kIntfStride * intfs[i];
    IntfRegs &f = p.intf[i];
    const uint32_t at[] = {kIntfEn, kIntfConfig, kIntfHsyncCtl, kIntfVsyncPeriod, kIntfVsyncWidth, kIntfDispVStart, kIntfDispVEnd,
                           kIntfDispHctl, kIntfActHctl, kIntfHsyncSkew, kIntfPolarity, kIntfConfig2, kIntfDataHctl, kIntfPanelFormat,
                           kIntfCountEn, kIntfFrameCount, kIntfMux, kIntfStatus, kIntfIntrEn, kIntfIntrStatus};
    uint32_t *to[] = {&f.en, &f.config, &f.hsyncCtl, &f.vsyncPeriod, &f.vsyncWidth, &f.dispVStart, &f.dispVEnd, &f.dispHctl,
                      &f.actHctl, &f.skew, &f.polarity, &f.config2, &f.dataHctl, &f.panelFormat, &f.countEn, &f.frameCount, &f.mux,
                      &f.status, &f.intrEn, &f.intrStatus};
    static_assert(sizeof at / sizeof at[0] == sizeof to / sizeof to[0], "INTF table");
    for (unsigned k = 0; k < sizeof at / sizeof at[0]; ++k) *to[k] = load(b + at[k]);
  }
}

// The whole survey, in the order of the file comment. Load: uint32_t(uint64_t physical address).
template <class Load> void survey(Survey &s, Load &&rawLoad)
{
  unsigned n = 0;
  auto load = [&](uint64_t pa) {
    ++n;
    return rawLoad(pa);
  };
  unsigned gateLoads = 0;
  s.mdss = pianodpu::surveyClocks(load, &gateLoads);
  surveyDpClocks(s, load);
  surveyDpCtrl(s, load);
  surveyDpPhy(s, load);
  surveySecondPath(s, load);
  s.pins = pianousbhost::surveyPins(load);
  s.loads = n;
}

// -------------------------------------------------------------------------------------------------------------- decoders
struct Version {
  unsigned major, minor, step;
};
inline Version hwVersion(uint32_t v) { return {v >> 28, (v >> 16) & 0xfffu, v & 0xffffu}; }
// HPD_INT_STATUS 31:29: 0 disconnected, 1 pending, 2 connected (msm DP_DP_HPD_STATE_STATUS_*).
inline unsigned hpdState(uint32_t hpdIntStatus) { return (hpdIntStatus >> 29) & 7u; }
inline const char *hpdStateName(unsigned s) { return s == 0 ? "disconnected" : s == 1 ? "pending" : s == 2 ? "connected" : "?"; }
// An RCG's parent and divider (clk-rcg2.c): CMD bit 31 ROOT_OFF; CFG source 10:8, half-integer divider 4:0 (2n - 1, 0 bypass).
struct Rcg {
  bool on;
  unsigned source, halfDivider, mode;
};
inline Rcg rcg(uint32_t cmd, uint32_t cfg) { return {!(cmd & kRcgRootOff), (cfg >> 8) & 7u, cfg & 0x1fu, (cfg >> 12) & 3u}; }
// CONFIGURATION_CTRL: lanes - 1 in 5:4.
inline unsigned configuredLanes(uint32_t configCtrl) { return ((configCtrl >> 4) & 3u) + 1; }
// The DP_PHY's power-down control as msm's bit names (1 = that part powered).
inline void pdCtlText(uint32_t v, char *out, unsigned size)
{
  static const char *const names[7] = {"pwrdn", "psr", "aux", "lanes01", "lanes23", "pll", "clamp"};
  unsigned at = 0;
  out[0] = 0;
  for (unsigned b = 0; b < 7 && at + 10 < size; ++b) {
    if (!((v >> b) & 1u)) continue;
    const char *n = names[b];
    if (at) out[at++] = '|';
    while (*n && at + 1 < size) out[at++] = *n++;
    out[at] = 0;
  }
  if (!at && size > 4) {
    out[0] = 'n'; out[1] = 'o'; out[2] = 'n'; out[3] = 'e'; out[4] = 0;
  }
}
// What the PD_CTL word says in one phrase.
inline const char *pdCtlVerdict(uint32_t v)
{
  if (v == 0) return "all off (reset value or never initialised)";
  if (v == kPdPoweredOff) return "msm's powered-off value (PSR only)";
  if ((v & (kPdPwrdn | kPdAux | kPdPll)) == (kPdPwrdn | kPdAux | kPdPll))
    return (v & (kPdLane01 | kPdLane23)) ? "powered with lanes (a link was up)" : "powered, AUX on, no lanes (msm's AUX-initialised state)";
  return "partly powered";
}

// ------------------------------------------------------------------------------------- the DPU's DP side: what is free
// The panel's path is every CTL whose INTF set has INTF1 or INTF2; what any CTL holds is busy. The DP path of displayport.md section
// 4.4 wants a CTL (CTL1 first), the LM pair 2/3 (DSPP2/3, PINGPONG2/3 -> MERGE_3D_1 for a frame wider than 2560) and INTF0.
struct PathView {
  int panelCtl = -1;
  unsigned panelLms = 0, panelMerge3d = 0, panelIntfs = 0;
  unsigned busyCtls = 0, busyLms = 0, busyMerge3d = 0, busyIntfs = 0;
  int dpCtl = -1;          // the first free CTL from 1, -1 none
  int dpLmPair = -1;       // the first free LM pair from pair 1 (LM2/LM3), -1 none
  bool merge3d1Free = false, intf0Free = false, intf0Running = false;
  bool fourK = false;      // CTL + LM pair 1 + MERGE_3D_1 + INTF0 all free
};
inline PathView viewPath(const SecondPath &p)
{
  using namespace pianodpu;
  PathView v;
  for (unsigned i = 0; i < kCtlCount; ++i) {
    const CtlView c = viewCtl(p.ctls[i]);
    if (c.intfs || c.lms || c.pipes || c.fetch) v.busyCtls |= 1u << i;
    v.busyLms |= c.lms;
    v.busyMerge3d |= c.merge3d;
    v.busyIntfs |= c.intfs;
    if (v.panelCtl < 0 && (c.intfs & 0x6u)) {
      v.panelCtl = int(i);
      v.panelLms = c.lms;
      v.panelMerge3d = c.merge3d;
      v.panelIntfs = c.intfs;
    }
  }
  for (unsigned i = 1; i < kCtlCount && v.dpCtl < 0; ++i)
    if (!((v.busyCtls >> i) & 1u)) v.dpCtl = int(i);
  for (unsigned pair = 1; pair < kLmCount / 2 && v.dpLmPair < 0; ++pair)
    if (!((v.busyLms >> (2 * pair)) & 3u)) v.dpLmPair = int(pair);
  v.merge3d1Free = !((v.busyMerge3d >> 1) & 1u);
  const IntfTiming t = viewIntf(p.intf[0]);
  v.intf0Running = t.enabled;
  v.intf0Free = !(v.busyIntfs & 1u) && !t.enabled;
  v.fourK = v.dpCtl >= 0 && v.dpLmPair == 1 && v.merge3d1Free && v.intf0Free;
  return v;
}

}  // namespace pianodp
