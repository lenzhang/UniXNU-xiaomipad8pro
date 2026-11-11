// SPDX-License-Identifier: BSD-2-Clause-Patent
// The power key with PianoPolicy (docs/policy.md 5.1): what the buttons report tells macOS about it, and when PianoPolicy is asked.
// Portable (no kernel headers): PianoInput.cpp feeds it the debounced key after every sample, tests/buttons_host.cpp drives it.
//
//   no PianoPolicy (hold false at the press)   the press is reported as it happens, down and up (as before this existed)
//   PianoPolicy there (hold true at the press) nothing is reported at the press;
//                                              still down kLongPressMs after it: a long press, reported down now, up at the release
//                                              (PianoPolicy is not asked); released before: ask, with the press's length in ms.
//                                              The caller sends PianoPolicy "piano-policy-power-key"; taken: nothing reaches macOS;
//                                              not taken: the HID Power usage down and up at once (macOS's power key, late by the press)
// The choice is made at the press: a press that started reported stays so until its release. reset() (after a system wake) forgets
// a press in progress; the report then says the key is up (the kernel held the press that woke the tablet until its release).
#ifndef PIANO_INPUT_POWER_KEY_H
#define PIANO_INPUT_POWER_KEY_H
#include <stdint.h>
#include "HidReports.h"

namespace pianoinput {

constexpr uint32_t kLongPressMs = 1500;   // PianoPolicy's pianopolicy::kLongPressMs

enum class PowerStep : uint8_t { none, ask };

class PowerKey {
 public:
  // After a debounced sample at ms: down = the power key debounced down (Debounce's output), hold = a PianoPolicy service is there
  // and the power key is reported to macOS at all (piano-input-mask). ask: a held-back short press ended, *heldMs its length.
  PowerStep update(bool down, bool hold, uint64_t ms, uint32_t *heldMs)
  {
    switch (state_) {
    case State::up:
      if (!down) return PowerStep::none;
      if (hold) {
        state_ = State::held;
        since_ = ms;
      } else {
        state_ = State::passThrough;
        reported_ = true;
      }
      return PowerStep::none;
    case State::held:
      if (down) {
        if (ms >= since_ && ms - since_ >= kLongPressMs) {
          state_ = State::longPress;
          reported_ = true;
          ++longPresses_;
        }
        return PowerStep::none;
      }
      *heldMs = uint32_t(ms >= since_ ? (ms - since_ > 0xffffffffull ? 0xffffffffull : ms - since_) : 0);
      state_ = State::up;
      ++shortPresses_;
      return PowerStep::ask;
    case State::longPress:
    case State::passThrough:
      if (!down) {
        state_ = State::up;
        reported_ = false;
      }
      return PowerStep::none;
    }
    return PowerStep::none;
  }

  // The report's bits for macOS: the other keys as debounced (and masked), the power bit as this machine says.
  uint8_t report(uint8_t down, uint8_t mask) const
  {
    return uint8_t((down & mask & ~kKeyPower) | (reported_ && (mask & kKeyPower) ? kKeyPower : 0));
  }

  // This sample starts a press (the key down with none in progress): the moment to decide whether to hold it back.
  bool starts(bool down) const { return down && state_ == State::up; }
  bool reported() const { return reported_; }
  bool holding() const { return state_ == State::held; }
  void reset()
  {
    state_ = State::up;
    reported_ = false;
  }
  uint32_t shortPresses() const { return shortPresses_; }
  uint32_t longPresses() const { return longPresses_; }

 private:
  enum class State : uint8_t { up, passThrough, held, longPress };
  State state_ = State::up;
  uint64_t since_ = 0;
  bool reported_ = false;
  uint32_t shortPresses_ = 0, longPresses_ = 0;
};

}  // namespace pianoinput
#endif
