// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDwc3 across a system sleep (docs/power.md 4, PianoPowerContract.h): what setPowerState(off) and (on) do to the gadget, and
// the guard that keeps every QSCRATCH write (the VBUS/session override, the UTMI clock select) away from a glue that may be
// unclocked. Ubuntu's K:7dffd78b342d: a VBUS-override write that ran while the DWC3 glue's clocks were off during s2idle never
// completed and the SoC reset; the fix records the override while suspended and applies it once the glue is back. Portable
// (tests/usb_sleep_host.cpp); the USB thread carries the steps out, setPowerState waits for off.
//
//   off, the takeover done:
//     keep     piano-sleep-usb=1 (the default) and a host has configured the device: the gadget stays as it is, so the console
//              survives (the Mac sees a device that does not answer while frozen and gets the backlog after the wake). The core's
//              interrupt is masked at its source (GEVNTSIZ) and the thread goes on polling until the freeze: no USB event can end a
//              suspend.
//     quiesce  otherwise (no host, or piano-sleep-usb=0): a soft disconnect (RUN_STOP off: the Mac sees a detach; a cable plugged
//              in while suspended is not enumerated half-way), the endpoints and the address forgotten, the interrupt masked; the
//              thread stops touching the core.
//   off before the takeover finished (piano-usb-delay): nothing to do; the takeover waits for the wake.
//   on: the glue's clocks are read first (GCC: the GDSC on, the master and AXI branches running). Off: the core is not touched
//     again this boot (stuck; logged). On: keep -> the passes as before (the interrupt unmasked by the next wait); quiesce -> the
//     events drained, the device side set up again, RUN_STOP on (the Mac enumerates afresh); and the QSCRATCH writes recorded
//     while asleep are applied, the last value per register, in the order first asked.
//   QSCRATCH: a write goes to the glue only while awake with the glue clocked; otherwise it is recorded.
#pragma once
#include <stdint.h>

namespace pianousb {

enum class SleepState : uint8_t { awake, early, kept, quiesced, stuck };
constexpr const char *sleepStateName(SleepState s)
{
  return s == SleepState::awake ? "awake" : s == SleepState::early ? "asleep before the takeover" : s == SleepState::kept ? "asleep, kept"
         : s == SleepState::quiesced ? "asleep, disconnected" : "stuck (the glue was unclocked at the wake)";
}

struct SleepOff {
  bool halt = false;          // soft disconnect: RUN_STOP off, the endpoints and the address forgotten
  bool maskIrq = false;       // GEVNTSIZ.INTMASK set (only when the core's interrupt is attached)
  bool keepPolling = false;   // the thread goes on with its passes, polled, until the freeze
};
struct SleepOn {
  bool touch = false;       // the core may be touched again
  bool reconnect = false;   // drain, the device side again, RUN_STOP on
  bool replay = false;      // apply the QSCRATCH writes recorded while asleep
};

class UsbSleep {
 public:
  static constexpr unsigned kRecorded = 4;
  explicit UsbSleep(bool keep = true) : keep_(keep) {}

  // setPowerState(off). running: the takeover is done; configured: a host has configured the device; irq: the core's interrupt is
  // attached (piano-usb-irq=1). A second off before an on does nothing.
  SleepOff off(bool running, bool configured, bool irq)
  {
    SleepOff o;
    if (state_ != SleepState::awake) return o;
    ++sleeps_;
    if (!running) {
      state_ = SleepState::early;
      return o;
    }
    o.maskIrq = irq;
    if (keep_ && configured) {
      state_ = SleepState::kept;
      o.keepPolling = true;
    } else {
      state_ = SleepState::quiesced;
      o.halt = true;
    }
    return o;
  }

  // setPowerState(on). clocked: the glue's clocks read on just now. An on without an off (the power tree's first) does nothing.
  SleepOn on(bool clocked)
  {
    SleepOn o;
    if (state_ == SleepState::awake || state_ == SleepState::stuck) return o;
    ++wakes_;
    if (!clocked && state_ != SleepState::early) {
      state_ = SleepState::stuck;
      return o;
    }
    const bool quiesced = state_ == SleepState::quiesced;
    state_ = SleepState::awake;
    o.touch = true;
    o.reconnect = quiesced;
    o.replay = count_ != 0 && clocked;
    return o;
  }

  // May this QSCRATCH write go to the glue now? Only awake with the glue clocked; else it is recorded (the last value per
  // register) for on() to replay.
  bool qscratchWrite(uint32_t offset, uint32_t value, bool clocked)
  {
    if (state_ == SleepState::awake && clocked) return true;
    ++deferred_;
    for (unsigned i = 0; i < count_; ++i)
      if (offset_[i] == offset) {
        value_[i] = value;
        return false;
      }
    if (count_ < kRecorded) {
      offset_[count_] = offset;
      value_[count_] = value;
      ++count_;
    } else {
      ++lost_;
    }
    return false;
  }
  unsigned recorded() const { return count_; }
  uint32_t recordedOffset(unsigned i) const { return i < count_ ? offset_[i] : 0; }
  uint32_t recordedValue(unsigned i) const { return i < count_ ? value_[i] : 0; }
  void replayed() { count_ = 0; }

  SleepState state() const { return state_; }
  // The USB thread may touch the core (awake, or kept: polling on until the freeze).
  bool coreUsable() const { return state_ == SleepState::awake || state_ == SleepState::kept; }
  // The takeover may start (not asleep).
  bool takeoverMayRun() const { return state_ == SleepState::awake; }
  bool keep() const { return keep_; }
  uint32_t sleeps() const { return sleeps_; }
  uint32_t wakes() const { return wakes_; }
  uint32_t deferred() const { return deferred_; }
  uint32_t lost() const { return lost_; }

 private:
  bool keep_;
  SleepState state_ = SleepState::awake;
  uint32_t offset_[kRecorded] = {}, value_[kRecorded] = {};
  unsigned count_ = 0;
  uint32_t sleeps_ = 0, wakes_ = 0, deferred_ = 0, lost_ = 0;
};

}  // namespace pianousb
