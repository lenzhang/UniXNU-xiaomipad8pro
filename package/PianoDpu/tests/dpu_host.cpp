// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu's level 1 survey (DpuSurvey.h) against register models:
//   - the gate: nothing of DISPCC is loaded with GCC's display AHB clock off, nothing of MDSS/DPU/DSI with the GDSC, the AHB or the
//     MDP clock off; with the gate open, every load lies in GCC's two words, DISPCC, MDSS/DPU, the two DSI controllers or the
//     panel ID pin, never in VBIF, REGDMA, a DSI PHY or the DP controller;
//   - the decoders: rect sizes, the source format and its memory byte order, splash origins, multirect, CTL paths, LM source
//     selects, PCC S3.15 coefficients, INTF and DSI timing, the Ubuntu mode match (144/120/90/60), the refresh arithmetic;
//   - the path verdict on a modelled boot-loader state (two VIG pipes, CTL0, LM0/LM1, DSC, INTF1+INTF2) and its flip flush mask;
//   - the SMMU scan (group match under a mask, the bank registers only at level 2, the route verdicts) and the level-3 walk.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_host.cpp -o /tmp/dpu_host && /tmp/dpu_host
#include "DpuSurvey.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace pianodpu;
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
  void set(uint64_t pa, uint32_t v) { regs[pa] = v; }
  void clocksOn()
  {
    set(kGcc + kGccDispAhb, 0x00000001);
    set(kDispCc + kCcMdssGdsc, 0x80000000);
    set(kDispCc + kCcAhb, 0x00000001);
    set(kDispCc + kCcMdp, 0x00000001);
  }
  void clockOff(uint32_t cbcr) { set(kDispCc + cbcr, 0x80000000); }
};

// One INTF's timing in msm's encoding (per INTF, compressed active width).
static void intfTiming(Soc &soc, unsigned intf, unsigned hsync, unsigned hbp, unsigned hactive, unsigned hfp, unsigned vsync,
                       unsigned vbp, unsigned vactive, unsigned vfp)
{
  const uint64_t b = kMdp + kIntfBase + kIntfStride * intf;
  const unsigned htotal = hsync + hbp + hactive + hfp, vtotal = vsync + vbp + vactive + vfp;
  soc.set(b + kIntfEn, 1);
  soc.set(b + kIntfHsyncCtl, htotal << 16 | hsync);
  soc.set(b + kIntfVsyncPeriod, vtotal * htotal);
  soc.set(b + kIntfVsyncWidth, vsync * htotal);
  soc.set(b + kIntfDispVStart, (vsync + vbp) * htotal);
  soc.set(b + kIntfDispVEnd, (vtotal - vfp) * htotal - 1);
  soc.set(b + kIntfDispHctl, (htotal - hfp - 1) << 16 | (hsync + hbp));
  soc.set(b + kIntfConfig2, 0x1011);   // wide bus, data HCTL, DSC
  soc.set(b + kIntfCountEn, 3);
}
// One DSI controller's video timing in msm's encoding (per link).
static void dsiTiming(Soc &soc, unsigned i, unsigned hsync, unsigned hbp, unsigned hactive, unsigned hfp, unsigned vsync, unsigned vbp,
                      unsigned vactive, unsigned vfp)
{
  const uint64_t b = kDsi[i];
  const unsigned htotal = hsync + hbp + hactive + hfp, vtotal = vsync + vbp + vactive + vfp;
  const unsigned hs = hsync + hbp, vs = vsync + vbp;
  soc.set(b + kDsiCtrl, 0x1f3);   // enable, video, 4 lanes, clock lane
  soc.set(b + kDsiActiveH, (hs + hactive) << 16 | hs);
  soc.set(b + kDsiActiveV, (vs + vactive) << 16 | vs);
  soc.set(b + kDsiTotal, (vtotal - 1) << 16 | (htotal - 1));
  soc.set(b + kDsiActiveHsync, hsync << 16);
  soc.set(b + kDsiVsyncVpos, vsync << 16);
  soc.set(b + kDsiVidCompress, 0x0c000001 | 0x0b << 8);
}

// The modelled boot-loader state: VIG0 left half, VIG1 right half (SRC_XY 1600), both 'BGRA' with alpha, CTL0 video mode on
// INTF1+INTF2 with LM0/LM1 and DSC 0/1, the 120 Hz timing.
static void bootState(Soc &soc)
{
  soc.clocksOn();
  for (unsigned v = 0; v < 2; ++v) {
    const uint64_t b = kMdp + kPipes[v].base;
    soc.set(b + kSrcSize, 2136u << 16 | 1600);
    soc.set(b + kOutSize, 2136u << 16 | 1600);
    soc.set(b + kSrcXy, v ? 1600 : 0);
    soc.set(b + kSrc0Addr, uint32_t(kSplash));
    soc.set(b + kYStride0, kSplashStride);
    soc.set(b + kSrcFormat, 0x000237ff);
    soc.set(b + kUnpack, 0x03020001);
  }
  const uint64_t c = kMdp + kCtlBase;
  soc.set(c + kCtlTop, 0xf0000000);
  soc.set(c + kCtlIntfActive, 0x6);
  soc.set(c + kCtlIntfMaster, 0x2);
  soc.set(c + kCtlLayerActive, 0x3);
  soc.set(c + kCtlPipeActive, 1u << 16 | 1u << 17);
  soc.set(c + kCtlFetchActive, 1u << 16 | 1u << 17);
  soc.set(c + kCtlDscActive, 0x3);
  for (unsigned l = 0; l < kLmCount; ++l)
    for (unsigned st = 0; st < kLmStages; ++st) soc.set(kMdp + kLmBase + kLmStride * l + lmStage(st) + kLmFgSrcSel, 0xc0c0);
  soc.set(kMdp + kLmBase + lmStage(0) + kLmFgSrcSel, 0xc040);                  // VIG0
  soc.set(kMdp + kLmBase + kLmStride + lmStage(0) + kLmFgSrcSel, 0xc041);      // VIG1
  soc.set(kMdp + kLmBase + kLmOutSize, 2136u << 16 | 1600);
  soc.set(kMdp + kLmBase + kLmStride + kLmOutSize, 2136u << 16 | 1600);
  soc.set(kMdp + kLmBase + kLmStride + kLmOpMode, 0x80000000);
  for (unsigned i = 1; i <= 2; ++i) intfTiming(soc, i, 16, 16, 427, 117, 2, 104, 2136, 68);
  for (unsigned i = 0; i < 2; ++i) dsiTiming(soc, i, 16, 16, 427, 117, 2, 104, 2136, 68);
}

// ----------------------------------------------------------------------------------------------------------------- tests
static void testGate()
{
  {   // GCC's display AHB off: only GCC's two words, then the panel ID pin
    Soc soc;
    soc.set(kGcc + kGccDispAhb, 0x80000000);
    Survey s;
    survey(s, [&](uint64_t pa) { return soc.load(pa); });
    CHECK(!s.clocks.readable);
    CHECK(soc.in(kDispCc, 0x20000) == 0);
    CHECK(soc.in(kMdss, 0x94000) == 0);
    CHECK(soc.in(kDsi[0], 0x3000) == 0);
    CHECK(soc.loads.size() == 4);
  }
  {   // AHB branch off: DISPCC read, nothing of MDSS
    Soc soc;
    soc.clocksOn();
    soc.set(kDispCc + kCcAhb, 0x80000000);
    Survey s;
    survey(s, [&](uint64_t pa) { return soc.load(pa); });
    CHECK(!s.clocks.readable);
    CHECK(soc.in(kDispCc, 0x20000) > 0);
    CHECK(soc.in(kMdss, 0x94000) == 0);
    CHECK(soc.in(kDsi[0], 0x3000) == 0);
    CHECK(std::strstr(s.clocks.why, "AHB") != nullptr);
  }
  {   // GDSC off
    Soc soc;
    soc.clocksOn();
    soc.set(kDispCc + kCcMdssGdsc, 0);
    Survey s;
    survey(s, [&](uint64_t pa) { return soc.load(pa); });
    CHECK(!s.clocks.readable);
    CHECK(soc.in(kMdss, 0x94000) == 0);
  }
  {   // MDP core clock off
    Soc soc;
    soc.clocksOn();
    soc.set(kDispCc + kCcMdp, 0x80000000);
    Survey s;
    survey(s, [&](uint64_t pa) { return soc.load(pa); });
    CHECK(!s.clocks.readable);
    CHECK(soc.in(kMdss, 0x94000) == 0);
  }
  {   // gate open: every load in an allowed block, none in VBIF, REGDMA, DSI PHYs, DP
    Soc soc;
    bootState(soc);
    Survey s;
    survey(s, [&](uint64_t pa) { return soc.load(pa); });
    CHECK(s.clocks.readable);
    unsigned bad = 0;
    for (uint64_t pa : soc.loads) {
      const bool ok = pa == kGcc + kGccDispAhb || pa == kGcc + kGccDispHfAxi || (pa >= kDispCc && pa < kDispCc + 0x20000) ||
                      (pa >= kMdss && pa < kMdp + kMdpBytes) || (pa >= kDsi[0] && pa < kDsi[0] + kDsiBytes) ||
                      (pa >= kDsi[1] && pa < kDsi[1] + kDsiBytes) ||
                      (pa >= kTlmm + kPanelIdGpio * kTlmmStride && pa < kTlmm + (kPanelIdGpio + 1) * kTlmmStride);
      bad += !ok;
    }
    CHECK(bad == 0);
    CHECK(soc.in(kVbif, 0x3000) == 0);
    CHECK(soc.in(kRegDma, 0x7000) == 0);
    CHECK(soc.in(kDsiPhy[0], 0x1000) == 0 && soc.in(kDsiPhy[1], 0x1000) == 0);
    CHECK(soc.in(0x0af54000, 0x4000) == 0);
    CHECK(soc.in(kMdp + kIntfBase, kIntfStride) == 0);                       // INTF0 (DP)
    CHECK(soc.in(kMdp + kIntfBase + 3 * kIntfStride, kIntfStride) == 0);     // INTF3 (DP)
    CHECK(soc.in(kMdp + kIntfBase + kIntfStride, kIntfStride) > 20);         // INTF1
    CHECK(s.dsiRead[0] && s.dsiRead[1] && soc.in(kDsi[1], kDsiBytes) > 20);
    CHECK(s.loads == soc.loads.size());
    CHECK(soc.loads.size() > 850 && soc.loads.size() < 1200);
  }
  {   // DSI1's byte clock off: DSI1 not read, DSI0 and the DPU still are
    Soc soc;
    bootState(soc);
    soc.clockOff(kCcByte1);
    Survey s;
    survey(s, [&](uint64_t pa) { return soc.load(pa); });
    CHECK(s.clocks.readable && s.dsiRead[0] && !s.dsiRead[1]);
    CHECK(soc.in(kDsi[1], kDsiBytes) == 0 && soc.in(kDsi[0], kDsiBytes) > 20);
    soc.loads.clear();
    soc.clockOff(kCcPclk0);
    Survey s2;
    survey(s2, [&](uint64_t pa) { return soc.load(pa); });
    CHECK(!s2.dsiRead[0] && soc.in(kDsi[0], kDsiBytes) == 0);
  }
}

static void testFormat()
{
  const Format a = decodeFormat(0x000237ff, 0x03020001);   // msm ARGB8888: memory B, G, R, A
  CHECK(std::strcmp(a.order, "BGRA") == 0);
  CHECK(a.alpha && a.bpp == 4 && a.unpackCount == 4 && a.tight && a.frameFormat == 0 && a.fetchType == 0);
  CHECK(a.bits[0] == 8 && a.bits[1] == 8 && a.bits[2] == 8 && a.bits[3] == 8);
  CHECK(flippableBgra(a));
  const Format x = decodeFormat(0x000236ff, 0x03020001);   // XRGB8888: alpha byte ignored
  CHECK(std::strcmp(x.order, "BGRX") == 0 && !x.alpha && flippableBgra(x));
  const Format rgba = decodeFormat(0x000237ff, 0x03010002);   // memory R, G, B, A: not macOS's 'BGRA'
  CHECK(std::strcmp(rgba.order, "RGBA") == 0 && !flippableBgra(rgba));
  const Format ubwc = decodeFormat(0x800237ffu, 0x03020001);
  CHECK(ubwc.frameFormat == 2 && !flippableBgra(ubwc));
  const Format rgb565 = decodeFormat(0x00022216, 0x00020001);   // RGB565: 2 bytes, 3 elements
  CHECK(rgb565.bpp == 2 && rgb565.unpackCount == 3 && rgb565.bits[0] == 6 && !flippableBgra(rgb565));
  CHECK(std::strcmp(frameFormatName(2), "UBWC") == 0);
}

static void testPipes()
{
  PipeRegs p{};
  p.srcSize = 2136u << 16 | 1600;
  p.addr[0] = uint32_t(kSplash);
  p.addr[1] = uint32_t(kSplash) + 6400;
  p.ystride[0] = kSplashStride << 16 | kSplashStride;
  p.format = p.format1 = 0x000237ff;
  p.unpack = p.unpack1 = 0x03020001;
  p.srcSize1 = 2136u << 16 | 1600;
  PipeView v = viewPipe(p);
  CHECK(v.configured && !v.rect1 && v.fromSplash[0] && !v.fromSplash[1]);   // rect 1 off: not counted
  p.multirect = 1;   // parallel, two rects
  v = viewPipe(p);
  CHECK(v.rect1 && !v.timeMux && v.fromSplash[0] && v.fromSplash[1]);
  CHECK(v.origin[0].valid && v.origin[0].x == 0 && v.origin[0].y == 0);
  CHECK(v.origin[1].valid && v.origin[1].x == 1600 && v.origin[1].y == 0);
  CHECK(v.stride[0] == kSplashStride && v.stride[1] == kSplashStride);
  p.multirect = 5;
  CHECK(viewPipe(p).timeMux);
  const Origin o = splashOrigin(uint32_t(kSplash) + 10 * kSplashStride + 400, kSplashStride, 1u << 16 | 3, 4);
  CHECK(o.valid && o.x == 103 && o.y == 11);
  CHECK(!splashOrigin(0x80000000, kSplashStride, 0, 4).valid);
  CHECK(inSplash(uint32_t(kSplash + kSplashBytes - 4)) && !inSplash(uint32_t(kSplash + kSplashBytes)));
  const Size sz = size(2136u << 16 | 3200);
  CHECK(sz.w == 3200 && sz.h == 2136);
}

static void testCtlLm()
{
  CtlRegs c{};
  c.top = 0xf0000000;
  c.intfActive = 0x6;
  c.layerActive = 0x3;
  c.pipeActive = 1u << 16 | 1u << 17 | 1u << 0;   // VIG0, VIG1, DMA0
  c.dscActive = 0x3;
  CtlView v = viewCtl(c);
  CHECK(v.used && v.video && v.group == 15 && v.intfs == 0x6 && v.lms == 0x3 && v.dsc == 0x3);
  CHECK(v.pipes == (1u << 0 | 1u << 1 | 1u << 4));   // kPipes order: VIG0, VIG1, DMA0
  c.top |= 1u << 17;
  CHECK(!viewCtl(c).video);
  unsigned rect = 9;
  CHECK(stagedPipe(0xc040, 0, &rect) == 0 && rect == 0);       // VIG0
  CHECK(stagedPipe(0xc040, 1, nullptr) == -1);                 // none
  CHECK(stagedPipe(0x0051, 0, &rect) == 1 && rect == 1);       // VIG1 rect 1
  CHECK(stagedPipe(0x0051, 1, nullptr) == 4);                  // DMA0
  CHECK(stagedPipe(0x0005, 0, nullptr) == 9);                  // DMA5
  CHECK(stagedPipe(0x000f, 0, nullptr) == -1);                 // no DMA15
  LmRegs l{};
  for (unsigned st = 0; st < kLmStages; ++st) l.src[st] = 0xc0c0;
  l.src[2] = 0x0041;   // VIG1 + DMA0 in stage 2
  l.opMode = 0x80000000;
  const LmView lv = viewLm(l);
  CHECK(lv.right && lv.firstStage == 2 && lv.stagedCount == 2 && lv.stagedPipes == (1u << 1 | 1u << 4));
  CHECK(flipFlushMask(0x3) == 0x3);                       // VIG0, VIG1
  CHECK(flipFlushMask(1u << 3 | 1u << 4) == (1u << 18 | 1u << 11));   // VIG3, DMA0
}

static void testPcc()
{
  CHECK(pccMilli(0x8000) == 1000);
  CHECK(pccMilli(0) == 0);
  CHECK(pccMilli(0x4000) == 500);
  CHECK(pccMilli(0x3c000) == -500);   // -0.5 in 18-bit two's complement
  CHECK(pccMilli(0x6666) == 800);
  DsppRegs d{};
  d.pcc[0] = 1;
  for (unsigned o = 0; o < 3; ++o) d.pcc[kPccCoef[o][o] / 4] = 0x8000;
  PccView p = viewPcc(d);
  CHECK(p.enabled && p.identity && p.m[1][1] == 1000 && p.m[0][1] == 0);
  d.pcc[kPccCoef[2][2] / 4] = 0x4ccd;   // blue 0.6: a warm Night Light matrix
  p = viewPcc(d);
  CHECK(!p.identity && p.m[2][2] == 600);
  CHECK(kPccCoef[0][0] == 0x10 && kPccCoef[0][1] == 0x14 && kPccCoef[1][0] == 0x1c && kPccCoef[2][2] == 0x30);
  d.pcc[0x14 / 4] = 0x1000;   // output R from input G (msm's PCC_GREEN_R_OFF): an off-diagonal term of row R
  CHECK(viewPcc(d).m[0][1] == 125 && viewPcc(d).m[1][0] == 0);
}

static void testTiming()
{
  IntfRegs f{};
  const unsigned htotal = 16 + 16 + 427 + 117, vtotal = 2310;
  f.en = 1;
  f.countEn = 3;
  f.hsyncCtl = htotal << 16 | 16;
  f.vsyncPeriod = vtotal * htotal;
  f.vsyncWidth = 2 * htotal;
  f.dispVStart = 106 * htotal;
  f.dispVEnd = (vtotal - 68) * htotal - 1;
  f.dispHctl = (htotal - 117 - 1) << 16 | 32;
  f.config2 = 0x1011;
  const IntfTiming t = viewIntf(f);
  CHECK(t.enabled && t.frameCounter && t.lineCounter && t.wideBus && t.compressed && t.exact);
  CHECK(t.htotal == htotal && t.hsync == 16 && t.hbp == 16 && t.hactive == 427 && t.hfp == 117);
  CHECK(t.vtotal == 2310 && t.vsync == 2 && t.vbp == 104 && t.vactive == 2136 && t.vfp == 68);
  f.vsyncPeriod = 4620 * htotal;   // 60 Hz: the VFP doubled by a whole frame
  CHECK(viewIntf(f).vtotal == 4620 && viewIntf(f).vfp == 4620 - 2242);

  Soc soc;
  dsiTiming(soc, 0, 16, 16, 427, 117, 2, 104, 2136, 68);
  DsiRegs d{};
  d.ctrl = soc.regs[kDsi[0] + kDsiCtrl];
  d.activeH = soc.regs[kDsi[0] + kDsiActiveH];
  d.activeV = soc.regs[kDsi[0] + kDsiActiveV];
  d.total = soc.regs[kDsi[0] + kDsiTotal];
  d.activeHsync = soc.regs[kDsi[0] + kDsiActiveHsync];
  d.vsyncVpos = soc.regs[kDsi[0] + kDsiVsyncVpos];
  d.compress = soc.regs[kDsi[0] + kDsiVidCompress];
  DsiTiming dt = viewDsi(d);
  CHECK(dt.enabled && dt.video && !dt.command && dt.lanes == 4 && dt.clockLane && dt.compression);
  CHECK(dt.hfp == 117 && dt.hsync == 16 && dt.hbp == 16 && dt.hactive == 427 && dt.htotal == 576);
  CHECK(dt.vfp == 68 && dt.vsync == 2 && dt.vbp == 104 && dt.vactive == 2136 && dt.vtotal == 2310);
  CHECK(matchMode(dt, true) == 1 && kModes[1].hz == 120);
  CHECK(matchMode(dt, false) == -1);   // a single link would be 117
  dt.hfp = 21;
  CHECK(matchMode(dt, true) == 0);     // 144 Hz: HFP 42
  dt.hfp = 309;
  CHECK(matchMode(dt, true) == 2);     // 90 Hz: HFP 618
  dt.hfp = 117;
  dt.vtotal = 4620;
  dt.vfp = 2378;
  CHECK(matchMode(dt, true) == 3 && kModes[3].hz == 60);
  // The modes' totals: 3498 x 2310 at 120 Hz (the panel driver's 969 646 kHz pixel clock), 4620 lines at 60 Hz.
  CHECK(modeHtotal(kModes[1]) == 3498 && modeVtotal(kModes[1]) == 2310 && modeVtotal(kModes[3]) == 4620);
  CHECK(uint64_t(modeHtotal(kModes[1])) * modeVtotal(kModes[1]) * 120 / 1000 == 969645);
}

static void testRefresh()
{
  CHECK(refreshMilliHz(1000, 1120, 1000000000ull) == 120000);
  CHECK(refreshMilliHz(0xfffffff0u, 0x68, 1000000000ull) == 120000);   // wrapped
  CHECK(refreshMilliHz(5, 5, 1000000000ull) == 0);
  CHECK(refreshMilliHz(5, 65, 0) == 0);
  CHECK(refreshMilliHz(0, 2000000, 1000000000ull) == 0);               // implausible
  CHECK(refreshMilliHz(0, 60, 1000500000ull) == 59970);                // 60 frames in 1.0005 s
  CHECK(intfPixelKHz(120000, 2310 * 576) == 159667);                   // the stock 159.67 MHz compressed pixel clock
  CHECK(std::strcmp(panelModule(0), "p81 42-02-0a") == 0 && std::strcmp(panelModule(1), "p81 35-02-0b") == 0);
  CHECK(std::strcmp(rateOrder(0), "B3 then B2") == 0);
}

static void testPath()
{
  Soc soc;
  bootState(soc);
  Survey s;
  survey(s, [&](uint64_t pa) { return soc.load(pa); });
  const Path p = viewPath(s);
  CHECK(p.splashPipes == 0x3 && p.splashRects == 2);
  CHECK(p.ctl == 0 && p.ctlCount == 1 && p.dualDsi && p.view.video);
  CHECK(p.flippable && p.stagedOk && p.alpha && p.stride == kSplashStride);
  CHECK(flipFlushMask(p.splashPipes) == 0x3);
  // The counters, read twice.
  const uint64_t i1 = kMdp + kIntfBase + kIntfStride;
  soc.set(i1 + kIntfFrameCount, 77);
  soc.set(i1 + kIntfStride + kIntfFrameCount, 78);
  const Counters c = readCounters([&](uint64_t pa) { return soc.load(pa); });
  CHECK(c.frame[0] == 77 && c.frame[1] == 78);
  // A pipe outside the splash does not count; an unstaged splash pipe clears stagedOk.
  soc.set(kMdp + kPipes[1].base + kSrc0Addr, 0x10000000);
  Survey s2;
  survey(s2, [&](uint64_t pa) { return soc.load(pa); });
  CHECK(viewPath(s2).splashPipes == 0x1);
  soc.set(kMdp + kPipes[1].base + kSrc0Addr, uint32_t(kSplash));
  soc.set(kMdp + kLmBase + kLmStride + lmStage(0) + kLmFgSrcSel, 0xc0c0);
  Survey s3;
  survey(s3, [&](uint64_t pa) { return soc.load(pa); });
  CHECK(!viewPath(s3).stagedOk);
  // The panel ID pin is read even with the gate closed.
  Soc dark;
  dark.set(kTlmm + kPanelIdGpio * kTlmmStride + 4, 1);
  Survey s4;
  survey(s4, [&](uint64_t pa) { return dark.load(pa); });
  CHECK(s4.panelIdIo == 1 && viewPath(s4).ctl == -1);
}

// An apps SMMU with 127 groups, 83 banks, 128 pages; group 5 routes 0x800 mask 2 to bank 2.
struct Smmu {
  std::map<uint32_t, uint32_t> regs;
  std::vector<uint32_t> loads;
  uint32_t rd(uint32_t off)
  {
    loads.push_back(off);
    auto it = regs.find(off);
    return it == regs.end() ? 0 : it->second;
  }
  Smmu(uint32_t sctlr)
  {
    regs[0x20] = 0x7f;
    regs[0x24] = 0x60000053;
    regs[0x800 + 4 * 1] = 0x80000060;    // UFS
    regs[0xc00 + 4 * 1] = 0x00000052;
    regs[0x800 + 4 * 5] = 0x80020800;    // 0x800 mask 2
    regs[0xc00 + 4 * 5] = 0x00000002;    // translate, bank 2
    regs[0x1000 + 4 * 2] = 0x0001f000;   // stage 1, stage 2 bypass
    regs[0x1800 + 4 * 2] = 0x1;
    const uint32_t cb = (128 + 2) * 4096;
    regs[cb] = sctlr;
    regs[cb + 0x30] = 32;                // T0SZ 32, 4 KiB
    regs[cb + 0x20] = 0x9e000000;
    regs[cb + 0x24] = 0x00050000;        // ASID 5
  }
  unsigned bankLoads() const
  {
    unsigned n = 0;
    for (uint32_t off : loads) n += off >= (128 + 2) * 4096 && off < (128 + 3) * 4096;
    return n;
  }
};

static void testSmmu()
{
  {
    Smmu m(0xe0);   // translation off (sheng's display bank)
    const SmmuSurvey s = surveySmmu(2, [&](uint32_t off) { return m.rd(off); });
    CHECK(!s.error && s.groups == 127 && s.banks == 83 && s.pages == 128);
    CHECK(s.count == 1 && s.found[0].group == 5 && s.found[0].bank.read);
    CHECK(route(s) == Route::bankOff);
    CHECK(ttbr0Base(s.found[0].bank) == 0x9e000000ull && tcrT0sz(s.found[0].bank.tcr) == 32 && tcrGranuleKiB(32) == 4);
    CHECK(m.bankLoads() == 7);
  }
  {
    Smmu m(0x1e5);   // translation on
    CHECK(route(surveySmmu(2, [&](uint32_t off) { return m.rd(off); })) == Route::bankOn);
  }
  {
    Smmu m(0x1e5);   // level 1: no bank page
    const SmmuSurvey s = surveySmmu(1, [&](uint32_t off) { return m.rd(off); });
    CHECK(m.bankLoads() == 0 && route(s) == Route::bankUnread);
  }
  {
    Smmu m(0);
    m.regs[0xc00 + 4 * 5] = 0x00010000;   // bypass
    CHECK(route(surveySmmu(2, [&](uint32_t off) { return m.rd(off); })) == Route::bypass);
    m.regs[0x800 + 4 * 5] = 0;            // no entry
    CHECK(route(surveySmmu(2, [&](uint32_t off) { return m.rd(off); })) == Route::none);
  }
  {
    Smmu m(0);
    m.regs[0x800 + 4 * 5] = 0x80000802;   // exactly 0x802 (no mask): covers the DPU's second stream, not 0x800
    const SmmuSurvey s = surveySmmu(2, [&](uint32_t off) { return m.rd(off); });
    CHECK(s.count == 1 && route(s) == Route::none);
  }
  {
    Smmu m(0);
    m.regs[0x24] = 0xe0000053;            // 64 KiB pages: refused
    const SmmuSurvey s = surveySmmu(2, [&](uint32_t off) { return m.rd(off); });
    CHECK(s.error != nullptr && route(s) == Route::unread && s.loads == 3);
  }
  CHECK(smrCovers(0x80020800, 0x802) && smrCovers(0x80020800, 0x800) && !smrCovers(0x80020800, 0x801));
  CHECK(std::strcmp(cbarType(0x0001f000), "stage 1, stage 2 bypass") == 0);
}

static void testWalk()
{
  std::map<uint64_t, uint64_t> mem;
  std::vector<uint64_t> loads;
  auto load64 = [&](uint64_t pa) {
    loads.push_back(pa);
    auto it = mem.find(pa);
    return it == mem.end() ? 0 : it->second;
  };
  SmmuBank b{};
  b.sctlr = 1;
  b.tcr = 32;   // 32-bit IOVA: the walk starts at level 1
  b.ttbr0lo = 0x9e000000;
  mem[0x9e000000 + 8 * 3] = 0xc0000000ull | 0x401;                // 1 GiB identity block for 3-4 GiB
  mem[0x9e000000 + 8 * 0] = 0x9e001000ull | 3;                    // table for 0-1 GiB
  mem[0x9e001000 + 8 * 0x80] = 0x9e002000ull | 3;                 // 256 MiB: L3 table
  mem[0x9e002000 + 8 * 1] = 0x8c1234000ull | 0x403;               // one 4 KiB page above 4 GiB
  Walk w = walk(b, kSplash + 0x123, load64);
  CHECK(w.ok && w.pa == kSplash + 0x123 && w.level == 1 && w.loads == 1);
  w = walk(b, 0x10001abc, load64);
  CHECK(w.ok && w.level == 3 && w.loads == 3 && w.pa == 0x8c1234abcull);
  w = walk(b, 0x40000000, load64);
  CHECK(!w.ok && w.error && std::strcmp(w.error, "invalid descriptor") == 0);
  w = walk(b, 0x100000000ull, load64);
  CHECK(!w.ok && w.loads == 0);                                   // beyond 32 bits
  mem[0x9e002000 + 8 * 2] = 0x8c1235000ull | 0x401;               // 0b01 at level 3: reserved
  w = walk(b, 0x10002000, load64);
  CHECK(!w.ok && std::strcmp(w.error, "reserved descriptor") == 0);
  b.tcr = 32 | 1u << 14;                                          // 64 KiB granule: refused, nothing loaded
  loads.clear();
  CHECK(!walk(b, kSplash, load64).ok && loads.empty());
  b.tcr = 25;                                                     // 39-bit IOVA: starts at level 1 too
  mem.clear();
  mem[0x9e000000 + 8 * 3] = 0xc0000000ull | 0x401;
  CHECK(walk(b, kSplash, load64).ok);
  b.tcr = 16;                                                     // 48-bit: starts at level 0
  mem[0x9e000000] = 0x9e003000ull | 3;
  mem[0x9e003000 + 8 * 3] = 0xc0000000ull | 0x401;
  w = walk(b, kSplash, load64);
  CHECK(w.ok && w.loads == 2 && w.pa == kSplash);
}

int main()
{
  testGate();
  testFormat();
  testPipes();
  testCtlLm();
  testPcc();
  testTiming();
  testRefresh();
  testPath();
  testSmmu();
  testWalk();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu host tests: all passed\n");
  return 0;
}
