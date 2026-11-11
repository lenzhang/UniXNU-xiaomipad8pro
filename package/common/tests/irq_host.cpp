// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of package/common/PianoIrq.h's facts and state machines: the TLMM INTR_CFG encoding (against the words PianoTouch
// and PianoKeyboard wrote before it existed), the reserved GPIOs, the PDC map, Backoff, StormGuard, MissGuard and WakeStats.
#include "../PianoIrq.h"
#include <cstdio>
#include <cstdlib>

using namespace pianoirq;

static unsigned gFailures;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++gFailures;                                                            \
    }                                                                         \
  } while (0)

static void testIntrCfg()
{
  using namespace tlmm;
  // The word PianoTouch's armIrq and PianoKeyboard's armIrq wrote for a rising edge, latched, not enabled, target 3.
  const uint32_t founds[] = {0x0u, 0x0e2u, 0x7ffu, 0xffffffffu, 0x00c0u, 0x0300u};
  for (uint32_t found : founds) {
    const uint32_t old = (found & ~(0x1fu | (7u << 8))) | (1u << 4) | (1u << 2) | (1u << 1) | (3u << 8);
    CHECK(intrCfg(found, Trigger::rising, false) == old);
    CHECK(intrCfg(found, Trigger::rising, true) == (old | 1u));
    // Bits outside the owned ones (wakeup present/enable among them) are kept.
    CHECK((intrCfg(found, Trigger::both, true) & ~kCfgOwned) == (found & ~kCfgOwned));
    CHECK((intrCfg(found, Trigger::levelLow, false) & (kCfgWakeupPresent | kCfgWakeupEnable)) ==
          (found & (kCfgWakeupPresent | kCfgWakeupEnable)));
  }
  // Detection and polarity per trigger (width-2 detection field).
  CHECK((intrCfg(0, Trigger::levelLow, false) & (kCfgDetectMask | kCfgPolarity)) == 0);
  CHECK((intrCfg(0, Trigger::levelHigh, false) & (kCfgDetectMask | kCfgPolarity)) == kCfgPolarity);
  CHECK((intrCfg(0, Trigger::rising, false) & (kCfgDetectMask | kCfgPolarity)) == ((1u << 2) | kCfgPolarity));
  CHECK((intrCfg(0, Trigger::falling, false) & (kCfgDetectMask | kCfgPolarity)) == ((2u << 2) | kCfgPolarity));
  CHECK((intrCfg(0, Trigger::both, false) & (kCfgDetectMask | kCfgPolarity)) == ((3u << 2) | kCfgPolarity));
  CHECK(intrCfg(0, Trigger::rising, false) & kCfgRawStatus);
  CHECK(targetsKpss(intrCfg(0x700, Trigger::levelHigh, false)));
  CHECK(!foreignEnabled(intrCfg(0, Trigger::rising, false)));
  CHECK(foreignEnabled(intrCfg(0, Trigger::rising, true)));
  CHECK(!foreignEnabled(0x1u | (2u << 8)));   // enabled, but to another processor: not on our line
  CHECK(isEdge(Trigger::rising) && isEdge(Trigger::both) && !isEdge(Trigger::levelHigh));
  // Reserved ranges <36 4>, <48 4>, <74 1>.
  unsigned reservedCount = 0;
  for (unsigned g = 0; g < kGpios; ++g) reservedCount += reserved(g);
  CHECK(reservedCount == 9);
  CHECK(reserved(36) && reserved(39) && !reserved(40) && reserved(48) && reserved(51) && !reserved(52) && reserved(74) && !reserved(75));
  CHECK(!reserved(97) && !reserved(162) && !reserved(80));
  // Register offsets.
  CHECK(kBase + 162 * kStride + kIntrStatus == 0x0f1a200cull);
  CHECK(kGpios * kStride <= kBytes);
}

static void testFacts()
{
  CHECK(intid(kSpiDwc3) == 165);
  CHECK(intid(kSpiTlmmSummary) == 240);
  CHECK(gicdIcenabler(165) == 0x180 + 4 * 5 && gicdBit(165) == (1u << 5));
  CHECK(pdcSpi(kPdcSpmiArbiter) == 746);   // the PMIC arbiter's summary
  CHECK(pdcSpi(0) == 745 && pdcSpi(50) == 795 && pdcSpi(51) == 527 && pdcSpi(97) == 573);
  CHECK(pdcSpi(14) == 759 && pdcSpi(15) == 760 && pdcSpi(17) == 762);   // USB dp/dm/ss PHY wake lines
  CHECK(pdcSpi(98) == 609 && pdcSpi(129) == 640 && pdcSpi(130) == 717 && pdcSpi(146) == 255 && pdcSpi(162) == 811);
  CHECK(pdcSpi(163) == -1 && pdcSpi(500) == -1);
  for (const TlmmWake &w : kTlmmWake) CHECK(pdcSpi(w.pdcPin) > 0);
  CHECK(pdcSpi(109) == 620);   // gpio162 (touch)
  CHECK(pdcSpi(100) == 611);   // gpio97 (keyboard)
  CHECK(pdcSpi(122) == 633);   // gpio80 (hall)
  CHECK(pdcIrqCfg(109) == 0x110 + 4 * 109);
}

static void testBackoff()
{
  Backoff b = backoff(1, 10, 250, 200, 2000);
  CHECK(b.tier(0) == 2 && b.periodMs(5) == 250);   // nothing seen yet: idle
  b.activity(1000);
  CHECK(b.periodMs(1000) == 1 && b.periodMs(1199) == 1);
  CHECK(b.periodMs(1200) == 10 && b.periodMs(3199) == 10);
  CHECK(b.periodMs(3200) == 250 && b.periodMs(100000) == 250);
  b.activity(100000);
  CHECK(b.periodMs(100000) == 1);
  CHECK(b.periodMs(50) == 1);   // a clock that went backwards counts as now
  Backoff two = backoff(10, 40, 40, 1000, 0);   // two tiers: mid == idle
  two.activity(0);
  CHECK(two.periodMs(999) == 10 && two.periodMs(1000) == 40);
}

static void testStormGuard()
{
  StormGuard g = stormGuard(100, 5);
  for (int i = 0; i < 5; ++i) CHECK(!g.stray(10 + i));
  CHECK(g.stray(20));   // the sixth within the window trips it
  CHECK(g.stray(5000)); // and it stays tripped
  StormGuard slow = stormGuard(100, 5);
  for (int i = 0; i < 100; ++i) CHECK(!slow.stray(uint64_t(i) * 30));   // ~3 per window: never
  StormGuard edge = stormGuard(100, 2);
  CHECK(!edge.stray(0) && !edge.stray(50));
  CHECK(!edge.stray(101));   // a new window starts after 100
  CHECK(!edge.stray(150) && edge.stray(160));
}

static void testMissGuard()
{
  MissGuard m = missGuard(3);
  CHECK(!m.miss() && !m.miss());
  m.delivered();
  CHECK(!m.miss() && !m.miss());
  CHECK(m.miss() && m.dead && m.total == 5);
}

static void testWakeStats()
{
  WakeStats w = wakeStats(60000);
  WakeStats::Window out{};
  CHECK(!w.due(1000, &out));   // starts the window
  for (int i = 0; i < 10; ++i) w.count(Woke::timeout);
  w.count(Woke::irq);
  w.count(Woke::kick);
  CHECK(!w.due(60999, &out));
  CHECK(w.due(61000, &out));
  CHECK(out.seconds == 60 && out.wakeups == 12 && out.irqs == 1 && out.kicks == 1 && out.timeouts == 10);
  CHECK(w.wakeups == 0 && w.totalWakeups == 12 && w.totalIrqs == 1);
  CHECK(!w.due(61001, &out));
  CHECK(w.due(125000, &out) && out.seconds == 64 && out.wakeups == 0);
  WakeStats off = wakeStats(0);
  CHECK(!off.due(0, &out) && !off.due(1000000, &out));
}

int main()
{
  testIntrCfg();
  testFacts();
  testBackoff();
  testStormGuard();
  testMissGuard();
  testWakeStats();
  if (gFailures) {
    std::fprintf(stderr, "irq_host: %u failure(s)\n", gFailures);
    return 1;
  }
  std::printf("irq_host: TLMM INTR_CFG encoding, reserved GPIOs, PDC map, Backoff, StormGuard, MissGuard, WakeStats: ok\n");
  return 0;
}
