// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: a CPU cluster's temperature against its stock trips (the Governor's thermal input), and the "near a trip" test
// that makes the poll fast. Portable; host-tested by tests/thermal_host.cpp.
//
// The cluster's sensors are the zones with a CPU cooling map in the stock DT: cpu-0-* (tsens0 sensors 1..12) cool cpu0's policy,
// cpu-1-* (tsens1 sensors 1..4) cpu6's (cpuss-* are monitoring only). Each sensor counts against its own zone's passive trip and
// hysteresis (105 C / 5 C, or 118 C / 8 C) unless piano-cpufreq-hot / -release (absolute, C) override them, and against the critical
// threshold (125 C - piano-thermal-margin, or piano-cpufreq-cut). Ubuntu's history: one prime-core load heats a zone at ~200 C/s,
// so Linux polls these zones every 10 ms while passive; PianoThermal's poll tiers are pollTier() below.
#ifndef PIANO_THERMAL_CLUSTER_H
#define PIANO_THERMAL_CLUSTER_H
#include "Governor.h"
#include "Tsens.h"

namespace pianothermal {

struct ClusterTrips {
  int32_t hotDeci = 0;       // 0: each sensor's stock passive trip
  int32_t releaseDeci = 0;   // 0: each sensor's passive trip - its hysteresis
  int32_t cutDeci = 0;       // 0: each sensor's critical trip - marginDeci
  int32_t marginDeci = 100;
};

constexpr int32_t kNearDeci = 100;   // fast polling within 10 C of a trip

inline ThermalInput clusterThermal(const Monitor &m, unsigned cluster, const ClusterTrips &t, bool critical)
{
  ThermalInput in{false, -100000, -100000, critical};
  if (cluster > 1) return in;
  const Controller &k = kMap[cluster];
  for (unsigned s = k.cpuFirst; s <= k.cpuLast && s < k.count; ++s) {
    if (!m.usable(cluster, s)) continue;
    const Sensor &sn = k.sensors[s];
    const int32_t d = m.deci(cluster, s);
    const int32_t hot = t.hotDeci ? t.hotDeci : sn.passiveDeci;
    const int32_t release = t.releaseDeci ? t.releaseDeci : sn.passiveDeci - sn.passiveHystDeci;
    const int32_t cut = t.cutDeci ? t.cutDeci : sn.criticalDeci - t.marginDeci;
    in.valid = true;
    if (d - hot > in.hotExcessDeci) in.hotExcessDeci = d - hot;
    if (d - release > in.releaseExcessDeci) in.releaseExcessDeci = d - release;
    if (d >= cut) in.cut = true;
  }
  return in;
}

// Any usable sensor of the SoC within kNearDeci of its policy passive threshold (Tsens.h passiveOf), or of a cluster override.
inline bool nearTrip(const Monitor &m, const ClusterTrips &t)
{
  for (unsigned c = 0; c < kControllers; ++c)
    for (unsigned s = 0; s < kMap[c].count; ++s) {
      if (!m.usable(c, s)) continue;
      int32_t trip = passiveOf(kMap[c].sensors[s]);
      if (c <= 1 && s >= kMap[c].cpuFirst && s <= kMap[c].cpuLast && t.hotDeci && t.hotDeci < trip) trip = t.hotDeci;
      if (m.deci(c, s) >= trip - kNearDeci) return true;
    }
  return false;
}

// The poll period's tier. A 1 s poll that only speeds up within 10 C of a trip can step over the whole window (34.5 -> 91 C in
// 0.26 s; a 250 ms poll overshot Linux's trip by ~10 C, so Linux polls these zones every 10 ms while passive):
//   fast  a sensor within kNearDeci of its trip, a cluster thermally limited, or a cluster set above the fast-poll clock
//         (piano-thermal-fast-mhz: the clocks at which a prime-core load heats that fast) -> piano-thermal-fast-ms (10 ms);
//   warm  the hottest sensor at piano-thermal-warm-poll (70 C) or more -> 250 ms (or the base when shorter);
//   base  1 s (the governor's 100 ms tick when it runs).
// The tsens upper-threshold interrupt will replace the warm tier's polling (docs/drivers/thermal.md section 10).
enum class Poll : uint8_t { base, warm, fast };

inline const char *pollName(Poll p) { return p == Poll::fast ? "fast" : p == Poll::warm ? "warm" : "base"; }

inline Poll pollTier(const Monitor &m, const ClusterTrips &t, bool limited, bool clusterFast, int32_t warmDeci)
{
  if (limited || clusterFast || nearTrip(m, t)) return Poll::fast;
  int32_t d;
  unsigned c, s;
  if (m.hottestAll(d, c, s) && d >= warmDeci) return Poll::warm;
  return Poll::base;
}

inline uint32_t pollPeriodMs(Poll p, uint32_t baseMs, uint32_t warmMs, uint32_t fastMs)
{
  if (p == Poll::fast) return fastMs < baseMs ? fastMs : baseMs;
  if (p == Poll::warm) return warmMs < baseMs ? warmMs : baseMs;
  return baseMs;
}

}  // namespace pianothermal

#endif
