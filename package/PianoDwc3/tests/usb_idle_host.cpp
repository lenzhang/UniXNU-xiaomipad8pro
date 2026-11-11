// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of UsbIdle.h: the USB thread's tier and period for every state of the link, and the wake-ups a second each gives.
#include "UsbIdle.h"
#include <cstdio>

using namespace pianousb;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main()
{
  const UsbActivity quiet{false, false, false, false, false, 0, 60000, 60000};
  // No cable (nothing ever configured, no event): idle, 250 ms polled (4 wake-ups a second instead of ~1000), 1 s with the IRQ.
  CHECK(usbTier(quiet) == UsbTier::idle);
  CHECK(periodMs(usbTier(quiet), kPolledPeriods) == 250 && periodMs(usbTier(quiet), kIrqPeriods) == 1000);
  CHECK(1000 / periodMs(UsbTier::idle, kPolledPeriods) == 4);
  {   // enumeration: events within the last 3 s, not configured yet
    UsbActivity a = quiet;
    a.sinceEventMs = 500;
    CHECK(usbTier(a) == UsbTier::attached);
    a.sinceActivityMs = 150;
    CHECK(usbTier(a) == UsbTier::busy && periodMs(UsbTier::busy, kPolledPeriods) == 1);
    a.sinceActivityMs = 200;
    CHECK(usbTier(a) == UsbTier::attached);
    a.sinceEventMs = 3000;
    CHECK(usbTier(a) == UsbTier::idle);
  }
  {   // configured
    UsbActivity a = quiet;
    a.configured = true;
    CHECK(usbTier(a) == UsbTier::attached && periodMs(UsbTier::attached, kPolledPeriods) == 50);
    a.portOpen = true;
    CHECK(usbTier(a) == UsbTier::open && periodMs(UsbTier::open, kPolledPeriods) == 20 && periodMs(UsbTier::open, kIrqPeriods) == 20);
    a.portOpen = false;
    a.netUp = true;
    CHECK(usbTier(a) == UsbTier::open);
    a.linkState = 3;   // the Mac asleep: suspend
    CHECK(usbTier(a) == UsbTier::idle);
    a.linkState = 0;
    a.ep0Active = true;
    CHECK(usbTier(a) == UsbTier::busy);
    a.ep0Active = false;
    a.txPending = true;
    CHECK(usbTier(a) == UsbTier::busy);
    a.txPending = false;
    a.sinceActivityMs = 0;
    CHECK(usbTier(a) == UsbTier::busy);
  }
  CHECK(linkAsleep(3) && linkAsleep(4) && linkAsleep(5) && !linkAsleep(0) && !linkAsleep(2) && !linkAsleep(14) && !linkAsleep(15));
  // Activity always wins over a sleeping link (a resume or reset arrives as an event).
  UsbActivity woke = quiet;
  woke.linkState = 3;
  woke.sinceActivityMs = 1;
  CHECK(usbTier(woke) == UsbTier::busy);
  CHECK(tierName(UsbTier::idle)[0] == 'i' && tierName(UsbTier::busy)[0] == 'b');
  // The interrupt handler's decision.
  CHECK(dwc3IrqAction(4, 0x1000) == IrqAction::mask);
  CHECK(dwc3IrqAction(4, 0x80001000u) == IrqAction::stray);   // masked and still entered: the mask did not drop the line
  CHECK(dwc3IrqAction(0, 0x1000) == IrqAction::stray);        // nothing pending
  CHECK(dwc3IrqAction(0x10000, 0x1000) == IrqAction::stray);  // only bits 15:0 count
  if (failures) {
    std::printf("usb_idle_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("usb_idle_host: all passed\n");
  return 0;
}
