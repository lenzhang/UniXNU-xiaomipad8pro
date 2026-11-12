// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of LogCursor.h, the console's reader of the kernel log ring, against a model of k27 0055's lock-free writers. Each
// write reserves its bytes with an atomic add, copies them, then stores the header (start = end % cap, size = min(end, cap)) from
// its own end, so writers that finish out of order move the header back.
//   the race: with writers finishing out of order, the host gets the log once, in order, with no lap and no garbage. The old
//             reader (replicated below) re-reads a whole lap on the first step back, and after a clamp stays behind for good:
//             desk1's symptom (2026-11-12), reproduced.
//   the settle: bytes are sent only kSettleUs after the header covered them (a late writer's copy is done by then).
//   overrun: a reader stalled for more than a ring skips to the oldest retained byte, counts the loss, and goes on in order.
//   sessions: the first open replays the retained log (or starts from now), a close remembers what had not reached the host, a
//             reopen resumes there (once), the monitor's `log` and `live on`.
//   small rings: the step-back limit scales with the ring.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. log_cursor_host.cpp -o /tmp/log_cursor_host && /tmp/log_cursor_host
#include "LogCursor.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pianousb;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The ring and its writers (0055/0058): bytes of absolute position i are byteAt(i); the unwritten part holds 0xee (the previous
// boot's decayed bytes).
struct Ring {
  uint32_t cap;
  std::vector<uint8_t> data;
  uint64_t total = 0;              // the writers' atomic counter
  uint32_t start = 0, size = 0;    // the header
  explicit Ring(uint32_t c) : cap(c), data(c, 0xee) {}
  static uint8_t byteAt(uint64_t i) { return uint8_t('a' + (i * 7 + i / 251) % 26); }
  struct Write { uint64_t at; uint32_t len; };
  Write reserve(uint32_t len)
  {
    const Write w{total, len};
    total += len;
    return w;
  }
  void copy(const Write &w)
  {
    for (uint32_t i = 0; i < w.len; ++i) data[(w.at + i) % cap] = byteAt(w.at + i);
  }
  void header(const Write &w)
  {
    const uint64_t end = w.at + w.len;
    start = uint32_t(end % cap);
    size = uint32_t(end < cap ? end : cap);
  }
  void write(uint32_t len)   // one writer alone
  {
    const Write w = reserve(len);
    copy(w);
    header(w);
  }
};

// The host side: what the reader sends, in order.
struct Host {
  std::vector<uint8_t> got;
  void pump(LogCursor &c, const Ring &r, uint32_t room = 1u << 30)
  {
    for (uint32_t n = c.readable(); n && room; n = c.readable()) {
      const uint32_t take = n < room ? n : room;
      for (uint32_t i = 0; i < take; ++i) got.push_back(r.data[c.index(c.sent + i)]);
      c.consume(take);
      room -= take;
    }
  }
  // got == the log from absolute position from on, without a gap (expected bytes for each)
  bool inOrderFrom(uint64_t from) const
  {
    for (size_t i = 0; i < got.size(); ++i)
      if (got[i] != Ring::byteAt(from + i)) return false;
    return true;
  }
};

// The reader before 2026-11-12 (PianoDwc3.cpp's logRewind/logPump), on the same ring.
struct OldReader {
  uint32_t cap, lastStart = 0, rdPos = 0, avail = 0;
  explicit OldReader(uint32_t c) : cap(c) {}
  void rewind(const Ring &r)
  {
    lastStart = r.start;
    avail = r.size;
    rdPos = (r.start + cap - avail) % cap;
  }
  void pump(const Ring &r, std::vector<uint8_t> &out, uint32_t room = ~0u)
  {
    const uint32_t delta = (r.start + cap - lastStart) % cap;
    lastStart = r.start;
    avail = avail + delta > cap ? cap : avail + delta;
    while (avail && room) {
      out.push_back(r.data[rdPos]);
      rdPos = (rdPos + 1) % cap;
      --avail;
      --room;
    }
  }
};

static void testRace()
{
  const uint32_t cap = 8192;
  {   // the old reader: one late writer, and the host gets a lap of garbage and the boot's start again
    Ring r(cap);
    OldReader old(cap);
    std::vector<uint8_t> out;
    r.write(1000);
    old.rewind(r);
    old.pump(r, out);
    const Ring::Write a = r.reserve(80), b = r.reserve(120);
    r.copy(b);
    r.header(b);
    old.pump(r, out);   // sees b's end
    r.copy(a);
    r.header(a);        // the header steps back to a's end
    old.pump(r, out);
    size_t garbage = 0;
    for (uint8_t c : out) garbage += c == 0xee;
    CHECK(out.size() > cap && garbage > 0);   // a whole lap re-read, through the unwritten region
  }
  {   // the old reader behind the writer (the host slower than the log) at a step back: the clamp, then never the live tail
    Ring r(cap);
    OldReader old(cap);
    std::vector<uint8_t> out;
    old.rewind(r);
    for (unsigned i = 0; i < 10; ++i) {
      r.write(300);
      old.pump(r, out, 200);   // 100 bytes behind more each time
    }
    const Ring::Write a = r.reserve(50), b = r.reserve(50);
    r.copy(b);
    r.header(b);
    old.pump(r, out, 200);
    r.copy(a);
    r.header(a);
    old.pump(r, out);   // the step back: avail clamps at cap, then it drains fully
    for (unsigned i = 0; i < 20; ++i) {   // from here the host keeps up, yet what arrives last is not what was written last
      r.write(100);
      old.pump(r, out);
    }
    bool live = true;
    for (unsigned i = 0; i < 100; ++i) live &= out[out.size() - 100 + i] == Ring::byteAt(r.total - 100 + i);
    CHECK(!live);
  }
  {   // the new one: the same order of events
    Ring r(cap);
    LogCursor c;
    Host h;
    r.write(1000);
    uint64_t t = 0;
    c.init(cap, r.start, r.size, t);
    c.open(true);
    t += 2000;
    c.sync(r.start, t);
    h.pump(c, r);
    const Ring::Write a = r.reserve(80), b = r.reserve(120);
    r.copy(b);
    r.header(b);
    c.sync(r.start, t += 10);
    r.copy(a);
    r.header(a);
    c.sync(r.start, t += 10);
    CHECK(c.backSteps == 1 && c.written == 1200);
    t += 2000;
    c.sync(r.start, t);
    c.sync(r.start, t += 2000);
    h.pump(c, r);
    CHECK(h.got.size() == 1200 && h.inOrderFrom(0) && c.laps == 0 && c.lost == 0);
  }
  {   // many writers in random order over several laps; the reader keeps up: everything once, in order
    Ring r(cap);
    LogCursor c;
    Host h;
    uint64_t t = 0;
    c.init(cap, r.start, r.size, t);
    c.open(true);
    uint32_t seed = 12345;
    auto rnd = [&]() { return seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff; };
    for (unsigned round = 0; round < 4000; ++round) {
      Ring::Write w[4];
      const unsigned n = 1 + rnd() % 4;
      for (unsigned i = 0; i < n; ++i) w[i] = r.reserve(1 + rnd() % 200);
      for (unsigned i = 0; i < n; ++i) r.copy(w[i]);
      for (unsigned k = 0; k < n; ++k) {   // the headers in a random order: steps back included
        const unsigned i = rnd() % n;
        r.header(w[i]);
        c.sync(r.start, t += 50);
      }
      r.header(w[n - 1]);   // the last reservation's end: where the writers really are
      c.sync(r.start, t += 1500);
      c.sync(r.start, t += 1500);
      h.pump(c, r);
    }
    CHECK(c.backSteps > 100 && c.laps == 0 && c.lost == 0);
    CHECK(h.got.size() == r.total && h.inOrderFrom(0));
  }
}

static void testSettle()
{
  Ring r(4096);
  LogCursor c;
  Host h;
  c.init(r.cap, r.start, r.size, 0);
  c.open(true);
  r.write(100);
  c.sync(r.start, 100);
  CHECK(c.readable() == 0 && c.unsettled());   // covered by the header for 100 us only
  c.sync(r.start, 1200);                       // 1.1 ms: now pending, settled next time
  c.sync(r.start, 2400);
  CHECK(c.readable() == 100 && !c.unsettled());
  h.pump(c, r);
  CHECK(h.got.size() == 100 && h.inOrderFrom(0));
}

static void testOverrun()
{
  const uint32_t cap = 8192;
  Ring r(cap);
  LogCursor c;
  Host h;
  uint64_t t = 0;
  c.init(cap, r.start, r.size, t);
  c.open(true);
  for (unsigned i = 0; i < 100; ++i) {   // 20 KB while the host reads nothing, synced every write
    r.write(200);
    c.sync(r.start, t += 2000);
  }
  c.sync(r.start, t += 2000);
  h.pump(c, r);
  const uint64_t from = r.total - (cap - c.margin());
  CHECK(c.laps == 1 && c.lost == from && h.got.size() == cap - c.margin() && h.inOrderFrom(from));
  r.write(300);   // and on from there
  c.sync(r.start, t += 2000);
  c.sync(r.start, t += 2000);
  h.pump(c, r);
  CHECK(h.got.size() == cap - c.margin() + 300 && h.inOrderFrom(from));
}

static void testSessions()
{
  const uint32_t cap = 65536;
  {   // the first open replays what the ring holds, a reopen resumes where the host's stream stopped (once)
    Ring r(cap);
    r.write(5000);
    LogCursor c;
    uint64_t t = 0;
    c.init(cap, r.start, r.size, t);
    CHECK(c.open(true) == LogCursor::Start::replay);
    Host h;
    c.sync(r.start, t += 2000);
    h.pump(c, r, 3000);                 // the host took 3000 bytes
    c.close(1000);                      // 1000 of them were still queued or in flight at the close
    r.write(4000);                      // the log goes on while the port is closed
    c.sync(r.start, t += 2000);
    c.sync(r.start, t += 2000);
    CHECK(c.open(true) == LogCursor::Start::resume && c.sent == 2000);
    Host h2;
    h2.pump(c, r);
    CHECK(h2.got.size() == 7000 && h2.inOrderFrom(2000) && c.sessions == 2);   // 2000.., not 0..: no second full replay
    c.close(0);
    CHECK(c.open(true) == LogCursor::Start::resume && c.readable() == 0);      // nothing new: nothing sent again
  }
  {   // piano-usb-replay=2: every open replays the retained log
    Ring r(cap);
    r.write(5000);
    LogCursor c;
    c.init(cap, r.start, r.size, 0);
    c.open(true, true);
    c.sent = 4000;
    c.close(0);
    CHECK(c.open(true, true) == LogCursor::Start::replay && c.sent == 0);
  }
  {   // piano-usb-replay=0: the first session starts from now
    Ring r(cap);
    r.write(5000);
    LogCursor c;
    c.init(cap, r.start, r.size, 0);
    CHECK(c.open(false) == LogCursor::Start::now && c.sent == 5000);
  }
  {   // a reopen after more than a ring: the oldest retained byte, the loss counted
    Ring r(cap);
    LogCursor c;
    uint64_t t = 0;
    c.init(cap, r.start, r.size, t);
    c.open(true);
    c.close(0);
    for (unsigned i = 0; i < 40; ++i) {
      r.write(2000);
      c.sync(r.start, t += 2000);
    }
    c.sync(r.start, t += 2000);
    c.open(true);
    Host h;
    h.pump(c, r);
    CHECK(c.lost > 0 && h.inOrderFrom(r.total - (cap - c.margin())));
  }
  {   // the monitor: `log` replays the retained log, `live on` goes on from now
    Ring r(cap);
    r.write(3000);
    LogCursor c;
    c.init(cap, r.start, r.size, 0);
    c.open(false);
    c.rewind(true);
    CHECK(c.sent == 0);
    c.rewind(false);
    CHECK(c.sent == 3000);
  }
  {   // the ring already wrapped at init (a long boot before the probe): positions still line up with the ring
    Ring r(cap);
    for (unsigned i = 0; i < 50; ++i) r.write(3000);
    LogCursor c;
    uint64_t t = 0;
    c.init(cap, r.start, r.size, t);
    c.open(true);
    r.write(100);
    c.sync(r.start, t += 2000);
    c.sync(r.start, t += 2000);
    Host h;
    h.pump(c, r);
    // the replay started at the oldest safe byte; the 100 written meanwhile overwrote as many of the oldest (counted lost)
    CHECK(h.got.size() == cap - c.margin() && h.inOrderFrom(r.total - (cap - c.margin())) && c.lost == 100);
  }
}

static void testSmallRing()
{
  LogCursor c;
  c.init(5934, 0, 0, 0);   // pstore-console-size=0x10000: (65536 - 256) / 11
  CHECK(c.backLimit() == 5934 / 4 && c.margin() == 5934 / 8);
  c.sync(4000, 2000);   // forward by 4000 (within cap - backLimit = 4451)
  CHECK(c.written == 4000 && c.backSteps == 0);
  c.sync(3900, 4000);   // back by 100
  CHECK(c.written == 4000 && c.backSteps == 1);
  c.sync(4000 + 1000, 6000);   // past the furthest again: forward by 1000 from 4000
  CHECK(c.written == 5000);
  LogCursor none;
  none.init(0, 0, 0, 0);
  none.sync(10, 10);
  CHECK(none.readable() == 0);
}

int main()
{
  testRace();
  testSettle();
  testOverrun();
  testSessions();
  testSmallRing();
  if (failures) {
    std::printf("log_cursor_host: %d FAILED\n", failures);
    return 1;
  }
  std::printf("log_cursor_host: the writers' race (the old reader's lap reproduced; the new one once and in order), the settle, an "
              "overrun, sessions (replay, resume once, from now, the monitor), small rings: all passed\n");
  return 0;
}
