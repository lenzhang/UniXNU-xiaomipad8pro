// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's frame pace (docs/power-irq.md). The NT36532E streams a frame about every 8.3 ms whether or not a finger is on the
// panel, raising gpio162 for each; the old loop polled the pin's latch every 1 ms and read every frame: ~1000 wake-ups and ~120
// frame reads a second at rest. Now every frame only while the panel is busy (a contact, the tip debouncing or down, the decoder
// still learning its reference, watchdog frames) and for kHoldMs after; otherwise one frame per idle period (piano-touch-idle):
// the thread sleeps until kLeadMs before the next read is due, drops the latch (its edge came mid-sleep: reading on it would start
// mid-frame), and takes the next fresh edge, by 1 ms polls or, with piano-touch-irq=1, by the TLMM interrupt. While busy it stays
// on 1 ms polls: a CPU woken late from its deepest idle state (Ubuntu measured 4.4 ms to leave it) would start a frame read so
// late that it overlaps the next frame. A finger is seen at most one idle period and two frames (the tip's confirmation) late.
// Without an edge a frame is read anyway after noEdgeMs: the first run on the tablet (2026-11-11, level 1, nobody touching) saw
// about one gpio162 edge in two seconds and every read without one had a good checksum (the old fixed 200 ms made it 5 frames a
// second, and a touch at rest up to 200 ms late). Now, paced: at rest the idle period and kNoEdgeRestMarginMs after the last read;
// once kBlindAfterReads reads in a row came without an edge the rest is blind: the thread sleeps to the due time itself and reads
// (one wake-up a read, ~20 reads a second), until an edge latches during a sleep again. Busy: kNoEdgeBusyMs (piano-touch-noedge:
// about 80 frames a second under a finger even with no edge at all; with edges every frame comes on its edge first). Not paced
// (piano-touch-idle=0, the old loop and the A of the battery A/B): 200 ms as before.
// Portable: tests/touch_host.cpp.
#ifndef PIANO_TOUCH_PACE_H
#define PIANO_TOUCH_PACE_H
#include <stdint.h>

namespace pianotouch {

constexpr uint32_t kIdleReadMs = 50, kIdleReadMinMs = 20, kIdleReadMaxMs = 500, kHoldMs = 1500, kLeadMs = 2;
constexpr uint32_t kNoEdgeBusyMs = 12, kNoEdgeRestMarginMs = 10, kNoEdgeOldMs = 200, kBlindAfterReads = 3;

struct TouchPace {
  uint32_t idleMs;   // 0: every frame (the old loop)
  uint64_t lastBusyMs;
  uint32_t busyNoEdgeMs = kNoEdgeBusyMs;
  uint32_t blindReads = 0;   // reads in a row that no edge started
  bool blind() const { return blindReads >= kBlindAfterReads; }
  // How long after the last read a frame is read without an edge (rest: the pace's state after the last pass).
  uint32_t noEdgeMs(bool rest) const
  {
    return !idleMs ? kNoEdgeOldMs : !rest ? busyNoEdgeMs : blind() ? idleMs : idleMs + kNoEdgeRestMarginMs;
  }
  // Each read: whether an edge started it. An edge latched during an idle sleep: edges come again.
  void read(bool onEdge) { blindReads = onEdge ? 0 : blindReads < 255 ? blindReads + 1 : 255; }
  void edgeSeen() { blindReads = 0; }
  static uint32_t clampIdle(uint32_t ms) { return !ms ? 0 : ms < kIdleReadMinMs ? kIdleReadMinMs : ms > kIdleReadMaxMs ? kIdleReadMaxMs : ms; }
  // After each loop pass: whether the loop paces idle frames now (busy: the panel's state after the last decoded frame).
  bool idle(uint64_t nowMs, bool busy)
  {
    if (busy || nowMs < lastBusyMs) lastBusyMs = nowMs;
    return idleMs && nowMs - lastBusyMs >= kHoldMs;
  }
  // Idle: how long to sleep before waiting for the next read's edge (0: wait for it now); blind, to the due time itself.
  uint32_t sleepMs(uint64_t nowMs, uint64_t lastReadMs) const
  {
    const uint64_t due = lastReadMs + idleMs, lead = blind() ? 0 : kLeadMs;
    return nowMs + lead < due ? uint32_t(due - lead - nowMs) : 0;
  }
};
inline TouchPace touchPace(uint32_t idleMs, uint64_t startMs, uint32_t busyNoEdgeMs = kNoEdgeBusyMs)
{
  return TouchPace{TouchPace::clampIdle(idleMs), startMs, busyNoEdgeMs < 4 ? 4 : busyNoEdgeMs > kNoEdgeOldMs ? kNoEdgeOldMs : busyNoEdgeMs};
}

// The CPU latency hold of a read burst (piano-touch-cpu-hold; piano_cpuidle_latency_hold, k27 0061, docs/power.md §2): taken at
// the first busy pass, released kHoldMs after the last one, so frames read every 8.3 ms while a finger is on the panel are never
// started late by a CPU leaving a deep idle state (Ubuntu: reads 43-51 % slow without its 2000 us hold), and the idle frames
// never hold it.
struct BurstHold {
  uint64_t lastBusyMs = 0;
  bool held = false;
  uint32_t takes = 0;
  // After each pass: 1 take the hold now, -1 release it now, 0 nothing.
  int step(uint64_t nowMs, bool busy)
  {
    if (busy || nowMs < lastBusyMs) lastBusyMs = nowMs;
    const bool want = busy || (held && nowMs - lastBusyMs < kHoldMs);
    if (want == held) return 0;
    held = want;
    if (want) ++takes;
    return want ? 1 : -1;
  }
};

}  // namespace pianotouch
#endif
