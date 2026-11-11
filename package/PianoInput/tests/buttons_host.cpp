// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoInput's portable parts: SpmiObserver.h on an SPMI arbiter v7 model (APID map, owner table, observer
// channels), Buttons.h's decoding and debounce, PowerKey.h (the power key held back for PianoPolicy, through the debounce at the
// 10 ms rate, and its reset after a wake), and HidReports.h's buttons descriptor (parsed item by item: the bits it declares are
// the bits the driver sends).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. buttons_host.cpp -o /tmp/buttons_host && /tmp/buttons_host
#include "SpmiObserver.h"
#include "Buttons.h"
#include "HidReports.h"
#include "PowerKey.h"
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
static PressRun runPress(uint32_t idleMs, uint64_t start, uint64_t length, uint8_t key = kKeyVolumeUp, uint32_t darkMs = 0)
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
    t += darkPeriod(plan.next(d, t), darkMs != 0, darkMs);
  }
  return r;
}

// piano-input-dark=MS: the display-off idle period; nothing changes lit or with 0; a busy key keeps 10 ms; capped at 200 ms; a press
// of the period plus the debounce plus a fast poll is always reported (every phase), with about 1000/MS samples an idle second.
static void testDarkPolling()
{
  CHECK(darkPeriod(kIdlePollMs, false, 100) == kIdlePollMs && darkPeriod(kIdlePollMs, true, 0) == kIdlePollMs);
  CHECK(darkPeriod(kPollMs, true, 100) == kPollMs && darkPeriod(kIdlePollMs, true, 100) == 100);
  CHECK(darkPeriod(kIdlePollMs, true, 20) == kIdlePollMs && darkPeriod(kIdlePollMs, true, 5000) == kIdlePollMaxMs);
  for (uint32_t dark : {100u, 200u})
    for (uint64_t phase = 0; phase < dark; phase += 7) {
      const uint64_t shortest = dark + kDebounceMs + kPollMs;
      const PressRun r = runPress(kIdlePollMs, 10000 + phase, shortest, kKeyPower, dark);
      CHECK(r.reported && r.released && r.reportedAfter <= dark + kDebounceMs + kPollMs);
      CHECK(r.idleSecondSamples >= 1000 / dark - 1 && r.idleSecondSamples <= 1000 / dark + 1);
    }
  // A tap shorter than the period can fall between two samples in the dark (lit at 40 ms it is reported): why it is opt-in.
  bool missed = false;
  for (uint64_t phase = 0; phase < 100 && !missed; ++phase) missed = !runPress(kIdlePollMs, 10000 + phase, 60, kKeyPower, 100).reported;
  CHECK(missed);
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

// The poll loop's power-key path as PianoInput.cpp runs it: raw samples every 10 ms through Debounce, PowerKey on its output, the
// report to macOS per sample, PianoPolicy's answer. Records every report change and every question to PianoPolicy.
struct KeyRun {
  struct Ask { uint64_t at; uint32_t heldMs; };
  std::vector<std::pair<uint64_t, uint8_t>> reports;   // (ms, report) at each change, a pulse as two entries
  std::vector<Ask> asks;
  Debounce d;
  PowerKey k;
  uint8_t sent = 0;
  bool policyThere = true, policyTakes = true;
  uint8_t mask = kKeysAll;
  void sample(uint8_t raw, uint64_t t)
  {
    const uint8_t down = d.update(true, raw, t);
    const bool power = down & kKeyPower;
    const bool hold = k.starts(power) && (mask & kKeyPower) && policyThere;
    uint32_t held = 0;
    const PowerStep step = k.update(power, hold, t, &held);
    const uint8_t report = k.report(down, mask);
    if (step == PowerStep::ask) {
      asks.push_back({t, held});
      if (!policyTakes) {
        reports.push_back({t, uint8_t(report | kKeyPower)});
        reports.push_back({t, report});
        sent = report;
      }
    }
    if (report != sent) {
      reports.push_back({t, report});
      sent = report;
    }
  }
  // raw: the power key down from `from` for `length` ms (plus `other` held throughout), sampled every 10 ms until `until`
  void press(uint64_t from, uint64_t length, uint64_t until, uint8_t other = 0, uint64_t start = 0)
  {
    for (uint64_t t = start; t < until; t += kPollMs) sample(uint8_t(other | (t >= from && t < from + length ? kKeyPower : 0)), t);
  }
};

static void testPowerKey()
{
  {   // no PianoPolicy: reported as it happens (as before), nobody asked
    KeyRun r;
    r.policyThere = false;
    r.press(1000, 300, 3000);
    CHECK(r.asks.empty());
    CHECK(r.reports.size() == 2 && r.reports[0].second == kKeyPower && r.reports[1].second == 0);
    CHECK(r.reports[0].first >= 1000 + kDebounceMs && r.reports[0].first <= 1000 + kDebounceMs + kPollMs);
    CHECK(r.reports[1].first >= 1300 + kDebounceMs && r.reports[1].first <= 1300 + kDebounceMs + kPollMs);
  }
  {   // PianoPolicy takes a short press: nothing reaches macOS; asked once, on the release, with the press's length
    KeyRun r;
    r.press(1000, 300, 3000);
    CHECK(r.reports.empty());
    CHECK(r.asks.size() == 1 && r.asks[0].heldMs >= 290 && r.asks[0].heldMs <= 310);
    CHECK(r.asks[0].at >= 1300 + kDebounceMs && r.asks[0].at <= 1300 + kDebounceMs + kPollMs);
    CHECK(r.k.shortPresses() == 1 && r.k.longPresses() == 0 && !r.k.reported());
  }
  {   // PianoPolicy does not take it (level 1, no system sleep, powerkey=0): the Power usage down and up at the release
    KeyRun r;
    r.policyTakes = false;
    r.press(1000, 200, 3000);
    CHECK(r.asks.size() == 1);
    CHECK(r.reports.size() == 2 && r.reports[0].second == kKeyPower && r.reports[1].second == 0 && r.reports[0].first == r.asks[0].at &&
          r.reports[1].first == r.asks[0].at);
  }
  {   // a long press: down at 1.5 s, up at the release; PianoPolicy is not asked
    KeyRun r;
    r.press(1000, 2500, 5000);
    CHECK(r.asks.empty());
    CHECK(r.reports.size() == 2 && r.reports[0].second == kKeyPower && r.reports[1].second == 0);
    const uint64_t pressed = r.reports[0].first - kLongPressMs;   // the debounced press
    CHECK(pressed >= 1000 + kDebounceMs && pressed <= 1000 + kDebounceMs + kPollMs);
    CHECK(r.reports[1].first >= 3500 + kDebounceMs && r.reports[1].first <= 3500 + kDebounceMs + kPollMs);
    CHECK(r.k.longPresses() == 1 && r.k.shortPresses() == 0);
  }
  {   // the threshold: 1490 ms debounced is short; a press seen down 1500 ms after its debounced start is long
    KeyRun s;
    s.press(1000, 1490, 4000);
    CHECK(s.asks.size() == 1 && s.asks[0].heldMs == 1490 && s.reports.empty());
    KeyRun l;
    l.press(1000, 1510, 4000);
    CHECK(l.asks.empty() && l.reports.size() == 2);
  }
  {   // the other keys pass while the power key is held back; a volume key held with it stays reported
    KeyRun r;
    for (uint64_t t = 0; t < 4000; t += kPollMs)
      r.sample(uint8_t((t >= 500 && t < 3000 ? kKeyVolumeUp : 0) | (t >= 1000 && t < 1400 ? kKeyPower : 0)), t);
    CHECK(r.asks.size() == 1);
    CHECK(r.reports.size() == 2 && r.reports[0].second == kKeyVolumeUp && r.reports[1].second == 0);
  }
  {   // the power key masked (piano-input-mask): never held back, never reported, never asked
    KeyRun r;
    r.mask = kKeyVolumeUp | kKeyVolumeDown;
    r.press(1000, 300, 3000);
    CHECK(r.asks.empty() && r.reports.empty());
  }
  {   // PianoPolicy appears during a press that started reported: it stays reported until its release
    KeyRun r;
    r.policyThere = false;
    for (uint64_t t = 0; t < 3000; t += kPollMs) {
      if (t == 1100) r.policyThere = true;
      r.sample(t >= 1000 && t < 1500 ? kKeyPower : 0, t);
    }
    CHECK(r.asks.empty() && r.reports.size() == 2);
    r.press(3500, 200, 5000, 0, 3000);   // the next press is held back
    CHECK(r.asks.size() == 1 && r.reports.size() == 2);
  }
  {   // a system sleep during a long press (reported down): after the wake the filter and the key are reset; the key read up
      // gives the release at once; a key still down after the wake is reported only after it has been up once
    KeyRun r;
    r.press(1000, 3000, 2800);   // long press reported at ~2540 ms, still down at 2800
    CHECK(r.reports.size() == 1 && r.reports[0].second == kKeyPower);
    r.d = Debounce();
    r.k.reset();
    r.press(0, 0, 3200, 0, 2800);   // after the wake: up
    CHECK(r.reports.size() == 2 && r.reports[1].second == 0 && r.reports[1].first == 2800);
    KeyRun h;
    h.press(1000, 3000, 2800);
    h.d = Debounce();
    h.k.reset();
    for (uint64_t t = 2800; t < 5000; t += kPollMs) h.sample(kKeyPower, t);   // held through the wake: not reported, not asked
    CHECK(h.asks.empty() && h.reports.size() == 2 && h.reports[1].second == 0);
    for (uint64_t t = 5000; t < 6000; t += kPollMs) h.sample(0, t);
    CHECK(h.asks.empty() && h.reports.size() == 2);
    h.press(6000, 200, 7000, 0, 6000);   // the next real press goes to PianoPolicy
    CHECK(h.asks.size() == 1);
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
  testDarkPolling();
  testSpmi();
  testDecode();
  testDebounce();
  testAdaptivePolling();
  testPowerKey();
  testDescriptor();
  if (failures) {
    std::printf("buttons_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("buttons_host: all passed\n");
  return 0;
}
