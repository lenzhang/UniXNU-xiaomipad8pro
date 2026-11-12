// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera's plan: several of docs/drivers/camera.md section 8.1's test images on one boot. piano-camera-plan=NAME,NAME,...
// names them (c1b c2f c2r c2rg c2r24 c3rp c3r c3rs c3f, at most kPlanMax); after level 1's survey each runs with exactly its
// image's arguments (level, sensor, stop, pattern, frames, rear bus, MCLK), in the order given, and the next one starts only
// when the one before ended with its power-down complete (everything as found) and, at level 3, its buffer freed or never
// allocated: the next sequence then starts from the state the sequences expect, and a sequence that takes the system down stops
// the rest after everything before it was logged (so the riskier ones go last). The plan's other arguments (piano-camera-delay,
// cci-khz, i2c-half-us, pull, blind, hold, votes, mmcx-floor, gain, timeout-ms, cycles, the survey's parts) apply to every item.
#pragma once
#include <stdint.h>
#include <string.h>

namespace pianocamera {

struct PlanItem {
  const char *name;
  uint8_t level;     // 2: power, chip ID, power down; 3: frames
  uint8_t sensor;    // 1 rear, 2 front
  uint8_t rearBus;   // 1: the rear over software I2C (c2rg)
  uint8_t mclk24;    // the rear's MCLK at 24 MHz (level 3's default)
  uint8_t pattern;   // level 3: the rear's colour bars (1) or the scene (0)
  uint8_t frames;    // level 3
  uint16_t stop;     // piano-camera-stop (0: none)
};

// The images' arguments (tooling/k27/build-camera-test-images.sh), with the defaults readArgs gives the rest.
// (c2fx, the front on the minimal path, is gone: answered on 2026-11-13, camera.md 4.2 "After c2fx".)
constexpr PlanItem kPlanItems[] = {
    {"c1b", 2, 1, 0, 0, 1, 2, 203},  {"c2f", 2, 2, 0, 0, 1, 2, 0},  {"c2r", 2, 1, 0, 0, 1, 2, 0},    {"c2rg", 2, 1, 1, 0, 1, 2, 0},
    {"c2r24", 2, 1, 0, 1, 1, 2, 0},  {"c3rp", 3, 1, 0, 1, 1, 2, 312}, {"c3r", 3, 1, 0, 1, 1, 2, 0}, {"c3rs", 3, 1, 0, 1, 0, 4, 0},
    {"c3f", 3, 2, 0, 1, 1, 2, 0},
};
constexpr unsigned kPlanMax = 8;

inline const PlanItem *planItem(const char *name, size_t len)
{
  for (const PlanItem &p : kPlanItems)
    if (strlen(p.name) == len && !strncmp(p.name, name, len)) return &p;
  return nullptr;
}

struct Plan {
  const PlanItem *item[kPlanMax] = {};
  unsigned count = 0;
  int error = 0;          // 0, -1 an unknown name (at `bad`), -2 more than kPlanMax, -3 empty
  unsigned bad = 0;       // the unknown name's offset in the string
  uint8_t level = 0;      // the highest level named (piano-camera's level for probe and the power tree)
};

// NAME[,NAME...]; a name may repeat (two runs of the same image). Spaces end the string (a boot argument has none).
inline Plan parsePlan(const char *s)
{
  Plan p;
  if (!s || !*s) {
    p.error = -3;
    return p;
  }
  for (size_t at = 0;;) {
    size_t end = at;
    while (s[end] && s[end] != ',' && s[end] != ' ') ++end;
    const PlanItem *it = planItem(s + at, end - at);
    if (!it) {
      p.error = -1;
      p.bad = unsigned(at);
      p.count = 0;
      return p;
    }
    if (p.count == kPlanMax) {
      p.error = -2;
      p.count = 0;
      return p;
    }
    p.item[p.count++] = it;
    if (it->level > p.level) p.level = it->level;
    if (s[end] != ',') break;
    at = end + 1;
  }
  return p;
}

// Whether the plan goes on after an item (see the top): its power-down complete and no buffer kept.
inline bool planGoesOn(bool cleanOff, bool bufferKept) { return cleanOff && !bufferKept; }

}  // namespace pianocamera
