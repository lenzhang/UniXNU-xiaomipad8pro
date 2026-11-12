// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's frame pace (docs/power-irq.md, docs/drivers/touch.md §13). Portable: tests/touch_host.cpp.
//
// How the NT36532E feeds frames, from the first level-2 runs on the tablet (2026-11-11): it raises gpio162 when a frame is ready
// and keeps it pending until the host has read that frame; it scans the next one only then. Edges equal reads, at most ~79 a second
// (a 5.1 ms read and a ~7.5 ms scan), and a latched edge always means a frame that waits. Two ways put it into its watchdog state
// (transport 0xfd: no frames until the download again): a read started before the next frame was ready (the 12 ms edge-less read
// of the previous pace, inside the ~12.6 ms cycle; three downloads in a row failed that way), and a frame left unread for longer
// than ~0.6 s (the display-off pace of 500 ms plus its 250 ms waits). The level-1 loop, which never had one, read on the edge and
// at most 200 ms apart. So:
//   - a latched edge is never dropped (it is the frame that waits); the latch is cleared before each read and again after it;
//   - busy (a contact, a gesture, the decoder learning its reference, watchdog frames) and for kHoldMs after: the latch polled every
//     piano-touch-poll ms, every frame read on its edge; no edge for kNoEdgeBusyMs (piano-touch-noedge, at least kNoEdgeMinMs: well
//     past a cycle, so a frame waits by then): read anyway;
//   - at rest (paced, piano-touch-idle): one frame per idle period: the thread sleeps to the due time, then reads on the latch (set
//     during the sleep) or waits for the edge, at most to the idle period + kNoEdgeRestMs;
//   - the display off (TouchPower.h): one frame per kMutedReadMs, the slowest tier;
//   - never more than kMaxGapMs between two reads (the idle period is at most kIdleReadMaxMs);
//   - not paced (piano-touch-idle=0): every frame on its edge, kNoEdgeOldMs without one (the level-1 loop).
// The watchdog (WatchdogRecovery): the download again at once; when the watchdog comes back within kWatchdogQuietMs of a download,
// again every kWatchdogRetryMs, never given up.
#ifndef PIANO_TOUCH_PACE_H
#define PIANO_TOUCH_PACE_H
#include <stdint.h>

namespace pianotouch {

constexpr uint32_t kIdleReadMs = 50, kIdleReadMinMs = 20, kIdleReadMaxMs = 200, kHoldMs = 1500;
constexpr uint32_t kNoEdgeBusyMs = 50, kNoEdgeMinMs = 25, kNoEdgeRestMs = 50, kNoEdgeOldMs = 200, kMutedReadMs = 200, kMaxGapMs = 250;
constexpr uint32_t kFrameCycleUs = 12600;   // a read (5.1 ms at 9.6 MHz) and the next frame's scan (~7.5 ms), measured: ~79 a second

struct TouchPace {
  uint32_t idleMs;   // 0: every frame (the old loop)
  uint64_t lastBusyMs;
  uint32_t busyNoEdgeMs = kNoEdgeBusyMs;
  static uint32_t clampIdle(uint32_t ms) { return !ms ? 0 : ms < kIdleReadMinMs ? kIdleReadMinMs : ms > kIdleReadMaxMs ? kIdleReadMaxMs : ms; }
  // After each loop pass: whether the loop paces idle frames now (busy: the panel's state after the last decoded frame).
  bool idle(uint64_t nowMs, bool busy)
  {
    if (busy || nowMs < lastBusyMs) lastBusyMs = nowMs;
    return idleMs && nowMs - lastBusyMs >= kHoldMs;
  }
  // At rest: how long to sleep before the next read is due (0: due now: read on the latch or wait for the edge).
  uint32_t sleepMs(uint64_t nowMs, uint64_t lastReadMs) const
  {
    const uint64_t due = lastReadMs + idleMs;
    return nowMs < due ? uint32_t(due - nowMs) : 0;
  }
  // How long after the last read a frame is read without an edge (rest: the pace's state after the last pass).
  uint32_t noEdgeMs(bool rest) const { return !idleMs ? kNoEdgeOldMs : rest ? idleMs + kNoEdgeRestMs : busyNoEdgeMs; }
};
inline TouchPace touchPace(uint32_t idleMs, uint64_t startMs, uint32_t busyNoEdgeMs = kNoEdgeBusyMs)
{
  return TouchPace{TouchPace::clampIdle(idleMs), startMs,
                   busyNoEdgeMs < kNoEdgeMinMs ? kNoEdgeMinMs : busyNoEdgeMs > kNoEdgeOldMs ? kNoEdgeOldMs : busyNoEdgeMs};
}

// The watchdog's download again: at once, unless the last one for it was less than kWatchdogRetryMs ago (then at that time).
// Never given up.
constexpr uint32_t kWatchdogRun = 11, kWatchdogRetryMs = 10000;
struct WatchdogRecovery {
  uint64_t lastMs = 0;    // the last download for the watchdog
  uint32_t downloads = 0;
  // A run of watchdog frames now: whether to download now (false: wait, the loop asks again with the next watchdog frame).
  bool due(uint64_t nowMs) const { return !downloads || nowMs - lastMs >= kWatchdogRetryMs; }
  void downloaded(uint64_t nowMs)
  {
    lastMs = nowMs;
    ++downloads;
  }
};

// After a download and start (the watchdog's, or the display's return when the controller had lost its state): its first frame
// comes about 0.85 s later (the boot's first gpio162 edge came 842 ms after the start, every run), while the busy pace reads without
// an edge 50 ms after the last read. Such a read is one before the frame is ready, which is what sends the controller into its
// watchdog state: desk1 (2026-11-12, nobody touching) went round "download, start, watchdog again ~2.2 s after the download" for
// most of the boot (10314 watchdog blocks among 17296 reads). So after a start nothing is read without an edge (or the line high)
// until the first edge, for at most kStartGraceMs; then the usual rules (a line that never rises is read as before).
constexpr uint32_t kStartGraceMs = 2000;
struct StartGrace {
  bool waiting = false;
  uint64_t sinceMs = 0;
  uint32_t starts = 0, edges = 0, expired = 0;
  void started(uint64_t nowMs)
  {
    waiting = true;
    sinceMs = nowMs;
    ++starts;
  }
  // An edge taken: the first frame after the start is there. Returns true for the first one (for the log).
  bool edge()
  {
    if (!waiting) return false;
    waiting = false;
    ++edges;
    return true;
  }
  // Whether a read still needs an edge or the line's level; the grace ends by itself after kStartGraceMs.
  bool holds(uint64_t nowMs)
  {
    if (waiting && nowMs - sinceMs >= kStartGraceMs) {
      waiting = false;
      ++expired;
    }
    return waiting;
  }
};

// The CPU latency hold of a read burst (piano-touch-cpu-hold; piano_cpuidle_latency_hold, k27 0061, docs/power.md §2): taken at
// the first busy pass, released kHoldMs after the last one, so frames read on every edge while a finger is on the panel are never
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
