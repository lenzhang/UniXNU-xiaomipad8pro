// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: a CPU cluster's temperature against its trips (the Governor's thermal input), and the "near a trip" test that makes
// the poll fast. Portable; host-tested by tests/thermal_host.cpp.
//
// The cluster's sensors are the zones with a CPU cooling map in the stock DT: cpu-0-* (tsens0 sensors 1..12) cool cpu0's policy,
// cpu-1-* (tsens1 sensors 1..4) cpu6's (cpuss-* are monitoring only). Each sensor counts against the CPU ceiling (piano-cpufreq-hot,
// default 90 C since 2026-11-12: P9, a governor image held cluster 0 at 3532 MHz and 102-105 C for 15 minutes with the stock 105 C
// trip as the only limit) but never above its own zone's stock passive trip (105 C or 118 C), with the release point
// piano-cpufreq-release (default 5 C under the ceiling; with the stock trips each zone's hysteresis, 5 or 8 C), and against the
// critical threshold (125 C - piano-thermal-margin, or piano-cpufreq-cut). hotDeci 0 is the stock trips (the old behaviour).
// Ubuntu's history: one prime-core load heats a zone at ~200 C/s, so Linux polls these zones every 10 ms while passive;
// PianoThermal's poll tiers are pollTier() below.
#ifndef PIANO_THERMAL_CLUSTER_H
#define PIANO_THERMAL_CLUSTER_H
#include "Governor.h"
#include "Tsens.h"

namespace pianothermal {

struct ClusterTrips {
  int32_t hotDeci = 0;       // 0: each sensor's stock passive trip; else the ceiling, capped at each sensor's stock passive trip
  int32_t releaseDeci = 0;   // 0: the trip - 5 C (a ceiling) or each sensor's passive trip - its hysteresis (the stock trips)
  int32_t cutDeci = 0;       // 0: each sensor's critical trip - marginDeci
  int32_t marginDeci = 100;
};

constexpr int32_t kNearDeci = 100;          // fast polling within 10 C of a trip
constexpr int32_t kCeilingDeci = 900;       // PianoThermal.cpp's default piano-cpufreq-hot (90 C)
constexpr int32_t kCeilingHystDeci = 50;    // its default release: 5 C under it

// One sensor's trip and release point under the trips T: the ceiling (never above the zone's stock passive trip) or the stock
// trip; the release point always under the trip.
inline int32_t sensorHot(const Sensor &sn, const ClusterTrips &t)
{
  return t.hotDeci && t.hotDeci < sn.passiveDeci ? t.hotDeci : sn.passiveDeci;
}
inline int32_t sensorRelease(const Sensor &sn, const ClusterTrips &t)
{
  const int32_t hot = sensorHot(sn, t);
  int32_t release = t.releaseDeci ? t.releaseDeci : t.hotDeci ? t.hotDeci - kCeilingHystDeci : sn.passiveDeci - sn.passiveHystDeci;
  if (release >= hot) release = hot - (sn.passiveHystDeci > 0 ? sn.passiveHystDeci : kCeilingHystDeci);
  return release;
}

inline ThermalInput clusterThermal(const Monitor &m, unsigned cluster, const ClusterTrips &t, bool critical)
{
  ThermalInput in{false, -100000, -100000, critical};
  if (cluster > 1) return in;
  const Controller &k = kMap[cluster];
  for (unsigned s = k.cpuFirst; s <= k.cpuLast && s < k.count; ++s) {
    if (!m.usable(cluster, s)) continue;
    const Sensor &sn = k.sensors[s];
    const int32_t d = m.deci(cluster, s);
    const int32_t hot = sensorHot(sn, t);
    const int32_t release = sensorRelease(sn, t);
    const int32_t cut = t.cutDeci ? t.cutDeci : sn.criticalDeci - t.marginDeci;
    in.valid = true;
    if (d - hot > in.hotExcessDeci) in.hotExcessDeci = d - hot;
    if (d - release > in.releaseExcessDeci) in.releaseExcessDeci = d - release;
    if (d >= cut) in.cut = true;
  }
  return in;
}

// Any usable sensor of the SoC within kNearDeci of its policy passive threshold (Tsens.h passiveOf), or of the cluster ceiling.
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
//   fast   a sensor within kNearDeci of its trip (80 C with the 90 C ceiling), a cluster thermally limited, or a cluster set above
//          the fast-poll clock (piano-thermal-fast-mhz: the clocks at which a prime-core load heats that fast) while the hottest
//          sensor is at piano-thermal-fast-temp (70 C) or more -> piano-thermal-fast-ms (10 ms);
//   clock  a cluster above the fast-poll clock with every sensor cooler than that -> piano-thermal-clock-ms (100 ms; the governor's
//          own tick, so no extra wakeup while it governs). At Ubuntu's ~220 C/s one such period adds at most ~22 C: from under
//          70 C that is under 92 C at the next poll, which is then fast, against the 90 C ceiling and the 105 C stock trip. Before
//          2026-11-12 this case was fast too: 100 wakeups a second whenever the governor sat above 2.5 GHz, at any temperature (P9);
//   warm   the hottest sensor at piano-thermal-warm-poll (70 C) or more -> 250 ms (or the base when shorter);
//   base   1 s (the governor's 100 ms tick when it runs).
// With piano-thermal-irq=1 the tsens upper-threshold interrupt stands in for the warm tier and the fast-clock rules at rest
// (TsensIrq.h, docs/drivers/thermal.md section 13).
enum class Poll : uint8_t { base, warm, clock, fast };
constexpr unsigned kPollTiers = 4;

inline const char *pollName(Poll p)
{
  return p == Poll::fast ? "fast" : p == Poll::clock ? "clock" : p == Poll::warm ? "warm" : "base";
}

inline Poll pollTier(const Monitor &m, const ClusterTrips &t, bool limited, bool clusterFast, int32_t warmDeci,
                     int32_t fastTempDeci = 700)
{
  if (limited || nearTrip(m, t)) return Poll::fast;
  int32_t d;
  unsigned c, s;
  const bool reading = m.hottestAll(d, c, s);
  if (clusterFast) return !reading || d >= fastTempDeci ? Poll::fast : Poll::clock;
  if (reading && d >= warmDeci) return Poll::warm;
  return Poll::base;
}

inline uint32_t pollPeriodMs(Poll p, uint32_t baseMs, uint32_t warmMs, uint32_t fastMs, uint32_t clockMs = 100)
{
  if (p == Poll::fast) return fastMs < baseMs ? fastMs : baseMs;
  if (p == Poll::clock) return clockMs < baseMs ? clockMs : baseMs;
  if (p == Poll::warm) return warmMs < baseMs ? warmMs : baseMs;
  return baseMs;
}

}  // namespace pianothermal

#endif
