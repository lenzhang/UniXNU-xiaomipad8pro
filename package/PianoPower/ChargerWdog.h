// SPDX-License-Identifier: BSD-2-Clause-Patent
// The charger PMIC's BARK/BITE watchdog (docs/drivers/charging-policy.md §18): find the charger's MISC and CHGR blocks, read the
// watchdog's configuration and the charger state, pet the watchdog and (piano-power-wdog=2) clear "a bite disables charging". The
// only PMIC writes PianoPower makes outside the DSP's protocol, through an allowlist of two registers of one peripheral.
//
// Register facts, read for the facts only (no code copied): Linux's drivers/power/supply/qcom_smbx.c (GPL-2.0; PMI8998, the SCHG
// family's one mainline host driver) and the PM8150B DT (vbus regulator @1100 and Type-C @1500 for the charger at @1000):
//   MISC  +0x43 BARK_BITE_WDOG_PET, bit 0 pets;
//         +0x51 WD_CFG: bit 7 a bite triggers AFP, 6 bark interrupt, 5 bite interrupt, 4:3 safety timer after the watchdog
//               interrupt, 2 the watchdog interrupt starts the safety timer, 1 the timer starts on a plug-in, 0 the timer is on;
//         +0x53 SNARL_BARK_BITE_WD_CFG: bit 7 a bite disables charging, 6:4 snarl, 3:2 bark (16 s << n), 1:0 bite (1 s << n);
//   CHGR  +0x06 BATTERY_CHARGER_STATUS_1, bits 2:0 the state: 0 trickle, 1 pre-charge, 2 fast, 3 full-on, 4 taper, 5 terminated,
//               6 inhibited, 7 DISABLED; +0x42 bit 0 the charging enable command; +0x51 CHGR_CFG2 (bit 7 CHG_EN_SRC: 0 the command
//               register decides, 1 the pin; bit 6 the pin's polarity; bit 0 inhibit). Written by ChargerEnable.h, not here.
//   The blocks' places relative to each other: CHGR, DCDC +0x100, BATIF +0x200, USBIN +0x300, DCIN +0x400, Type-C +0x500, MISC
//   +0x600 (Linux's MISC at CHGR + 0x600; PM8150B's Type-C at CHGR + 0x500).
// On piano (PMIH010x, inferred, needs-boot): the Type-C block ChargerPmic.h finds at 0x2b00 is verified (attach, CC, Rp), so CHGR =
// Type-C - 0x500 = 0x2600 (what ChargerPmic.h calls DCDC: its +0x06 read 0xc3/0x83 while the packs charged and 0x87 from the ~40 s
// stop on, i.e. state 3 full-on -> 7 disabled in this decode, in every boot of 2026-11-11/12) and MISC = Type-C + 0x100 = 0x2c00.
// Every SCHG block has the peripheral TYPE 0x02 (ChargerPmic.h's charger block is one), so MISC's TYPE is checked before a write.
//
// The gates before any write: MISC found at Type-C + 0x100, owned by EE 0 (this processor), TYPE 0x02, its configuration read; a pet
// only while the watchdog reads armed (WD_CFG bit 0 or 1), the clear only while bit 7 of +0x53 is set and not after the DSP's
// firmware has taken the charger (level 2). The clear is a read-modify-write of bit 7 alone, read back. Nothing else is written:
// the float voltage, the charge and input currents, JEITA and the safety timer stay the boot stages' (or the DSP firmware's) values;
// the enable command is ChargerEnable.h's (2026-11-12 dev14: this watchdog read off, WD_CFG 0x00 and +0x53 0x00).
#ifndef PIANO_POWER_CHARGER_WDOG_H
#define PIANO_POWER_CHARGER_WDOG_H
#include <stdint.h>
#include "SpmiObserver.h"
#include "ChargerPmic.h"

namespace pianopower {

constexpr uint8_t kSchgType = 0x02;
constexpr uint16_t kChgrBelowTypec = 0x500, kMiscAboveTypec = 0x100;

constexpr uint8_t kChgrStatus1 = 0x06, kChgrStateMask = 0x07, kChgrDisabled = 7;
constexpr uint8_t kChgrCmd = 0x40, kChgrEnableCmd = 0x42, kChgrCfg = 0x50, kChgrCfg2 = 0x51;
constexpr uint8_t kMiscPet = 0x43, kMiscPetBit = 0x01;
constexpr uint8_t kMiscWdCfg = 0x51;
constexpr uint8_t kWdAfp = 0x80, kWdBarkIrq = 0x40, kWdBiteIrq = 0x20, kWdOnPlugin = 0x02, kWdTimerOn = 0x01;
constexpr uint8_t kMiscWdTimeouts = 0x53, kWdBiteStopsCharging = 0x80;
constexpr uint8_t kBlockRt = 0x10;

inline unsigned barkSeconds(uint8_t timeouts) { return 16u << ((timeouts >> 2) & 3); }
inline unsigned biteSeconds(uint8_t timeouts) { return 1u << (timeouts & 3); }
// The pet period: a quarter of the bark time, 4..15 s (a 16 s bark: every 4 s; 32 s: 8 s; 64 s and more: 15 s).
inline uint32_t petPeriodMs(uint8_t timeouts)
{
  const uint32_t ms = barkSeconds(timeouts) * 1000u / 4u;
  return ms < 4000 ? 4000 : ms > 15000 ? 15000 : ms;
}

inline const char *chargerStateName(uint8_t state)
{
  static const char *const kNames[] = {"trickle", "pre-charge", "fast", "full-on", "taper", "terminated", "inhibited", "DISABLED"};
  return kNames[state & 7];
}

enum class WdogMode : uint8_t { off = 0, pet = 1, petAndClear = 2 };

inline const char *wdogModeName(WdogMode m)
{
  return m == WdogMode::off ? "read only" : m == WdogMode::pet ? "pet" : "pet and clear the bite's charging stop";
}

struct WdogBlock {
  uint16_t address = 0;
  int apid = -1;
  uint32_t owner = 0;
  bool idOk = false;
  uint8_t type = 0, subtype = 0;
};

// One look at the two blocks (observer reads only).
struct WdogView {
  bool cfgOk = false;
  uint8_t wdCfg = 0, timeouts = 0;
  bool miscOk = false;
  uint8_t misc[8] = {};      // MISC +0x06..+0x0d
  uint8_t miscRt = 0;        // MISC +0x10
  bool chgrOk = false;
  uint8_t chgr[8] = {};      // CHGR +0x06..+0x0d
  uint8_t chgrRt = 0;
  bool chgrCmdOk = false, chgrCfgOk = false;
  uint8_t chgrCmd[8] = {};   // CHGR +0x40..+0x47
  uint8_t chgrCfg[8] = {};   // CHGR +0x50..+0x57
  bool armed() const { return cfgOk && (wdCfg & (kWdTimerOn | kWdOnPlugin)) != 0; }
  bool biteStops() const { return cfgOk && (timeouts & kWdBiteStopsCharging) != 0; }
  uint8_t state() const { return uint8_t(chgr[0] & kChgrStateMask); }
};

// One register write of one byte over the APID's owner channel; *status gets the channel's status word. The kext's goes through
// the write channel page of the MISC APID only (PianoPower.cpp); the host test's through its model.
struct SpmiWriter {
  void *ctx = nullptr;
  bool (*write)(void *ctx, int apid, uint8_t reg, uint8_t value, uint32_t *status) = nullptr;
};

struct WdogWrite {
  bool done = false, ok = false;
  uint8_t reg = 0, value = 0, before = 0, after = 0;
  bool readBack = false;   // after is the register read back
  uint32_t status = 0;
};

class ChargerWdog {
 public:
  WdogMode mode = WdogMode::petAndClear;
  WdogBlock chgr, misc;
  bool located = false;
  const char *why = "not located";   // why no write is made (empty: writes allowed)
  bool dspOwns = false;              // level 2 once the DSP runs: its firmware owns the charger's configuration; pets go on
  bool petting = false;
  uint32_t petMs = 0;
  uint64_t petDue = 0;
  uint32_t pets = 0, petFailures = 0, petFailRun = 0, lastPetStatus = 0;
  uint32_t clears = 0, clearFailures = 0;
  static constexpr uint32_t kMaxClears = 3, kMaxPetFailRun = 3;
  WdogView last;
  uint32_t refused = 0;              // writes the allowlist refused (a bug if ever non-zero)

  // The blocks from the Type-C block ChargerPmic.h found (TYPE/SUBTYPE read once).
  void locate(const Io &io, const Arbiter &a, const ChargerLayout &l)
  {
    located = false;
    if (!l.found || l.typec.address < kChgrBelowTypec) {
      why = "no Type-C block: the charger's blocks are not located";
      return;
    }
    chgr = block(io, a, uint16_t(l.typec.address - kChgrBelowTypec));
    misc = block(io, a, uint16_t(l.typec.address + kMiscAboveTypec));
    located = true;
    why = gate();
  }

  WdogView read(const Io &io) const
  {
    WdogView v;
    if (misc.apid >= 0) {
      uint8_t c[3] = {};
      v.cfgOk = spmiRead(io, misc.apid, kMiscWdCfg, c, 3) == ReadResult::ok;
      v.wdCfg = c[0];
      v.timeouts = c[2];
      v.miscOk = spmiRead(io, misc.apid, 0x06, v.misc, 8) == ReadResult::ok;
      spmiRead(io, misc.apid, kBlockRt, &v.miscRt, 1);
    }
    if (chgr.apid >= 0) {
      v.chgrOk = spmiRead(io, chgr.apid, kChgrStatus1, v.chgr, 8) == ReadResult::ok;
      spmiRead(io, chgr.apid, kBlockRt, &v.chgrRt, 1);
      v.chgrCmdOk = spmiRead(io, chgr.apid, kChgrCmd, v.chgrCmd, 8) == ReadResult::ok;
      v.chgrCfgOk = spmiRead(io, chgr.apid, kChgrCfg, v.chgrCfg, 8) == ReadResult::ok;
    }
    return v;
  }

  // The allowlist: a pet (MISC +0x43 = 0x01) or the clear of bit 7 of MISC +0x53 from a value that has it; nothing else, MISC only.
  bool allowed(int apid, uint8_t reg, uint8_t value, uint8_t before) const
  {
    if (apid < 0 || apid != misc.apid || why[0]) return false;
    if (reg == kMiscPet) return value == kMiscPetBit;
    if (reg == kMiscWdTimeouts) return (before & kWdBiteStopsCharging) && value == uint8_t(before & ~kWdBiteStopsCharging);
    return false;
  }

  // One check (at the start and with every report): read both blocks; with the mode and the gates, the clear (only while the
  // watchdog reads armed, at most kMaxClears per boot, never once the DSP owns the charger) and the start of the pets (a first pet
  // at once). *clear and *firstPet get those writes when made.
  WdogView check(const Io &io, const SpmiWriter &w, uint64_t now, WdogWrite *clear, WdogWrite *firstPet)
  {
    last = read(io);
    if (!located) return last;
    why = gate();
    if (why[0] || mode == WdogMode::off) return last;
    if (mode == WdogMode::petAndClear && !dspOwns && last.armed() && last.biteStops() && clears + clearFailures < kMaxClears) {
      WdogWrite r;
      r.done = true;
      r.reg = kMiscWdTimeouts;
      r.before = last.timeouts;
      r.value = uint8_t(last.timeouts & ~kWdBiteStopsCharging);
      r.ok = writeOne(w, r.reg, r.value, r.before, &r.status);
      uint8_t back[3] = {};
      r.readBack = spmiRead(io, misc.apid, kMiscWdCfg, back, 3) == ReadResult::ok;
      r.after = back[2];
      if (r.readBack) {
        last.wdCfg = back[0];
        last.timeouts = back[2];
      }
      r.ok = r.ok && r.readBack && r.after == r.value;
      (r.ok ? clears : clearFailures)++;
      if (clear) *clear = r;
    }
    if (!petting && last.armed() && petFailRun < kMaxPetFailRun) {
      petting = true;
      petMs = petPeriodMs(last.timeouts);
      petDue = now;
      if (firstPet) *firstPet = pet(w, now);
      else pet(w, now);
    } else if (petting) {
      petMs = petPeriodMs(last.timeouts);   // the bark time as it reads now
    }
    return last;
  }

  // A pet when it is due (the thread calls this at petDue); false when none was due.
  bool petIfDue(const SpmiWriter &w, uint64_t now, WdogWrite *out = nullptr)
  {
    if (!petting || now < petDue) return false;
    const WdogWrite r = pet(w, now);
    if (out) *out = r;
    return true;
  }

  // 0 when no pet is pending, else the time of the next one.
  uint64_t nextDue() const { return petting ? petDue : 0; }

 private:
  static WdogBlock block(const Io &io, const Arbiter &a, uint16_t address)
  {
    WdogBlock b;
    b.address = address;
    const Apid found = spmiFindApid(io, a, uint16_t(kChargerSid << 8 | address >> 8));
    b.apid = found.apid;
    b.owner = found.owner;
    uint8_t id[2] = {};
    b.idOk = b.apid >= 0 && spmiRead(io, b.apid, 4, id, 2) == ReadResult::ok;
    b.type = id[0];
    b.subtype = id[1];
    return b;
  }

  const char *gate() const
  {
    if (!located) return "not located";
    if (misc.apid < 0) return "no MISC block at Type-C + 0x100";
    if (misc.owner != 0) return "MISC is owned by another EE";
    if (!misc.idOk || misc.type != kSchgType) return "MISC's TYPE is not the charger's (0x02): the map does not fit";
    return "";
  }

  bool writeOne(const SpmiWriter &w, uint8_t reg, uint8_t value, uint8_t before, uint32_t *status)
  {
    if (!w.write || !allowed(misc.apid, reg, value, before)) {
      refused++;
      return false;
    }
    return w.write(w.ctx, misc.apid, reg, value, status);
  }

  WdogWrite pet(const SpmiWriter &w, uint64_t now)
  {
    WdogWrite r;
    r.done = true;
    r.reg = kMiscPet;
    r.value = kMiscPetBit;
    r.ok = writeOne(w, r.reg, r.value, 0, &r.status);
    lastPetStatus = r.status;
    if (r.ok) {
      pets++;
      petFailRun = 0;
    } else {
      petFailures++;
      if (++petFailRun >= kMaxPetFailRun) petting = false;   // a channel that refuses: stop (the report says so)
    }
    petDue = now + (petMs ? petMs : 4000);
    return r;
  }
};

}  // namespace pianopower
#endif
