// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoThermal: the TSENS upper/lower threshold ("uplow") interrupts behind piano-thermal-irq=1. Portable (no IOKit): the kernel side
// (PianoThermalIrq.cpp) and tests/uplow_host.cpp (against a model of the controller and a level-triggered line) include it.
// docs/drivers/thermal.md section 13.
//
// Sources: the piano boot DT (out/inputs/piano-boot.dtb): every thermal-sensor@c22x000 has interrupts = <0 0x303..0x306 4> "uplow"
// and <0 0x1e4..0x1e7 4> "critical", inheriting the root's interrupt-parent, the GICv3 (tooling/k27/check-tsens-irq.py checks the
// table below at every build). Linux drivers/thermal/qcom/tsens.c and tsens-v2.c read as the specification only (GPL: no code taken):
// on v2 an interrupt is enabled by clearing its mask bit and disabled by setting it, then writing its clear bit 1 and then 0
// (tsens_set_interrupt_v2); a threshold is violated while TM_Sn_STATUS's UPPER_STATUS (bit 18) / LOWER_STATUS (bit 17) is set
// (tsens_threshold_violated: the status word, not TM_UPPER_LOWER_INT_STATUS); thresholds are clamped to -40..120 C (v2's
// trip_min_temp / trip_max_temp) and written in deci-degrees; TM_INT_EN [2:0] holds v2's separate critical, upper and lower enables
// (which bit is which is not stated: Linux writes 7); set_trips writes the window's thresholds before it unmasks; Linux's piano
// kernel (08e750858038) masks every lower interrupt before system suspend and keeps the upper ones armed.
//
// Design (the numbers are boot arguments; defaults here):
//   - every sensor usable at the start gets lower 70.0 C / upper 85.0 C in TM_Sn_UPPER_LOWER_THRESHOLD; a sensor is "low" (its upper
//     interrupt unmasked, its lower one masked) or "high" (the reverse): an upper interrupt makes it high, a lower one low again.
//   - before it relies on the line, the start proves it on each controller: one sensor's upper threshold 5 C under its reading,
//     then its lower threshold 5 C above, each must interrupt within 250 ms; any failure: the path is off for the boot (fallback).
//   - at rest (every armed sensor low, no other sensor >= 70 C, no fast hold, no test, nothing suspect) the warm tier and the
//     fast-clock rule stand down (the upper interrupt stands in for them) and the base poll relaxes to 5 s (or the report period);
//     an upper interrupt holds the fast tier (10 ms) for 1 s, then today's tiers apply until every sensor is under 70 C again.
//   - a poll that finds an armed sensor 2 C past its threshold with no interrupt is suspect: fast polling for 50 ms of grace, then
//     a miss (the sensor changes state by the poll); 3 misses in a row, 40 interrupts in 10 s, or the line's storm guard: off for the
//     boot, logged once, every uplow interrupt masked, thresholds and TM_INT_EN back as found, the lines disabled at the GIC: today's
//     polling.
//   - the critical thresholds, the critical interrupts (0x14/0x18/0x1c) and SROT are never written.
#ifndef PIANO_THERMAL_TSENS_IRQ_H
#define PIANO_THERMAL_TSENS_IRQ_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "../common/PianoIrq.h"
#include "Cluster.h"
#include "Tsens.h"

namespace pianothermal::uplow {

// ------------------------------------------------------------------------------------------------------------- facts
constexpr uint32_t kSpiUplow[kControllers] = {771, 772, 773, 774};      // DT "uplow" <0 0x303..0x306 4>: level high, GIC
constexpr uint32_t kSpiCritical[kControllers] = {484, 485, 486, 487};   // DT "critical" <0 0x1e4..0x1e7 4>: not used
constexpr uint32_t kThresholdField = 0xffffffu;                       // lower [11:0], upper [23:12]; [31:24] kept as found
constexpr int32_t kTripMinDeci = -400, kTripMaxDeci = 1200;           // tsens-v2 trip_min_temp / trip_max_temp
constexpr uint32_t kIntEnEnables = kTmIntEnAll;                       // TM_INT_EN [2:0]: v2's three enables (Linux: 7)
constexpr uint32_t kCheckMs = 250;                                    // the start's proof: an interrupt within this
constexpr int32_t kCheckOffsetDeci = 50;                              // the proof's threshold: 5 C past the reading

inline int32_t clampTrip(int32_t d) { return d < kTripMinDeci ? kTripMinDeci : d > kTripMaxDeci ? kTripMaxDeci : d; }
// TM_Sn_UPPER_LOWER_THRESHOLD with the window [lower, upper] (clamped), the bits above the two fields as found.
inline uint32_t thresholdWord(uint32_t found, int32_t lowerDeci, int32_t upperDeci)
{
  return (found & ~kThresholdField) | encodeUpperLower(clampTrip(lowerDeci), clampTrip(upperDeci));
}
inline uint32_t lowerBit(unsigned s) { return 1u << s; }
inline uint32_t upperBit(unsigned s) { return 1u << (16 + s); }
inline uint32_t ourBits(unsigned sensors) { return lowerIntBits(sensors) | upperIntBits(sensors); }
// Sensor s's violated thresholds from its TM_Sn_STATUS word, as bits of the up/low mask's layout.
inline uint32_t violatedBits(uint32_t status, unsigned s)
{
  return ((status & kStatusLower) ? lowerBit(s) : 0u) | ((status & kStatusUpper) ? upperBit(s) : 0u);
}
inline uint16_t sensorMask(unsigned sensors) { return uint16_t(lowerIntBits(sensors)); }

// The TM ranges of the four controllers (byte offsets, 32-bit).
struct Regs {
  void *context;
  uint32_t (*read)(void *context, unsigned controller, uint32_t offset);
  void (*write)(void *context, unsigned controller, uint32_t offset, uint32_t value);
};

// ------------------------------------------------------------------------------------------------------------- the handler
// The uplow line's handler for controller c (primary interrupt context; the kernel holds the mask lock around it): every interrupt
// that is up and unmasked (its threshold violated in TM_Sn_STATUS, or latched in TM_UPPER_LOWER_INT_STATUS) is masked and its clear
// pulsed 1 then 0, which drops the level line; a latched status of an interrupt already masked is cleared too. bits = what was
// masked (0: nothing of ours was up: a stray for the storm guard).
struct Serviced {
  uint32_t bits, cleared, latched;
};
inline Serviced service(const Regs &r, unsigned c, unsigned sensors)
{
  const uint32_t ours = ourBits(sensors);
  const uint32_t mask = r.read(r.context, c, kTmUpLowIntMask);
  const uint32_t latched = r.read(r.context, c, kTmUpLowIntStatus) & ours;
  uint32_t violated = latched;
  for (unsigned s = 0; s < sensors; ++s) violated |= violatedBits(r.read(r.context, c, kTmStatus + 4 * s), s);
  const uint32_t pending = violated & ~mask & ours;
  const uint32_t clear = pending | (latched & mask);
  if (pending) r.write(r.context, c, kTmUpLowIntMask, mask | pending);
  if (clear) {
    r.write(r.context, c, kTmUpLowIntClear, clear);
    r.write(r.context, c, kTmUpLowIntClear, 0);
  }
  return {pending, clear, latched};
}

// ------------------------------------------------------------------------------------------------------------- the machine
enum class Phase : uint8_t { off, starting, live, failed };
inline const char *phaseName(Phase p)
{
  return p == Phase::live ? "live" : p == Phase::starting ? "starting" : p == Phase::failed ? "off (fell back to polling)" : "off";
}

struct Config {
  int32_t upperDeci = 850, lowerDeci = 700;
  uint32_t restPollMs = 5000;     // the base poll at rest (the caller passes the report period for "off at rest")
  uint32_t holdMs = 1000;         // the fast tier after an upper interrupt (or a suspect reading)
  uint32_t graceMs = 50;          // a suspect reading becomes a miss after this without its interrupt
  int32_t missMarginDeci = 20;    // suspect: this far past the threshold
  uint32_t missLimit = 3;         // misses in a row: off
  uint32_t stormWindowMs = 10000, stormLimit = 40;   // interrupts per window: off
  uint32_t testDeci = 0;          // piano-thermal-irq-test: one sensor per controller, its upper threshold this far above its reading
  uint32_t testMs = 120000;
  uint32_t logEvents = 16;        // events logged one by one, then every 100th
  bool governing = false;         // the governor's tick is the base: never relaxed
};

// What the machine needs from the kernel (or the host model).
struct Env {
  void *context;
  Regs regs;
  bool (*mapped)(void *context, unsigned controller);
  uintptr_t (*lock)(void *context);                   // the mask lock (the handler takes it too), interrupts off
  void (*unlock)(void *context, uintptr_t token);
  uint32_t (*take)(void *context, unsigned controller);   // the handler's record of masked bits, exchanged for 0
  // Waits up to ms for any of `bits` in the controller's record (the start's proof; consumes the record). True: they came.
  bool (*wait)(void *context, unsigned controller, uint32_t bits, uint32_t ms);
  bool (*attach)(void *context, unsigned controller);    // the controller's SPI (logs its own line)
  bool (*lineLive)(void *context, unsigned controller);  // false once its storm guard disabled it
  void (*disable)(void *context, unsigned controller);   // at the GIC, for good
  uint64_t (*count)(void *context, unsigned controller); // the line's handled interrupts
  uint64_t (*nowMs)(void *context);
  void (*note)(void *context, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

struct Deci1 {
  char s[16];
  explicit Deci1(int32_t d) { deciText(d, s, sizeof s); }
};

class Machine {
 public:
  void configure(const Config &config)
  {
    cfg_ = config;
    phase_ = Phase::starting;
    miss_ = pianoirq::missGuard(cfg_.missLimit);
    storm_ = pianoirq::stormGuard(cfg_.stormWindowMs, cfg_.stormLimit);
  }
  Phase phase() const { return phase_; }
  bool live() const { return phase_ == Phase::live; }
  bool rest() const { return phase_ == Phase::live && rest_; }
  bool holding(uint64_t nowMs) const { return phase_ == Phase::live && nowMs < holdUntilMs_; }
  const char *why() const { return why_; }
  const Config &config() const { return cfg_; }

  // The first poll: the proof on every controller, then the arming. False: off for the boot (logged; everything as found).
  bool start(const Env &env, const Monitor &m, uint64_t nowMs)
  {
    if (phase_ != Phase::starting) return false;
    unsigned controllers = 0, sensors = 0, high = 0;
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      const unsigned n = kMap[c].count;
      const uint32_t ours = ourBits(n);
      if (!env.mapped(env.context, c)) {
        env.note(env.context, "uplow: tsens%u %s: not mapped: no interrupt", c, kMap[c].name);
        continue;
      }
      const uint16_t usable = uint16_t(m.usableMask(c) & sensorMask(n));
      int32_t d = 0;
      unsigned s = 0;
      if (!usable || !m.hottest(c, d, s)) {
        env.note(env.context, "uplow: tsens%u %s: no usable sensor: not armed (polled)", c, kMap[c].name);
        continue;
      }
      k.foundMask = rd(env, c, kTmUpLowIntMask);
      k.foundIntEn = rd(env, c, kTmIntEn);
      for (unsigned i = 0; i < n; ++i) k.foundWord[i] = rd(env, c, kTmUpLowThreshold + 4 * i);
      k.used = true;
      // quiet: every up/low interrupt of the controller masked and cleared, before the line is attached
      uintptr_t t = env.lock(env.context);
      wr(env, c, kTmUpLowIntMask, k.foundMask | ours);
      pulseClear(env, c, ours);
      env.unlock(env.context, t);
      if (!env.attach(env.context, c)) return fail(env, nowMs, "tsens%u: SPI %u not attached", c, kSpiUplow[c]);
      k.attached = true;
      if (!env.lineLive(env.context, c))
        return fail(env, nowMs, "storm: SPI %u stayed up at its attach with nothing to mask; disabled at the GIC", kSpiUplow[c]);
      if ((k.foundIntEn & kIntEnEnables) != kIntEnEnables) {
        wr(env, c, kTmIntEn, k.foundIntEn | kIntEnEnables);
        k.intEnWritten = true;
      }
      // the proof: the upper threshold under the reading, then the lower one above it
      k.written |= uint16_t(1u << s);
      uint32_t ms[2] = {};
      for (unsigned step = 0; step < 2; ++step) {
        const bool upper = step == 0;
        const int32_t thr = upper ? d - kCheckOffsetDeci : d + kCheckOffsetDeci;
        wr(env, c, kTmUpLowThreshold + 4 * s,
           thresholdWord(k.foundWord[s], upper ? kTripMinDeci : thr, upper ? thr : kTripMaxDeci));
        const uint32_t bit = upper ? upperBit(s) : lowerBit(s);
        t = env.lock(env.context);
        wr(env, c, kTmUpLowIntMask, rd(env, c, kTmUpLowIntMask) & ~bit);
        env.unlock(env.context, t);
        const uint64_t t0 = env.nowMs(env.context);
        const bool came = env.wait(env.context, c, bit, kCheckMs);
        ms[step] = uint32_t(env.nowMs(env.context) - t0);
        if (!env.lineLive(env.context, c))
          return fail(env, nowMs, "storm: SPI %u stayed up during the proof with nothing to mask; disabled at the GIC", kSpiUplow[c]);
        if (!came) {
          // the registers as the interrupt did not come (bit 18/17: the hardware saw the crossing); fail() masks everything
          const uint32_t st = rd(env, c, kTmStatus + 4 * s), is = rd(env, c, kTmUpLowIntStatus), mk = rd(env, c, kTmUpLowIntMask);
          return fail(env, nowMs, "proof: tsens%u s%u %s at %s C: %s threshold %s C and no interrupt in %u ms (status %#x, int status %#x, "
                      "mask %#x, SPI %u count %llu)", c, s, kMap[c].sensors[s].name, Deci1(d).s, upper ? "upper" : "lower",
                      Deci1(clampTrip(thr)).s, kCheckMs, st, is, mk, kSpiUplow[c], (unsigned long long)env.count(env.context, c));
        }
      }
      env.note(env.context, "uplow: tsens%u %s: proof on s%u %s at %s C: upper %s C -> interrupt in %u ms, lower %s C -> interrupt in %u ms "
               "(found: mask %#x, int-en %#x%s)", c, kMap[c].name, s, kMap[c].sensors[s].name, Deci1(d).s, Deci1(clampTrip(d - kCheckOffsetDeci)).s,
               ms[0], Deci1(clampTrip(d + kCheckOffsetDeci)).s, ms[1], k.foundMask, k.foundIntEn, k.intEnWritten ? ", int-en set to 7" : "");
      // the arming: every usable sensor's window, its state from the reading
      k.armed = usable;
      for (unsigned i = 0; i < n; ++i) {
        if (!(usable & (1u << i))) continue;
        wr(env, c, kTmUpLowThreshold + 4 * i, thresholdWord(k.foundWord[i], cfg_.lowerDeci, cfg_.upperDeci));
        k.written |= uint16_t(1u << i);
        if (m.deci(c, i) >= cfg_.upperDeci) {
          k.high |= uint16_t(1u << i);
          ++high;
        }
      }
      // statuses latched under the old or the proof's thresholds go before anything is unmasked
      t = env.lock(env.context);
      pulseClear(env, c, ours);
      env.unlock(env.context, t);
      ++controllers;
      sensors += popcount(usable);
    }
    if (!controllers) return fail(env, nowMs, "no controller could be armed");
    phase_ = Phase::live;
    apply(env, true);
    char line[160];
    int at = 0;
    line[0] = 0;
    for (unsigned c = 0; c < kControllers; ++c)
      if (ctl_[c].attached) at = cat(line, sizeof line, at, " tsens%u %#x", c, ctl_[c].lastMask);
    char base[48];
    baseText(base, sizeof base);
    env.note(env.context, "uplow: live: %u sensors armed on %u controllers (upper %s C, lower %s C; %u above the upper threshold); masks%s; "
             "at rest %s, an upper interrupt polls fast for %u ms", sensors, controllers, Deci1(cfg_.upperDeci).s, Deci1(cfg_.lowerDeci).s,
             high, line, base, cfg_.holdMs);
    if (cfg_.testDeci) startTest(env, m, nowMs);
    updateRest(m);
    return true;
  }

  // Every later poll: the handler's records, the state changes, misses, the test, the guards, the masks.
  void step(const Env &env, const Monitor &m, uint64_t nowMs)
  {
    if (phase_ != Phase::live) return;
    uintptr_t t = env.lock(env.context);
    uint32_t ev[kControllers];
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (k.attached) k.inbox |= env.take(env.context, c);
      ev[c] = k.inbox;
      k.inbox = 0;
    }
    env.unlock(env.context, t);
    bool any = false;
    for (unsigned c = 0; c < kControllers; ++c)
      if (ev[c]) any |= events(env, c, ev[c], m, nowMs);
    if (any) miss_.delivered();
    checkMisses(env, m, nowMs);
    if (testsRunning_) checkTests(env, m, nowMs);
    if (miss_.dead) {
      fail(env, nowMs, "%u missed interrupts in a row", cfg_.missLimit);
      return;
    }
    if (storm_.tripped) {
      fail(env, nowMs, "stormy: more than %u threshold interrupts in %u s", cfg_.stormLimit, cfg_.stormWindowMs / 1000);
      return;
    }
    for (unsigned c = 0; c < kControllers; ++c)
      if (ctl_[c].attached && !env.lineLive(env.context, c)) {
        fail(env, nowMs, "storm: SPI %u stayed up with nothing to mask; disabled at the GIC", kSpiUplow[c]);
        return;
      }
    apply(env, false);
    const bool was = rest_;
    updateRest(m);
    if (rest_ && !was && restLogs_ < cfg_.logEvents) {
      ++restLogs_;
      char base[48];
      baseText(base, sizeof base);
      env.note(env.context, "[%llus] uplow: at rest again (every sensor under %s C, or back under %s C after a crossing): %s",
               (unsigned long long)(nowMs / 1000), Deci1(cfg_.upperDeci).s, Deci1(cfg_.lowerDeci).s, base);
    }
  }
  void baseText(char *out, size_t size) const
  {
    if (cfg_.governing) snprintf(out, size, "the base stays the governor's tick");
    else snprintf(out, size, "the base poll is %u ms", cfg_.restPollMs);
  }

  // The poll tier: today's (`polled`), or at rest the tier without the warm rule and the fast-clock rule (`atRest`), fast while held.
  Poll tier(Poll polled, Poll atRest, uint64_t nowMs) const
  {
    if (phase_ != Phase::live) return polled;
    if (nowMs < holdUntilMs_) return Poll::fast;
    return rest_ ? atRest : polled;
  }
  uint32_t baseMs(uint32_t base) const
  {
    if (phase_ != Phase::live || !rest_ || cfg_.governing) return base;
    return cfg_.restPollMs > base ? cfg_.restPollMs : base;
  }

  // System suspend while live: every lower interrupt masked (and its clear pulsed), the upper ones kept as they are (armed for the low
  // sensors: overheating still interrupts), no lower interrupt unmasked until finish. The number of controllers, -1 when not live.
  // While still starting, the caller masks with its own path (-1) and the arming, should it come meanwhile, keeps every lower
  // interrupt masked until suspendFinish.
  int suspendPrepare(const Env &env)
  {
    if (prepared_ || external_) return -1;
    if (phase_ == Phase::starting) {
      const uintptr_t t = env.lock(env.context);
      suspended_ = external_ = true;
      env.unlock(env.context, t);
      return -1;
    }
    if (phase_ != Phase::live) return -1;
    int done = 0;
    char line[200];
    int at = 0;
    line[0] = 0;
    const uintptr_t t = env.lock(env.context);
    suspended_ = true;
    prepared_ = true;
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (!k.attached) continue;
      const unsigned n = kMap[c].count;
      const uint32_t found = rd(env, c, kTmUpLowIntMask), want = found | lowerIntBits(n);
      wr(env, c, kTmUpLowIntMask, want);
      pulseClear(env, c, lowerIntBits(n));
      k.lastMask = want & ourBits(n);
      ++done;
      at = cat(line, sizeof line, at, " tsens%u %#x->%#x", c, found, want);
    }
    env.unlock(env.context, t);
    env.note(env.context, "suspend prepare: lower threshold interrupts masked on %d controllers, the upper ones kept armed (the uplow "
             "interrupt is serviced):%s", done, line);
    return done;
  }
  // True when the masks are this path's (the caller then restores nothing): its own prepare, or a caller's suspend during which the
  // path went live (the masks the caller saved predate the arming: restoring them would unmask every lower interrupt).
  bool suspendFinish(const Env &env)
  {
    if (external_) {
      const uintptr_t t = env.lock(env.context);
      suspended_ = external_ = false;
      env.unlock(env.context, t);
      if (phase_ != Phase::live) return false;
      apply(env, true);
      env.note(env.context, "suspend finish: the uplow interrupt went live during this suspend: its masks, not the ones found before");
      return true;
    }
    if (!prepared_) return false;
    const uintptr_t t = env.lock(env.context);
    suspended_ = false;
    prepared_ = false;
    env.unlock(env.context, t);
    if (phase_ == Phase::live) {
      apply(env, true);
      env.note(env.context, "suspend finish: the uplow interrupt masks are back (lower armed for the sensors above %s C)", Deci1(cfg_.upperDeci).s);
    } else {
      env.note(env.context, "suspend finish: the uplow path fell back meanwhile: every up/low interrupt stays masked");
    }
    return true;
  }
  bool suspended() const { return suspended_; }

  // Counters and per-sensor state, for the logs and the host test.
  uint64_t events() const { return events_; }
  uint64_t upperEvents() const { return upper_; }
  uint64_t lowerEvents() const { return lower_; }
  uint64_t spurious() const { return spurious_; }
  uint64_t late() const { return late_; }
  uint32_t misses() const { return miss_.total; }
  uint16_t armed(unsigned c) const { return ctl_[c].armed; }
  uint16_t high(unsigned c) const { return ctl_[c].high; }
  uint32_t lastMask(unsigned c) const { return ctl_[c].lastMask; }
  bool attached(unsigned c) const { return ctl_[c].attached; }
  bool testRunning() const { return testsRunning_ != 0; }
  unsigned testsPassed() const { return testPassed_; }
  unsigned testsMissed() const { return testMissed_; }
  unsigned testsQuiet() const { return testQuiet_; }
  bool testOn(unsigned c) const { return ctl_[c].test; }
  unsigned testSensor(unsigned c) const { return ctl_[c].testSensor; }
  int32_t testThreshold(unsigned c) const { return ctl_[c].testDeci; }

 private:
  struct Ctl {
    bool used, attached, intEnWritten, test;
    uint16_t armed, high, written;
    uint32_t foundMask, foundIntEn, lastMask, inbox;
    uint32_t foundWord[kMaxSensors];
    uint64_t suspectMs[kMaxSensors];   // 0: none
    uint8_t testSensor;
    int32_t testDeci, testStartDeci, testMaxDeci;
    uint64_t testStartMs;
  };

  static uint32_t rd(const Env &env, unsigned c, uint32_t off) { return env.regs.read(env.regs.context, c, off); }
  static void wr(const Env &env, unsigned c, uint32_t off, uint32_t v) { env.regs.write(env.regs.context, c, off, v); }
  static void pulseClear(const Env &env, unsigned c, uint32_t bits)
  {
    wr(env, c, kTmUpLowIntClear, bits);
    wr(env, c, kTmUpLowIntClear, 0);
  }
  static int cat(char *line, size_t size, int at, const char *fmt, ...) __attribute__((format(printf, 4, 5)))
  {
    if (at < 0 || size_t(at) + 1 >= size) return at;
    va_list ap;
    va_start(ap, fmt);
    const int wanted = vsnprintf(line + at, size - size_t(at), fmt, ap);
    va_end(ap);
    if (wanted < 0) return at;
    return size_t(at) + size_t(wanted) >= size ? int(size - 1) : at + wanted;
  }

  uint32_t desired(unsigned c) const
  {
    const Ctl &k = ctl_[c];
    uint32_t m = ourBits(kMap[c].count);
    for (unsigned s = 0; s < kMap[c].count; ++s) {
      if (!(k.armed & (1u << s))) continue;
      if (k.high & (1u << s)) {
        if (!suspended_) m &= ~lowerBit(s);
      } else {
        m &= ~upperBit(s);
      }
    }
    return m;
  }
  // The masks the states want, against the register (the handler masks bits on its own), with every interrupt still waiting in a
  // record kept masked (it is processed next step). A bit about to be unmasked has its clear pulsed first: a status latched while it
  // was masked is stale (the live TM_Sn_STATUS comparison raises the line again if the threshold is still violated).
  void apply(const Env &env, bool force)
  {
    const uintptr_t t = env.lock(env.context);
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (!k.attached) continue;
      k.inbox |= env.take(env.context, c);
      const uint32_t ours = ourBits(kMap[c].count), want = desired(c) | (k.inbox & ours);
      const uint32_t cur = rd(env, c, kTmUpLowIntMask);
      k.lastMask = want;
      if (!force && (cur & ours) == want) continue;
      const uint32_t unmasking = cur & ours & ~want;
      if (unmasking) pulseClear(env, c, unmasking);
      wr(env, c, kTmUpLowIntMask, (cur & ~ours) | want);
    }
    env.unlock(env.context, t);
  }

  bool events(const Env &env, unsigned c, uint32_t ev, const Monitor &m, uint64_t nowMs)
  {
    Ctl &k = ctl_[c];
    bool delivered = false;
    for (unsigned s = 0; s < kMap[c].count; ++s) {
      const uint16_t b = uint16_t(1u << s);
      const bool usable = m.usable(c, s);
      const int32_t d = usable ? m.deci(c, s) : 0;
      for (unsigned up = 0; up < 2; ++up) {
        if (!(ev & (up ? upperBit(s) : lowerBit(s)))) continue;
        ++events_;
        storm_.stray(nowMs);
        delivered = true;
        k.suspectMs[s] = 0;
        if (up) ++upper_;
        else ++lower_;
        if (up && k.test && k.testSensor == s) {
          endTest(env, c, nowMs, 1, d, usable);
          continue;
        }
        if (!(k.armed & b) || bool(k.high & b) == bool(up)) {   // not ours, or the state already changed (a late or repeated one)
          ++late_;
          continue;
        }
        if (usable && (up ? d < cfg_.lowerDeci : d > cfg_.upperDeci)) {   // spurious: the reading says the other side; re-armed
          ++spurious_;
          if (logged_ < cfg_.logEvents) {
            ++logged_;
            env.note(env.context, "[%llus] uplow: tsens%u s%u %s %s interrupt at %s C: spurious (re-armed)", (unsigned long long)(nowMs / 1000),
                     c, s, kMap[c].sensors[s].name, up ? "upper" : "lower", Deci1(d).s);
          }
          continue;
        }
        if (up) {
          k.high |= b;
          holdUntilMs_ = nowMs + cfg_.holdMs;
        } else {
          k.high &= uint16_t(~b);
        }
        if (logged_ < cfg_.logEvents || events_ % 100 == 0) {
          ++logged_;
          char at[16] = "no reading";
          if (usable) deciText(d, at, sizeof at);
          if (up)
            env.note(env.context, "[%llus] uplow: tsens%u s%u %s upper interrupt at %s C (event %llu): polling every 10 ms for %u ms, then by "
                     "temperature until it is under %s C", (unsigned long long)(nowMs / 1000), c, s, kMap[c].sensors[s].name, at,
                     (unsigned long long)events_, cfg_.holdMs, Deci1(cfg_.lowerDeci).s);
          else
            env.note(env.context, "[%llus] uplow: tsens%u s%u %s lower interrupt at %s C (event %llu): back under %s C",
                     (unsigned long long)(nowMs / 1000), c, s, kMap[c].sensors[s].name, at, (unsigned long long)events_,
                     Deci1(cfg_.lowerDeci).s);
        }
      }
    }
    return delivered;
  }

  // A reading past an armed threshold with no interrupt: suspect (fast polls), a miss after the grace; the poll changes the state.
  void checkMisses(const Env &env, const Monitor &m, uint64_t nowMs)
  {
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (!k.attached) continue;
      for (unsigned s = 0; s < kMap[c].count; ++s) {
        const uint16_t b = uint16_t(1u << s);
        if (!(k.armed & b) || !m.usable(c, s)) {
          k.suspectMs[s] = 0;
          continue;
        }
        const int32_t d = m.deci(c, s);
        const bool high = k.high & b, test = k.test && k.testSensor == s;
        const int32_t thr = high ? cfg_.lowerDeci : test ? k.testDeci : cfg_.upperDeci;
        const bool past = high ? d <= thr - cfg_.missMarginDeci : d >= thr + cfg_.missMarginDeci;
        if (!past) {
          k.suspectMs[s] = 0;
          continue;
        }
        if (high && suspended_) {   // the lower interrupts are masked across suspend: the poll moves it, no miss
          k.high &= uint16_t(~b);
          continue;
        }
        if (!k.suspectMs[s]) {
          k.suspectMs[s] = nowMs ? nowMs : 1;
          if (!high && holdUntilMs_ < nowMs + cfg_.holdMs) holdUntilMs_ = nowMs + cfg_.holdMs;   // a rise: poll fast meanwhile
          continue;
        }
        if (nowMs - k.suspectMs[s] < cfg_.graceMs) continue;
        k.suspectMs[s] = 0;
        miss_.miss();
        env.note(env.context, "[%llus] uplow: %sMISSED: tsens%u s%u %s at %s C %s %s C and no interrupt within %u ms (status %#x, mask %#x, "
                 "SPI %u count %llu; miss %u of %u in a row)", (unsigned long long)(nowMs / 1000), test ? "test: " : "", c, s,
                 kMap[c].sensors[s].name, Deci1(d).s, high ? "<=" : ">=", Deci1(thr).s, cfg_.graceMs, m.raw(c, s), k.lastMask, kSpiUplow[c],
                 (unsigned long long)env.count(env.context, c), miss_.run, cfg_.missLimit);
        if (test) {
          endTest(env, c, nowMs, 2, d, true);
        } else if (high) {
          k.high &= uint16_t(~b);
        } else {
          k.high |= b;
        }
      }
    }
  }

  void startTest(const Env &env, const Monitor &m, uint64_t nowMs)
  {
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (!k.attached) continue;
      bool any = false;
      int32_t best = 0;
      unsigned s = 0;
      for (unsigned i = 0; i < kMap[c].count; ++i) {
        const int32_t d = m.deci(c, i);
        if (!(k.armed & (1u << i)) || (k.high & (1u << i)) || !m.usable(c, i) || d + int32_t(cfg_.testDeci) >= cfg_.upperDeci) continue;
        if (!any || d > best) {
          any = true;
          best = d;
          s = i;
        }
      }
      if (!any) {
        env.note(env.context, "uplow: test: tsens%u %s: no sensor low enough for the test", c, kMap[c].name);
        continue;
      }
      k.test = true;
      k.testSensor = uint8_t(s);
      k.testStartDeci = k.testMaxDeci = best;
      k.testDeci = clampTrip(best + int32_t(cfg_.testDeci));
      k.testStartMs = nowMs;
      ++testsRunning_;
      wr(env, c, kTmUpLowThreshold + 4 * s, thresholdWord(k.foundWord[s], kTripMinDeci, k.testDeci));
      env.note(env.context, "uplow: test: tsens%u s%u %s at %s C: upper threshold %s C (piano-thermal-irq-test=%u) for up to %u s; warm the SoC "
               "a little (a short CPU load) to cross it", c, s, kMap[c].sensors[s].name, Deci1(best).s, Deci1(k.testDeci).s, cfg_.testDeci,
               cfg_.testMs / 1000);
    }
  }
  void checkTests(const Env &env, const Monitor &m, uint64_t nowMs)
  {
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (!k.test) continue;
      if (m.usable(c, k.testSensor) && m.deci(c, k.testSensor) > k.testMaxDeci) k.testMaxDeci = m.deci(c, k.testSensor);
      if (nowMs - k.testStartMs >= cfg_.testMs) endTest(env, c, nowMs, 0, 0, false);
    }
  }
  // how: 1 the interrupt came, 2 missed, 0 no crossing in time. The sensor's window goes back to the armed one.
  void endTest(const Env &env, unsigned c, uint64_t nowMs, unsigned how, int32_t d, bool usable)
  {
    Ctl &k = ctl_[c];
    const unsigned s = k.testSensor;
    k.test = false;
    --testsRunning_;
    wr(env, c, kTmUpLowThreshold + 4 * s, thresholdWord(k.foundWord[s], cfg_.lowerDeci, cfg_.upperDeci));
    char after[16];
    deciText(int32_t((nowMs - k.testStartMs) / 100), after, sizeof after);
    if (how == 1) {
      ++testPassed_;
      char at[16] = "no reading";
      if (usable) deciText(d, at, sizeof at);
      env.note(env.context, "[%llus] uplow: test: tsens%u s%u %s: upper interrupt at %s C after %s s (threshold %s C, from %s C; SPI %u count %llu): "
               "PASS; threshold back to %s C", (unsigned long long)(nowMs / 1000), c, s, kMap[c].sensors[s].name, at, after, Deci1(k.testDeci).s,
               Deci1(k.testStartDeci).s, kSpiUplow[c], (unsigned long long)env.count(env.context, c), Deci1(cfg_.upperDeci).s);
    } else if (how == 2) {
      ++testMissed_;
      env.note(env.context, "[%llus] uplow: test: tsens%u s%u %s: FAILED (no interrupt); threshold back to %s C", (unsigned long long)(nowMs / 1000),
               c, s, kMap[c].sensors[s].name, Deci1(cfg_.upperDeci).s);
    } else {
      ++testQuiet_;
      env.note(env.context, "[%llus] uplow: test: tsens%u s%u %s: no crossing in %u s (highest %s C, threshold %s C): threshold back to %s C",
               (unsigned long long)(nowMs / 1000), c, s, kMap[c].sensors[s].name, cfg_.testMs / 1000, Deci1(k.testMaxDeci).s,
               Deci1(k.testDeci).s, Deci1(cfg_.upperDeci).s);
    }
    if (!testsRunning_)
      env.note(env.context, "[%llus] uplow: test: done: %u passed, %u missed, %u without a crossing", (unsigned long long)(nowMs / 1000),
               testPassed_, testMissed_, testQuiet_);
  }

  void updateRest(const Monitor &m)
  {
    bool rest = phase_ == Phase::live && !testsRunning_;
    for (unsigned c = 0; c < kControllers && rest; ++c) {
      const Ctl &k = ctl_[c];
      if (k.high) rest = false;
      for (unsigned s = 0; s < kMap[c].count && rest; ++s) {
        if (k.suspectMs[s]) rest = false;
        // a sensor without an armed interrupt (not usable at the start, or its controller not armed) keeps today's polling from
        // the lower threshold on
        else if (!(k.armed & (1u << s)) && m.usable(c, s) && m.deci(c, s) >= cfg_.lowerDeci) rest = false;
      }
    }
    rest_ = rest;
  }

  // Off for the boot: logged once; every up/low interrupt masked, the thresholds and TM_INT_EN written back as found, the lines
  // disabled at the GIC. Always false (for `return fail(...)`).
  bool fail(const Env &env, uint64_t nowMs, const char *fmt, ...) __attribute__((format(printf, 4, 5)))
  {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why_, sizeof why_, fmt, ap);
    va_end(ap);
    const bool wasLive = phase_ == Phase::live;
    phase_ = Phase::failed;
    rest_ = false;
    holdUntilMs_ = 0;
    const uintptr_t t = env.lock(env.context);
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (!k.used) continue;
      const uint32_t ours = ourBits(kMap[c].count);
      wr(env, c, kTmUpLowIntMask, rd(env, c, kTmUpLowIntMask) | ours);
      pulseClear(env, c, ours);
      k.lastMask = ours;
      k.inbox = 0;
    }
    env.unlock(env.context, t);
    unsigned lines = 0;
    for (unsigned c = 0; c < kControllers; ++c) {
      Ctl &k = ctl_[c];
      if (!k.used) continue;
      for (unsigned s = 0; s < kMap[c].count; ++s)
        if (k.written & (1u << s)) wr(env, c, kTmUpLowThreshold + 4 * s, k.foundWord[s]);
      if (k.intEnWritten) wr(env, c, kTmIntEn, k.foundIntEn);
      k.written = 0;
      k.intEnWritten = false;
      k.test = false;
      if (k.attached) {
        env.disable(env.context, c);
        ++lines;
      }
      k.attached = false;
      k.armed = k.high = 0;
    }
    testsRunning_ = 0;
    env.note(env.context, "[%llus] uplow: OFF (%s)%s: %llu interrupts (%llu upper, %llu lower, %llu spurious), %u misses; every up/low interrupt "
             "masked, thresholds and TM_INT_EN back as found, %u lines disabled at the GIC; polled as before for the rest of the boot",
             (unsigned long long)(nowMs / 1000), why_, wasLive ? "" : " at the start", (unsigned long long)events_, (unsigned long long)upper_,
             (unsigned long long)lower_, (unsigned long long)spurious_, miss_.total, lines);
    return false;
  }

  Config cfg_;
  Phase phase_ = Phase::off;
  Ctl ctl_[kControllers] = {};
  bool rest_ = false, suspended_ = false, prepared_ = false, external_ = false;
  uint64_t holdUntilMs_ = 0;
  pianoirq::MissGuard miss_ = pianoirq::missGuard(3);
  pianoirq::StormGuard storm_ = pianoirq::stormGuard(10000, 40);
  uint64_t events_ = 0, upper_ = 0, lower_ = 0, spurious_ = 0, late_ = 0;
  uint32_t logged_ = 0, restLogs_ = 0;
  unsigned testsRunning_ = 0, testPassed_ = 0, testMissed_ = 0, testQuiet_ = 0;
  char why_[160] = {};
};

}  // namespace pianothermal::uplow

#endif
