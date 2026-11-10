// SPDX-License-Identifier: BSD-2-Clause-Patent
// The HID devices PianoInput publishes, one PianoHIDDevice each (as DroidVMInput publishes one IOHIDDevice per input kind):
//   buttons  a Consumer Control: Power (0x30), Volume Increment (0xE9), Volume Decrement (0xEA), one bit each, no report ID.
//            A consumer-only device: IOHIDEventDriver posts the keys and Keyboard Setup Assistant has nothing to ask about.
//   touch    (later: NT36532E over GENI SPI) a second device of its own, Digitizer/Touch Screen as its primary usage, Built-In and
//            displayUUID set, as DroidVMInput's digitizer; its descriptor and reports go next to the buttons' here.
// Descriptor written for piano from the HID 1.11 specification and the HID Usage Tables (consumer page 0x0C).
#ifndef PIANO_INPUT_HID_REPORTS_H
#define PIANO_INPUT_HID_REPORTS_H
#include <stdint.h>

namespace pianoinput {

enum class Kind : uint8_t { buttons };

// Vendor 6900 (0x1AF4) is DroidVMInput's, whose class this is; the product IDs are PianoInput's own ('PB'; 'PT' for touch).
// tooling/k27/kext/PianoInput.json's codeless IOHIDEventDriver personality matches the buttons' pair.
constexpr uint32_t kVendorId = 0x1af4, kProductButtons = 0x5042, kProductTouch = 0x5054;

enum : uint8_t { kKeyPower = 1u << 0, kKeyVolumeUp = 1u << 1, kKeyVolumeDown = 1u << 2, kKeysAll = 7 };

constexpr uint8_t kButtonsDescriptor[] = {
  0x05, 0x0c,         // Usage Page (Consumer)
  0x09, 0x01,         // Usage (Consumer Control)
  0xa1, 0x01,         // Collection (Application)
  0x15, 0x00,         //   Logical Minimum (0)
  0x25, 0x01,         //   Logical Maximum (1)
  0x75, 0x01,         //   Report Size (1)
  0x95, 0x03,         //   Report Count (3)
  0x09, 0x30,         //   Usage (Power)             bit 0
  0x09, 0xe9,         //   Usage (Volume Increment)  bit 1
  0x09, 0xea,         //   Usage (Volume Decrement)  bit 2
  0x81, 0x02,         //   Input (Data, Variable, Absolute)
  0x95, 0x05,         //   Report Count (5)
  0x81, 0x01,         //   Input (Constant): padding to a byte
  0xc0,               // End Collection
};
constexpr unsigned kButtonsReportBytes = 1;

// What each PianoHIDDevice says about itself, by Kind (touch adds its row here).
struct DeviceInfo {
  const char *product;
  uint32_t productId, usagePage, usage;
  const uint8_t *descriptor;
  unsigned descriptorBytes;
};
constexpr DeviceInfo kDevices[] = {
  {"Piano Buttons", kProductButtons, 0x0c, 0x01, kButtonsDescriptor, sizeof kButtonsDescriptor},   // Kind::buttons
};
inline const DeviceInfo &deviceInfo(Kind kind) { return kDevices[unsigned(kind)]; }

inline const char *keyName(uint8_t bit)
{
  return bit == kKeyPower ? "power" : bit == kKeyVolumeUp ? "vol+" : bit == kKeyVolumeDown ? "vol-" : "?";
}

}  // namespace pianoinput
#endif
