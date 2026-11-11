// SPDX-License-Identifier: BSD-2-Clause-Patent
// The controller hand-off between PianoDwc3 (device mode, the debug channel) and PianoUsbHost (host mode), docs/drivers/usb-host.md H3.
// Default off: only with piano-usb-handoff=1 does PianoDwc3 answer the platform function below; without it the controller is
// PianoDwc3's for the whole boot, as before. Shared by both kexts (PianoUsbHost includes it from package/PianoDwc3); no link
// dependency: PianoUsbHost finds the registered PianoDwc3 service by class and calls
//     callPlatformFunction("piano-usb-handoff", false, (void *)ask, (uint32_t *)state, (void *)timeoutMs, nullptr)
// ask: release (PianoDwc3 stops its device controller, masks its event interrupt, stops touching the core and parks its USB thread;
// the console rings keep filling and are replayed later; only while awake) or reclaim (PianoDwc3 waits out a sleep, re-reads the core's
// power domain and clocks and, only when they read on, runs its whole takeover again: halt, soft reset, device mode, the VBUS/session
// override through its sleep guard, run), or state (which also withdraws a pending ask: a release not taken in time). It
// returns kIOReturnSuccess with *state once the USB thread has done it (or the state as it is after timeoutMs, at most 5 s),
// kIOReturnUnsupported when the switch is off. The caller (PianoUsbHost) returns the core to device mode (PRTCAPDIR, the override as
// it found it, each glue store behind its clock gate) before it asks for the reclaim. Portable (host-tested by
// package/PianoDwc3/tests/usb_handoff_host.cpp): the decision the USB thread takes between passes.
#pragma once
#include <stdint.h>

namespace pianousb {

constexpr const char *kHandoffFunction = "piano-usb-handoff";
enum HandoffAsk : uint32_t { kAskState = 0, kAskRelease = 1, kAskReclaim = 2 };
enum HandoffState : uint32_t {
  kHoOff = 0,        // piano-usb-handoff=0: never released
  kHoStarting = 1,   // before (or during) PianoDwc3's takeover: ask again later
  kHoDevice = 2,     // the device controller runs: PianoDwc3's
  kHoReleased = 3,   // stopped and parked: the core is free for PianoUsbHost
  kHoFailed = 4,     // a takeover failed (or a reclaim found the clocks off): the core is free, PianoDwc3 idle
};
inline const char *handoffStateName(uint32_t s)
{
  switch (s) {
    case kHoOff: return "off (piano-usb-handoff=0)";
    case kHoStarting: return "starting";
    case kHoDevice: return "device (PianoDwc3 runs the controller)";
    case kHoReleased: return "released";
    case kHoFailed: return "failed (PianoDwc3 idle)";
    default: return "?";
  }
}

// What the USB thread does with the pending ask in its state: release only a running device controller, reclaim only a released (or
// failed) one; anything else leaves it as it is (the caller reads the state back).
enum class HandoffDo : uint8_t { nothing, release, reclaim };
inline HandoffDo handoffDecide(uint32_t state, uint32_t ask)
{
  if (ask == kAskRelease && state == kHoDevice) return HandoffDo::release;
  if (ask == kAskReclaim && (state == kHoReleased || state == kHoFailed)) return HandoffDo::reclaim;
  return HandoffDo::nothing;
}
// The state the caller waits for after an ask (the USB thread got to it): release -> released or failed, reclaim -> device or failed.
inline bool handoffSettled(uint32_t ask, uint32_t state)
{
  if (ask == kAskRelease) return state == kHoReleased || state == kHoFailed || state == kHoOff;
  if (ask == kAskReclaim) return state == kHoDevice || state == kHoFailed || state == kHoOff;
  return true;
}
// The caller's view: may the core be switched to host now? (PianoDwc3 absent: yes; present: released or failed only.)
inline bool handoffCoreFree(bool present, uint32_t state) { return !present || state == kHoReleased || state == kHoFailed; }

}  // namespace pianousb
