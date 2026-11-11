// SPDX-License-Identifier: BSD-2-Clause-Patent
// The PianoBacklight service's interface for other drivers, header only (no link dependency: a caller finds the service by class
// name and calls IOService::callPlatformFunction, which every IOService has):
//   "PianoBacklightDisplay"  param1 = on (0/1), param2 = the system power state, param3 = WindowServer's (setAPIPowerState)
//                            power state: NativeGopDisplay's display power, from its do_power_state_change_gated (piano-backlight>=2)
//   "PianoBacklightStep"     param1 = steps (intptr_t, + brighter, - darker), param2 = the caller's name (a static C string)
// Both only record the request and wake the service's thread: they never wait for I2C, so a gated or interrupt-free caller is fine.
// NativeGopDisplay also publishes "piano-display-power" (a boolean, on itself) with each change, which the service reads as well.
//
// KeyWatch turns a producer's keyboard and consumer reports into steps: consumer usage 0x6F (Display Brightness Increment) and 0x70
// (Display Brightness Decrement), HID Usage Tables 1.4 section 15, on the press; and the combo Right Ctrl + 1 (darker) / Right Ctrl + 2
// (brighter), the F1/F2 places of a Mac keyboard's brightness keys. Callers send the reports to macOS unchanged as well. A producer
// takes it behind piano-backlight-keys=1 (keysEnabled()); without it nothing changes.
// Integration in a producer (PianoDwc3's HID thread does this; PianoKeyboard can do the same in its report dispatch):
//   static pianobacklight::KeyWatch gBrightnessKeys;
//   if (pianobacklight::keysEnabled()) pianobacklight::step(gBrightnessKeys.consumer(usage), "the cover");     // report 6
//   if (pianobacklight::keysEnabled()) pianobacklight::step(gBrightnessKeys.keyboard(report9), "the cover");  // report 5
#ifndef PIANO_BACKLIGHT_CLIENT_H
#define PIANO_BACKLIGHT_CLIENT_H
#include <stdint.h>

namespace pianobacklight {

constexpr const char *kServiceClass = "PianoBacklight";
constexpr const char *kFunctionDisplay = "PianoBacklightDisplay";
constexpr const char *kFunctionStep = "PianoBacklightStep";
constexpr const char *kDisplayPowerProperty = "piano-display-power";
constexpr uint16_t kUsageBrightnessUp = 0x6f, kUsageBrightnessDown = 0x70;
constexpr uint8_t kModRightCtrl = 0x10, kUsageKey1 = 0x1e, kUsageKey2 = 0x1f;

class KeyWatch {
 public:
  // A consumer report's usage (0: released): +1 / -1 on the press of a brightness usage, else 0.
  int consumer(uint16_t usage)
  {
    const uint16_t previous = usage_;
    usage_ = usage;
    if (usage == previous) return 0;
    return usage == kUsageBrightnessUp ? 1 : usage == kUsageBrightnessDown ? -1 : 0;
  }
  // A 9-byte keyboard report {ID, modifiers, reserved, six usages}: +1 / -1 when the combo's key goes down with Right Ctrl held.
  int keyboard(const uint8_t *report)
  {
    bool one = false, two = false;
    for (unsigned i = 3; i < 9; ++i) {
      one = one || report[i] == kUsageKey1;
      two = two || report[i] == kUsageKey2;
    }
    const bool ctrl = report[1] & kModRightCtrl;
    const uint8_t held = uint8_t((ctrl && one ? 1 : 0) | (ctrl && two ? 2 : 0));
    const int delta = ((held & 2) && !(held_ & 2) ? 1 : 0) - ((held & 1) && !(held_ & 1) ? 1 : 0);
    held_ = held;
    return delta;
  }

 private:
  uint16_t usage_ = 0;
  uint8_t held_ = 0;
};

}  // namespace pianobacklight

#ifdef KERNEL
#include <IOKit/IOService.h>
#include <pexpert/pexpert.h>

namespace pianobacklight {

// piano-backlight-keys=1, read once.
inline bool keysEnabled()
{
  static int enabled = -1;
  if (enabled < 0) {
    uint32_t value = 0;
    enabled = PE_parse_boot_argn("piano-backlight-keys", &value, sizeof value) && value ? 1 : 0;
  }
  return enabled == 1;
}
// The service, looked up until found once (then kept: it never terminates). Not for a gated or interrupt context (the lookup takes
// the registry's locks): NativeGopDisplay uses a publish notification instead.
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
inline bool step(int delta, const char *source)
{
  if (!delta) return false;
  IOService *s = service();
  return s && s->callPlatformFunction(kFunctionStep, false, reinterpret_cast<void *>(intptr_t(delta)), const_cast<char *>(source),
                                      nullptr, nullptr) == kIOReturnSuccess;
}

}  // namespace pianobacklight
#endif
#endif
