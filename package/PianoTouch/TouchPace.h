// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's frame pace (docs/drivers/touch.md §13, docs/power-irq.md). Portable: tests/touch_host.cpp runs readOn() against a
// model of the controller.
//
// The rule (2026-11-12, after desk1 and desk-diag): a frame is read only when the NT36532E announced it: a rising edge of gpio162
// latched in TLMM (raw status), or the line still high when its edge was missed. Never by a timer. What the runs show:
//   - the controller announces frames only while it is active (a finger, a start): every frame on its edge, then the next one
//     scanned after the read (~79 a second, edges = reads; attend1's touch session: no read without an edge, no watchdog in 300 s);
//   - op mode 2, which the start sets (the stock driver's "idle"), lets it idle when nobody touches: it then announces a frame
//     about every 1.5 s (desk-diag at rest: 39 edges a minute) while it still rewrites the event buffer by itself;
//   - every watchdog state (transport 0xfd/0xfe; the line then rises all the time, ~150 a second) began while the loop read without
//     announced frames: 12 ms edge-less reads (attend2, the 01:32 run), the 50 ms busy reads once the controller idled ~0.3 s after a
//     download's start (desk-diag: 0xfd 1.8-2.2 s after start, download after download), the 100 ms rest reads (every 50-110 s)
//     and the 200 ms display-off reads (32-39 s); reads of the controller idling also caught half-written frames (bad checksums,
//     isolated 0xfd blocks);
//   - piano's stock driver and its Ubuntu read only in their gpio162 interrupt thread (Ubuntu has no watchdog recovery at all, and
//     it is the touch path of its daily-use releases).
// So:
//   - busy (a contact, a gesture, watchdog frames, the decoder learning until the controller is quiet) and for kHoldMs after: the
//     latch looked at every piano-touch-poll ms, every announced frame read at once;
//   - at rest (paced, piano-touch-idle): the latch looked at once per idle period (or the TLMM interrupt waited for with
//     piano-touch-irq=1): an announced frame waits at most that long; idling, the controller is not read at all;
//   - the display off (TouchPower.h): the latch looked at once per kMutedReadMs;
//   - a latched edge is never dropped (it is the frame that waits); the latch is cleared when taken, and once after each download
//     (the edges before it were the old firmware's);
//   - the controller quiet (Quiet: no edge for kQuietMs): a reference still being learned is formed from its frames so far
//     (TouchMatrix.h matrixSettle);
//   - a contact or a gesture without a frame for kStallMs (an active controller announces one every ~12.6 ms): taken as the lift,
//     what is still going on released (piano-thp releases a stalled stream's contacts after 100 ms);
//   - piano-touch-noedge=MS (25..200; default 0): the reads without an announced frame of before (busy MS after the last read,
//     at rest the idle period + kNoEdgeRestMs, the display off every kMutedReadMs), only for an A/B; gpio162 not mapped (no
//     latch): every kNoEdgeOldMs, as the level-1 loop.
// The watchdog (WatchdogRecovery): the download again at once; when the watchdog comes back within kWatchdogRetryMs of a download,
// again every kWatchdogRetryMs, never given up.
#ifndef PIANO_TOUCH_PACE_H
#define PIANO_TOUCH_PACE_H
#include <stdint.h>

namespace pianotouch {

constexpr uint32_t kIdleReadMs = 50, kIdleReadMinMs = 20, kIdleReadMaxMs = 200, kHoldMs = 1500, kStallMs = 250;
// The controller quiet: no edge for kQuietMs (active it announces a frame every ~12.6 ms, or one scan after each read at the rest
// pace); active again after kActiveEdges edges (an idling one announces a single frame every ~1.5 s).
constexpr uint32_t kQuietMs = 400, kActiveEdges = 3;
struct Quiet {
  bool quiet = false;
  uint32_t edges = 0, quiets = 0;
  uint64_t lastEdgeMs = 0;
  // An edge taken: true when it makes the controller active again (kActiveEdges edges, each within kQuietMs of the one before).
  bool edge(uint64_t nowMs)
  {
    const bool close = nowMs >= lastEdgeMs && nowMs - lastEdgeMs < kQuietMs;
    lastEdgeMs = nowMs;
    if (!quiet) return false;
    edges = close ? edges + 1 : 1;
    if (edges < kActiveEdges) return false;
    quiet = false;
    return true;
  }
  // Each pass: true when the controller has just gone quiet.
  bool step(uint64_t nowMs)
  {
    if (quiet || nowMs < lastEdgeMs || nowMs - lastEdgeMs < kQuietMs) return false;
    quiet = true;
    edges = 0;
    ++quiets;
    return true;
  }
  // A download and start: active until it goes quiet.
  void started(uint64_t nowMs)
  {
    quiet = false;
    edges = 0;
    lastEdgeMs = nowMs;
  }
};
constexpr uint32_t kNoEdgeBusyMs = 50, kNoEdgeMinMs = 25, kNoEdgeRestMs = 50, kNoEdgeOldMs = 200, kMutedReadMs = 200;
// An announced frame waits at most this long for its read (the idle period is at most kIdleReadMaxMs, the display off kMutedReadMs).
constexpr uint32_t kMaxWaitMs = kIdleReadMaxMs > kMutedReadMs ? kIdleReadMaxMs : kMutedReadMs;
constexpr uint32_t kFrameCycleUs = 12600;   // a read (5.1 ms at 9.6 MHz) and the next frame's scan (~7.5 ms), measured: ~79 a second

struct TouchPace {
  uint32_t idleMs;   // 0: every frame (the old loop)
  uint64_t lastBusyMs;
  uint32_t blindMs = 0;   // piano-touch-noedge: 0 never a read without an announced frame; else the busy delay of the old rules
  static uint32_t clampIdle(uint32_t ms) { return !ms ? 0 : ms < kIdleReadMinMs ? kIdleReadMinMs : ms > kIdleReadMaxMs ? kIdleReadMaxMs : ms; }
  static uint32_t clampBlind(uint32_t ms) { return !ms ? 0 : ms < kNoEdgeMinMs ? kNoEdgeMinMs : ms > kNoEdgeOldMs ? kNoEdgeOldMs : ms; }
  // After each loop pass: whether the loop paces idle frames now (busy: the panel's state after the last decoded frame).
  bool idle(uint64_t nowMs, bool busy)
  {
    if (busy || nowMs < lastBusyMs) lastBusyMs = nowMs;
    return idleMs && nowMs - lastBusyMs >= kHoldMs;
  }
  // At rest: how long to sleep before the latch is looked at (0: the idle period is over: look now).
  uint32_t sleepMs(uint64_t nowMs, uint64_t lastReadMs) const
  {
    const uint64_t due = lastReadMs + idleMs;
    return nowMs < due ? uint32_t(due - nowMs) : 0;
  }
  // The old rules (blindMs): how long after the last read a frame was read without an edge.
  uint32_t noEdgeMs(bool rest) const { return !idleMs ? kNoEdgeOldMs : rest ? idleMs + kNoEdgeRestMs : blindMs; }
};
inline TouchPace touchPace(uint32_t idleMs, uint64_t startMs, uint32_t blindMs = 0)
{
  return TouchPace{TouchPace::clampIdle(idleMs), startMs, TouchPace::clampBlind(blindMs)};
}

// What a frame read starts on (none: not now).
enum class ReadOn : uint8_t { none, edge, level, blind };
struct ReadCue {
  bool edge;          // a latched rising edge not read yet: a frame the controller announced
  bool level;         // no edge, but the line high kNoEdgeMinMs or more after the last read started (its edge was missed)
  bool latch;         // gpio162's latch works; false: nothing announces a frame, every read is blind (every kNoEdgeOldMs)
  bool muted;         // the display off
  bool rest;          // the pace's state after the last pass
  bool grace;         // StartGrace holds: after a start no read without an edge, even under the old rules
  uint64_t sinceMs;   // since the last read started
};
inline ReadOn readOn(const TouchPace &p, const ReadCue &c)
{
  if (!c.latch) return c.sinceMs >= (c.muted ? kMutedReadMs : kNoEdgeOldMs) ? ReadOn::blind : ReadOn::none;
  if (c.sinceMs < (c.muted ? kMutedReadMs : c.rest ? p.idleMs : 0)) return ReadOn::none;
  if (c.edge) return ReadOn::edge;
  if (c.level) return ReadOn::level;
  if (!p.blindMs || c.grace) return ReadOn::none;
  return c.sinceMs >= (c.muted ? kMutedReadMs : p.noEdgeMs(c.rest)) ? ReadOn::blind : ReadOn::none;
}
inline const char *readOnName(ReadOn r)
{
  return r == ReadOn::edge ? "on an edge" : r == ReadOn::level ? "on the line still high" : r == ReadOn::blind ? "without either" : "none";
}

// A contact or a gesture that got no frame for kStallMs: released (frames lost).
inline bool stalled(bool live, uint64_t nowMs, uint64_t lastReadMs) { return live && nowMs >= lastReadMs && nowMs - lastReadMs >= kStallMs; }

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

// After a download and start (the watchdog's, or the display's return when the controller had lost its state): the first edge after
// it, for the log ("frames: the first edge N ms after start S"), and under the old rules (piano-touch-noedge) no read without an
// edge until then, for at most kStartGraceMs (desk1's fix of 2026-11-12, which did not stop the loop: desk-diag). Under the
// default rules every read needs an announced frame anyway.
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
