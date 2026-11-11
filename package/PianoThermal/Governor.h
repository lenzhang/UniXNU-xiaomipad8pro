// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: the CPU frequency policy of one SCMI performance domain (cluster 0 = cpu0-5, cluster 1 = cpu6-7), one decision per
// tick. Portable (levels as kHz, slowest first, as scmi::Domain keeps them); host-tested by tests/thermal_host.cpp.
//
// piano-cpufreq=2 (fixed): the domain sits at the fastest level <= its cap (piano-cpufreq-cap0/cap1 MHz) and <= its sustained
//   level; only the thermal limit below moves it. The A/B "battery level" against PianoPower's 30 s battery current.
// piano-cpufreq=3 (governor), per tick (piano-cpufreq-tick-ms, 100 ms):
//   busy   the busiest core of the cluster over the tick, per mille (XNU's per-processor idle time, PianoThermal.cpp);
//   want   the slowest level >= current kHz x busy / target (target 80 %: schedutil's 1.25 margin); at >= 90 % busy at least the
//          hispeed level (piano-cpufreq-hispeed0/1 MHz) so a burst does not climb 1.25x per tick from the floor; an idle cluster
//          (busy ~0) gets the slowest level;
//   up     at once; down only after downTicks ticks in a row that wanted less, to the fastest of what those ticks wanted;
//   ceiling min(sustained, piano-cpufreq-cap0/1, the power cap: piano-cpufreq-bat0/1 on battery or when the power source is
//          unknown, piano-cpufreq-ac0/1 on external power, the thermal limit); floor piano-cpufreq-min0/1. A ceiling that drops
//          applies at once.
// thermal limit (both modes): the cluster's hottest sensor >= hot (85.0 C) -> one level down per tick; < release (80.0 C) -> one
//   level up per riseTicks ticks; in between held; >= cut (95.0 C), no reading, or PianoThermal's protection state passive or
//   critical -> the slowest level at once. Stock passive is 105 C: this acts well before it.
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
  int32_t hotDeci = 850, releaseDeci = 800, cutDeci = 950;
  uint32_t riseTicks = 10;
};

struct TickInput {
  uint32_t busyPermille;   // governor only
  PowerSource power;
  bool thermalValid;
  int32_t thermalDeci;     // the cluster's hottest sensor
  bool protect;            // PianoThermal's state is passive or critical
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
    lowTicks_ = coolTicks_ = 0;
    pending_ = 0;
  }

  // One tick: the level the domain should be at (the caller sends LEVEL_SET only when it differs from what it set last).
  unsigned tick(const TickInput &in)
  {
    updateLimit(in);
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

  unsigned ceiling(PowerSource power) const
  {
    unsigned top = sustained_;
    auto clampTo = [&](uint32_t khz) {
      if (!khz) return;
      const unsigned l = fastestAtMost(khz);
      if (l < top) top = l;
    };
    clampTo(config_.capKhz);
    clampTo(power == PowerSource::external ? config_.externalKhz : config_.batteryKhz);
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
    return count_ - 1;
  }

 private:
  void updateLimit(const TickInput &in)
  {
    const unsigned top = count_ - 1;
    if (!in.thermalValid || in.protect || in.thermalDeci >= config_.cutDeci) {
      limit_ = 0;
      coolTicks_ = 0;
    } else if (in.thermalDeci >= config_.hotDeci) {
      if (limit_ > 0) --limit_;
      coolTicks_ = 0;
    } else if (in.thermalDeci < config_.releaseDeci && limit_ < top) {
      if (++coolTicks_ >= config_.riseTicks) {
        ++limit_;
        coolTicks_ = 0;
      }
    } else {
      coolTicks_ = 0;
    }
  }

  static constexpr unsigned kMax = 32;
  CpuMode mode_ = CpuMode::off;
  GovernorConfig config_{};
  uint32_t khz_[kMax] = {};
  uint8_t count_ = 1, sustained_ = 0, level_ = 0, limit_ = 0, pending_ = 0;
  uint32_t lowTicks_ = 0, coolTicks_ = 0;
};

// Per mille busy of one core over a window: wall time minus its idle time (both in the same unit), clamped.
inline uint32_t busyPermille(uint64_t wall, uint64_t idle)
{
  if (!wall) return 0;
  if (idle >= wall) return 0;
  return uint32_t((wall - idle) * 1000 / wall);
}

}  // namespace pianothermal

#endif
