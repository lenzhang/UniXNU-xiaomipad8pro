// SPDX-License-Identifier: BSD-2-Clause-Patent
// The console's reader of the kernel log ring (PianoDwc3's port 0, docs/drivers/usb-device-debug.md section 18). The ring's header
// says only where the writer is now: start = total % cap and size = min(total, cap) (k27 0055/0058 pstore-console), or the message
// buffer's write index. The reader keeps absolute positions over it.
//
// Why (desk1, 2026-11-12): 0055's writer is lock-free. Each console_write reserves its bytes with an atomic add, copies them, then
// stores the header from its own end. So a writer that reserved first and finished last moves the header back by up to one write
// (two CPUs at once, or an interrupt inside a write). The old reader took a step back of b bytes for a step forward of cap - b. It
// re-read a whole lap: this boot's start, preceded by the decayed bytes of the region not yet written. Its clamp at cap then left
// its read position a fraction of a ring behind the writer for good. A GPU desktop run streamed the boot's first 5-10 k lines 12
// times, then old lines at the log's own rate, and never the live tail.
//
// Here:
// - The writer's position is absolute and only moves forward: a step back of at most backLimit() bytes is a late writer, ignored.
// - A byte is sent only once it has been in the header for kSettleUs, by which time a late writer's copy is done.
// - A reader overtaken by the writer skips to the oldest retained byte (with a margin from the writer) and counts what was lost.
// - The port's first open of the boot replays the retained log (or starts from now). A later open resumes where the last session
//   stopped: the bytes still queued or in flight at its close are sent again, so a few lines may repeat but none is skipped.
// The assumption: between two syncs the writer moves less than cap - backLimit() (piano: 600 KiB). The USB thread syncs every
// pass, and once a second through piano-usb-delay. A larger move reads as a step back until the writer has gone past the furthest
// position again. Positions stay right (they are taken mod cap); only that lap is missing from `lost`.
// Portable: PianoDwc3.cpp holds one, tests/log_cursor_host.cpp drives it against a model of the racing writers.
#pragma once
#include <stdint.h>

namespace pianousb {

struct LogCursor {
  static constexpr uint32_t kMaxBack = 64 * 1024;   // a late writer's step back: one console_write at most (a line; 32 KiB once)
  static constexpr uint64_t kSettleUs = 1000;       // a byte is sent this long after the header first covered it
  enum class Start : uint8_t { replay, now, resume };

  uint32_t cap = 0;            // the ring's bytes (0: no ring)
  uint32_t base = 0;           // the ring index of absolute position 0
  uint32_t lastStart = 0;      // the header's write index as last taken
  uint64_t written = 0;        // the writer, absolute (0 = the oldest byte retained at init)
  uint64_t settled = 0;        // what may be sent: written as it was kSettleUs ago
  uint64_t pending = 0, pendingAtUs = 0;
  uint64_t sent = 0;           // the reader, absolute
  uint64_t resumeAt = 0;       // where the next session starts (set at a close)
  bool opened = false;         // a session was opened this boot
  uint64_t lost = 0;           // bytes overwritten before they were sent
  uint32_t backSteps = 0, laps = 0, sessions = 0;

  uint32_t backLimit() const { return cap / 4 < kMaxBack ? cap / 4 : kMaxBack; }
  uint32_t margin() const { return cap / 8 < 16384 ? cap / 8 : 16384; }   // kept from the writer when skipping to the oldest byte
  uint32_t index(uint64_t at) const { return uint32_t((base + at) % cap); }
  // The oldest byte that can be read safely now: a full ring behind the writer, less the margin.
  uint64_t oldest() const { return written + margin() > cap ? written + margin() - cap : 0; }

  // The header as the ring is mapped: start (the write index) and size (the bytes retained).
  void init(uint32_t capacity, uint32_t start, uint32_t size, uint64_t nowUs)
  {
    *this = LogCursor();
    cap = capacity;
    if (!cap) return;
    if (size > cap) size = cap;
    start %= cap;
    base = (start + cap - size) % cap;
    lastStart = start;
    written = settled = pending = size;
    pendingAtUs = nowUs;
  }

  // The header now. Forward moves count; a move back of at most backLimit() is a late writer's (ignored: the furthest stays).
  void sync(uint32_t start, uint64_t nowUs)
  {
    if (!cap) return;
    start %= cap;
    const uint32_t ahead = (start + cap - lastStart) % cap;
    if (ahead && ahead <= cap - backLimit()) {
      written += ahead;
      lastStart = start;
    } else if (ahead) {
      ++backSteps;
    }
    if (nowUs - pendingAtUs >= kSettleUs) {
      settled = pending;
      pending = written;
      pendingAtUs = nowUs;
    }
  }

  // Bytes that may be sent now, from index(sent). A reader the writer has overtaken first skips to oldest() (counted in lost).
  uint32_t readable()
  {
    if (!cap) return 0;
    const uint64_t floor = oldest();
    if (sent < floor) {
      lost += floor - sent;
      ++laps;
      sent = floor;
    }
    const uint64_t n = settled > sent ? settled - sent : 0;
    return n > cap ? cap : uint32_t(n);
  }
  void consume(uint32_t n) { sent += n; }

  // The console port opened. The boot's first session replays the retained log (replay) or starts from now; a later one resumes,
  // unless everyOpen (piano-usb-replay=2: each open replays the retained log, as before 2026-11-12).
  Start open(bool replay, bool everyOpen = false)
  {
    Start how;
    if (!opened || (replay && everyOpen)) {
      how = replay ? Start::replay : Start::now;
      sent = replay ? oldest() : written;   // what the ring no longer holds is not this session's loss
    } else {
      how = Start::resume;
      sent = resumeAt;
    }
    opened = true;
    ++sessions;
    return how;
  }
  // The console port closed (the host closed it, or a bus reset). unsent: the bytes that had not reached the host (still queued for
  // the port, or in a bulk IN transfer that did not complete); the next session sends them again.
  void close(uint64_t unsent) { resumeAt = sent > unsent ? sent - unsent : 0; }
  // The monitor: `log` replays everything retained, `live on` goes on from now.
  void rewind(bool replay) { sent = replay ? oldest() : written; }
  // Bytes not sent yet (the banner's number); unsettled: written but not yet readable (the thread should come back soon).
  uint64_t behind() const { return written > sent ? written - sent : 0; }
  bool unsettled() const { return written > settled && settled <= sent; }
};

}  // namespace pianousb
