// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's frame pace (docs/power-irq.md). The NT36532E streams a frame about every 8.3 ms whether or not a finger is on the
// panel, raising gpio162 for each; the old loop polled the pin's latch every 1 ms and read every frame: ~1000 wake-ups and ~120
// frame reads a second at rest. Now every frame only while the panel is busy (a contact, the tip debouncing or down, the decoder
// still learning its reference, watchdog frames) and for kHoldMs after; otherwise one frame per idle period (piano-touch-idle):
// the thread sleeps until kLeadMs before the next read is due, drops the latch (its edge came mid-sleep: reading on it would start
// mid-frame), and takes the next fresh edge, by 1 ms polls or, with piano-touch-irq=1, by the TLMM interrupt. While busy it stays
// on 1 ms polls: a CPU woken late from its deepest idle state (Ubuntu measured 4.4 ms to leave it) would start a frame read so
// late that it overlaps the next frame. A finger is seen at most one idle period and two frames (the tip's confirmation) late.
// Portable: tests/touch_host.cpp.
#ifndef PIANO_TOUCH_PACE_H
#define PIANO_TOUCH_PACE_H
#include <stdint.h>

namespace pianotouch {

constexpr uint32_t kIdleReadMs = 50, kIdleReadMinMs = 20, kIdleReadMaxMs = 500, kHoldMs = 1500, kLeadMs = 2;

struct TouchPace {
  uint32_t idleMs;   // 0: every frame (the old loop)
  uint64_t lastBusyMs;
  static uint32_t clampIdle(uint32_t ms) { return !ms ? 0 : ms < kIdleReadMinMs ? kIdleReadMinMs : ms > kIdleReadMaxMs ? kIdleReadMaxMs : ms; }
  // After each loop pass: whether the loop paces idle frames now (busy: the panel's state after the last decoded frame).
  bool idle(uint64_t nowMs, bool busy)
  {
    if (busy || nowMs < lastBusyMs) lastBusyMs = nowMs;
    return idleMs && nowMs - lastBusyMs >= kHoldMs;
  }
  // Idle: how long to sleep before waiting for the next read's edge (0: wait for it now).
  uint32_t sleepMs(uint64_t nowMs, uint64_t lastReadMs) const
  {
    const uint64_t due = lastReadMs + idleMs;
    return nowMs + kLeadMs < due ? uint32_t(due - kLeadMs - nowMs) : 0;
  }
};
inline TouchPace touchPace(uint32_t idleMs, uint64_t startMs) { return TouchPace{TouchPace::clampIdle(idleMs), startMs}; }

}  // namespace pianotouch
#endif
