// SPDX-License-Identifier: BSD-2-Clause-Patent
// The charger complex's whole picture, the moment of its stop, and a gated recovery of a charger that reads DISABLED with its
// enable command on (docs/drivers/charging-policy.md §20).
//
// 1. The snapshot (every level, read only). The seven blocks at their places relative to the Type-C block ChargerPmic.h finds
//    (the SCHG family's order, PM8150B's 0x1000..0x1600: CHGR -0x500, DCDC -0x400, BATIF -0x300, USBIN -0x200, DCIN -0x100, Type-C,
//    MISC +0x100; on piano CHGR 0x2600 .. MISC 0x2c00, inferred: Type-C's place and its status decode are measured, CHGR's state
//    byte fits, the rest is the family's order), each +0x06..+0x10 (status and real-time interrupt status) and +0x40..+0xbf
//    (commands and configuration). With every report: one console line of what changed since the previous report, and the
//    picture in the IORegistry (PianoPower.cpp: PianoCharger, PianoChargerText, PianoChargerChanges) for a shell to watch.
// 2. The watch (every level, read only): the status part every 2 s for the first three minutes of PianoPower and for 90 s after
//    each recovery step; a change of CHGR's state is logged with every status byte that moved within those 2 s.
// 3. The recovery (level 1 only; piano-power-chgfix=1 or 2, default 0 = none). When CHGR has read DISABLED for 20 s with its
//    enable command on, the command register in control, and every gate of ChargerEnable's set passing (a source on the port, both
//    packs read, below 4350 mV, not full, 15-45 C), one step, judged 15 s after its last write by the CHGR state and the packs:
//      A  the enable command pulsed: CHGR +0x42 bit 0 off for 2 s, then on (ChargerEnable::fixPulse, its allowlist);
//      B  AICL restarted: MISC +0x44 0x00 -> 0x02 (only when it reads 0x00: a command register at rest);
//      C  (=2 only) the USB input suspended for 1 s: USBIN +0x40 bit 0 set, then clear (only with USBIN's plug-in status bit set and
//         bit 0 reading clear before), the clear retried every second until it reads back; at the next boot's start a suspend found
//         set (a reset inside that second) is cleared (chgfix=1 or 2).
//    The steps in that order, the next one 15 s after a judge that says "no change"; a step that brings charging back is the one
//    used again on a later stop, two minutes apart; six attempts a boot at most; when no step works, none more. Every write is
//    read back and logged; no limit is written (currents, float voltage, JEITA, timers, the inhibit).
//
// Register facts, read for the facts only (no code copied): the downstream SMB5 charger driver of the PM8150B generation
// (smb5-reg.h, qpnp-smb5.c, smb5-lib.c, GPL-2.0) and mainline qcom_smbx.c (PMI8998): MISC +0x44 AICL_CMD (bit 1 restart, bit 0
// rerun; the driver sets the bit, skipped while the input is suspended); USBIN +0x40 USBIN_CMD_IL bit 0 suspend (the driver's
// suspend vote, also its charge-termination workaround); USBIN real-time status bit 4 usbin-plugin; CHGR +0x42 bit 0 the charging
// enable (SMB5 toggles it off and on around a JEITA zone change), +0x43 bit 0 the charging pause; CHGR +0x06 bits 2:0 the state
// (SMB5: 0 inhibit, 1 trickle, 2 pre, 3 full-on, 4 taper, 5 terminate, 6 pause, 7 disable; PMI8998 differs below 3, both have 3
// full-on and 7 disable); the CHGR real-time bits (0 chgr-error, 1 chg-state-change, ...), MISC's (0 snarl, 1 bark, 2 aicl-fail,
// 3 aicl-done, ...), BATIF's (... 4 therm/id missing, 5 terminal missing, 6 buck-oc, 7 vph-ov), the DCDC ICL status +0x06..+0x08
// and AICL status +0x0a (bit 0 done) in the vendor DT's and header's order. On PMIH010x every one of these is inferred.
#ifndef PIANO_POWER_CHARGER_FIX_H
#define PIANO_POWER_CHARGER_FIX_H
#include <stdint.h>
#include "SpmiObserver.h"
#include "ChargerPmic.h"
#include "ChargerWdog.h"
#include "ChargerEnable.h"

namespace pianopower {

// ------------------------------------------------------------------------------------------------------------- the snapshot
constexpr unsigned kCxBlocks = 7;
enum : unsigned { kCxChgr = 0, kCxDcdc = 1, kCxBatif = 2, kCxUsbin = 3, kCxDcin = 4, kCxTypec = 5, kCxMisc = 6 };
constexpr uint8_t kCxStatus = 0x06, kCxStatusBytes = 11;   // +0x06..+0x10: status, then the real-time interrupt status
constexpr uint8_t kCxCfg = 0x40;
constexpr unsigned kCxCfgBytes = 0x80;                     // +0x40..+0xbf: commands and configuration, read in 8-byte pieces

inline const char *cxName(unsigned b)
{
  static const char *const kNames[kCxBlocks] = {"CHGR", "DCDC", "BATIF", "USBIN", "DCIN", "TYPEC", "MISC"};
  return b < kCxBlocks ? kNames[b] : "?";
}
inline int cxOffsetFromTypec(unsigned b) { return -0x500 + 0x100 * int(b); }   // CHGR -0x500 .. Type-C 0 .. MISC +0x100

struct CxBlock {
  uint16_t address = 0;
  int apid = -1;
  uint32_t owner = 0;
  bool idOk = false;
  uint8_t type = 0, subtype = 0;
  bool ours() const { return apid >= 0 && owner == 0 && idOk && type == kSchgType; }
};

struct CxRegs {
  bool statusOk = false;
  uint8_t status[kCxStatusBytes] = {};   // [0] = +0x06 .. [9] = +0x0f, [10] = +0x10 (rt)
  uint16_t cfgOk = 0;                    // bit n: +0x40 + 8n .. +0x47 + 8n read
  uint8_t cfg[kCxCfgBytes] = {};
};

struct CxSnapshot {
  bool taken = false, full = false;      // full: the configuration part was read too
  uint64_t ms = 0;
  CxRegs r[kCxBlocks];
  bool chgrOk() const { return taken && r[kCxChgr].statusOk; }
  uint8_t chgrState() const { return uint8_t(r[kCxChgr].status[0] & kChgrStateMask); }
  // +reg of block b when it was read (false otherwise)
  bool reg(unsigned b, uint8_t address, uint8_t *v) const
  {
    if (!taken || b >= kCxBlocks) return false;
    const CxRegs &x = r[b];
    if (address >= kCxStatus && address < kCxStatus + kCxStatusBytes) {
      if (!x.statusOk) return false;
      *v = x.status[address - kCxStatus];
      return true;
    }
    if (address >= kCxCfg && unsigned(address) < kCxCfg + kCxCfgBytes) {
      const unsigned i = unsigned(address - kCxCfg);
      if (!full || !(x.cfgOk & (1u << (i / 8)))) return false;
      *v = x.cfg[i];
      return true;
    }
    return false;
  }
};

// A charging state in either enum (1..4: trickle/pre/fast or pre/full-on/taper); 0 is trickle in PMI8998's and inhibit in SMB5's.
inline bool chgrCharging(uint8_t state) { return state >= 1 && state <= 4; }

class ChargerComplex {
 public:
  CxBlock b[kCxBlocks];
  bool located = false;

  void locate(const Io &io, const Arbiter &a, const ChargerLayout &l)
  {
    located = false;
    if (!l.found || l.typec.address < 0x500) return;
    for (unsigned i = 0; i < kCxBlocks; ++i) {
      CxBlock &x = b[i];
      x = CxBlock();
      x.address = uint16_t(int(l.typec.address) + cxOffsetFromTypec(i));
      const Apid found = spmiFindApid(io, a, uint16_t(kChargerSid << 8 | x.address >> 8));
      x.apid = found.apid;
      x.owner = found.owner;
      uint8_t id[2] = {};
      x.idOk = x.apid >= 0 && spmiRead(io, x.apid, 4, id, 2) == ReadResult::ok;
      x.type = id[0];
      x.subtype = id[1];
    }
    located = true;
  }

  // The status part of every block (2 reads a block), and with `full` the configuration part (16 more).
  CxSnapshot read(const Io &io, bool full, uint64_t now) const
  {
    CxSnapshot s;
    if (!located) return s;
    s.taken = true;
    s.full = full;
    s.ms = now;
    for (unsigned i = 0; i < kCxBlocks; ++i) {
      const CxBlock &x = b[i];
      CxRegs &r = s.r[i];
      if (x.apid < 0) continue;
      r.statusOk = spmiRead(io, x.apid, kCxStatus, r.status, 8) == ReadResult::ok &&
                   spmiRead(io, x.apid, uint8_t(kCxStatus + 8), r.status + 8, kCxStatusBytes - 8) == ReadResult::ok;
      if (!full) continue;
      for (unsigned c = 0; c < kCxCfgBytes / 8; ++c)
        if (spmiRead(io, x.apid, uint8_t(kCxCfg + 8 * c), r.cfg + 8 * c, 8) == ReadResult::ok) r.cfgOk = uint16_t(r.cfgOk | 1u << c);
    }
    return s;
  }
};

// A text writer that never overruns: out is always terminated; `cut` says something did not fit.
struct CxText {
  char *out;
  unsigned size, at = 0;
  bool cut = false;
  CxText(char *o, unsigned n) : out(o), size(n)
  {
    if (size) out[0] = 0;
  }
  void put(char c)
  {
    if (at + 1 < size) {
      out[at++] = c;
      out[at] = 0;
    } else {
      cut = true;
    }
  }
  void text(const char *s)
  {
    while (*s) put(*s++);
  }
  void hex(uint8_t v)
  {
    static const char kHex[] = "0123456789abcdef";
    put(kHex[v >> 4]);
    put(kHex[v & 15]);
  }
};

// "CHGR +06 c3>87 +10 22>20; DCDC +07 1e>14; MISC +44 00>02" for every byte read in both snapshots that differs (the configuration
// part only when both are full). Returns how many differ.
inline unsigned cxDiff(const CxSnapshot &a, const CxSnapshot &b, char *out, unsigned size)
{
  CxText t(out, size);
  unsigned n = 0;
  if (!a.taken || !b.taken) return 0;
  for (unsigned i = 0; i < kCxBlocks; ++i) {
    bool said = false;
    auto one = [&](uint8_t reg, uint8_t x, uint8_t y) {
      if (x == y) return;
      if (!said) {
        if (n) t.text("; ");
        t.text(cxName(i));
        said = true;
      }
      t.text(" +");
      t.hex(reg);
      t.put(' ');
      t.hex(x);
      t.put('>');
      t.hex(y);
      n++;
    };
    const CxRegs &x = a.r[i], &y = b.r[i];
    if (x.statusOk && y.statusOk)
      for (unsigned k = 0; k < kCxStatusBytes; ++k) one(uint8_t(kCxStatus + k), x.status[k], y.status[k]);
    if (a.full && b.full)
      for (unsigned k = 0; k < kCxCfgBytes; ++k)
        if ((x.cfgOk & y.cfgOk) & (1u << (k / 8))) one(uint8_t(kCxCfg + k), x.cfg[k], y.cfg[k]);
  }
  if (t.cut && size >= 4) {   // a visible end of a line that did not fit
    out[size - 4] = '.';
    out[size - 3] = '.';
    out[size - 2] = '.';
    out[size - 1] = 0;
  }
  return n;
}

// "CHGR 87 00 0a 03 05 be 00 38 00 00 rt 20 | DCDC ... | ..." (the status part of every block; "--" for one that did not read).
inline void cxStatusText(const CxSnapshot &s, char *out, unsigned size)
{
  CxText t(out, size);
  for (unsigned i = 0; i < kCxBlocks; ++i) {
    if (i) t.text(" | ");
    t.text(cxName(i));
    const CxRegs &r = s.r[i];
    if (!s.taken || !r.statusOk) {
      t.text(" --");
      continue;
    }
    for (unsigned k = 0; k < kCxStatusBytes - 1; ++k) {
      t.put(' ');
      t.hex(r.status[k]);
    }
    t.text(" rt ");
    t.hex(r.status[kCxStatusBytes - 1]);
  }
}

// ------------------------------------------------------------------------------------------------------------- the watch
struct CxTransition {
  uint8_t from = 0, to = 0;
  uint64_t fromMs = 0, toMs = 0;   // the tick before and the tick that saw it
  bool stop = false;               // a charging state -> DISABLED
  unsigned changed = 0;
  char diff[480] = "";
};

class ChargerWatch {
 public:
  static constexpr uint32_t kPeriodMs = 2000;
  bool active = false;
  uint64_t untilMs = 0, dueMs = 0;
  uint32_t ticks = 0, transitions = 0, stops = 0;
  uint64_t firstStopMs = 0, lastStopMs = 0;
  CxSnapshot last;
  bool haveLast = false;

  // Watch (again) from now until now + forMs at the latest; a running watch keeps its pace.
  void extend(uint64_t now, uint64_t forMs)
  {
    if (now + forMs > untilMs) untilMs = now + forMs;
    if (!active) {
      active = true;
      dueMs = now;
    }
  }
  // 0: no tick pending (the thread's deadline list); a tick due at time 0 reads as 1.
  uint64_t nextDue() const { return active ? (dueMs ? dueMs : 1) : 0; }

  // One tick when due: the status part read; true (and *t) when CHGR's state differs from the previous tick's.
  bool tick(const Io &io, const ChargerComplex &cx, uint64_t now, CxTransition *t)
  {
    if (!active || now < dueMs) return false;
    dueMs = now + kPeriodMs;
    if (dueMs > untilMs) active = false;   // the window is over (extend() opens another)
    ticks++;
    const CxSnapshot s = cx.read(io, false, now);
    return see(s, t);
  }

  // A snapshot taken elsewhere (a report's) also counts: the watch's state stays the latest reading.
  bool see(const CxSnapshot &s, CxTransition *t)
  {
    bool moved = false;
    if (haveLast && s.chgrOk() && last.chgrOk() && s.chgrState() != last.chgrState()) {
      moved = true;
      transitions++;
      CxTransition x;
      x.from = last.chgrState();
      x.to = s.chgrState();
      x.fromMs = last.ms;
      x.toMs = s.ms;
      x.stop = chgrCharging(x.from) && x.to == kChgrDisabled;
      if (x.stop) {
        stops++;
        if (!firstStopMs) firstStopMs = s.ms;
        lastStopMs = s.ms;
      }
      CxSnapshot a = last, b = s;
      a.full = b.full = false;   // the status part only: both readings have it
      x.changed = cxDiff(a, b, x.diff, sizeof x.diff);
      if (t) *t = x;
    }
    if (s.chgrOk()) {
      last = s;
      haveLast = true;
    }
    return moved;
  }
};

// ------------------------------------------------------------------------------------------------------------- the recovery
enum class FixMode : uint8_t { off = 0, light = 1, suspend = 2 };
enum class FixStep : uint8_t { none = 0, pulse = 1, aicl = 2, suspend = 3 };

constexpr uint8_t kMiscAiclCmd = 0x44, kAiclRestart = 0x02;
constexpr uint8_t kUsbinCmdIl = 0x40, kUsbinSuspendBit = 0x01, kUsbinRtPlugin = 0x10;
constexpr uint32_t kFixDisabledMs = 20000;     // DISABLED this long before a step
constexpr uint32_t kFixJudgeMs = 15000;        // a step is judged this long after its last write
constexpr uint32_t kFixStepGapMs = 15000;      // after a "no change", the next step this long after the judge
constexpr uint32_t kFixRepeatGapMs = 120000;   // the step that worked, used again on a later stop, at least this far apart
constexpr uint32_t kFixMaxAttempts = 6;        // a boot
constexpr uint32_t kFixSuspendHoldMs = 1000, kFixRetryMs = 1000, kFixGateRetryMs = 30000;
constexpr int kFixChargingMa = 50;             // the packs at +50 mA or more: charging

inline const char *fixModeName(FixMode m)
{
  return m == FixMode::off ? "off (no recovery write)" : m == FixMode::light ? "steps A (the enable command pulsed) and B (AICL restarted)"
                                                                             : "steps A, B and C (the USB input suspended 1 s)";
}
inline const char *fixStepName(FixStep s)
{
  return s == FixStep::pulse ? "A (the enable command pulsed)" : s == FixStep::aicl ? "B (AICL restarted)"
         : s == FixStep::suspend ? "C (the USB input suspended 1 s)" : "none";
}
inline char fixStepLetter(FixStep s) { return s == FixStep::pulse ? 'A' : s == FixStep::aicl ? 'B' : s == FixStep::suspend ? 'C' : '-'; }

struct FixEvent {
  enum Kind : uint8_t { none, skipped, wrote, judged, finished, guard } kind = none;
  FixStep step = FixStep::none;
  uint32_t attempt = 0;
  const char *what = "", *why = "";
  const char *block = "";
  uint16_t address = 0;
  uint8_t reg = 0, before = 0, value = 0, after = 0, state = 0;
  bool ok = false, readBack = false, charging = false;
  uint32_t status = 0;
  int packsMa = 0;
};

class ChargerFix {
 public:
  FixMode mode = FixMode::off;
  CxBlock chgr, misc, usbin;
  bool located = false;
  const char *why = "not located";   // the block gate (empty: writes possible)
  uint32_t attempts = 0, successes = 0, writeFailures = 0, refused = 0;
  FixStep winner = FixStep::none, step = FixStep::none;
  bool finished = false, suspendOpen = false;
  enum class Phase : uint8_t { idle, pulse, hold, restore, judge } phase = Phase::idle;
  uint64_t phaseDue = 0, disabledSinceMs = 0, nextAttemptMs = 0, stepMs = 0;
  bool disabledSeen = false;         // disabledSinceMs holds the first poll of this DISABLED stretch
  uint32_t restoreTries = 0;
  const char *lastSkip = "";
  // The last outcome, for the IORegistry (PianoPower.cpp formats it): what, the step it was about and the attempt's number.
  const char *statusWhat = "off";
  FixStep statusStep = FixStep::none;
  uint32_t statusAttempt = 0;

  void locate(const ChargerComplex &cx, const ChargerEnable &e)
  {
    located = cx.located && e.located;
    chgr = cx.b[kCxChgr];
    misc = cx.b[kCxMisc];
    usbin = cx.b[kCxUsbin];
    why = !located ? "the charger complex is not located" : e.why[0] ? e.why : "";
    mark(mode == FixMode::off ? "off (piano-power-chgfix=0)" : why[0] ? why : "armed: waiting for a DISABLED charger");
  }

  // The allowlist: MISC +0x44 0x00 -> 0x02 while step B writes; USBIN +0x40 bit 0 alone, set while step C starts, clear while a
  // suspend of ours is open or the start guard runs. Nothing else, and nothing while no write is armed.
  bool allowed(int apid, uint8_t reg, uint8_t value, uint8_t before) const
  {
    if (apid < 0 || why[0] || mode == FixMode::off) return false;
    if (armed == Armed::aicl) return apid == misc.apid && misc.ours() && reg == kMiscAiclCmd && before == 0 && value == kAiclRestart;
    if (apid != usbin.apid || !usbin.ours() || reg != kUsbinCmdIl) return false;
    if (armed == Armed::suspendSet)
      return mode == FixMode::suspend && !(before & kUsbinSuspendBit) && value == uint8_t(before | kUsbinSuspendBit);
    if (armed == Armed::suspendClear) return (before & kUsbinSuspendBit) && value == uint8_t(before & ~kUsbinSuspendBit);
    return false;
  }

  uint64_t nextDue() const
  {
    if (mode == FixMode::off || why[0]) return 0;
    if (phase != Phase::idle) return phaseDue ? phaseDue : 1;   // 0 is "nothing pending" in the thread's deadline list
    if (finished || !disabledSeen) return 0;
    const uint64_t at = disabledSinceMs + kFixDisabledMs;
    return at > nextAttemptMs ? at : nextAttemptMs;
  }
  bool judgeDue(uint64_t now) const { return phase == Phase::judge && now >= phaseDue; }

  // At start (chgfix 1 or 2): a USB input suspend found set (a reset inside step C's second) is cleared when a source is attached.
  FixEvent startGuard(const Io &io, const SpmiWriter &w, bool sourcePresent)
  {
    FixEvent ev;
    if (mode == FixMode::off || why[0] || !usbin.ours()) return ev;
    uint8_t cmd = 0;
    if (spmiRead(io, usbin.apid, kUsbinCmdIl, &cmd, 1) != ReadResult::ok || !(cmd & kUsbinSuspendBit)) return ev;
    ev.step = FixStep::suspend;
    if (!sourcePresent) {
      ev.kind = FixEvent::skipped;
      ev.why = "USBIN +0x40 reads the input suspended, no source on the port: left as it is";
      ev.before = cmd;
      return ev;
    }
    ev = writeOne(io, w, Armed::suspendClear, usbin, "USBIN", kUsbinCmdIl, cmd, uint8_t(cmd & ~kUsbinSuspendBit), "start guard: suspend lifted");
    ev.kind = FixEvent::guard;
    ev.ok = ev.ok && ev.readBack && !(ev.after & kUsbinSuspendBit);
    return ev;
  }

  // The thread's call: when nextDue() is reached, with every report and every watch tick. `in` the latest gauge and port readings,
  // `packsMa` the packs' current (read afresh when judgeDue()), `level1` false at level 2 (no step there).
  FixEvent poll(const Io &io, const SpmiWriter &w, ChargerEnable &e, const ChgEnInputs &in, bool level1, int packsMa, uint64_t now)
  {
    FixEvent ev;
    ev.attempt = attempts;
    if (mode == FixMode::off || why[0]) return ev;
    uint8_t st = 0;
    const bool stOk = spmiRead(io, chgr.apid, kChgrStatus1, &st, 1) == ReadResult::ok;
    const uint8_t state = uint8_t(st & kChgrStateMask);
    switch (phase) {
      case Phase::pulse:   // step A: ChargerEnable sets the bit again (restoreIfDue); judged 15 s after that
        if (e.pulseOpen) {
          phaseDue = now + 500;
          return ev;
        }
        phase = Phase::judge;
        phaseDue = now + kFixJudgeMs;
        return ev;
      case Phase::hold:
      case Phase::restore:
        if (now < phaseDue) return ev;
        return lift(io, w, now);
      case Phase::judge:
        if (now < phaseDue) return ev;
        return judge(stOk, state, packsMa, now);
      case Phase::idle:
        break;
    }
    if (finished || !stOk) return ev;
    if (state != kChgrDisabled) {
      disabledSeen = false;
      lastSkip = "";
      return ev;
    }
    if (!disabledSeen) {
      disabledSeen = true;
      disabledSinceMs = now;
      return ev;
    }
    if (now - disabledSinceMs < kFixDisabledMs || now < nextAttemptMs) return ev;
    if (attempts >= kFixMaxAttempts) return finish("six attempts this boot: none more");
    const FixStep next = winner != FixStep::none ? winner : ladderStep();
    if (next == FixStep::none) return finish("no step brought charging back: none more this boot");
    uint8_t cmd = 0;
    if (const char *g = gate(io, e, in, level1, &cmd)) {
      nextAttemptMs = now + kFixGateRetryMs;
      if (g == lastSkip) return ev;
      lastSkip = g;
      ev.kind = FixEvent::skipped;
      ev.step = next;
      ev.why = g;
      mark(g, next);
      return ev;
    }
    lastSkip = "";
    // the step's own gate: a step that cannot run here is passed over for good
    if (const char *g = stepGate(io, next)) {
      passOver(next);
      nextAttemptMs = now + kFixRetryMs;
      ev.kind = FixEvent::skipped;
      ev.step = next;
      ev.why = g;
      return ev;
    }
    attempts++;
    step = next;
    stepMs = now;
    if (winner == FixStep::none) passOver(next);
    ev.attempt = attempts;
    if (next == FixStep::pulse) {
      ChgEnWrite r;
      if (!e.fixPulse(io, w, now, &r)) {   // read on a moment ago: a race; judged as a step without effect
        phase = Phase::judge;
        phaseDue = now;
        ev.kind = FixEvent::skipped;
        ev.step = next;
        ev.why = "the enable command did not read on at the write";
        return ev;
      }
      ev.kind = FixEvent::wrote;
      ev.step = next;
      ev.what = r.what;
      ev.block = "CHGR";
      ev.address = chgr.address;
      ev.reg = kChgrEnableCmd;
      ev.before = r.before;
      ev.value = r.value;
      ev.after = r.after;
      ev.readBack = r.readBack;
      ev.ok = r.ok;
      ev.status = r.status;
      ev.state = r.stateAfter;
      if (!r.ok) writeFailures++;
      phase = e.pulseOpen ? Phase::pulse : Phase::judge;
      phaseDue = e.pulseOpen ? now + kChgEnPulseHoldMs : now + kFixJudgeMs;
      mark(r.ok ? "the enable command off for 2 s" : "the enable command's clear FAILED", next);
      return ev;
    }
    if (next == FixStep::aicl) {
      ev = writeOne(io, w, Armed::aicl, misc, "MISC", kMiscAiclCmd, 0x00, kAiclRestart, "AICL restart");
      ev.step = next;
      ev.attempt = attempts;
      // a command bit may clear itself: 0x00 or 0x02 read back are both the register this rule expects
      ev.ok = ev.ok && ev.readBack && (ev.after == 0x00 || ev.after == kAiclRestart);
      if (!ev.ok) writeFailures++;
      phase = Phase::judge;
      phaseDue = now + kFixJudgeMs;
      mark(ev.ok ? "AICL restarted: judged in 15 s" : "the AICL restart FAILED", next);
      return ev;
    }
    // step C
    uint8_t before = 0;
    spmiRead(io, usbin.apid, kUsbinCmdIl, &before, 1);   // read by stepGate a moment ago: clear
    ev = writeOne(io, w, Armed::suspendSet, usbin, "USBIN", kUsbinCmdIl, before, uint8_t(before | kUsbinSuspendBit), "suspend");
    ev.step = next;
    ev.attempt = attempts;
    const bool on = ev.readBack ? (ev.after & kUsbinSuspendBit) != 0 : ev.ok;   // unknown: lift it to be sure
    ev.ok = ev.ok && ev.readBack && on;
    if (!ev.ok) writeFailures++;
    if (on || !ev.readBack) {
      suspendOpen = true;
      phase = Phase::hold;
      phaseDue = now + kFixSuspendHoldMs;
      restoreTries = 0;
    } else {
      phase = Phase::judge;
      phaseDue = now + kFixJudgeMs;
    }
    mark(ev.ok ? "the USB input suspended for 1 s" : "the suspend FAILED", next);
    return ev;
  }

 private:
  enum class Armed : uint8_t { none, aicl, suspendSet, suspendClear } armed = Armed::none;
  uint8_t passed = 0;   // the ladder's steps passed over (bit per step)

  void mark(const char *what, FixStep s = FixStep::none)
  {
    statusWhat = what;
    statusStep = s;
    statusAttempt = attempts;
  }

  FixStep ladderStep() const
  {
    const FixStep order[3] = {FixStep::pulse, FixStep::aicl, FixStep::suspend};
    for (FixStep s : order) {
      if (s == FixStep::suspend && mode != FixMode::suspend) continue;
      if (!(passed & (1u << unsigned(s)))) return s;
    }
    return FixStep::none;
  }
  void passOver(FixStep s) { passed = uint8_t(passed | 1u << unsigned(s)); }

  FixEvent finish(const char *reason)
  {
    FixEvent ev;
    finished = true;
    ev.kind = FixEvent::finished;
    ev.attempt = attempts;
    ev.why = reason;
    mark(reason);
    return ev;
  }

  const char *gate(const Io &io, const ChargerEnable &e, const ChgEnInputs &in, bool level1, uint8_t *cmd) const
  {
    if (!level1) return "level 2: the DSP's firmware owns the charger";
    if (e.why[0]) return e.why;
    if (e.pulseOpen) return "a pulse of the enable command is open";
    uint8_t c = 0, cfg2 = 0;
    if (spmiRead(io, chgr.apid, kChgrEnableCmd, &c, 1) != ReadResult::ok || spmiRead(io, chgr.apid, kChgrCfg2, &cfg2, 1) != ReadResult::ok)
      return "CHGR did not read";
    if (cfg2 & (kChgEnSrcPin | kChgEnPolarity)) return "CHGR_CFG2 bit 7 or 6 is set: not the command register's configuration";
    if (!(c & kChgEnableBit)) return "the enable command reads off: that is ChargerEnable's set";
    if (cmd) *cmd = c;
    return chgBatteryGate(in);
  }

  const char *stepGate(const Io &io, FixStep s) const
  {
    if (s == FixStep::aicl) {
      if (!misc.ours()) return "step B: MISC is not this processor's TYPE 0x02 block: passed over";
      uint8_t v = 0;
      if (spmiRead(io, misc.apid, kMiscAiclCmd, &v, 1) != ReadResult::ok) return "step B: MISC +0x44 does not read: passed over";
      if (v != 0) return "step B: MISC +0x44 does not read 0x00 (not a command register at rest): passed over";
    }
    if (s == FixStep::suspend) {
      if (!usbin.ours()) return "step C: USBIN is not this processor's TYPE 0x02 block: passed over";
      uint8_t rt = 0, v = 0;
      if (spmiRead(io, usbin.apid, kBlockRt, &rt, 1) != ReadResult::ok || spmiRead(io, usbin.apid, kUsbinCmdIl, &v, 1) != ReadResult::ok)
        return "step C: USBIN does not read: passed over";
      if (!(rt & kUsbinRtPlugin)) return "step C: USBIN's plug-in status bit (rt bit 4) reads clear: passed over";
      if (v & kUsbinSuspendBit) return "step C: USBIN +0x40 already reads suspended: passed over";
    }
    return nullptr;
  }

  FixEvent writeOne(const Io &io, const SpmiWriter &w, Armed what, const CxBlock &b, const char *name, uint8_t reg, uint8_t before,
                    uint8_t value, const char *label)
  {
    FixEvent ev;
    ev.kind = FixEvent::wrote;
    ev.what = label;
    ev.block = name;
    ev.address = b.address;
    ev.reg = reg;
    ev.before = before;
    ev.value = value;
    armed = what;
    const bool permit = w.write && allowed(b.apid, reg, value, before);
    armed = Armed::none;
    if (!permit) {
      refused++;
      ev.why = "refused by the allowlist";
      return ev;
    }
    ev.ok = w.write(w.ctx, b.apid, reg, value, &ev.status);
    ev.readBack = spmiRead(io, b.apid, reg, &ev.after, 1) == ReadResult::ok;
    uint8_t st = 0;
    if (spmiRead(io, chgr.apid, kChgrStatus1, &st, 1) == ReadResult::ok) ev.state = uint8_t(st & kChgrStateMask);
    return ev;
  }

  // Step C's second half: bit 0 cleared from the value read now, read back; again every second (after 30 tries every 30 s) until
  // it reads clear.
  FixEvent lift(const Io &io, const SpmiWriter &w, uint64_t now)
  {
    FixEvent ev;
    uint8_t cmd = 0;
    restoreTries++;
    if (spmiRead(io, usbin.apid, kUsbinCmdIl, &cmd, 1) != ReadResult::ok) {
      phase = Phase::restore;
      phaseDue = now + (restoreTries < 30 ? kFixRetryMs : 30000);
      ev.kind = FixEvent::skipped;
      ev.step = FixStep::suspend;
      ev.why = "USBIN +0x40 did not read: the lift again in a second";
      return ev;
    }
    if (!(cmd & kUsbinSuspendBit)) {
      suspendOpen = false;
      phase = Phase::judge;
      phaseDue = now + kFixJudgeMs;
      return ev;   // already clear
    }
    ev = writeOne(io, w, Armed::suspendClear, usbin, "USBIN", kUsbinCmdIl, cmd, uint8_t(cmd & ~kUsbinSuspendBit), "lift");
    ev.step = FixStep::suspend;
    ev.attempt = attempts;
    ev.ok = ev.ok && ev.readBack && !(ev.after & kUsbinSuspendBit);
    if (ev.ok) {
      suspendOpen = false;
      phase = Phase::judge;
      phaseDue = now + kFixJudgeMs;
    } else {
      writeFailures++;
      phase = Phase::restore;
      phaseDue = now + (restoreTries < 30 ? kFixRetryMs : 30000);
    }
    return ev;
  }

  FixEvent judge(bool stOk, uint8_t state, int packsMa, uint64_t now)
  {
    FixEvent ev;
    ev.kind = FixEvent::judged;
    ev.step = step;
    ev.attempt = attempts;
    ev.state = state;
    ev.packsMa = packsMa;
    ev.ok = stOk;
    ev.charging = packsMa >= kFixChargingMa;
    ev.why = ev.charging ? "CHARGING again"
             : stOk && chgrCharging(state) ? "the charger reads a charging state (the packs not yet at +50 mA)"
             : stOk && state != kChgrDisabled ? "the charger left DISABLED (not charging)"
             : "no change";
    phase = Phase::idle;
    const bool repeat = winner != FixStep::none && step == winner;
    if (ev.charging || (stOk && chgrCharging(state))) {
      successes++;
      if (winner == FixStep::none) winner = step;
      disabledSeen = false;
      nextAttemptMs = now + kFixRepeatGapMs;
      mark(ev.charging ? "WORKED: charging again (used again on a later stop)" : "WORKED: a charging state (used again on a later stop)", step);
    } else {
      nextAttemptMs = now + kFixStepGapMs;
      if (repeat) finished = true;   // the step that worked once did not again: none more
      mark(ev.why, step);
    }
    step = FixStep::none;
    return ev;
  }
};

}  // namespace pianopower
#endif
