// SPDX-License-Identifier: BSD-2-Clause-Patent
// The charger's charging-enable command (docs/drivers/charging-policy.md §19): CHGR +0x42 bit 0 set again when it reads clear while
// CHGR_CFG2 says the command register (not the pin) decides, the charger reads disabled, a source is on the port and both packs are
// below the float voltage, not full and in 15-45 °C; re-checked with every report, at most five writes a boot. Every change of the
// charger's registers between two reports is logged (the time window of a stop, and whether the enable command took part in it).
// piano-power-chgen=1 (default) does that, =0 reads and logs only; =2 also pulses an enable that already reads on (bit 0 cleared for
// 2 s, then set again: an edge of the command, which may restart the charger's state machine, a hypothesis), at most three a boot,
// two minutes apart.
//
// Register facts, read for the facts only (no code copied): Linux's drivers/power/supply/qcom_smbx.c (GPL-2.0; PMI8998, the SCHG
// family's one mainline host driver): CHGR +0x42 CHARGING_ENABLE_CMD bit 0; CHGR +0x51 CHGR_CFG2: bit 7 CHG_EN_SRC (0: the command
// register, "software control"; 1: the pin), bit 6 CHG_EN_POLARITY (the pin's polarity in hardware control), bit 5 pre-to-fast
// transition, 4 BAT_OV ECC, 3 I_TERM, 2 AUTO_RECHG, 1 analog drop in VBATT, 0 CHARGER_INHIBIT (Linux writes the whole register
// 0x01: software control, active high, inhibit by voltage). CHGR's place on piano is ChargerWdog.h's (Type-C - 0x500 = 0x2600).
//
// Writes: CHGR +0x42 only, through the CHGR APID owned by EE 0 (this processor). The charger blocks appear twice in the arbiter's map
// (2026-11-12 dev14: APIDs 547-558 owned by EE 3 and 698-708 owned by EE 0, the same PPIDs 0x722-0x72d); spmiFindApid takes EE 0's, the
// other one is never written. EE 3 is (inferred) the audio DSP's channel: the processor whose charger firmware drives these blocks
// under Linux and the stock system (EE 4 owns the regulators and the RTC: the AOP's). The value written is the register read a
// moment before with bit 0 alone changed, read back after. The float voltage, currents, JEITA, the inhibit and every other bit stay.
#ifndef PIANO_POWER_CHARGER_ENABLE_H
#define PIANO_POWER_CHARGER_ENABLE_H
#include <stdint.h>
#include "SpmiObserver.h"
#include "ChargerWdog.h"

namespace pianopower {

constexpr uint8_t kChgEnableBit = 0x01;                          // CHGR +0x42 bit 0
constexpr uint8_t kChgEnSrcPin = 0x80, kChgEnPolarity = 0x40;     // CHGR_CFG2 (+0x51) bits 7, 6
constexpr int kChgEnMaxPackMv = 4350;     // 50 mV under the DSP firmware's 4400 mV float (stock full 4480 mV)
constexpr int kChgEnMinDeciC = 150, kChgEnMaxDeciC = 450;
constexpr uint32_t kChgEnMaxSets = 5;     // a boot
constexpr uint32_t kChgEnPulseHoldMs = 2000, kChgEnPulseGapMs = 120000, kChgEnMaxPulses = 3, kChgEnRestoreRetryMs = 1000;

enum class ChgEnMode : uint8_t { off = 0, restore = 1, pulse = 2 };

inline const char *chgEnModeName(ChgEnMode m)
{
  return m == ChgEnMode::off ? "read only" : m == ChgEnMode::restore ? "set the enable command when it reads off"
                                                                     : "set it when off, pulse it when on and disabled";
}

// What the battery and the port say at the report that runs the check (the kext fills it from the gauges and the Type-C block).
struct ChgEnInputs {
  bool packsOk = false;        // both gauges read
  int maxPackMv = 0;           // the higher pack's voltage
  int minDeciC = 0, maxDeciC = 0;
  bool anyFull = false;        // a pack's FC bit
  int maxSocPct = 0;
  bool sourcePresent = false;  // the Type-C block sees a source's Rp
  bool dspOwns = false;        // level 2 with the DSP running: its firmware owns the enable
};

struct ChgEnWrite {
  bool done = false, ok = false, readBack = false;
  const char *what = "";       // "set", "pulse off", "pulse on"
  uint8_t before = 0, value = 0, after = 0, stateAfter = 0;
  uint32_t status = 0;
};

// One check's outcome.
struct ChgEnResult {
  bool changed = false;        // a register of CHGR or MISC differs from the previous check's (diffViews says which)
  bool cmdCleared = false;     // bit 0 of +0x42 went 1 -> 0 since the previous check, not by a write of ours
  bool cmdSet = false;         // bit 0 went 0 -> 1, not by a write of ours
  uint8_t cmdWas = 0, cmdNow = 0, stateWas = 0;
  uint64_t sinceMs = 0;        // the previous check's time: a change is between it and now
  bool judged = false;         // the first check after a write of ours: what the charger says now
  const char *judgedWhat = "";
  uint64_t judgedWriteMs = 0;
  ChgEnWrite write;            // a write this check made
  const char *why = "";        // why no write (empty: none was needed or one was made)
};

// "+06 c3>87 +07 19>00 ..." for every byte that differs between two views (CHGR +0x06..+0x0d, its rt, +0x40..+0x47, +0x50..+0x57;
// MISC +0x06..+0x0d, its rt, +0x51, +0x53). Returns how many differ; out is always terminated.
inline unsigned diffViews(const WdogView &a, const WdogView &b, char *out, unsigned size)
{
  unsigned n = 0, at = 0;
  auto put = [&](char c) {
    if (at + 1 < size) out[at++] = c;
  };
  auto hex = [&](uint8_t v) {
    static const char kHex[] = "0123456789abcdef";
    put(kHex[v >> 4]);
    put(kHex[v & 15]);
  };
  auto text = [&](const char *s) {
    while (*s) put(*s++);
  };
  bool chgrSaid = false, miscSaid = false;
  auto one = [&](bool misc, uint8_t reg, uint8_t x, uint8_t y) {
    if (x == y) return;
    if (!misc && !chgrSaid) { text("CHGR"); chgrSaid = true; }
    if (misc && !miscSaid) { text(chgrSaid ? "; MISC" : "MISC"); miscSaid = true; }
    text(" +");
    hex(reg);
    put(' ');
    hex(x);
    put('>');
    hex(y);
    n++;
  };
  if (a.chgrOk && b.chgrOk) {
    for (unsigned i = 0; i < 8; ++i) one(false, uint8_t(0x06 + i), a.chgr[i], b.chgr[i]);
    one(false, kBlockRt, a.chgrRt, b.chgrRt);
  }
  if (a.chgrCmdOk && b.chgrCmdOk)
    for (unsigned i = 0; i < 8; ++i) one(false, uint8_t(kChgrCmd + i), a.chgrCmd[i], b.chgrCmd[i]);
  if (a.chgrCfgOk && b.chgrCfgOk)
    for (unsigned i = 0; i < 8; ++i) one(false, uint8_t(kChgrCfg + i), a.chgrCfg[i], b.chgrCfg[i]);
  if (a.miscOk && b.miscOk) {
    for (unsigned i = 0; i < 8; ++i) one(true, uint8_t(0x06 + i), a.misc[i], b.misc[i]);
    one(true, kBlockRt, a.miscRt, b.miscRt);
  }
  if (a.cfgOk && b.cfgOk) {
    one(true, kMiscWdCfg, a.wdCfg, b.wdCfg);
    one(true, kMiscWdTimeouts, a.timeouts, b.timeouts);
  }
  if (size) out[at] = 0;
  return n;
}

// Another APID mapping the same PPID (the charger blocks' second, EE 3 mapping): -1 when there is none.
inline Apid otherMapping(const Io &io, const Arbiter &a, uint16_t address, int except)
{
  Apid found;
  const uint16_t ppid = uint16_t(kChargerSid << 8 | address >> 8);
  for (uint32_t i = 0; i < a.count; ++i) {
    const uint32_t map = spmiMap(io, i);
    if (!map || spmiPpid(map) != ppid || int(i) == except) continue;
    found.apid = int(i);
    found.owner = spmiOwner(io, i);
    break;
  }
  return found;
}

class ChargerEnable {
 public:
  ChgEnMode mode = ChgEnMode::restore;
  WdogBlock chgr;
  bool located = false;
  const char *why = "not located";   // the block gate (empty: writes allowed)
  uint32_t sets = 0, setFailures = 0, pulses = 0, pulseFailures = 0, clearedByOthers = 0, refused = 0;
  bool pulseOpen = false;            // bit 0 cleared by a pulse and not yet set again
  uint64_t pulseDue = 0, lastPulseMs = 0;
  uint32_t restoreTries = 0;

  // CHGR as ChargerWdog located it (the EE 0 entry of its PPID, TYPE/SUBTYPE read).
  void locate(const ChargerWdog &w)
  {
    located = w.located;
    chgr = w.chgr;
    why = gate();
  }

  // The allowlist: CHGR +0x42 alone, bit 0 alone changed from the value read before it: set from clear (any mode but off), clear
  // from set only while a pulse is started (mode pulse).
  bool allowed(int apid, uint8_t reg, uint8_t value, uint8_t before, bool pulseOff) const
  {
    if (apid < 0 || apid != chgr.apid || why[0] || reg != kChgrEnableCmd || mode == ChgEnMode::off) return false;
    if (!pulseOff) return !(before & kChgEnableBit) && value == uint8_t(before | kChgEnableBit);
    return mode == ChgEnMode::pulse && (before & kChgEnableBit) && value == uint8_t(before & ~kChgEnableBit);
  }

  // One check with every report, on the view ChargerWdog::check just read: the changes since the previous check, then (by the mode
  // and the gates) the set or the start of a pulse.
  // *diff (diffSize bytes) gets diffViews' text of the changes, when given.
  ChgEnResult check(const Io &io, const SpmiWriter &w, const WdogView &v, const ChgEnInputs &in, uint64_t now, char *diff = nullptr,
                    unsigned diffSize = 0)
  {
    ChgEnResult r;
    r.cmdNow = v.chgrCmd[2];
    if (diff && diffSize) diff[0] = 0;
    if (havePrev) {
      char scratch[2];
      r.changed = (diff && diffSize ? diffViews(prev, v, diff, diffSize) : diffViews(prev, v, scratch, sizeof scratch)) != 0;
      r.sinceMs = prevMs;
      r.cmdWas = prev.chgrCmd[2];
      r.stateWas = prev.state();
      if (prev.chgrCmdOk && v.chgrCmdOk && !pulseOpen) {
        r.cmdCleared = (r.cmdWas & kChgEnableBit) && !(r.cmdNow & kChgEnableBit);
        r.cmdSet = !(r.cmdWas & kChgEnableBit) && (r.cmdNow & kChgEnableBit);
        if (r.cmdCleared) clearedByOthers++;
      }
    }
    if (judgePending) {
      r.judged = true;
      r.judgedWhat = judgeWhat;
      r.judgedWriteMs = judgeMs;
      judgePending = false;
    }
    prev = v;
    prevMs = now;
    havePrev = true;
    r.why = decide(io, w, v, in, now, &r.write);
    if (r.write.done && r.write.readBack) prev.chgrCmd[2] = r.write.after;   // the next diff shows others' changes only
    return r;
  }

  // The second half of a pulse (the thread calls it at pulseDue): bit 0 set again from the value read now; retried every second
  // until it reads back set. false when nothing was due.
  bool restoreIfDue(const Io &io, const SpmiWriter &w, uint64_t now, ChgEnWrite *out = nullptr)
  {
    if (!pulseOpen || now < pulseDue) return false;
    uint8_t cmd = 0;
    ChgEnWrite r;
    if (spmiRead(io, chgr.apid, kChgrEnableCmd, &cmd, 1) != ReadResult::ok) {
      r.done = true;
      r.what = "pulse on (the command did not read)";
    } else if (cmd & kChgEnableBit) {
      r = ChgEnWrite();   // already set (by someone else): the pulse is over
      r.done = true;
      r.ok = r.readBack = true;
      r.what = "pulse on (already set)";
      r.before = r.value = r.after = cmd;
    } else {
      r = writeBit(io, w, "pulse on", cmd, false);
    }
    restoreTries++;
    if (r.readBack && havePrev) prev.chgrCmd[2] = r.after;   // the next diff shows others' changes only
    if (r.ok) {
      pulseOpen = false;
      if (!pulseOffFailed) pulses++;   // a failed clear is counted as a failure already
      pulseOffFailed = false;
      judgePending = true;
      judgeWhat = "pulse";
      judgeMs = now;
    } else {
      pulseDue = now + (restoreTries < 10 ? kChgEnRestoreRetryMs : 30000);   // every second, after ten tries every 30 s
    }
    if (out) *out = r;
    return true;
  }

  uint64_t nextDue() const { return pulseOpen ? pulseDue : 0; }

 private:
  WdogView prev;
  uint64_t prevMs = 0;
  bool havePrev = false;
  bool pulseOffFailed = false, pulsedOnce = false;
  bool judgePending = false;
  const char *judgeWhat = "";
  uint64_t judgeMs = 0;

  const char *gate() const
  {
    if (!located) return "CHGR not located";
    if (chgr.apid < 0) return "no CHGR block at Type-C - 0x500";
    if (chgr.owner != 0) return "CHGR's APID is another EE's";
    if (!chgr.idOk || chgr.type != kSchgType) return "CHGR's TYPE is not the charger's (0x02): the map does not fit";
    return "";
  }

  ChgEnWrite writeBit(const Io &io, const SpmiWriter &w, const char *what, uint8_t before, bool pulseOff)
  {
    ChgEnWrite r;
    r.done = true;
    r.what = what;
    r.before = before;
    r.value = pulseOff ? uint8_t(before & ~kChgEnableBit) : uint8_t(before | kChgEnableBit);
    if (!w.write || !allowed(chgr.apid, kChgrEnableCmd, r.value, before, pulseOff)) {
      refused++;
      return r;
    }
    r.ok = w.write(w.ctx, chgr.apid, kChgrEnableCmd, r.value, &r.status);
    r.readBack = spmiRead(io, chgr.apid, kChgrEnableCmd, &r.after, 1) == ReadResult::ok;
    uint8_t st = 0;
    if (spmiRead(io, chgr.apid, kChgrStatus1, &st, 1) == ReadResult::ok) r.stateAfter = uint8_t(st & kChgrStateMask);
    r.ok = r.ok && r.readBack && r.after == r.value;
    return r;
  }

  const char *decide(const Io &io, const SpmiWriter &w, const WdogView &v, const ChgEnInputs &in, uint64_t now, ChgEnWrite *out)
  {
    if (why[0]) return why;
    if (mode == ChgEnMode::off) return "piano-power-chgen=0: read only";
    if (pulseOpen) return "a pulse is open";
    if (!v.chgrOk || !v.chgrCmdOk || !v.chgrCfgOk) return "CHGR did not read";
    const uint8_t cmd = v.chgrCmd[2], cfg2 = v.chgrCfg[1];
    if (cfg2 & kChgEnSrcPin) return "the enable is the pin's (CHGR_CFG2 bit 7): not the command register's";
    if (cfg2 & kChgEnPolarity) return "CHGR_CFG2 bit 6 is set: not the software-control configuration this rule knows";
    if (v.state() != kChgrDisabled) return "";   // charging or done by its own rules: nothing to do
    if (in.dspOwns) return "the DSP runs: its firmware owns the enable";
    if (!in.sourcePresent) return "no source on the port";
    if (!in.packsOk) return "the packs did not both read";
    if (in.anyFull || in.maxSocPct >= 100) return "a pack reads full";
    if (in.maxPackMv >= kChgEnMaxPackMv) return "a pack is at 4350 mV or above";
    if (in.minDeciC < kChgEnMinDeciC || in.maxDeciC > kChgEnMaxDeciC) return "a pack is outside 15-45 C";
    if (!(cmd & kChgEnableBit)) {
      if (sets + setFailures >= kChgEnMaxSets) return "five sets this boot: none more";
      *out = writeBit(io, w, "set", cmd, false);
      (out->ok ? sets : setFailures)++;
      judgePending = true;
      judgeWhat = "set";
      judgeMs = now;
      return "";
    }
    if (mode != ChgEnMode::pulse) return "the enable command is on: the charger is disabled by something else";
    if (pulses + pulseFailures >= kChgEnMaxPulses) return "the enable command is on; three pulses this boot: none more";
    if (pulsedOnce && now - lastPulseMs < kChgEnPulseGapMs) return "the enable command is on; the next pulse two minutes after the last";
    *out = writeBit(io, w, "pulse off", cmd, true);
    lastPulseMs = now;
    pulsedOnce = true;
    if (out->ok) {
      pulseOpen = true;
      pulseDue = now + kChgEnPulseHoldMs;
      restoreTries = 0;
    } else {
      pulseFailures++;
      // a clear that went through without reading back: set it again at once
      if (out->readBack && !(out->after & kChgEnableBit)) {
        pulseOpen = pulseOffFailed = true;
        pulseDue = now;
        restoreTries = 0;
      }
    }
    return "";
  }
};

}  // namespace pianopower
#endif
