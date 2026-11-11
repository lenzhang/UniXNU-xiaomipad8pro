// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1b: zero-copy scanout (docs/drivers/dpu.md sections 4.2 and 8). The pipes the boot loader left scanning the boot
// framebuffer (the B1a survey's path) are pointed at another buffer by their source address and stride alone, with one CTL flush; the
// hardware latches the new values at the next frame start (video mode: no START) and CTL_FLUSH reads back 0 for the pipes' bits once
// it did. Below 4 GiB nothing of XNU's: a surface is reached through the DPU's SMMU bank (DpuSmmu.h).
//
// Portable (host tests in tests/dpu_flip_host.cpp): the hardware is an Hw object (the kernel's is in DpuFlip.cpp):
//   uint32_t rd(uint64_t pa); void wr(uint64_t pa, uint32_t v); void barrier()   MDSS/DPU registers (physical addresses)
//   uint32_t smmuRd(uint32_t off); void smmuWr(uint32_t off, uint32_t v)        the apps SMMU (offsets from kSmmu)
//   uint64_t nowUs(); void delayUs(uint32_t); void sleepMs(uint32_t)
//   void vsyncArm(); bool waitVsync(uint32_t us)   the vsync wake-up (true: the interrupt woke it; polled: false after ~1 ms)
//   void attachIrq()                                the MDSS SPI's handler (serviceIrq), once, after setup() found the line low
//   bool irqLive(); void irqMiss(); void irqDelivered(); uint32_t takeIrqBits()   the SPI's state, the status bits its handler cleared
//   uint64_t irqCount()                             the SPI's entries so far (for the summary)
//   void clean(const void *p, size_t n)            the CPU caches over [p, p + n) to the point of coherency (the tables)
//   bool allocTables(uint64_t **cpu, uint64_t *pa); bool allocGuard(uint64_t *pa)
//   uint64_t surfaceLength(const PianoDpuFlipRequest &)                         the memory's length
//   bool surfaceMap(const PianoDpuFlipRequest &, uint64_t *pages, unsigned max, unsigned *count, void **cookie, const char **why)
//                                                prepared (wired), listed as 4 KiB pages, the clean alias made
//   bool surfaceClean(void *cookie, uint64_t offset, uint64_t bytes)             false: no cacheable alias to clean with
//   void surfaceRelease(void *cookie)
//   bool testBuffer(uint64_t *pages, unsigned max, unsigned *count, void **cookie)   level 3's 3200x2136 pattern, drawn and cleaned
//   void note(const char *fmt, ...)                 one "PianoDpu: ..." line
// Every entry point is called with the caller's lock held (one flip at a time).
//
// Safety (any one of them puts the boot framebuffer back and turns flips off for the boot; the caller copies from then on): a latch
// that does not come within piano-dpu-latch ms, an SMMU fault (FSR) seen at a latch or by the watch, an underrun (TOP INTR 26/28)
// seen at a latch or by the watch. The revert points the pipes at the boot framebuffer and waits for that latch; only once it
// latched is the translation put back as found and the surfaces let go (with a pipe that may still fetch a window, the window and its
// memory stay: translation off would turn its IOVA into a physical address).
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef KERNEL
#include <libkern/libkern.h>   // snprintf
#else
#include <stdio.h>
#endif
#include "DpuSmmu.h"
#include "PianoDpuFlipClient.h"

namespace pianodpu {
namespace flip {

// ------------------------------------------------------------------------------------------------------------------ facts
// TOP0 interrupts (msm dpu_hw_interrupts: INTR_EN +0x10, INTR_STATUS +0x14, INTR_CLEAR +0x18; the status latches whether or not the
// bit is enabled, msm clears every set bit and masks by the enable afterwards): INTF1 vsync 27, underruns INTF1 26 / INTF2 28.
constexpr uint32_t kTopIntrClear = 0x018;
constexpr uint32_t kVsyncIntf1 = 1u << 27, kUnderrunBits = 1u << 26 | 1u << 28;
// MDSS HW_INTR_STATUS bit 0: the DPU (its hwirq 0); the MDSS line is GIC SPI 83, level high (DT display-subsystem@ae00000).
constexpr uint32_t kMdssIntrMdp = 1u << 0;
constexpr uint32_t kSpiMdss = 83;
constexpr uint32_t kFormatAlphaEnable = 1u << 8;   // SRC_FORMAT SRCC3_EN: per-pixel alpha
constexpr uint32_t kFourccBgra = 0x42475241u;      // 'BGRA'
constexpr uint32_t kShiftRows = 64;                // level 2: the boot framebuffer shown 64 rows lower
constexpr uint32_t kRowAlign = 64;                 // rows and offsets: multiples of 64 bytes (the policy: see checkSurface)

// The SPI handler: what the MDSS line carries now. Clears the DPU's enabled status bits (the line drops) and returns them; 0 is a
// stray (the caller's storm guard counts it).
template <class Rd, class Wr>
uint32_t serviceIrq(Rd rd, Wr wr)
{
  if (!(rd(kMdss + kMdssIntrStatus) & kMdssIntrMdp)) return 0;
  const uint32_t mine = rd(kMdp + kTopIntrStatus) & rd(kMdp + kTopIntrEn);
  if (mine) wr(kMdp + kTopIntrClear, mine);
  return mine;
}

// --------------------------------------------------------------------------------------------------------------- the path
// One pipe of the boot framebuffer's path, as found (what the revert writes back) and where each of its rects starts in it.
struct PipeState {
  unsigned pipe = 0;          // kPipes index
  bool multirect = false;     // rect 1 fetches too (SRC1, YSTRIDE0's upper half, SRC_FORMAT_REC1)
  uint32_t addr[2] = {0, 0}, offset[2] = {0, 0}, ystride0 = 0, format[2] = {0, 0};
};
struct FlipPath {
  bool ok = false;
  const char *why = nullptr;
  int ctl = -1;
  uint32_t flushMask = 0;
  unsigned count = 0;         // pipes
  PipeState pipe[4];
  unsigned rects = 0;
  bool alpha = false;         // a rect's format uses per-pixel alpha: cleared while a surface is shown
  bool counter = false;       // INTF1's frame counter runs
  uint64_t flushPa() const { return kMdp + kCtlBase + kCtlStride * unsigned(ctl) + kCtlFlush; }
};
inline FlipPath flipPath(const Survey &s, uint32_t flushOverride)
{
  FlipPath f;
  if (!s.clocks.readable) {
    f.why = "the display clocks' gate is closed: nothing was surveyed";
    return f;
  }
  const Path p = viewPath(s);
  f.ctl = p.ctl;
  f.alpha = p.alpha;
  f.counter = viewIntf(s.intfs[1]).frameCounter;
  if (p.ctl < 0) f.why = "no CTL drives INTF1/INTF2";
  else if (!p.view.video) f.why = "the CTL is in command mode (a flip there needs START)";
  else if (!p.splashRects) f.why = "no pipe fetches the boot framebuffer";
  else if (!p.flippable) f.why = "the boot framebuffer's pipes need more than an address change ('BGRA' linear, stride 12800)";
  else if (!p.stagedOk) f.why = "a pipe of the boot framebuffer is not staged in the CTL's mixers";
  if (f.why) return f;
  for (unsigned i = 0; i < kPipeCount; ++i) {
    if (!((p.splashPipes >> i) & 1u)) continue;
    const PipeRegs &r = s.pipes[i];
    const PipeView v = viewPipe(r);
    if (v.rect1 && !(v.fromSplash[0] && v.fromSplash[1])) {
      f.why = "a multirect pipe with one rect outside the boot framebuffer";
      return f;
    }
    if (f.count == 4) {
      f.why = "more than four pipes fetch the boot framebuffer";
      return f;
    }
    PipeState &ps = f.pipe[f.count++];
    ps.pipe = i;
    ps.multirect = v.rect1;
    ps.ystride0 = r.ystride[0];
    ps.format[0] = r.format;
    ps.format[1] = r.format1;
    for (unsigned k = 0; k < (v.rect1 ? 2u : 1u); ++k) {
      ps.addr[k] = v.addr[k];
      ps.offset[k] = v.addr[k] - uint32_t(kSplash);
      ++f.rects;
    }
  }
  f.flushMask = flushOverride ? flushOverride : flipFlushMask(p.splashPipes);
  f.ok = true;
  return f;
}

// Where the pipes fetch: the address of the boot framebuffer's pixel (0, 0) in the buffer shown, and its row bytes. A rect that starts
// at offset o in the boot framebuffer (row o / 12800, byte o % 12800) starts at the same row and byte of that buffer.
struct Target {
  uint32_t base = uint32_t(kSplash), stride = kSplashStride;
  bool surface = false;       // not the boot framebuffer (the alpha bit cleared when the path has one)
};
constexpr uint32_t rectAddress(uint32_t offset, const Target &t)
{
  return t.base + (offset / kSplashStride) * t.stride + offset % kSplashStride;
}
inline bool sameTarget(const Target &a, const Target &b) { return a.base == b.base && a.stride == b.stride && a.surface == b.surface; }

// The surface rules (the 'BGRA' requests NativeGopDisplay sends). Rows and offsets in multiples of 64 bytes is this driver's policy,
// not a measured DPU limit: the boot framebuffer's 12800 and msm's buffers on this DPU (32-pixel aligned pitch, 128 bytes) satisfy
// it, an odd surface is copied instead.
struct Need {
  uint32_t result = kPianoDpuFlipShown;
  uint64_t bytes = 0;         // from the offset: the last row's end
};
inline Need checkSurface(const PianoDpuFlipRequest &r)
{
  Need n;
  if (r.size < sizeof(PianoDpuFlipRequest) || r.version != kPianoDpuFlipVersion) n.result = kPianoDpuFlipVersionMismatch;
  else if (r.format != kFourccBgra) n.result = kPianoDpuFlipFormat;
  else if (r.planes > 1) n.result = kPianoDpuFlipPlanes;
  else if (r.width < kPanelWidth || r.height < kPanelHeight) n.result = kPianoDpuFlipSize;
  else if (r.rowBytes > 0xffff || r.rowBytes < uint64_t(kPanelWidth) * 4 || r.rowBytes % kRowAlign) n.result = kPianoDpuFlipStride;
  else if (r.offset % kRowAlign) n.result = kPianoDpuFlipAlign;
  if (n.result != kPianoDpuFlipShown) return n;
  n.bytes = (uint64_t(kPanelHeight) - 1) * r.rowBytes + uint64_t(kPanelWidth) * 4;
  if (r.offset + n.bytes + 4096 > smmu::kSlotBytes - smmu::kGuardPages * 4096ull) n.result = kPianoDpuFlipSize;
  return n;
}

// The boot loader's translation of the splash, walked every 2 MiB and at its last page (a bank with translation on is taken over
// only when it maps the boot framebuffer to itself: the switch through "off" then changes nothing on the panel).
template <class Load64>
bool splashIdentity(const SmmuBank &b, Load64 load64, unsigned *walks, uint64_t *badIova)
{
  *walks = 0;
  for (uint64_t at = kSplash;; at += 0x200000) {
    const uint64_t iova = at < kSplash + kSplashBytes ? at : kSplash + kSplashBytes - 0x1000;
    const Walk w = walk(b, iova, load64);
    ++*walks;
    if (!w.ok || w.pa != iova) {
      *badIova = iova;
      return false;
    }
    if (at >= kSplash + kSplashBytes) return true;
  }
}

// --------------------------------------------------------------------------------------------------------------- the engine
struct Config {
  uint32_t level = 1;         // piano-dpu-flip: 1 reflush, 2 + shifted flip, 3 + SMMU in place and a test pattern, 4 serve flips
  bool irq = true;            // piano-dpu-irq: the vsync interrupt (else CTL_FLUSH polled every ~1 ms)
  uint32_t clean = 1;         // piano-dpu-clean: 0 never, 1 through a cacheable alias when one can be made, 2 required
  uint32_t flush = 0;         // piano-dpu-flush: a CTL_FLUSH mask instead of the pipes' bits (0)
  bool underrun = true;       // piano-dpu-underrun: an underrun reverts
  uint32_t latchMs = 50;      // piano-dpu-latch: the latch timeout
  uint32_t watchS = 10;       // piano-dpu-watch: the fault/underrun check while a surface is shown (0 off)
  uint32_t reflushes = 3;     // level 1
  uint32_t holdMs = 2000;     // levels 2 and 3: how long the shifted picture / the pattern stays
  uint32_t minuteMs = 60000;  // level 3: the second fault check
  uint32_t summaryS = 60;     // the rate-limited summary line
  uint32_t detailFlips = 8;   // flips logged one by one
};

enum class State : uint8_t { off, ready, failed };

struct Latch {
  bool ok = false;
  uint32_t us = 0, irqs = 0, slices = 0, flush = 0;
};

struct Stats {
  uint64_t flips = 0, reshows = 0, toBoot = 0, maps = 0, evictions = 0, cleans = 0, uncleaned = 0;
  uint64_t latchUs = 0, latchMaxUs = 0, mapUs = 0, cleanUs = 0, workUs = 0, copyUs = 0;
  uint64_t refused[kPianoDpuFlipResults] = {};
  uint64_t windowStartUs = 0, windowFlips = 0, windowRefused = 0;
};

template <class Hw>
class Flipper {
 public:
  constexpr explicit Flipper(Hw &h) : hw(h) {}   // constant-initialized as a kernel global: no static constructor

  Hw &hw;
  Config cfg;
  FlipPath path;
  State state = State::off;
  bool smmuOn = false;
  smmu::BankPlan plan;
  smmu::BankRegs found;
  smmu::Tables tables;
  smmu::Slots slots;
  uint64_t guardPa = 0;
  Target current;             // what the pipes fetch (latched)
  uint32_t intrEn = 0;        // TOP INTR_EN as this driver keeps it
  uint32_t baseEn = 0;        // bits enabled whenever we run (the underruns, when their status does not latch by itself)
  bool rawLatch = true;       // TOP INTR_STATUS latches the vsync with its enable off
  uint32_t foundIntrEn = 0;
  uint32_t mdssBefore = 0;    // MDSS HW_INTR_STATUS after the DPU's enables went off: another block holds the line when not 0
  const char *failure = nullptr;
  Stats st;
  uint64_t *pages = nullptr;   // kSlotPages entries, the caller's (64 KiB: not in the kext's __DATA, which the KC has little room for)

  // --------------------------------------------------------------------------------------------- the interrupt enables
  void writeEn(uint32_t v)
  {
    intrEn = v;
    hw.wr(kMdp + kTopIntrEn, v);
    hw.barrier();
  }
  void vsyncOn()
  {
    hw.vsyncArm();   // first: an interrupt right after the enable already counts
    hw.wr(kMdp + kTopIntrClear, kVsyncIntf1);
    writeEn(intrEn | kVsyncIntf1);
  }
  void vsyncOff()
  {
    writeEn(intrEn & ~kVsyncIntf1);
    hw.wr(kMdp + kTopIntrClear, kVsyncIntf1);
    hw.barrier();
  }
  // Underrun bits since the last call (the raw status, and what the handler cleared), cleared.
  uint32_t underruns()
  {
    const uint32_t bits = (hw.rd(kMdp + kTopIntrStatus) | hw.takeIrqBits()) & kUnderrunBits;
    if (bits) hw.wr(kMdp + kTopIntrClear, bits);
    return bits;
  }
  uint32_t fsr() { return smmuOn ? hw.smmuRd(plan.cb + smmu::kFsr) : 0; }
  uint32_t frames() { return hw.rd(kMdp + kIntfBase + kIntfStride + kIntfFrameCount); }

  // ------------------------------------------------------------------------------------------------- program and latch
  void program(const Target &t)
  {
    for (unsigned i = 0; i < path.count; ++i) {
      const PipeState &ps = path.pipe[i];
      const uint64_t b = kMdp + kPipes[ps.pipe].base;
      hw.wr(b + kSrc0Addr, rectAddress(ps.offset[0], t));
      if (ps.multirect) hw.wr(b + kSrc1Addr, rectAddress(ps.offset[1], t));
      hw.wr(b + kYStride0, ps.multirect ? t.stride << 16 | t.stride : (ps.ystride0 & 0xffff0000u) | t.stride);
      const uint32_t clear = t.surface && path.alpha ? kFormatAlphaEnable : 0;
      hw.wr(b + kSrcFormat, ps.format[0] & ~clear);
      if (ps.multirect) hw.wr(b + kSrcFormatRec1, ps.format[1] & ~clear);
    }
    hw.barrier();
    hw.wr(path.flushPa(), path.flushMask);
    hw.barrier();
  }
  Latch waitLatch()
  {
    Latch l;
    const uint64_t t0 = hw.nowUs(), limit = uint64_t(cfg.latchMs) * 1000;
    for (;;) {
      l.flush = hw.rd(path.flushPa());
      const uint64_t dt = hw.nowUs() - t0;
      l.us = uint32_t(dt);
      if (!(l.flush & path.flushMask)) {
        l.ok = true;
        return l;
      }
      if (dt >= limit) return l;
      const bool live = hw.irqLive();
      const uint64_t before = hw.nowUs();
      ++l.slices;
      const bool woke = hw.waitVsync(uint32_t(limit - dt < 10000 ? limit - dt : 10000));
      if (woke) {
        ++l.irqs;
        hw.irqDelivered();
      } else if (live && hw.nowUs() - before >= 10000) {
        hw.irqMiss();   // a 10 ms slice with the vsync enabled and no interrupt: a frame is 8.3 ms
      }
    }
  }
  // One flush to t with the vsync interrupt on for it. current follows only a latch.
  Latch flushTo(const Target &t)
  {
    vsyncOn();
    program(t);
    const Latch l = waitLatch();
    vsyncOff();
    if (l.ok) current = t;
    return l;
  }

  // ------------------------------------------------------------------------------------------------------------ setup
  // The path from the survey, the DPU's interrupt enables off (this driver's from now on), their status cleared, then 20 ms:
  // whether the vsync status latches with its enable off (the latch wait and the underrun check rely on it), the underrun
  // baseline, the frame counter. True when the path can be flipped.
  bool setup(const Survey &s)
  {
    path = flipPath(s, cfg.flush);
    if (!path.ok) {
      hw.note("B1b: not flippable: %s", path.why);
      return false;
    }
    foundIntrEn = hw.rd(kMdp + kTopIntrEn);
    writeEn(0);
    hw.wr(kMdp + kTopIntrClear, 0xffffffffu);
    hw.barrier();
    mdssBefore = hw.rd(kMdss + kMdssIntrStatus);
    const uint32_t f0 = frames();
    hw.sleepMs(20);
    const uint32_t status = hw.rd(kMdp + kTopIntrStatus), f1 = frames();
    rawLatch = (status & kVsyncIntf1) != 0;
    hw.wr(kMdp + kTopIntrClear, status);
    baseEn = rawLatch ? 0 : kUnderrunBits;
    if (baseEn) writeEn(baseEn);
    char list[64];
    size_t n = 0;
    list[0] = 0;
    for (unsigned i = 0; i < path.count && n < sizeof list; ++i)
      n += size_t(snprintf(list + n, sizeof list - n, "%s%s%s", i ? "+" : "", kPipes[path.pipe[i].pipe].name,
                           path.pipe[i].multirect ? " (2 rects)" : ""));
    hw.note("B1b: path %s, ctl%d flush %08x%s, alpha %s; dpu intr en was %08x (now %08x), status in 20 ms %08x (vsync %s with its "
            "enable off, underrun %s), mdss status %08x%s; intf1 frames %u -> %u%s",
            list, path.ctl, path.flushMask, cfg.flush ? " (piano-dpu-flush)" : "", path.alpha ? "on: cleared while a surface is shown" : "off",
            foundIntrEn, intrEn, status, rawLatch ? "latches" : "does NOT latch (underruns enabled to latch)",
            (status & kUnderrunBits) ? "SET" : "clear", mdssBefore, mdssBefore ? " (another block holds the line: no interrupt)" : "",
            f0, f1, path.counter ? "" : " (counter off)");
    if (cfg.underrun && (status & kUnderrunBits)) {
      hw.note("B1b: an underrun before any flip: the check would mean nothing (piano-dpu-underrun=0 ignores underruns); stopped");
      return false;
    }
    return true;
  }
  bool interruptWanted() const { return cfg.irq && !mdssBefore; }

  // ------------------------------------------------------------------------------------------------- the ladder steps
  // Step 1: the boot framebuffer's own addresses written again and flushed: latched, the frame counter moving, no underrun.
  bool reflush(unsigned n, unsigned of)
  {
    const uint32_t f0 = frames();
    const Latch l = flushTo(Target{});
    const uint32_t f1 = frames(), u = underruns();
    const bool pass = l.ok && (!path.counter || f1 != f0) && !(cfg.underrun && u);
    hw.note("flip 1: reflush %u/%u: ctl%d flush %08x %s in %u us (%u vsync interrupts, %u waits), frames %u -> %u, underrun %s: %s", n,
            of, path.ctl, path.flushMask, l.ok ? "latched" : "did NOT latch", l.us, l.irqs, l.slices, f0, f1, u ? "SET" : "none",
            pass ? "pass" : "FAIL");
    if (!pass) fail(!l.ok ? "the reflush did not latch" : u ? "an underrun at the reflush" : "the frame counter stopped");
    return pass;
  }
  // Step 2: the boot framebuffer 64 rows lower for holdMs (a visible jolt, no SMMU), then back.
  bool shift()
  {
    const Target down{uint32_t(kSplash) + kShiftRows * kSplashStride, kSplashStride, false};
    const Latch a = flushTo(down);
    hw.note("flip 2: the boot framebuffer + %u rows (address %08x): %s in %u us; held %u ms", kShiftRows, rectAddress(path.pipe[0].offset[0], down),
            a.ok ? "latched" : "did NOT latch", a.us, cfg.holdMs);
    if (!a.ok) {
      revert("the shifted flip did not latch");
      return false;
    }
    hw.sleepMs(cfg.holdMs);
    const uint32_t u = underruns();
    const Latch b = flushTo(Target{});
    hw.note("flip 2: back to the boot framebuffer: %s in %u us; underrun %s: %s", b.ok ? "latched" : "did NOT latch", b.us, u ? "SET" : "none",
            b.ok && !(cfg.underrun && u) ? "pass" : "FAIL");
    if (!b.ok) {
      revert("the flip back did not latch");
      return false;
    }
    if (cfg.underrun && u) {
      fail("an underrun while shifted");
      return false;
    }
    return true;
  }

  // Step 3 (and 4): the bank in place. smmuLevel: piano-dpu-smmu; identityVerified: the boot loader's tables (when translation is on)
  // map the boot framebuffer to itself.
  bool smmuStart(const SmmuSurvey &m, bool identityVerified)
  {
    SmmuIo io{hw};
    const uint32_t idr0 = io.rd(smmu::kIdr0), idr2 = io.rd(smmu::kIdr2);
    plan = smmu::planBank(m, idr0, idr2, identityVerified);
    if (!plan.ok) {
      hw.note("smmu: not taken: %s (route %s)", plan.why, routeName(plan.route));
      return false;
    }
    tables.stage = plan.stage;
    if (!hw.allocTables(&tables.cpu, &tables.pa) || !hw.allocGuard(&guardPa)) {
      hw.note("smmu: no memory for the tables (%u pages, physically contiguous) or the guard page", smmu::kTablePages);
      return false;
    }
    smmu::buildTables(tables, [&](const void *p, size_t n) { hw.clean(p, n); });
    const Walk splash = smmu::lookup(tables, uint32_t(kSplash)), end = smmu::lookup(tables, uint32_t(kSplash + kSplashBytes - 4));
    const Walk window = smmu::lookup(tables, smmu::kWindowBase), low = smmu::lookup(tables, 0x1000);
    if (!splash.ok || splash.pa != kSplash || !end.ok || end.pa != kSplash + kSplashBytes - 4 || window.ok || low.ok) {
      hw.note("smmu: our own table does not translate as built: not taken");
      return false;
    }
    found = smmu::readBank(io, plan.cb);
    if ((found.sctlr & smmu::kSctlrM) != (route(m) == Route::bankOn ? smmu::kSctlrM : 0u)) {
      hw.note("smmu: bank %u's SCTLR changed since the survey (now %08x): not taken", plan.bank, found.sctlr);
      return false;
    }
    hw.note("smmu: bank %u (group %u, %s, cbar %08x cba2r %08x) as found: sctlr %08x tcr %08x tcr2 %08x ttbr0 %08x%08x mair0 %08x; idr0 %08x "
            "idr2 %08x (output %u bits); our table at %#llx (%u pages): identity 0x80000000-0xffffffff, windows %#x + %u x 32 MiB",
            plan.bank, plan.group, plan.stage == smmu::Stage::s1 ? "stage 1" : "stage 2", plan.cbar, plan.cba2r, found.sctlr, found.tcr,
            found.tcr2, found.ttbr0hi, found.ttbr0lo, found.mair0, idr0, idr2, smmu::kPaBits[plan.pasize], (unsigned long long)tables.pa,
            smmu::kTablePages, smmu::kWindowBase, smmu::kSlots);
    const smmu::EnableResult r = smmu::enableBank(plan, found, tables.pa, 50000, io);
    const smmu::BankRegs now = smmu::readBank(io, plan.cb);
    if (!r.ok) {
      hw.note("smmu: translation NOT on: %s; fsr %08x far %08x%08x fsynr0 %08x after %u us; bank %s (sctlr %08x ttbr0 %08x%08x)", r.why, r.fsr,
              r.farHi, r.farLo, r.fsynr0, r.watchedUs, r.restored ? "back as found" : "NOT as found", now.sctlr, now.ttbr0hi, now.ttbr0lo);
      return false;
    }
    smmuOn = true;
    hw.note("smmu: translation on in place: sctlr %08x tcr %08x tcr2 %08x ttbr0 %08x%08x mair0 %08x; fsr 0 for %u us of the display's "
            "own fetches", now.sctlr, now.tcr, now.tcr2, now.ttbr0hi, now.ttbr0lo, now.mair0, r.watchedUs);
    return true;
  }
  bool smmuStop(const char *why)
  {
    if (!smmuOn) return true;
    SmmuIo io{hw};
    const bool ok = smmu::restoreBank(plan, found, io);
    const smmu::BankRegs now = smmu::readBank(io, plan.cb);
    hw.note("smmu: translation back as found (%s): %s (sctlr %08x ttbr0 %08x%08x tcr %08x)", why, ok ? "done" : "readback DIFFERS",
            now.sctlr, now.ttbr0hi, now.ttbr0lo, now.tcr);
    if (ok) smmuOn = false;
    return ok;
  }

  // Map pages into slot s (TLB synced), checked through our own tables. False: the slot left empty.
  bool mapPages(unsigned s, unsigned count, const char **why)
  {
    if (!smmu::mapSlot(tables, s, pages, count, guardPa, [&](const void *p, size_t n) { hw.clean(p, n); }, why)) return false;
    SmmuIo io{hw};
    const Walk first = smmu::lookup(tables, smmu::slotIova(s)), last = smmu::lookup(tables, smmu::slotIova(s) + (count - 1) * 4096u + 4092u);
    const Walk guard = smmu::lookup(tables, smmu::slotIova(s) + count * 4096u);
    const bool synced = smmu::tlbSync(io, plan.cb);
    if (!first.ok || first.pa != pages[0] || !last.ok || last.pa != pages[count - 1] + 4092 || !guard.ok || guard.pa != guardPa || !synced) {
      *why = synced ? "the window does not translate as written" : "TLB sync timeout";
      smmu::unmapSlot(tables, s, [&](const void *p, size_t n) { hw.clean(p, n); });
      smmu::tlbSync(io, plan.cb);
      return false;
    }
    return true;
  }
  void unmap(unsigned s)
  {
    SmmuIo io{hw};
    smmu::unmapSlot(tables, s, [&](const void *p, size_t n) { hw.clean(p, n); });
    smmu::tlbSync(io, plan.cb);
  }

  // Step 3: the test pattern at IOVA 0x10000000 for holdMs and back, then the minute's fault check, then the bank as found.
  bool pattern()
  {
    unsigned count = 0;
    void *cookie = nullptr;
    if (!hw.testBuffer(pages, smmu::kSlotPages - smmu::kGuardPages, &count, &cookie)) {
      hw.note("flip 3: no test buffer");
      return false;
    }
    const char *why = nullptr;
    if (!mapPages(0, count, &why)) {
      hw.note("flip 3: the test buffer not mapped: %s", why);
      hw.surfaceRelease(cookie);
      return false;
    }
    slots.take(0, cookie, nullptr, cookie, count);
    slots.pending = 0;
    const Latch a = flushTo(Target{smmu::slotIova(0), kSplashStride, true});
    const uint32_t f = fsr(), u = underruns();
    hw.note("flip 3: the test pattern (8 colour bars, a white border, a checker at the top left) at iova %#x (%u pages): %s in %u us, "
            "fsr %08x, underrun %s; held %u ms", smmu::slotIova(0), count, a.ok ? "latched" : "did NOT latch", a.us, f, u ? "SET" : "none",
            cfg.holdMs);
    if (!a.ok || (f & smmu::kFsrFaults) || (cfg.underrun && u)) {
      revert(!a.ok ? "the test pattern did not latch" : (f & smmu::kFsrFaults) ? "an SMMU fault on the test pattern" : "an underrun on the test pattern");
      return false;
    }
    slots.shown = 0;
    slots.pending = -1;
    hw.sleepMs(cfg.holdMs);
    const uint32_t f2 = fsr(), u2 = underruns();
    const Latch b = flushTo(Target{});
    hw.note("flip 3: back to the boot framebuffer: %s in %u us; fsr %08x, underrun %s while shown", b.ok ? "latched" : "did NOT latch", b.us,
            f2, u2 ? "SET" : "none");
    if (!b.ok || (f2 & smmu::kFsrFaults) || (cfg.underrun && u2)) {
      revert(!b.ok ? "the flip back did not latch" : (f2 & smmu::kFsrFaults) ? "an SMMU fault on the test pattern" : "an underrun on the test pattern");
      return false;
    }
    slots.shown = -1;
    unmap(0);
    slots.clear(0);
    hw.surfaceRelease(cookie);
    hw.note("flip 3: translation stays on with the identity blocks for %u s, then the fault status again", cfg.minuteMs / 1000);
    hw.sleepMs(cfg.minuteMs);
    const uint32_t f3 = fsr(), u3 = underruns();
    const bool pass = !(f3 & smmu::kFsrFaults) && !(cfg.underrun && u3);
    hw.note("flip 3: after %u s: fsr %08x, underrun %s: %s", cfg.minuteMs / 1000, f3, u3 ? "SET" : "none", pass ? "pass" : "FAIL");
    if (!pass) {
      revert((f3 & smmu::kFsrFaults) ? "an SMMU fault with the identity table" : "an underrun with the identity table");
      return false;
    }
    smmuStop("step 3 done");
    return true;
  }

  // The steps of cfg.level, in order; a failure stops them. Level 4 ends ready to serve.
  bool ladder(const Survey &s, const SmmuSurvey &m, bool identityVerified)
  {
    st.windowStartUs = hw.nowUs();
    if (!pages) {
      hw.note("B1b: no page list (%u entries)", smmu::kSlotPages);
      return false;
    }
    if (!setup(s)) return false;
    if (interruptWanted()) hw.attachIrq();   // the DPU's enables are off and the MDSS line is low: attaching raises nothing
    const unsigned n = cfg.level == 1 ? (cfg.reflushes ? cfg.reflushes : 1) : 1;
    for (unsigned i = 1; i <= n; ++i) {
      if (!reflush(i, n)) return false;
      if (i < n) hw.sleepMs(1000);
    }
    if (cfg.level == 1) {
      hw.note("B1b step 1 passed: %u reflushes latched; the next image takes piano-dpu-flip=2", n);
      return true;
    }
    if (cfg.level == 2) {
      if (!shift()) return false;
      hw.note("B1b step 2 passed; the next image takes piano-dpu-flip=3");
      return true;
    }
    if (!smmuStart(m, identityVerified)) {
      fail("the SMMU bank was not taken");
      return false;
    }
    if (cfg.level == 3) {
      if (!pattern()) return false;
      hw.note("B1b step 3 passed; the next image takes piano-dpu-flip=4 with native-gop-display-flip=1");
      return true;
    }
    state = State::ready;
    hw.note("B1b: serving flips (NativeGopDisplay with native-gop-display-flip=1): %u windows, clean %u, latch timeout %u ms, watch %u s, "
            "vsync %s", smmu::kSlots, cfg.clean, cfg.latchMs, cfg.watchS, hw.irqLive() ? "interrupt" : "polled");
    return true;
  }

  // ------------------------------------------------------------------------------------------------------- the service
  void evict(unsigned s)
  {
    unmap(s);
    hw.surfaceRelease(slots.slot[s].cookie);
    slots.clear(s);
    ++st.evictions;
  }
  uint32_t refuse(PianoDpuFlipRequest &r, uint32_t why)
  {
    ++st.refused[why < kPianoDpuFlipResults ? why : 0];
    ++st.windowRefused;
    r.result = why;
    summary(false);
    return why;
  }
  uint32_t flipSurface(PianoDpuFlipRequest &r)
  {
    const uint64_t t0 = hw.nowUs();
    r.latchUs = r.workUs = 0;
    if (state != State::ready) return refuse(r, kPianoDpuFlipOff);
    const Need need = checkSurface(r);
    if (need.result != kPianoDpuFlipShown) return refuse(r, need.result);
    int s = slots.find(r.surface, r.memory);
    uint64_t mapUs = 0;
    bool mapped = false;
    if (s < 0) {
      s = slots.victim();
      if (s < 0) return refuse(r, kPianoDpuFlipSlots);
      if (slots.slot[s].key) evict(unsigned(s));
      const uint64_t length = hw.surfaceLength(r);
      if (r.offset + need.bytes > length || length > uint64_t(smmu::kSlotPages - smmu::kGuardPages) * 4096)
        return refuse(r, kPianoDpuFlipSize);
      unsigned count = 0;
      void *cookie = nullptr;
      const char *why = "?";
      if (!hw.surfaceMap(r, pages, smmu::kSlotPages - smmu::kGuardPages, &count, &cookie, &why)) {
        hw.note("flip: a %llux%llu surface not mapped: %s", (unsigned long long)r.width, (unsigned long long)r.height, why);
        return refuse(r, kPianoDpuFlipMemory);
      }
      if (uint64_t(count) * 4096 < r.offset + need.bytes) {
        hw.surfaceRelease(cookie);
        return refuse(r, kPianoDpuFlipSize);
      }
      if (!mapPages(unsigned(s), count, &why)) {
        hw.note("flip: a %llux%llu surface not mapped into window %d: %s", (unsigned long long)r.width, (unsigned long long)r.height, s, why);
        hw.surfaceRelease(cookie);
        return refuse(r, kPianoDpuFlipMemory);
      }
      slots.take(unsigned(s), r.surface, r.memory, cookie, count);
      mapUs = hw.nowUs() - t0;
      mapped = true;
      ++st.maps;
      st.mapUs += mapUs;
    }
    const uint64_t c0 = hw.nowUs();
    if (cfg.clean) {
      if (hw.surfaceClean(slots.slot[s].cookie, r.offset, need.bytes)) {
        ++st.cleans;
      } else {
        if (cfg.clean >= 2) return refuse(r, kPianoDpuFlipClean);
        if (!st.uncleaned++) hw.note("flip: no cacheable alias of a surface to clean the caches with: flipped without (piano-dpu-clean=2 copies instead)");
      }
    }
    const uint64_t cleanUs = hw.nowUs() - c0;
    st.cleanUs += cleanUs;
    if (s == slots.shown) {   // the surface on the panel again: the DPU fetches it every frame, the clean was all
      ++st.reshows;
      ++st.windowFlips;
      r.result = kPianoDpuFlipShown;
      r.workUs = uint32_t(hw.nowUs() - t0);
      st.workUs += r.workUs;
      st.copyUs += r.copyUs ? r.copyUs : 39000;
      return r.result;
    }
    slots.pending = s;
    const Target t{smmu::slotIova(unsigned(s)) + uint32_t(r.offset), uint32_t(r.rowBytes), true};
    const uint64_t p0 = hw.nowUs();
    const Latch l = flushTo(t);
    r.latchUs = l.us;
    if (!l.ok) {
      revert("a flip did not latch");
      return refuse(r, kPianoDpuFlipLatch);
    }
    const uint32_t f = fsr(), u = underruns();
    if (f & smmu::kFsrFaults) {
      hw.note("flip: SMMU fault: fsr %08x far %08x%08x fsynr0 %08x", f, hw.smmuRd(plan.cb + smmu::kFarHi), hw.smmuRd(plan.cb + smmu::kFarLo),
              hw.smmuRd(plan.cb + smmu::kFsynr0));
      revert("an SMMU fault at a flip");
      return refuse(r, kPianoDpuFlipFault);
    }
    if (cfg.underrun && u) {
      hw.note("flip: underrun %08x at a flip", u);
      revert("an underrun at a flip");
      return refuse(r, kPianoDpuFlipUnderrun);
    }
    slots.shown = s;
    slots.pending = -1;
    const uint64_t work = p0 - t0;   // map + clean: the CPU's part (the latch is a wait)
    r.result = kPianoDpuFlipShown;
    r.workUs = uint32_t(work);
    ++st.flips;
    ++st.windowFlips;
    st.latchUs += l.us;
    if (l.us > st.latchMaxUs) st.latchMaxUs = l.us;
    st.workUs += work;
    st.copyUs += r.copyUs ? r.copyUs : 39000;
    if (st.flips <= cfg.detailFlips)
      hw.note("flip %llu: %llux%llu row %llu -> window %d iova %#x%s, clean %llu us, ctl%d flush %08x latched in %u us (%u vsync "
              "interrupts)", (unsigned long long)st.flips, (unsigned long long)r.width, (unsigned long long)r.height,
              (unsigned long long)r.rowBytes, s, t.base, mapped ? " (mapped now)" : "", (unsigned long long)cleanUs, path.ctl,
              path.flushMask, l.us, l.irqs);
    summary(false);
    return r.result;
  }
  // Back to the boot framebuffer (the caller copied a frame there).
  uint32_t toBoot(PianoDpuFlipRequest &r)
  {
    r.result = kPianoDpuFlipShown;
    r.latchUs = r.workUs = 0;
    if (state != State::ready || slots.shown < 0) return r.result;
    const Latch l = flushTo(Target{});
    r.latchUs = l.us;
    if (!l.ok) {
      revert("the flip back to the boot framebuffer did not latch");
      r.result = kPianoDpuFlipLatch;
      return r.result;
    }
    slots.shown = -1;
    ++st.toBoot;
    return r.result;
  }
  uint32_t request(PianoDpuFlipRequest &r)
  {
    if (r.size < sizeof(PianoDpuFlipRequest) || r.version != kPianoDpuFlipVersion) return r.result = kPianoDpuFlipVersionMismatch;
    return r.surface ? flipSurface(r) : toBoot(r);
  }

  // The watch while a surface is shown (the caller's thread, every cfg.watchS): a fault or an underrun reverts. Also the summary.
  void watch()
  {
    if (state == State::ready && slots.shown >= 0) {
      const uint32_t f = fsr(), u = underruns();
      if (f & smmu::kFsrFaults) {
        hw.note("watch: SMMU fault: fsr %08x far %08x%08x fsynr0 %08x", f, hw.smmuRd(plan.cb + smmu::kFarHi), hw.smmuRd(plan.cb + smmu::kFarLo),
                hw.smmuRd(plan.cb + smmu::kFsynr0));
        revert("an SMMU fault while a surface was shown");
      } else if (cfg.underrun && u) {
        hw.note("watch: underrun %08x while a surface was shown", u);
        revert("an underrun while a surface was shown");
      }
    }
    summary(false);
  }
  bool watching() const { return state == State::ready && slots.shown >= 0 && cfg.watchS; }

  // At most one line per cfg.summaryS, and only when something happened in it (or force).
  void summary(bool force)
  {
    const uint64_t now = hw.nowUs();
    if (!force && (now - st.windowStartUs < uint64_t(cfg.summaryS) * 1000000 || (!st.windowFlips && !st.windowRefused))) return;
    const uint64_t saved = st.copyUs > st.workUs ? st.copyUs - st.workUs : 0;
    hw.note("flips: %llu in the last %llu s (%llu in all: latch mean %llu us max %llu us; %llu shown again; %llu maps mean %llu us; "
            "%llu cleans mean %llu us, %llu without; %llu back to the boot framebuffer); refused: format %llu planes %llu size %llu stride %llu "
            "align %llu memory %llu slots %llu clean %llu off %llu; copies avoided %llu, CPU time saved ~%llu ms (copies at %llu us mean "
            "minus %llu ms of flip work); vsync interrupts %llu (%s); state %s",
            (unsigned long long)st.windowFlips, (unsigned long long)((now - st.windowStartUs) / 1000000), (unsigned long long)st.flips,
            (unsigned long long)(st.flips ? st.latchUs / st.flips : 0), (unsigned long long)st.latchMaxUs, (unsigned long long)st.reshows,
            (unsigned long long)st.maps, (unsigned long long)(st.maps ? st.mapUs / st.maps : 0), (unsigned long long)st.cleans,
            (unsigned long long)(st.cleans ? st.cleanUs / st.cleans : 0), (unsigned long long)st.uncleaned, (unsigned long long)st.toBoot,
            (unsigned long long)st.refused[kPianoDpuFlipFormat], (unsigned long long)st.refused[kPianoDpuFlipPlanes],
            (unsigned long long)st.refused[kPianoDpuFlipSize], (unsigned long long)st.refused[kPianoDpuFlipStride],
            (unsigned long long)st.refused[kPianoDpuFlipAlign], (unsigned long long)st.refused[kPianoDpuFlipMemory],
            (unsigned long long)st.refused[kPianoDpuFlipSlots], (unsigned long long)st.refused[kPianoDpuFlipClean],
            (unsigned long long)st.refused[kPianoDpuFlipOff], (unsigned long long)(st.flips + st.reshows), (unsigned long long)(saved / 1000),
            (unsigned long long)(st.flips + st.reshows ? st.copyUs / (st.flips + st.reshows) : 0), (unsigned long long)(st.workUs / 1000),
            (unsigned long long)hw.irqCount(), hw.irqLive() ? "live" : "polled",
            state == State::ready ? "serving" : state == State::failed ? "off for the boot (copies)" : "not serving");
    st.windowStartUs = now;
    st.windowFlips = st.windowRefused = 0;
  }

  // ------------------------------------------------------------------------------------------------------- the safety
  // No flip from now on (nothing on the panel to undo).
  void fail(const char *why)
  {
    if (state != State::failed) hw.note("B1b off for the boot: %s", why);
    state = State::failed;
    failure = why;
  }
  // The boot framebuffer back, then (once latched) the translation as found and the surfaces let go; flips off for the boot.
  void revert(const char *why)
  {
    hw.note("revert: %s: the pipes back to the boot framebuffer", why);
    fail(why);
    const Latch l = flushTo(Target{});
    if (!l.ok) {
      hw.note("revert: the boot framebuffer did NOT latch (flush %08x after %u us): translation and windows left as they are", l.flush, l.us);
      return;
    }
    slots.shown = slots.pending = -1;
    const bool restored = smmuStop("revert");
    for (unsigned i = 0; i < smmu::kSlots; ++i) {
      if (!slots.slot[i].key) continue;
      if (!restored) unmap(i);
      hw.surfaceRelease(slots.slot[i].cookie);
      slots.clear(i);
    }
    writeEn(baseEn);
    hw.note("revert: done in %u us: the boot framebuffer on the panel, the translation %s, every window let go; copies from now on", l.us,
            restored ? "back as found" : "left on (identity)");
  }

 private:
  struct SmmuIo {
    Hw &hw;
    uint32_t rd(uint32_t off) { return hw.smmuRd(off); }
    void wr(uint32_t off, uint32_t v) { hw.smmuWr(off, v); }
    void delayUs(uint32_t us) { hw.delayUs(us); }
  };
  static const char *routeName(Route r)
  {
    return r == Route::bankOff ? "bank-off" : r == Route::bankOn ? "bank-on" : r == Route::bypass ? "bypass" : r == Route::none ? "none"
           : r == Route::unread ? "unread" : r == Route::fault ? "fault" : r == Route::bankUnread ? "bank unread" : "other";
  }
};

}  // namespace flip
}  // namespace pianodpu
