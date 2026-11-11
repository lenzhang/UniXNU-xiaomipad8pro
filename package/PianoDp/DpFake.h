// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDp D6: the macOS-side probe of an external display, with no DP code and no hardware. NativeGopDisplay publishes a second
// IOMobileFramebuffer instance on its own AppleParavirtGPU nub (DisplayPortCount 2), backed by a RAM surface instead of the panel's
// boot framebuffer, that starts disconnected and is hot-plugged on a timer: does WindowServer extend the desktop onto it?
// (docs/drivers/displayport.md D6, section 4.5.) This header is the part both kexts share, portable and host-tested
// (tests/dp_host.cpp): the boot arguments into one configuration, and the plug schedule. NativeGopDisplay includes it (as it includes
// PianoDpu's flip request); PianoDp includes it to log the D6 configuration with its own lines.
//
// Arguments (all off without piano-dp >= 1, like every PianoDp slice):
//   piano-dp-fake=S          hot-plug S seconds after the second display started (1..3600; 0 or absent: no second display)
//   piano-dp-fake-boot=1     the second display connected from its start instead (the "two displays fixed from boot" fallback)
//   piano-dp-fake-unplug=U   then alternate every U seconds (5..3600): unplug, plug, ... (0: stays plugged)
//   piano-dp-fake-cycles=N   with U: N plugs in all (1..50, default 1), ending plugged
//   piano-dp-fake-mode=WxH@HZ  its one timing (default 1920x1080@60; 640..3840 x 480..2160, 24..120 Hz; W even)
//   piano-dp-fake-mm=WxH     its physical size in mm (default 527x296, a 24-inch 16:9 monitor; 100..2000 each)
//   piano-dp-fake-primary=N  the "primary" property it publishes (default 0, as the panel's; 1 to try the other value)
#pragma once
#include <stdint.h>

namespace pianodp {

struct FakeArgs {
  uint32_t dp = 0;             // piano-dp
  uint32_t plug = 0;           // piano-dp-fake
  uint32_t boot = 0;           // piano-dp-fake-boot
  uint32_t unplug = 0;         // piano-dp-fake-unplug
  uint32_t cycles = 0;         // piano-dp-fake-cycles (0: not given)
  uint32_t primary = 0;        // piano-dp-fake-primary
  const char *mode = nullptr;  // piano-dp-fake-mode (nullptr: not given)
  const char *mm = nullptr;    // piano-dp-fake-mm
};
struct FakeConfig {
  bool armed = false;          // the second display exists (and the nub reports two display ports)
  bool atBoot = false;         // connected from its start
  uint32_t plugS = 0, unplugS = 0, cycles = 1;
  uint32_t width = 1920, height = 1080, hz = 60, widthMm = 527, heightMm = 296, primary = 0;
  const char *note = "";       // why it is off, or what was replaced by a default
};

// "WxH@HZ" / "WxH": decimal fields; false on anything else.
inline bool parseDecimal(const char *&c, uint32_t &out)
{
  if (*c < '0' || *c > '9') return false;
  uint32_t v = 0;
  while (*c >= '0' && *c <= '9') {
    if (v > 100000) return false;
    v = v * 10 + uint32_t(*c++ - '0');
  }
  out = v;
  return true;
}
inline bool parseMode(const char *s, uint32_t &w, uint32_t &h, uint32_t &hz)
{
  if (!s) return false;
  const char *c = s;
  uint32_t a = 0, b = 0, r = 60;
  if (!parseDecimal(c, a) || *c++ != 'x' || !parseDecimal(c, b)) return false;
  if (*c == '@') {
    ++c;
    if (!parseDecimal(c, r)) return false;
  }
  if (*c) return false;
  w = a;
  h = b;
  hz = r;
  return true;
}
inline bool parseSize(const char *s, uint32_t &w, uint32_t &h)
{
  if (!s) return false;
  const char *c = s;
  uint32_t a = 0, b = 0;
  if (!parseDecimal(c, a) || *c++ != 'x' || !parseDecimal(c, b) || *c) return false;
  w = a;
  h = b;
  return true;
}

inline FakeConfig fakeConfig(const FakeArgs &a)
{
  FakeConfig c;
  if (a.dp < 1) {
    c.note = "piano-dp is 0";
    return c;
  }
  if (!a.plug && !a.boot) {
    c.note = "neither piano-dp-fake=S nor piano-dp-fake-boot=1";
    return c;
  }
  c.atBoot = a.boot != 0;
  if (!c.atBoot) {
    if (a.plug > 3600) {
      c.note = "piano-dp-fake above 3600 s: off";
      return c;
    }
    c.plugS = a.plug;
  }
  c.armed = true;
  if (a.unplug) {
    if (a.unplug >= 5 && a.unplug <= 3600) c.unplugS = a.unplug;
    else c.note = "piano-dp-fake-unplug outside 5..3600: stays plugged";
  }
  if (a.cycles) {
    if (a.cycles <= 50) c.cycles = a.cycles;
    else c.note = "piano-dp-fake-cycles above 50: 1";
  }
  if (!c.unplugS) c.cycles = 1;
  uint32_t w = 0, h = 0, hz = 0;
  if (a.mode) {
    if (parseMode(a.mode, w, h, hz) && w >= 640 && w <= 3840 && !(w & 1u) && h >= 480 && h <= 2160 && hz >= 24 && hz <= 120) {
      c.width = w;
      c.height = h;
      c.hz = hz;
    } else {
      c.note = "piano-dp-fake-mode not WxH@HZ within 640..3840 x 480..2160 @ 24..120 (W even): 1920x1080@60";
    }
  }
  if (a.mm) {
    if (parseSize(a.mm, w, h) && w >= 100 && w <= 2000 && h >= 100 && h <= 2000) {
      c.widthMm = w;
      c.heightMm = h;
    } else {
      c.note = "piano-dp-fake-mm not WxH within 100..2000 mm: 527x296";
    }
  }
  c.primary = a.primary;
  return c;
}

// The schedule: the second display is connected from base = (atBoot ? 0 : plugS) seconds after its start, then with unplugS it
// alternates every unplugS seconds, cycles connections in all, ending connected. Event k (from 0) of that list; the start-up
// connection of piano-dp-fake-boot is the initial state, not an event.
struct PlugStep {
  bool valid = false;
  bool connect = false;
  uint32_t atS = 0;            // seconds after the second display's start
  uint32_t plugNumber = 0;     // 1-based count of connections up to and including this step
};
inline uint32_t plugEvents(const FakeConfig &c)
{
  if (!c.armed) return 0;
  const uint32_t all = c.unplugS ? 2 * c.cycles - 1 : 1;   // the states connected, (disconnected, connected) x (cycles - 1)
  return c.atBoot ? all - 1 : all;
}
inline PlugStep plugStep(const FakeConfig &c, uint32_t k)
{
  PlugStep s;
  if (k >= plugEvents(c)) return s;
  const uint32_t j = k + (c.atBoot ? 1 : 0);   // the index among all states
  s.valid = true;
  s.connect = (j & 1u) == 0;
  s.atS = (c.atBoot ? 0 : c.plugS) + j * c.unplugS;
  s.plugNumber = j / 2 + 1;
  return s;
}

// The second display's identity: a UUID of its own (the panel's ends in 8750), so WindowServer keeps its arrangement apart.
constexpr const char *kFakeUuid = "4E474F50-4449-5350-0001-0000000000D6";
constexpr const char *kFakeProductName = "PianoDp test display";
constexpr uint32_t kFakeProductId = 0xd6;

}  // namespace pianodp
