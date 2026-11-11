// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano's three physical keys, read from the PMICs (the local bring-up notes, touch-native-plan.md section 3; the same registers
// as piano-uefi's KeypadDeviceLib, on which the UniXNU menu runs at EL1 on this tablet):
//   power  PMK8550 PON, SID 0 base 0x1300 (PPID 0x013), INT_RT_STS 0x10 bit 7 (KPDPWR), active high
//   vol-   the same register, bit 6 (RESIN), active high
//   vol+   PM8550 GPIO6, SID 1 base 0x8D00 (PPID 0x18D), INT_RT_STS 0x10 bit 0, active low
// Polled and debounced here; no interrupt is configured and no PMIC register is written. The polling adapts (docs/power-irq.md):
// every kPollMs while a key is down or changing and for kActiveHoldMs after, every piano-input-idle ms otherwise.
#ifndef PIANO_INPUT_BUTTONS_H
#define PIANO_INPUT_BUTTONS_H
#include <stdint.h>
#include "HidReports.h"
#include "../common/PianoIrq.h"

namespace pianoinput {

constexpr uint16_t kPonPpid = 0x013, kVolUpGpioPpid = 0x18d;
constexpr uint8_t kRegType = 0x04, kRegSubtype = 0x05, kRegRtStatus = 0x10, kRegGpioMode = 0x40, kRegGpioEnable = 0x46;
// The peripherals' interrupt block (read once at start, for the interrupt path's facts; never written): SET_TYPE, POLARITY_HIGH,
// POLARITY_LOW, EN_SET (reads back the enables), LATCHED_STS, PENDING_STS.
constexpr uint8_t kRegIntSetType = 0x11, kRegIntPolarityHigh = 0x12, kRegIntPolarityLow = 0x13, kRegIntEnable = 0x15,
                  kRegIntLatched = 0x18, kRegIntPending = 0x19;
constexpr uint8_t kPonPower = 0x80, kPonVolumeDown = 0x40, kGpioLevel = 0x01;
constexpr uint32_t kPollMs = 10, kDebounceMs = 32;
// Adaptive polling: the idle period's default, and how long after the last key activity the 10 ms polling continues. A press is
// seen when it lasts at least the idle period plus the debounce (40 + 32 + one 10 ms sample: ~82 ms; a deliberate press is
// 100 ms or more); the first sample of a press is at most one idle period late.
constexpr uint32_t kIdlePollMs = 40, kActiveHoldMs = 1000, kIdlePollMaxMs = 200;

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
  // A key reads down, is held down, or a change is being debounced: poll fast.
  bool busy() const { return candidate || stable || !settled; }
};

// The poll loop's period: kPollMs while the keys are busy and kActiveHoldMs after, then idleMs (piano-input-idle; 0, or anything
// not above kPollMs: kPollMs always, the old behaviour; capped at kIdlePollMaxMs).
struct PollPlan {
  pianoirq::Backoff backoff;
  explicit PollPlan(uint32_t idleMs)
    : backoff(pianoirq::backoff(kPollMs, clampIdle(idleMs), clampIdle(idleMs), kActiveHoldMs, 0)) {}
  static uint32_t clampIdle(uint32_t ms) { return ms <= kPollMs ? kPollMs : ms > kIdlePollMaxMs ? kIdlePollMaxMs : ms; }
  bool adaptive() const { return backoff.idleMs > kPollMs; }
  // After a sample at nowMs: the sleep before the next one.
  uint32_t next(const Debounce &f, uint64_t nowMs)
  {
    if (f.busy()) backoff.activity(nowMs);
    return backoff.periodMs(nowMs);
  }
  bool fast(uint64_t nowMs) const { return backoff.tier(nowMs) == 0; }
};

}  // namespace pianoinput
#endif
