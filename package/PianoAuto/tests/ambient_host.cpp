// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU-piano contributors. All rights reserved.
// Host tests of AmbientBacklight.h (PianoBacklight's automatic mode, docs/drivers/sensors.md section 5a), synthetic inputs only:
//   ladder   its shift is PianoBacklight's own step ladder (Ktz8866.h's Steps, included read only from package/PianoBacklight): linear and
//            exponential mapping, the floor
//   modes    off without piano-backlight-auto, manual until a target, automatic, the keys as an offset (always a visible step unless at
//            an end), the user's explicit level, leaving and re-entering, the sensor leaving
//   chain    lux -> AutoLight (SensorsPolicy's curve and smoothing) -> targets -> codes: monotonic, the floor in the dark, full scale in
//            daylight, the offset kept, a display off/on (the code kept while dark, the room's level at once at wake)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "Ktz8866.h"
#include "AmbientBacklight.h"
extern "C" {
#include "AutoLight.h"
}

static unsigned checks;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } ++checks; } while (0)

using pianoauto::Ambient;
using pianoauto::Mode;

static uint32_t bits(float f)
{
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

static void test_ladder()
{
  for (uint16_t floor : {uint16_t(32), uint16_t(128), uint16_t(400)}) {
    pianobacklight::Steps linear;
    linear.build(floor, false);
    const unsigned n = linear.count();
    for (unsigned k = 0; k + 1 < n; ++k) CHECK(Ambient::shift(2047, -int(k), floor, false) == linear.at(n - 1 - k));
    CHECK(Ambient::shift(2047, -int(n - 1), floor, false) == floor);     // the ladder ends at the floor
    pianobacklight::Steps exponential;
    exponential.build(floor, true);
    for (unsigned i = 0; i < exponential.count(); ++i) CHECK(Ambient::shift(floor, int(i), floor, true) == exponential.at(i));
  }
  // one step up then down comes back within rounding; the ends hold
  for (uint16_t code = 128; code < 1600; code += 37) {   // below the clamp at full scale
    const uint16_t up = Ambient::shift(code, 1, 128, false), back = Ambient::shift(up, -1, 128, false);
    CHECK(up > code && (back + 1 >= code && back <= code + 1));
  }
  CHECK(Ambient::shift(2047, 3, 128, false) == 2047 && Ambient::shift(128, -3, 128, false) == 128);
  CHECK(Ambient::codeForPermille(0, 128) == 128 && Ambient::codeForPermille(40, 128) == 128 && Ambient::codeForPermille(500, 128) == 1024 &&
        Ambient::codeForPermille(1000, 128) == 2047 && Ambient::codeForPermille(5000, 128) == 2047);
  std::printf("PASS ladder: the shift is PianoBacklight's step ladder (linear and exponential, three floors), round trips, the ends\n");
}

static void test_modes()
{
  Ambient a;
  a.configure(false, 128, false, 12);
  CHECK(a.mode() == Mode::off && !a.target(370, 300) && !a.step(1) && !a.setAutomatic(true) && a.targets() == 0);
  a.configure(true, 128, false, 12);
  CHECK(a.mode() == Mode::manual && !a.step(1));                 // no target yet: the keys step as today
  CHECK(a.target(370, 300) && a.automatic() && a.code() == 757 && a.lastLux() == 300);
  CHECK(a.step(1) && a.offset() == 1 && a.code() == 954);
  CHECK(a.step(-2) && a.offset() == -1 && a.code() == 601);
  CHECK(a.target(1000, 10000) && a.code() == 1625);              // daylight with the user's -1: one step under full scale
  CHECK(a.step(1) && a.offset() == 0 && a.code() == 2047);
  CHECK(a.step(1) && a.offset() == 0 && a.code() == 2047);       // at full scale: nothing (the offset does not creep)
  CHECK(a.target(40, 0) && a.code() == 128);                     // dark: the floor
  CHECK(a.step(-1) && a.offset() == 0 && a.code() == 128);
  CHECK(a.step(1) && a.offset() == 1 && a.code() == 161);
  // an offset the floor hides is skipped: one press is always a visible step
  CHECK(a.target(370, 300) && a.step(-3) && a.offset() == -2 && a.code() == 477);
  CHECK(a.target(40, 0) && a.code() == 128);
  CHECK(a.step(1) && a.offset() == 1 && a.code() == 161);
  // the offset is bounded by the ladder
  for (int i = 0; i < 40; ++i) a.step(1);
  CHECK(a.offset() <= 12 && a.code() == 2047);
  // an explicit level: manual, targets only recorded; piano-backlight-auto=1 again: the last target at once with the offset
  a.configure(true, 128, false, 12);
  CHECK(a.target(500, 400) && a.code() == 1024 && a.step(-1) && a.code() == 813);
  a.manual();
  CHECK(a.mode() == Mode::manual && !a.target(600, 600) && !a.step(1) && a.lastTarget() == 600);
  CHECK(a.setAutomatic(true) && a.automatic() && a.code() == Ambient::shift(Ambient::codeForPermille(600, 128), -1, 128, false));
  CHECK(!a.setAutomatic(false) && !a.target(700, 700) && a.mode() == Mode::manual);
  CHECK(a.setAutomatic(true) && a.automatic());
  // the sensor leaves (-1): manual with the code as it is; its next target returns to automatic
  CHECK(!a.target(-1, 0) && a.mode() == Mode::manual && a.lastTarget() == -1 && !a.step(1));
  CHECK(!a.setAutomatic(true) && a.mode() == Mode::manual);       // nothing to apply until a target comes
  CHECK(a.target(450, 500) && a.automatic());
  // exponential mapping: 1/16 of the range per press
  a.configure(true, 128, true, 16);
  CHECK(a.target(500, 400) && a.step(1) && a.code() == 1024 + 120);
  std::printf("PASS modes: off, manual until a target, automatic, keys as an offset (visible steps, bounded), explicit levels, "
              "leaving and re-entering, the sensor leaving\n");
}

static PalLight light;

static int32_t first_target(float lux, uint64_t t)
{
  PasConfig c;
  pas_defaults(&c);
  pal_init(&light, &c);
  pal_conditions(&light, 1, 1, 1, t);
  pas_sent(&light.stream, pas_action(&light.stream, t), t);
  pas_response(&light.stream, 1, t + 5);
  return pal_event(&light, bits(lux), t + 10);
}

static void test_chain()
{
  // the curve's points as first samples: codes rise with the light, the floor in the dark, full scale in daylight; an offset of -1
  // keeps every code one step lower (or at the floor)
  static const float lux[] = {0, 5, 20, 50, 100, 200, 400, 800, 1500, 3000, 6000, 10000, 50000};
  Ambient plain, darker;
  plain.configure(true, 128, false, 12);
  darker.configure(true, 128, false, 12);
  CHECK(darker.target(500, 400) && darker.step(-1) && darker.offset() == -1);   // the user's "one step darker", once
  uint16_t last = 0;
  for (float l : lux) {
    const int32_t target = first_target(l, 0);
    CHECK(target >= 0 && plain.target(target, light.lux) && darker.target(target, light.lux));
    CHECK(plain.code() >= last && darker.code() <= plain.code() && darker.code() >= 128 && darker.offset() == -1);
    CHECK(plain.code() == 128 || darker.code() < plain.code());
    last = plain.code();
  }
  CHECK(Ambient::codeForPermille(uint32_t(first_target(0, 0)), 128) == 128 && last == 2047);
  // a session: 300 lux, a lamp on (1000 lux) smoothed up; the display sleeps (the code kept), wakes in the dark: the floor at once
  Ambient a;
  a.configure(true, 128, false, 12);
  CHECK(a.target(first_target(300, 0), light.lux) && a.code() == 757);
  uint16_t previous = a.code();
  const int32_t lamp = pal_event(&light, bits(1000.0f), 1000);
  if (lamp >= 0) {
    CHECK(a.target(lamp, light.lux) && a.code() > previous);
    previous = a.code();
  }
  for (uint64_t t = 1250; light.tick_at && t < 20000; t = light.tick_at) {
    const int32_t target = pal_tick(&light, light.tick_at);
    if (target >= 0) {
      CHECK(a.target(target, light.lux) && a.code() > previous);
      previous = a.code();
    }
  }
  CHECK(a.code() == Ambient::codeForPermille(574, 128));
  pal_conditions(&light, 1, 0, 1, 30000);                             // display off: the stream goes off, nothing comes
  pas_sent(&light.stream, pas_action(&light.stream, 30000), 30000);
  CHECK(pal_event(&light, bits(0.0f), 30001) == -1 && a.code() == Ambient::codeForPermille(574, 128));
  pas_response(&light.stream, 1, 30010);
  pal_conditions(&light, 1, 1, 1, 90000);                             // wake
  pas_sent(&light.stream, pas_action(&light.stream, 90000), 90000);
  pas_response(&light.stream, 1, 90010);
  const int32_t wake = pal_event(&light, bits(0.0f), 90020);
  CHECK(wake == 40 && a.target(wake, light.lux) && a.code() == 128);
  std::printf("PASS chain: lux to codes through the light policy, the offset kept, display off/on with the room's level at wake\n");
}

int main()
{
  test_ladder();
  test_modes();
  test_chain();
  std::printf("PASS AmbientBacklight host tests (%u checks)\n", checks);
  return 0;
}
