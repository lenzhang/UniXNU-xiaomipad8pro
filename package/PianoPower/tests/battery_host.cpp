// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of BatteryMap.h: what macOS is shown as the battery (piano-power-ps=1) from the two gauge readings and the Type-C
// state. Built and run by tooling/k27/build-native-piano-power.sh before the kext.
#include <cstdio>
#include "BatteryMap.h"

using namespace pianopower;

static int gFailures = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures;                                                               \
    }                                                                            \
  } while (0)

static GaugeReading pack(int mv, int ma, int avg, int soc, int rem, int full, int design, int deciC, uint16_t status, int cycles)
{
  GaugeReading g;
  g.ok = true;
  g.voltageMv = mv; g.currentMa = ma; g.averageMa = avg; g.socPct = soc; g.remainingMah = rem; g.fullMah = full;
  g.designMah = design; g.tempDeciC = deciC; g.status = status; g.cycles = cycles;
  return g;
}

static PortState source(uint8_t rpBits)
{
  PortState p;
  p.ok = true; p.attached = true; p.sourceSeen = true; p.cc = 1; p.rpBits = rpBits;
  return p;
}

int main()
{
  // No reading: nothing to publish.
  {
    GaugeReading none[2];
    const PackReading empty = combinePacks(none, 2);
    CHECK(!mapBattery(none, 2, empty, nullptr, 0).known);
  }
  // On battery, discharging: percentage from the combined charge, negative amperage, time to empty from the average current.
  {
    const GaugeReading g[2] = {pack(3900, -400, -380, 55, 2400, 4800, 5000, 305, kGaugeDsg, 113),
                               pack(3902, -420, -400, 56, 2450, 4850, 5000, 312, kGaugeDsg, 114)};
    const PackReading p = combinePacks(g, 2);
    PortState nothing;
    nothing.ok = true;
    const BatteryValues v = mapBattery(g, 2, p, &nothing, 0);
    CHECK(v.known && v.installed && !v.external && !v.charging && !v.full && !v.clamped);
    CHECK(v.percent == 50 && v.maxCapacity == 100);                           // 4850 of 9650 mAh
    CHECK(v.rawMah == 4850 && v.fullMah == 9650 && v.designMah == 10000);
    CHECK(v.voltageMv == 3901 && v.amperageMa == -780 && v.instantMa == -820);
    CHECK(v.temperatureValid && v.temperatureCenti == 3120 && v.cycles == 114);
    CHECK(v.timeToEmpty == 4850 * 60 / 780 && v.timeRemaining == v.timeToEmpty && v.timeToFull == kTimeUnknown);
    CHECK(!v.adapter);
  }
  // On the Mac's port, charging: external, charging, time to full, adapter details from the Rp bits.
  {
    const GaugeReading g[2] = {pack(4100, 600, 580, 80, 3800, 4800, 5000, 290, 0, 113), pack(4100, 620, 600, 81, 3900, 4850, 5000, 291, 0, 114)};
    const PackReading p = combinePacks(g, 2);
    const PortState port = source(0x4);
    const BatteryValues v = mapBattery(g, 2, p, &port, 0);
    CHECK(v.external && v.chargeCapable && v.charging && !v.full);
    CHECK(v.timeToFull == (9650 - 7700) * 60 / 1180 && v.timeRemaining == v.timeToFull && v.timeToEmpty == kTimeUnknown);
    CHECK(v.adapter && v.adapterFamily == kFamilyTypeC && v.adapterMv == 5000 && v.adapterMa == 1500 && v.adapterWatts == 8);
  }
  // On power at 99 %, idle (the measured level 1 state): external, not charging, not full, no time.
  {
    const GaugeReading g[2] = {pack(4480, 0, 0, 99, 4287, 4344, 5000, 290, 0, 113), pack(4479, 0, 0, 99, 4235, 4290, 5000, 300, 0, 114)};
    const PackReading p = combinePacks(g, 2);
    const PortState port = source(0x2);
    const BatteryValues v = mapBattery(g, 2, p, &port, 0);
    CHECK(v.external && !v.charging && !v.full && v.percent == 99);
    CHECK(v.timeRemaining == kTimeUnknown && v.adapterMa == 3000 && v.adapterWatts == 15);   // the Mac's port: Rp 0x2 is 3.0 A
  }
  // Full: both packs report FC on power.
  {
    const GaugeReading g[2] = {pack(4480, 0, 0, 100, 4344, 4344, 5000, 290, kGaugeFc, 113), pack(4479, 0, 0, 100, 4290, 4290, 5000, 300, kGaugeFc, 114)};
    const PackReading p = combinePacks(g, 2);
    const PortState port = source(0x8);
    const BatteryValues v = mapBattery(g, 2, p, &port, 0);
    CHECK(v.full && v.percent == 100 && v.adapterMa == 500);   // Rp bit 3: the default USB current
  }
  // The floor: on battery the percentage shown never goes under it; on power, and with no floor, the true value.
  {
    const GaugeReading g[2] = {pack(3500, -500, -500, 3, 140, 4800, 5000, 300, kGaugeDsg, 113), pack(3500, -500, -500, 3, 150, 4850, 5000, 300, kGaugeDsg, 114)};
    const PackReading p = combinePacks(g, 2);
    PortState nothing;
    nothing.ok = true;
    CHECK(mapBattery(g, 2, p, &nothing, 0).percent == 3);
    const BatteryValues f = mapBattery(g, 2, p, &nothing, 10);
    CHECK(f.percent == 10 && f.clamped);
    const PortState port = source(0x2);
    const BatteryValues on = mapBattery(g, 2, p, &port, 10);
    CHECK(on.percent == 3 && !on.clamped);
  }
  // One pack missing, no port view, no design capacity: the other pack alone; external from the current; design = full.
  {
    GaugeReading g[2];
    g[1] = pack(4000, 300, 300, 60, 2900, 4850, 0, 300, 0, 114);
    const PackReading p = combinePacks(g, 2);
    const BatteryValues v = mapBattery(g, 2, p, nullptr, 0);
    CHECK(v.known && v.external && v.charging && v.designMah == 4850 && v.percent == 60 && !v.adapter);
  }
  // An implausible temperature is not published.
  {
    const GaugeReading g[1] = {pack(4000, 0, 0, 60, 2900, 4850, 5000, 899, 0, 114)};
    GaugeReading copy = g[0];
    copy.tempDeciC = 950;   // above the gauge check: combinePacks would not take it; the map's own guard is tested directly
    PackReading p = combinePacks(g, 1);
    p.tempDeciC = 950;
    CHECK(!mapBattery(&copy, 1, p, nullptr, 0).temperatureValid);
  }
  if (gFailures) {
    std::fprintf(stderr, "battery_host: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("battery_host: all passed\n");
  return 0;
}
