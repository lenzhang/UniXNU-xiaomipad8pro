// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: the CPU frequency policy of one SCMI performance domain (cluster 0 = cpu0-5, cluster 1 = cpu6-7). Portable (levels as
// kHz, slowest first, as scmi::Domain keeps them); host-tested by tests/thermal_host.cpp.
//
// piano-cpufreq=2 (fixed): the domain sits at the fastest level <= its cap (piano-cpufreq-cap0/cap1, default Ubuntu's Power Saver
//   profile: 1785.6 / 1958.4 MHz) and <= its sustained level; only the thermal limit moves it. The A/B "battery level" against
//   PianoPower's 30 s battery current.
// piano-cpufreq=3 (governor), one load decision per tick (piano-cpufreq-tick-ms, 100 ms):
//   busy    the busiest core of the cluster over the tick, per mille (XNU's per-processor idle time, PianoThermal.cpp);
//   want    the slowest level >= current kHz x busy / target (target 80 %: schedutil's 1.25 margin); at >= 90 % busy at least the
//           hispeed level (piano-cpufreq-hispeed0/1) so a burst does not climb 1.25x per tick from the floor; an idle cluster
//           (busy ~0) gets the slowest level;
//   up      at once; down only after downTicks ticks in a row that wanted less, to the fastest of what those ticks wanted;
//   ceiling min(sustained (Linux: boost off), piano-cpufreq-cap0/1 when given, the power cap: Ubuntu's Power Saver caps on battery
//           or when the power source is unknown (piano-cpufreq-bat0/1), its Balanced caps on external power (-ac0/1: 3532.8 /
//           4089.6 MHz), the thermal limit); floor piano-cpufreq-min0/1. A ceiling that drops applies at once.
// thermal limit (both modes; thermal() is called on every sensor poll, 10 ms apart while a sensor is near its trip): Linux's
//   step_wise on the stock trips (Cluster.h): any of the cluster's cpu-* sensors at or over its passive trip (105 C, or 118 C)
//   -> one level down per stepMs (10 ms, Linux's passive polling); all under passive - hysteresis (5 or 8 C) -> one level up per
//   riseMs (1 s); in between
//   held; at the critical threshold (125 C - piano-thermal-margin), without a reading, or in PianoThermal's critical state ->
//   the slowest level at once.
#ifndef PIANO_THERMAL_GOVERNOR_H
#define PIANO_THERMAL_GOVERNOR_H
#include <stdint.h>

namespace pianothermal {

enum class CpuMode : uint8_t { off = 0, report = 1, fixed = 2, govern = 3 };
enum class PowerSource : uint8_t { unknown, battery, external };

inline const char *powerName(PowerSource p)
{
  return p == PowerSource::external ? "on power" : p == PowerSource::battery ? "on battery" : "power unknown (battery caps)";
}

struct GovernorConfig {
  uint32_t capKhz = 0;          // 0: none (fixed mode needs one; PianoThermal.cpp gives the default)
  uint32_t batteryKhz = 0, externalKhz = 0;   // 0: none
  uint32_t floorKhz = 0;
  uint32_t hispeedKhz = 0;      // 0: no hispeed jump
  uint32_t targetPermille = 800, hispeedPermille = 900;
  uint32_t downTicks = 3;
  uint32_t stepMs = 10, riseMs = 1000;   // PianoThermal.cpp sets stepMs to piano-thermal-fast-ms
};

struct TickInput {
  uint32_t busyPermille;   // governor only
  PowerSource power;
};

// The cluster's temperature against its trips (Cluster.h computes it from a poll).
struct ThermalInput {
  bool valid;                // at least one of the cluster's sensors has a usable reading
  int32_t hotExcessDeci;     // max over the sensors of reading - passive trip: >= 0 is at or over the trip
  int32_t releaseExcessDeci; // max over the sensors of reading - (passive trip - hysteresis): < 0 is under the release point
  bool cut;                  // a sensor at the critical threshold, or PianoThermal's critical state
};

class Governor {
 public:
  // levels: kHz, slowest first; count >= 1; sustained: the index the firmware calls sustained; boot: the index found at start.
  void init(CpuMode mode, const GovernorConfig &config, const uint32_t *khz, unsigned count, unsigned sustained, unsigned boot)
  {
    mode_ = mode;
    config_ = config;
    count_ = uint8_t(count > kMax ? kMax : count ? count : 1);
    for (unsigned i = 0; i < count_; ++i) khz_[i] = khz[i];
    sustained_ = uint8_t(sustained < count_ ? sustained : count_ - 1u);
    level_ = uint8_t(boot < count_ ? boot : 0);
    limit_ = uint8_t(count_ - 1);
    lowTicks_ = 0;
    pending_ = 0;
    downAtMs_ = upAtMs_ = 0;
    coolSinceMs_ = 0;
    cooling_ = false;
  }

  // The thermal limit, from one poll of the sensors at nowMs. Returns true when the limit changed.
  bool thermal(const ThermalInput &in, uint64_t nowMs)
  {
    const uint8_t before = limit_;
    const unsigned top = count_ - 1u;
    if (!in.valid || in.cut) {
      limit_ = 0;
      cooling_ = false;
    } else if (in.hotExcessDeci >= 0) {
      if (limit_ > 0 && (!downAtMs_ || nowMs - downAtMs_ >= config_.stepMs)) {
        --limit_;
        downAtMs_ = nowMs;
      }
      cooling_ = false;
    } else if (in.releaseExcessDeci < 0 && limit_ < top) {
      if (!cooling_) {
        cooling_ = true;
        coolSinceMs_ = nowMs;
      } else if (nowMs - coolSinceMs_ >= config_.riseMs) {
        ++limit_;
        coolSinceMs_ = nowMs;
      }
    } else {
      cooling_ = false;
    }
    return limit_ != before;
  }

  // One load decision (governor), or the fixed level (fixed); report mode never moves. The level the domain should be at (the caller
  // sends LEVEL_SET only when it differs from what it set last).
  unsigned tick(const TickInput &in)
  {
    const unsigned top = ceiling(in.power);
    const unsigned bottom = floorLevel() < top ? floorLevel() : top;
    if (mode_ == CpuMode::fixed) {
      level_ = uint8_t(top);
      return level_;
    }
    if (mode_ != CpuMode::govern) return level_;
    const uint64_t need = uint64_t(khz_[level_]) * in.busyPermille / (config_.targetPermille ? config_.targetPermille : 800);
    unsigned want = slowestAtLeast(uint32_t(need > 0xffffffffu ? 0xffffffffu : need));
    if (in.busyPermille >= config_.hispeedPermille && config_.hispeedKhz) {
      const unsigned hs = slowestAtLeast(config_.hispeedKhz);
      if (want < hs) want = hs;
    }
    if (want < bottom) want = bottom;
    if (want > top) want = top;
    if (want > level_) {
      level_ = uint8_t(want);
      lowTicks_ = 0;
      pending_ = 0;
    } else if (want < level_) {
      if (!lowTicks_ || want > pending_) pending_ = uint8_t(want);
      if (++lowTicks_ >= config_.downTicks) {
        level_ = pending_;
        lowTicks_ = 0;
        pending_ = 0;
      }
    } else {
      lowTicks_ = 0;
      pending_ = 0;
    }
    if (level_ > top) level_ = uint8_t(top);
    return level_;
  }

  // Between load decisions: the level, lowered at once to a ceiling that dropped (a thermal step, the power source); fixed mode
  // also follows a ceiling that rose. Report mode never moves.
  unsigned enforce(PowerSource power)
  {
    if (mode_ == CpuMode::fixed) return tick({0, power});
    if (mode_ != CpuMode::govern) return level_;
    const unsigned top = ceiling(power);
    if (level_ > top) level_ = uint8_t(top);
    return level_;
  }

  unsigned ceiling(PowerSource power) const
  {
    unsigned top = sustained_;
    auto clampTo = [&](uint32_t khz) {
      if (!khz) return;
      const unsigned l = fastestAtMost(khz);
      if (l < top) top = l;
    };
    clampTo(config_.capKhz);
    if (mode_ == CpuMode::govern) clampTo(power == PowerSource::external ? config_.externalKhz : config_.batteryKhz);
    if (limit_ < top) top = limit_;
    return top;
  }
  unsigned floorLevel() const { return config_.floorKhz ? slowestAtLeast(config_.floorKhz) : 0; }
  unsigned level() const { return level_; }
  unsigned limit() const { return limit_; }
  unsigned count() const { return count_; }
  uint32_t khz(unsigned i) const { return i < count_ ? khz_[i] : 0; }
  CpuMode mode() const { return mode_; }
  const GovernorConfig &config() const { return config_; }
  // Force the bookkeeping to what the domain really is at (a refused or failed LEVEL_SET, a LEVEL_GET that disagrees).
  void setLevel(unsigned i)
  {
    if (i < count_) level_ = uint8_t(i);
  }

  unsigned fastestAtMost(uint32_t khz) const
  {
    unsigned found = 0;
    for (unsigned i = 0; i < count_; ++i)
      if (khz_[i] <= khz) found = i;
    return found;
  }
  unsigned slowestAtLeast(uint32_t khz) const
  {
    for (unsigned i = 0; i < count_; ++i)
      if (khz_[i] >= khz) return i;
    return count_ - 1u;
  }

 private:
  static constexpr unsigned kMax = 32;
  CpuMode mode_ = CpuMode::off;
  GovernorConfig config_{};
  uint32_t khz_[kMax] = {};
  uint8_t count_ = 1, sustained_ = 0, level_ = 0, limit_ = 0, pending_ = 0;
  uint32_t lowTicks_ = 0;
  uint64_t downAtMs_ = 0, upAtMs_ = 0, coolSinceMs_ = 0;
  bool cooling_ = false;
};

// Per mille busy of one core over a window: wall time minus its idle time (both in the same unit), clamped.
inline uint32_t busyPermille(uint64_t wall, uint64_t idle)
{
  if (!wall) return 0;
  if (idle >= wall) return 0;
  return uint32_t((wall - idle) * 1000 / wall);
}

// A cap in MHz as the kHz it admits: the whole MHz, so "1785" admits the 1785.6 MHz level.
inline uint32_t capKhz(uint32_t mhz) { return mhz ? mhz * 1000u + 999u : 0; }

}  // namespace pianothermal

#endif
