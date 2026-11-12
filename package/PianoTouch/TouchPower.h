// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTouch's power states (docs/power.md §4, package/include/PianoPowerContract.h; docs/drivers/touch.md §16): the system's
// (setPowerState off for suspend-to-idle, restart and shutdown; on after them) and the display's (NativeGopDisplay's
// piano-display-power, which it publishes with piano-backlight=2; absent: on). Portable: tests/touch_host.cpp.
//   on          frames read and decoded, the gestures reported (the frame pace of TouchPace.h)
//   display off nothing reported (a touch on a dark screen must not click, nor wake it), every contact and gesture ended at the
//               change, the latch looked at once per kMutedReadMs and only announced frames read (never busy: no CPU latency
//               hold); read errors and watchdog
//               frames are only counted (a TDDI stops scanning with its panel, Ubuntu: the THP stream stopped at panel disable)
//   suspended   no frame read at all (the facts give the NT36532E no sleep command of its own: stopping the reads is what stops
//               the host's work; Ubuntu stopped touch transfers before the SPI bus suspended), the latency hold released, the
//               TLMM interrupt masked at the pin (touch is no wake source, as in Ubuntu); the thread parked until on
// Leaving display off or suspended (resume / unmute): the caller probes the controller (Nt36532.h probeState): the state kept, the
// frames go on (after a suspend the reference is learned again: the time asleep is unknown); lost, the download again (at most
// kResumeDownloads times), then the reference.
#ifndef PIANO_TOUCH_POWER_H
#define PIANO_TOUCH_POWER_H
#include <stdint.h>

namespace pianotouch {

constexpr unsigned kResumeDownloads = 2;
constexpr uint32_t kDisplayPollMs = 250;   // how often the frame thread reads piano-display-power

enum class PowerMode : uint8_t { on, displayOff, suspended };
enum class PowerStep : uint8_t { none, suspend, resume, mute, unmute };

struct TouchPower {
  bool followDisplay = true;   // piano-touch-display (0: the display's state is ignored)
  PowerMode mode = PowerMode::on;
  uint32_t suspends = 0, resumes = 0, mutes = 0, unmutes = 0;
  bool reporting() const { return mode == PowerMode::on; }
  bool reading() const { return mode != PowerMode::suspended; }
  bool muted() const { return mode == PowerMode::displayOff; }
  // display: 1 on, 0 off, -1 unknown (not published: counts as on). The step to act on now, at most one per call.
  PowerStep step(bool systemOff, int display)
  {
    const bool dark = followDisplay && display == 0;
    switch (mode) {
    case PowerMode::on:
      if (systemOff) { mode = PowerMode::suspended; ++suspends; return PowerStep::suspend; }
      if (dark) { mode = PowerMode::displayOff; ++mutes; return PowerStep::mute; }
      return PowerStep::none;
    case PowerMode::displayOff:
      if (systemOff) { mode = PowerMode::suspended; ++suspends; return PowerStep::suspend; }
      if (!dark) { mode = PowerMode::on; ++unmutes; return PowerStep::unmute; }
      return PowerStep::none;
    case PowerMode::suspended:
      if (systemOff) return PowerStep::none;
      mode = dark ? PowerMode::displayOff : PowerMode::on;
      ++resumes;
      return PowerStep::resume;
    }
    return PowerStep::none;
  }
};

// What the frame loop does after resume / unmute, from the probe and the downloads tried so far: go on (kept), go on after the
// reference is learned again, download again, or give up (frames stopped; the next resume tries again).
enum class Restart : uint8_t { keep, relearn, download, giveUp };
inline Restart restartPlan(bool fromSuspend, bool stateKept, unsigned downloadsTried)
{
  if (stateKept) return fromSuspend ? Restart::relearn : Restart::keep;
  return downloadsTried < kResumeDownloads ? Restart::download : Restart::giveUp;
}

}  // namespace pianotouch
#endif
