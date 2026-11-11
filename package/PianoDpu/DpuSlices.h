// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu's writing slices B1c (refresh by the vertical front porch, DpuRefresh.h), B1d (Night Light in the DSPP's PCC, DpuPcc.h)
// and B1e (panel sleep over the DSI command path, DpuPanel.h): what they share, portable (host tests in tests/dpu_*_host.cpp
// against tests/dpu_model.h). docs/drivers/dpu.md §4.3-4.5 is the design, §8 the test plan.
//
// Every hardware access goes through an Io (the kernel's in DpuSlices.cpp, a timed register model in the tests):
//   uint32_t rd(uint64_t pa)              a 32-bit load
//   bool wr(uint64_t pa, uint32_t v)      a 32-bit store, refused (false, nothing written) unless storeAllowed(pa, v)
//   void delayUs(uint32_t us)             a short spin
//   void sleepMs(uint32_t ms)             a blocking sleep
//   uint64_t nowUs()                      a monotonic clock
//   uintptr_t irqOff(); void irqRestore(uintptr_t)   a few back-to-back stores with nothing in between
//
// The store guard (storeAllowed) is the whole list of registers the three slices write: CTL flush words, INTF1/INTF2's timing
// engine enable, frame period and counter enable, the DSPPs' PCC, the DSI controllers' command-DMA words and their video TOTAL and
// timing shadow words, and the DPU's INTF underrun status (write-one-to-clear, those two bits only). Nothing else: never TLMM (so
// never the panel's reset gpio98 or the supply enables gpio117/118 that switch the KTZ8866's AVDD/AVEE), never the backlight's I2C,
// the display clocks, the DSI PHYs, VBIF, REGDMA, the SSPPs, the LMs, the DSC or the SMMU.
#pragma once
#include <stdint.h>
#include "DpuSurvey.h"

namespace pianodpu {
namespace slices {

// ------------------------------------------------------------------------------------------------- facts the slices add
// DPU TOP interrupts (msm dpu_hwio.h INTR_EN 0x010 / INTR_STATUS 0x014 / INTR_CLEAR 0x018; the SM8750 catalog's INTF1 underrun is
// TOP0 bit 26, INTF2's bit 28). The status latches whether or not the interrupt is enabled (msm masks it with the enables itself).
constexpr uint32_t kTopIntrClear = 0x018;
constexpr uint32_t kUnderrunBits = (1u << kIntrIntfUnderrun[1]) | (1u << kIntrIntfUnderrun[2]);
// CTL flush (msm dpu_hw_ctl.c): CTL_FLUSH bit 31 = the INTFs named in CTL_INTF_FLUSH (bit n = INTF n), bit 29 = the DSPPs named in
// CTL_DSPP_n_FLUSH (bit 4 = PCC). The hardware clears a bit when its registers latched, at the next frame start in video mode.
constexpr uint32_t kFlushIntf = 1u << 31, kFlushDspp = 1u << 29, kIntfFlushDsi = (1u << 1) | (1u << 2), kDsppFlushPcc = 1u << 4;
// INTF (msm dpu_hw_intf.c): INTF_STATUS bit 0 = the timing engine runs (get_status, core >= 5); INTF_CONFIG bit 31 = programmable
// fetch on (setup_prg_fetch); INTF_FRAME_LINE_COUNT_EN = 3 turns both counters on (setup_timing_engine writes 3).
constexpr uint32_t kIntfStatusOn = 1u, kIntfConfigProgFetch = 1u << 31, kCountersOn = 3u;
// The 120 Hz family's line time: 2310 lines at 120 Hz (dpu.md §2: refresh by HFP, 60 Hz = the same line with VFP + 2310), so a
// VFP change keeps it: 1e9 / (120 * 2310) ns.
constexpr uint32_t kLineNs = 3608;
constexpr unsigned kVtotal120 = 2310, kVtotal60 = 4620;
constexpr unsigned kVActiveEnd = kVsync + kVbp + kPanelHeight;   // 2242: the first VFP line, the same in both modes

// Absolute addresses.
constexpr uint64_t intfReg(unsigned i, uint32_t off) { return kMdp + kIntfBase + uint64_t(kIntfStride) * i + off; }
constexpr uint64_t ctlReg(unsigned c, uint32_t off) { return kMdp + kCtlBase + uint64_t(kCtlStride) * c + off; }
constexpr uint64_t pccReg(unsigned d, uint32_t off) { return kMdp + kDsppBase + uint64_t(kDsppStride) * d + kPcc + off; }
constexpr uint64_t dsiReg(unsigned l, uint32_t off) { return kDsi[l] + off; }

// The DSI 6G words the slices write, offsets from the controller's base (DpuHw.h counts like the hardware: msm's dsi.xml + 4).
// Command DMA (msm dsi.xml CMD_DMA_CTRL 0x38, DMA_LEN 0x48, TRIG_CTRL 0x80, TRIG_DMA 0x8c, INTR_CTRL 0x10c, TEST_PATTERN_GEN_CTRL
// 0x158; the command FIFO word and the second DMA control word from UniXNU-sm8750-common's PeripheralIO.h, which sends DCS through
// this FIFO on an SM8750 command-mode panel): see DpuPanel.h. Video timing: TOTAL (DpuHw.h) and the timing shadow words (DpuRefresh.h).
constexpr uint32_t kDsiCmdDmaCtrl = 0x03c, kDsiDmaLength = 0x04c, kDsiAckErr = 0x068, kDsiTrigCtrl = 0x084, kDsiTrigDma = 0x090,
                   kDsiTpgCtrl = 0x15c, kDsiTpgFifo = 0x17c, kDsiDmaCtrl1 = 0x390, kDsiShadow[3] = {0x1e4, 0x1e8, 0x1ec};

// ------------------------------------------------------------------------------------------------------------ the guard
inline bool storeAllowed(uint64_t pa, uint32_t v)
{
  if (pa & 3u) return false;
  if (pa == kMdp + kTopIntrClear) return v && !(v & ~kUnderrunBits);
  for (unsigned c = 0; c < kCtlCount; ++c)
    if (pa == ctlReg(c, kCtlFlush) || pa == ctlReg(c, kCtlIntfFlush) || (pa >= ctlReg(c, kCtlDspp0Flush) && pa < ctlReg(c, kCtlDspp0Flush + 16)))
      return true;
  for (unsigned i = 1; i <= 2; ++i) {
    if (pa == intfReg(i, kIntfEn)) return v <= 1;
    if (pa == intfReg(i, kIntfVsyncPeriod)) return true;
    if (pa == intfReg(i, kIntfCountEn)) return v == kCountersOn;
  }
  for (unsigned d = 0; d < kDsppCount; ++d)
    if (pa == pccReg(d, 0) || (pa >= pccReg(d, 0x10) && pa <= pccReg(d, 0x30))) return true;
  for (unsigned l = 0; l < 2; ++l) {
    const uint32_t at[] = {kDsiCtrl, kDsiTotal, kDsiCmdDmaCtrl, kDsiDmaLength, kDsiTrigDma, kDsiIntrCtrl, kDsiTpgCtrl, kDsiTpgFifo,
                           kDsiDmaCtrl1, kDsiShadow[0], kDsiShadow[1], kDsiShadow[2]};
    for (uint32_t off : at)
      if (pa == dsiReg(l, off)) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------------- the path, read only
// What the slices act on, as the hardware is now: the clock gate (DpuSurvey.h's, loads only), the video-mode CTL that drives
// INTF1+INTF2, its LMs and their DSPPs (LM n -> DSPP n for n < 4, the catalog's wiring), both INTFs' timing words and engine state,
// both DSI controllers' control and TOTAL words.
struct Link {
  uint32_t en = 0, status = 0, config = 0, hsyncCtl = 0, period = 0, countEn = 0, frame = 0, line = 0;
  uint32_t htotal = 0, vtotal = 0;           // INTF clocks per line, lines per frame (VSYNC_PERIOD_F0 / htotal)
  uint32_t dsiCtrl = 0, dsiTotal = 0, dsiTrig = 0;
  bool engine() const { return (en & 1u) && (status & kIntfStatusOn); }
  unsigned dsiVtotal() const { return (dsiTotal >> 16) + 1; }
};
struct LivePath {
  bool ok = false;
  const char *why = "not read";
  int ctl = -1;
  unsigned lms = 0, dspps = 0;
  uint32_t ctlTop = 0, intfActive = 0, intfMaster = 0;
  Link link[2];   // [0] INTF1 + DSI0 (the master), [1] INTF2 + DSI1
  ClockGate clocks;
};

template <class Io>
void readLink(Io &io, unsigned i, Link &k)
{
  k.en = io.rd(intfReg(i + 1, kIntfEn));
  k.status = io.rd(intfReg(i + 1, kIntfStatus));
  k.config = io.rd(intfReg(i + 1, kIntfConfig));
  k.hsyncCtl = io.rd(intfReg(i + 1, kIntfHsyncCtl));
  k.period = io.rd(intfReg(i + 1, kIntfVsyncPeriod));
  k.countEn = io.rd(intfReg(i + 1, kIntfCountEn));
  k.frame = io.rd(intfReg(i + 1, kIntfFrameCount));
  k.line = io.rd(intfReg(i + 1, kIntfLineCount));
  k.htotal = k.hsyncCtl >> 16;
  k.vtotal = k.htotal && k.period % k.htotal == 0 ? k.period / k.htotal : 0;
  k.dsiCtrl = io.rd(dsiReg(i, kDsiCtrl));
  k.dsiTotal = io.rd(dsiReg(i, kDsiTotal));
  k.dsiTrig = io.rd(dsiReg(i, kDsiTrigCtrl));
}

template <class Io>
LivePath readPath(Io &io)
{
  LivePath p;
  unsigned loads = 0;
  p.clocks = surveyClocks([&](uint64_t pa) { return io.rd(pa); }, &loads);
  if (!p.clocks.readable) {
    p.why = p.clocks.why;
    return p;
  }
  for (unsigned l = 0; l < 2; ++l)
    if (!branchOn(p.clocks.byte[l]) || !branchOn(p.clocks.pclk[l])) {
      p.why = l ? "DSI1's byte or pixel clock is off: nothing of it is read" : "DSI0's byte or pixel clock is off: nothing of it is read";
      return p;
    }
  for (unsigned c = 0; c < kCtlCount && p.ctl < 0; ++c) {
    const uint32_t active = io.rd(ctlReg(c, kCtlIntfActive)), top = io.rd(ctlReg(c, kCtlTop));
    if ((active & 0x6u) != 0x6u || ((top >> 17) & 1u)) continue;
    p.ctl = int(c);
    p.ctlTop = top;
    p.intfActive = active;
    p.intfMaster = io.rd(ctlReg(c, kCtlIntfMaster));
    p.lms = io.rd(ctlReg(c, kCtlLayerActive)) & 0xffu;
  }
  if (p.ctl < 0) {
    p.why = "no video-mode CTL drives INTF1+INTF2 (the dual-DSI panel)";
    return p;
  }
  p.dspps = p.lms & 0xfu;
  for (unsigned i = 0; i < 2; ++i) readLink(io, i, p.link[i]);
  const Link &a = p.link[0], &b = p.link[1];
  if (!a.engine() || !b.engine()) p.why = "an INTF timing engine is off (the panel is not scanned)";
  else if (!a.htotal || a.htotal != b.htotal || !a.vtotal || a.vtotal != b.vtotal) p.why = "INTF1 and INTF2 timings differ or are not whole lines";
  else if ((a.dsiCtrl & 3u) != 3u || (b.dsiCtrl & 3u) != 3u) p.why = "a DSI controller is not enabled in video mode";
  else {
    p.ok = true;
    p.why = "ok";
  }
  return p;
}

// ------------------------------------------------------------------------------------------------- frames, lines, latches
// Both frame counters, read back to back with nothing in between.
struct Frames {
  uint32_t f[2] = {0, 0};
  uint64_t us = 0;
};
template <class Io>
Frames readFrames(Io &io)
{
  Frames r;
  const uintptr_t s = io.irqOff();
  r.f[0] = io.rd(intfReg(1, kIntfFrameCount));
  r.f[1] = io.rd(intfReg(2, kIntfFrameCount));
  r.us = io.nowUs();
  io.irqRestore(s);
  return r;
}

// The next frame start of INTF1: its frame counter seen changing (polled every 50 us), at most timeoutUs. The time and both counts.
template <class Io>
bool waitFrameEdge(Io &io, uint32_t timeoutUs, Frames *edge)
{
  const uint32_t f0 = io.rd(intfReg(1, kIntfFrameCount));
  const uint64_t end = io.nowUs() + timeoutUs;
  while (io.nowUs() < end) {
    io.delayUs(50);
    const Frames r = readFrames(io);
    if (r.f[0] != f0) {
      *edge = r;
      return true;
    }
  }
  return false;
}

// The refresh, frame-edge to frame-edge over about ms milliseconds (both INTFs counted over the same interval): thousandths of a
// hertz, 0 when a counter did not move. lockstep: INTF2 counted the same number of frames as INTF1 (one either way for the read).
struct Rate {
  uint64_t milliHz = 0;
  uint32_t frames[2] = {0, 0};
  uint64_t us = 0;
  bool lockstep = false;
};
template <class Io>
Rate measureRate(Io &io, uint32_t ms)
{
  Rate r;
  Frames a, b;
  if (!waitFrameEdge(io, 40000, &a)) return r;
  if (ms > 4) io.sleepMs(ms - 4);
  if (!waitFrameEdge(io, 40000, &b)) return r;
  r.frames[0] = b.f[0] - a.f[0];
  r.frames[1] = b.f[1] - a.f[1];
  r.us = b.us - a.us;
  r.milliHz = r.us ? (uint64_t(r.frames[0]) * 1000000000ull + r.us / 2) / r.us : 0;
  const int32_t d = int32_t(r.frames[0] - r.frames[1]);
  r.lockstep = r.frames[0] && d >= -1 && d <= 1;
  return r;
}

// INTF1's line counter inside [lo, hi] (a frame start is then at least vtotal - hi lines away and lo lines behind). Sleeps while
// far, spins when near. False when the window did not come within timeoutUs (a counter that does not move).
template <class Io>
bool waitLines(Io &io, unsigned lo, unsigned hi, unsigned vtotal, uint32_t timeoutUs)
{
  const uint64_t end = io.nowUs() + timeoutUs;
  for (;;) {
    const uint32_t line = io.rd(intfReg(1, kIntfLineCount));
    if (line >= lo && line <= hi) return true;
    if (io.nowUs() >= end) return false;
    const uint64_t lines = line < lo ? lo - line : (line < vtotal ? vtotal - line : 0) + lo;
    const uint64_t us = lines * kLineNs / 1000;
    if (us > 2500) io.sleepMs(uint32_t((us - 1500) / 1000));
    else io.delayUs(us > 100 ? 100 : uint32_t(us) + 1);
  }
}

// CTL_FLUSH's mask bits cleared (latched) within timeoutUs: the microseconds it took, or -1.
template <class Io>
int32_t waitFlush(Io &io, unsigned ctl, uint32_t mask, uint32_t timeoutUs)
{
  const uint64_t t0 = io.nowUs();
  for (;;) {
    const uint64_t now = io.nowUs();
    if (!(io.rd(ctlReg(ctl, kCtlFlush)) & mask)) return int32_t(now - t0);
    if (now - t0 >= timeoutUs) return -1;
    io.sleepMs(1);
  }
}

// The counters on (INTF_FRAME_LINE_COUNT_EN = 3, what msm writes with every timing) when the boot stages left them off: the slices
// measure and time with them. True when both are on (or now turned on).
template <class Io>
bool countersOn(Io &io, LivePath &p)
{
  bool ok = true;
  for (unsigned i = 0; i < 2; ++i) {
    if ((p.link[i].countEn & kCountersOn) == kCountersOn) continue;
    io.wr(intfReg(i + 1, kIntfCountEn), kCountersOn);
    p.link[i].countEn = io.rd(intfReg(i + 1, kIntfCountEn));
    ok &= (p.link[i].countEn & kCountersOn) == kCountersOn;
  }
  return ok;
}

}  // namespace slices
}  // namespace pianodpu
