// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoInput's portable parts: SpmiObserver.h on an SPMI arbiter v7 model (APID map, owner table, observer
// channels), Buttons.h's decoding and debounce, and HidReports.h's buttons descriptor (parsed item by item: the bits it
// declares are the bits the driver sends).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. buttons_host.cpp -o /tmp/buttons_host && /tmp/buttons_host
#include "SpmiObserver.h"
#include "Buttons.h"
#include "HidReports.h"
#include <cstdio>
#include <map>
#include <vector>

using namespace pianoinput;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The arbiter: PON (PPID 0x013) twice, at APID 4 owned by EE 3 and at APID 9 owned by EE 0; GPIO6 (PPID 0x18d) at APID 200.
struct Arb {
  std::map<uint32_t, uint32_t> core, cnfg, obsv;
  std::map<uint32_t, uint8_t> regs;   // PPID << 8 | register
  std::map<uint32_t, uint32_t> apidPpid;
  uint32_t failStatus = 0;            // the status a read completes with (0: success)
  bool hang = false;                  // a read never completes
  unsigned commands = 0, delays = 0, otherWrites = 0;
  Arb()
  {
    core[0] = 0x70010000;
    core[4] = 0x80000000u | 300;      // count in bits 10:0
    place(4, 0x013, 3);
    place(9, 0x013, 0);
    place(200, 0x18d, 0);
  }
  void place(uint32_t apid, uint32_t ppid, uint32_t owner)
  {
    core[kSpmiApidMap + 4 * apid] = 1u << 31 | ppid << 8;
    cnfg[4 * apid] = owner;
    apidPpid[apid] = ppid;
  }
  static uint32_t rd(void *c, Window w, uint32_t o)
  {
    Arb &a = *static_cast<Arb *>(c);
    return (w == Window::core ? a.core : w == Window::cnfg ? a.cnfg : a.obsv)[o];
  }
  static void wr(void *c, Window w, uint32_t o, uint32_t v)
  {
    Arb &a = *static_cast<Arb *>(c);
    if (w != Window::obsv || o % kObsvStride != kObsvCmd) { ++a.otherWrites; return; }
    ++a.commands;
    const uint32_t apid = o / kObsvStride, opcode = v >> 27, reg = (v >> 4) & 0xff, count = (v & 7) + 1;
    CHECK(opcode == kOpExtReadL && count == 1);
    if (a.hang) { a.obsv[o + kObsvStatus] = 0; return; }
    a.obsv[o + kObsvStatus] = kStatusDone | a.failStatus;
    a.obsv[o + kObsvData] = a.regs[a.apidPpid[apid] << 8 | reg];
  }
  static void delay(void *c, unsigned) { ++static_cast<Arb *>(c)->delays; }
  Io io() { return {this, rd, wr, delay}; }
};

static void testSpmi()
{
  Arb m;
  const Io io = m.io();
  const Arbiter a = spmiArbiter(io);
  CHECK(a.v7 && a.count == 300 && a.version == 0x70010000);
  const Apid pon = spmiFindApid(io, a, kPonPpid), gpio = spmiFindApid(io, a, kVolUpGpioPpid), none = spmiFindApid(io, a, 0x071);
  CHECK(pon.apid == 9 && pon.owner == 0);   // EE 0's entry over EE 3's
  CHECK(gpio.apid == 200 && gpio.owner == 0);
  CHECK(none.apid < 0);

  m.regs[0x013 << 8 | kRegRtStatus] = 0xc0;
  m.regs[0x18d << 8 | kRegRtStatus] = 0x01;
  uint8_t v = 0;
  uint32_t st = 0;
  CHECK(spmiRead8(io, pon.apid, kRegRtStatus, &v, &st) == ReadResult::ok && v == 0xc0 && st == kStatusDone);
  CHECK(spmiRead8(io, gpio.apid, kRegRtStatus, &v) == ReadResult::ok && v == 0x01);
  CHECK(m.otherWrites == 0);   // the only stores are observer commands

  m.failStatus = 1u << 2;   // denied
  v = 0x5a;
  CHECK(spmiRead8(io, pon.apid, kRegRtStatus, &v, &st) == ReadResult::error && v == 0x5a && st == (kStatusDone | 4));
  m.failStatus = 0;
  m.hang = true;
  const unsigned before = m.delays;
  CHECK(spmiRead8(io, pon.apid, kRegRtStatus, &v) == ReadResult::timeout && m.delays - before == 1000);

  Arb old;
  old.core[0] = 0x50000000;   // v5: not read
  CHECK(!spmiArbiter(old.io()).v7);
}

static void testDecode()
{
  CHECK(decodeKeys(0x00, 0x01) == 0);
  CHECK(decodeKeys(0x80, 0x01) == kKeyPower);
  CHECK(decodeKeys(0x40, 0x01) == kKeyVolumeDown);
  CHECK(decodeKeys(0x00, 0x00) == kKeyVolumeUp);
  CHECK(decodeKeys(0xff, 0xfe) == kKeysAll);
  CHECK(decodeKeys(0x3f, 0xff) == 0);   // other PON bits are not keys
}

static void testDebounce()
{
  {   // a press after kDebounceMs of stable samples, a release likewise; a 20 ms bounce changes nothing
    Debounce d;
    uint64_t t = 0;
    for (; t < 100; t += kPollMs) CHECK(d.update(true, 0, t) == 0);
    CHECK(d.update(true, kKeyVolumeUp, t) == 0);
    t += kPollMs;
    CHECK(d.update(true, kKeyVolumeUp, t) == 0);
    t += kPollMs;
    CHECK(d.update(true, kKeyVolumeUp, t) == 0);
    t += kPollMs;
    CHECK(d.update(true, kKeyVolumeUp, t) == 0);   // 30 ms
    t += kPollMs;
    CHECK(d.update(true, kKeyVolumeUp, t) == kKeyVolumeUp);   // 40 ms
    t += kPollMs;
    CHECK(d.update(true, 0, t) == kKeyVolumeUp);   // a 20 ms bounce
    t += kPollMs;
    CHECK(d.update(true, 0, t) == kKeyVolumeUp);
    t += kPollMs;
    CHECK(d.update(true, kKeyVolumeUp, t) == kKeyVolumeUp);
    for (unsigned i = 0; i < 4; ++i) CHECK(d.update(true, 0, t += kPollMs) == kKeyVolumeUp);   // up for 0-30 ms
    CHECK(d.update(true, 0, t += kPollMs) == 0);   // released after 40 ms up
  }
  {   // a key down from the start is reported only after it has been up once
    Debounce d;
    uint64_t t = 1000;
    for (unsigned i = 0; i < 20; ++i, t += kPollMs) CHECK(d.update(true, kKeyPower, t) == 0);
    for (unsigned i = 0; i < 6; ++i, t += kPollMs) CHECK(d.update(true, 0, t) == 0);
    uint8_t got = 0;
    for (unsigned i = 0; i < 6; ++i, t += kPollMs) got = d.update(true, kKeyPower, t);
    CHECK(got == kKeyPower);
  }
  {   // read errors: shorter than the debounce keep the key, longer release it
    Debounce d;
    uint64_t t = 0;
    for (unsigned i = 0; i < 10; ++i, t += kPollMs) d.update(true, 0, t);
    for (unsigned i = 0; i < 6; ++i, t += kPollMs) d.update(true, kKeyVolumeDown, t);
    CHECK(d.update(true, kKeyVolumeDown, t) == kKeyVolumeDown);
    t += kPollMs;
    CHECK(d.update(false, 0, t) == kKeyVolumeDown);
    t += kPollMs;
    CHECK(d.update(true, kKeyVolumeDown, t) == kKeyVolumeDown);
    uint8_t got = kKeyVolumeDown;
    for (unsigned i = 0; i < 6; ++i) got = d.update(false, kKeyVolumeDown, t += kPollMs);
    CHECK(got == 0);
  }
}

// The adaptive poll loop (PollPlan + Debounce) over a press of `length` ms starting at `start`: whether it was reported, when
// (ms after the press began), the samples in total, and the samples in the idle second before it.
struct PressRun {
  bool reported = false, released = false;
  uint64_t reportedAfter = 0;
  unsigned samples = 0, idleSecondSamples = 0;
};
static PressRun runPress(uint32_t idleMs, uint64_t start, uint64_t length, uint8_t key = kKeyVolumeUp)
{
  PollPlan plan(idleMs);
  Debounce d;
  PressRun r;
  for (uint64_t t = 0; t < start + length + 3000;) {
    const uint8_t keys = t >= start && t < start + length ? key : 0;
    const uint8_t down = d.update(true, keys, t);
    ++r.samples;
    if (t >= start - 1000 && t < start) ++r.idleSecondSamples;
    if ((down & key) && !r.reported) {
      r.reported = true;
      r.reportedAfter = t - start;
    }
    if (r.reported && !(down & key)) r.released = true;
    t += plan.next(d, t);
  }
  return r;
}

static void testAdaptivePolling()
{
  CHECK(!PollPlan(0).adaptive() && !PollPlan(10).adaptive() && PollPlan(40).adaptive());
  CHECK(PollPlan(5000).backoff.idleMs == kIdlePollMaxMs);
  {   // idle 0: the old 10 ms everywhere
    const PressRun r = runPress(0, 10000, 60);
    CHECK(r.reported && r.released && r.idleSecondSamples == 100);
  }
  // Idle 40 ms: 25 samples in an idle second instead of 100; every press of 82 ms or more is reported, at every phase of the
  // idle period, and released; at most one idle period plus the debounce late.
  for (uint64_t phase = 0; phase < 40; ++phase) {
    for (uint64_t length : {82ull, 100ull, 150ull, 400ull, 2000ull}) {
      const PressRun r = runPress(kIdlePollMs, 10000 + phase, length);
      CHECK(r.reported && r.released);
      CHECK(r.reportedAfter <= kIdlePollMs + kDebounceMs + kPollMs);
      CHECK(r.idleSecondSamples >= 24 && r.idleSecondSamples <= 26);
    }
    const PressRun power = runPress(kIdlePollMs, 20000 + phase, 120, kKeyPower);
    CHECK(power.reported && power.released);
  }
  {   // a 30 ms tap is below the debounce: not reported at 10 ms polling either
    CHECK(!runPress(0, 10000, 30).reported);
    CHECK(!runPress(kIdlePollMs, 10003, 30).reported);
  }
  {   // after a press: 10 ms polling for kActiveHoldMs after the release (repeated presses stay fast), then idle again
    PollPlan plan(kIdlePollMs);
    Debounce d;
    uint64_t t = 0;
    for (; t < 3000; t += plan.next(d, t)) d.update(true, 0, t);
    CHECK(!plan.fast(t));
    for (; t < 3200; t += plan.next(d, t)) d.update(true, kKeyVolumeDown, t);
    CHECK(plan.fast(t));
    const uint64_t released = t;
    uint64_t lastFast = t;
    for (; t < 6000; t += plan.next(d, t)) {
      d.update(true, 0, t);
      if (plan.fast(t)) lastFast = t;
    }
    CHECK(lastFast + 2 * kPollMs >= released + kDebounceMs + kActiveHoldMs && lastFast <= released + kDebounceMs + kActiveHoldMs + 2 * kPollMs);
    CHECK(!plan.fast(t));
  }
  {   // a key held at start (never up yet) keeps the polling fast: it is busy
    PollPlan plan(kIdlePollMs);
    Debounce d;
    unsigned samples = 0;
    for (uint64_t t = 0; t < 2000; t += plan.next(d, t), ++samples) CHECK(d.update(true, kKeyPower, t) == 0);
    CHECK(samples >= 199);
  }
}

// A HID 1.11 short-item walk of the buttons descriptor: the input bits in order with their (page, usage).
static void testDescriptor()
{
  struct Field { uint32_t page, usage; bool constant; };
  std::vector<Field> bits;
  std::vector<uint32_t> usages;
  uint32_t page = 0, size = 0, count = 0, depth = 0, appPage = 0, appUsage = 0;
  const uint8_t *p = kButtonsDescriptor, *end = p + sizeof kButtonsDescriptor;
  while (p < end) {
    const uint8_t prefix = *p++;
    const unsigned n = (prefix & 3) == 3 ? 4 : prefix & 3;
    CHECK(p + n <= end);
    uint32_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= uint32_t(p[i]) << (8 * i);
    p += n;
    switch (prefix & 0xfc) {
    case 0x04: page = value; break;
    case 0x74: size = value; break;
    case 0x94: count = value; break;
    case 0x08: usages.push_back(value); break;
    case 0xa0:
      if (!depth) { appPage = page; appUsage = usages.empty() ? 0 : usages[0]; CHECK(value == 1); }
      ++depth;
      usages.clear();
      break;
    case 0xc0: CHECK(depth > 0); --depth; break;
    case 0x80:
      for (uint32_t i = 0; i < size * count; ++i)
        bits.push_back({page, (value & 1) || usages.empty() ? 0 : usages[i < usages.size() ? i : usages.size() - 1], (value & 1) != 0});
      usages.clear();
      break;
    case 0x14: case 0x24: break;   // logical minimum/maximum
    default: std::printf("FAIL: unexpected item %02x\n", prefix); ++failures;
    }
  }
  CHECK(depth == 0);
  CHECK(appPage == deviceInfo(Kind::buttons).usagePage && appUsage == deviceInfo(Kind::buttons).usage);
  CHECK(bits.size() == 8 * kButtonsReportBytes);
  auto bitOf = [&](uint8_t key) { unsigned i = 0; while (!((key >> i) & 1)) ++i; return bits[i]; };
  CHECK(!bitOf(kKeyPower).constant && bitOf(kKeyPower).page == 0x0c && bitOf(kKeyPower).usage == 0x30);
  CHECK(!bitOf(kKeyVolumeUp).constant && bitOf(kKeyVolumeUp).usage == 0xe9);
  CHECK(!bitOf(kKeyVolumeDown).constant && bitOf(kKeyVolumeDown).usage == 0xea);
  for (unsigned i = 3; i < bits.size(); ++i) CHECK(bits[i].constant);
  CHECK(deviceInfo(Kind::buttons).descriptor == kButtonsDescriptor && deviceInfo(Kind::buttons).descriptorBytes == sizeof kButtonsDescriptor);
}

int main()
{
  testSpmi();
  testDecode();
  testDebounce();
  testAdaptivePolling();
  testDescriptor();
  if (failures) {
    std::printf("buttons_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("buttons_host: all passed\n");
  return 0;
}
