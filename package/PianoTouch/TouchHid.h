// SPDX-License-Identifier: BSD-2-Clause-Patent
// The HID device PianoTouch publishes at piano-touch=2: the touch screen as an absolute pointer (a "tablet", the shape that logged
// in by tap on piano under the earlier VM-based port, UPSTREAM) with a wheel and AC Pan. IOHIDFamily's IOHIDEventDriver turns its
// absolute X/Y into pointer positions on the display, its buttons into clicks (1 left, 2 right) and the wheel and AC Pan into
// scrolling; TouchGesture.h decides what a touch sends. macOS has no touch screen interface: this is what it understands from HID.
// Report (no report ID, 7 bytes): buttons (bit 0 left, 1 right, 2 middle), X and Y as LE16 in 0..32767 over the whole desktop
// (TouchRotation.h: the panel, or the rotated desktop), the wheel and AC Pan as signed bytes. With the pinch's modifier
// (piano-touch-pinch=1/2) the descriptor gains a keyboard collection with the eight modifiers: report ID 1 is then the pointer
// (8 bytes), report ID 2 the modifiers (2 bytes).
// Scroll resolution: macOS takes it from the Wheel's extents (IOHIDEventService::determineResolution, docs/drivers/keyboard.md
// §6a: logical range over physical range, counts per inch; 9 without extents) and ignores the Resolution Multiplier, so fine
// scrolling is fine counts with a correspondingly higher declared resolution, as PianoKeyboard's touchpad descriptor does
// (package/PianoKeyboard/HidDescriptors.h makeTouchpadDescriptor): Physical -W..W with Unit inch and Unit Exponent -3, W chosen so
// that the integer quotient is exactly the resolution. The absolute X/Y keep Physical = Logical (no resolution: they are positions).
// Written from the HID 1.11 specification and the HID Usage Tables (pages 0x01, 0x07, 0x09 and 0x0C).
#ifndef PIANO_TOUCH_HID_H
#define PIANO_TOUCH_HID_H
#include <stdint.h>
#include "../PianoAuto/TouchRotation.h"

namespace pianotouch {

// Vendor 0x1af4 as PianoInput's and PianoKeyboard's devices; the product 'PT' is PianoTouch's own. tooling/k27/kext/PianoTouch.json's
// codeless IOHIDEventDriver personality matches the pair.
constexpr uint32_t kVendorId = 0x1af4, kProductId = 0x5054;
constexpr int32_t kHidMax = 32767;
constexpr unsigned kTabletBytes = 5, kPointerBytes = 7, kMaxReportBytes = 8;
constexpr uint8_t kPointerReportId = 1, kKeyboardReportId = 2;
constexpr uint8_t kButtonLeft = 1, kButtonRight = 2, kButtonMiddle = 4;
constexpr uint8_t kModifierControl = 0x01, kModifierCommand = 0x08;   // Keyboard page E0 (Left Control), E3 (Left GUI)

// What TouchGesture.h sends: the buttons, the position in desktop pixels, wheel and AC Pan counts (-127..127, positive: up /
// right), the keyboard modifiers held (only with the pinch's keyboard collection).
struct PointerReport {
  uint8_t buttons;
  int32_t x, y;
  int32_t wheel, pan;
  uint8_t modifiers;
};

// The descriptor, made for the scroll resolution (counts per inch, 10..900) and with or without the modifiers' collection.
struct TouchDescriptor {
  uint8_t bytes[144];
  unsigned length;
};
constexpr uint32_t kTouchScrollResolution = 45;
constexpr uint32_t touchScrollResolution(uint32_t r) { return r < 10 ? 10 : r > 900 ? 900 : r; }
constexpr TouchDescriptor makeTouchDescriptor(uint32_t scrollResolution, bool keyboard)
{
  TouchDescriptor d = {};
  unsigned n = 0;
  auto item = [&](uint8_t tag, int32_t value, unsigned size) {   // a short item of 1 or 2 data bytes
    d.bytes[n++] = uint8_t(tag | size);
    for (unsigned i = 0; i < size; ++i) d.bytes[n++] = uint8_t(uint32_t(value) >> (8 * i));
  };
  const int32_t wheel = int32_t(254000 / (2 * touchScrollResolution(scrollResolution)));   // 254 x 1000 / (2W) = resolution
  item(0x04, 0x01, 1); item(0x08, 0x02, 1); item(0xa0, 0x01, 1);   // Usage Page (GD), Usage (Mouse), Collection (Application)
  if (keyboard) item(0x84, kPointerReportId, 1);                   //   Report ID (1)
  item(0x08, 0x01, 1); item(0xa0, 0x00, 1);                        //   Usage (Pointer), Collection (Physical)
  item(0x04, 0x09, 1); item(0x18, 0x01, 1); item(0x28, 0x03, 1);   //     Usage Page (Button), Usage Minimum (1), Maximum (3)
  item(0x14, 0, 1); item(0x24, 1, 1); item(0x74, 1, 1); item(0x94, 3, 1);
  item(0x80, 0x02, 1);                                             //     Input (Data, Variable, Absolute): the buttons
  item(0x74, 5, 1); item(0x94, 1, 1); item(0x80, 0x01, 1);         //     Input (Constant): padding
  item(0x04, 0x01, 1); item(0x08, 0x30, 1); item(0x08, 0x31, 1);   //     Usage Page (GD), Usage (X), Usage (Y)
  item(0x14, 0, 1); item(0x24, kHidMax, 2);                        //     Logical 0..32767
  item(0x34, 0, 1); item(0x44, kHidMax, 2);                        //     Physical 0..32767 (= logical: no resolution)
  item(0x74, 16, 1); item(0x94, 2, 1); item(0x80, 0x02, 1);        //     16 bits x 2, Input (Data, Variable, Absolute)
  item(0x08, 0x38, 1);                                             //     Usage (Wheel)
  item(0x14, -127, 1); item(0x24, 127, 1);                         //     Logical -127..127
  item(0x34, -wheel, 2); item(0x44, wheel, 2);                     //     Physical -W..W
  item(0x64, 0x13, 1); item(0x54, 0x0d, 1);                        //     Unit (inch), Unit Exponent (-3)
  item(0x74, 8, 1); item(0x94, 1, 1); item(0x80, 0x06, 1);         //     8 bits, Input (Data, Variable, Relative)
  item(0x04, 0x0c, 1);                                             //     Usage Page (Consumer)
  d.bytes[n++] = 0x0a; d.bytes[n++] = 0x38; d.bytes[n++] = 0x02;   //     Usage (AC Pan), the Wheel's extents
  item(0x74, 8, 1); item(0x94, 1, 1); item(0x80, 0x06, 1);         //     8 bits, Input (Data, Variable, Relative)
  d.bytes[n++] = 0xc0; d.bytes[n++] = 0xc0;                        //   End Collection, End Collection
  if (keyboard) {
    item(0x64, 0, 1); item(0x54, 0, 1); item(0x34, 0, 1); item(0x44, 0, 1);   // Unit, Unit Exponent, Physical: none again
    item(0x04, 0x01, 1); item(0x08, 0x06, 1); item(0xa0, 0x01, 1);   // Usage Page (GD), Usage (Keyboard), Collection (Application)
    item(0x84, kKeyboardReportId, 1);                                //   Report ID (2)
    item(0x04, 0x07, 1); item(0x18, 0xe0, 1); item(0x28, 0xe7, 1);   //   Usage Page (Keyboard), Left Control .. Right GUI
    item(0x14, 0, 1); item(0x24, 1, 1); item(0x74, 1, 1); item(0x94, 8, 1);
    item(0x80, 0x02, 1);                                             //   Input (Data, Variable, Absolute): the modifiers
    d.bytes[n++] = 0xc0;                                             // End Collection
  }
  d.length = n;
  return d;
}
constexpr TouchDescriptor kTouchDescriptorDefault = makeTouchDescriptor(kTouchScrollResolution, false);

// Desktop pixels (0..w-1, 0..h-1) to the first five bytes: buttons, X and Y in 0..32767.
inline void tabletReport(uint8_t buttons, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t out[kTabletBytes])
{
  auto scale = [](int32_t v, int32_t size) {
    if (v < 0) v = 0;
    if (v > size - 1) v = size - 1;
    return uint32_t(int64_t(v) * kHidMax / (size - 1));
  };
  const uint32_t hx = scale(x, w), hy = scale(y, h);
  out[0] = uint8_t(buttons & 7);
  out[1] = uint8_t(hx);
  out[2] = uint8_t(hx >> 8);
  out[3] = uint8_t(hy);
  out[4] = uint8_t(hy >> 8);
}

// The same under the display's rotation (piano-touch-rotate=1; package/PianoAuto/TouchRotation.h, docs/drivers/sensors.md 5a.3):
// degrees as CGDisplayRotation reports them (90 = the panel's left edge up), the panel contact mapped into the rotated desktop and
// scaled over its size. At 0 the bytes are tabletReport's for the same contact (pat_rotate clamps as tabletReport does).
inline void rotatedTabletReport(uint8_t buttons, int32_t x, int32_t y, int32_t panelW, int32_t panelH, uint32_t degrees,
                                uint8_t out[kTabletBytes])
{
  const PatPoint p = pat_rotate(degrees, x, y, panelW, panelH);
  tabletReport(buttons, p.x, p.y, p.width, p.height, out);
}

inline int8_t countByte(int32_t v) { return int8_t(v < -127 ? -127 : v > 127 ? 127 : v); }

// The pointer report of r (x, y already in the w x h desktop): 7 bytes, or 8 with report ID 1 first. Returns its length.
inline unsigned pointerReport(const PointerReport &r, int32_t w, int32_t h, bool reportIds, uint8_t out[kMaxReportBytes])
{
  uint8_t *p = out;
  if (reportIds) *p++ = kPointerReportId;
  tabletReport(r.buttons, r.x, r.y, w, h, p);
  p[5] = uint8_t(countByte(r.wheel));
  p[6] = uint8_t(countByte(r.pan));
  return reportIds ? kPointerBytes + 1 : kPointerBytes;
}
// The modifiers' report (report ID 2): 2 bytes.
inline unsigned modifierReport(uint8_t modifiers, uint8_t out[kMaxReportBytes])
{
  out[0] = kKeyboardReportId;
  out[1] = modifiers;
  return 2;
}

}  // namespace pianotouch
#endif
