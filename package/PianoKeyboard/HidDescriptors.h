// SPDX-License-Identifier: BSD-2-Clause-Patent
// The two HID devices PianoKeyboard publishes (piano-keyboard=2), one PianoKeyboardHID each, as DroidVMInput publishes one per kind:
//   keyboard  the cover's keyboard (report 5, sent as the controller sends it: modifiers, a reserved byte, six usages 0..0xff; the
//             Num/Caps/Scroll/Compose/Kana LEDs as output report 5, of which Caps Lock goes to the controller) and its consumer keys
//             (report 6: one 16-bit usage 0..0x3ff, the first three bytes of the controller's 5-byte report)
//   pointer   a relative mouse: three buttons, X and Y as signed 16-bit counts, the wheel and AC Pan as signed bytes; no report ID.
//             macOS takes HID pointers, not a touchpad's finger collections. PianoDwc3's USB pointer uses it as it is.
//   touchpad  the same 7-byte report for the cover's touchpad (Wn8030Piano.h Pointer), with physical extents, so that macOS knows the
//             counts' size: IOHIDEventService::determineResolution takes (logical range) / (physical range x 10^exponent) of the
//             relative X element as HIDPointerResolution (counts per inch; 400 without extents) and of the Wheel as
//             HIDScrollResolution (9 without), and its pointer and scroll accelerators work in those units. IOHIDEventDriver reads
//             Wheel and AC Pan as whole counts and has no Resolution Multiplier, so smooth scrolling is finer counts with a
//             correspondingly higher declared scroll resolution (makeTouchpadDescriptor).
// Written for piano from the HID 1.11 specification and the HID Usage Tables (pages 0x01, 0x07, 0x08, 0x09, 0x0C).
#ifndef PIANO_KEYBOARD_HID_DESCRIPTORS_H
#define PIANO_KEYBOARD_HID_DESCRIPTORS_H
#include <stdint.h>

namespace pianokeyboard {

// Vendor 0x1AF4 is DroidVMInput's (whose IOHIDDevice class PianoKeyboardHID is) and PianoInput's; the product 'PK' is PianoKeyboard's
// own, shared by both devices as DroidVMInput's devices share theirs. tooling/k27/kext/PianoKeyboard.json's codeless
// IOHIDEventDriver personality matches the pair.
constexpr uint32_t kVendorId = 0x1af4, kProductId = 0x504b;

enum class Kind : uint8_t { keyboard, pointer, touchpad };

constexpr uint8_t kKeyboardDescriptor[] = {
  0x05, 0x01,         // Usage Page (Generic Desktop)
  0x09, 0x06,         // Usage (Keyboard)
  0xa1, 0x01,         // Collection (Application)
  0x85, 0x05,         //   Report ID (5)
  0x05, 0x07,         //   Usage Page (Keyboard/Keypad)
  0x19, 0xe0,         //   Usage Minimum (Left Control)
  0x29, 0xe7,         //   Usage Maximum (Right GUI)
  0x15, 0x00,         //   Logical Minimum (0)
  0x25, 0x01,         //   Logical Maximum (1)
  0x75, 0x01,         //   Report Size (1)
  0x95, 0x08,         //   Report Count (8)
  0x81, 0x02,         //   Input (Data, Variable, Absolute): the modifiers
  0x75, 0x08,         //   Report Size (8)
  0x95, 0x01,         //   Report Count (1)
  0x81, 0x01,         //   Input (Constant): reserved
  0x05, 0x08,         //   Usage Page (LEDs)
  0x19, 0x01,         //   Usage Minimum (Num Lock)
  0x29, 0x05,         //   Usage Maximum (Kana)
  0x75, 0x01,         //   Report Size (1)
  0x95, 0x05,         //   Report Count (5)
  0x91, 0x02,         //   Output (Data, Variable, Absolute)
  0x75, 0x03,         //   Report Size (3)
  0x95, 0x01,         //   Report Count (1)
  0x91, 0x01,         //   Output (Constant): padding
  0x05, 0x07,         //   Usage Page (Keyboard/Keypad)
  0x19, 0x00,         //   Usage Minimum (0)
  0x2a, 0xff, 0x00,   //   Usage Maximum (255)
  0x15, 0x00,         //   Logical Minimum (0)
  0x26, 0xff, 0x00,   //   Logical Maximum (255)
  0x75, 0x08,         //   Report Size (8)
  0x95, 0x06,         //   Report Count (6)
  0x81, 0x00,         //   Input (Data, Array): the keys
  0xc0,               // End Collection
  0x05, 0x0c,         // Usage Page (Consumer)
  0x09, 0x01,         // Usage (Consumer Control)
  0xa1, 0x01,         // Collection (Application)
  0x85, 0x06,         //   Report ID (6)
  0x19, 0x00,         //   Usage Minimum (0)
  0x2a, 0xff, 0x03,   //   Usage Maximum (0x3ff)
  0x15, 0x00,         //   Logical Minimum (0)
  0x26, 0xff, 0x03,   //   Logical Maximum (0x3ff)
  0x75, 0x10,         //   Report Size (16)
  0x95, 0x01,         //   Report Count (1)
  0x81, 0x00,         //   Input (Data, Array)
  0xc0,               // End Collection
};

constexpr uint8_t kPointerDescriptor[] = {
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
  0x81, 0x02,         //     Input (Data, Variable, Absolute)
  0x75, 0x05,         //     Report Size (5)
  0x95, 0x01,         //     Report Count (1)
  0x81, 0x01,         //     Input (Constant): padding
  0x05, 0x01,         //     Usage Page (Generic Desktop)
  0x09, 0x30,         //     Usage (X)
  0x09, 0x31,         //     Usage (Y)
  0x16, 0x01, 0x80,   //     Logical Minimum (-32767)
  0x26, 0xff, 0x7f,   //     Logical Maximum (32767)
  0x75, 0x10,         //     Report Size (16)
  0x95, 0x02,         //     Report Count (2)
  0x81, 0x06,         //     Input (Data, Variable, Relative)
  0x09, 0x38,         //     Usage (Wheel)
  0x15, 0x81,         //     Logical Minimum (-127)
  0x25, 0x7f,         //     Logical Maximum (127)
  0x75, 0x08,         //     Report Size (8)
  0x95, 0x01,         //     Report Count (1)
  0x81, 0x06,         //     Input (Data, Variable, Relative)
  0x05, 0x0c,         //     Usage Page (Consumer)
  0x0a, 0x38, 0x02,   //     Usage (AC Pan)
  0x15, 0x81,         //     Logical Minimum (-127)
  0x25, 0x7f,         //     Logical Maximum (127)
  0x75, 0x08,         //     Report Size (8)
  0x95, 0x01,         //     Report Count (1)
  0x81, 0x06,         //     Input (Data, Variable, Relative)
  0xc0,               //   End Collection
  0xc0,               // End Collection
};

// The touchpad's descriptor: kPointerDescriptor's report with Physical Minimum/Maximum, Unit (inch) and Unit Exponent on X/Y
// (exponent -2) and the Wheel (exponent -3), chosen so that determineResolution's integer quotient is exactly `dpi` and
// `scrollResolution` (counts per inch). dpi is clamped to 120..4000, scrollResolution to 10..900 (inside those the physical extents
// never equal the logical ones, which would make macOS ignore them); scrollResolution 0 declares none (macOS's default 9). The
// AC Pan inherits the Wheel's extents (macOS reads only the Wheel's).
struct TouchpadDescriptor {
  uint8_t bytes[112];
  unsigned length;
};
constexpr uint32_t touchpadDpi(uint32_t dpi) { return dpi < 120 ? 120 : dpi > 4000 ? 4000 : dpi; }
constexpr uint32_t touchpadScrollResolution(uint32_t r) { return !r ? 0 : r < 10 ? 10 : r > 900 ? 900 : r; }
constexpr TouchpadDescriptor makeTouchpadDescriptor(uint32_t dpi, uint32_t scrollResolution)
{
  TouchpadDescriptor d = {};
  unsigned n = 0;
  auto item = [&](uint8_t tag, int32_t value, unsigned size) {   // a short item of 1, 2 (or 0) data bytes
    d.bytes[n++] = uint8_t(tag | (size == 2 ? 2 : size));
    for (unsigned i = 0; i < size; ++i) d.bytes[n++] = uint8_t(uint32_t(value) >> (8 * i));
  };
  dpi = touchpadDpi(dpi);
  scrollResolution = touchpadScrollResolution(scrollResolution);
  const int32_t xy = int32_t(6553400 / (2 * dpi));                                     // 65534 x 100 / (2P) = dpi
  const int32_t wheel = scrollResolution ? int32_t(254000 / (2 * scrollResolution)) : 127;   // 254 x 1000 / (2P) = resolution
  item(0x04, 0x01, 1); item(0x08, 0x02, 1); item(0xa0, 0x01, 1);          // Usage Page (GD), Usage (Mouse), Collection (Application)
  item(0x08, 0x01, 1); item(0xa0, 0x00, 1);                               //   Usage (Pointer), Collection (Physical)
  item(0x04, 0x09, 1); item(0x18, 0x01, 1); item(0x28, 0x03, 1);          //     Usage Page (Button), Usage Minimum (1), Maximum (3)
  item(0x14, 0x00, 1); item(0x24, 0x01, 1); item(0x74, 0x01, 1); item(0x94, 0x03, 1);
  item(0x80, 0x02, 1);                                                    //     Input (Data, Variable, Absolute): 3 buttons
  item(0x74, 0x05, 1); item(0x94, 0x01, 1); item(0x80, 0x01, 1);          //     Input (Constant): 5 bits
  item(0x04, 0x01, 1); item(0x08, 0x30, 1); item(0x08, 0x31, 1);          //     Usage Page (GD), Usage (X), Usage (Y)
  item(0x14, -32767, 2); item(0x24, 32767, 2);                            //     Logical -32767..32767
  item(0x34, -xy, 2); item(0x44, xy, 2);                                  //     Physical -P..P
  item(0x64, 0x13, 1); item(0x54, 0x0e, 1);                               //     Unit (inch), Unit Exponent (-2)
  item(0x74, 0x10, 1); item(0x94, 0x02, 1); item(0x80, 0x06, 1);          //     16 bits x 2, Input (Data, Variable, Relative)
  item(0x08, 0x38, 1);                                                    //     Usage (Wheel)
  item(0x14, -127, 1); item(0x24, 127, 1);                                //     Logical -127..127
  item(0x34, -wheel, 2); item(0x44, wheel, 2); item(0x54, 0x0d, 1);       //     Physical -W..W, Unit Exponent (-3)
  item(0x74, 0x08, 1); item(0x94, 0x01, 1); item(0x80, 0x06, 1);          //     8 bits, Input (Data, Variable, Relative)
  item(0x04, 0x0c, 1); d.bytes[n++] = 0x0a; d.bytes[n++] = 0x38; d.bytes[n++] = 0x02;   //     Usage Page (Consumer), Usage (AC Pan)
  item(0x74, 0x08, 1); item(0x94, 0x01, 1); item(0x80, 0x06, 1);          //     8 bits, Input (Data, Variable, Relative)
  d.bytes[n++] = 0xc0; d.bytes[n++] = 0xc0;                               //   End Collection, End Collection
  d.length = n;
  return d;
}
// The defaults: 400 dpi declared for 865 sent (Wn8030Piano.h PointerConfig: 2.2 times a 400-dpi mouse's travel at the same
// acceleration, for a 94 mm pad), and 90 scroll counts per inch for ten counts per former wheel step.
constexpr uint32_t kTouchpadDpi = 400, kTouchpadScrollResolution = 90;
constexpr TouchpadDescriptor kTouchpadDefault = makeTouchpadDescriptor(kTouchpadDpi, kTouchpadScrollResolution);

struct DeviceInfo {
  const char *product;
  uint32_t usagePage, usage;
  const uint8_t *descriptor;
  unsigned descriptorBytes;
};
constexpr DeviceInfo kDevices[] = {
  {"Piano Keyboard Cover", 0x01, 0x06, kKeyboardDescriptor, sizeof kKeyboardDescriptor},    // Kind::keyboard
  {"Piano Keyboard Touchpad", 0x01, 0x02, kPointerDescriptor, sizeof kPointerDescriptor},   // Kind::pointer (PianoDwc3's)
  {"Piano Keyboard Touchpad", 0x01, 0x02, kTouchpadDefault.bytes, kTouchpadDefault.length},  // Kind::touchpad (PianoKeyboard's)
};
inline const DeviceInfo &deviceInfo(Kind kind) { return kDevices[unsigned(kind)]; }

// Output report 5's LED bits (usages 1..5 from bit 0): Caps Lock is bit 1.
constexpr uint8_t kLedCapsLock = 1u << 1;

}  // namespace pianokeyboard
#endif
