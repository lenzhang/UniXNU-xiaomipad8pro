// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoKeyboard's power policy parts (docs/policy.md 5.4, docs/power.md 4), portable and host-tested (tests/keyboard_host.cpp):
//
//   InputGate    piano-keyboard-lid-drop=1: while the hall sensor reads closed, key and touchpad reports are dropped (Linux's
//                input-off-while-closed: a closed cover pressed in a bag neither types nor wakes the display); the caller releases
//                every key and button once when the gate closes.
//   latch...     the keyboard's wake line for a system sleep (k27 0062 piano-sleep-wake bit 16): gpio95's INTR_CFG with raw status
//                and a rising edge latched, enable off (nothing reaches the GIC), armed only when PianoPolicy says so (acting:
//                "piano-policy-keyboard-wake"), else by the driver's own rule (the MCU running, a cover attached, the cover not read
//                closed); not armed = raw status off, so 0062 leaves bit 16 out. Restored as found at the wake.
//   CoverPower   piano-keyboard-blank=1: the MCU's power follows the display as Ubuntu's driver does (the stock offsets): display
//                off -> keyboard power 0x25 = 0, then the sleep line (gpio3) asserted, no keep-alive, no frame read; display on ->
//                the sleep line released, gpio97 awaited (<= 100 ms; 5 ms more if it had to wait), pending frames drained, 0x25 = 1,
//                LEDs and backlight again. A system sleep blanks too (if the display has not); the wake unblanks only when the
//                display is on or not known (a resume without a display-on sends nothing). gpio95 rising while blanked (the MCU's wake
//                request) wakes the MCU early only when the display may wake now (PianoPolicy's `now`, else the own rule); if the
//                display stays off for kEarlyWakeMs after that, the MCU is blanked again.
#ifndef PIANO_KEYBOARD_COVER_POWER_H
#define PIANO_KEYBOARD_COVER_POWER_H
#include <stdint.h>

namespace pianokeyboard {

// ------------------------------------------------------------------------------------------------------------- the input gate
class InputGate {
 public:
  void configure(bool enabled) { enabled_ = enabled; }
  // The accepted hall level (1 open, 0 closed, -1 not read). True when the gate has just closed (release everything once).
  bool hall(int level)
  {
    const bool closed = enabled_ && level == 0;
    const bool closing = closed && !closed_;
    if (closed != closed_) ++changes_;
    closed_ = closed;
    if (closing) droppedNow_ = 0;
    return closing;
  }
  // A key, consumer, touchpad or mouse report: true when it may go on; false when dropped (counted).
  bool pass()
  {
    if (!closed_) return true;
    ++dropped_;
    ++droppedNow_;
    return false;
  }
  bool enabled() const { return enabled_; }
  bool closed() const { return closed_; }
  uint32_t dropped() const { return dropped_; }       // the whole boot
  uint32_t droppedNow() const { return droppedNow_; } // since the gate last closed
  uint32_t changes() const { return changes_; }

 private:
  bool enabled_ = false, closed_ = false;
  uint32_t dropped_ = 0, droppedNow_ = 0, changes_ = 0;
};

// ------------------------------------------------------------------------------------------------------------- gpio95's latch
// TLMM INTR_CFG (docs/power-irq.md 2.3): bit 0 enable, 1 polarity, 3:2 detection (1 rising edge), 4 raw status.
constexpr uint32_t kIntrEnable = 1u << 0, kIntrPolarityHigh = 1u << 1, kIntrEdge = 1u << 2, kIntrDetectMask = 3u << 2, kIntrRaw = 1u << 4;

// Armed: rising edges latched into INTR_STATUS (raw status, edge, polarity high), enable off; the other bits as found. Not armed: raw
// status and enable off (0062 arms bit 16 only with the raw status bit set), the rest as found.
inline uint32_t latchCfg(uint32_t found, bool arm)
{
  if (!arm) return found & ~(kIntrRaw | kIntrEnable);
  return (found & ~(kIntrEnable | kIntrPolarityHigh | kIntrDetectMask | kIntrRaw)) | kIntrRaw | kIntrEdge | kIntrPolarityHigh;
}

struct LatchDecision {
  bool arm;
  const char *why;
};
// At a system sleep. serving: the MCU runs (step 6); answered: an acting PianoPolicy answered "piano-policy-keyboard-wake" with
// policyLatch; hall: 1 open, 0 closed, -1 not read; attached: the MCU's attach bit.
inline LatchDecision latchDecision(bool serving, bool answered, bool policyLatch, int hall, bool attached)
{
  if (!serving) return {false, "the MCU is not running"};
  if (answered) return {policyLatch, policyLatch ? "PianoPolicy: cover open and attached, keyboard wake armed in the kernel"
                                                 : "PianoPolicy: no keyboard wake now"};
  if (!attached) return {false, "no cover attached (own rule)"};
  if (hall == 0) return {false, "the cover is closed (own rule)"};
  return {true, "cover open and attached (own rule; the kernel arms it only with piano-sleep-wake bit 16)"};
}
// gpio95 rising while the display is off: may it wake the display (and the MCU) now? PianoPolicy's `now` when it acts, else the
// own rule (a cover attached and not read closed).
inline bool wakeNow(bool answered, bool policyNow, int hall, bool attached)
{
  return answered ? policyNow : attached && hall != 0;
}

// ------------------------------------------------------------------------------------------------------------- the MCU's power
class CoverPower {
 public:
  enum class Step : uint8_t { none, blank, unblank };
  static constexpr uint64_t kEarlyWakeMs = 10000;

  void configure(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }
  bool running() const { return running_; }
  bool asleep() const { return asleep_; }
  int display() const { return display_; }

  // NativeGopDisplay's piano-display-power: 1 on, 0 off, -1 not known (no NativeGopDisplay, or before WindowServer's first call).
  Step display(int state, uint64_t nowMs)
  {
    if (state == display_) return Step::none;
    display_ = state;
    if (state != 0) early_ = false;
    return settle(nowMs);
  }
  // setPowerState off (true) and on (false).
  Step sleep(bool asleep, uint64_t nowMs)
  {
    if (asleep == asleep_) return Step::none;
    asleep_ = asleep;
    early_ = false;
    return settle(nowMs);
  }
  // gpio95 rose while blanked and awake; now: the display may wake now (wakeNow()).
  Step wakeRequest(bool now, uint64_t nowMs)
  {
    if (!enabled_ || running_ || asleep_ || !now) return Step::none;
    early_ = true;
    earlySince_ = nowMs;
    ++earlyWakes_;
    return settle(nowMs);
  }
  // Every pass: an early wake whose display did not come on within kEarlyWakeMs ends.
  Step tick(uint64_t nowMs)
  {
    if (early_ && display_ == 0 && nowMs - earlySince_ >= kEarlyWakeMs) {
      early_ = false;
      ++earlyTimeouts_;
      return settle(nowMs);
    }
    return Step::none;
  }
  uint32_t blanks() const { return blanks_; }
  uint32_t unblanks() const { return unblanks_; }
  uint32_t earlyWakes() const { return earlyWakes_; }
  uint32_t earlyTimeouts() const { return earlyTimeouts_; }

 private:
  bool want() const { return enabled_ && (asleep_ || (display_ == 0 && !early_)); }
  Step settle(uint64_t)
  {
    const bool blank = want();
    if (blank && running_) {
      running_ = false;
      ++blanks_;
      return Step::blank;
    }
    if (!blank && !running_) {
      running_ = true;
      ++unblanks_;
      return Step::unblank;
    }
    return Step::none;
  }
  bool enabled_ = false, running_ = true, asleep_ = false, early_ = false;
  int display_ = -1;
  uint64_t earlySince_ = 0;
  uint32_t blanks_ = 0, unblanks_ = 0, earlyWakes_ = 0, earlyTimeouts_ = 0;
};

// The unblank's wait for gpio97 (Ubuntu: polled every 1 ms for at most 100 ms, then 5 ms to settle only if it had to wait).
struct ReadyWait {
  static constexpr unsigned kPollMs = 1, kMaxMs = 100, kSettleMs = 5;
  // level(ctx) reads gpio97; sleep(ctx, ms) waits. Returns the ms waited (kMaxMs + 1: never high).
  template <class Level, class Sleep>
  static unsigned run(Level level, Sleep sleep)
  {
    unsigned waited = 0;
    while (!level()) {
      if (waited >= kMaxMs) return kMaxMs + 1;
      sleep(kPollMs);
      waited += kPollMs;
    }
    if (waited) sleep(kSettleMs);
    return waited;
  }
};

}  // namespace pianokeyboard
#endif
