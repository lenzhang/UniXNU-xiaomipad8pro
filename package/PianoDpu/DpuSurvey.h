// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu level 1: what the survey reads, in which order, behind which gate, and what the readings say. Portable (host tests in
// tests/dpu_host.cpp): every hardware access goes through Load, a function of a physical address that returns the 32-bit word there.
// There is no store anywhere in this file, so the survey cannot write to the hardware.
//
// Order: (1) the clock gate: GCC's display AHB branch, then DISPCC's MDSS GDSC and the MDSS AHB / MDP branches. A load from an
// unclocked MDSS block can stall the bus (UniXNU's SM8750 phone ports hung or reset on exactly that), so nothing of MDSS, the DPU
// or the DSI controllers is loaded unless the gate passes. (2) MDSS top, DPU top, the 10 pipes, the 6 CTLs, the 8 LMs, the 4 DSPPs'
// PCC/GC words, MERGE_3D, the 4 DSC cores, INTF1 and INTF2 (the panel's; the DP INTFs 0 and 3 are not read), each DSI controller
// only with its own byte and pixel clocks on. (3) The panel ID pin (TLMM, always readable). (4) The
// apps SMMU's entries for the DPU's streams and their context banks. (5) Optionally (smmu level 3), the bank's own translation of
// the splash framebuffer, walked in its page tables: loads from the table memory the boot loader left, which XNU does not own.
#pragma once
#include <stdint.h>
#include "DpuHw.h"

namespace pianodpu {

// ------------------------------------------------------------------------------------------------------------- the record
struct ClockGate {
  uint32_t gccDispAhb = 0, gccDispAxi = 0, gdsc = 0, ahb = 0, nonGdscAhb = 0, mdp = 0, vsync = 0, pclk[2] = {0, 0},
           byte[2] = {0, 0}, esc0 = 0, rcgCmd = 0, rcgCfg = 0;
  bool readable = false;   // MDSS, DPU and DSI registers may be loaded
  const char *why = "";
};
struct PipeRegs {
  uint32_t srcSize, srcXy, outSize, outXy, addr[4], ystride[2], format, unpack, opMode, constColor, ubwcStatic, fetchConfig, qos,
      swStatus, ubwcError, multirect, srcSize1, srcXy1, outSize1, outXy1, format1, unpack1, opMode1;
};
struct CtlRegs {
  uint32_t top, flush, start, prepare, merge3dActive, dscActive, wbActive, cwbActive, intfActive, cdmActive, fetchActive,
      merge3dFlush, dscFlush, intfFlush, periphFlush, pipeActive, layerActive, intfMaster, dsppFlush[4];
};
struct LmRegs {
  uint32_t opMode, outSize, bgSrcSel, border0, border1;
  uint32_t op[kLmStages], src[kLmStages], alpha[kLmStages];
};
struct DsppRegs {
  uint32_t pcc[13];   // +0x1700 .. +0x1730: enable, then the constant and matrix words
  uint32_t gcEnable, gcSwap;
};
struct DscRegs {
  uint32_t mainCnf;
  uint32_t dfCtrl[2], mainConf[2], picSize[2], sliceSize[2], ctl[2], cfg[2];
};
struct IntfRegs {
  uint32_t en, config, hsyncCtl, vsyncPeriod, vsyncWidth, dispVStart, dispVEnd, actVStart, actVEnd, dispHctl, actHctl, border,
      underflowColor, skew, polarity, config2, dataHctl, panelFormat, countEn, frameCount, lineCount, progFetch, mux, status, avr,
      intrEn, intrStatus;
};
struct DsiRegs {
  uint32_t hwVersion, ctrl, status0, fifoStatus, vidCfg0, vidCfg1, activeH, activeV, total, activeHsync, vsyncHpos, vsyncVpos,
      laneStatus, laneCtrl, errMask, intrCtrl, clkCtrl, clkStatus, version, compress, timing[3];
};
struct TopRegs {
  uint32_t intfSel, intr2En, intr2Status, intrEn, intrStatus, splitEn, splitUpper, splitLower, danger, safe, vsyncSel, dceSel,
      dpPhyIntfSel;
};
struct Survey {
  ClockGate clocks;
  uint32_t mdssVersion = 0, mdssIntr = 0;
  TopRegs top{};
  PipeRegs pipes[kPipeCount]{};
  CtlRegs ctls[kCtlCount]{};
  LmRegs lms[kLmCount]{};
  DsppRegs dspps[kDsppCount]{};
  uint32_t merge3d[4][2]{};
  DscRegs dscs[kDscCount]{};
  IntfRegs intfs[kIntfCount]{};
  DsiRegs dsi[2]{};
  bool dsiRead[2] = {false, false};   // a controller is read only with its byte and pixel clocks on
  uint32_t panelIdCfg = 0, panelIdIo = 0;
  unsigned loads = 0;
};

// -------------------------------------------------------------------------------------------------------------- the gate
constexpr uint64_t kGcc = 0x00100000;
constexpr uint32_t kGccDispAhb = 0x27004, kGccDispHfAxi = 0x27008;   // msm gcc-sm8750: gcc_disp_ahb_clk (kept on), gcc_disp_hf_axi_clk
constexpr uint32_t kCcNonGdscAhb = 0xc004;                          // msm dispcc-sm8750: disp_cc_mdss_non_gdsc_ahb_clk
inline bool branchOn(uint32_t cbcr) { return !(cbcr >> 31); }       // CBCR bit 31: CLK_OFF
inline bool gdscOn(uint32_t gdscr) { return (gdscr >> 31) & 1u; }   // GDSCR bit 31: PWR_ON
// CBCR bit 0 CLK_ENABLE (the software vote), bit 1 HW_CTL (dynamic hardware clock gating: msm gcc-sm8750's hwcg_bit). A branch
// with both set is enabled and gated by the hardware while idle, so CLK_OFF reads 1 between accesses; Linux's halt check skips
// CLK_OFF for such a branch (clk-branch.c, the specification only). The tablet's GCC display AHB read 0x88000003 so (2026-11-12).
constexpr uint32_t kCbcrEnable = 1u << 0, kCbcrHwCtl = 1u << 1;
inline bool branchHwGated(uint32_t cbcr)   // all ones: no reading (an unmapped page), never a pass
{
  return cbcr != 0xffffffffu && (cbcr & (kCbcrEnable | kCbcrHwCtl)) == (kCbcrEnable | kCbcrHwCtl);
}
// The INTFs read: INTF1 and INTF2 (the panel's). INTF0/INTF3 drive DP, which nothing runs: not read.
constexpr bool intfSurveyed(unsigned i) { return i == 1 || i == 2; }

// hwGatedAhb (piano-dpu-ahb-vote=1, DpuAhbVote.h): GCC's display AHB branch also counts as on when it is enabled and hardware-gated
// (Linux's rule for it); DISPCC's GDSC and branches are checked as before. Without it: CLK_OFF must read clear.
template <class Load>
ClockGate surveyClocks(Load load, unsigned *loads, bool hwGatedAhb = false)
{
  ClockGate g;
  g.gccDispAhb = load(kGcc + kGccDispAhb);
  g.gccDispAxi = load(kGcc + kGccDispHfAxi);
  *loads += 2;
  if (!branchOn(g.gccDispAhb) && !(hwGatedAhb && branchHwGated(g.gccDispAhb))) {   // DISPCC's own registers sit behind this clock
    g.why = branchHwGated(g.gccDispAhb)
                ? "GCC display AHB clock off: DISPCC, MDSS, DPU and DSI not read (enabled and hardware-gated, CLK_OFF only says idle: "
                  "piano-dpu-ahb-vote=1 reads behind it)"
                : "GCC display AHB clock off: DISPCC, MDSS, DPU and DSI not read";
    return g;
  }
  const uint32_t at[] = {kCcMdssGdsc, kCcAhb, kCcNonGdscAhb, kCcMdp, kCcVsync, kCcPclk0, kCcPclk1, kCcByte0, kCcByte1, kCcEsc0,
                         kCcMdpRcg, kCcMdpRcg + 4};
  uint32_t *to[] = {&g.gdsc, &g.ahb, &g.nonGdscAhb, &g.mdp, &g.vsync, &g.pclk[0], &g.pclk[1], &g.byte[0], &g.byte[1], &g.esc0,
                    &g.rcgCmd, &g.rcgCfg};
  for (unsigned i = 0; i < sizeof at / sizeof at[0]; ++i) *to[i] = load(kDispCc + at[i]);
  *loads += sizeof at / sizeof at[0];
  if (!gdscOn(g.gdsc)) g.why = "MDSS GDSC off: MDSS, DPU and DSI not read";
  else if (!branchOn(g.ahb)) g.why = "MDSS AHB clock off: MDSS, DPU and DSI not read (their registers need it)";
  else if (!branchOn(g.mdp)) g.why = "MDP core clock off: MDSS, DPU and DSI not read";
  else {
    g.readable = true;
    g.why = "MDSS powered, AHB and MDP clocks on";
  }
  return g;
}

// ------------------------------------------------------------------------------------------------------------ the blocks
template <class Load>
void surveyDpu(Survey &s, Load load)
{
  unsigned n = 0;
  auto rd = [&](uint64_t pa) {
    ++n;
    return load(pa);
  };
  s.mdssVersion = rd(kMdss + kMdssHwVersion);
  s.mdssIntr = rd(kMdss + kMdssIntrStatus);
  TopRegs &t = s.top;
  t.intfSel = rd(kMdp + kTopIntfSel);
  t.intr2En = rd(kMdp + kTopIntr2En);
  t.intr2Status = rd(kMdp + kTopIntr2Status);
  t.intrEn = rd(kMdp + kTopIntrEn);
  t.intrStatus = rd(kMdp + kTopIntrStatus);
  t.splitEn = rd(kMdp + kTopSplitEn);
  t.splitUpper = rd(kMdp + kTopSplitUpper);
  t.splitLower = rd(kMdp + kTopSplitLower);
  t.danger = rd(kMdp + kTopDanger);
  t.safe = rd(kMdp + kTopSafe);
  t.vsyncSel = rd(kMdp + kTopVsyncSel);
  t.dceSel = rd(kMdp + kTopDceSel);
  t.dpPhyIntfSel = rd(kMdp + kTopDpPhyIntfSel);
  for (unsigned i = 0; i < kPipeCount; ++i) {
    const uint64_t b = kMdp + kPipes[i].base;
    PipeRegs &p = s.pipes[i];
    p.srcSize = rd(b + kSrcSize);
    p.srcXy = rd(b + kSrcXy);
    p.outSize = rd(b + kOutSize);
    p.outXy = rd(b + kOutXy);
    for (unsigned k = 0; k < 4; ++k) p.addr[k] = rd(b + kSrc0Addr + 4 * k);
    p.ystride[0] = rd(b + kYStride0);
    p.ystride[1] = rd(b + kYStride1);
    p.format = rd(b + kSrcFormat);
    p.unpack = rd(b + kUnpack);
    p.opMode = rd(b + kOpMode);
    p.constColor = rd(b + kConstColor);
    p.ubwcStatic = rd(b + kUbwcStatic);
    p.fetchConfig = rd(b + kFetchConfig);
    p.qos = rd(b + kQosCtrl);
    p.swStatus = rd(b + kAddrSwStatus);
    p.ubwcError = rd(b + kUbwcError);
    p.multirect = rd(b + kMultirect);
    p.srcSize1 = rd(b + kSrcSizeRec1);
    p.srcXy1 = rd(b + kSrcXyRec1);
    p.outSize1 = rd(b + kOutSizeRec1);
    p.outXy1 = rd(b + kOutXyRec1);
    p.format1 = rd(b + kSrcFormatRec1);
    p.unpack1 = rd(b + kUnpackRec1);
    p.opMode1 = rd(b + kOpModeRec1);
  }
  for (unsigned i = 0; i < kCtlCount; ++i) {
    const uint64_t b = kMdp + kCtlBase + kCtlStride * i;
    CtlRegs &c = s.ctls[i];
    c.top = rd(b + kCtlTop);
    c.flush = rd(b + kCtlFlush);
    c.start = rd(b + kCtlStart);
    c.prepare = rd(b + kCtlPrepare);
    c.merge3dActive = rd(b + kCtlMerge3dActive);
    c.dscActive = rd(b + kCtlDscActive);
    c.wbActive = rd(b + kCtlWbActive);
    c.cwbActive = rd(b + kCtlCwbActive);
    c.intfActive = rd(b + kCtlIntfActive);
    c.cdmActive = rd(b + kCtlCdmActive);
    c.fetchActive = rd(b + kCtlFetchActive);
    c.merge3dFlush = rd(b + kCtlMerge3dFlush);
    c.dscFlush = rd(b + kCtlDscFlush);
    c.intfFlush = rd(b + kCtlIntfFlush);
    c.periphFlush = rd(b + kCtlPeriphFlush);
    c.pipeActive = rd(b + kCtlPipeActive);
    c.layerActive = rd(b + kCtlLayerActive);
    c.intfMaster = rd(b + kCtlIntfMaster);
    for (unsigned k = 0; k < 4; ++k) c.dsppFlush[k] = rd(b + kCtlDspp0Flush + 4 * k);
  }
  for (unsigned i = 0; i < kLmCount; ++i) {
    const uint64_t b = kMdp + kLmBase + kLmStride * i;
    LmRegs &l = s.lms[i];
    l.opMode = rd(b + kLmOpMode);
    l.outSize = rd(b + kLmOutSize);
    l.bgSrcSel = rd(b + kLmBgSrcSel);
    l.border0 = rd(b + kLmBorder0);
    l.border1 = rd(b + kLmBorder1);
    for (unsigned st = 0; st < kLmStages; ++st) {
      l.op[st] = rd(b + lmStage(st) + kLmBlendOp);
      l.src[st] = rd(b + lmStage(st) + kLmFgSrcSel);
      l.alpha[st] = rd(b + lmStage(st) + kLmConstAlpha);
    }
  }
  for (unsigned i = 0; i < kDsppCount; ++i) {
    const uint64_t b = kMdp + kDsppBase + kDsppStride * i;
    for (unsigned k = 0; k < 13; ++k) s.dspps[i].pcc[k] = rd(b + kPcc + 4 * k);
    s.dspps[i].gcEnable = rd(b + kGc);
    s.dspps[i].gcSwap = rd(b + kGc + 0x1c);
  }
  for (unsigned i = 0; i < 4; ++i) {
    s.merge3d[i][0] = rd(kMdp + kMerge3dBase[i]);
    s.merge3d[i][1] = rd(kMdp + kMerge3dBase[i] + 4);
  }
  for (unsigned i = 0; i < kDscCount; ++i) {
    const uint64_t b = kMdp + kDscBase + kDscStride * i;
    DscRegs &d = s.dscs[i];
    d.mainCnf = rd(b);
    for (unsigned e = 0; e < 2; ++e) {
      d.dfCtrl[e] = rd(b + kDscEnc[e]);
      d.mainConf[e] = rd(b + kDscEnc[e] + 0x30);
      d.picSize[e] = rd(b + kDscEnc[e] + 0x34);
      d.sliceSize[e] = rd(b + kDscEnc[e] + 0x38);
      d.ctl[e] = rd(b + kDscCtl[e]);
      d.cfg[e] = rd(b + kDscCtl[e] + 4);
    }
  }
  for (unsigned i = 0; i < kIntfCount; ++i) {
    if (!intfSurveyed(i)) continue;
    const uint64_t b = kMdp + kIntfBase + kIntfStride * i;
    IntfRegs &f = s.intfs[i];
    const uint32_t at[] = {kIntfEn, kIntfConfig, kIntfHsyncCtl, kIntfVsyncPeriod, kIntfVsyncWidth, kIntfDispVStart, kIntfDispVEnd,
                           kIntfActVStart, kIntfActVEnd, kIntfDispHctl, kIntfActHctl, kIntfBorder, kIntfUnderflowColor,
                           kIntfHsyncSkew, kIntfPolarity, kIntfConfig2, kIntfDataHctl, kIntfPanelFormat, kIntfCountEn,
                           kIntfFrameCount, kIntfLineCount, kIntfProgFetch, kIntfMux, kIntfStatus, kIntfAvrControl, kIntfIntrEn,
                           kIntfIntrStatus};
    uint32_t *to[] = {&f.en, &f.config, &f.hsyncCtl, &f.vsyncPeriod, &f.vsyncWidth, &f.dispVStart, &f.dispVEnd, &f.actVStart,
                      &f.actVEnd, &f.dispHctl, &f.actHctl, &f.border, &f.underflowColor, &f.skew, &f.polarity, &f.config2,
                      &f.dataHctl, &f.panelFormat, &f.countEn, &f.frameCount, &f.lineCount, &f.progFetch, &f.mux, &f.status,
                      &f.avr, &f.intrEn, &f.intrStatus};
    static_assert(sizeof at / sizeof at[0] == sizeof to / sizeof to[0], "INTF table");
    for (unsigned k = 0; k < sizeof at / sizeof at[0]; ++k) *to[k] = rd(b + at[k]);
  }
  for (unsigned i = 0; i < 2; ++i) {
    // A DSI controller's registers are read only while its link clocks run (an unclocked DSI1 read hung an SM8650 under the same
    // hypervisor: UniXNU-sm8750-common PeripheralIO.h).
    if (!branchOn(s.clocks.byte[i]) || !branchOn(s.clocks.pclk[i])) continue;
    s.dsiRead[i] = true;
    const uint64_t b = kDsi[i];
    DsiRegs &d = s.dsi[i];
    const uint32_t at[] = {kDsiHwVersion, kDsiCtrl, kDsiStatus0, kDsiFifoStatus, kDsiVidCfg0, kDsiVidCfg1, kDsiActiveH, kDsiActiveV,
                           kDsiTotal, kDsiActiveHsync, kDsiVsyncHpos, kDsiVsyncVpos, kDsiLaneStatus, kDsiLaneCtrl, kDsiErrIntMask0,
                           kDsiIntrCtrl, kDsiClkCtrl, kDsiClkStatus, kDsiVersion, kDsiVidCompress};
    uint32_t *to[] = {&d.hwVersion, &d.ctrl, &d.status0, &d.fifoStatus, &d.vidCfg0, &d.vidCfg1, &d.activeH, &d.activeV, &d.total,
                      &d.activeHsync, &d.vsyncHpos, &d.vsyncVpos, &d.laneStatus, &d.laneCtrl, &d.errMask, &d.intrCtrl,
                      &d.clkCtrl, &d.clkStatus, &d.version, &d.compress};
    static_assert(sizeof at / sizeof at[0] == sizeof to / sizeof to[0], "DSI table");
    for (unsigned k = 0; k < sizeof at / sizeof at[0]; ++k) *to[k] = rd(b + at[k]);
    for (unsigned k = 0; k < 3; ++k) d.timing[k] = rd(b + kDsiTimingWords[k]);
  }
  s.loads += n;
}

template <class Load>
void surveyPanelId(Survey &s, Load load)
{
  const uint64_t page = kTlmm + uint64_t(kPanelIdGpio) * kTlmmStride;
  s.panelIdCfg = load(page);
  s.panelIdIo = load(page + 4);
  s.loads += 2;
}

// The whole survey: the gate first, the display blocks only behind it, the panel ID pin always.
template <class Load>
void survey(Survey &s, Load load)
{
  s.clocks = surveyClocks(load, &s.loads);
  if (s.clocks.readable) surveyDpu(s, load);
  surveyPanelId(s, load);
}

// The frame and line counters of INTF1/INTF2 only (the refresh measurement reads them twice): 4 loads.
struct Counters {
  uint32_t frame[2] = {0, 0}, line[2] = {0, 0};
};
template <class Load>
Counters readCounters(Load load)
{
  Counters c;
  for (unsigned i = 0; i < 2; ++i) {
    const uint64_t b = kMdp + kIntfBase + kIntfStride * (i + 1);
    c.frame[i] = load(b + kIntfFrameCount);
    c.line[i] = load(b + kIntfLineCount);
  }
  return c;
}

// --------------------------------------------------------------------------------------------------------------- decoders
struct Size {
  unsigned w, h;
};
inline Size size(uint32_t v) { return {v & 0xffffu, v >> 16}; }
inline Size xy(uint32_t v) { return {v & 0xffffu, v >> 16}; }   // w = x, h = y

struct Format {
  unsigned bpp;                // bytes per pixel (bits 10:9 + 1)
  unsigned bits[4];            // G, B, R, A component widths
  bool alpha;                  // SRCC3_EN: per-pixel alpha used
  unsigned fetchType;          // 0 interleaved, 1 planar, 2 pseudo-planar
  unsigned frameFormat;        // 0 linear, 1 tile, 2 UBWC
  unsigned unpackCount;
  bool tight, msb, solidFill, rot90;
  char order[5];               // memory byte order, e.g. "BGRA" (X when the alpha byte is ignored)
};
inline Format decodeFormat(uint32_t format, uint32_t unpack)
{
  static const unsigned colourBits[4] = {4, 5, 6, 8}, alphaBits[4] = {1, 4, 6, 8};
  Format f{};
  f.bits[0] = colourBits[format & 3u];
  f.bits[1] = colourBits[(format >> 2) & 3u];
  f.bits[2] = colourBits[(format >> 4) & 3u];
  f.bits[3] = alphaBits[(format >> 6) & 3u];
  f.alpha = (format >> 8) & 1u;
  f.bpp = ((format >> 9) & 3u) + 1;
  f.rot90 = (format >> 11) & 1u;
  f.unpackCount = ((format >> 12) & 3u) + 1;
  f.tight = (format >> 17) & 1u;
  f.msb = (format >> 18) & 1u;
  f.fetchType = (format >> 19) & 3u;
  f.solidFill = (format >> 22) & 1u;
  f.frameFormat = format >> 30;
  static const char names[4] = {'G', 'B', 'R', 'A'};
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned e = (unpack >> (8 * i)) & 0xffu;
    f.order[i] = i < f.unpackCount && e < 4 ? (e == 3 && !f.alpha ? 'X' : names[e]) : '-';
  }
  f.order[4] = 0;
  return f;
}
inline const char *frameFormatName(unsigned v) { return v == 0 ? "linear" : v == 1 ? "tiled" : v == 2 ? "UBWC" : "reserved"; }
// A pipe macOS's 'BGRA' IOSurface can be flipped into by changing only its source address: interleaved, linear, 4 bytes in the memory
// order B, G, R, A/X with 8-bit components, no rotation, no solid fill.
inline bool flippableBgra(const Format &f)
{
  return f.fetchType == 0 && f.frameFormat == 0 && f.bpp == 4 && f.unpackCount == 4 && !f.rot90 && !f.solidFill &&
         f.bits[0] == 8 && f.bits[1] == 8 && f.bits[2] == 8 && f.order[0] == 'B' && f.order[1] == 'G' && f.order[2] == 'R';
}

inline bool inSplash(uint32_t addr) { return addr >= kSplash && uint64_t(addr) < kSplash + kSplashBytes; }

// Where in the splash framebuffer a rect's first fetched pixel lies: the address offset plus the source position.
struct Origin {
  bool valid;
  unsigned x, y;
};
inline Origin splashOrigin(uint32_t addr, uint32_t stride, uint32_t srcXy, unsigned bpp)
{
  if (!inSplash(addr) || !stride || !bpp) return {false, 0, 0};
  const uint32_t off = addr - uint32_t(kSplash);
  return {true, (off % stride) / bpp + (srcXy & 0xffffu), off / stride + (srcXy >> 16)};
}

// A pipe as the survey sees it: does it fetch, from where, how many rects.
struct PipeView {
  bool configured;     // a non-zero source size and address
  bool rect1;          // multirect: rect 1 enabled
  bool timeMux;        // multirect time-multiplexed (else parallel)
  bool fromSplash[2];  // rect 0 / rect 1 address inside the splash framebuffer
  uint32_t addr[2], stride[2];
  Size src[2], out[2], srcPos[2], outPos[2];
  Format fmt[2];
  Origin origin[2];
};
inline PipeView viewPipe(const PipeRegs &p)
{
  PipeView v{};
  v.rect1 = (p.multirect & 1u) != 0;
  v.timeMux = (p.multirect & 4u) != 0;
  v.addr[0] = p.addr[0];
  v.addr[1] = p.addr[1];
  v.stride[0] = p.ystride[0] & 0xffffu;
  v.stride[1] = p.ystride[0] >> 16;
  v.src[0] = size(p.srcSize);
  v.src[1] = size(p.srcSize1);
  v.out[0] = size(p.outSize);
  v.out[1] = size(p.outSize1);
  v.srcPos[0] = xy(p.srcXy);
  v.srcPos[1] = xy(p.srcXy1);
  v.outPos[0] = xy(p.outXy);
  v.outPos[1] = xy(p.outXy1);
  v.fmt[0] = decodeFormat(p.format, p.unpack);
  v.fmt[1] = decodeFormat(p.format1, p.unpack1);
  for (unsigned r = 0; r < 2; ++r) {
    v.fromSplash[r] = (r == 0 || v.rect1) && inSplash(v.addr[r]);
    v.origin[r] = splashOrigin(v.addr[r], v.stride[r], r ? p.srcXy1 : p.srcXy, v.fmt[r].bpp);
  }
  v.configured = p.srcSize != 0 && p.addr[0] != 0;
  return v;
}

// A CTL path: which pipes, LMs, INTFs, DSC encoders and MERGE_3D it drives.
struct CtlView {
  bool used;           // any INTF active
  bool video;          // CTL_TOP bit 17 clear
  unsigned group;      // CTL_TOP 31:28
  unsigned intfs, lms, pipes, fetch, dsc, merge3d;   // bit sets; pipes/fetch in kPipes order
};
inline unsigned pipeBits(uint32_t active)
{
  unsigned out = 0;
  for (unsigned i = 0; i < kPipeCount; ++i)
    if ((active >> kPipes[i].activeBit) & 1u) out |= 1u << i;
  return out;
}
inline CtlView viewCtl(const CtlRegs &c)
{
  CtlView v{};
  v.intfs = c.intfActive & 0xfu;
  v.used = v.intfs != 0;
  v.video = !((c.top >> 17) & 1u);
  v.group = c.top >> 28;
  v.lms = c.layerActive & 0xffu;
  v.pipes = pipeBits(c.pipeActive);
  v.fetch = pipeBits(c.fetchActive);
  v.dsc = c.dscActive & 0xffu;
  v.merge3d = c.merge3dActive & 0x3fu;
  return v;
}

// An LM stage's source select byte: 7:6 type (0 DMA, 1 VIG, 3 none), 5:4 rect, 3:0 id. Returns the kPipes index or -1.
inline int stagedPipe(uint32_t sel, unsigned which, unsigned *rect)
{
  const unsigned b = (sel >> (8 * which)) & 0xffu, type = b >> 6, id = b & 0xfu;
  if (rect) *rect = (b >> 4) & 3u;
  if (type > 1) return -1;
  for (unsigned i = 0; i < kPipeCount; ++i)
    if (kPipes[i].vig == (type == 1) && kPipes[i].id == id) return int(i);
  return -1;
}
struct LmView {
  Size out;
  bool right;               // OP_MODE bit 31: the right mixer of a split pair
  unsigned stagedPipes;     // kPipes bit set of every pipe staged in any stage
  int firstStage;           // the lowest stage with a pipe, -1 none
  unsigned stagedCount;     // number of staged (pipe, rect) entries
};
inline LmView viewLm(const LmRegs &l)
{
  LmView v{size(l.outSize), (l.opMode >> 31) != 0, 0, -1, 0};
  for (unsigned st = 0; st < kLmStages; ++st)
    for (unsigned k = 0; k < 2; ++k) {
      const int p = stagedPipe(l.src[st], k, nullptr);
      if (p < 0) continue;
      v.stagedPipes |= 1u << p;
      ++v.stagedCount;
      if (v.firstStage < 0) v.firstStage = int(st);
    }
  return v;
}

// PCC: S3.15 coefficients in 18 bits (two's complement), 0x8000 = 1.0. In thousandths.
inline int pccMilli(uint32_t raw)
{
  int32_t v = int32_t(raw & 0x3ffffu);
  if (v & 0x20000) v -= 0x40000;
  return int((int64_t(v) * 1000 + (v >= 0 ? 16384 : -16384)) / 32768);
}
struct PccView {
  bool enabled;
  int m[3][3];       // [out][in] in thousandths
  bool identity;     // exactly the unit matrix
};
inline PccView viewPcc(const DsppRegs &d)
{
  PccView v{};
  v.enabled = d.pcc[0] & 1u;
  v.identity = true;
  for (unsigned o = 0; o < 3; ++o)
    for (unsigned i = 0; i < 3; ++i) {
      v.m[o][i] = pccMilli(d.pcc[kPccCoef[o][i] / 4]);
      if (v.m[o][i] != (o == i ? 1000 : 0)) v.identity = false;
    }
  return v;
}

// INTF timing in INTF pixel clocks (msm's encoding, see DpuHw.h).
struct IntfTiming {
  bool enabled;               // TIMING_ENGINE_EN bit 0
  bool frameCounter, lineCounter;
  unsigned htotal, hsync, hbp, hactive, hfp;
  unsigned vtotal, vsync, vbp, vactive, vfp;
  unsigned dataWidth;         // DISPLAY_DATA_HCTL's span (the compressed data, with DSC)
  bool wideBus, compressed;
  bool exact;                 // VSYNC_PERIOD_F0 is a whole number of lines
};
inline IntfTiming viewIntf(const IntfRegs &f)
{
  IntfTiming t{};
  t.enabled = f.en & 1u;
  t.frameCounter = f.countEn & 1u;
  t.lineCounter = (f.countEn >> 1) & 1u;
  t.htotal = f.hsyncCtl >> 16;
  t.hsync = f.hsyncCtl & 0xffffu;
  t.wideBus = f.config2 & 1u;
  t.compressed = (f.config2 >> 12) & 1u;
  const unsigned hstart = f.dispHctl & 0xffffu, hend = f.dispHctl >> 16;
  if (t.htotal) {
    t.hbp = hstart >= t.hsync ? hstart - t.hsync : 0;
    t.hactive = hend >= hstart ? hend - hstart + 1 : 0;
    t.hfp = t.htotal > hend + 1 ? t.htotal - hend - 1 : 0;
    t.vtotal = f.vsyncPeriod / t.htotal;
    t.exact = f.vsyncPeriod % t.htotal == 0;
    t.vsync = f.vsyncWidth / t.htotal;
    const unsigned skew = f.skew;
    const unsigned vstart = f.dispVStart >= skew ? (f.dispVStart - skew) / t.htotal : 0;
    const unsigned vend = f.dispVEnd + 1 >= skew ? (f.dispVEnd + 1 - skew) / t.htotal : 0;
    t.vbp = vstart >= t.vsync ? vstart - t.vsync : 0;
    t.vactive = vend >= vstart ? vend - vstart : 0;
    t.vfp = t.vtotal >= vend ? t.vtotal - vend : 0;
  }
  const unsigned ds = f.dataHctl & 0xffffu, de = f.dataHctl >> 16;
  t.dataWidth = de >= ds && f.dataHctl ? de - ds + 1 : 0;
  return t;
}

// DSI video timing of one controller (msm's encoding, see DpuHw.h): per controller, compressed active part, porches as they are.
struct DsiTiming {
  bool enabled, video, command, clockLane;
  unsigned lanes;
  unsigned htotal, hsync, hbp, hactive, hfp;
  unsigned vtotal, vsync, vbp, vactive, vfp;
  bool compression;           // VIDEO_COMPRESSION_MODE_CTRL bit 0
  unsigned compressWc;        // its word count
};
inline unsigned popcount4(uint32_t v) { return (v & 1u) + ((v >> 1) & 1u) + ((v >> 2) & 1u) + ((v >> 3) & 1u); }
inline DsiTiming viewDsi(const DsiRegs &d)
{
  DsiTiming t{};
  t.enabled = d.ctrl & 1u;
  t.video = (d.ctrl >> 1) & 1u;
  t.command = (d.ctrl >> 2) & 1u;
  t.lanes = popcount4(d.ctrl >> 4);
  t.clockLane = (d.ctrl >> 8) & 1u;
  const unsigned hs = d.activeH & 0xffffu, he = d.activeH >> 16, vs = d.activeV & 0xffffu, ve = d.activeV >> 16;
  t.htotal = (d.total & 0xffffu) + 1;
  t.vtotal = (d.total >> 16) + 1;
  t.hsync = (d.activeHsync >> 16) >= (d.activeHsync & 0xffffu) ? (d.activeHsync >> 16) - (d.activeHsync & 0xffffu) : 0;
  t.hbp = hs >= t.hsync ? hs - t.hsync : 0;
  t.hactive = he >= hs ? he - hs : 0;
  t.hfp = t.htotal >= he ? t.htotal - he : 0;
  t.vsync = (d.vsyncVpos >> 16) >= (d.vsyncVpos & 0xffffu) ? (d.vsyncVpos >> 16) - (d.vsyncVpos & 0xffffu) : 0;
  t.vbp = vs >= t.vsync ? vs - t.vsync : 0;
  t.vactive = ve >= vs ? ve - vs : 0;
  t.vfp = t.vtotal >= ve ? t.vtotal - ve : 0;
  t.compression = d.compress & 1u;
  t.compressWc = d.compress >> 16;
  return t;
}
// The Ubuntu panel mode a bonded pair's timing is (each controller has half the horizontal porches): -1 none.
inline int matchMode(const DsiTiming &t, bool bonded)
{
  const unsigned hfp = bonded ? 2 * t.hfp : t.hfp, hsync = bonded ? 2 * t.hsync : t.hsync, hbp = bonded ? 2 * t.hbp : t.hbp;
  for (unsigned i = 0; i < sizeof kModes / sizeof kModes[0]; ++i)
    if (kModes[i].hfp == hfp && hsync == kHsync && hbp == kHbp && t.vtotal == modeVtotal(kModes[i]) && t.vsync == kVsync &&
        t.vbp == kVbp && t.vactive == kPanelHeight)
      return int(i);
  return -1;
}

// The refresh from two frame-counter readings dt_ns apart, in thousandths of a hertz (0 when the counter did not move or dt is 0).
// The counter is 32 bits and may wrap once.
inline uint64_t refreshMilliHz(uint32_t frames0, uint32_t frames1, uint64_t dtNs)
{
  const uint32_t frames = frames1 - frames0;
  if (!dtNs || !frames || frames > 1000000) return 0;   // a counter that did not move, or garbage
  return (uint64_t(frames) * 1000000000000ull + dtNs / 2) / dtNs;
}
// The INTF pixel clock the measured refresh implies (VSYNC_PERIOD_F0 clocks per frame), in kHz.
inline uint64_t intfPixelKHz(uint64_t milliHz, uint32_t vsyncPeriod) { return milliHz * vsyncPeriod / 1000000; }

// The panel module from the ID pin's input level (TLMM IO bit 0).
inline const char *panelModule(uint32_t io) { return (io & 1u) ? "p81 35-02-0b" : "p81 42-02-0a"; }
// Its rate-group register order (XML / panel driver): 42-02-0a writes 0xB3 then 0xB2, 35-02-0b 0xB2 then 0xB3.
inline const char *rateOrder(uint32_t io) { return (io & 1u) ? "B2 then B3" : "B3 then B2"; }

// --------------------------------------------------------------------------------------------- the path, in a few words
struct Path {
  unsigned splashPipes = 0;    // kPipes bit set of pipes with a rect fetching the splash framebuffer
  unsigned splashRects = 0;    // how many rects fetch it
  int ctl = -1;                // the CTL driving INTF1 and/or INTF2 (the panel)
  unsigned ctlCount = 0;       // how many CTLs drive a DSI INTF
  CtlView view{};
  bool dualDsi = false;        // INTF1 and INTF2 both active on that CTL
  bool flippable = false;      // every splash rect: flippableBgra and the splash stride
  bool stagedOk = false;       // every splash pipe is staged in an active LM of that CTL
  bool alpha = false;          // a splash rect uses per-pixel alpha (SRCC3_EN)
  uint32_t stride = 0;
};
inline Path viewPath(const Survey &s)
{
  Path p;
  bool allFlip = true;
  for (unsigned i = 0; i < kPipeCount; ++i) {
    const PipeView v = viewPipe(s.pipes[i]);
    for (unsigned r = 0; r < 2; ++r) {
      if (!v.fromSplash[r]) continue;
      p.splashPipes |= 1u << i;
      ++p.splashRects;
      p.stride = v.stride[r];
      p.alpha |= v.fmt[r].alpha;
      allFlip &= flippableBgra(v.fmt[r]) && v.stride[r] == kSplashStride;
    }
  }
  p.flippable = p.splashRects && allFlip;
  for (unsigned c = 0; c < kCtlCount; ++c) {
    const CtlView v = viewCtl(s.ctls[c]);
    if (!(v.intfs & 0x6u)) continue;
    ++p.ctlCount;
    if (p.ctl < 0) {
      p.ctl = int(c);
      p.view = v;
    }
  }
  if (p.ctl >= 0) {
    p.dualDsi = (p.view.intfs & 0x6u) == 0x6u;
    unsigned staged = 0;
    for (unsigned l = 0; l < kLmCount; ++l)
      if ((p.view.lms >> l) & 1u) staged |= viewLm(s.lms[l]).stagedPipes;
    p.stagedOk = p.splashPipes && (p.splashPipes & ~staged) == 0;
  }
  return p;
}
// CTL_FLUSH bits an address-only flip of the splash pipes sets (their SSPP bits; the LMs and INTFs are unchanged).
inline uint32_t flipFlushMask(unsigned pipes)
{
  uint32_t m = 0;
  for (unsigned i = 0; i < kPipeCount; ++i)
    if ((pipes >> i) & 1u) m |= 1u << kPipes[i].flushBit;
  return m;
}

// --------------------------------------------------------------------------------------------------- apps SMMU, read only
// Every stream-match group covering one of the DPU's streams 0x800..0x803 (ARM SMMUv2: SMR valid 31, mask 30:16, id 14:0; S2CR
// type 17:16: 0 translate through bank 7:0, 1 bypass, 2 fault), and for a translating group its bank: CBAR (GR1 +4b: type 17:16, 1 =
// stage 1 with stage 2 bypass), CBA2R (GR1 0x800 +4b: bit 0 VA64), SCTLR (M bit 0), TCR2 (PASize 2:0, SEP 17:15), TTBR0, TCR (T0SZ
// 5:0, TG0 15:14, EPD1 23), MAIR0, FSR. smmu level 1 reads the global pages only; level 2 also the bank's page (read only, as
// PianoWifi's survey on the same SMMU).
struct SmmuBank {
  uint32_t cbar = 0, cba2r = 0, sctlr = 0, tcr2 = 0, ttbr0lo = 0, ttbr0hi = 0, tcr = 0, mair0 = 0, fsr = 0;
  bool read = false;
};
struct SmmuEntry {
  unsigned group = 0;
  uint32_t smr = 0, s2cr = 0;
  SmmuBank bank;
};
struct SmmuSurvey {
  uint32_t scr0 = 0, idr0 = 0, idr1 = 0, groups = 0, banks = 0, pages = 0;
  const char *error = nullptr;
  SmmuEntry found[6];
  unsigned count = 0, more = 0;
  unsigned loads = 0;
};
inline bool smrCovers(uint32_t smr, uint16_t sid)
{
  const uint32_t mask = (smr >> 16) & 0x7fffu;
  return ((smr ^ sid) & 0x7fffu & ~mask) == 0;
}
template <class Read>
SmmuSurvey surveySmmu(unsigned level, Read rd)   // rd(offset from kSmmu)
{
  SmmuSurvey r;
  auto ld = [&](uint32_t off) {
    ++r.loads;
    return rd(off);
  };
  r.scr0 = ld(0x0);
  r.idr0 = ld(0x20);
  r.idr1 = ld(0x24);
  r.groups = r.idr0 & 0xffu;
  if (r.groups > 128) r.groups = 128;   // Qualcomm: groups above 128 do not behave architecturally
  r.banks = r.idr1 & 0xffu;
  r.pages = 1u << (((r.idr1 >> 28) & 7u) + 1);
  if ((r.idr1 >> 31) || !r.groups || !r.banks || r.banks > 128 || r.pages > 128 || uint64_t(r.pages + r.banks) * 4096 > kSmmuBytes) {
    r.error = "unsupported SMMU geometry (64 KiB pages or out of the 1 MiB window)";
    return r;
  }
  for (unsigned i = 0; i < r.groups; ++i) {
    const uint32_t smr = ld(0x800 + 4 * i);
    if (!(smr & 0x80000000u)) continue;
    bool hit = false;
    for (uint16_t sid = kStreamDpu; sid <= kStreamDpu + 3; ++sid) hit |= smrCovers(smr, sid);
    if (!hit) continue;
    if (r.count >= sizeof r.found / sizeof r.found[0]) {
      ++r.more;
      continue;
    }
    SmmuEntry &e = r.found[r.count++];
    e.group = i;
    e.smr = smr;
    e.s2cr = ld(0xc00 + 4 * i);
    const unsigned type = (e.s2cr >> 16) & 3u, bank = e.s2cr & 0xffu;
    if (type != 0 || bank >= r.banks) continue;
    e.bank.cbar = ld(0x1000 + 4 * bank);
    e.bank.cba2r = ld(0x1800 + 4 * bank);
    if (level < 2) continue;
    const uint32_t cb = (r.pages + bank) * 4096;
    e.bank.sctlr = ld(cb + 0x00);
    e.bank.tcr2 = ld(cb + 0x10);
    e.bank.ttbr0lo = ld(cb + 0x20);
    e.bank.ttbr0hi = ld(cb + 0x24);
    e.bank.tcr = ld(cb + 0x30);
    e.bank.mair0 = ld(cb + 0x38);
    e.bank.fsr = ld(cb + 0x58);
    e.bank.read = true;
  }
  return r;
}
inline const char *s2crType(uint32_t s2cr)
{
  const unsigned t = (s2cr >> 16) & 3u;
  return t == 0 ? "translate" : t == 1 ? "bypass" : t == 2 ? "fault" : "reserved";
}
inline const char *cbarType(uint32_t cbar)
{
  const unsigned t = (cbar >> 16) & 3u;
  return t == 0 ? "stage 2 only" : t == 1 ? "stage 1, stage 2 bypass" : t == 2 ? "stage 1, stage 2 fault" : "stage 1 + stage 2";
}
inline uint64_t ttbr0Base(const SmmuBank &b) { return (uint64_t(b.ttbr0hi & 0xffffu) << 32 | b.ttbr0lo) & ~0xfffull; }
inline unsigned tcrT0sz(uint32_t tcr) { return tcr & 0x3fu; }
inline unsigned tcrGranuleKiB(uint32_t tcr)
{
  const unsigned tg = (tcr >> 14) & 3u;
  return tg == 0 ? 4 : tg == 1 ? 64 : tg == 2 ? 16 : 0;
}

// What the DPU's stream route means for scanning out an IOSurface (all of them lie above 4 GiB, the fetch address is 32 bits).
enum class Route : uint8_t {
  unread,        // the SMMU was not read (or its geometry refused)
  none,          // no stream-match entry: sCR0's unmatched-stream policy applies
  bypass,        // S2CR bypass: fetch address = physical address
  fault,         // S2CR fault
  bankOff,       // a stage-1 bank with translation off (SCTLR.M = 0): physical addresses
  bankOn,        // a stage-1 bank with translation on: the boot loader's page tables
  bankUnread,    // a translating group whose bank page was not read (smmu level 1)
  other,         // a bank of another kind (stage 2, nested)
};
inline Route route(const SmmuSurvey &s)
{
  if (s.error || !s.idr1) return Route::unread;
  const SmmuEntry *e = nullptr;
  for (unsigned i = 0; i < s.count && !e; ++i)
    if (smrCovers(s.found[i].smr, kStreamDpu)) e = &s.found[i];
  if (!e) return Route::none;
  const unsigned type = (e->s2cr >> 16) & 3u;
  if (type == 1) return Route::bypass;
  if (type != 0) return Route::fault;
  if (((e->bank.cbar >> 16) & 3u) != 1) return Route::other;
  if (!e->bank.read) return Route::bankUnread;
  return (e->bank.sctlr & 1u) ? Route::bankOn : Route::bankOff;
}
inline const char *routeVerdict(Route r)
{
  switch (r) {
  case Route::unread: return "SMMU route not read";
  case Route::none: return "no stream-match entry for 0x800: the SMMU's unmatched-stream policy applies (no translation to use)";
  case Route::bypass:
    return "0x800 bypasses: fetch addresses are physical, and no IOSurface (all above 4 GiB) is reachable by a 32-bit address; "
           "zero-copy needs a translating bank, which moves the live stream (reset risk)";
  case Route::fault: return "0x800 faults: the DPU cannot fetch at all (the panel is not fed by the DPU?)";
  case Route::bankOff:
    return "0x800 -> a stage-1 bank with translation off (IOVA = PA): ShengUfs's recipe applies: turn translation on in place "
           "with an identity map of 0x80000000-0xffffffff (the splash) plus IOSurface windows below 2 GiB";
  case Route::bankOn:
    return "0x800 -> a stage-1 bank with translation on (the boot loader's tables): zero-copy must replace that table, keeping "
           "whatever it maps (walk it: piano-dpu-smmu=3)";
  case Route::bankUnread: return "0x800 -> a translating group; its bank was not read (piano-dpu-smmu=2 reads it)";
  case Route::other: return "0x800 -> a bank that is not stage 1 / stage 2 bypass: not what the boot loader is expected to leave";
  }
  return "";
}

// ------------------------------------------------------------------------------------------- the bank's translation (level 3)
// A 4 KiB-granule stage-1 walk of one IOVA in the bank's TTBR0 tables, at most 4 loads, each a descriptor of the boot loader's
// tables. Load64(pa) returns the 64-bit descriptor at a physical address. Stops at the first invalid or block/page descriptor.
struct Walk {
  bool ok = false;           // reached a block or page
  uint64_t pa = 0;           // the output address of the IOVA
  unsigned level = 0, loads = 0;
  uint64_t desc[4] = {0, 0, 0, 0};
  const char *error = nullptr;
};
template <class Load64>
Walk walk(const SmmuBank &b, uint64_t iova, Load64 load64)
{
  Walk w;
  const unsigned t0sz = tcrT0sz(b.tcr);
  if (tcrGranuleKiB(b.tcr) != 4) {
    w.error = "not a 4 KiB granule";
    return w;
  }
  if (t0sz < 16 || t0sz > 39) {
    w.error = "T0SZ out of range";
    return w;
  }
  const unsigned vaBits = 64 - t0sz;
  if (vaBits < 64 && (iova >> vaBits)) {
    w.error = "IOVA beyond the bank's input range";
    return w;
  }
  unsigned level = vaBits > 39 ? 0 : vaBits > 30 ? 1 : 2;
  uint64_t table = ttbr0Base(b);
  if (!table) {
    w.error = "TTBR0 is zero";
    return w;
  }
  for (; level <= 3; ++level) {
    const unsigned shift = 39 - 9 * level;
    const uint64_t index = (iova >> shift) & 0x1ffu;
    const uint64_t d = load64(table + 8 * index);
    w.desc[w.loads++] = d;
    w.level = level;
    if (!(d & 1u)) {
      w.error = "invalid descriptor";
      return w;
    }
    const uint64_t out = d & 0x0000fffffffff000ull;
    if (level == 3 || !(d & 2u)) {   // page (level 3) or block (levels 1-2)
      if (level == 0 || (level == 3 && !(d & 2u))) {   // no level-0 blocks with 4 KiB; 0b01 at level 3 is reserved
        w.error = "reserved descriptor";
        return w;
      }
      const uint64_t span = 1ull << shift;
      w.pa = (out & ~(span - 1)) | (iova & (span - 1));
      w.ok = true;
      return w;
    }
    table = out;
  }
  w.error = "walk too deep";
  return w;
}

}  // namespace pianodpu
