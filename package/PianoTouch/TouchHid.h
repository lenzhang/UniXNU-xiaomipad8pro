// SPDX-License-Identifier: BSD-2-Clause-Patent
// The HID device PianoTouch publishes at piano-touch=2: the touch screen as a single-touch absolute pointer (a "tablet"), the shape
// that logged in by tap on piano under the earlier VM-based port (UPSTREAM): IOHIDFamily's IOHIDEventDriver turns its absolute
// X/Y into pointer positions on the display and its first button into clicks, so a tap clicks and a drag drags. Report (no report
// ID, 5 bytes): buttons (bit 0 the tip), X and Y as LE16 in 0..32767 over the whole panel. Written from the HID 1.11 specification
// and the HID Usage Tables (pages 0x01 and 0x09). Multi-touch (Apple's path frames to HSTouchHIDService) is not done here.
#ifndef PIANO_TOUCH_HID_H
#define PIANO_TOUCH_HID_H
#include <stdint.h>
#include "../PianoAuto/TouchRotation.h"

namespace pianotouch {

// Vendor 0x1af4 as PianoInput's and PianoKeyboard's devices; the product 'PT' is PianoTouch's own. tooling/k27/kext/PianoTouch.json's
// codeless IOHIDEventDriver personality matches the pair.
constexpr uint32_t kVendorId = 0x1af4, kProductId = 0x5054;
constexpr int32_t kHidMax = 32767;
constexpr unsigned kReportBytes = 5;

constexpr uint8_t kTabletDescriptor[] = {
  0x05, 0x01,         // Usage Page (Generic Desktop)
  0x09, 0x02,         // Usage (Mouse)
  0xa1, 0x01,         // Collection (Application)
  0x09, 0x01,         //   Usage (Pointer)
  0xa1, 0x00,         //   Collection (Physical)
  0x05, 0x09,         //     Usage Page (Button)
  0x19, 0x01,         //     Usage Minimum (1)
  0x29, 0x03,         //     Usage Maximum (3)
  0x15, 0x00,         //     Logical Minimum (0)
  0x25, 0x01,         //     Logical Maximum (1)
  0x75, 0x01,         //     Report Size (1)
  0x95, 0x03,         //     Report Count (3)
  0x81, 0x02,         //     Input (Data, Variable, Absolute): the buttons, bit 0 the tip
  0x75, 0x05,         //     Report Size (5)
  0x95, 0x01,         //     Report Count (1)
  0x81, 0x01,         //     Input (Constant): padding
  0x05, 0x01,         //     Usage Page (Generic Desktop)
  0x09, 0x30,         //     Usage (X)
  0x09, 0x31,         //     Usage (Y)
  0x15, 0x00,         //     Logical Minimum (0)
  0x26, 0xff, 0x7f,   //     Logical Maximum (32767)
  0x35, 0x00,         //     Physical Minimum (0)
  0x46, 0xff, 0x7f,   //     Physical Maximum (32767)
  0x75, 0x10,         //     Report Size (16)
  0x95, 0x02,         //     Report Count (2)
  0x81, 0x02,         //     Input (Data, Variable, Absolute): X, Y
  0xc0,               //   End Collection
  0xc0,               // End Collection
};

// Panel pixels (0..3199, 0..2135) to the report.
inline void tabletReport(bool tip, int32_t x, int32_t y, int32_t panelW, int32_t panelH, uint8_t out[kReportBytes])
{
  auto scale = [](int32_t v, int32_t size) {
    if (v < 0) v = 0;
    if (v > size - 1) v = size - 1;
    return uint32_t(int64_t(v) * kHidMax / (size - 1));
  };
  const uint32_t hx = scale(x, panelW), hy = scale(y, panelH);
  out[0] = tip ? 1 : 0;
  out[1] = uint8_t(hx);
  out[2] = uint8_t(hx >> 8);
  out[3] = uint8_t(hy);
  out[4] = uint8_t(hy >> 8);
}

// The same under the display's rotation (piano-touch-rotate=1; package/PianoAuto/TouchRotation.h, docs/drivers/sensors.md 5a.3):
// degrees as CGDisplayRotation reports them (90 = the panel's left edge up), the contact mapped into the rotated desktop and scaled
// over its size. At 0 the bytes are tabletReport's for the same contact (pat_rotate clamps as tabletReport does).
inline void rotatedTabletReport(bool tip, int32_t x, int32_t y, int32_t panelW, int32_t panelH, uint32_t degrees,
                                uint8_t out[kReportBytes])
{
  const PatPoint p = pat_rotate(degrees, x, y, panelW, panelH);
  tabletReport(tip, p.x, p.y, p.width, p.height, out);
}

}  // namespace pianotouch
#endif
