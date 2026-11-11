// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal level 2: the protection state machine over a Monitor poll (Tsens.h). Portable; host-tested by tests/thermal_host.cpp.
//
// States, worst sensor wins, each entered at its threshold and left only below threshold - hysteresis:
//   warm      any sensor >= piano-thermal-warm (default 85.0 C; hysteresis 5 C): logged, nothing else;
//   passive   any sensor >= its stock passive trip (105.0 or 118.0 C; zones without one: 105.0 C, Tsens.h passiveOf), at most 5 C
//             under the critical threshold below (so 110.0 C for the 118 C zones with the default margin), with the zone's
//             hysteresis (5 or 8 C): the GPU is parked at its lowest level for the rest of the boot (adreno830_dcvs_park), the
//             CPU governor (piano-cpufreq >= 2) takes its slowest levels;
//   critical  any sensor >= its stock critical trip (125.0 C) - piano-thermal-margin (default 10 C, so 115.0 C) on every update
//             for criticalHoldMs (1 s) and at least two updates in a row (a single wild reading is not a shutdown; hysteresis 5 C):
//             logged loudly; with piano-thermal-shutdown=1 the kext halts the system (PianoThermal.cpp).
//   unknown   (a flag next to the state) no usable sensor at all for unknownMs (5 s) and two updates: logged loudly; the state is
//             kept.
// Time-based, so the update rate may change (PianoThermal.cpp updates every 1 s, every 20 ms while a sensor is near its trip).
#ifndef PIANO_THERMAL_POLICY_H
#define PIANO_THERMAL_POLICY_H
#include "Tsens.h"

namespace pianothermal {

enum class State : uint8_t { normal, warm, passive, critical };

inline const char *stateName(State s)
{
  switch (s) {
    case State::normal: return "normal";
    case State::warm: return "warm";
    case State::passive: return "PASSIVE";
    case State::critical: return "CRITICAL";
  }
  return "?";
}

struct PolicyConfig {
  int32_t warmDeci = 850, warmHystDeci = 50;
  int32_t criticalMarginDeci = 100, criticalHystDeci = 50;
  uint32_t criticalHoldMs = 1000;
  uint32_t unknownMs = 5000;
};

struct PolicyEvent {
  State before, after;
  bool unknownBefore, unknownAfter;
  bool criticalPending;     // the critical threshold is met but not yet for criticalHoldMs
  // the sensor that decided (the hottest relative to the threshold of the resulting state, or the hottest overall)
  unsigned controller, sensor;
  int32_t deci;
  bool changed() const { return before != after || unknownBefore != unknownAfter; }
};

class Policy {
 public:
  explicit Policy(const PolicyConfig &config = PolicyConfig()) : config_(config) {}

  PolicyEvent update(const Monitor &m, uint64_t nowMs)
  {
    PolicyEvent e{state_, state_, unknown_, unknown_, false, 0, 0, 0};
    if (!m.anyUsable()) {
      if (!noReading_) noReadingSinceMs_ = nowMs;
      if (noReading_ < 0xffffffffu) ++noReading_;
      if (noReading_ >= 2 && nowMs - noReadingSinceMs_ >= config_.unknownMs) unknown_ = true;
      criticalStreak_ = 0;
      e.after = state_;
      e.unknownAfter = unknown_;
      return e;
    }
    noReading_ = 0;
    unknown_ = false;
    unsigned hc = 0, hs = 0;
    int32_t hd = 0;
    m.hottestAll(hd, hc, hs);
    e.controller = hc;
    e.sensor = hs;
    e.deci = hd;

    // Which levels are entered and which are held.
    bool enter[4] = {true, false, false, false}, hold[4] = {true, false, false, false};
    unsigned who[4][2] = {};
    for (unsigned c = 0; c < kControllers; ++c)
      for (unsigned s = 0; s < kMap[c].count; ++s) {
        if (!m.usable(c, s)) continue;
        const int32_t t = m.deci(c, s);
        const Sensor &sn = kMap[c].sensors[s];
        const int32_t critical = sn.criticalDeci - config_.criticalMarginDeci;
        // passive at most 5 C under the critical threshold: the 118 C zones' passive would otherwise come after it
        const int32_t passive = passiveOf(sn) < critical - 50 ? passiveOf(sn) : critical - 50;
        const int32_t thr[4] = {0, config_.warmDeci, passive, critical};
        const int32_t hyst[4] = {0, config_.warmHystDeci, passiveHystOf(sn), config_.criticalHystDeci};
        for (unsigned k = 1; k < 4; ++k) {
          if (t >= thr[k] && !enter[k]) {
            enter[k] = true;
            who[k][0] = c;
            who[k][1] = s;
          }
          if (t >= thr[k] - hyst[k]) hold[k] = true;
        }
      }
    // critical needs criticalHoldMs (and two updates) in a row at its threshold
    if (enter[3]) {
      if (!criticalStreak_) criticalSinceMs_ = nowMs;
      if (criticalStreak_ < 0xffffffffu) ++criticalStreak_;
      if ((criticalStreak_ < 2 || nowMs - criticalSinceMs_ < config_.criticalHoldMs) && state_ != State::critical) {
        enter[3] = false;
        e.criticalPending = true;
      }
    } else {
      criticalStreak_ = 0;
    }
    unsigned next = 0;
    for (unsigned k = 3; k > 0; --k)
      if (enter[k]) {
        next = k;
        break;
      }
    for (unsigned k = unsigned(state_); k > next; --k)
      if (hold[k]) {
        next = k;
        break;
      }
    state_ = State(next);
    if (next && enter[next]) {
      e.controller = who[next][0];
      e.sensor = who[next][1];
      e.deci = m.deci(e.controller, e.sensor);
    }
    e.after = state_;
    e.unknownAfter = unknown_;
    if (e.after > peak_) peak_ = e.after;
    return e;
  }

  State state() const { return state_; }
  bool unknown() const { return unknown_; }
  State peak() const { return peak_; }
  const PolicyConfig &config() const { return config_; }

 private:
  PolicyConfig config_;
  State state_ = State::normal, peak_ = State::normal;
  bool unknown_ = false;
  uint32_t noReading_ = 0, criticalStreak_ = 0;
  uint64_t noReadingSinceMs_ = 0, criticalSinceMs_ = 0;
};

}  // namespace pianothermal

#endif
