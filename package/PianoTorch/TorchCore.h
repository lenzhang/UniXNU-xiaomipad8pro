// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoTorch's portable part (docs/drivers/torch.md section 4): the SPMI write channel, the level 1 survey of the PM8550 flash
// module, the only writes level 2 makes (an allowlist of ten registers of that one peripheral, each written read-modify-write
// under its mask and read back), the torch on/off sequences, and the guard that turns the torch off (time limit, the module's own
// fault and over-temperature bits, the SoC and battery temperatures other drivers publish, a lost or unreadable state).
// No I/O of its own: reads and writes go through a Bus the kext (PianoTorch.cpp) or the host test (tests/torch_host.cpp) gives.
#ifndef PIANO_TORCH_CORE_H
#define PIANO_TORCH_CORE_H
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "FlashHw.h"

namespace pianotorch {

// ------------------------------------------------------------------------------------------------- the SPMI write channel
// The write channel of one APID: its 0x20 bytes at chnls + 0x1000 * APID (the kext maps that one page and nothing else of the
// channel window). rd/wr take an offset into the channel.
struct ChnlIo {
  void *ctx;
  uint32_t (*rd)(void *ctx, uint32_t offset);
  void (*wr)(void *ctx, uint32_t offset, uint32_t value);
  void (*delayUs)(void *ctx, unsigned us);
};

enum class WriteResult : uint8_t { ok, error, timeout };

// One register write of one byte (spmi-pmic-arb v7: the data first, then the command; status bit 0 done, bits 1-3 failure).
inline WriteResult spmiWriteByte(const ChnlIo &io, uint8_t reg, uint8_t value, uint32_t *status = nullptr)
{
  io.wr(io.ctx, kChnlWdata0, value);
  io.wr(io.ctx, kChnlCmd, kOpExtWriteL << 27 | uint32_t(reg) << 4 | 0u);
  uint32_t st = 0;
  for (unsigned i = 0; i < 1000; ++i) {
    st = io.rd(io.ctx, kChnlStatus);
    if (st & kStatusDone) {
      if (status) *status = st;
      return (st & kStatusErrors) ? WriteResult::error : WriteResult::ok;
    }
    io.delayUs(io.ctx, 1);
  }
  if (status) *status = st;
  return WriteResult::timeout;
}

// ------------------------------------------------------------------------------------------------------------- the bus
// read: bytes (1..8) from reg over the observer channel; write: one byte over the owner channel (null at level 1).
struct Bus {
  void *ctx = nullptr;
  bool (*read)(void *ctx, uint8_t reg, uint8_t *value, unsigned bytes) = nullptr;
  bool (*write)(void *ctx, uint8_t reg, uint8_t value) = nullptr;
};

// ------------------------------------------------------------------------------------------------------- level 1: survey
// The byte ranges level 1 reads (8 bytes a read): identity and status, the interrupt block, the channel registers, the thermal
// thresholds, the clamp's neighbourhood. Nothing outside the peripheral's 256 bytes.
struct Range {
  uint8_t start, bytes;
};
constexpr Range kSurveyRanges[] = {{0x00, 16}, {0x10, 16}, {0x38, 24}, {0x78, 8}, {0xe8, 8}};

struct Survey {
  bool identity = false;     // TYPE/SUBTYPE/REVISION answered
  bool flash = false;        // TYPE = 0x18
  bool fourChannel = false;  // SUBTYPE = 0x07: the layout FlashHw.h describes
  bool complete = false;     // every range answered
  uint8_t revision = 0, type = 0, subtype = 0;
  uint8_t reg[256] = {};
  bool valid[256] = {};
  unsigned reads = 0, failures = 0;

  // The four-channel decode (meaningful with fourChannel).
  bool moduleEn() const { return reg[kRegModuleEn] & kModuleEnBit; }
  uint8_t chanEn() const { return reg[kRegChanEn] & 0x0f; }
  bool lit() const { return moduleEn() && (chanEn() & kPianoChannelMask); }
  ChannelView channel(unsigned ch) const
  {
    ChannelView v;
    v.itarget = reg[kRegItarget0 + ch] & kItargetMask;
    v.strobe = reg[kRegStrobe0 + ch] & kStrobeMask;
    v.timer = reg[kRegTimer0 + ch];
    v.torchSteps = reg[kRegIres] & (1u << ch);
    v.on = chanEn() & (1u << ch);
    return v;
  }
  Faults faults() const { return decodeFaults(reg[kRegStatus1], reg[kRegStatus2], reg[kRegStatus3], kPianoChannelMask); }
};

inline Survey survey(const Bus &bus)
{
  Survey s;
  uint8_t id[8] = {};
  ++s.reads;
  if (!bus.read(bus.ctx, 0x00, id, 8)) {
    ++s.failures;
    return s;
  }
  s.identity = true;
  s.revision = id[kRegRevision];
  s.type = id[kRegType];
  s.subtype = id[kRegSubtype];
  s.flash = s.type == kTypeFlash;
  s.fourChannel = s.flash && s.subtype == kSubtype4ch;
  if (!s.flash) return s;   // not the flash module: nothing more is read
  s.complete = true;
  for (const Range &r : kSurveyRanges) {
    for (unsigned at = r.start; at < unsigned(r.start) + r.bytes; at += 8) {
      uint8_t b[8] = {};
      ++s.reads;
      if (!bus.read(bus.ctx, uint8_t(at), b, 8)) {
        ++s.failures;
        s.complete = false;
        continue;
      }
      for (unsigned i = 0; i < 8; ++i) {
        s.reg[at + i] = b[i];
        s.valid[at + i] = true;
      }
    }
  }
  return s;
}

// Why level 2 must not write, or nullptr. ownerEe: the owner of the flash APID (cnfg); the arbiter drops another EE's writes.
inline const char *writeRefusal(const Survey &s, int apid, uint32_t ownerEe)
{
  if (apid < 0) return "the flash module's PPID 0x1ee is not in the SPMI arbiter's APID map";
  if (ownerEe != 0) return "the flash APID belongs to another execution environment (owner EE is not 0): the arbiter refuses our writes";
  if (!s.identity) return "the module did not answer the identity read";
  if (!s.flash) return "TYPE is not 0x18: not a flash LED module";
  if (!s.fourChannel) return "SUBTYPE is not 0x07: not the four-channel layout this driver knows";
  if (!s.complete) return "part of the survey did not read: the state is not known";
  if (s.chanEn() & kOtherChannelMask) return "channel 3 or 4 is enabled: someone else drives this module";
  return nullptr;
}

// ----------------------------------------------------------------------------------------------- level 2: the writes
// Every register level 2 may write and the bits it may change in it. Nothing else is ever written (Controller::put refuses).
struct Writable {
  uint8_t reg, mask;
};
constexpr Writable kWritable[] = {
    {kRegTimer0 + 0, 0xff},  {kRegTimer0 + 1, 0xff},   {kRegItarget0 + 0, kItargetMask}, {kRegItarget0 + 1, kItargetMask},
    {kRegModuleEn, kModuleEnBit}, {kRegIres, kPianoChannelMask}, {kRegStrobe0 + 0, kStrobeMask}, {kRegStrobe0 + 1, kStrobeMask},
    {kRegChanEn, kPianoChannelMask}, {kRegTorchClamp, kClampMask},
};
inline bool writable(uint8_t reg, uint8_t mask)
{
  if (!mask) return false;
  for (const Writable &w : kWritable)
    if (w.reg == reg) return (mask & ~w.mask) == 0;
  return false;
}

// The hard limits, whatever the boot arguments say: never more than Ubuntu's 100 mA lamp current (2 x 50 mA, ITARGET 9).
constexpr unsigned kHardMaxMa = kUbuntuTorchMaxMa;
constexpr uint8_t kHardMaxItarget = itargetForMa(kHardMaxMa);
inline unsigned clampMa(unsigned ma, unsigned maxMa)
{
  if (maxMa > kHardMaxMa) maxMa = kHardMaxMa;
  if (maxMa < kTorchMinMa) maxMa = kTorchMinMa;
  return ma < kTorchMinMa ? kTorchMinMa : ma > maxMa ? maxMa : ma;
}

struct StepResult {
  bool ok = true;
  unsigned writes = 0, readbacks = 0, refused = 0;
  uint8_t failedReg = 0, failedWant = 0, failedGot = 0;
  const char *why = nullptr;   // the first failure
};

class Controller {
 public:
  Controller() = default;
  explicit Controller(const Bus &bus) : bus_(bus) {}
  void setBus(const Bus &bus) { bus_ = bus; }

  // The survey's bytes as the last known values (a masked write whose read fails merges into these, so off() still writes).
  void seed(const Survey &s)
  {
    for (unsigned i = 0; i < 256; ++i)
      if (s.valid[i]) remember(uint8_t(i), s.reg[i]);
  }

  // A masked write: read (unless the mask is the whole byte; on a failed read the last known value), merge, write, read back,
  // compare under the mask.
  bool put(StepResult &r, uint8_t reg, uint8_t mask, uint8_t value)
  {
    if (!writable(reg, mask) || (value & ~mask) || !bus_.write) {
      ++r.refused;
      return fail(r, reg, value, 0, "a write outside the allowlist was refused");
    }
    uint8_t old = 0;
    if (mask != 0xff) {
      if (bus_.read(bus_.ctx, reg, &old, 1)) remember(reg, old);
      else if (known_[reg]) old = cache_[reg];
      else return fail(r, reg, value, 0, "the read before a masked write failed and the register's value is not known");
    }
    const uint8_t next = uint8_t((old & ~mask) | value);
    if (!bus_.write(bus_.ctx, reg, next)) return fail(r, reg, value, 0, "the SPMI write failed (status error or timeout)");
    ++r.writes;
    remember(reg, next);
    uint8_t back = 0;
    if (!bus_.read(bus_.ctx, reg, &back, 1)) return fail(r, reg, value, 0, "the read-back failed");
    ++r.readbacks;
    remember(reg, back);
    if ((back & mask) != value) return fail(r, reg, value, uint8_t(back & mask), "the read-back differs from the value written");
    return true;
  }

  // Torch on at itarget on both channels (piano's lamp), with Linux's torch settings: software strobe, 5 mA steps, no safety
  // timer, the clamp set for maxMa. Stops at the first failure; the caller then runs off().
  StepResult on(uint8_t itarget, uint8_t clamp)
  {
    StepResult r;
    if (itarget > kHardMaxItarget) itarget = kHardMaxItarget;
    if (clamp > kHardMaxItarget) clamp = kHardMaxItarget;
    put(r, kRegChanEn, kPianoChannelMask, 0) && put(r, kRegModuleEn, kModuleEnBit, 0) &&
        put(r, kRegStrobe0 + 0, kStrobeMask, kStrobeSoftwareLevelHigh) && put(r, kRegStrobe0 + 1, kStrobeMask, kStrobeSoftwareLevelHigh) &&
        put(r, kRegTorchClamp, kClampMask, clamp) && put(r, kRegItarget0 + 0, kItargetMask, itarget) &&
        put(r, kRegItarget0 + 1, kItargetMask, itarget) && put(r, kRegIres, kPianoChannelMask, kPianoChannelMask) &&
        put(r, kRegTimer0 + 0, 0xff, 0) && put(r, kRegTimer0 + 1, 0xff, 0) && put(r, kRegModuleEn, kModuleEnBit, kModuleEnBit) &&
        put(r, kRegChanEn, kPianoChannelMask, kPianoChannelMask);
    return r;
  }

  // Torch off: channels, module, current to the lowest step. Every step is tried even after a failure.
  StepResult off()
  {
    StepResult r;
    put(r, kRegChanEn, kPianoChannelMask, 0);
    put(r, kRegModuleEn, kModuleEnBit, 0);
    put(r, kRegItarget0 + 0, kItargetMask, 0);
    put(r, kRegItarget0 + 1, kItargetMask, 0);
    return r;
  }

  // The live state while on: status bytes and the enables, over the observer channel.
  struct Live {
    bool ok = false;
    uint8_t status1 = 0, status2 = 0, status3 = 0, moduleEn = 0, chanEn = 0;
    bool lit() const { return (moduleEn & kModuleEnBit) && (chanEn & kPianoChannelMask) == kPianoChannelMask; }
  };
  Live live() const
  {
    Live l;
    uint8_t st[4] = {}, en = 0, ch = 0;   // 0x06..0x09: STATUS1, STATUS2, (0x08), STATUS3
    l.ok = bus_.read(bus_.ctx, kRegStatus1, st, 4) && bus_.read(bus_.ctx, kRegModuleEn, &en, 1) && bus_.read(bus_.ctx, kRegChanEn, &ch, 1);
    l.status1 = st[0];
    l.status2 = st[1];
    l.status3 = st[3];
    l.moduleEn = en;
    l.chanEn = ch;
    return l;
  }

 private:
  void remember(uint8_t reg, uint8_t value)
  {
    cache_[reg] = value;
    known_[reg] = true;
  }
  static bool fail(StepResult &r, uint8_t reg, uint8_t want, uint8_t got, const char *why)
  {
    if (r.ok) {
      r.ok = false;
      r.failedReg = reg;
      r.failedWant = want;
      r.failedGot = got;
      r.why = why;
    }
    return false;
  }
  Bus bus_;
  uint8_t cache_[256] = {};
  bool known_[256] = {};
};

// ----------------------------------------------------------------------------------------------------------- the guard
enum class Reason : uint8_t {
  none, user, timeout, test, bootFoundOn, pmicThermal, pmicCurrent, pmicVoltage, shortCircuit, socHot, batteryHot,
  statusUnreadable, lostState, writeFailed, sleep, shutdown
};
inline const char *reasonName(Reason r)
{
  switch (r) {
    case Reason::none: return "none";
    case Reason::user: return "asked";
    case Reason::timeout: return "the time limit";
    case Reason::test: return "the boot test is over";
    case Reason::bootFoundOn: return "found on when the driver started";
    case Reason::pmicThermal: return "the module's over-temperature bits";
    case Reason::pmicCurrent: return "the module's current-limit bits";
    case Reason::pmicVoltage: return "the module's low-supply bit";
    case Reason::shortCircuit: return "a channel's short-circuit bit";
    case Reason::socHot: return "the SoC temperature (PianoThermal)";
    case Reason::batteryHot: return "the battery temperature (PianoPower)";
    case Reason::statusUnreadable: return "the status could not be read twice in a row";
    case Reason::lostState: return "the enables no longer read on";
    case Reason::writeFailed: return "a write or its read-back failed";
    case Reason::sleep: return "system sleep";
    case Reason::shutdown: return "shutdown or restart";
  }
  return "?";
}

struct GuardConfig {
  uint32_t defaultS = 300;        // piano-torch-timeout: the auto-off of a request without its own time
  uint32_t maxS = 1800;           // no request lasts longer
  int32_t socMaxDeci = 850;       // piano-torch-soc-max (C): the hottest TSENS controller (PianoThermal's warm level)
  int32_t batteryMaxCenti = 4500; // piano-torch-batt-max (C): the gauges' battery temperature
  uint32_t coolDownMs = 60000;    // after a thermal or fault off, no new on for this long
};

// The temperatures other drivers publish (absent when that driver is not running: the time limit and the module's own bits
// still apply).
struct Temperatures {
  bool socValid = false, batteryValid = false;
  int32_t socDeci = 0, batteryCenti = 0;
};

// The decision on one poll of a lit torch.
inline Reason judge(const Controller::Live &l, unsigned &unreadable, const Temperatures &t, const GuardConfig &c)
{
  if (!l.ok) return ++unreadable >= 2 ? Reason::statusUnreadable : Reason::none;
  unreadable = 0;
  const Faults f = decodeFaults(l.status1, l.status2, l.status3, kPianoChannelMask);
  if (f.thermal) return Reason::pmicThermal;
  if (f.shortCircuit) return Reason::shortCircuit;
  if (f.current) return Reason::pmicCurrent;
  if (f.voltage) return Reason::pmicVoltage;
  // f.timeout (STATUS3) is not a reason of its own: a torch runs with the safety timer off, and a timer that did fire turned the
  // channel off, which the enables show (lostState).
  if (!l.lit()) return Reason::lostState;
  if (t.socValid && t.socDeci >= c.socMaxDeci) return Reason::socHot;
  if (t.batteryValid && t.batteryCenti >= c.batteryMaxCenti) return Reason::batteryHot;
  return Reason::none;
}

// Why an on request is refused before any write (the temperatures), or Reason::none.
inline Reason refuseOn(const Temperatures &t, const GuardConfig &c)
{
  if (t.socValid && t.socDeci >= c.socMaxDeci) return Reason::socHot;
  if (t.batteryValid && t.batteryCenti >= c.batteryMaxCenti) return Reason::batteryHot;
  return Reason::none;
}
inline bool coolsDown(Reason r)
{
  return r == Reason::pmicThermal || r == Reason::pmicCurrent || r == Reason::pmicVoltage || r == Reason::shortCircuit ||
         r == Reason::socHot || r == Reason::batteryHot || r == Reason::writeFailed;
}

// ------------------------------------------------------------------------------------------------- the torch's state
// One owner (the kext's thread, under its lock) calls these. ma 0 = off.
class Torch {
 public:
  Torch() = default;
  Torch(const Bus &bus, const GuardConfig &c, unsigned maxMa) { configure(bus, c, maxMa); }
  void configure(const Bus &bus, const GuardConfig &c, unsigned maxMa)
  {
    ctl_.setBus(bus);
    cfg_ = c;
    maxMa_ = clampMa(maxMa, maxMa);
  }

  struct Outcome {
    bool lit = false;           // the torch is on after this
    Reason reason = Reason::none;   // why it is off (or why an on was refused)
    StepResult steps;           // the writes of this action
    Controller::Live live;      // the state read after an on
  };

  // On at ma (clamped) for seconds (0: the default; at most maxS), or off when ma is 0.
  Outcome request(uint64_t nowMs, unsigned ma, unsigned seconds, const Temperatures &t, Reason offReason = Reason::user)
  {
    if (!ma) return turnOff(nowMs, offReason);
    Outcome o;
    const Reason refused = nowMs < coolUntilMs_ ? lastReason_ : refuseOn(t, cfg_);
    if (refused != Reason::none) {
      o.reason = refused;
      o.lit = lit_;
      return o;
    }
    ma = clampMa(ma, maxMa_);
    const uint8_t itarget = itargetForMa(ma);
    o.steps = ctl_.on(itarget, clampForMa(maxMa_));
    if (!o.steps.ok) {
      const StepResult undo = ctl_.off();
      o.steps.writes += undo.writes;
      o.steps.readbacks += undo.readbacks;
      settleOff(nowMs, Reason::writeFailed);
      o.reason = Reason::writeFailed;
      return o;
    }
    o.live = ctl_.live();
    unsigned unreadable = 0;
    const Reason bad = o.live.ok ? judge(o.live, unreadable, t, cfg_) : Reason::statusUnreadable;
    if (bad != Reason::none) {
      const StepResult undo = ctl_.off();
      o.steps.writes += undo.writes;
      o.steps.readbacks += undo.readbacks;
      settleOff(nowMs, bad);
      o.reason = bad;
      return o;
    }
    lit_ = true;
    ma_ = maForItarget(itarget);
    itarget_ = itarget;
    unreadable_ = 0;
    if (!seconds) seconds = cfg_.defaultS;
    if (seconds > cfg_.maxS) seconds = cfg_.maxS;
    deadlineMs_ = nowMs + uint64_t(seconds) * 1000;
    o.lit = true;
    return o;
  }

  // While lit, about once a second: the time limit, then the guard. Returns the reason it went off (Reason::none: still lit).
  Outcome poll(uint64_t nowMs, const Temperatures &t)
  {
    Outcome o;
    if (!lit_) return o;
    if (nowMs >= deadlineMs_) return turnOff(nowMs, testMode_ ? Reason::test : Reason::timeout);
    o.live = ctl_.live();
    const Reason bad = judge(o.live, unreadable_, t, cfg_);
    if (bad != Reason::none) return turnOff(nowMs, bad);
    o.lit = true;
    return o;
  }

  Outcome turnOff(uint64_t nowMs, Reason why)
  {
    Outcome o;
    o.steps = ctl_.off();
    settleOff(nowMs, o.steps.ok ? why : (why == Reason::user ? Reason::writeFailed : why));
    o.reason = lastReason_;
    return o;
  }

  void setTestMode(bool on) { testMode_ = on; }
  bool lit() const { return lit_; }
  unsigned ma() const { return lit_ ? ma_ : 0; }
  uint8_t itarget() const { return itarget_; }
  unsigned maxMa() const { return maxMa_; }
  Reason lastReason() const { return lastReason_; }
  uint64_t deadlineMs() const { return deadlineMs_; }
  uint64_t coolUntilMs() const { return coolUntilMs_; }
  const GuardConfig &config() const { return cfg_; }
  Controller &controller() { return ctl_; }

  // One line for the piano-torch-state property and the log.
  void describe(uint64_t nowMs, char *out, unsigned size) const
  {
    if (lit_) {
      const uint64_t left = deadlineMs_ > nowMs ? (deadlineMs_ - nowMs + 999) / 1000 : 0;
      snprintf(out, size, "on %u mA (2 x %u mA, itarget %u), off in %llu s", ma_, ma_ / 2, itarget_, (unsigned long long)left);
    } else {
      snprintf(out, size, "off (%s)", reasonName(lastReason_));
    }
  }

 private:
  void settleOff(uint64_t nowMs, Reason why)
  {
    lit_ = false;
    ma_ = 0;
    lastReason_ = why;
    testMode_ = false;
    if (coolsDown(why)) coolUntilMs_ = nowMs + cfg_.coolDownMs;
  }

  Controller ctl_;
  GuardConfig cfg_;
  unsigned maxMa_ = kTorchMinMa;
  bool lit_ = false, testMode_ = false;
  unsigned ma_ = 0, unreadable_ = 0;
  uint8_t itarget_ = 0;
  uint64_t deadlineMs_ = 0, coolUntilMs_ = 0;
  Reason lastReason_ = Reason::none;
};

}  // namespace pianotorch
#endif
