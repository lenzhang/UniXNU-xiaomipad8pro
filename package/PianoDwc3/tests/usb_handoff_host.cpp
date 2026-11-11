// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the controller hand-off's decisions (UsbHandoff.h, docs/drivers/usb-host.md H3): what PianoDwc3's USB thread does
// with an ask in each state, when the asking side has its answer, and when the core is free for host mode; then a model of one
// round (PianoUsbHost asks for the release, the thread parks; host mode; the core back in device mode, the reclaim, the takeover).
#include "UsbHandoff.h"
#include <cstdio>

using namespace pianousb;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The USB thread between passes (PianoDwc3.cpp): the ask taken, the action, the state after it.
struct Thread {
  uint32_t state = kHoStarting, ask = kAskState;
  bool clocksOn = true, takeoverOk = true;
  unsigned releases = 0, takeovers = 0;
  void pass()
  {
    const uint32_t a = ask;
    ask = kAskState;
    switch (handoffDecide(state, a)) {
      case HandoffDo::release:
        releases++;
        state = kHoReleased;
        break;
      case HandoffDo::reclaim:
        // the clocks re-read first: never a VBUS-override store with them off
        if (!clocksOn) {
          state = kHoFailed;
          break;
        }
        takeovers++;
        state = takeoverOk ? kHoDevice : kHoFailed;
        break;
      default:
        break;
    }
  }
};

int main()
{
  // The table.
  CHECK(handoffDecide(kHoDevice, kAskRelease) == HandoffDo::release);
  CHECK(handoffDecide(kHoStarting, kAskRelease) == HandoffDo::nothing);   // the takeover is not through: ask again later
  CHECK(handoffDecide(kHoReleased, kAskRelease) == HandoffDo::nothing);
  CHECK(handoffDecide(kHoOff, kAskRelease) == HandoffDo::nothing);
  CHECK(handoffDecide(kHoReleased, kAskReclaim) == HandoffDo::reclaim);
  CHECK(handoffDecide(kHoFailed, kAskReclaim) == HandoffDo::reclaim);
  CHECK(handoffDecide(kHoDevice, kAskReclaim) == HandoffDo::nothing);
  CHECK(handoffDecide(kHoDevice, kAskState) == HandoffDo::nothing);
  CHECK(handoffSettled(kAskRelease, kHoReleased) && handoffSettled(kAskRelease, kHoFailed) && !handoffSettled(kAskRelease, kHoDevice));
  CHECK(handoffSettled(kAskReclaim, kHoDevice) && !handoffSettled(kAskReclaim, kHoReleased));
  CHECK(handoffCoreFree(false, kHoOff));                                     // no PianoDwc3: the core is ours
  CHECK(!handoffCoreFree(true, kHoOff) && !handoffCoreFree(true, kHoDevice) && !handoffCoreFree(true, kHoStarting));
  CHECK(handoffCoreFree(true, kHoReleased) && handoffCoreFree(true, kHoFailed));
  // A round.
  Thread t;
  t.ask = kAskRelease;
  t.pass();
  CHECK(t.state == kHoStarting && t.releases == 0);   // asked too early: nothing happens, the asker defers
  t.state = kHoDevice;
  t.ask = kAskRelease;
  t.pass();
  CHECK(t.state == kHoReleased && handoffCoreFree(true, t.state));
  t.pass();
  CHECK(t.state == kHoReleased && t.releases == 1);   // parked: passes change nothing
  t.ask = kAskReclaim;
  t.pass();
  CHECK(t.state == kHoDevice && t.takeovers == 1);
  // A reclaim with the clocks off: no takeover (its override store would be the K:7dffd78b342d hazard); idle until asked again.
  t.ask = kAskRelease;
  t.pass();
  t.clocksOn = false;
  t.ask = kAskReclaim;
  t.pass();
  CHECK(t.state == kHoFailed && t.takeovers == 1);
  t.clocksOn = true;
  t.ask = kAskReclaim;
  t.pass();
  CHECK(t.state == kHoDevice && t.takeovers == 2);
  if (failures) {
    std::printf("usb_handoff_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("usb_handoff_host: all hand-off decision tests passed\n");
  return 0;
}
