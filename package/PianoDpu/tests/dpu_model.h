// SPDX-License-Identifier: BSD-2-Clause-Patent
// A timed register model of piano's panel path for the B1c/B1d/B1e host tests (dpu_refresh_host.cpp, dpu_pcc_host.cpp,
// dpu_panel_host.cpp): the Io of DpuSlices.h over simulated time. What it models, from the facts in DpuHw.h / DpuSlices.h:
//   - INTF1/INTF2 timing engines: frames of vtotal lines at the 120 Hz family's line time, frame and line counters, INTF_STATUS;
//     EN = 0 stops an engine at its frame end, EN = 1 starts it at once; VSYNC_PERIOD_F0 is a shadow taken at a CTL INTF flush;
//   - the CTL: FLUSH bits set by writing ones, latched (and cleared) at INTF1's frame start, or at the engines' start when stopped;
//   - the DSPPs' PCC words: shadows taken at a CTL DSPP flush (CTL_DSPP_n_FLUSH bit 4);
//   - each DSI controller's video TOTAL with its timing shadow (DB mode, flush) at one of the two candidate places (DbHw), latched at
//     its INTF's frame start; a frame whose DSI and INTF totals differ, or a TOTAL that changes inside a frame, is a glitch and sets
//     the INTF's underrun bit in DPU TOP0 (cleared by INTR_CLEAR);
//   - each DSI controller's command DMA from its FIFO: TRIG_DMA sends the FIFO's packet in the next vertical blanking (or at once when
//     the engine is stopped), sets INTR_CTRL bit 0; the panel halves follow the DCS commands they receive;
//   - the store guard: a store DpuSlices.h does not allow is refused and recorded.
// Fault injection: a DMA that never finishes, an engine that does not stop or start, a flush that never latches, counters off.
#pragma once
#include "DpuSlices.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace model {
using namespace pianodpu;
using namespace pianodpu::slices;

enum class DbHw : uint8_t { sunuefi, downstream };   // where the hardware's DSI timing DB mode / flush really are
constexpr uint32_t kHtotal = 576;

struct Store {
  uint64_t ns, pa;
  uint32_t v;
  uint32_t frame, line;   // INTF1's at the store
};
struct Dcs {
  uint64_t ns;
  unsigned link;
  uint8_t command;
  bool lowPower;
};

struct Dpu {
  // ---- configuration (set before use)
  DbHw dbHw = DbHw::sunuefi;
  bool blockDma = true;                 // TRIG_CTRL bit 12 as the boot stages left it
  bool dmaHang[2] = {false, false};     // a link's command DMA never finishes
  int dmaBudget[2] = {-1, -1};          // >= 0: this many more transfers finish, then the link hangs
  bool engineStuck[2] = {false, false}; // EN = 0 does not stop INTF1 / INTF2
  bool engineDead[2] = {false, false};  // EN = 1 does not start it
  bool flushStuck = false;              // CTL_FLUSH never latches
  // ---- state
  uint64_t ns = 0;
  std::map<uint64_t, uint32_t> regs;
  std::vector<Store> stores;
  std::vector<uint64_t> refused;
  std::vector<Dcs> dcs;
  unsigned glitches = 0, midFrameTotal = 0, commandsInFrame = 0, desync = 0, irqDepth = 0;
  struct Intf {
    bool on = true, stopPending = false, counting = true;
    uint64_t start = 0;
    uint32_t frames = 1000, period = 2310 * kHtotal, shadow = 2310 * kHtotal, cur = 2310 * kHtotal;
  } intf[3];
  struct Dsi {
    uint32_t total = 2309u << 16 | 575, shadow = 2309u << 16 | 575;
    bool db = false, armed = false;
    std::vector<uint32_t> fifo;
    bool dmaPending = false;
    uint64_t dmaAt = 0;
    uint32_t intr = 0x00000002;   // the DMA done enable bit on, status clear
  } dsi[2];
  uint32_t ctlFlush = 0, ctlIntfFlush = 0, dsppFlush[4] = {0, 0, 0, 0};
  uint32_t pccShadow[4][13] = {}, pccActive[4][13] = {};
  uint32_t topStatus = 0;
  struct Half {
    bool displayOn = true, sleeping = false;
  } half[2];
  uint64_t startNs[3] = {0, 0, 0};

  Dpu()
  {
    regs[kGcc + kGccDispAhb] = 1;
    regs[kDispCc + kCcMdssGdsc] = 0x80000000u;
    regs[kDispCc + kCcAhb] = 1;
    regs[kDispCc + kCcMdp] = 1;
    regs[ctlReg(0, kCtlTop)] = 0xf0000000u;
    regs[ctlReg(0, kCtlIntfActive)] = 0x6;
    regs[ctlReg(0, kCtlIntfMaster)] = 0x2;
    regs[ctlReg(0, kCtlLayerActive)] = 0x3;
    for (unsigned i = 1; i <= 2; ++i) {
      regs[intfReg(i, kIntfEn)] = 1;
      regs[intfReg(i, kIntfHsyncCtl)] = kHtotal << 16 | 16;
      regs[intfReg(i, kIntfCountEn)] = 3;
      regs[intfReg(i, kIntfConfig2)] = 0x1011;
    }
    for (unsigned l = 0; l < 2; ++l) {
      regs[dsiReg(l, kDsiCtrl)] = 0x1f3;
      regs[dsiReg(l, kDsiCmdDmaCtrl)] = kDmaLowPowerFact | 0x10000000u;
      regs[dsiReg(l, kDsiDmaCtrl1)] = 0x2;
      regs[dsiReg(l, kDsiDmaLength)] = 0;
    }
  }
  static constexpr uint32_t kDmaLowPowerFact = 1u << 26;

  // ---- time
  uint64_t frameNs(unsigned i) const { return uint64_t(intf[i].cur / kHtotal) * kLineNs; }
  uint32_t vtotal(unsigned i) const { return intf[i].cur / kHtotal; }
  uint32_t line(unsigned i) const { return intf[i].on ? uint32_t((ns - intf[i].start) / kLineNs) : 0; }
  void latchCtl()
  {
    if (!ctlFlush || flushStuck) return;
    if (ctlFlush & kFlushIntf)
      for (unsigned i = 1; i <= 2; ++i)
        if ((ctlIntfFlush >> i) & 1u) intf[i].period = intf[i].shadow;
    if (ctlFlush & kFlushDspp)
      for (unsigned d = 0; d < 4; ++d)
        if (dsppFlush[d] & kDsppFlushPcc)
          for (unsigned k = 0; k < 13; ++k) pccActive[d][k] = pccShadow[d][k];
    ctlFlush = ctlIntfFlush = 0;
    for (unsigned &f : dsppFlush) f = 0;
  }
  void checkFrame(unsigned i)
  {
    const Dsi &d = dsi[i - 1];
    if ((d.total >> 16) + 1 != vtotal(i)) {
      ++glitches;
      topStatus |= 1u << kIntrIntfUnderrun[i];
    }
  }
  void advanceTo(uint64_t t)
  {
    for (;;) {
      uint64_t next = UINT64_MAX;
      for (unsigned i = 1; i <= 2; ++i)
        if (intf[i].on && intf[i].start + frameNs(i) < next) next = intf[i].start + frameNs(i);
      for (unsigned l = 0; l < 2; ++l)
        if (dsi[l].dmaPending && dsi[l].dmaAt < next) next = dsi[l].dmaAt;
      if (next > t) break;
      ns = next;
      for (unsigned l = 0; l < 2; ++l)
        if (dsi[l].dmaPending && dsi[l].dmaAt == next) finishDma(l);
      bool started[3] = {false, false, false};
      for (unsigned i = 1; i <= 2; ++i) {
        if (!intf[i].on || intf[i].start + frameNs(i) != next) continue;
        if (intf[i].stopPending) {
          intf[i].on = false;
          intf[i].stopPending = false;
          continue;
        }
        if (intf[i].counting) ++intf[i].frames;
        intf[i].start = next;
        started[i] = true;
      }
      if (started[1]) latchCtl();
      for (unsigned i = 1; i <= 2; ++i) {
        if (!started[i]) continue;
        intf[i].cur = intf[i].period;
        Dsi &d = dsi[i - 1];
        if (d.armed) {
          d.total = d.shadow;
          d.armed = false;
        }
        checkFrame(i);
      }
    }
    ns = t;
  }
  void tick(uint64_t dns) { advanceTo(ns + dns); }

  // ---- the DSI command DMA
  void finishDma(unsigned l)
  {
    Dsi &d = dsi[l];
    d.dmaPending = false;
    if (!d.fifo.empty()) {
      const uint32_t w = d.fifo[0];
      const uint8_t type = uint8_t((w >> 16) & 0x3fu), command = uint8_t(w & 0xffu);
      if (type == 0x05) {
        dcs.push_back({ns, l, command, (regs[dsiReg(l, kDsiCmdDmaCtrl)] & (1u << 26)) != 0});
        Half &h = half[l];
        if (command == 0x28) h.displayOn = false;
        if (command == 0x29) h.displayOn = true;
        if (command == 0x10) h.sleeping = true;
        if (command == 0x11) h.sleeping = false;
      }
    }
    d.fifo.clear();
    d.intr |= 1u;
  }
  void trigger(unsigned l)
  {
    Dsi &d = dsi[l];
    const uint32_t ctrl = regs[dsiReg(l, kDsiCtrl)], tpg = regs[dsiReg(l, kDsiTpgCtrl)], dma = regs[dsiReg(l, kDsiCmdDmaCtrl)];
    if (!(ctrl & 1u) || !(ctrl & 4u) || tpg != 0x30006u || !(dma & (1u << 28)) || !regs[dsiReg(l, kDsiDmaLength)] || d.fifo.empty())
      return;   // not a FIFO transfer the engine takes: nothing happens
    if (dmaBudget[l] == 0) dmaHang[l] = true;
    if (dmaBudget[l] > 0) --dmaBudget[l];
    if (dmaHang[l]) {   // the engine took it and never finishes: busy for good
      d.dmaPending = true;
      d.dmaAt = UINT64_MAX;
      return;
    }
    const unsigned i = l + 1;
    d.dmaPending = true;
    if (!intf[i].on) {
      d.dmaAt = ns + 20000;
      return;
    }
    const uint32_t ln = line(i);
    if (blockDma) {
      d.dmaAt = ln >= kVActiveEnd ? ns + 20000 : intf[i].start + uint64_t(kVActiveEnd + 1) * kLineNs;
    } else {
      if (ln < kVActiveEnd) ++commandsInFrame;   // a command inside the active frame: the picture tears
      d.dmaAt = ns + 20000;
    }
  }

  // ---- the Io
  uint32_t rd(uint64_t pa)
  {
    tick(150);
    for (unsigned i = 1; i <= 2; ++i) {
      if (pa == intfReg(i, kIntfStatus)) return intf[i].on ? 1u : 0u;
      if (pa == intfReg(i, kIntfFrameCount)) return intf[i].frames;
      if (pa == intfReg(i, kIntfLineCount)) return intf[i].counting ? line(i) : 0;
      if (pa == intfReg(i, kIntfVsyncPeriod)) return intf[i].shadow;
      if (pa == intfReg(i, kIntfEn)) return regs[pa];
    }
    if (pa == ctlReg(0, kCtlFlush)) return ctlFlush;
    if (pa == ctlReg(0, kCtlIntfFlush)) return ctlIntfFlush;
    for (unsigned d = 0; d < 4; ++d) {
      if (pa == ctlReg(0, kCtlDspp0Flush + 4 * d)) return dsppFlush[d];
      if (pa >= pccReg(d, 0) && pa <= pccReg(d, 0x30)) return pccShadow[d][(pa - pccReg(d, 0)) / 4];
    }
    if (pa == kMdp + kTopIntrStatus) return topStatus;
    for (unsigned l = 0; l < 2; ++l) {
      if (pa == dsiReg(l, kDsiTotal)) return dsi[l].db ? dsi[l].shadow : dsi[l].total;
      if (pa == dsiReg(l, kDsiIntrCtrl)) return dsi[l].intr;
      if (pa == dsiReg(l, kDsiStatus0)) return dsi[l].dmaPending ? 0x2u | 0x8u : 0x8u;
      if (pa == dsiReg(l, kDsiTrigCtrl)) return 4u | (blockDma ? 1u << 12 : 0u);
      if (pa == dsiReg(l, dbModeWord())) return dsi[l].db ? 1u : 0u;
    }
    auto it = regs.find(pa);
    return it == regs.end() ? 0 : it->second;
  }
  uint32_t dbModeWord() const { return dbHw == DbHw::sunuefi ? kDsiShadow[2] : kDsiShadow[1]; }
  uint32_t dbFlushWord() const { return dbHw == DbHw::sunuefi ? kDsiShadow[1] : kDsiShadow[0]; }
  bool wr(uint64_t pa, uint32_t v)
  {
    tick(150);
    if (!storeAllowed(pa, v)) {
      refused.push_back(pa);
      return false;
    }
    stores.push_back({ns, pa, v, intf[1].frames, line(1)});
    for (unsigned i = 1; i <= 2; ++i) {
      if (pa == intfReg(i, kIntfVsyncPeriod)) {
        intf[i].shadow = v;
        return true;
      }
      if (pa == intfReg(i, kIntfCountEn)) {
        regs[pa] = v;
        intf[i].counting = (v & 3u) == 3u;
        return true;
      }
      if (pa == intfReg(i, kIntfEn)) {
        regs[pa] = v;
        if (!v && intf[i].on && !engineStuck[i - 1]) intf[i].stopPending = true;
        if (v && !intf[i].on && !engineDead[i - 1]) {
          intf[i].on = true;
          intf[i].stopPending = false;
          intf[i].start = ns;
          startNs[i] = ns;
          const unsigned other = 3 - i;
          if (intf[other].on) {   // started beside a running engine: in phase with its frame start, or the halves tear
            const uint64_t phase = (ns - intf[other].start) % frameNs(other);
            if (phase > 4 * kLineNs && phase + 4 * kLineNs < frameNs(other)) ++desync;
            if (i == 2 && ns - intf[1].start < 4 * kLineNs) {   // the pair started together: one frame start, one latch
              latchCtl();
              for (unsigned k = 1; k <= 2; ++k) intf[k].cur = intf[k].period;
              intf[2].start = intf[1].start;
            }
          }
        }
        if (v) intf[i].stopPending = false;
        return true;
      }
    }
    if (pa == ctlReg(0, kCtlFlush)) {
      ctlFlush |= v;
      return true;
    }
    if (pa == ctlReg(0, kCtlIntfFlush)) {
      ctlIntfFlush = v;
      return true;
    }
    for (unsigned d = 0; d < 4; ++d) {
      if (pa == ctlReg(0, kCtlDspp0Flush + 4 * d)) {
        dsppFlush[d] = v;
        return true;
      }
      if (pa >= pccReg(d, 0) && pa <= pccReg(d, 0x30)) {
        pccShadow[d][(pa - pccReg(d, 0)) / 4] = v;
        return true;
      }
    }
    if (pa == kMdp + kTopIntrClear) {
      topStatus &= ~v;
      return true;
    }
    for (unsigned l = 0; l < 2; ++l) {
      Dsi &d = dsi[l];
      const unsigned i = l + 1;
      if (pa == dsiReg(l, kDsiTotal)) {
        if (d.db) {
          d.shadow = v;
        } else {
          if ((v >> 16) != (d.total >> 16) && intf[i].on) {   // the video engine's frame changes length mid-frame
            ++midFrameTotal;
            topStatus |= 1u << kIntrIntfUnderrun[i];
          }
          d.total = d.shadow = v;
        }
        return true;
      }
      if (pa == dsiReg(l, dbModeWord())) {
        if (!v && d.armed) ++glitches;   // DB mode dropped before the latch
        d.db = v & 1u;
        return true;
      }
      if (pa == dsiReg(l, dbFlushWord())) {
        if (v & 1u && d.db) d.armed = true;
        return true;
      }
      if (pa == dsiReg(l, kDsiIntrCtrl)) {
        d.intr = (d.intr & ~(v & 1u)) | (v & ~1u);
        return true;
      }
      if (pa == dsiReg(l, kDsiTpgFifo)) {
        if (regs[dsiReg(l, kDsiTpgCtrl)] == 0x30006u) d.fifo.push_back(v);
        return true;
      }
      if (pa == dsiReg(l, kDsiTrigDma)) {
        if (v & 1u) trigger(l);
        return true;
      }
      if (dbHw == DbHw::downstream && pa == dsiReg(l, kDsiShadow[2])) {   // the TPG FIFO reset there
        if (v & 1u) d.fifo.clear();
        return true;
      }
    }
    regs[pa] = v;
    return true;
  }
  void delayUs(uint32_t us) { tick(uint64_t(us) * 1000); }
  void sleepMs(uint32_t ms) { tick(uint64_t(ms) * 1000000); }
  uint64_t nowUs() { return ns / 1000; }
  uintptr_t irqOff() { return irqDepth++; }
  void irqRestore(uintptr_t) { --irqDepth; }

  // ---- helpers for the tests
  unsigned storesTo(uint64_t pa) const
  {
    unsigned n = 0;
    for (const Store &s : stores) n += s.pa == pa;
    return n;
  }
  int firstStore(uint64_t pa, uint32_t v) const
  {
    for (size_t k = 0; k < stores.size(); ++k)
      if (stores[k].pa == pa && stores[k].v == v) return int(k);
    return -1;
  }
  const Dcs *firstDcs(uint8_t command, unsigned link) const
  {
    for (const Dcs &d : dcs)
      if (d.command == command && d.link == link) return &d;
    return nullptr;
  }
  bool anyStoreIn(uint64_t base, uint64_t bytes) const
  {
    for (const Store &s : stores)
      if (s.pa >= base && s.pa < base + bytes) return true;
    return false;
  }
};

}  // namespace model
