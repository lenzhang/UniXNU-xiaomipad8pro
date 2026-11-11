// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1c: 120 <-> 60 Hz by the vertical front porch alone (docs/drivers/dpu.md §4.3), portable (tests/dpu_refresh_host.cpp).
//
// The panel's 120 Hz mode has 2310 lines; its 60 Hz mode is the same line with VFP + 2310 (4620 lines): no DCS command (the panel's
// rate group B2/B3 is the same), no clock, DSC or link change. Two timing generators must agree on the frame length: each DSI
// controller's video engine (TOTAL.V_TOTAL) and each INTF's timing engine (VSYNC_PERIOD_F0 = vtotal * htotal). Both are changed in
// shadow registers inside one frame and take effect together at the next frame start, as the stock immediate-VFP switch does and
// Project-SunUEFI's piano patch does on Linux (read as the specification only: the order and the registers, nothing copied):
//   1. INTF1's line counter inside a window well away from a frame start (lines 200..2000 of 2310 or 4620);
//   2. with nothing in between: on DSI0 (the clock master) then DSI1: timing DB mode = 1, TOTAL = (vtotal - 1) << 16 | (htotal - 1),
//      timing flush = 1; on INTF1 then INTF2: VSYNC_PERIOD_F0 = vtotal * htotal; CTL_INTF_FLUSH = INTF1|INTF2, CTL_FLUSH = bit 31;
//   3. CTL_FLUSH bit 31 reads 0 again (latched at the frame start), then DB mode = 0 on DSI0 then DSI1;
//   4. verified: both periods read back, the refresh measured edge to edge on the frame counters (60.0 or 120.0 Hz within 2 %),
//      INTF2 counting the same frames as INTF1 (the halves in lockstep), no underrun bit (DPU TOP0 26/28, cleared first).
// Any anomaly (a refused store, no window, no latch, a period, rate or lockstep that is wrong, an underrun) switches back the same
// way to the previous total and leaves the policy off for the boot. Programmable fetch must be off (INTF_CONFIG bit 31): with it, the
// fetch start line would have to move with the VFP (SunUEFI keeps it off for the same reason; piano's 106 lines of VSYNC + VBP leave
// the 24-line worst case without it).
//
// Where the DSI timing shadow lives is open (dpu.md §10.5): SunUEFI's patch writes msm-relative 0x1e8 (DB mode) and 0x1e4 (flush),
// i.e. base + 0x1ec / + 0x1e8 (msm's DSI registers are 4 bytes in from the base); downstream Qualcomm's register list as other
// UniXNU ports read it (PeripheralIO.h's TPG FIFO reset at base + 0x1ec) puts DB mode at + 0x1e8 and the flush at + 0x1e4. The
// default is SunUEFI's, the one that switched this panel; piano-dpu-refresh-db=2 selects the other, 0 writes TOTAL directly.
//
// The policy (piano-dpu-refresh=auto): 60 Hz once no burst of swaps came for idleMs, 120 Hz at the next burst (burstSwaps swaps within
// burstWindowMs): a blinking cursor (two swaps a second) is no burst and runs at 60, scrolling or an animation is one. The asymmetry
// is the hysteresis; dwellMs bounds the switch rate whatever the swaps do.
#pragma once
#include "DpuSlices.h"

namespace pianodpu {
namespace refresh {
using namespace slices;

enum class Db : uint8_t { direct = 0, sunuefi = 1, downstream = 2 };
struct DbWords {
  uint32_t mode, flush;
};
constexpr DbWords dbWords(Db d)
{
  return d == Db::sunuefi ? DbWords{kDsiShadow[2], kDsiShadow[1]} : d == Db::downstream ? DbWords{kDsiShadow[1], kDsiShadow[0]} : DbWords{0, 0};
}
inline const char *dbName(Db d) { return d == Db::sunuefi ? "SunUEFI's" : d == Db::downstream ? "downstream's" : "none (direct)"; }

constexpr unsigned kWindowLo = 200, kWindowHi = 2000;
constexpr uint32_t kLatchTimeoutUs = 60000;

struct Options {
  Db db = Db::sunuefi;
  uint32_t measureMs = 250;       // the rate check after a switch
  uint32_t toleranceMilli = 20;   // 2 %
};

// The expected refresh of a total, in thousandths of a hertz (the 120 Hz line time).
constexpr uint64_t expectedMilliHz(unsigned vtotal) { return vtotal ? 120000ull * kVtotal120 / vtotal : 0; }
inline bool rateOk(uint64_t milliHz, unsigned vtotal, uint32_t toleranceMilli)
{
  const uint64_t e = expectedMilliHz(vtotal), tol = e * toleranceMilli / 1000;
  return milliHz + tol >= e && milliHz <= e + tol;
}

// Whether this path takes a VFP switch at all (read only).
inline const char *refuse(const LivePath &p)
{
  if (!p.ok) return p.why;
  const Link &a = p.link[0], &b = p.link[1];
  if ((a.vtotal != kVtotal120 && a.vtotal != kVtotal60) || b.vtotal != a.vtotal)
    return "INTF vtotal is neither 2310 nor 4620 (not the 120 Hz family)";
  if (a.dsiVtotal() != a.vtotal || b.dsiVtotal() != b.vtotal) return "a DSI TOTAL does not match its INTF's vtotal";
  if ((a.dsiTotal & 0xffffu) != (b.dsiTotal & 0xffffu)) return "the two DSI controllers' horizontal totals differ";
  if ((a.config | b.config) & kIntfConfigProgFetch) return "programmable fetch is on (its start line would have to move)";
  return nullptr;
}

struct Switch {
  bool ok = false;           // switched and verified
  bool reverted = false;     // an anomaly: switched back
  bool revertOk = false;     //   and the way back verified
  const char *why = "";      // the anomaly (or the refusal)
  unsigned from = 0, to = 0;
  unsigned refused = 0;      // stores the guard refused
  unsigned stores = 0;
  unsigned tries = 0;        // windows tried
  int32_t latchUs = -1;
  uint32_t period[2] = {0, 0}, dsiTotal[2] = {0, 0}, underrun = 0;
  Rate rate;
};

// Steps 1-3 for one total, no verification. Returns the anomaly or nullptr.
template <class Io>
const char *stage(Io &io, const LivePath &p, unsigned vtotal, const Options &o, Switch &s)
{
  const DbWords db = dbWords(o.db);
  const unsigned from = p.link[0].vtotal ? p.link[0].vtotal : kVtotal120;
  auto wr = [&](uint64_t pa, uint32_t v) {
    ++s.stores;
    if (!io.wr(pa, v)) ++s.refused;
  };
  bool inWindow = false;
  for (s.tries = 1; s.tries <= 3 && !inWindow; ++s.tries) {
    if (!waitLines(io, kWindowLo, kWindowHi, from, 3 * from * kLineNs / 1000)) return "INTF1's line counter never reached the window";
    const uintptr_t irq = io.irqOff();
    const uint32_t line = io.rd(intfReg(1, kIntfLineCount));
    inWindow = line >= kWindowLo && line <= kWindowHi;
    if (inWindow) {
      for (unsigned l = 0; l < 2; ++l) {
        if (db.mode) wr(dsiReg(l, db.mode), 1);
        wr(dsiReg(l, kDsiTotal), (vtotal - 1) << 16 | (p.link[l].dsiTotal & 0xffffu));
        if (db.flush) wr(dsiReg(l, db.flush), 1);
      }
      for (unsigned i = 0; i < 2; ++i) wr(intfReg(i + 1, kIntfVsyncPeriod), vtotal * p.link[i].htotal);
      wr(ctlReg(unsigned(p.ctl), kCtlIntfFlush), kIntfFlushDsi);
      wr(ctlReg(unsigned(p.ctl), kCtlFlush), kFlushIntf);
    }
    io.irqRestore(irq);
  }
  if (!inWindow) return "the window passed three times before the stores";
  s.latchUs = waitFlush(io, unsigned(p.ctl), kFlushIntf, kLatchTimeoutUs);
  if (db.mode)
    for (unsigned l = 0; l < 2; ++l) wr(dsiReg(l, db.mode), 0);   // after the latch (SunUEFI: at the commit's completion)
  if (s.refused) return "the store guard refused a store";
  if (s.latchUs < 0) return "CTL_FLUSH bit 31 did not clear within 60 ms";
  return nullptr;
}

// Step 4.
template <class Io>
const char *verify(Io &io, unsigned vtotal, const LivePath &p, const Options &o, Switch &s)
{
  for (unsigned i = 0; i < 2; ++i) {
    s.period[i] = io.rd(intfReg(i + 1, kIntfVsyncPeriod));
    s.dsiTotal[i] = io.rd(dsiReg(i, kDsiTotal));
  }
  s.rate = measureRate(io, o.measureMs);
  s.underrun = io.rd(kMdp + kTopIntrStatus) & kUnderrunBits;
  for (unsigned i = 0; i < 2; ++i)
    if (s.period[i] != vtotal * p.link[i].htotal) return "an INTF period did not read back";
  if (!s.rate.milliHz) return "the frame counter stopped";
  if (!rateOk(s.rate.milliHz, vtotal, o.toleranceMilli)) return "the measured refresh is not the expected one";
  if (!s.rate.lockstep) return "INTF2 did not count the frames INTF1 counted (the halves are out of step)";
  if (s.underrun) return "an INTF underran";
  return nullptr;
}

// One switch to vtotal (2310 or 4620), verified, and switched back on an anomaly. p is the path as read before (its vtotal the
// current one); on success the caller updates it from the result (to, period).
template <class Io>
Switch switchTo(Io &io, const LivePath &p, unsigned vtotal, const Options &o)
{
  Switch s;
  s.from = p.link[0].vtotal;
  s.to = vtotal;
  if (const char *why = refuse(p)) {
    s.why = why;
    return s;
  }
  if (vtotal != kVtotal120 && vtotal != kVtotal60) {
    s.why = "not a total of the 120 Hz family";
    return s;
  }
  if ((io.rd(kMdp + kTopIntrStatus) & kUnderrunBits) && !io.wr(kMdp + kTopIntrClear, kUnderrunBits)) ++s.refused;
  const char *why = stage(io, p, vtotal, o, s);
  if (!why) why = verify(io, vtotal, p, o, s);
  if (!why) {
    s.ok = true;
    return s;
  }
  s.why = why;
  s.reverted = true;
  Switch back;
  LivePath q = p;   // the path with its stores done: revert from the new total
  q.link[0].vtotal = q.link[1].vtotal = vtotal;
  if (io.rd(kMdp + kTopIntrStatus) & kUnderrunBits) io.wr(kMdp + kTopIntrClear, kUnderrunBits);
  const char *backWhy = stage(io, q, s.from, o, back);
  if (!backWhy) backWhy = verify(io, s.from, p, o, back);
  s.revertOk = !backWhy;
  s.stores += back.stores;
  s.refused += back.refused;
  return s;
}

// ------------------------------------------------------------------------------------------------------------ the policy
struct PolicyConfig {
  uint32_t idleMs = 1000;         // at 120: no burst for this long -> 60
  uint32_t burstSwaps = 2;        // at 60: this many swaps ...
  uint32_t burstWindowMs = 250;   //   ... within this window -> 120 (2 in 250 ms: 8 frames a second or more)
  uint32_t dwellMs = 200;         // never two switches closer than this
};

class Policy {
 public:
  static constexpr unsigned kRing = 8;
  explicit Policy(const PolicyConfig &c = PolicyConfig()) : c_(c)
  {
    if (c_.burstSwaps < 1) c_.burstSwaps = 1;
    if (c_.burstSwaps > kRing) c_.burstSwaps = kRing;
  }
  // A swap shown at now. True when it completes a burst while at 60 Hz (the caller wakes the deciding thread then).
  bool swap(uint64_t nowMs)
  {
    ring_[head_] = nowMs;
    head_ = (head_ + 1) % kRing;
    if (count_ < kRing) ++count_;
    ++swaps_;
    if (count_ >= c_.burstSwaps) {
      const uint64_t first = ring_[(head_ + kRing - c_.burstSwaps) % kRing];
      if (nowMs - first <= c_.burstWindowMs) {
        lastBurst_ = nowMs;
        burstSeen_ = true;
        return hz_ == 60 && enabled_;
      }
    }
    return false;
  }
  struct Decision {
    unsigned hz;        // the rate wanted now
    uint64_t nextMs;    // decide again then (0: only at the next burst)
  };
  Decision decide(uint64_t nowMs) const
  {
    if (!enabled_) return {hz_, 0};
    const uint64_t dwellEnd = lastSwitch_ + c_.dwellMs;
    if (hz_ == 120) {
      const uint64_t idleEnd = (burstSeen_ ? lastBurst_ : lastSwitch_) + c_.idleMs;
      const uint64_t at = idleEnd > dwellEnd ? idleEnd : dwellEnd;
      return nowMs >= at ? Decision{60, 0} : Decision{120, at};
    }
    if (burstSeen_ && lastBurst_ >= lastSwitch_) return nowMs >= dwellEnd ? Decision{120, 0} : Decision{60, dwellEnd};
    return {60, 0};
  }
  // The rate as it is now (a switch done, or the starting rate).
  void applied(unsigned hz, uint64_t nowMs)
  {
    if (hz != hz_) ++switches_;
    hz_ = hz;
    lastSwitch_ = nowMs;
  }
  void disable() { enabled_ = false; }
  bool enabled() const { return enabled_; }
  unsigned hz() const { return hz_; }
  uint64_t swaps() const { return swaps_; }
  uint64_t switches() const { return switches_; }

 private:
  PolicyConfig c_;
  uint64_t ring_[kRing] = {};
  unsigned head_ = 0, count_ = 0;
  uint64_t lastBurst_ = 0, lastSwitch_ = 0, swaps_ = 0, switches_ = 0;
  bool burstSeen_ = false, enabled_ = true;
  unsigned hz_ = 120;
};

}  // namespace refresh
}  // namespace pianodpu
