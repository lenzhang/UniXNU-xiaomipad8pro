// SPDX-License-Identifier: BSD-2-Clause-Patent
// The two HID devices PianoKeyboard publishes (piano-keyboard=2), one PianoKeyboardHID each, as DroidVMInput publishes one per kind:
//   keyboard  the cover's keyboard (report 5, sent as the controller sends it: modifiers, a reserved byte, six usages 0..0xff; the
//             Num/Caps/Scroll/Compose/Kana LEDs as output report 5, of which Caps Lock goes to the controller) and its consumer keys
//             (report 6: one 16-bit usage 0..0x3ff, the first three bytes of the controller's 5-byte report)
//   pointer   the touchpad as a relative mouse (Wn8030Piano.h Pointer): three buttons, X and Y as signed 16-bit counts, the wheel and
//             AC Pan as signed bytes; no report ID. macOS takes HID pointers, not a touchpad's finger collections.
// Written for piano from the HID 1.11 specification and the HID Usage Tables (pages 0x01, 0x07, 0x08, 0x09, 0x0C).
#ifndef PIANO_KEYBOARD_HID_DESCRIPTORS_H
#define PIANO_KEYBOARD_HID_DESCRIPTORS_H
#include <stdint.h>

namespace pianokeyboard {

// Vendor 0x1AF4 is DroidVMInput's (whose IOHIDDevice class PianoKeyboardHID is) and PianoInput's; the product 'PK' is PianoKeyboard's
// own, shared by both devices as DroidVMInput's devices share theirs. tooling/k27/kext/PianoKeyboard.json's codeless
// IOHIDEventDriver personality matches the pair.
constexpr uint32_t kVendorId = 0x1af4, kProductId = 0x504b;

enum class Kind : uint8_t { keyboard, pointer };

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

struct DeviceInfo {
  const char *product;
  uint32_t usagePage, usage;
  const uint8_t *descriptor;
  unsigned descriptorBytes;
};
constexpr DeviceInfo kDevices[] = {
  {"Piano Keyboard Cover", 0x01, 0x06, kKeyboardDescriptor, sizeof kKeyboardDescriptor},    // Kind::keyboard
  {"Piano Keyboard Touchpad", 0x01, 0x02, kPointerDescriptor, sizeof kPointerDescriptor},   // Kind::pointer
};
inline const DeviceInfo &deviceInfo(Kind kind) { return kDevices[unsigned(kind)]; }

// Output report 5's LED bits (usages 1..5 from bit 0): Caps Lock is bit 1.
constexpr uint8_t kLedCapsLock = 1u << 1;

}  // namespace pianokeyboard
#endif
