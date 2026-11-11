// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano's charger PMIC (PMIH010x, SID 7) as the application processor sees it: read only, through SpmiObserver.h. The buck charger is
// driven by the audio DSP's firmware (docs/drivers/charging.md); this side can only look. Facts (Project-SunUEFI PianoUsbTypec.c,
// BSD-2-Clause-Patent, from the stock PmicDxe; UPSTREAM):
//   REVID 0x100: TYPE (+4) 0x51, SUBTYPE (+5) the PMIC model, 0x47 or 0x56 on piano;
//   the charger block is the lowest SID 7 peripheral of TYPE 0x02 with SUBTYPE 0x7f, 0xa8 or 0xba; the other blocks are offsets from
//   it, each moved by 0x300 for SUBTYPE 0xba..0xc4: DCDC +0x100, USBIN +0x300, Type-C +0x600;
//   Type-C +0x06 sink status (a source's Rp seen: bits 1..3), +0x08 source status (a sink's Rd: bits 3, 2), +0x0b bit 0 attached,
//   bit 1 the CC line (0 CC1, 1 CC2), +0x44 the port mode; DCDC/USBIN/Type-C +0x10 the real-time interrupt status.
// The Rp bits of the sink status (Linux's qcom_pmic_typec_port.c register facts, GPL, read for facts only; docs/drivers/usb-host.md
// §facts): bit 3 default USB current, bit 2 1.5 A, bit 1 3.0 A (bit 0 a shorted CC). Until 2026-11-11 this file had the order
// reversed (inferred), so the level-1 run's "Rp 0x2 = 500 mA" on the Mac's port was 3.0 A.
#ifndef PIANO_POWER_CHARGER_PMIC_H
#define PIANO_POWER_CHARGER_PMIC_H
#include <stdint.h>
#include "SpmiObserver.h"

namespace pianopower {

constexpr uint8_t kChargerSid = 7;
constexpr uint16_t kRevid = 0x100;

struct Block {
  uint16_t address = 0;  // peripheral base (SID-relative)
  int apid = -1;
  uint32_t owner = 0;    // the APID's owner EE (0 = this processor)
};

struct ChargerLayout {
  bool found = false;
  uint8_t revType = 0, model = 0, subtype = 0;
  Block charger, dcdc, usbin, typec;
  // The survey of SID 7: peripherals in the map, and how many each EE owns.
  uint32_t peripherals = 0, ownedBy[8] = {};
  const char *why = "";   // why found is false
};

inline bool chargerSubtype(uint8_t subtype) { return subtype == 0x7f || subtype == 0xa8 || subtype == 0xba; }
inline uint16_t chargerShift(uint8_t subtype) { return subtype >= 0xba && subtype <= 0xc4 ? 0x300 : 0; }

inline Block findBlock(const Io &io, const Arbiter &a, uint16_t address)
{
  Block b;
  b.address = address;
  const Apid found = spmiFindApid(io, a, uint16_t(kChargerSid << 8 | address >> 8));
  b.apid = found.apid;
  b.owner = found.owner;
  return b;
}

// Finds the blocks; reads only peripheral TYPE/SUBTYPE registers and the arbiter's tables.
inline ChargerLayout discoverCharger(const Io &io, const Arbiter &a)
{
  ChargerLayout l;
  for (uint32_t i = 0; i < a.count; ++i) {
    const uint32_t map = spmiMap(io, i);
    if (!map || (spmiPpid(map) >> 8) != kChargerSid) continue;
    l.peripherals++;
    l.ownedBy[spmiOwner(io, i)]++;
  }
  uint8_t id[2] = {};
  const Block revid = findBlock(io, a, kRevid);
  if (revid.apid < 0 || spmiRead(io, revid.apid, 4, id, 2) != ReadResult::ok) {
    l.why = "no REVID on SID 7";
    return l;
  }
  l.revType = id[0];
  l.model = id[1];
  if (id[0] != 0x51 || (id[1] != 0x47 && id[1] != 0x56)) {
    l.why = "not a PMIH010x (REVID type/model)";
    return l;
  }
  // The lowest charger-type peripheral.
  bool have = false;
  uint16_t best = 0;
  for (uint32_t i = 0; i < a.count; ++i) {
    const uint32_t map = spmiMap(io, i);
    const uint16_t ppid = spmiPpid(map);
    if (!map || (ppid >> 8) != kChargerSid) continue;
    const uint16_t address = uint16_t((ppid & 0xff) << 8);
    if (have && address >= best) continue;
    uint8_t kind[2] = {};
    if (spmiRead(io, int(i), 4, kind, 2) != ReadResult::ok || kind[0] != 0x02 || !chargerSubtype(kind[1])) continue;
    best = address;
    l.subtype = kind[1];
    have = true;
  }
  if (!have) {
    l.why = "no charger block (TYPE 0x02)";
    return l;
  }
  const uint16_t shift = chargerShift(l.subtype);
  l.charger = findBlock(io, a, best);
  l.dcdc = findBlock(io, a, uint16_t(best + shift + 0x100));
  l.usbin = findBlock(io, a, uint16_t(best + shift + 0x300));
  l.typec = findBlock(io, a, uint16_t(best + shift + 0x600));
  if (l.typec.apid < 0) {
    l.why = "no Type-C block at charger + 0x600";
    return l;
  }
  l.found = true;
  l.why = "";
  return l;
}

// One look at the port: raw registers and what they say.
struct PortState {
  bool ok = false;
  uint8_t typec[6] = {};        // Type-C +0x06..+0x0b
  uint8_t typecRt = 0, mode = 0, usbinRt = 0, dcdcRt = 0;
  uint8_t usbin[8] = {}, dcdc[8] = {}, charger[8] = {};  // +0x06..+0x0d of each block
  bool usbinOk = false, dcdcOk = false, chargerOk = false;
  bool attached = false, sourceSeen = false, sinkSeen = false;
  uint8_t cc = 0;               // 1 or 2 when attached
  uint8_t rpBits = 0;           // sink status bits 3:1
};

// The source's advertised current from the Rp bits (bit 1 3.0 A, bit 2 1.5 A, bit 3 the default: 500 mA, a USB 2.0 port's; a USB 3
// port's default is 900 mA, which Rp does not tell): 0 none, else mA.
inline unsigned rpMilliamps(uint8_t rpBits) { return rpBits & 0x2 ? 3000 : rpBits & 0x4 ? 1500 : rpBits & 0x8 ? 500 : 0; }

inline PortState readPort(const Io &io, const ChargerLayout &l)
{
  PortState s;
  if (!l.found) return s;
  s.ok = spmiRead(io, l.typec.apid, 0x06, s.typec, 6) == ReadResult::ok;
  spmiRead(io, l.typec.apid, 0x10, &s.typecRt, 1);
  spmiRead(io, l.typec.apid, 0x44, &s.mode, 1);
  if (l.usbin.apid >= 0) {
    s.usbinOk = spmiRead(io, l.usbin.apid, 0x06, s.usbin, 8) == ReadResult::ok;
    spmiRead(io, l.usbin.apid, 0x10, &s.usbinRt, 1);
  }
  if (l.dcdc.apid >= 0) {
    s.dcdcOk = spmiRead(io, l.dcdc.apid, 0x06, s.dcdc, 8) == ReadResult::ok;
    spmiRead(io, l.dcdc.apid, 0x10, &s.dcdcRt, 1);
  }
  if (l.charger.apid >= 0) s.chargerOk = spmiRead(io, l.charger.apid, 0x06, s.charger, 8) == ReadResult::ok;
  if (!s.ok) return s;
  const uint8_t snk = s.typec[0], src = s.typec[2], misc = s.typec[5];
  s.attached = (misc & 1) != 0;
  s.cc = s.attached ? ((misc & 2) ? 2 : 1) : 0;
  s.rpBits = snk & 0x0e;
  s.sourceSeen = s.attached && s.rpBits != 0;                          // a charger or a computer's port: we are the sink
  s.sinkSeen = s.attached && (src & 0x0c) != 0 && (snk & 0x0f) == 0;  // a USB device: we are the source
  return s;
}

}  // namespace pianopower
#endif
