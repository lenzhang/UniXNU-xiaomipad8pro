// SPDX-License-Identifier: BSD-2-Clause-Patent
// The PianoTouch service's interface for other drivers, header only (no link dependency: a caller finds the service by class name
// and calls IOService::callPlatformFunction, which every IOService has), as PianoBacklightClient.h:
//   "PianoTouchContact"   param1 = uint64_t *: set to the uptime in milliseconds (mach_absolute_time through
//                         absolutetime_to_nanoseconds, / 1000000) of the touch screen's last frame with a finger on it (a palm does
//                         not count), 0 when there was none since the boot. Answered from one word the frame thread stores: no lock,
//                         no wait.
// PianoKeyboard asks before it sends a touchpad tap (Wn8030Piano.h PointerConfig::tapVeto): a "tap" on the cover's touchpad while a
// finger is on the screen is the screen touch showing through on the pad. attend1 (2026-11-11): all three cover taps (33-56 ms) came
// inside screen taps, 11-16 ms before the screen's own click, which made one click two (a double click).
#ifndef PIANO_TOUCH_CLIENT_H
#define PIANO_TOUCH_CLIENT_H
#include <stdint.h>

namespace pianotouch {

constexpr const char *kServiceClass = "PianoTouch";
constexpr const char *kFunctionContact = "PianoTouchContact";
// A cover tap is dropped when the screen had a finger on it during the tap or this much before it.
constexpr uint32_t kContactMarginMs = 60;

// Pure: was the screen touched within the tap (tapMs long, ending at nowMs) or kContactMarginMs before it?
inline bool overlaps(uint64_t lastContactMs, uint64_t nowMs, uint32_t tapMs)
{
  if (!lastContactMs) return false;
  if (lastContactMs >= nowMs) return true;   // a finger on the screen now
  return nowMs - lastContactMs <= uint64_t(tapMs) + kContactMarginMs;
}

}  // namespace pianotouch

#ifdef KERNEL
#include <IOKit/IOService.h>
#include <kern/clock.h>

namespace pianotouch {

inline uint64_t uptimeMs()
{
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  return ns / 1000000;
}
// The service, looked up until found once (then kept: it never terminates). Not for a gated or interrupt context (the lookup takes
// the registry's locks).
inline IOService *service()
{
  static IOService *volatile found = nullptr;
  if (IOService *s = found) return s;
  OSDictionary *matching = IOService::serviceMatching(kServiceClass);
  IOService *s = matching ? IOService::copyMatchingService(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (s && !__sync_bool_compare_and_swap(&found, nullptr, s)) s->release();
  return found;
}
// The screen's last contact (uptime ms), 0 when none or without a PianoTouch service.
inline uint64_t lastContactMs()
{
  IOService *s = service();
  uint64_t ms = 0;
  if (!s || s->callPlatformFunction(kFunctionContact, false, &ms, nullptr, nullptr, nullptr) != kIOReturnSuccess) return 0;
  return ms;
}
// A touchpad tap of tapMs that just ended: was a finger on the screen meanwhile?
inline bool screenTouchedDuring(uint32_t tapMs) { return overlaps(lastContactMs(), uptimeMs(), tapMs); }

}  // namespace pianotouch
#endif
#endif
