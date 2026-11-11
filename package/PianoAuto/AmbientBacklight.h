// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// PianoBacklight's automatic mode, for PianoBacklight's owner to take in (docs/drivers/sensors.md section 5a, "PianoBacklight").
// Header only, portable C++ as Ktz8866.h (no kernel header, no allocation, integer only); host-tested against PianoBacklight's own
// step ladder (tests/ambient_host.cpp). Nothing here touches an IC: it only says which 11-bit code the backlight should have.
//
// The targets come from PianoSensors' kext (AutoLight.h: SensorsPolicy's curve, smoothing and hysteresis; at most one per 250 ms,
// none at rest) through callPlatformFunction(kFunctionAmbient, false, (void *)(intptr_t)permille, (void *)source, (void *)(uintptr_t)
// lux, nullptr). A target is permille of the code range (PsCurvePoint's unit: 1000 = 2047). The user's brightness keys keep working
// in automatic mode: each press shifts the curve by one of PianoBacklight's own steps (linear mapping: x 2^(1/3), three per doubling,
// the Steps ladder's factor; exponential mapping: 1/16 of the range), and the shift (the offset) stays for the rest of the boot, so
// "a little darker than automatic" holds as the room changes. A press always changes what is shown unless the code is at an end
// (the floor, full scale): an offset that the clamp at the floor would hide is skipped over.
//
// Modes: off (piano-backlight-auto not 1: targets are refused, PianoBacklight behaves as today); manual (allowed, but no target yet,
// the sensor left with -1, or the user chose a level: piano-backlight-percent, or piano-backlight-auto=0 as a property); automatic.
// A target enters automatic mode unless the user chose manual; the property piano-backlight-auto=1 re-enters it (the last target
// at once, the offset kept).
//
// Integration in Ktz8866.h's Controller (three calls, PianoBacklight's owner):
//   pianoauto::Ambient ambient_;                         // start(): ambient_.configure(cfg_.autoAllowed, cfg_.floor, exponential_,
//                                                        //                            int(steps_.count()) - 1);
//   void ambient(int32_t permille, uint32_t lux) { if (control_ && ambient_.target(permille, lux)) { code_ = ambient_.code(); apply(); } }
//   step(delta, source): if (ambient_.step(delta)) { code_ = ambient_.code(); ...log...; apply(); return; }   // else as today
//   percent(p, source):  ambient_.manual(); ...as today
// apply() already writes only a changed code, and nothing while the backlight is off (display asleep, lid closed): the newest code
// is the one the wake lights.
#ifndef PIANO_AUTO_AMBIENT_BACKLIGHT_H
#define PIANO_AUTO_AMBIENT_BACKLIGHT_H
#include <stdint.h>

namespace pianoauto {

// The interface PianoBacklight adds (its owner mirrors these names in PianoBacklightClient.h).
constexpr const char *kFunctionAmbient = "PianoBacklightAmbient";   // p1 permille (intptr_t, -1 leaves), p2 source, p3 lux (uintptr_t)
constexpr const char *kArgAuto = "piano-backlight-auto";            // boot argument (1: targets taken) and setProperties key (0/1)
constexpr const char *kPropertyAuto = "piano-backlight-auto";       // published: automatic mode now (boolean)
constexpr const char *kPropertyOffset = "piano-backlight-offset";   // published: the user's shift in steps (signed)
constexpr const char *kPropertyTarget = "piano-backlight-ambient-permille";
constexpr const char *kPropertyLux = "piano-backlight-ambient-lux";
constexpr uint16_t kAmbientFullScale = 2047;

enum class Mode : uint8_t { off, manual, automatic };

class Ambient {
 public:
  // allowed: piano-backlight-auto=1; floor: piano-backlight-min; exponential: BL_CFG1's mapping; maxOffset: the ladder's length - 1.
  void configure(bool allowed, uint16_t floor, bool exponential, int maxOffset)
  {
    allowed_ = allowed;
    floor_ = floor < 1 ? 1 : floor > kAmbientFullScale ? kAmbientFullScale : floor;
    exponential_ = exponential;
    maxOffset_ = maxOffset < 0 ? 0 : maxOffset > 40 ? 40 : maxOffset;
    mode_ = allowed ? Mode::manual : Mode::off;
    offset_ = 0;
    have_ = userManual_ = false;
  }

  // PianoBacklightAmbient. True when the code should be applied now (code()); -1 leaves automatic mode with the code as it is.
  bool target(int32_t permille, uint32_t lux)
  {
    if (!allowed_) return false;
    if (permille < 0) {
      have_ = false;
      mode_ = Mode::manual;
      return false;
    }
    permille_ = permille > 1000 ? 1000 : uint32_t(permille);
    lux_ = lux;
    have_ = true;
    ++targets_;
    if (userManual_) return false;
    mode_ = Mode::automatic;
    return true;
  }

  // A brightness step (keys, piano-backlight-step) in automatic mode: true when taken here (code() is the new code; it may be the old
  // one at an end); false when not automatic (the caller steps as it does today).
  bool step(int delta)
  {
    if (mode_ != Mode::automatic) return false;
    for (; delta > 0; --delta)
      if (!nudge(+1)) break;
    for (; delta < 0; ++delta)
      if (!nudge(-1)) break;
    return true;
  }

  // An explicit level from an administrator (piano-backlight-percent): manual until piano-backlight-auto=1 is set again.
  void manual()
  {
    if (!allowed_) return;
    userManual_ = true;
    mode_ = Mode::manual;
  }

  // The property piano-backlight-auto: 0 leaves (manual), 1 re-enters. True when code() should be applied now.
  bool setAutomatic(bool on)
  {
    if (!allowed_) return false;
    userManual_ = !on;
    if (!on) {
      mode_ = Mode::manual;
      return false;
    }
    if (!have_) return false;                // the next target enters it
    mode_ = Mode::automatic;
    return true;
  }

  Mode mode() const { return mode_; }
  bool automatic() const { return mode_ == Mode::automatic; }
  int offset() const { return offset_; }
  int32_t lastTarget() const { return have_ ? int32_t(permille_) : -1; }
  uint32_t lastLux() const { return lux_; }
  uint32_t targets() const { return targets_; }
  // The curve's code for the last target, shifted by the offset: floor..2047.
  uint16_t code() const { return shift(codeForPermille(have_ ? permille_ : 0, floor_), offset_, floor_, exponential_); }

  static uint16_t clampCode(int64_t v, uint16_t floor)
  {
    return uint16_t(v < floor ? floor : v > kAmbientFullScale ? kAmbientFullScale : v);
  }
  // permille of the range (1000 = full scale), never below the floor.
  static uint16_t codeForPermille(uint32_t permille, uint16_t floor)
  {
    const uint32_t p = permille > 1000 ? 1000 : permille;
    return clampCode((int64_t(p) * kAmbientFullScale + 500) / 1000, floor);
  }
  // code shifted by offset steps: linear mapping x 2^(offset/3) (Ktz8866.h Steps' factors, the same rounding), exponential mapping
  // + offset/16 of the range above the floor; clamped to floor..2047.
  static uint16_t shift(uint16_t code, int offset, uint16_t floor, bool exponential)
  {
    const uint32_t k = uint32_t(offset < 0 ? -offset : offset);
    if (exponential) {
      const int64_t step = (int64_t(k) * (kAmbientFullScale - floor) + 8) / 16;
      return clampCode(offset < 0 ? int64_t(code) - step : int64_t(code) + step, floor);
    }
    static constexpr uint64_t kCubeRoots[3] = {1000000, 1259921, 1587401};   // 2^(0/3), 2^(1/3), 2^(2/3) x 10^6
    const uint64_t factor = kCubeRoots[k % 3] << (k / 3);                      // k <= 40: below 2^35
    const uint64_t v = offset < 0 ? (uint64_t(code) * 1000000 + factor / 2) / factor : (uint64_t(code) * factor + 500000) / 1000000;
    return clampCode(int64_t(v > kAmbientFullScale ? kAmbientFullScale : v), floor);
  }

 private:
  // One press: the next offset that shows a different code; false (the offset unchanged) at an end.
  bool nudge(int direction)
  {
    const uint16_t before = code();
    const int start = offset_;
    while (direction > 0 ? offset_ < maxOffset_ : offset_ > -maxOffset_) {
      offset_ += direction;
      if (code() != before) return true;
    }
    offset_ = start;
    return false;
  }

  bool allowed_ = false, exponential_ = false, have_ = false, userManual_ = false;
  Mode mode_ = Mode::off;
  uint16_t floor_ = 128;
  int maxOffset_ = 12, offset_ = 0;
  uint32_t permille_ = 0, lux_ = 0, targets_ = 0;
};

}  // namespace pianoauto
#endif
