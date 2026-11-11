// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoDpu's B1b (DpuSmmu.h, DpuFlip.h) against models:
//   - RAM: the table buffer as the CPU writes it and as DRAM holds it; only cleaned 64-byte lines reach DRAM, and the SMMU walks DRAM;
//   - the SMMU: global pages, the DPU's bank 2 (SCTLR/TTBR0/TCR/TCR2/MAIR/FSR/RESUME, TLBIALL/TLBSYNC/TLBSTATUS), a TLB that only
//     TLBIALL empties, the walk of the bank's registers (SEP other than 7 faults every IOVA with bit 31 set, as on SM8550), faults
//     into FSR/FAR/FSYNR0; and a record of every write (the stream's group, S2CR, CBAR, CBA2R must never be written);
//   - the DPU: pipes with shadow and latched registers, CTL_FLUSH pending until the next frame start, TOP INTR EN/STATUS/CLEAR (the
//     status latches with its enable off), MDSS HW_INTR_STATUS, the INTF1 frame counter, and each frame's fetch of every rect's first
//     and last pixel through the SMMU (a fault is an underrun);
//   - time: a simulated clock with a frame every 8333 us; the vsync interrupt (when live) runs the SPI handler at a frame start.
// The tests: the table layout and its descriptors (stage 1 and 2), the window mapping and its guard, the bank plan for every route
// and bank kind, the in-place enable and its undo (readback refusals, a stuck TLB sync, a fault during the watch), the take-over of
// a bank the boot loader translates, the four ladder steps, the flip service (windows, LRU, TLB invalidation, refusals, the clean
// policy, the alpha bit, multirect), the safety reverts (missed latch, fault, underrun, the watch), the interrupt (service, misses),
// the rate-limited summary.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. dpu_flip_host.cpp -o /tmp/dpu_flip_host && /tmp/dpu_flip_host
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "DpuFlip.h"

using namespace pianodpu;
using namespace pianodpu::flip;
namespace sm = pianodpu::smmu;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// --------------------------------------------------------------------------------------------------------------- RAM
struct Ram {
  std::vector<uint64_t> cpu, dram;   // the table buffer
  uint64_t pa = 0x8c1000000ull;
  unsigned cleans = 0;
  void alloc(size_t words)
  {
    cpu.assign(words, 0x5a5a5a5a5a5a5a5aull);   // garbage until written
    dram.assign(words, 0xa5a5a5a5a5a5a5a5ull);
  }
  bool contains(uint64_t a) const { return a >= pa && a < pa + dram.size() * 8; }
  void clean(const void *p, size_t n)
  {
    ++cleans;
    const size_t off = size_t(static_cast<const uint8_t *>(p) - reinterpret_cast<const uint8_t *>(cpu.data()));
    const size_t first = off / 64 * 8, last = (off + n + 63) / 64 * 8;
    for (size_t i = first; i < last && i < cpu.size(); ++i) dram[i] = cpu[i];
  }
  uint64_t load64(uint64_t a) const { return contains(a) ? dram[(a - pa) / 8] : 0; }
  bool coherent() const { return cpu == dram; }
};

// The boot loader's own tables (for a bank with translation on): another memory, walked through Load64.
struct BootTables {
  std::map<uint64_t, uint64_t> mem;
  uint64_t load64(uint64_t a) const
  {
    auto it = mem.find(a);
    return it == mem.end() ? 0 : it->second;
  }
};

// -------------------------------------------------------------------------------------------------------------- the SMMU
struct SmmuModel {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  std::map<uint64_t, uint64_t> tlb;   // 4 KiB IOVA page -> PA page
  Ram *ram = nullptr;
  BootTables *boot = nullptr;
  unsigned pages = 128, bank = 2;
  unsigned syncBusy = 2, syncLeft = 0;
  bool syncStuck = false, ignoreTtbr = false, ignoreSctlrM = false, walkFromLevel0 = false;
  unsigned faultAfter = 0;            // > 0: the n-th translation from now on faults
  unsigned translations = 0;
  uint32_t cb() const { return (pages + bank) * 4096; }
  explicit SmmuModel(uint32_t sctlr = 0xe0)
  {
    regs[0x20] = 0x6000007f;               // S1TS, S2TS, 127 groups
    regs[0x24] = 0x60000053;               // 128 pages, 83 banks
    regs[0x28] = 0x00001055;               // PTFS 4 KiB, OAS 48, IAS 48
    regs[0x800 + 4 * 1] = 0x80000060;      // UFS
    regs[0xc00 + 4 * 1] = 0x00000052;
    regs[0x800 + 4 * 5] = 0x80020800;      // 0x800 mask 2
    regs[0xc00 + 4 * 5] = 0x00000002;      // translate, bank 2
    regs[0x1000 + 4 * 2] = 0x0001f000;     // stage 1, stage 2 bypass
    regs[0x1800 + 4 * 2] = 0x1;            // VA64
    regs[cb()] = sctlr;
    regs[cb() + sm::kTcr] = 0x80000000;    // whatever the boot loader left
    regs[cb() + sm::kTcr2] = 0x00000000;
    regs[cb() + sm::kTtbr0Lo] = 0x12345000;
    regs[cb() + sm::kTtbr0Hi] = 0x00030000;   // ASID 3
    regs[cb() + sm::kMair0] = 0x0000ff44;
  }
  uint32_t rd(uint32_t off)
  {
    if (off == cb() + sm::kTlbStatus) {
      if (syncStuck) return 1;
      if (syncLeft) {
        --syncLeft;
        return 1;
      }
      return 0;
    }
    auto it = regs.find(off);
    return it == regs.end() ? 0 : it->second;
  }
  void wr(uint32_t off, uint32_t v)
  {
    writes.push_back({off, v});
    if (off == cb() + sm::kFsr) {
      regs[off] &= ~v;
      return;
    }
    if (off == cb() + sm::kTlbiAll) {
      tlb.clear();
      return;
    }
    if (off == cb() + sm::kTlbSync) {
      syncLeft = syncBusy;
      return;
    }
    if (off == cb() + sm::kResume) {
      regs[cb() + sm::kFsr] &= ~sm::kFsrSs;
      return;
    }
    if (ignoreTtbr && (off == cb() + sm::kTtbr0Lo || off == cb() + sm::kTtbr0Hi)) return;
    if (ignoreSctlrM && off == cb() + sm::kSctlr) v &= ~1u;
    regs[off] = v;
  }
  bool onlyBankWrites() const
  {
    for (auto &w : writes)
      if (w.first < cb() || w.first >= cb() + 4096) return false;
    return true;
  }
  void fault(uint32_t iova, unsigned level, uint32_t bit)
  {
    uint32_t &fsr = regs[cb() + sm::kFsr];
    if (fsr & sm::kFsrFaults) {   // the first fault's syndrome stays; MULTI says there were more
      fsr |= 1u << 31;
      return;
    }
    fsr |= bit;
    regs[cb() + sm::kFarLo] = iova;
    regs[cb() + sm::kFarHi] = 0;
    regs[cb() + sm::kFsynr0] = level;
  }
  // A DPU read of one IOVA. False: a fault (recorded).
  bool translate(uint32_t iova, uint64_t *pa)
  {
    ++translations;
    if (!(rd(cb()) & 1u)) {
      *pa = iova;
      return true;
    }
    if (faultAfter && !--faultAfter) {
      fault(iova, 3, 1u << 1);
      return false;
    }
    auto hit = tlb.find(iova >> 12);
    if (hit != tlb.end()) {
      *pa = hit->second << 12 | (iova & 0xfffu);
      return true;
    }
    const uint32_t tcr2 = rd(cb() + sm::kTcr2), cbar = rd(0x1000 + 4 * bank);
    if (((cbar >> 16) & 3u) == 1 && ((tcr2 >> 15) & 7u) != 7 && (iova & 0x80000000u)) {   // SEP below bit 31: out of TTBR0's range
      fault(iova, 0, 1u << 1);
      return false;
    }
    SmmuBank b{};
    b.tcr = rd(cb() + sm::kTcr);
    if (walkFromLevel0) b.tcr = (b.tcr & ~0x3fu) | 16;   // as if the walk started at level 0
    b.ttbr0lo = rd(cb() + sm::kTtbr0Lo);
    b.ttbr0hi = rd(cb() + sm::kTtbr0Hi);
    const Walk w = walk(b, iova, [&](uint64_t a) { return ram && ram->contains(a) ? ram->load64(a) : boot ? boot->load64(a) : 0; });
    if (!w.ok) {
      fault(iova, w.level, 1u << 1);
      return false;
    }
    if (!(w.desc[w.loads - 1] & sm::kAf)) {
      fault(iova, w.level, 1u << 2);
      return false;
    }
    tlb[iova >> 12] = w.pa >> 12;
    *pa = w.pa;
    return true;
  }
};

// --------------------------------------------------------------------------------------------------------------- the DPU
struct DpuModel {
  std::map<uint64_t, uint32_t> regs, active;
  std::vector<std::pair<uint64_t, uint32_t>> writes;
  uint32_t pendingFlush = 0, status = 0, en = 0, frameCount = 1000, mdssOther = 0;
  bool latchNever = false, rawLatch = true, underrunNext = false;
  unsigned flushWrites = 0;
  SmmuModel *smmu = nullptr;
  std::vector<uint64_t> lastFetch;          // the PAs of the last frame's fetches (first and last pixel of every rect)
  std::vector<uint32_t> fetchIova;
  static constexpr uint64_t kFlush = kMdp + kCtlBase + kCtlFlush;
  uint32_t rd(uint64_t pa)
  {
    if (pa == kFlush) return pendingFlush;
    if (pa == kMdp + kTopIntrStatus) return status;
    if (pa == kMdp + kTopIntrEn) return en;
    if (pa == kMdss + kMdssIntrStatus) return ((status & en) ? kMdssIntrMdp : 0) | mdssOther;
    if (pa == kMdp + kIntfBase + kIntfStride + kIntfFrameCount) return frameCount;
    auto it = regs.find(pa);
    return it == regs.end() ? 0 : it->second;
  }
  void wr(uint64_t pa, uint32_t v)
  {
    writes.push_back({pa, v});
    if (pa == kFlush) {
      pendingFlush |= v;
      ++flushWrites;
      return;
    }
    if (pa == kMdp + kTopIntrClear) {
      status &= ~v;
      return;
    }
    if (pa == kMdp + kTopIntrEn) {
      en = v;
      return;
    }
    regs[pa] = v;
  }
  void latchPipe(unsigned i)
  {
    const uint64_t b = kMdp + kPipes[i].base;
    for (uint32_t off : {kSrc0Addr, kSrc1Addr, kYStride0, kSrcFormat, kSrcFormatRec1, kSrcXy, kSrcXyRec1, kSrcSize, kSrcSizeRec1, kMultirect})
      active[b + off] = rd(b + off);
  }
  uint32_t act(unsigned pipe, uint32_t off) { return active[kMdp + kPipes[pipe].base + off]; }
  void fetch()
  {
    lastFetch.clear();
    fetchIova.clear();
    for (unsigned i = 0; i < kPipeCount; ++i) {
      if (!act(i, kSrcSize) || !act(i, kSrc0Addr)) continue;
      const bool two = act(i, kMultirect) & 1u;
      for (unsigned r = 0; r < (two ? 2u : 1u); ++r) {
        const uint32_t addr = act(i, r ? kSrc1Addr : kSrc0Addr), xy = act(i, r ? kSrcXyRec1 : kSrcXy),
                       size = act(i, r ? kSrcSizeRec1 : kSrcSize);
        const uint32_t stride = r ? act(i, kYStride0) >> 16 : act(i, kYStride0) & 0xffffu;
        const uint32_t x = xy & 0xffffu, y = xy >> 16, w = size & 0xffffu, h = size >> 16;
        const uint32_t first = addr + y * stride + x * 4, last = addr + (y + h - 1) * stride + (x + w) * 4 - 1;
        for (uint32_t iova : {first, last}) {
          uint64_t pa = 0;
          fetchIova.push_back(iova);
          if (!smmu || smmu->translate(iova, &pa)) lastFetch.push_back(pa);
          else status |= 1u << 26;   // a fetch error: an underrun
        }
      }
    }
  }
  void frame()
  {
    ++frameCount;
    if (pendingFlush && !latchNever) {
      for (unsigned i = 0; i < kPipeCount; ++i)
        if ((pendingFlush >> kPipes[i].flushBit) & 1u) latchPipe(i);
      pendingFlush = 0;
    }
    if (rawLatch || (en & (1u << 27))) status |= 1u << 27;
    if (rawLatch || (en & (1u << 29))) status |= 1u << 29;
    fetch();
    if (underrunNext) {
      status |= 1u << 26;
      underrunNext = false;
    }
  }
  std::set<uint64_t> written() const
  {
    std::set<uint64_t> s;
    for (auto &w : writes) s.insert(w.first);
    return s;
  }
};

// The modelled boot-loader state (dpu_host.cpp's): VIG0 left half, VIG1 right half by SRC_XY, 'BGRA' with alpha, CTL0 video on
// INTF1+INTF2, LM0/LM1, DSC, 120 Hz; the clocks on. variant 1: VIG1 by its address (+6400) instead of SRC_XY; 2: one DMA0 pipe,
// multirect, rect 1 at +6400.
static void bootState(DpuModel &d, int variant = 0)
{
  auto set = [&](uint64_t pa, uint32_t v) { d.regs[pa] = v; };
  set(kGcc + kGccDispAhb, 1);
  set(kDispCc + kCcMdssGdsc, 0x80000000);
  set(kDispCc + kCcAhb, 1);
  set(kDispCc + kCcMdp, 1);
  const uint64_t c = kMdp + kCtlBase;
  set(c + kCtlTop, 0xf0000000);
  set(c + kCtlIntfActive, 0x6);
  set(c + kCtlIntfMaster, 0x2);
  set(c + kCtlLayerActive, 0x3);
  set(c + kCtlDscActive, 0x3);
  for (unsigned l = 0; l < kLmCount; ++l)
    for (unsigned st = 0; st < kLmStages; ++st) set(kMdp + kLmBase + kLmStride * l + lmStage(st) + kLmFgSrcSel, 0xc0c0);
  if (variant == 2) {
    const uint64_t b = kMdp + kPipes[4].base;   // DMA0
    set(b + kSrcSize, 2136u << 16 | 1600);
    set(b + kSrcSizeRec1, 2136u << 16 | 1600);
    set(b + kOutSize, 2136u << 16 | 1600);
    set(b + kSrc0Addr, uint32_t(kSplash));
    set(b + kSrc1Addr, uint32_t(kSplash) + 6400);
    set(b + kYStride0, kSplashStride << 16 | kSplashStride);
    set(b + kSrcFormat, 0x000236ff);
    set(b + kSrcFormatRec1, 0x000236ff);
    set(b + kUnpack, 0x03020001);
    set(b + kUnpackRec1, 0x03020001);
    set(b + kMultirect, 1);
    set(c + kCtlPipeActive, 1u << 0);
    set(c + kCtlFetchActive, 1u << 0);
    set(kMdp + kLmBase + lmStage(0) + kLmFgSrcSel, 0xc000);                 // DMA0 rect 0
    set(kMdp + kLmBase + kLmStride + lmStage(0) + kLmFgSrcSel, 0xc010);     // DMA0 rect 1
  } else {
    for (unsigned v = 0; v < 2; ++v) {
      const uint64_t b = kMdp + kPipes[v].base;
      set(b + kSrcSize, 2136u << 16 | 1600);
      set(b + kOutSize, 2136u << 16 | 1600);
      set(b + kSrcXy, v && variant == 0 ? 1600 : 0);
      set(b + kSrc0Addr, uint32_t(kSplash) + (v && variant == 1 ? 6400 : 0));
      set(b + kYStride0, kSplashStride);
      set(b + kSrcFormat, 0x000237ff);
      set(b + kUnpack, 0x03020001);
    }
    set(c + kCtlPipeActive, 1u << 16 | 1u << 17);
    set(c + kCtlFetchActive, 1u << 16 | 1u << 17);
    set(kMdp + kLmBase + lmStage(0) + kLmFgSrcSel, 0xc040);
    set(kMdp + kLmBase + kLmStride + lmStage(0) + kLmFgSrcSel, 0xc041);
  }
  set(kMdp + kLmBase + kLmOutSize, 2136u << 16 | 1600);
  set(kMdp + kLmBase + kLmStride + kLmOutSize, 2136u << 16 | 1600);
  set(kMdp + kLmBase + kLmStride + kLmOpMode, 0x80000000);
  for (unsigned i = 1; i <= 2; ++i) {
    const uint64_t b = kMdp + kIntfBase + kIntfStride * i;
    const unsigned htotal = 576, vtotal = 2310;
    set(b + kIntfEn, 1);
    set(b + kIntfHsyncCtl, htotal << 16 | 16);
    set(b + kIntfVsyncPeriod, vtotal * htotal);
    set(b + kIntfVsyncWidth, 2 * htotal);
    set(b + kIntfDispVStart, 106 * htotal);
    set(b + kIntfDispVEnd, (vtotal - 68) * htotal - 1);
    set(b + kIntfDispHctl, (htotal - 117 - 1) << 16 | 32);
    set(b + kIntfConfig2, 0x1011);
    set(b + kIntfCountEn, 3);
  }
  for (unsigned i = 0; i < kPipeCount; ++i) d.latchPipe(i);
}

// ----------------------------------------------------------------------------------------------------------- the surfaces
struct FakeSurface {
  std::vector<uint64_t> pages;   // 4 KiB pages of its memory
  uint64_t length = 0;
  int refs = 0;
  bool cleanable = true, mapFails = false;
  unsigned cleans = 0;
  // 16 KiB pages from base, every fourth 16 KiB page skipped (not contiguous)
  FakeSurface(uint64_t base, uint64_t bytes)
  {
    length = bytes;
    uint64_t pa = base;
    for (uint64_t off = 0; off < bytes; off += 4096) {
      pages.push_back(pa);
      pa += 4096;
      if ((off / 4096) % 4 == 3) pa += 16384 * ((off / 16384) % 4 == 3 ? 1 : 0);
    }
  }
};
struct Cookie {
  FakeSurface *s;
};

// ---------------------------------------------------------------------------------------------------------- the Hw model
struct HostHw {
  DpuModel &dpu;
  SmmuModel &smmu;
  Ram &ram;
  uint64_t now = 0, nextFrame = 8333;
  bool irq = true, deliver = true, irqDead = false;
  unsigned misses = 0, handled = 0, strays = 0;
  uint32_t gen = 0, armed = 0, sticky = 0;
  std::vector<std::string> notes;
  bool verbose = false, allocFails = false;
  FakeSurface testBuf{0x900000000ull, 2136ull * 12800};
  int liveCookies = 0;
  HostHw(DpuModel &d, SmmuModel &s, Ram &r) : dpu(d), smmu(s), ram(r) {}

  uint32_t rd(uint64_t pa) { return dpu.rd(pa); }
  void wr(uint64_t pa, uint32_t v) { dpu.wr(pa, v); }
  void barrier() {}
  uint32_t smmuRd(uint32_t off) { return smmu.rd(off); }
  void smmuWr(uint32_t off, uint32_t v) { smmu.wr(off, v); }
  uint64_t nowUs() { return now; }
  void tick()   // one frame start: the DPU, then the interrupt when live
  {
    now = nextFrame;
    nextFrame += 8333;
    dpu.frame();
    if (irqLive() && deliver && (dpu.rd(kMdss + kMdssIntrStatus) & kMdssIntrMdp)) {
      const uint32_t bits = serviceIrq([&](uint64_t pa) { return dpu.rd(pa); }, [&](uint64_t pa, uint32_t v) { dpu.wr(pa, v); });
      if (bits) {
        sticky |= bits;
        ++handled;
        ++gen;
      } else {
        ++strays;
      }
    }
  }
  void advance(uint64_t us)
  {
    const uint64_t target = now + us;
    while (nextFrame <= target) tick();
    now = target;
  }
  void delayUs(uint32_t us) { advance(us); }
  void sleepMs(uint32_t ms) { advance(uint64_t(ms) * 1000); }
  void vsyncArm() { armed = gen; }
  bool waitVsync(uint32_t us)
  {
    if (!irqLive()) {
      advance(us < 1000 ? us : 1000);
      return false;
    }
    const uint64_t target = now + us;
    while (nextFrame <= target) {
      tick();
      if (gen != armed) {
        armed = gen;
        return true;
      }
    }
    now = target;
    return false;
  }
  bool attached = false;
  void attachIrq() { attached = irq; }
  bool irqLive() const { return attached && !irqDead; }
  void irqMiss()
  {
    if (++misses >= 3) irqDead = true;
  }
  void irqDelivered() { misses = 0; }
  uint32_t takeIrqBits()
  {
    const uint32_t b = sticky;
    sticky = 0;
    return b;
  }
  void clean(const void *p, size_t n) { ram.clean(p, n); }
  bool allocTables(uint64_t **cpu, uint64_t *pa)
  {
    if (allocFails) return false;
    ram.alloc(size_t(sm::kTablePages) * 512);
    *cpu = ram.cpu.data();
    *pa = ram.pa;
    return true;
  }
  bool allocGuard(uint64_t *pa)
  {
    *pa = 0x8c2000000ull;
    return true;
  }
  uint64_t surfaceLength(const PianoDpuFlipRequest &r) { return static_cast<FakeSurface *>(r.surface)->length; }
  bool surfaceMap(const PianoDpuFlipRequest &r, uint64_t *pages, unsigned max, unsigned *count, void **cookie, const char **why)
  {
    FakeSurface *f = static_cast<FakeSurface *>(r.surface);
    if (f->mapFails || f->pages.size() > max) {
      *why = "the model refuses";
      return false;
    }
    for (size_t i = 0; i < f->pages.size(); ++i) pages[i] = f->pages[i];
    *count = unsigned(f->pages.size());
    *cookie = new Cookie{f};
    ++f->refs;
    ++liveCookies;
    return true;
  }
  bool surfaceClean(void *cookie, uint64_t, uint64_t)
  {
    FakeSurface *f = static_cast<Cookie *>(cookie)->s;
    ++f->cleans;
    return f->cleanable;
  }
  void surfaceRelease(void *cookie)
  {
    Cookie *c = static_cast<Cookie *>(cookie);
    --c->s->refs;
    --liveCookies;
    delete c;
  }
  bool testBuffer(uint64_t *pages, unsigned max, unsigned *count, void **cookie)
  {
    if (testBuf.pages.size() > max) return false;
    for (size_t i = 0; i < testBuf.pages.size(); ++i) pages[i] = testBuf.pages[i];
    *count = unsigned(testBuf.pages.size());
    *cookie = new Cookie{&testBuf};
    ++testBuf.refs;
    ++liveCookies;
    return true;
  }
  void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    notes.push_back(line);
    if (verbose) std::printf("PianoDpu: %s\n", line);
  }
  bool said(const char *needle) const
  {
    for (auto &n : notes)
      if (n.find(needle) != std::string::npos) return true;
    return false;
  }
};

// One whole rig: models, the survey of them, the engine.
struct Rig {
  Ram ram;
  SmmuModel smmu;
  DpuModel dpu;
  HostHw hw{dpu, smmu, ram};
  Survey s;
  SmmuSurvey m;
  Flipper<HostHw> f{hw};
  explicit Rig(int variant = 0, uint32_t sctlr = 0xe0) : smmu(sctlr)
  {
    smmu.ram = &ram;
    dpu.smmu = &smmu;
    bootState(dpu, variant);
    survey(s, [&](uint64_t pa) { return dpu.rd(pa); });
    m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    smmu.writes.clear();
    dpu.writes.clear();
    f.cfg.holdMs = 100;
    f.cfg.minuteMs = 1000;
  }
  bool ready(bool identity = false)
  {
    f.cfg.level = 4;
    return f.ladder(s, m, identity) && f.state == State::ready;
  }
};

static PianoDpuFlipRequest req(FakeSurface *surface, uint64_t row = 12800, uint32_t format = kFourccBgra)
{
  PianoDpuFlipRequest r{};
  r.size = sizeof r;
  r.version = kPianoDpuFlipVersion;
  r.surface = surface;
  r.memory = surface ? &surface->length : nullptr;   // any stable pointer per surface
  r.rowBytes = row;
  r.width = 3200;
  r.height = 2136;
  r.format = format;
  r.copyUs = 39000;
  return r;
}

// ----------------------------------------------------------------------------------------------------------------- tests
static void testTables()
{
  for (sm::Stage stage : {sm::Stage::s1, sm::Stage::s2}) {
    Ram ram;
    ram.alloc(size_t(sm::kTablePages) * 512);
    sm::Tables t;
    t.cpu = ram.cpu.data();
    t.pa = ram.pa;
    t.stage = stage;
    sm::buildTables(t, [&](const void *p, size_t n) { ram.clean(p, n); });
    CHECK(ram.coherent());
    Walk w = sm::lookup(t, uint32_t(kSplash) + 0x1234);
    CHECK(w.ok && w.pa == kSplash + 0x1234 && w.level == 1);
    CHECK(sm::lookup(t, 0x80000000u).ok && sm::lookup(t, 0x80000000u).pa == 0x80000000u);
    CHECK(sm::lookup(t, 0xffffffffu).ok && sm::lookup(t, 0xffffffffu).pa == 0xffffffffu);
    CHECK(!sm::lookup(t, 0x1000).ok);                     // nothing below the windows
    CHECK(!sm::lookup(t, sm::kWindowBase).ok);            // windows empty
    CHECK(!sm::lookup(t, 0x40000000).ok);                 // 1-2 GiB unmapped
    const uint64_t l1_3 = t.page(sm::kL1Page)[3];
    CHECK((l1_3 & sm::kAf) && sm::descWritable(stage, l1_3) && (l1_3 & 3u) == 1u && (l1_3 & sm::kXn));
    if (stage == sm::Stage::s1) CHECK(((l1_3 >> 2) & 7u) == 0 && ((l1_3 >> 6) & 3u) == 1);   // AttrIndx 0, AP 01
    else CHECK(((l1_3 >> 2) & 0xfu) == 5 && ((l1_3 >> 6) & 3u) == 3);                    // MemAttr NC, S2AP RW
    // A surface in slot 1: 16 KiB pages, not contiguous.
    FakeSurface a(0x8d0000000ull, 2136ull * 12800);
    const char *why = nullptr;
    CHECK(sm::mapSlot(t, 1, a.pages.data(), unsigned(a.pages.size()), 0x8c2000000ull, [&](const void *p, size_t n) { ram.clean(p, n); }, &why));
    CHECK(ram.coherent());
    bool all = true;
    for (size_t i = 0; i < a.pages.size(); i += 97) {
      const Walk p = sm::lookup(t, sm::slotIova(1) + uint32_t(i) * 4096 + 8);
      all &= p.ok && p.pa == a.pages[i] + 8 && p.level == 3 && !sm::descWritable(stage, p.desc[2]);
    }
    CHECK(all);
    const uint32_t end = sm::slotIova(1) + uint32_t(a.pages.size()) * 4096;
    CHECK(sm::lookup(t, end).ok && sm::lookup(t, end).pa == 0x8c2000000ull);                       // the guard
    CHECK(sm::lookup(t, end + 3 * 4096).ok && sm::lookup(t, end + 3 * 4096).pa == 0x8c2003000ull);
    CHECK(!sm::lookup(t, end + 4 * 4096).ok);
    CHECK(!sm::lookup(t, sm::slotIova(0)).ok && !sm::lookup(t, sm::slotIova(2)).ok);
    // A table write without a clean is not seen by the walker's memory.
    t.slotL3(2)[0] = sm::pageDesc(stage, 0x8d0000000ull, false);
    CHECK(!ram.coherent());
    ram.clean(t.slotL3(2), 8);
    sm::unmapSlot(t, 1, [&](const void *p, size_t n) { ram.clean(p, n); });
    sm::unmapSlot(t, 2, [&](const void *p, size_t n) { ram.clean(p, n); });
    CHECK(ram.coherent() && !sm::lookup(t, sm::slotIova(1)).ok);
    // Refusals: outside XNU's RAM, unaligned, too many, a bad guard.
    uint64_t bad[2] = {0x80000000ull, 0x8d0000000ull};
    CHECK(!sm::mapSlot(t, 0, bad, 2, 0x8c2000000ull, [&](const void *, size_t) {}, &why));
    bad[0] = 0x8d0000800ull;
    CHECK(!sm::mapSlot(t, 0, bad, 2, 0x8c2000000ull, [&](const void *, size_t) {}, &why));
    std::vector<uint64_t> many(sm::kSlotPages, 0x8d0000000ull);
    CHECK(!sm::mapSlot(t, 0, many.data(), sm::kSlotPages - sm::kGuardPages + 1, 0x8c2000000ull, [&](const void *, size_t) {}, &why));
    CHECK(sm::mapSlot(t, 0, many.data(), sm::kSlotPages - sm::kGuardPages, 0x8c2000000ull, [&](const void *, size_t) {}, &why));
    CHECK(!sm::mapSlot(t, 0, many.data(), 4, 0x8c2001000ull, [&](const void *, size_t) {}, &why));
    CHECK(!sm::mapSlot(t, sm::kSlots, many.data(), 4, 0x8c2000000ull, [&](const void *, size_t) {}, &why));
  }
  CHECK(sm::kTablePages == 66 && sm::slotIova(3) == 0x16000000u && sm::kSlotPages == 8192);
  CHECK(sm::kTcrS1 == 0x00800020u && sm::tcr2S1(5) == 0x00038005u && sm::tcrS2(5) == 0x00050060u);
}

static void testPlan()
{
  {   // the expected case: bank off, stage 1, VA64
    SmmuModel smmu(0xe0);
    const SmmuSurvey m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    const sm::BankPlan p = sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), false);
    CHECK(p.ok && p.route == Route::bankOff && p.stage == sm::Stage::s1 && p.bank == 2 && p.group == 5 && p.cb == (128 + 2) * 4096u);
    CHECK(p.pasize == 5);
    CHECK(!sm::planBank(m, smmu.rd(0x20), 0x00001005, false).ok);            // OAS 32 bits
    CHECK(sm::planBank(m, smmu.rd(0x20), 0x00001015, false).pasize == 1);    // OAS 36 bits: XNU's RAM ends at 48 GiB
    CHECK(sm::planBank(m, smmu.rd(0x20), 0x00001025, false).pasize == 2);    // OAS 40 bits
    CHECK(sm::planBank(m, smmu.rd(0x20), 0x00001075, false).pasize == 5);    // above 48: 48
    CHECK(!sm::planBank(m, smmu.rd(0x20), 0x00000055, false).ok);            // no 4 KiB AArch64 tables
    CHECK(!sm::planBank(m, 0x2000007f, smmu.rd(0x28), false).ok);            // no stage 1
    const sm::BankRegs found{0xe0, 0, 0x12345000, 0x00030000, 0x80000000, 0xff44, 0};
    const sm::BankRegs w = sm::wantRegs(p, found, 0x8c1000000ull);
    CHECK(w.ttbr0lo == 0xc1000000u && w.ttbr0hi == 0x00030008u && w.tcr == 0x00800020u && w.tcr2 == 0x00038005u && w.mair0 == 0x44);
    CHECK(w.sctlr == 0x61);   // 0xe0 without CFCFG, with M, CFRE, CFIE: sheng's 0x61
  }
  {   // translation on: only with the boot loader's tables verified
    SmmuModel smmu(0x1e5);
    const SmmuSurvey m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    CHECK(!sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), false).ok);
    const sm::BankPlan p = sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), true);
    CHECK(p.ok && p.route == Route::bankOn);
    CHECK(sm::wantRegs(p, sm::BankRegs{0x1e5, 0, 0, 0, 0, 0, 0}, 0x8c1000000ull).sctlr == 0x165);   // CFCFG off, M CFRE CFIE on
  }
  {   // bypass, none, fault, level 1: refused
    SmmuModel smmu(0);
    smmu.regs[0xc00 + 4 * 5] = 0x00010000;
    SmmuSurvey m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    CHECK(!sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), true).ok);
    smmu.regs[0x800 + 4 * 5] = 0;
    m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    CHECK(!sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), true).ok);
    SmmuModel s1(0xe0);
    m = surveySmmu(1, [&](uint32_t off) { return s1.rd(off); });
    CHECK(!sm::planBank(m, s1.rd(0x20), s1.rd(0x28), true).ok);
  }
  {   // stage 2 alone, off: ours, the stage 2 format; stage 2 on, nested, stage-2-fault and AArch32 banks: refused
    SmmuModel smmu(0);
    smmu.regs[0x1000 + 4 * 2] = 0x00000005;   // type 0, VMID 5
    SmmuSurvey m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    sm::BankPlan p = sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), false);
    CHECK(p.ok && p.stage == sm::Stage::s2 && route(m) == Route::other);
    CHECK(sm::wantRegs(p, sm::BankRegs{}, 0x8c1000000ull).tcr == 0x00050060u);
    smmu.regs[smmu.cb()] = 1;
    m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    CHECK(!sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), true).ok);
    smmu.regs[smmu.cb()] = 0;
    for (uint32_t type : {2u, 3u}) {
      smmu.regs[0x1000 + 4 * 2] = type << 16;
      m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
      CHECK(!sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), true).ok);
    }
    smmu.regs[0x1000 + 4 * 2] = 0x0001f000;
    smmu.regs[0x1800 + 4 * 2] = 0;
    m = surveySmmu(2, [&](uint32_t off) { return smmu.rd(off); });
    CHECK(!sm::planBank(m, smmu.rd(0x20), smmu.rd(0x28), false).ok);
  }
}

struct Io {
  SmmuModel &m;
  HostHw *hw = nullptr;
  uint32_t rd(uint32_t off) { return m.rd(off); }
  void wr(uint32_t off, uint32_t v) { m.wr(off, v); }
  void delayUs(uint32_t us)
  {
    if (hw) hw->advance(us);
  }
};

static void testEnable()
{
  {   // the expected case, with the display fetching the splash through the new table every frame of the watch
    Rig r;
    Io io{r.smmu, &r.hw};
    const sm::BankPlan p = sm::planBank(r.m, r.smmu.rd(0x20), r.smmu.rd(0x28), false);
    uint64_t *cpu = nullptr, pa = 0;
    r.hw.allocTables(&cpu, &pa);
    sm::Tables t{cpu, pa, p.stage};
    sm::buildTables(t, [&](const void *q, size_t n) { r.ram.clean(q, n); });
    const sm::BankRegs found = sm::readBank(io, p.cb);
    const unsigned before = r.smmu.translations;
    const sm::EnableResult e = sm::enableBank(p, found, pa, 50000, io);
    CHECK(e.ok && e.watchedUs == 50000 && e.fsr == 0);
    CHECK(r.smmu.translations > before + 20);              // 6 frames x 4 fetches, all identity
    CHECK(r.dpu.lastFetch.size() == 4 && r.dpu.lastFetch[0] == kSplash && !(r.dpu.status & (1u << 26)));
    CHECK(r.smmu.onlyBankWrites());                        // the route is never written
    // Order: TTBR0 first, SCTLR (M) last; the TLB synced before M.
    size_t ttbr = 99, sync = 99, sctlr = 99;
    for (size_t i = 0; i < r.smmu.writes.size(); ++i) {
      const uint32_t off = r.smmu.writes[i].first - p.cb;
      if (off == sm::kTtbr0Lo && ttbr == 99) ttbr = i;
      if (off == sm::kTlbSync) sync = i;
      if (off == sm::kSctlr) sctlr = i;
    }
    CHECK(ttbr < sync && sync < sctlr && sctlr == r.smmu.writes.size() - 1);
    for (auto &w : r.smmu.writes)   // the TLB operations' ignored values are never 0; MAIR1 is not written
      CHECK((w.second != 0 || (w.first - p.cb != sm::kTlbiAll && w.first - p.cb != sm::kTlbSync)) && w.first - p.cb != sm::kMair1);
    CHECK(r.smmu.rd(p.cb) == 0x61 && r.smmu.rd(p.cb + sm::kTcr2) == 0x38005 && r.smmu.rd(p.cb + sm::kMair0) == 0x44);
    // Back as found.
    CHECK(sm::restoreBank(p, found, io));
    const sm::BankRegs now = sm::readBank(io, p.cb);
    CHECK(now.sctlr == found.sctlr && now.ttbr0lo == found.ttbr0lo && now.ttbr0hi == found.ttbr0hi && now.tcr == found.tcr &&
          now.tcr2 == found.tcr2 && now.mair0 == found.mair0);
  }
  {   // the bank ignores TTBR0: refused, as found
    Rig r;
    r.smmu.ignoreTtbr = true;
    Io io{r.smmu, &r.hw};
    const sm::BankPlan p = sm::planBank(r.m, r.smmu.rd(0x20), r.smmu.rd(0x28), false);
    const sm::BankRegs found = sm::readBank(io, p.cb);
    const sm::EnableResult e = sm::enableBank(p, found, 0x8c1000000ull, 50000, io);
    CHECK(!e.ok && std::strstr(e.why, "ignores") && e.restored);
    CHECK(r.smmu.rd(p.cb) == 0xe0 && r.smmu.rd(p.cb + sm::kTcr) == found.tcr);
  }
  {   // a stuck TLB sync: refused before M, as found
    Rig r;
    r.smmu.syncStuck = true;
    Io io{r.smmu, nullptr};
    const sm::BankPlan p = sm::planBank(r.m, r.smmu.rd(0x20), r.smmu.rd(0x28), false);
    const sm::BankRegs found = sm::readBank(io, p.cb);
    const sm::EnableResult e = sm::enableBank(p, found, 0x8c1000000ull, 50000, io);
    CHECK(!e.ok && std::strstr(e.why, "TLB") && !(r.smmu.rd(p.cb) & 1u));
  }
  {   // M does not stick: refused, SCTLR exactly as found
    Rig r;
    r.smmu.ignoreSctlrM = true;
    Io io{r.smmu, &r.hw};
    const sm::BankPlan p = sm::planBank(r.m, r.smmu.rd(0x20), r.smmu.rd(0x28), false);
    const sm::BankRegs found = sm::readBank(io, p.cb);
    const sm::EnableResult e = sm::enableBank(p, found, 0x8c1000000ull, 50000, io);
    CHECK(!e.ok && e.restored && r.smmu.rd(p.cb) == 0xe0);
  }
  {   // the walk starts at level 0 (what SM8550 seemed to do with SEP 0): the splash faults, the watch catches it, undone
    Rig r;
    r.smmu.walkFromLevel0 = true;
    Io io{r.smmu, &r.hw};
    const sm::BankPlan p = sm::planBank(r.m, r.smmu.rd(0x20), r.smmu.rd(0x28), false);
    uint64_t *cpu = nullptr, pa = 0;
    r.hw.allocTables(&cpu, &pa);
    sm::Tables t{cpu, pa, p.stage};
    sm::buildTables(t, [&](const void *q, size_t n) { r.ram.clean(q, n); });
    const sm::BankRegs found = sm::readBank(io, p.cb);
    const sm::EnableResult e = sm::enableBank(p, found, pa, 50000, io);
    CHECK(!e.ok && (e.fsr & 2u) && e.farLo == uint32_t(kSplash) && e.watchedUs < 10000 && e.restored);
    CHECK(r.smmu.rd(p.cb) == 0xe0 && !(r.smmu.rd(p.cb + sm::kFsr) & sm::kFsrFaults));
    r.dpu.status = 0;
    r.hw.advance(9000);
    CHECK(!(r.dpu.status & (1u << 26)) && r.dpu.lastFetch[0] == kSplash);   // the bank off again: the splash fetched as before
  }
}

static void testBankOn()
{
  // The boot loader's table at 0x9e000000: 1 GiB identity blocks for 2-4 GiB (SunUEFI's "translation on?" case).
  BootTables boot;
  boot.mem[0x9e000000 + 8 * 2] = 0x80000000ull | 0x401;
  boot.mem[0x9e000000 + 8 * 3] = 0xc0000000ull | 0x401;
  Rig r(0, 0x1e5);
  r.smmu.boot = &boot;
  r.smmu.regs[r.smmu.cb() + sm::kTcr] = 32;
  r.smmu.regs[r.smmu.cb() + sm::kTcr2] = 0x38005;
  r.smmu.regs[r.smmu.cb() + sm::kTtbr0Lo] = 0x9e000000;
  r.smmu.regs[r.smmu.cb() + sm::kTtbr0Hi] = 0x00050000;
  r.m = surveySmmu(2, [&](uint32_t off) { return r.smmu.rd(off); });
  r.hw.advance(9000);
  CHECK(r.dpu.lastFetch.size() == 4 && r.dpu.lastFetch[0] == kSplash);
  unsigned walks = 0;
  uint64_t bad = 0;
  CHECK(splashIdentity(r.m.found[0].bank, [&](uint64_t a) { return boot.load64(a); }, &walks, &bad) && walks == 23);
  BootTables shifted = boot;
  shifted.mem[0x9e000000 + 8 * 3] = 0x880000000ull | 0x401;   // the splash elsewhere: not identity
  CHECK(!splashIdentity(r.m.found[0].bank, [&](uint64_t a) { return shifted.load64(a); }, &walks, &bad) && bad == kSplash);
  CHECK(!r.ready(false) && r.hw.said("walk them first"));
  Rig q(0, 0x1e5);
  q.smmu.boot = &boot;
  q.smmu.regs[q.smmu.cb() + sm::kTcr] = 32;
  q.smmu.regs[q.smmu.cb() + sm::kTcr2] = 0x38005;
  q.smmu.regs[q.smmu.cb() + sm::kTtbr0Lo] = 0x9e000000;
  q.smmu.regs[q.smmu.cb() + sm::kTtbr0Hi] = 0x00050000;
  q.m = surveySmmu(2, [&](uint32_t off) { return q.smmu.rd(off); });
  CHECK(q.ready(true));
  CHECK(q.smmu.writes.size() > 3 && q.smmu.writes[0].first == q.smmu.cb() && q.smmu.writes[0].second == (0x1e5u & ~1u));   // off first
  CHECK(q.smmu.rd(q.smmu.cb() + sm::kTtbr0Lo) == uint32_t(q.ram.pa) && (q.smmu.rd(q.smmu.cb() + sm::kTtbr0Hi) >> 16) == 5);   // ASID kept
  CHECK(q.f.smmuStop("test") && q.smmu.rd(q.smmu.cb()) == 0x1e5 && q.smmu.rd(q.smmu.cb() + sm::kTtbr0Lo) == 0x9e000000);
  q.hw.advance(9000);
  CHECK(q.dpu.lastFetch[0] == kSplash && q.smmu.onlyBankWrites());
}

static void testLadder()
{
  {   // step 1: three reflushes, interrupt
    Rig r;
    r.f.cfg.level = 1;
    CHECK(r.f.ladder(r.s, r.m, false));
    CHECK(r.hw.said("flip 1: reflush 3/3") && r.hw.said("B1b step 1 passed") && r.hw.handled >= 3 && r.hw.strays == 0);
    CHECK(r.f.state == State::off && r.dpu.flushWrites == 3 && r.smmu.writes.empty());
    // Only the pipes' address/stride/format, the CTL flush and the interrupt enable/clear were written, all pipe values as found.
    std::set<uint64_t> allowed = {DpuModel::kFlush, kMdp + kTopIntrEn, kMdp + kTopIntrClear};
    for (unsigned v = 0; v < 2; ++v)
      for (uint32_t off : {kSrc0Addr, kYStride0, kSrcFormat}) allowed.insert(kMdp + kPipes[v].base + off);
    bool ok = true;
    for (auto &w : r.dpu.writes) {
      ok &= allowed.count(w.first) != 0;
      if (w.first == kMdp + kPipes[0].base + kSrc0Addr || w.first == kMdp + kPipes[1].base + kSrc0Addr) ok &= w.second == uint32_t(kSplash);
      if (w.first == kMdp + kPipes[1].base + kYStride0) ok &= w.second == kSplashStride;
      if (w.first == kMdp + kPipes[1].base + kSrcFormat) ok &= w.second == 0x000237ffu;
      if (w.first == DpuModel::kFlush) ok &= w.second == 0x3;
    }
    CHECK(ok);
    CHECK(r.dpu.en == 0);   // the vsync enable only while a flush is pending
  }
  {   // polled
    Rig r;
    r.f.cfg.level = 1;
    r.f.cfg.irq = false;
    r.hw.irq = false;
    CHECK(r.f.ladder(r.s, r.m, false) && r.hw.handled == 0);
  }
  {   // a flush that never latches: failed, nothing more
    Rig r;
    r.f.cfg.level = 4;
    r.dpu.latchNever = true;
    CHECK(!r.f.ladder(r.s, r.m, false) && r.f.state == State::failed && r.hw.said("did NOT latch") && r.smmu.writes.empty());
  }
  {   // an underrun before any flip: stopped
    Rig r;
    r.f.cfg.level = 1;
    r.dpu.underrunNext = true;
    CHECK(!r.f.ladder(r.s, r.m, false) && r.hw.said("an underrun before any flip"));
    Rig q;
    q.f.cfg.level = 1;
    q.f.cfg.underrun = false;
    q.dpu.underrunNext = true;
    CHECK(q.f.ladder(q.s, q.m, false));
  }
  {   // another block holds the MDSS line: no interrupt wanted
    Rig r;
    r.dpu.mdssOther = 1u << 4;
    r.f.cfg.level = 1;
    CHECK(r.f.ladder(r.s, r.m, false) && !r.f.interruptWanted() && !r.hw.attached && r.hw.handled == 0);
    CHECK(r.hw.said("another block holds the line"));
  }
  {   // the status does not latch with its enable off: underruns enabled to latch
    Rig r;
    r.dpu.rawLatch = false;
    r.f.cfg.level = 1;
    r.hw.irq = false;
    r.f.cfg.irq = false;
    CHECK(r.f.setup(r.s) && !r.f.rawLatch && r.f.baseEn == kUnderrunBits && r.dpu.en == kUnderrunBits);
  }
  {   // step 2: 64 rows lower while held, then back; the addresses as found at the end
    Rig r;
    r.f.cfg.level = 2;
    r.f.cfg.holdMs = 100;
    CHECK(r.f.ladder(r.s, r.m, false) && r.hw.said("B1b step 2 passed"));
    CHECK(r.hw.said("address fc8c8000"));   // 0xfc800000 + 64 * 12800
    CHECK(r.dpu.act(0, kSrc0Addr) == uint32_t(kSplash) && r.dpu.act(1, kSrc0Addr) == uint32_t(kSplash) && r.smmu.writes.empty());
  }
  {   // step 3: the test pattern through window 0, then the bank as found
    Rig r;
    r.f.cfg.level = 3;
    r.f.cfg.holdMs = 100;
    r.f.cfg.minuteMs = 2000;
    std::vector<uint64_t> shown;
    CHECK(r.f.ladder(r.s, r.m, false) && r.hw.said("B1b step 3 passed"));
    CHECK(r.hw.testBuf.refs == 0 && r.hw.liveCookies == 0);
    CHECK(r.smmu.rd(r.smmu.cb()) == 0xe0 && r.smmu.rd(r.smmu.cb() + sm::kTtbr0Lo) == 0x12345000 && r.smmu.onlyBankWrites());
    CHECK(r.dpu.act(0, kSrc0Addr) == uint32_t(kSplash) && r.dpu.lastFetch[0] == kSplash);
  }
  {   // step 3, the pattern on the panel: what the DPU fetched is the buffer's pages
    Rig r;
    r.f.cfg.level = 3;
    CHECK(r.f.setup(r.s) && r.f.smmuStart(r.m, false));
    unsigned count = 0;
    void *cookie = nullptr;
    CHECK(r.hw.testBuffer(r.f.pages, sm::kSlotPages, &count, &cookie));
    const char *why = nullptr;
    CHECK(r.f.mapPages(0, count, &why));
    CHECK(r.f.flushTo(Target{sm::slotIova(0), kSplashStride, true}).ok);
    r.hw.advance(9000);
    const auto &tp = r.hw.testBuf.pages;
    CHECK(r.dpu.lastFetch.size() == 4 && r.dpu.lastFetch[0] == tp[0] && r.dpu.lastFetch[2] == tp[1] + 6400 % 4096);
    CHECK(r.dpu.lastFetch[3] == tp.back() + 4095 && !(r.smmu.rd(r.smmu.cb() + sm::kFsr) & sm::kFsrFaults));
    CHECK(r.dpu.act(0, kSrcFormat) == 0x000236ffu);   // alpha cleared on a surface
    CHECK(r.f.flushTo(Target{}).ok && r.dpu.act(0, kSrcFormat) == 0x000237ffu);
    r.hw.surfaceRelease(cookie);
  }
  {   // step 4 refused: tables not allocatable, bank changed since the survey
    Rig r;
    r.hw.allocFails = true;
    CHECK(!r.ready() && r.f.state == State::failed && r.hw.said("no memory for the tables"));
    BootTables boot;   // the bank turned on behind the survey's back, with an identity table (the display keeps fetching)
    boot.mem[0x9e000000 + 8 * 2] = 0x80000000ull | 0x401;
    boot.mem[0x9e000000 + 8 * 3] = 0xc0000000ull | 0x401;
    Rig q;
    q.smmu.boot = &boot;
    q.smmu.regs[q.smmu.cb() + sm::kTcr] = 32;
    q.smmu.regs[q.smmu.cb() + sm::kTcr2] = 0x38005;
    q.smmu.regs[q.smmu.cb() + sm::kTtbr0Lo] = 0x9e000000;
    q.smmu.regs[q.smmu.cb()] = 0xe1;
    CHECK(!q.ready() && q.hw.said("changed since the survey"));
  }
}

static void testService()
{
  Rig r;
  CHECK(r.ready() && r.hw.said("serving flips"));
  CHECK(r.smmu.rd(r.smmu.cb()) == 0x61);
  FakeSurface a(0x8d0000000ull, 2136ull * 12800), b(0x8e0000000ull, 2136ull * 12800), c(0x8f0000000ull, 2136ull * 12800),
      d(0x900000000ull + 0x4000000, 2136ull * 12800), e(0x910000000ull, 2136ull * 12800 + 16384);
  PianoDpuFlipRequest q = req(&a);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && q.latchUs > 0 && q.latchUs < 8400);
  r.hw.advance(9000);
  CHECK(r.dpu.act(0, kSrc0Addr) == sm::slotIova(0) && r.dpu.act(1, kSrc0Addr) == sm::slotIova(0) && r.dpu.act(1, kSrcXy) == 1600);
  CHECK(r.dpu.lastFetch[0] == a.pages[0] && r.dpu.lastFetch[2] == a.pages[1] + 6400 % 4096 && r.dpu.lastFetch[3] == a.pages.back() + 4095);
  CHECK(a.refs == 1 && a.cleans == 1 && r.f.slots.shown == 0 && r.dpu.act(0, kSrcFormat) == 0x000236ffu);
  CHECK(r.hw.said("flip 1: 3200x2136 row 12800 -> window 0 iova 0x10000000 (mapped now)"));
  // B, then A again (its window kept), C, D: four windows.
  q = req(&b);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.slots.shown == 1 && a.refs == 1);
  q = req(&a);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.slots.shown == 0 && r.f.st.maps == 2);
  q = req(&c);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.slots.shown == 2);
  q = req(&d);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.slots.shown == 3 && r.f.slots.used() == 4);
  // E takes the least recently used window that is not on screen: B's (window 1). The TLB had B's translations of it.
  r.hw.advance(9000);
  q = req(&e);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.slots.shown == 1 && b.refs == 0 && e.refs == 1 && r.f.st.evictions == 1);
  r.hw.advance(9000);
  CHECK(r.dpu.lastFetch[0] == e.pages[0] && r.dpu.lastFetch[3] == e.pages[(2135ull * 12800 + 12799) / 4096] + (2135ull * 12800 + 12799) % 4096);
  // E again: no flush, cleaned again.
  const unsigned flushes = r.dpu.flushWrites;
  q = req(&e);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.dpu.flushWrites == flushes && e.cleans == 2 && r.f.st.reshows == 1);
  // Refusals: nothing written to the pipes.
  const size_t writes = r.dpu.writes.size();
  q = req(&a, 12800, 0x52476841);   // 'RGhA'
  CHECK(r.f.request(q) == kPianoDpuFlipFormat);
  q = req(&a, 12804);
  CHECK(r.f.request(q) == kPianoDpuFlipStride);
  q = req(&a, 0x10000);
  CHECK(r.f.request(q) == kPianoDpuFlipStride);
  q = req(&a, 12736);
  CHECK(r.f.request(q) == kPianoDpuFlipStride);
  q = req(&a);
  q.width = 1600;
  CHECK(r.f.request(q) == kPianoDpuFlipSize);
  q = req(&a);
  q.offset = 32;
  CHECK(r.f.request(q) == kPianoDpuFlipAlign);
  q = req(&a);
  q.planes = 2;
  CHECK(r.f.request(q) == kPianoDpuFlipPlanes);
  q = req(&a);
  q.version = 7;
  CHECK(r.f.request(q) == kPianoDpuFlipVersionMismatch);
  FakeSurface small(0x920000000ull, 2135ull * 12800);
  q = req(&small);
  CHECK(r.f.request(q) == kPianoDpuFlipSize);
  FakeSurface huge(0x930000000ull, 33ull << 20);
  q = req(&huge);
  CHECK(r.f.request(q) == kPianoDpuFlipSize && huge.refs == 0);
  FakeSurface broken(0x940000000ull, 2136ull * 12800);
  broken.mapFails = true;
  q = req(&broken);
  CHECK(r.f.request(q) == kPianoDpuFlipMemory && broken.refs == 0);
  CHECK(r.dpu.writes.size() == writes && r.f.slots.shown == 1);
  // A wider row (13312) and a surface larger than the panel.
  FakeSurface wide(0x950000000ull, 2200ull * 13312);
  q = req(&wide, 13312);
  q.width = 3328;
  q.height = 2200;
  CHECK(r.f.request(q) == kPianoDpuFlipShown);
  r.hw.advance(9000);
  CHECK((r.dpu.act(0, kYStride0) & 0xffffu) == 13312 && r.dpu.lastFetch[3] == wide.pages[(2135ull * 13312 + 12799) / 4096] + (2135ull * 13312 + 12799) % 4096);
  // Back to the boot framebuffer.
  q = req(nullptr);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.slots.shown == -1 && r.dpu.act(0, kSrc0Addr) == uint32_t(kSplash));
  CHECK((r.dpu.act(0, kYStride0) & 0xffffu) == kSplashStride && r.dpu.act(0, kSrcFormat) == 0x000237ffu);
  const unsigned again = r.dpu.flushWrites;
  q = req(nullptr);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.dpu.flushWrites == again);   // already there
  // All windows in use with one on screen and none free: the LRU one that is not shown goes.
  q = req(&a);
  CHECK(r.f.request(q) == kPianoDpuFlipShown);
  CHECK(r.f.slots.victim() >= 0 && r.f.slots.victim() != r.f.slots.shown);
  // The summary after a minute.
  r.hw.advance(61000000);
  q = req(&c);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.hw.said("flips: ") && r.hw.said("copies avoided"));
  CHECK(r.smmu.onlyBankWrites() && r.hw.irqLive() && r.hw.strays == 0);
}

static void testClean()
{
  Rig r;
  CHECK(r.ready());
  FakeSurface a(0x8d0000000ull, 2136ull * 12800);
  a.cleanable = false;
  PianoDpuFlipRequest q = req(&a);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.st.uncleaned == 1 && r.hw.said("without"));
  r.f.cfg.clean = 2;
  FakeSurface b(0x8e0000000ull, 2136ull * 12800);
  b.cleanable = false;
  q = req(&b);
  CHECK(r.f.request(q) == kPianoDpuFlipClean && r.f.slots.shown == 0);
  r.f.cfg.clean = 0;
  FakeSurface c(0x8f0000000ull, 2136ull * 12800);
  q = req(&c);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && c.cleans == 0);
}

static void testVariants()
{
  {   // VIG1 by its address (+6400), SRC_XY 0
    Rig r(1);
    CHECK(r.ready());
    FakeSurface a(0x8d0000000ull, 2136ull * 13312);
    PianoDpuFlipRequest q = req(&a, 13312);
    CHECK(r.f.request(q) == kPianoDpuFlipShown);
    CHECK(r.dpu.act(1, kSrc0Addr) == sm::slotIova(0) + 6400 && r.dpu.act(0, kSrc0Addr) == sm::slotIova(0));
    q = req(nullptr);
    CHECK(r.f.request(q) == kPianoDpuFlipShown && r.dpu.act(1, kSrc0Addr) == uint32_t(kSplash) + 6400);
  }
  {   // one DMA pipe, multirect: SRC0 and SRC1, both stride halves, the DMA0 flush bit
    Rig r(2);
    CHECK(r.ready() && r.f.path.count == 1 && r.f.path.rects == 2 && r.f.path.flushMask == (1u << 11) && !r.f.path.alpha);
    FakeSurface a(0x8d0000000ull, 2136ull * 12800);
    PianoDpuFlipRequest q = req(&a);
    CHECK(r.f.request(q) == kPianoDpuFlipShown);
    CHECK(r.dpu.act(4, kSrc0Addr) == sm::slotIova(0) && r.dpu.act(4, kSrc1Addr) == sm::slotIova(0) + 6400);
    CHECK(r.dpu.act(4, kYStride0) == (12800u << 16 | 12800u));
    r.hw.advance(9000);
    CHECK(r.dpu.lastFetch.size() == 4 && r.dpu.lastFetch[2] == a.pages[1] + 6400 % 4096);
    q = req(nullptr);
    CHECK(r.f.request(q) == kPianoDpuFlipShown && r.dpu.act(4, kSrc1Addr) == uint32_t(kSplash) + 6400);
  }
  {   // the flush override
    Rig r;
    r.f.cfg.flush = 0x20003;
    r.f.cfg.level = 1;
    CHECK(r.f.ladder(r.s, r.m, false) && r.f.path.flushMask == 0x20003);
  }
  {   // not flippable: command mode, no splash pipe, an RGBA format
    Rig r;
    r.dpu.regs[kMdp + kCtlBase + kCtlTop] |= 1u << 17;
    survey(r.s, [&](uint64_t pa) { return r.dpu.rd(pa); });
    CHECK(!flipPath(r.s, 0).ok);
    Rig q;
    q.dpu.regs[kMdp + kPipes[0].base + kUnpack] = 0x03010002;
    survey(q.s, [&](uint64_t pa) { return q.dpu.rd(pa); });
    CHECK(!flipPath(q.s, 0).ok);
    Rig z;
    z.dpu.regs[kMdp + kPipes[0].base + kSrc0Addr] = 0x10000000;
    z.dpu.regs[kMdp + kPipes[1].base + kSrc0Addr] = 0x10000000;
    survey(z.s, [&](uint64_t pa) { return z.dpu.rd(pa); });
    CHECK(!flipPath(z.s, 0).ok);
  }
  CHECK(rectAddress(6400, Target{0x10000000, 13312, true}) == 0x10000000 + 6400);
  CHECK(rectAddress(10 * 12800 + 6400, Target{0x10000000, 13312, true}) == 0x10000000 + 10 * 13312 + 6400);
}

static void testSafety()
{
  {   // a flip that never latches: reverted (the revert does not latch either: windows and translation stay), off for the boot
    Rig r;
    CHECK(r.ready());
    FakeSurface a(0x8d0000000ull, 2136ull * 12800);
    PianoDpuFlipRequest q = req(&a);
    r.dpu.latchNever = true;
    CHECK(r.f.request(q) == kPianoDpuFlipLatch && r.f.state == State::failed);
    CHECK(r.hw.said("did NOT latch") && r.f.smmuOn && a.refs == 1);   // the pipe may fetch window 0: nothing let go
    CHECK(r.hw.said("revert: the boot framebuffer did NOT latch"));
    q = req(&a);
    CHECK(r.f.request(q) == kPianoDpuFlipOff);
    q = req(nullptr);
    CHECK(r.f.request(q) == kPianoDpuFlipShown);
  }
  {   // a missed latch whose revert latches: translation as found, the surfaces let go
    Rig r;
    CHECK(r.ready());
    FakeSurface a(0x8d0000000ull, 2136ull * 12800), b(0x8e0000000ull, 2136ull * 12800);
    PianoDpuFlipRequest q = req(&a);
    CHECK(r.f.request(q) == kPianoDpuFlipShown);
    r.f.cfg.latchMs = 5;   // shorter than a frame: this flip "misses", the revert gets 5 ms too...
    q = req(&b);
    const uint32_t res = r.f.request(q);
    CHECK(res == kPianoDpuFlipLatch && r.f.state == State::failed);
  }
  {   // an SMMU fault at a flip: reverted, translation back as found, every window let go
    Rig r;
    CHECK(r.ready());
    FakeSurface a(0x8d0000000ull, 2136ull * 12800), b(0x8e0000000ull, 2136ull * 12800);
    PianoDpuFlipRequest q = req(&a);
    CHECK(r.f.request(q) == kPianoDpuFlipShown);
    r.smmu.faultAfter = 1;
    q = req(&b);
    CHECK(r.f.request(q) == kPianoDpuFlipFault && r.f.state == State::failed && !r.f.smmuOn);
    CHECK(a.refs == 0 && b.refs == 0 && r.hw.liveCookies == 0 && r.smmu.rd(r.smmu.cb()) == 0xe0);
    CHECK(r.dpu.act(0, kSrc0Addr) == uint32_t(kSplash) && r.hw.said("revert: done"));
    r.hw.advance(9000);
    CHECK(r.dpu.lastFetch[0] == kSplash && r.smmu.onlyBankWrites());
  }
  {   // an underrun at a flip
    Rig r;
    CHECK(r.ready());
    FakeSurface a(0x8d0000000ull, 2136ull * 12800);
    r.dpu.underrunNext = true;
    PianoDpuFlipRequest q = req(&a);
    CHECK(r.f.request(q) == kPianoDpuFlipUnderrun && r.f.state == State::failed && a.refs == 0 && !r.f.smmuOn);
  }
  {   // the watch: a fault while a surface is shown
    Rig r;
    CHECK(r.ready());
    FakeSurface a(0x8d0000000ull, 2136ull * 12800);
    PianoDpuFlipRequest q = req(&a);
    CHECK(r.f.request(q) == kPianoDpuFlipShown && r.f.watching());
    r.f.watch();
    CHECK(r.f.state == State::ready);
    r.smmu.faultAfter = 1;
    r.smmu.tlb.clear();
    r.hw.advance(9000);
    r.f.watch();
    CHECK(r.f.state == State::failed && a.refs == 0 && r.hw.said("watch: SMMU fault") && !r.f.watching());
  }
  {   // the watch: an underrun the handler cleared (sticky bits)
    Rig r;
    CHECK(r.ready());
    FakeSurface a(0x8d0000000ull, 2136ull * 12800);
    PianoDpuFlipRequest q = req(&a);
    CHECK(r.f.request(q) == kPianoDpuFlipShown);
    r.hw.sticky |= 1u << 28;
    r.f.watch();
    CHECK(r.f.state == State::failed && r.hw.said("underrun"));
  }
}

static void testIrq()
{
  DpuModel d;
  auto rd = [&](uint64_t pa) { return d.rd(pa); };
  auto wr = [&](uint64_t pa, uint32_t v) { d.wr(pa, v); };
  CHECK(serviceIrq(rd, wr) == 0);                         // nothing pending: a stray
  d.status = kVsyncIntf1 | 1u << 29;
  CHECK(serviceIrq(rd, wr) == 0);                         // pending but not enabled: the line is low, a stray
  d.en = kVsyncIntf1;
  CHECK(serviceIrq(rd, wr) == kVsyncIntf1 && d.status == 1u << 29);   // ours cleared, the rest kept
  d.mdssOther = 1u << 4;
  CHECK(serviceIrq(rd, wr) == 0);                         // DSI0's: not the DPU's
  // Interrupts that never come: three misses, then polled; the flips still latch.
  Rig r;
  CHECK(r.ready());
  r.hw.deliver = false;
  FakeSurface a(0x8d0000000ull, 2136ull * 12800), b(0x8e0000000ull, 2136ull * 12800);
  PianoDpuFlipRequest q = req(&a);
  CHECK(r.f.request(q) == kPianoDpuFlipShown && q.latchUs < 11000);
  for (int i = 0; i < 4 && r.hw.irqLive(); ++i) {
    q = req(i & 1 ? &a : &b);
    CHECK(r.f.request(q) == kPianoDpuFlipShown);
  }
  CHECK(!r.hw.irqLive());
  q = req(&a);
  CHECK(r.f.request(q) == kPianoDpuFlipShown);
}

static void testCheckSurface()
{
  PianoDpuFlipRequest q = req(nullptr);
  CHECK(checkSurface(q).result == kPianoDpuFlipShown && checkSurface(q).bytes == 2135ull * 12800 + 12800);
  q.rowBytes = 15360;   // the widest row whose 2136 rows still fit a 32 MiB window with the guard
  CHECK(checkSurface(q).result == kPianoDpuFlipShown);
  q.rowBytes = 0xffc0;
  CHECK(checkSurface(q).result == kPianoDpuFlipSize);
  q.size = 8;
  CHECK(checkSurface(q).result == kPianoDpuFlipVersionMismatch);
  CHECK(std::strcmp(pianoDpuFlipResultName(kPianoDpuFlipUnderrun), "underrun") == 0 && std::strcmp(pianoDpuFlipResultName(99), "?") == 0);
}

int main(int argc, char **argv)
{
  (void)argv;
  if (argc > 1) {   // a verbose run of step 4 and a few flips: the log lines as the kernel prints them
    Rig r;
    r.hw.verbose = true;
    r.ready();
    FakeSurface a(0x8d0000000ull, 2136ull * 12800);
    PianoDpuFlipRequest q = req(&a);
    r.f.request(q);
    r.hw.advance(61000000);
    q = req(nullptr);
    r.f.request(q);
    q = req(&a, 12800, 0x52476841);
    r.f.request(q);
    r.f.summary(true);
  }
  testTables();
  testPlan();
  testEnable();
  testBankOn();
  testLadder();
  testService();
  testClean();
  testVariants();
  testSafety();
  testIrq();
  testCheckSurface();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("PianoDpu B1b host tests: all passed\n");
  return 0;
}
