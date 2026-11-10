// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano's three physical keys, read from the PMICs (droidvm-build/piano-patches/touch-native-plan.md section 3; the same registers
// as piano-uefi's KeypadDeviceLib, on which the UniXNU menu runs at EL1 on this tablet):
//   power  PMK8550 PON, SID 0 base 0x1300 (PPID 0x013), INT_RT_STS 0x10 bit 7 (KPDPWR), active high
//   vol-   the same register, bit 6 (RESIN), active high
//   vol+   PM8550 GPIO6, SID 1 base 0x8D00 (PPID 0x18D), INT_RT_STS 0x10 bit 0, active low
// Polled and debounced here; no interrupt is configured and no PMIC register is written.
#ifndef PIANO_INPUT_BUTTONS_H
#define PIANO_INPUT_BUTTONS_H
#include <stdint.h>
#include "HidReports.h"

namespace pianoinput {

constexpr uint16_t kPonPpid = 0x013, kVolUpGpioPpid = 0x18d;
constexpr uint8_t kRegType = 0x04, kRegSubtype = 0x05, kRegRtStatus = 0x10, kRegGpioMode = 0x40, kRegGpioEnable = 0x46;
constexpr uint8_t kPonPower = 0x80, kPonVolumeDown = 0x40, kGpioLevel = 0x01;
constexpr uint32_t kPollMs = 10, kDebounceMs = 32;

// The keys held in one sample, as the bits of the buttons report.
inline uint8_t decodeKeys(uint8_t pon, uint8_t gpio)
{
  return uint8_t(((pon & kPonPower) ? kKeyPower : 0) | ((pon & kPonVolumeDown) ? kKeyVolumeDown : 0) |
                 ((gpio & kGpioLevel) ? 0 : kKeyVolumeUp));
}

// A key changes state after its new state has been read for kDebounceMs. A sample that could not be read counts as all keys up,
// so a bus fault longer than the debounce releases everything instead of leaving a key stuck down. A key down in the first
// stable state (held at start, or a level that reads as held when it is not) is reported only after it has been up once.
struct Debounce {
  uint8_t candidate = 0, stable = 0, armed = 0;
  bool primed = false, settled = false;   // settled: `stable` holds a debounced state
  uint64_t since = 0;

  // Returns the keys to report as down after the sample taken at `ms` (milliseconds, any origin).
  uint8_t update(bool valid, uint8_t keys, uint64_t ms)
  {
    keys = valid ? uint8_t(keys & kKeysAll) : 0;
    if (!primed || keys != candidate || ms < since) {
      candidate = keys;
      since = ms;
      primed = true;
    } else if (ms - since >= kDebounceMs) {
      stable = candidate;
      settled = true;
    }
    if (settled) armed |= uint8_t(~stable & kKeysAll);
    return uint8_t(stable & armed);
  }
};

}  // namespace pianoinput
#endif
