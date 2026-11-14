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
//   step_wise (Cluster.h), since 2026-11-12 on a hard ceiling under the stock trips: any of the cluster's cpu-* sensors at or over
//   piano-cpufreq-hot (default 90 C; never above the zone's stock passive trip, 105 C or 118 C; 0 = the stock trips)
//   -> one level down per stepMs (10 ms, Linux's passive polling); all under the release point (piano-cpufreq-release, default
//   5 C under the ceiling) -> one level up per riseMs (1 s); in between
//   held; at the critical threshold (125 C - piano-thermal-margin), without a reading, or in PianoThermal's critical state ->
//   the slowest level at once. The ceiling holds at every piano-thermal level (piano-cpufreq implies the level 1 readout).
// busy excludes PianoThermal's own work (busyPermilleExcluding); a window with the per-core clock measurement in it is dropped.
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

// PianoPolicy's power profile (docs/policy.md 5.2, the platform function "piano-thermal-profile"): 0 the caps by the power source
// (as without PianoPolicy), 1 Power Saver whatever the source (the battery caps, -bat0/-bat1), 2 Balanced whatever the source (the
// external-power caps, -ac0/-ac1). The governor (mode 3) is handed profilePower() instead of the measured source; the fixed level
// (mode 2) has no power caps and so ignores it.
enum class Profile : uint8_t { bySource = 0, saver = 1, balanced = 2 };
inline bool profileValid(uintptr_t mode) { return mode <= 2; }
inline PowerSource profilePower(PowerSource measured, Profile p)
{
  return p == Profile::saver ? PowerSource::battery : p == Profile::balanced ? PowerSource::external : measured;
}
inline const char *profileName(Profile p)
{
  return p == Profile::saver ? "Power Saver" : p == Profile::balanced ? "Balanced" : "by the power source";
}

// A system sleep (docs/power.md 4, PianoPowerContract.h): off parks the thread (no SCMI request, no TSENS read in flight) and, with
// piano-thermal-sleep-mask=1, masks the threshold interrupts (piano_thermal_suspend_prepare); on puts back what that off masked
// (piano_thermal_suspend_finish), then the thread runs again (its first zone update follows at once). A second off, or an on
// without an off (the power tree's first on), does nothing; a prepare that answered -1 (another caller prepared first) is not
// finished here.
struct SleepSteps {
  bool park = false, mask = false, finish = false, unpark = false;
};
class SleepPlan {
 public:
  explicit SleepPlan(bool mask = false) : mask_(mask) {}
  SleepSteps off()
  {
    SleepSteps s;
    if (asleep_) return s;
    asleep_ = true;
    masked_ = mask_;
    s.park = true;
    s.mask = mask_;
    return s;
  }
  void maskResult(int prepared)
  {
    if (prepared < 0) masked_ = false;
  }
  SleepSteps on()
  {
    SleepSteps s;
    if (!asleep_) return s;
    asleep_ = false;
    s.finish = masked_;
    s.unpark = true;
    masked_ = false;
    ++wakes_;
    return s;
  }
  bool asleep() const { return asleep_; }
  uint32_t wakes() const { return wakes_; }

 private:
  bool mask_, asleep_ = false, masked_ = false;
  uint32_t wakes_ = 0;
};

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
  // piano-cpufreq-boost=S (2026-11-14, boot time): the level while the boot boost lasts, in every mode (report included): the
  // sustained level, or the fastest at or under capKhz when one is given (piano-cpufreq-boost-cap), never above the thermal limit.
  unsigned boostLevel(uint32_t capKhz) const
  {
    unsigned top = sustained_;
    if (capKhz) {
      const unsigned l = fastestAtMost(capKhz);
      if (l < top) top = l;
    }
    return limit_ < top ? limit_ : top;
  }
  unsigned sustained() const { return sustained_; }
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

// The boot boost (piano-cpufreq-boost=S) of one domain at a poll at uptime nowMs: boost (send boostLevel) until endMs, then end
// once (report mode back to the level the boot stages left, the other modes on from where the domain is), then none. endMs 0: off.
enum class BoostStep : uint8_t { none, boost, end };
inline BoostStep boostStep(uint64_t nowMs, uint64_t endMs, bool ended)
{
  if (!endMs || ended) return BoostStep::none;
  return nowMs < endMs ? BoostStep::boost : BoostStep::end;
}

// Per mille busy of one core over a window: wall time minus its idle time and minus the time PianoThermal's own thread ran on that
// core in the window (its sensor polls, SCMI waits and load reads: P9 of 2026-11-12, the governor must not count its own work as
// load), all in the same unit, clamped.
inline uint32_t busyPermilleExcluding(uint64_t wall, uint64_t idle, uint64_t self)
{
  if (!wall) return 0;
  if (idle >= wall) return 0;
  const uint64_t used = wall - idle;
  if (self >= used) return 0;
  return uint32_t((used - self) * 1000 / wall);
}
inline uint32_t busyPermille(uint64_t wall, uint64_t idle) { return busyPermilleExcluding(wall, idle, 0); }

// One core's load window between two readings (PianoThermal.cpp domainBusy): its idle time and PianoThermal's own time on it, both
// cumulative, in ns. read() returns the window's busy per mille with the own time left out, or -1 when there is no window: the
// first reading, a reading after a failed one, or one with rebase set (the per-core clock measurement ran on every core in the
// window, so the window is dropped and the reading only starts the next).
struct CoreLoad {
  uint64_t idleNs = 0, selfNs = 0, atNs = 0;
  bool ok = false;
  int32_t read(uint64_t nowNs, uint64_t idleNow, uint64_t selfNow, bool rebase)
  {
    int32_t busy = -1;
    if (ok && !rebase && nowNs > atNs) {
      const uint64_t di = idleNow >= idleNs ? idleNow - idleNs : 0;
      const uint64_t ds = selfNow >= selfNs ? selfNow - selfNs : 0;
      busy = int32_t(busyPermilleExcluding(nowNs - atNs, di, ds));
    }
    idleNs = idleNow;
    selfNs = selfNow;
    atNs = nowNs;
    ok = true;
    return busy;
  }
  void fail() { ok = false; }
};

// A cap in MHz as the kHz it admits: the whole MHz, so "1785" admits the 1785.6 MHz level.
inline uint32_t capKhz(uint32_t mhz) { return mhz ? mhz * 1000u + 999u : 0; }

}  // namespace pianothermal

#endif
