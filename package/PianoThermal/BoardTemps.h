// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal B6: the snapshot of a round that the ADC thread publishes, and package/include/PianoBoardTemps.h's answer built from
// it (which channel is which role, validity, age). Portable: the kext (PianoThermalAdc.cpp, which adds the sequence count) and
// tests/adc_host.cpp include it. docs/drivers/thermal.md section 12.
#ifndef PIANO_THERMAL_BOARD_TEMPS_H
#define PIANO_THERMAL_BOARD_TEMPS_H
#include <stdint.h>
#include "PianoBoardTemps.h"
#include "PmicAdc.h"

namespace pianothermal::adc {

constexpr uint32_t kFreshPeriods = 3;   // a reading counts for three periods (a round may be late by a slow conversion or two)

struct Snapshot {
  uint32_t state = PIANO_BOARD_TEMPS_OFF, rounds = 0, periodMs = 0, validMask = 0, disabledMask = 0;
  int32_t milliC[kChannelCount] = {};
  uint64_t atUs[kChannelCount] = {};
};

inline Snapshot snapshotOf(const Board &b, uint32_t periodMs)
{
  Snapshot s;
  s.state = b.adc.stopped != Err::none ? PIANO_BOARD_TEMPS_STOPPED : b.rounds ? PIANO_BOARD_TEMPS_RUNNING : PIANO_BOARD_TEMPS_STARTING;
  s.rounds = b.rounds;
  s.periodMs = periodMs;
  for (unsigned i = 0; i < kChannelCount; ++i) {
    const ChannelState &c = b.ch[i];
    if (c.valid) {
      s.validMask |= 1u << i;
      s.milliC[i] = c.last.milliC;
      s.atUs[i] = c.atUs;
    }
    if (!c.enabled) s.disabledMask |= 1u << i;
  }
  return s;
}

// The answer for the charging roles (PIANO_BOARD_TEMP_BATTERY.. in Role order). Returns the valid count, -1 for a bad argument.
inline int fillBoardTemps(const Snapshot &s, uint64_t nowUs, piano_board_temps *t)
{
  if (!t || t->version != PIANO_BOARD_TEMPS_VERSION) return -1;
  t->state = s.state;
  t->valid = t->disabled = 0;
  t->period_ms = s.periodMs;
  t->rounds = s.rounds;
  int n = 0;
  for (unsigned r = 0; r < PIANO_BOARD_TEMP_COUNT; ++r) {
    t->milli_c[r] = 0;
    t->age_ms[r] = 0;
    const int i = roleIndex(Role(r));
    if (i < 0) continue;
    if (s.disabledMask & (1u << i)) t->disabled |= 1u << r;
    if (!(s.validMask & (1u << i)) || s.state == PIANO_BOARD_TEMPS_STOPPED) continue;
    const uint64_t ageMs = nowUs > s.atUs[i] ? (nowUs - s.atUs[i]) / 1000 : 0;
    if (ageMs > uint64_t(kFreshPeriods) * s.periodMs) continue;
    t->valid |= 1u << r;
    t->milli_c[r] = s.milliC[i];
    t->age_ms[r] = uint32_t(ageMs);
    ++n;
  }
  return n;
}

static_assert(int(Role::battery) == PIANO_BOARD_TEMP_BATTERY && int(Role::charger) == PIANO_BOARD_TEMP_CHARGER &&
                  int(Role::connector) == PIANO_BOARD_TEMP_CONNECTOR && int(Role::connector2) == PIANO_BOARD_TEMP_CONNECTOR2,
              "the API's indices are the roles'");

}  // namespace pianothermal::adc
#endif
