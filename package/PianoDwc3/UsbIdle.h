// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB thread's wait between passes (docs/power-irq.md). The loop used to sleep 1 ms after every pass without events, forever:
// about 1000 wake-ups a second with no cable at all. Now the wait follows the link: 1 ms only while something moves (and 200 ms
// after), 20 ms with a port open (the kernel-log pump), 50 ms configured without an open port, 250 ms with no host (nothing
// configured and no event for 3 s, or the link in suspend). With the core's interrupt (piano-usb-irq=1: SPI 133) and the producers'
// kicks ending a wait early, the periods only bound the bookkeeping (EP0 watchdog, log pump, DSTS report): 10/20/200/1000 ms.
// Portable (tests/usb_idle_host.cpp).
#pragma once
#include <stdint.h>

namespace pianousb {

enum class UsbTier : uint8_t { busy, open, attached, idle };
constexpr const char *tierName(UsbTier t)
{
  return t == UsbTier::busy ? "busy" : t == UsbTier::open ? "open" : t == UsbTier::attached ? "attached" : "idle";
}

// One pass's view of the link.
struct UsbActivity {
  bool configured;            // SET_CONFIGURATION 1
  bool portOpen;              // a port's DTR set (console, pipe, input port)
  bool netUp;                 // the NCM data interface selected
  bool ep0Active;             // a control transfer in its data or status stage
  bool txPending;             // bytes waiting for an open port whose IN endpoint is free
  uint32_t linkState;         // DSTS.USBLNKST
  uint64_t sinceActivityMs;   // since the last pass that saw an event, moved log bytes or was kicked
  uint64_t sinceEventMs;      // since the last pass that saw an event (or the thread's start)
};

struct UsbPeriods {
  uint32_t busy, open, attached, idle;
};
constexpr UsbPeriods kPolledPeriods{1, 20, 50, 250};
constexpr UsbPeriods kIrqPeriods{10, 20, 200, 1000};
constexpr uint32_t kBusyHoldMs = 200, kQuietMs = 3000;

// High/full-speed link states (DSTS.USBLNKST): 3 suspend (L2), 4 disconnected, 5 early suspend. No host traffic until a resume or a
// reset, and those arrive as events.
constexpr bool linkAsleep(uint32_t state) { return state == 3 || state == 4 || state == 5; }

constexpr UsbTier usbTier(const UsbActivity &a)
{
  if (a.ep0Active || a.txPending || a.sinceActivityMs < kBusyHoldMs) return UsbTier::busy;
  if (linkAsleep(a.linkState) || (!a.configured && a.sinceEventMs >= kQuietMs)) return UsbTier::idle;
  if (a.configured && (a.portOpen || a.netUp)) return UsbTier::open;
  return UsbTier::attached;
}

constexpr uint32_t periodMs(UsbTier t, const UsbPeriods &p)
{
  return t == UsbTier::busy ? p.busy : t == UsbTier::open ? p.open : t == UsbTier::attached ? p.attached : p.idle;
}

// The core interrupt's primary handler (piano-usb-irq=1): events pending (GEVNTCOUNT) with the interrupt unmasked -> mask it
// (GEVNTSIZ bit 31: the level line drops) and wake the thread, which unmasks after it has taken the events; anything else is an
// entry with nothing to deassert, for the storm guard.
enum class IrqAction : uint8_t { mask, stray };
constexpr IrqAction dwc3IrqAction(uint32_t gevntcount, uint32_t gevntsiz)
{
  return (gevntcount & 0xffffu) && !(gevntsiz & (1u << 31)) ? IrqAction::mask : IrqAction::stray;
}

}  // namespace pianousb
